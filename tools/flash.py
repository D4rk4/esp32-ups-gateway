#!/usr/bin/env python3
"""Serial flashing for the single-slot ESP32 gateway; preserves dedicated appcfg unless erase is requested.

Python 3.10+; real serial operations require esptool==4.9.0. Global options go
before the action. Config backup/restore --file and restore/erase --confirm go after it.
Backups contain passwords and SNMP communities and must be kept private.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import hashlib
from importlib import metadata
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
from typing import Callable

FLASH_SIZE = 0x1000000
CONFIG_OFFSET = 0xF00000
CONFIG_SIZE = 0x10000
APP_OFFSET = 0x10000
APP_SIZE = 0x3F0000
PARTITION_OFFSET = 0x8000
PARTITION_SIZE = 0x1000
REQUIRED_IMAGES = ("bootloader.bin", "partitions.bin", "firmware.bin")
PROJECT_DIR = Path(__file__).resolve().parents[1]
EXPECTED_PARTITIONS = {
    "nvs": (1, 2, 0x9000, 0x5000, 0),
    "factory": (0, 0, APP_OFFSET, APP_SIZE, 0),
    "appcfg": (1, 2, CONFIG_OFFSET, CONFIG_SIZE, 0),
}
Runner = Callable[[list[str]], str]


class FlashError(ValueError):
    """An operation cannot safely continue."""


def validate_partition_table(data: bytes) -> list[tuple]:
    """Validate ESP-IDF binary entries, optional MD5, ranges and exact layout."""
    if not data or len(data) > PARTITION_SIZE or len(data) % 32:
        raise FlashError("Partition table must contain 32-byte entries within 4096 bytes")
    entries = []
    found = {}
    end = len(data)
    for position in range(0, len(data), 32):
        entry = data[position:position + 32]
        if entry == b"\xff" * 32:
            end = position
            break
        if entry[:2] == b"\xeb\xeb":
            if entry[2:16] != b"\xff" * 14 or hashlib.md5(data[:position]).digest() != entry[16:]:
                raise FlashError("Partition table MD5 mismatch")
            end = position + 32
            break
        magic, kind, subtype, offset, size, raw_name, flags = struct.unpack("<HBBII16sI", entry)
        if magic != 0x50AA:
            raise FlashError("Invalid partition entry magic")
        try:
            name = raw_name.split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError as error:
            raise FlashError("Invalid partition label") from error
        if not name or name in found:
            raise FlashError("Missing or duplicate partition label")
        if not size or offset < 0x9000 or offset % 0x1000 or size % 0x1000 or offset + size > FLASH_SIZE:
            raise FlashError(f"Partition {name} has invalid alignment, size or flash bounds")
        if kind == 0 and offset % 0x10000:
            raise FlashError(f"Application partition {name} must be aligned to 0x10000")
        for previous in entries:
            if offset < previous[3] + previous[4] and previous[3] < offset + size:
                raise FlashError(f"Overlapping partitions: {previous[0]} and {name}")
        entries.append((name, kind, subtype, offset, size, flags))
        found[name] = (kind, subtype, offset, size, flags)
    if data[end:] != b"\xff" * (len(data) - end):
        raise FlashError("Unexpected data after partition table terminator/checksum")
    if found != EXPECTED_PARTITIONS:
        raise FlashError("Partition layout mismatch: require only nvs, factory 0x10000/0x3f0000, appcfg NVS 0xf00000/0x10000; OTA slots are forbidden")
    return entries


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_images(directory: Path) -> None:
    for name in REQUIRED_IMAGES:
        if not (directory / name).is_file():
            raise FlashError(f"Required image missing: {directory / name}")
    for name, maximum in (("bootloader.bin", 0x7000), ("firmware.bin", APP_SIZE)):
        path = directory / name
        if not 1 <= path.stat().st_size <= maximum:
            raise FlashError(f"{name} must contain 1–{maximum} bytes")
        with path.open("rb") as source:
            if source.read(1) != b"\xe9":
                raise FlashError(f"{name} is not an ESP image (expected magic 0xe9)")
    table = directory / "partitions.bin"
    if table.stat().st_size > PARTITION_SIZE:
        raise FlashError("partitions.bin exceeds its 4096-byte sector")
    validate_partition_table(table.read_bytes())
    manifest = directory / "SHA256SUMS.txt"
    if manifest.exists():
        hashes = {}
        try:
            lines = manifest.read_text(encoding="ascii").splitlines()
        except UnicodeDecodeError as error:
            raise FlashError("SHA256SUMS.txt must be ASCII") from error
        for line in lines:
            if not line.strip():
                continue
            match = re.fullmatch(r"([0-9a-fA-F]{64})[ \t]+\*?([^\s]+)", line)
            if not match or match[2] not in REQUIRED_IMAGES or match[2] in hashes:
                raise FlashError("SHA256SUMS.txt requires exactly one entry for each of bootloader.bin, partitions.bin, firmware.bin")
            hashes[match[2]] = match[1].lower()
        if set(hashes) != set(REQUIRED_IMAGES):
            raise FlashError("SHA256SUMS.txt is missing a required image entry")
        for name, expected in hashes.items():
            if sha256_file(directory / name) != expected:
                raise FlashError(f"SHA-256 mismatch: {name}")


def require_esptool() -> None:
    try:
        version = metadata.version("esptool")
    except metadata.PackageNotFoundError as error:
        raise FlashError(f"Install the pinned tool first: {sys.executable} -m pip install esptool==4.9.0") from error
    if version != "4.9.0":
        raise FlashError(f"esptool==4.9.0 is required; installed version is {version}")


def subprocess_runner(command: list[str]) -> str:
    try:
        completed = subprocess.run(command, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "").strip()
        raise FlashError(f"esptool failed (exit {error.returncode}): {detail}") from error
    return completed.stdout + completed.stderr


class Transport:
    def __init__(self, port: str, baud: int, dry_run: bool, runner: Runner):
        self.base = [sys.executable, "-m", "esptool", "--chip", "esp32", "--port", port, "--baud", str(baud)]
        self.dry_run = dry_run
        self.runner = runner

    def execute(self, *arguments: str, after: str = "no_reset") -> str:
        command = self.base + ["--before", "default_reset", "--after", after] + list(arguments)
        print(("PLAN " if self.dry_run else "RUN  ") + shlex.join(command))
        return "" if self.dry_run else self.runner(command)

    def finish(self) -> None:
        # A harmless ID read provides an explicit final hard reset. Intermediate
        # commands keep ROM-loader mode so first boot cannot initialize blank
        # appcfg between the before/after preservation snapshots.
        self.execute("flash_id", after="hard_reset")

    def verify_capacity(self) -> None:
        output = self.execute("flash_id")
        if self.dry_run:
            print("CHECK require detected flash size exactly 16MB before any read/write")
        elif not re.search(r"^\s*Detected flash size:\s*16MB\s*$", output, re.IGNORECASE | re.MULTILINE):
            raise FlashError("Device must report detected flash size 16MB; refusing to continue")

    def read(self, address: int, size: int, path: Path) -> str:
        if self.dry_run:
            self.execute("read_flash", hex(address), hex(size), str(path))
            print(f"CHECK {path}: exactly {size} bytes")
            return ""
        # Exclusive reservation prevents overwriting a previous backup. esptool
        # then fills only this newly reserved output path.
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            os.close(descriptor)
        except FileExistsError as error:
            raise FlashError(f"Refusing to overwrite existing output: {path}") from error
        try:
            self.execute("read_flash", hex(address), hex(size), str(path))
            if path.stat().st_size != size:
                raise FlashError(f"Readback {path} has wrong size; expected {size} bytes")
            return sha256_file(path)
        except BaseException:
            path.unlink(missing_ok=True)
            raise

    def write(self, pairs: list[tuple[int, Path]]) -> None:
        arguments = ["write_flash", "--flash_size", "16MB"]
        for address, path in pairs:
            arguments.extend((hex(address), str(path)))
        self.execute(*arguments)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="Serial port (required except with --dry-run)")
    parser.add_argument("--baud", type=int, default=460800, help="Serial transport baud, default 460800")
    parser.add_argument("--image-dir", type=Path, default=PROJECT_DIR / "firmware", help="Directory with all three image binaries and optional SHA256SUMS.txt")
    parser.add_argument("--backup-dir", type=Path, default=PROJECT_DIR / "backups", help="Automatic pre-flash backups; contains passwords and SNMP communities")
    parser.add_argument("--dry-run", action="store_true", help="Validate local inputs and print the plan; no serial calls, output files or esptool installation required")
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("install", help="Write bootloader, partition table and application; preserve appcfg")
    actions.add_parser("update", help="Validate device layout and write application at 0x10000 only; preserve appcfg")
    backup = actions.add_parser("backup-config", help="Read only appcfg into a new 65536-byte private backup file")
    backup.add_argument("--file", type=Path, required=True, help="New backup filename; existing files are never overwritten; contains secrets")
    restore = actions.add_parser("restore-config", help="DESTRUCTIVE: replace only appcfg with an exact 65536-byte backup")
    restore.add_argument("--file", type=Path, required=True, help="65536-byte raw appcfg backup containing credentials")
    restore.add_argument("--confirm", default="", help="Required exact confirmation: RESTORE_CONFIG")
    erase = actions.add_parser("erase", help="DESTRUCTIVE: erase the entire flash including firmware, nvs and appcfg; run install afterwards")
    erase.add_argument("--confirm", default="", help="Required exact confirmation: ERASE_FLASH")
    return parser


def main(argv: list[str] | None = None, runner: Runner | None = None) -> int:
    parser = build_parser()
    arguments = parser.parse_args(argv)
    if not arguments.dry_run and not arguments.port:
        parser.error("--port is required for every real device operation")
    if arguments.baud <= 0:
        parser.error("--baud must be positive")
    action = arguments.action
    if action in ("install", "update"):
        validate_images(arguments.image_dir)
        print("CHECK local image headers, bounds, partition layout and optional SHA256SUMS.txt passed")
    elif action == "backup-config":
        if arguments.file.exists():
            raise FlashError(f"Refusing to overwrite existing backup: {arguments.file}")
    elif action == "restore-config":
        if arguments.confirm != "RESTORE_CONFIG":
            raise FlashError("restore-config replaces appcfg and requires --confirm RESTORE_CONFIG")
        if not arguments.file.is_file() or arguments.file.stat().st_size != CONFIG_SIZE:
            raise FlashError("restore-config requires an existing backup of exactly 65536 bytes")
    elif action == "erase":
        if arguments.confirm != "ERASE_FLASH":
            raise FlashError("erase wipes the entire flash including appcfg and requires --confirm ERASE_FLASH")
    if not arguments.dry_run and runner is None:
        require_esptool()
    transport = Transport(arguments.port or "<PORT>", arguments.baud, arguments.dry_run, runner or subprocess_runner)
    transport.verify_capacity()
    with ExitStack() as stack:
        temporary = Path("<temporary>") if arguments.dry_run else Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="ups-flash-")))
        if action == "backup-config":
            digest = transport.read(CONFIG_OFFSET, CONFIG_SIZE, arguments.file)
            print(f"{'Planned' if arguments.dry_run else 'Saved'} private config backup: {arguments.file} (contains secrets)" + (f"; SHA-256 {digest}" if digest else ""))
            transport.finish()
            return 0
        if action == "restore-config":
            print("RESTORE replaces appcfg only; the supplied file contains credentials")
            transport.write([(CONFIG_OFFSET, arguments.file)])
            observed = transport.read(CONFIG_OFFSET, CONFIG_SIZE, temporary / "restored-appcfg.bin")
            if not arguments.dry_run and observed != sha256_file(arguments.file):
                raise FlashError("appcfg restore readback SHA-256 mismatch")
            print("CHECK planned restore readback comparison" if arguments.dry_run else "Verified restored appcfg SHA-256")
            transport.finish()
            return 0
        if action == "erase":
            print("ERASE wipes the entire flash: firmware, nvs and appcfg; no backup is taken")
            transport.execute("erase_flash")
            erased = temporary / "erased-appcfg.bin"
            transport.read(CONFIG_OFFSET, CONFIG_SIZE, erased)
            if not arguments.dry_run and erased.read_bytes() != b"\xff" * CONFIG_SIZE:
                raise FlashError("appcfg is not blank after erase")
            print("CHECK planned blank appcfg readback" if arguments.dry_run else "Verified appcfg is blank; run install to flash firmware again")
            transport.finish()
            return 0
        if action == "update":
            table_path = temporary / "device-partitions.bin"
            transport.read(PARTITION_OFFSET, PARTITION_SIZE, table_path)
            if arguments.dry_run:
                print("CHECK device partition table must match the fixed single-factory layout before write")
            else:
                validate_partition_table(table_path.read_bytes())
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        backup_path = arguments.backup_dir / f"appcfg-{stamp}.bin"
        before = transport.read(CONFIG_OFFSET, CONFIG_SIZE, backup_path)
        print(f"{'Planned private' if arguments.dry_run else 'Private'} pre-flash backup: {backup_path} (contains passwords and SNMP communities)")
        pairs = [(APP_OFFSET, arguments.image_dir / "firmware.bin")]
        if action == "install":
            pairs = [(0x1000, arguments.image_dir / "bootloader.bin"), (PARTITION_OFFSET, arguments.image_dir / "partitions.bin")] + pairs
        transport.write(pairs)
        after = transport.read(CONFIG_OFFSET, CONFIG_SIZE, temporary / "appcfg-after.bin")
        if not arguments.dry_run and before != after:
            raise FlashError(f"appcfg CHANGED after {action}; original backup retained at {backup_path}; no automatic restore was attempted")
        print("CHECK planned before/after appcfg SHA-256 comparison" if arguments.dry_run else f"Verified appcfg unchanged; SHA-256 {after}")
        transport.finish()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FlashError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
