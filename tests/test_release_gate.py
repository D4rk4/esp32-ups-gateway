"""Unit-test fakes only; these fixtures are never release evidence."""
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import release_gate as gate

spec = importlib.util.spec_from_file_location("package_release", ROOT / "tools/package_release.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def process(data, returncode=0):
    return subprocess.CompletedProcess(["unit-test-fake"], returncode, json.dumps(data), "")


def fixture(root):
    for name in ("README_RU.md", "LICENSE", "THIRD_PARTY.md", "requirements.txt",
                 "platformio.ini", "partitions.csv", ".gitignore", "src/example.cpp",
                 "lib/code.hpp", "web/index.html", "tools/semgrep-release.yml",
                 "integration/observium/esp32ups.inc.php", "tests/test_fixture.py",
                 "firmware/bootloader.bin", "firmware/partitions.bin", "firmware/firmware.bin",
                 "firmware/SHA256SUMS.txt", "firmware/build-info.json", "docs/guide.md",
                 "reference/source.txt"):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("initial " + name, encoding="utf-8")


class ReleasePackageTests(unittest.TestCase):
    def test_failed_gate_moves_stale_zip_to_history(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            archive = root / "dist/esp32-ups-gateway-v0.2.0.zip"
            archive.parent.mkdir()
            archive.write_bytes(b"old release")
            with mock.patch.object(package, "ROOT", root), \
                 mock.patch.object(package, "validate_images"), \
                 mock.patch.object(package, "run_release_gate", return_value=False), \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(package.main(), 1)
            self.assertFalse(archive.exists())
            history = list(archive.parent.glob(archive.name + ".superseded-*"))
            self.assertEqual(len(history), 1)
            self.assertEqual(history[0].read_bytes(), b"old release")

    def test_archive_corruption_leaves_no_release_or_temporary_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            with mock.patch.object(package, "ROOT", root), \
                 mock.patch.object(package, "validate_images"), \
                 mock.patch.object(package, "run_release_gate", return_value=True), \
                 mock.patch.object(zipfile.ZipFile, "testzip", return_value="corrupted"), \
                 redirect_stdout(io.StringIO()):
                with self.assertRaises(ValueError):
                    package.main()
            self.assertEqual(list((root / "dist").iterdir()), [])

    def test_gate_mutating_its_input_cannot_change_published_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            def mutate_gate_input(snapshot):
                (snapshot / "src/example.cpp").write_text("mutated while scanning")
                return {name: ("PASS", "unit-test fake") for name in gate.CHECKS}
            with mock.patch.object(package, "ROOT", root), \
                 mock.patch.object(package, "validate_images"), \
                 mock.patch.object(gate, "run_checks", side_effect=mutate_gate_input), \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(package.main(), 1)
            self.assertFalse((root / "dist/esp32-ups-gateway-v0.2.0.zip").exists())

    def test_archive_write_error_leaves_only_historical_zip(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            archive = root / "dist/esp32-ups-gateway-v0.2.0.zip"
            archive.parent.mkdir()
            archive.write_bytes(b"old release")
            with mock.patch.object(package, "ROOT", root), \
                 mock.patch.object(package, "validate_images"), \
                 mock.patch.object(package, "run_release_gate", return_value=True), \
                 mock.patch.object(zipfile.ZipFile, "write", side_effect=OSError("disk full")), \
                 redirect_stdout(io.StringIO()):
                with self.assertRaises(OSError):
                    package.main()
            self.assertFalse(archive.exists())
            history = list(archive.parent.glob(archive.name + ".superseded-*"))
            self.assertEqual(len(history), 1)
            self.assertEqual(history[0].read_bytes(), b"old release")
            self.assertEqual(set(archive.parent.iterdir()), set(history))

    def test_source_change_during_gate_is_not_packaged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            source = root / "src/example.cpp"
            def change_live_tree(snapshot):
                self.assertNotEqual(snapshot, root)
                source.write_text("changed after snapshot")
                return True
            with mock.patch.object(package, "ROOT", root), \
                 mock.patch.object(package, "validate_images"), \
                 mock.patch.object(package, "run_release_gate", side_effect=change_live_tree), \
                 redirect_stdout(io.StringIO()):
                self.assertEqual(package.main(), 0)
            with zipfile.ZipFile(root / "dist/esp32-ups-gateway-v0.2.0.zip") as archive:
                self.assertEqual(archive.read("esp32-ups-gateway/src/example.cpp"), b"initial src/example.cpp")

    def test_skip_option_is_not_accepted(self):
        result = subprocess.run([sys.executable, str(ROOT / "tools/package_release.py"), "--skip"],
                                text=True, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("accepts no options", result.stderr)

    def test_missing_tools_cannot_create_or_overwrite_release(self):
        for existing in (False, True):
            with self.subTest(existing_archive=existing), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                fixture(root)
                archive = root / "dist/esp32-ups-gateway-v0.2.0.zip"
                if existing:
                    archive.parent.mkdir()
                    archive.write_bytes(b"existing archive")
                with mock.patch.object(package, "ROOT", root), \
                     mock.patch.object(package, "validate_images"), \
                     mock.patch.dict(os.environ, {"PATH": ""}):
                    result = package.main()
                self.assertNotEqual(result, 0)
                self.assertFalse(archive.exists())
                if existing:
                    history = list(archive.parent.glob(archive.name + ".superseded-*"))
                    self.assertEqual(len(history), 1)
                    self.assertEqual(history[0].read_bytes(), b"existing archive")
                else:
                    self.assertFalse(archive.parent.exists())


class UnsupportedGateTests(unittest.TestCase):
    def test_full_gate_does_not_pass_an_unsupported_or_failed_check(self):
        with mock.patch.object(gate, "run_checks", return_value={
            "semgrep": ("PASS", "unit-test fake"),
            "trivy": ("PASS", "unit-test fake"),
            "coverage": ("PASS", "unit-test fake"),
            "crapper": ("BLOCKED", "C++ unsupported"),
            "dryer": ("PASS", "unit-test fake"),
            "mutator": ("FAIL", "survivor"),
            "freshness": ("PASS", "unit-test fake"),
        }):
            self.assertFalse(gate.run(ROOT))


    def test_source_test_or_firmware_change_during_checks_blocks_package(self):
        for changed in ("src", "tests", "firmware"):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                fixture(root)
                archive = root / "dist/esp32-ups-gateway-v0.2.0.zip"
                archive.parent.mkdir()
                archive.write_bytes(b"original")
                def alter(snapshot, *args):
                    (snapshot / changed / "changed-file").write_text("changed")
                    return "PASS", "unit-test fake"
                with mock.patch.object(package, "ROOT", root), \
                     mock.patch.object(package, "validate_images"), \
                     mock.patch.object(gate, "source_files", return_value=[]), \
                     mock.patch.object(gate, "fingerprint", side_effect=lambda root: (root / changed / "changed-file").exists()), \
                     mock.patch.object(gate, "scan_semgrep", side_effect=alter), \
                     mock.patch.object(gate, "scan_trivy", return_value=("PASS", "unit-test fake")), \
                     mock.patch.object(gate, "check_coverage", return_value=("PASS", "unit-test fake")), \
                     mock.patch.object(gate, "check_quality_tool", return_value=("PASS", "unit-test fake")), \
                     redirect_stdout(io.StringIO()):
                    self.assertEqual(package.main(), 1)
                self.assertFalse(archive.exists())
                history = list(archive.parent.glob(archive.name + ".superseded-*"))
                self.assertEqual(len(history), 1)
                self.assertEqual(history[0].read_bytes(), b"original")

    def test_failure_of_any_individual_gate_preserves_archive(self):
        for failed in gate.CHECKS:
            for status in ("FAIL", "BLOCKED"):
                with self.subTest(check=failed, status=status), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    fixture(root)
                    archive = root / "dist/esp32-ups-gateway-v0.2.0.zip"
                    archive.parent.mkdir()
                    archive.write_bytes(b"original")
                    checks = {name: ("PASS", "unit-test fake") for name in gate.CHECKS}
                    checks[failed] = (status, "unit-test fake")
                    with mock.patch.object(package, "ROOT", root), \
                         mock.patch.object(package, "validate_images"), \
                         mock.patch.object(gate, "run_checks", return_value=checks):
                        self.assertNotEqual(package.main(), 0)
                    self.assertFalse(archive.exists())
                    history = list(archive.parent.glob(archive.name + ".superseded-*"))
                    self.assertEqual(len(history), 1)
                    self.assertEqual(history[0].read_bytes(), b"original")

    def test_unit_fake_all_pass_allows_fixture_packaging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            checks = {name: ("PASS", "unit-test fake") for name in gate.CHECKS}
            with mock.patch.object(package, "ROOT", root), \
                 mock.patch.object(package, "validate_images"), \
                 mock.patch.object(gate, "run_checks", return_value=checks):
                self.assertEqual(package.main(), 0)
            self.assertTrue((root / "dist/esp32-ups-gateway-v0.2.0.zip").is_file())


class ScannerResultTests(unittest.TestCase):
    def test_semgrep_finding_with_exit_zero_fails(self):
        report = {"results": [{"check_id": "test"}], "errors": [],
                  "paths": {"scanned": ["src/example.cpp"]}}
        self.assertEqual(gate.semgrep_result(process(report), {"src/example.cpp"})[0], "FAIL")

    def test_semgrep_process_error_or_skipped_target_blocks(self):
        report = {"results": [], "errors": [], "paths": {"scanned": []}}
        for result in (process(report, 2), process(report)):
            with self.subTest(returncode=result.returncode):
                self.assertEqual(gate.semgrep_result(result, {"src/example.cpp"})[0], "BLOCKED")

    def test_semgrep_clean_exact_targets_pass(self):
        report = {"results": [], "errors": [], "paths": {"scanned": ["src/example.cpp"]}}
        self.assertEqual(gate.semgrep_result(process(report), {"src/example.cpp"})[0], "PASS")

    def test_public_gate_rejects_medium_and_missing_package_inventory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            (root / "requirements.txt").write_text("example==1.0\n")
            entry = {"Target": "requirements.txt", "Class": "lang-pkgs", "Type": "pip"}
            for issue, expected in (({"Packages": [{"Name": "example", "Version": "1.0"}],
                                      "Vulnerabilities": [{"Severity": "MEDIUM"}]}, "FAIL"),
                                    ({}, "BLOCKED")):
                with self.subTest(issue=issue):
                    report = process({"SchemaVersion": 2, "Results": [{**entry, **issue}]})
                    def invoke(args, *_args, **_kwargs):
                        if "--version" in args:
                            return subprocess.CompletedProcess(args, 0, "1.0", "")
                        return report
                    with mock.patch.object(gate.shutil, "which", return_value="/fake/trivy"), \
                         mock.patch.object(gate, "command", side_effect=invoke), \
                         mock.patch.object(gate, "scan_semgrep", return_value=("PASS", "unit-test fake")), \
                         mock.patch.object(gate, "check_coverage", return_value=("PASS", "unit-test fake")), \
                         mock.patch.object(gate, "check_quality_tool", return_value=("PASS", "unit-test fake")):
                        output = io.StringIO()
                        with redirect_stdout(output):
                            self.assertFalse(gate.run(root))
                    self.assertIn("trivy: " + expected, output.getvalue())
                    if not issue:
                        self.assertIn("packages", output.getvalue().lower())

    def test_trivy_high_and_secret_findings_with_exit_zero_fail(self):
        for item in ({"Vulnerabilities": [{"Severity": "HIGH"}]},
                     {"Secrets": [{"RuleID": "test"}]}):
            with self.subTest(item=item):
                item.update({"Target": "requirements.txt", "Class": "lang-pkgs", "Type": "pip"})
                report = {"SchemaVersion": 2, "Results": [item]}
                self.assertEqual(gate.trivy_result(process(report), {"example": "1.0"})[0], "FAIL")

    def test_trivy_error_or_invalid_report_blocks(self):
        for result in (process({"SchemaVersion": 2, "Results": []}, 2), process({})):
            with self.subTest(returncode=result.returncode):
                self.assertEqual(gate.trivy_result(result, {"example": "1.0"})[0], "BLOCKED")

    def test_trivy_rejects_all_finding_types_and_unknown_class(self):
        entry = {"Target": "requirements.txt", "Class": "lang-pkgs", "Type": "pip",
                 "Packages": [{"Name": "example", "Version": "1.0"}]}
        for key in ("Vulnerabilities", "Secrets", "Misconfigurations", "Licenses"):
            with self.subTest(key=key):
                report = {"SchemaVersion": 2, "Results": [{**entry, key: [{}]}]}
                self.assertEqual(gate.trivy_result(process(report), {"example": "1.0"})[0], "FAIL")
        report = {"SchemaVersion": 2, "Results": [entry, {"Class": "unknown"}]}
        self.assertEqual(gate.trivy_result(process(report), {"example": "1.0"})[0], "BLOCKED")
        for packages in ([], [{"Name": "example", "Version": "0.9"}],
                         [{"Name": "other", "Version": "1.0"}]):
            with self.subTest(packages=packages):
                report = {"SchemaVersion": 2, "Results": [{**entry, "Packages": packages}]}
                self.assertEqual(gate.trivy_result(process(report), {"example": "1.0"})[0], "BLOCKED")

    def test_trivy_without_dependency_manifest_result_blocks(self):
        report = {"SchemaVersion": 2, "Results": [{"Target": "src/main.cpp", "Class": "secret"}]}
        self.assertEqual(gate.trivy_result(process(report), {"example": "1.0"})[0], "BLOCKED")

    def test_semgrep_does_not_accept_duplicate_or_malformed_findings(self):
        report = {"results": [], "errors": [], "paths": {"scanned": ["src/example.cpp", 1]}}
        self.assertEqual(gate.semgrep_result(process(report), {"src/example.cpp"})[0], "BLOCKED")


class CoverageResultTests(unittest.TestCase):
    def test_incomplete_zero_or_rounded_coverage_never_passes(self):
        for lines, branches in (((9999, 10000), (1, 1)), ((0, 0), (0, 0)),
                                ((10000, 10000), (9999, 10000))):
            with self.subTest(lines=lines, branches=branches):
                result = gate.coverage_result({"a.py": {"lines": lines, "branches": branches}},
                                              {"a.py"})
                self.assertNotEqual(result[0], "PASS")

    def test_missing_file_and_uninstrumented_header_block(self):
        report = {"a.py": {"lines": (5, 5), "branches": (2, 2)}}
        self.assertNotEqual(gate.coverage_result(report, {"a.py", "src/a.hpp"})[0], "PASS")


class EnvironmentIsolationTests(unittest.TestCase):
    def test_public_gate_ignores_hostile_scanner_and_coverage_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "project"
            root.mkdir()
            fixture(root)
            (root / "requirements.txt").write_text("example==1.0\n")
            (root / "tools/a.py").write_text("x = 1\n")
            outside = base / "external-coverage.db"
            outside.write_bytes(b"do not overwrite")
            script = base / "fake-scanner.py"
            script.write_text(
                "import json, os, pathlib\n"
                "p = os.environ.get('COVERAGE_FILE')\n"
                "if p: pathlib.Path(p).write_bytes(b'coverage run')\n"
                "entry = {'Target':'requirements.txt','Class':'lang-pkgs','Type':'pip',"
                "'Packages':[{'Name':'example','Version':'1.0'}]}\n"
                "if os.environ.get('TRIVY_SEVERITY') != 'CRITICAL': "
                "entry['Vulnerabilities'] = [{'Severity':'MEDIUM'}]\n"
                "print(json.dumps({'SchemaVersion':2,'Results':[entry]}))\n"
            )
            real_command = gate.command
            def invoke(args, cwd, timeout=120):
                return real_command([sys.executable, str(script)], cwd, timeout)
            with mock.patch.dict(os.environ, {"COVERAGE_FILE": str(outside),
                                              "TRIVY_SEVERITY": "CRITICAL",
                                              "SEMGREP_EXCLUDE": "*"}), \
                 mock.patch.object(gate.shutil, "which", return_value="unit-test-fake"), \
                 mock.patch.object(gate, "version", return_value="unit-test fake"), \
                 mock.patch.object(gate, "command", side_effect=invoke), \
                 mock.patch.object(gate, "scan_semgrep", return_value=("PASS", "unit-test fake")), \
                 mock.patch.object(gate, "check_coverage", return_value=("PASS", "unit-test fake")), \
                 mock.patch.object(gate, "check_quality_tool", return_value=("PASS", "unit-test fake")):
                output = io.StringIO()
                with redirect_stdout(output):
                    self.assertFalse(gate.run(root))
            self.assertEqual(outside.read_bytes(), b"do not overwrite")
            self.assertIn("trivy: FAIL", output.getvalue())


class QualityReportTests(unittest.TestCase):
    def test_crapper_snapshot_includes_complete_test_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "root"
            root.mkdir()
            fixture(root)
            (root / "tools/a.py").write_text("x = 1\n")
            temporary = Path(directory) / "temporary"
            temporary.mkdir()
            observed = []
            def invoke(args, snapshot, timeout=120):
                if "run" in args:
                    observed.append((snapshot / "firmware/firmware.bin").is_file())
                    return subprocess.CompletedProcess(args, 2 if not observed[-1] else 0, "", "")
                return subprocess.CompletedProcess(args, 0, "fake", "")
            with mock.patch.object(gate.shutil, "which", return_value="/fake/tool"), \
                 mock.patch.object(gate, "command", side_effect=invoke), \
                 redirect_stdout(io.StringIO()):
                gate.check_quality_tool("crapper", root, gate.source_files(root), temporary)
            self.assertEqual(observed, [True])

    def test_unsupported_cpp_is_reported_without_running_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(gate, "command") as invoke:
                status, detail = gate.check_quality_tool("mutator", ROOT, gate.source_files(ROOT), directory)
            self.assertEqual(status, "BLOCKED")
            self.assertIn("C/C++", detail)
            invoke.assert_not_called()

    def test_crap_rejects_ten_missing_coverage_or_empty_report(self):
        for report in ('{:entries []}', '{:entries [{:coverage nil :crap nil}]}',
                       '{:entries [{:coverage 100.0 :crap 10.0}]}'):
            with self.subTest(report=report):
                self.assertNotEqual(gate.crap_result(report)[0], "PASS")

    def test_dryer_exit_zero_candidate_still_fails(self):
        self.assertEqual(gate.dryer_result('{:candidates [{:score 0.8}]}')[0], "FAIL")
        self.assertEqual(gate.dryer_result('not a report')[0], "BLOCKED")


class MutationResultTests(unittest.TestCase):
    def test_surviving_or_uncovered_mutants_fail(self):
        for total in ((1, 1, 0, 2), (1, 0, 1, 2)):
            with self.subTest(total=total):
                text = "Function Namespace Killed Survived Uncovered Sites Score\nTotal " + " ".join(map(str, total)) + " 50.0%\n"
                self.assertEqual(gate.mutation_result(subprocess.CompletedProcess([], 0, text, ""))[0], "FAIL")

    def test_zero_or_incomplete_mutants_block(self):
        for total in ((0, 0, 0, 0), (1, 0, 0, 2)):
            text = "Function Namespace Killed Survived Uncovered Sites Score\nTotal " + " ".join(map(str, total)) + " 100.0%\n"
            self.assertEqual(gate.mutation_result(subprocess.CompletedProcess([], 0, text, ""))[0], "BLOCKED")


class FreshnessTests(unittest.TestCase):
    def test_packaged_files_and_new_ino_change_fingerprint(self):
        names = ("integration/observium/esp32ups.inc.php", "firmware/SHA256SUMS.txt",
                 "firmware/build-info.json", "README_RU.md", "docs/guide.md",
                 "reference/source.txt", "LICENSE", "THIRD_PARTY.md", ".gitignore")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            before = gate.fingerprint(root)
            for name in names:
                with self.subTest(name=name):
                    path = root / name
                    original = path.read_bytes()
                    path.write_bytes(original + b" changed")
                    self.assertNotEqual(gate.fingerprint(root), before)
                    path.write_bytes(original)
            (root / "src/new.ino").write_text("void setup() {}")
            self.assertNotEqual(gate.fingerprint(root), before)

    def test_php_and_ino_are_owned_code(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            (root / "src/new.ino").write_text("void setup() {}")
            files = gate.source_files(root)
            self.assertIn("src/new.ino", files)
            self.assertIn("integration/observium/esp32ups.inc.php", files)

    def test_unknown_source_suffix_blocks_public_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            (root / "src/new.mystery").write_text("unrecognized code")
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertFalse(gate.run(root))
            self.assertIn("src/new.mystery", output.getvalue())
            self.assertIn("BLOCKED", output.getvalue())

    def test_source_test_and_firmware_changes_are_detected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            for name in ("src/a.cpp", "tests/test_a.py"):
                path = root / name
                path.write_text("initial")
            before = gate.fingerprint(root)
            for name in ("src/a.cpp", "tests/test_a.py", "firmware/firmware.bin"):
                with self.subTest(name=name):
                    path = root / name
                    original = path.read_bytes()
                    path.write_text("changed")
                    self.assertNotEqual(gate.fingerprint(root), before)
                    path.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
