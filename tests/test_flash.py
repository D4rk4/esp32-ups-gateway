"""Flashing safety tests use recorded commands; never access a serial device."""
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

MODULE = Path(os.path.abspath(__file__)).parents[1] / "tools" / "flash.py"
spec = importlib.util.spec_from_file_location("gateway_flash", MODULE)
flash = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = flash
spec.loader.exec_module(flash)


def partition_table(entries=None):
    if entries is None:
        entries = [("nvs", 1, 2, 0x9000, 0x5000),
                   ("factory", 0, 0, 0x10000, 0x3F0000),
                   ("appcfg", 1, 2, 0xF00000, 0x10000)]
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

    def test_exact_partition_layout_and_md5(self):
        self.assertEqual(len(flash.validate_partition_table(partition_table())), 3)
        damaged = bytearray(partition_table()); damaged[20] ^= 1
        with self.assertRaises(flash.FlashError):
            flash.validate_partition_table(bytes(damaged))

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

    def test_oversize_app_and_invalid_magic_reject_before_any_device_call(self):
        for content in (b"\xe9" + b"F" * 0x3F0000, b"bad header"):
            (self.images / "firmware.bin").write_bytes(content)
            recorder = Recorder()
            with self.assertRaises(flash.FlashError):
                self.invoke("update", recorder)
            self.assertEqual(recorder.commands, [])

    def test_update_writes_only_application_and_retains_config(self):
        recorder = Recorder()
        self.assertEqual(self.invoke("update", recorder), 0)
        writes = [c for c in recorder.commands if "write_flash" in c]
        self.assertEqual(len(writes), 1)
        self.assertEqual(writes[0][-2:], ["0x10000", str(self.images / "firmware.bin")])
        self.assertNotIn("0x1000", writes[0]); self.assertNotIn("0x8000", writes[0]); self.assertNotIn("0xf00000", writes[0])
        self.assertEqual(list(self.backups.glob("*.bin"))[0].read_bytes(), recorder.config)
        self.assertTrue(any("read_flash" in c and "0x8000" in c for c in recorder.commands))
        self.assertEqual(sum("read_flash" in c and "0xf00000" in c for c in recorder.commands), 2)
        self.assertEqual(recorder.commands[-1][recorder.commands[-1].index("--after") + 1], "hard_reset")
        for command in recorder.commands[:-1]:
            self.assertEqual(command[command.index("--after") + 1], "no_reset")
        for command in recorder.commands:
            self.assertEqual(command[:3], [sys.executable, "-m", "esptool"])
            self.assertNotIn("erase_flash", command)

    def test_install_has_only_three_approved_write_addresses(self):
        recorder = Recorder()
        self.invoke("install", recorder)
        write = next(c for c in recorder.commands if "write_flash" in c)
        self.assertEqual(write[-6::2], ["0x1000", "0x8000", "0x10000"])

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

    def test_erase_requires_exact_confirmation_before_device_access(self):
        for extra in ([], ["--confirm", "WRONG"]):
            recorder = Recorder()
            with self.subTest(extra=extra), self.assertRaises(flash.FlashError):
                self.invoke("erase", recorder, *extra)
            self.assertEqual(recorder.commands, [])

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
