"""Flashing safety tests use recorded commands; never access a serial device."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

MODULE = Path(os.path.abspath(__file__)).parents[1] / "tools" / "flash.py"
spec = importlib.util.spec_from_file_location("gateway_flash", MODULE)
flash = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = flash
spec.loader.exec_module(flash)


OTA_ENTRIES = [
    ("nvs", 1, 2, 0x9000, 0x5000),
    ("otadata", 1, 0, 0xE000, 0x2000),
    ("ota_0", 0, 16, 0x10000, 0x3F0000),
    ("ota_1", 0, 17, 0x400000, 0x3F0000),
    ("logs", 1, 0x40, 0x7F0000, 0x100000),
    ("appcfg", 1, 2, 0xF00000, 0x10000),
]
LEGACY_ENTRIES = [
    ("nvs", 1, 2, 0x9000, 0x5000),
    ("factory", 0, 0, 0x10000, 0x3F0000),
    ("appcfg", 1, 2, 0xF00000, 0x10000),
]


def partition_table(entries=None):
    if entries is None:
        entries = OTA_ENTRIES
    data = b"".join(struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset,
                                size, name.encode(), 0)
                    for name, kind, subtype, offset, size in entries)
    data += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(data).digest()
    return data.ljust(0xC00, b"\xff")


class Recorder:
    def __init__(self, config=None, after=None, table=None, flash_size="16MB", erased=None):
        self.commands = []
        self.config = config or b"C" * 0x10000
        self.after = after if after is not None else self.config
        self.table = table or partition_table().ljust(4096, b"\xff")
        self.flash_size = flash_size
        self.erased = erased if erased is not None else b"\xff" * 0x10000
        self.wrote = False

    def __call__(self, command):
        self.commands.append(command)
        if "flash_id" in command:
            return f"Detected flash size: {self.flash_size}\n"
        if "read_flash" in command:
            index = command.index("read_flash")
            address, size, path = command[index + 1:]
            data = self.table if int(address, 0) == 0x8000 else (self.after if self.wrote else self.config)
            Path(path).write_bytes(data[:int(size, 0)])
        if "write_flash" in command:
            self.wrote = True
        if "erase_flash" in command:
            self.config = self.erased
        return ""


class MemoryFlash:
    """Small serial-device simulation with physical erase-sector side effects."""
    def __init__(self, table):
        self.contents = bytearray(b"\xff" * 0x1000000)
        self.contents[0x8000:0x8c00] = table
        self.contents[0xe000:0x10000] = b"S" * 0x2000  # OTA selector points at slot 1.
        self.contents[0x7f0000:0x8f0000] = b"L" * 0x100000
        self.contents[0xf00000:0xf10000] = b"C" * 0x10000
        self.commands = []
        self.fail_before_erase = False
        self.fail_firmware_write = False

    def __call__(self, command):
        self.commands.append(command)
        if "flash_id" in command:
            return "Detected flash size: 16MB\n"
        if "read_flash" in command:
            address, size, path = command[-3:]
            address, size = int(address, 0), int(size, 0)
            Path(path).write_bytes(self.contents[address:address + size])
        if "write_flash" in command:
            start = command.index("16MB") + 1
            for index in range(start, len(command), 2):
                address, image = int(command[index], 0), Path(command[index + 1]).read_bytes()
                if address == 0x10000 and self.fail_firmware_write:
                    raise flash.FlashError("simulated firmware write failure")
                size = (len(image) + 0xfff) & ~0xfff
                self.contents[address:address + size] = b"\xff" * size
                self.contents[address:address + len(image)] = image
        if "erase_region" in command:
            if self.fail_before_erase:
                raise flash.FlashError("simulated interruption before erase")
            address, size = int(command[-2], 0), int(command[-1], 0)
            self.contents[address:address + size] = b"\xff" * size
        return ""


class FlashTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.images = self.root / "images"
        self.images.mkdir()
        (self.images / "bootloader.bin").write_bytes(b"\xe9" + b"B" * 99)
        (self.images / "firmware.bin").write_bytes(b"\xe9" + b"F" * 99)
        (self.images / "partitions.bin").write_bytes(partition_table())
        self.backups = self.root / "backups"

    def invoke(self, action, recorder=None, *extra, dry=False):
        args = ["--image-dir", str(self.images), "--backup-dir", str(self.backups)]
        if dry:
            args += ["--dry-run"]
        else:
            args += ["--port", "TEST_PORT"]
        with redirect_stdout(io.StringIO()):
            return flash.main(args + [action] + list(extra), runner=recorder)

    def test_csv_builds_exact_ota_partition_layout(self):
        import csv
        path = MODULE.parents[1] / "partitions.csv"
        with path.open(encoding="utf-8", newline="") as source:
            entries = [row for row in csv.reader(line for line in source if not line.startswith("#")) if row]
        self.assertEqual([tuple(field.strip() for field in row[:5]) for row in entries], [
            ("nvs", "data", "nvs", "0x9000", "0x5000"),
            ("otadata", "data", "ota", "0xE000", "0x2000"),
            ("ota_0", "app", "ota_0", "0x10000", "0x3F0000"),
            ("ota_1", "app", "ota_1", "0x400000", "0x3F0000"),
            ("logs", "data", "0x40", "0x7F0000", "0x100000"),
            ("appcfg", "data", "nvs", "0xF00000", "0x10000"),
        ])

    def test_build_prepares_dual_slot_metadata_and_validated_images(self):
        source = self.root / ".pio" / "build" / "esp32-poe-16mb"
        source.mkdir(parents=True)
        for name in ("bootloader.bin", "partitions.bin", "firmware.bin"):
            (source / name).write_bytes((self.images / name).read_bytes())
        sys.modules.setdefault("flash", flash)
        build_spec = importlib.util.spec_from_file_location("gateway_build", MODULE.parent / "build.py")
        build = importlib.util.module_from_spec(build_spec)
        build_spec.loader.exec_module(build)
        with patch.object(build, "ROOT", self.root), patch.object(build.subprocess, "run"):
            with redirect_stdout(io.StringIO()):
                build.main()
        info = json.loads((self.root / "firmware" / "build-info.json").read_text())
        self.assertEqual(info.get("application_slots"), 2)
        self.assertNotIn("single_application_slot", info)
        self.assertEqual(info["appcfg"], {"offset": "0xF00000", "size": "0x10000"})
        self.assertEqual((self.root / "firmware" / "partitions.bin").read_bytes(), partition_table())

    def test_ota_partition_layout_accepts_exact_binary_table(self):
        table = partition_table()
        try:
            entries = flash.validate_partition_table(table)
        except flash.FlashError as error:
            self.fail(f"Approved OTA layout was rejected: {error}")
        self.assertEqual(len(entries), 6)

    def test_partition_table_checksum_detects_corruption(self):
        damaged = bytearray(partition_table()); damaged[20] ^= 1
        with self.assertRaises(flash.FlashError):
            flash.validate_partition_table(bytes(damaged))

    def test_esptool_version_must_be_pinned(self):
        with patch.object(flash.metadata, "version", return_value="4.9.0"):
            flash.require_esptool()
        with patch.object(flash.metadata, "version", return_value="5.0"):
            with self.assertRaisesRegex(flash.FlashError, "4.9.0"):
                flash.require_esptool()
        with patch.object(flash.metadata, "version", side_effect=flash.metadata.PackageNotFoundError):
            with self.assertRaisesRegex(flash.FlashError, "4.9.0"):
                flash.require_esptool()

    def test_esptool_runner_returns_output_and_reports_failures(self):
        result = subprocess.CompletedProcess(["esptool"], 0, "out", "err")
        with patch.object(flash.subprocess, "run", return_value=result) as run:
            self.assertEqual(flash.subprocess_runner(["esptool"]), "outerr")
            self.assertTrue(run.call_args.kwargs["check"])
            self.assertTrue(run.call_args.kwargs["capture_output"])
            self.assertTrue(run.call_args.kwargs["text"])
        for stderr, stdout, detail in (("error", "message", "error"), ("", "message", "message")):
            failure = subprocess.CalledProcessError(7, ["esptool"], output=stdout, stderr=stderr)
            with patch.object(flash.subprocess, "run", side_effect=failure):
                with self.assertRaisesRegex(flash.FlashError, f"exit 7.*{detail}"):
                    flash.subprocess_runner(["esptool"])

    def test_esptool_missing_or_unpinned_stops_before_device_access(self):
        argv = ["--image-dir", str(self.images), "--port", "TEST_PORT", "update"]
        with patch.object(flash.metadata, "version", return_value="5.0"):
            with redirect_stdout(io.StringIO()), self.assertRaisesRegex(flash.FlashError, "4.9.0"):
                flash.main(argv)

    def test_partition_table_rejects_invalid_binary_shapes_and_names(self):
        invalid = [b"", b"\xff" * 31, b"\xff" * 4128]
        wrong_magic = bytearray(partition_table()); wrong_magic[0] = 0
        bad_name = bytearray(partition_table()); bad_name[12] = 0xff
        bad_padding = bytearray(partition_table()); bad_padding[-1] = 0
        bad_md5_padding = bytearray(partition_table()); bad_md5_padding[32 * 6 + 2] = 0
        duplicate = partition_table([OTA_ENTRIES[0], OTA_ENTRIES[0]] + OTA_ENTRIES[1:])
        empty_name = partition_table([("", 1, 2, 0x9000, 0x1000)])
        invalid += [bytes(wrong_magic), bytes(bad_name), bytes(bad_padding), bytes(bad_md5_padding), duplicate, empty_name]
        for table in invalid:
            with self.subTest(length=len(table), prefix=table[:4]), self.assertRaises(flash.FlashError):
                flash.validate_partition_table(table)

    def test_invalid_partition_ranges_are_rejected_before_layout_matching(self):
        entries = [
            ("zero", 1, 2, 0x9000, 0),
            ("low", 1, 2, 0x8000, 0x1000),
            ("unaligned", 1, 2, 0x9001, 0x1000),
            ("oddsize", 1, 2, 0x9000, 0x1001),
            ("outside", 1, 2, 0x1000000, 0x1000),
            ("app", 0, 16, 0x11000, 0x1000),
        ]
        for entry in entries:
            with self.subTest(entry=entry), self.assertRaisesRegex(flash.FlashError, "alignment|aligned"):
                flash.validate_partition_table(partition_table([entry]))

    def test_overlap_error_identifies_the_previous_partition(self):
        first = ("first", 1, 2, 0x10000, 0x2000)
        second = ("second", 1, 2, 0x11000, 0x2000)
        with self.assertRaisesRegex(flash.FlashError, "first and second"):
            flash.validate_partition_table(partition_table([first, second]))

    def test_partition_ranges_allow_flash_boundary_and_touching_edges(self):
        try:
            flash.validate_partition_range("last", 1, 0xFFF000, 0x1000, [])
            flash.validate_partition_overlap("next", 0xA000, 0x1000, [("previous", 1, 2, 0x9000, 0x1000, 0)])
            flash.validate_partition_overlap("previous", 0x9000, 0x1000, [("next", 1, 2, 0xA000, 0x1000, 0)])
        except flash.FlashError as error:
            self.fail(f"Adjacent, aligned partitions should be valid: {error}")

    def test_partition_overlap_detected_in_either_entry_order(self):
        low = ("low", 1, 2, 0x10000, 0x2000)
        high = ("high", 1, 2, 0x11000, 0x2000)
        for entries in ([low, high], [high, low]):
            with self.subTest(entries=entries), self.assertRaisesRegex(flash.FlashError, "Overlapping partitions"):
                flash.validate_partition_table(partition_table(entries))

    def test_missing_partition_binary_stops_before_device_access(self):
        (self.images / "partitions.bin").unlink()
        recorder = Recorder()
        with self.assertRaisesRegex(flash.FlashError, "partitions.bin"):
            self.invoke("install", recorder)
        self.assertEqual(recorder.commands, [])

    def test_near_miss_ota_layouts_are_rejected_for_local_images_and_device(self):
        variants = [
            OTA_ENTRIES[:-2] + [("logs", 1, 0x41, 0x7F0000, 0x100000)] + OTA_ENTRIES[-1:],
            OTA_ENTRIES[:-2] + [("logs", 1, 0x40, 0x7F0000, 0x0FF000)] + OTA_ENTRIES[-1:],
            OTA_ENTRIES[:-1] + [("appcfg", 1, 2, 0xE00000, 0x10000)],
            OTA_ENTRIES[:3] + [("ota_1", 0, 17, 0x400000, 0x3E0000)] + OTA_ENTRIES[4:],
            OTA_ENTRIES + [("other", 1, 0x40, 0x900000, 0x1000)],
        ]
        for entries in variants:
            with self.subTest(entries=entries):
                table = partition_table(entries)
                (self.images / "partitions.bin").write_bytes(table)
                recorder = Recorder()
                with self.assertRaises(flash.FlashError):
                    self.invoke("install", recorder)
                self.assertEqual(recorder.commands, [])
                (self.images / "partitions.bin").write_bytes(partition_table())
                for action in ("update", "migrate"):
                    recorder = Recorder(table=table.ljust(4096, b"\xff"))
                    with self.assertRaises(flash.FlashError):
                        self.invoke(action, recorder)
                    self.assertFalse(any("write_flash" in c or "erase_region" in c for c in recorder.commands))

    def test_overlap_second_app_wrong_appcfg_and_wrong_factory_are_rejected(self):
        cases = [
            [("nvs", 1, 2, 0x9000, 0x5000), ("factory", 0, 0, 0x10000, 0xF10000), ("appcfg", 1, 2, 0xF00000, 0x10000)],
            [("nvs", 1, 2, 0x9000, 0x5000), ("factory", 0, 0, 0x10000, 0x3F0000), ("appcfg", 1, 2, 0xF00000, 0x10000), ("ota_0", 0, 16, 0x400000, 0x100000)],
            [("nvs", 1, 2, 0x9000, 0x5000), ("factory", 0, 0, 0x10000, 0x3F0000), ("appcfg", 1, 2, 0xE00000, 0x10000)],
            [("nvs", 1, 2, 0x9000, 0x5000), ("factory", 0, 0, 0x20000, 0x3F0000), ("appcfg", 1, 2, 0xF00000, 0x10000)],
        ]
        for entries in cases:
            with self.subTest(entries=entries), self.assertRaises(flash.FlashError):
                flash.validate_partition_table(partition_table(entries))

    def test_wrong_layout_with_label_garbage_is_rejected(self):
        raw = bytearray(partition_table())
        raw[12 + 5] = ord("X")  # Inject hidden bytes after the nvs label's NUL.
        checksum = len(OTA_ENTRIES) * 32
        raw[checksum + 16:checksum + 32] = hashlib.md5(raw[:checksum]).digest()
        with self.assertRaises(flash.FlashError):
            flash.validate_partition_table(bytes(raw))

    def test_oversize_app_and_invalid_magic_reject_before_any_device_call(self):
        for content in (b"\xe9" + b"F" * 0x3F0000, b"bad header"):
            (self.images / "firmware.bin").write_bytes(content)
            recorder = Recorder()
            with self.assertRaises(flash.FlashError):
                self.invoke("update", recorder)
            self.assertEqual(recorder.commands, [])

    def test_valid_app_size_boundaries_and_full_partition_sector_are_accepted(self):
        for name, maximum in (("bootloader.bin", 0x7000), ("firmware.bin", 0x3F0000)):
            original = (self.images / name).read_bytes()
            try:
                for length in (1, maximum):
                    (self.images / name).write_bytes(b"\xe9" + b"F" * (length - 1))
                    with self.subTest(name=name, length=length):
                        flash.validate_images(self.images)
            finally:
                (self.images / name).write_bytes(original)
        (self.images / "partitions.bin").write_bytes(partition_table().ljust(0x1000, b"\xff"))
        flash.validate_images(self.images)

    def test_update_writes_application_then_resets_ota_selection_and_retains_config(self):
        recorder = Recorder()
        self.assertEqual(self.invoke("update", recorder), 0)
        writes = [c for c in recorder.commands if "write_flash" in c]
        self.assertEqual(len(writes), 1)
        self.assertEqual(writes[0][-2:], ["0x10000", str(self.images / "firmware.bin")])
        erase = next(i for i, c in enumerate(recorder.commands) if "erase_region" in c)
        self.assertEqual(recorder.commands[erase][-2:], ["0xe000", "0x2000"])
        self.assertGreater(erase, recorder.commands.index(writes[0]))
        self.assertEqual(list(self.backups.glob("*.bin"))[0].read_bytes(), recorder.config)
        self.assertTrue(any("read_flash" in c and "0x8000" in c for c in recorder.commands))
        self.assertEqual(sum("read_flash" in c and "0xf00000" in c for c in recorder.commands), 2)
        self.assertEqual(recorder.commands[-1][recorder.commands[-1].index("--after") + 1], "hard_reset")
        for command in recorder.commands[:-1]:
            self.assertEqual(command[command.index("--after") + 1], "no_reset")
        for command in recorder.commands:
            self.assertEqual(command[:3], [sys.executable, "-m", "esptool"])
            self.assertNotIn("erase_flash", command)

    def test_install_writes_only_the_three_images_then_resets_selection(self):
        recorder = Recorder()
        self.invoke("install", recorder)
        pairs = [c[i:i + 2] for c in recorder.commands if "write_flash" in c
                 for i in range(c.index("16MB") + 1, len(c), 2)]
        self.assertEqual(pairs, [["0x1000", str(self.images / "bootloader.bin")],
                                 ["0x8000", str(self.images / "partitions.bin")],
                                 ["0x10000", str(self.images / "firmware.bin")]])
        erase = next(i for i, c in enumerate(recorder.commands) if "erase_region" in c)
        firmware = next(i for i, c in enumerate(recorder.commands) if str(self.images / "firmware.bin") in c)
        self.assertEqual(recorder.commands[erase][-2:], ["0xe000", "0x2000"])
        self.assertGreater(erase, firmware)

    def test_migrate_writes_firmware_then_erases_otadata_then_writes_table(self):
        recorder = Recorder(table=partition_table(LEGACY_ENTRIES).ljust(4096, b"\xff"))
        self.assertEqual(self.invoke("migrate", recorder), 0)
        writes = [(i, c) for i, c in enumerate(recorder.commands) if "write_flash" in c]
        self.assertEqual(len(writes), 2)
        self.assertEqual(writes[0][1][-4:], ["0x1000", str(self.images / "bootloader.bin"),
                                              "0x10000", str(self.images / "firmware.bin")])
        self.assertEqual(writes[1][1][-2:], ["0x8000", str(self.images / "partitions.bin")])
        erase = next(i for i, c in enumerate(recorder.commands) if "erase_region" in c)
        self.assertEqual(recorder.commands[erase][-2:], ["0xe000", "0x2000"])
        self.assertLess(writes[0][0], erase)
        self.assertLess(erase, writes[1][0])
        self.assertEqual(list(self.backups.glob("*.bin"))[0].read_bytes(), recorder.config)
        self.assertEqual(sum("read_flash" in c and "0xf00000" in c for c in recorder.commands), 2)
        self.assertIn("hard_reset", recorder.commands[-1])

    def test_update_rejects_legacy_and_migrate_rejects_ota_with_guidance(self):
        for action, table, hint in (("update", partition_table(LEGACY_ENTRIES), "migrate"),
                                    ("migrate", partition_table(), "update")):
            recorder = Recorder(table=table.ljust(4096, b"\xff"))
            with self.subTest(action=action), self.assertRaisesRegex(flash.FlashError, hint):
                self.invoke(action, recorder)
            self.assertFalse(any("write_flash" in c or "erase_region" in c for c in recorder.commands))
            self.assertFalse(any("hard_reset" in c for c in recorder.commands))

    def test_normal_actions_never_write_or_erase_reserved_logs_or_appcfg(self):
        for action in ("install", "update", "migrate"):
            table = partition_table(LEGACY_ENTRIES if action == "migrate" else OTA_ENTRIES)
            recorder = Recorder(table=table.ljust(4096, b"\xff"))
            self.invoke(action, recorder)
            for command in recorder.commands:
                self.assertNotIn("erase_flash", command)
                if "write_flash" in command:
                    start = command.index("16MB") + 1
                    ranges = [(int(command[i], 0), Path(command[i + 1]).stat().st_size)
                              for i in range(start, len(command), 2)]
                elif "erase_region" in command:
                    ranges = [(int(command[-2], 0), int(command[-1], 0))]
                else:
                    continue
                for address, size in ranges:
                    self.assertFalse(address < 0x8F0000 and address + size > 0x7F0000, command)
                    self.assertFalse(address < 0xF10000 and address + size > 0xF00000, command)

    def test_migration_failure_before_table_write_never_changes_table(self):
        for failed_operation in ("write_flash", "erase_region"):
            recorder = Recorder(table=partition_table(LEGACY_ENTRIES).ljust(4096, b"\xff"))
            def fail_before_table(command):
                if failed_operation in command:
                    recorder.commands.append(command)
                    raise flash.FlashError("simulated interruption")
                return recorder(command)
            with self.subTest(failed_operation=failed_operation), self.assertRaisesRegex(flash.FlashError, "interruption"):
                self.invoke("migrate", fail_before_table)
            self.assertFalse(any("write_flash" in c and str(self.images / "partitions.bin") in c
                                 for c in recorder.commands))
            self.assertFalse(any("hard_reset" in c for c in recorder.commands))

    def test_interrupted_update_retries_and_selects_newly_written_slot_zero(self):
        device = MemoryFlash(partition_table())
        device.fail_before_erase = True
        with self.assertRaisesRegex(flash.FlashError, "interruption"):
            self.invoke("update", device)
        self.assertEqual(device.contents[0xe000:0x10000], b"S" * 0x2000)
        self.assertFalse(any("hard_reset" in c for c in device.commands))
        device.fail_before_erase = False
        self.assertEqual(self.invoke("update", device), 0)
        self.assertEqual(device.contents[0xe000:0x10000], b"\xff" * 0x2000)
        self.assertEqual(device.contents[0x10000:0x10064], (self.images / "firmware.bin").read_bytes())
        self.assertEqual(device.contents[0x7f0000:0x8f0000], b"L" * 0x100000)
        self.assertEqual(device.contents[0xf00000:0xf10000], b"C" * 0x10000)

    def test_interrupted_migration_retries_while_legacy_table_is_intact(self):
        device = MemoryFlash(partition_table(LEGACY_ENTRIES))
        device.fail_before_erase = True
        with self.assertRaisesRegex(flash.FlashError, "interruption"):
            self.invoke("migrate", device)
        self.assertEqual(device.contents[0x8000:0x8c00], partition_table(LEGACY_ENTRIES))
        self.assertEqual(device.contents[0xe000:0x10000], b"S" * 0x2000)
        device.fail_before_erase = False
        self.assertEqual(self.invoke("migrate", device), 0)
        self.assertEqual(device.contents[0x8000:0x8c00], partition_table())
        self.assertEqual(device.contents[0xe000:0x10000], b"\xff" * 0x2000)
        self.assertEqual(device.contents[0x10000:0x10064], (self.images / "firmware.bin").read_bytes())
        self.assertEqual(device.contents[0x7f0000:0x8f0000], b"L" * 0x100000)
        self.assertEqual(device.contents[0xf00000:0xf10000], b"C" * 0x10000)

    def test_failed_firmware_write_keeps_existing_ota_selection(self):
        device = MemoryFlash(partition_table())
        device.fail_firmware_write = True
        with self.assertRaisesRegex(flash.FlashError, "firmware write failure"):
            self.invoke("update", device)
        self.assertEqual(device.contents[0xe000:0x10000], b"S" * 0x2000)
        self.assertEqual(device.contents[0x7f0000:0x8f0000], b"L" * 0x100000)
        self.assertEqual(device.contents[0xf00000:0xf10000], b"C" * 0x10000)
        self.assertFalse(any("erase_region" in c or "hard_reset" in c for c in device.commands))

    def test_detected_wrong_capacity_or_device_table_prevents_write(self):
        for recorder in (Recorder(flash_size="4MB"), Recorder(table=b"\xff" * 4096)):
            with self.subTest(recorder=recorder), self.assertRaises(flash.FlashError):
                self.invoke("update", recorder)
            self.assertFalse(any("write_flash" in c for c in recorder.commands))

    def test_retention_mismatch_reports_failure_and_keeps_original_backup(self):
        recorder = Recorder(after=b"X" * 0x10000)
        with self.assertRaisesRegex(flash.FlashError, "appcfg"):
            self.invoke("update", recorder)
        self.assertEqual(list(self.backups.glob("*.bin"))[0].read_bytes(), recorder.config)
        self.assertFalse(any("hard_reset" in c for c in recorder.commands))

    def test_backup_is_exact_and_existing_file_is_never_overwritten(self):
        path = self.root / "private.bin"
        self.invoke("backup-config", Recorder(), "--file", str(path))
        self.assertEqual(path.read_bytes(), b"C" * 0x10000)
        self.assertEqual(path.stat().st_mode & 0o077, 0, "Credential backup must be private")
        recorder = Recorder()
        with self.assertRaises(flash.FlashError):
            self.invoke("backup-config", recorder, "--file", str(path))
        self.assertEqual(recorder.commands, [])

    def test_repeated_backups_succeed_in_existing_directory(self):
        self.backups.mkdir()
        first = self.backups / "first.bin"
        second = self.backups / "second.bin"
        self.invoke("backup-config", Recorder(), "--file", str(first))
        self.invoke("backup-config", Recorder(), "--file", str(second))
        self.assertEqual(first.read_bytes(), b"C" * 0x10000)
        self.assertEqual(second.read_bytes(), b"C" * 0x10000)

    def test_restore_requires_confirmation_exact_size_and_writes_only_appcfg(self):
        path = self.root / "saved.bin"; path.write_bytes(b"R" * 0x10000)
        for extra in ([], ["--confirm", "WRONG"]):
            recorder = Recorder()
            with self.assertRaises(flash.FlashError):
                self.invoke("restore-config", recorder, "--file", str(path), *extra)
            self.assertEqual(recorder.commands, [])
        recorder = Recorder(after=path.read_bytes())
        self.invoke("restore-config", recorder, "--file", str(path), "--confirm", "RESTORE_CONFIG")
        write = next(c for c in recorder.commands if "write_flash" in c)
        self.assertEqual(write[-2:], ["0xf00000", str(path)])
        path.write_bytes(b"short")
        with self.assertRaises(flash.FlashError):
            self.invoke("restore-config", Recorder(), "--file", str(path), "--confirm", "RESTORE_CONFIG")

    def test_restore_readback_mismatch_aborts_without_reset(self):
        path = self.root / "saved.bin"
        path.write_bytes(b"R" * 0x10000)
        recorder = Recorder(after=b"X" * 0x10000)
        with self.assertRaisesRegex(flash.FlashError, "readback"):
            self.invoke("restore-config", recorder, "--file", str(path), "--confirm", "RESTORE_CONFIG")
        self.assertFalse(any("hard_reset" in c for c in recorder.commands))

    def test_restore_without_file_argument_is_rejected_by_main(self):
        with patch("sys.stderr", io.StringIO()), self.assertRaises(SystemExit) as error:
            flash.main(["--port", "TEST_PORT", "restore-config"], runner=Recorder())
        self.assertEqual(error.exception.code, 2)

    def test_missing_restore_file_and_required_file_arguments_are_rejected(self):
        with self.assertRaises(flash.FlashError):
            self.invoke("restore-config", Recorder(), "--file", str(self.root / "missing"), "--confirm", "RESTORE_CONFIG")
        for action in ("backup-config", "restore-config"):
            with self.subTest(action=action), patch("sys.stderr", io.StringIO()), self.assertRaises(SystemExit):
                flash.build_parser().parse_args([action])

    def test_erase_requires_exact_confirmation_before_device_access(self):
        for extra in ([], ["--confirm", "WRONG"]):
            recorder = Recorder()
            with self.subTest(extra=extra), self.assertRaises(flash.FlashError):
                self.invoke("erase", recorder, *extra)
            self.assertEqual(recorder.commands, [])

    def test_full_erase_warning_names_all_destructive_regions(self):
        help_text = " ".join(flash.build_parser().format_help().split())
        for region in ("both app slots", "otadata", "logs", "nvs", "appcfg"):
            self.assertIn(region, help_text)
        output = io.StringIO()
        with redirect_stdout(output):
            flash.main(["--port", "TEST_PORT", "erase", "--confirm", "ERASE_FLASH"], runner=Recorder())
        for region in ("both app slots", "otadata", "logs", "nvs", "appcfg"):
            self.assertIn(region, output.getvalue())

    def test_erase_reads_blank_appcfg_before_reset_without_backup_or_write(self):
        recorder = Recorder()
        self.assertEqual(self.invoke("erase", recorder, "--confirm", "ERASE_FLASH"), 0)
        erases = [i for i, c in enumerate(recorder.commands) if "erase_flash" in c]
        self.assertEqual(len(erases), 1)
        self.assertFalse(any("write_flash" in c for c in recorder.commands))
        reads = [i for i, c in enumerate(recorder.commands) if "read_flash" in c and "0xf00000" in c]
        self.assertEqual(len(reads), 1)
        self.assertGreater(reads[0], erases[0])
        self.assertLess(reads[0], len(recorder.commands) - 1)
        self.assertEqual(recorder.commands[reads[0]][-3:-1], ["0xf00000", "0x10000"])
        self.assertIn("flash_id", recorder.commands[-1])
        self.assertEqual(recorder.commands[-1][recorder.commands[-1].index("--after") + 1], "hard_reset")
        for command in recorder.commands[:-1]:
            self.assertEqual(command[command.index("--after") + 1], "no_reset")
        self.assertFalse(self.backups.exists())

    def test_erase_nonblank_appcfg_aborts_before_reset(self):
        recorder = Recorder(erased=b"X" * 0x10000)
        with self.assertRaisesRegex(flash.FlashError, "appcfg"):
            self.invoke("erase", recorder, "--confirm", "ERASE_FLASH")
        self.assertFalse(any("hard_reset" in c for c in recorder.commands))

    def test_erase_dry_run_never_calls_runner(self):
        def prohibited(command):
            self.fail(f"Dry-run executed a device command: {command}")
        self.assertEqual(self.invoke("erase", prohibited, "--confirm", "ERASE_FLASH", dry=True), 0)
        self.assertFalse(self.backups.exists())

    def test_dry_run_reports_action_specific_layout_checks(self):
        for action, expected in (("update", "exact OTA layout"), ("migrate", "exact legacy factory layout")):
            output = io.StringIO()
            with self.subTest(action=action), redirect_stdout(output):
                flash.main(["--image-dir", str(self.images), "--dry-run", action])
            self.assertIn(expected, output.getvalue())

    def test_dry_run_does_not_require_esptool(self):
        with patch.object(flash, "require_esptool", side_effect=AssertionError("esptool required during dry-run")):
            with redirect_stdout(io.StringIO()):
                self.assertEqual(flash.main(["--image-dir", str(self.images), "--dry-run", "update"]), 0)

    def test_dry_run_never_calls_runner_or_creates_backup_directory(self):
        def prohibited(command):
            self.fail(f"Dry-run executed a device command: {command}")
        self.assertEqual(self.invoke("update", prohibited, dry=True), 0)
        self.assertFalse(self.backups.exists())

    def test_image_sector_bounds_and_supplied_layout_fail_before_device_access(self):
        cases = [("bootloader.bin", b"\xe9" + b"B" * 0x7000),
                 ("partitions.bin", b"P" * 4097),
                 ("partitions.bin", b"\xff" * 4096)]
        for name, data in cases:
            original = (self.images / name).read_bytes()
            try:
                (self.images / name).write_bytes(data)
                recorder = Recorder()
                with self.subTest(name=name), self.assertRaises(flash.FlashError):
                    self.invoke("install", recorder)
                self.assertEqual(recorder.commands, [])
            finally:
                (self.images / name).write_bytes(original)

    def test_short_backup_read_aborts_before_write(self):
        recorder = Recorder(config=b"short")
        with self.assertRaises(flash.FlashError):
            self.invoke("install", recorder)
        self.assertFalse(any("write_flash" in c for c in recorder.commands))
        self.assertEqual(list(self.backups.glob("*.bin")), [])

    def test_minimum_positive_baud_is_accepted(self):
        recorder = Recorder()
        with redirect_stdout(io.StringIO()):
            self.assertEqual(flash.main(["--baud", "1", "--port", "TEST_PORT", "backup-config", "--file", str(self.root / "b")], runner=recorder), 0)
        self.assertIn("1", recorder.commands[0])

    def test_transport_uses_builtin_runner_when_no_runner_injected(self):
        recorder = Recorder()
        args = ["--port", "TEST_PORT", "backup-config", "--file", str(self.root / "backup")]
        with patch.object(flash, "require_esptool"), patch.object(flash, "subprocess_runner", recorder):
            with redirect_stdout(io.StringIO()):
                self.assertEqual(flash.main(args), 0)
        self.assertTrue(recorder.commands)

    def test_install_does_not_attempt_device_layout_read(self):
        recorder = Recorder()
        self.assertEqual(self.invoke("install", recorder), 0)
        self.assertFalse(any("read_flash" in c and "0x8000" in c for c in recorder.commands))

    def test_partial_backup_read_failure_removes_private_partial_output(self):
        path = self.root / "partial.bin"
        def fail_read(command):
            if "flash_id" in command:
                return "Detected flash size: 16MB\n"
            Path(command[-1]).write_bytes(b"partial")
            raise flash.FlashError("read failed")
        with self.assertRaisesRegex(flash.FlashError, "read failed"):
            self.invoke("backup-config", fail_read, "--file", str(path))
        self.assertFalse(path.exists())

    def test_concurrent_backup_creation_preserves_the_existing_file(self):
        path = self.root / "race.bin"
        def race(command):
            if "flash_id" in command:
                path.write_bytes(b"existing")
                return "Detected flash size: 16MB\n"
            self.fail("Existing file must not be passed to esptool")
        original_open = os.open
        def concurrent_open(target, flags, *args, **kwargs):
            if Path(target) == path:
                raise FileExistsError("already exists")
            return original_open(target, flags, *args, **kwargs)
        with patch("os.open", side_effect=concurrent_open):
            with self.assertRaisesRegex(flash.FlashError, "overwrite"):
                self.invoke("backup-config", race, "--file", str(path))
        self.assertEqual(path.read_bytes(), b"existing")

    def test_backup_does_not_overwrite_file_created_after_preflight(self):
        path = self.root / "race.bin"
        def race(command):
            if "flash_id" in command:
                path.write_bytes(b"existing")
                return "Detected flash size: 16MB\n"
            self.fail("Existing file must not be passed to esptool")
        with self.assertRaisesRegex(flash.FlashError, "overwrite"):
            self.invoke("backup-config", race, "--file", str(path))
        self.assertEqual(path.read_bytes(), b"existing")

    def test_nonpositive_baud_is_rejected_before_device_access(self):
        recorder = Recorder()
        for baud in ("0", "-1"):
            with self.subTest(baud=baud), patch("sys.stderr", io.StringIO()), self.assertRaises(SystemExit):
                flash.main(["--baud", baud, "--port", "TEST_PORT", "update"], runner=recorder)
            self.assertEqual(recorder.commands, [])

    def test_real_device_action_requires_port_before_runner(self):
        recorder = Recorder()
        from contextlib import redirect_stderr
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            flash.main(["--image-dir", str(self.images), "update"], runner=recorder)
        self.assertEqual(raised.exception.code, 2)
        self.assertEqual(recorder.commands, [])

    def test_manifest_rejects_bad_missing_duplicate_and_extra_entries(self):
        names = ["bootloader.bin", "partitions.bin", "firmware.bin"]
        good = [hashlib.sha256((self.images / name).read_bytes()).hexdigest() + "  " + name for name in names]
        manifest = self.images / "SHA256SUMS.txt"
        manifest.write_text("\n".join(good) + "\n")
        self.assertEqual(self.invoke("update", dry=True), 0)
        for lines in (["0" * 64 + "  bootloader.bin"] + good[1:], good[:-1], good + good[:1], good + ["0" * 64 + "  ../extra.bin"]):
            manifest.write_text("\n".join(lines) + "\n")
            recorder = Recorder()
            with self.assertRaises(flash.FlashError):
                self.invoke("update", recorder)
            self.assertEqual(recorder.commands, [])


if __name__ == "__main__":
    unittest.main()
