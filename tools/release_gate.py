#!/usr/bin/env python3
"""Inline release checks. No saved report is accepted as release evidence."""
from pathlib import Path
import hashlib
from html.parser import HTMLParser
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

SOURCE_DIRS = ("src", "lib", "web", "tools", "integration")
EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".ino", ".py", ".php",
              ".js", ".mjs", ".cjs", ".jsx", ".html"}
NON_CODE = {".md", ".txt", ".example", ".yml", ".yaml"}
SKIP_DIRS = {"__pycache__"}
DIRECTORIES = (*SOURCE_DIRS, "tests", "firmware", "docs", "reference")
ROOT_FILES = ("README.md", "LICENSE", "THIRD_PARTY.md", "requirements.txt",
              "platformio.ini", "partitions.csv", ".gitignore")
REQUIRED = (*ROOT_FILES, "firmware/bootloader.bin", "firmware/partitions.bin",
            "firmware/firmware.bin", "firmware/SHA256SUMS.txt", "firmware/build-info.json",
            "tools/semgrep-release.yml")
CHECKS = ("semgrep", "trivy", "coverage", "crapper", "dryer", "mutator", "freshness")


def inventory(root):
    files = list(ROOT_FILES)
    for name in ROOT_FILES:
        if (root / name).is_symlink():
            raise ValueError(f"symlink in release inventory: {name}")
    for name in DIRECTORIES:
        directory = root / name
        if directory.is_symlink():
            raise ValueError(f"symlink in release inventory: {name}")
        if not directory.is_dir():
            raise ValueError(f"missing release directory: {name}")
        for path in directory.rglob("*"):
            relative = path.relative_to(root)
            if path.is_symlink():
                raise ValueError(f"symlink in release inventory: {relative.as_posix()}")
            if not path.is_file() or set(relative.parts) & SKIP_DIRS or path.suffix == ".pyc":
                continue
            if relative.parts[:2] in (("docs", "tasks"), ("docs", "superpowers")):
                continue
            if relative.as_posix() in ("docs/PROGRESS.md", "src/web_assets.hpp"):
                continue
            files.append(relative.as_posix())
    for name in REQUIRED:
        if not (root / name).is_file():
            raise ValueError(f"missing release input: {name}")
    return sorted(set(files))


def copy_snapshot(root, snapshot, files):
    for name in files:
        parts = Path(name).parts
        descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            for part in parts[:-1]:
                child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor)
                os.close(descriptor)
                descriptor = child
            source = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW, dir_fd=descriptor)
            target = snapshot / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with os.fdopen(source, "rb") as input_file, target.open("wb") as output_file:
                shutil.copyfileobj(input_file, output_file)
        finally:
            os.close(descriptor)


def source_files(root):
    result = []
    for name in inventory(root):
        path = Path(name)
        if path.parts[0] not in SOURCE_DIRS:
            continue
        suffix = path.suffix.lower()
        if suffix in EXTENSIONS:
            result.append(name)
        elif suffix not in NON_CODE:
            raise ValueError(f"unknown owned source suffix: {name}")
    if not result:
        raise ValueError("owned source inventory is empty")
    return result


def file_digests(root):
    return {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in inventory(root)}


def fingerprint(root, digests=None):
    digest = hashlib.sha256()
    for name, value in (file_digests(root) if digests is None else digests).items():
        digest.update(name.encode("utf-8") + b"\0" + bytes.fromhex(value))
    return digest.hexdigest()


class Scripts(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.inside = False
        self.parts = []
        self.has_external = False
        self.has_inline_handler = False

    def handle_starttag(self, tag, attrs):
        self.has_inline_handler |= any(name.lower().startswith("on") for name, _ in attrs)
        if tag.lower() == "script":
            self.has_external |= any(name.lower() == "src" for name, _ in attrs)
            self.inside = True
            self.parts.append("\n")

    def handle_endtag(self, tag):
        if tag.lower() == "script":
            self.inside = False
            self.parts.append("\n")

    def handle_data(self, data):
        if self.inside:
            self.parts.append(data)


def command(args, root, timeout=120):
    print("COMMAND", shlex.join(str(arg) for arg in args), flush=True)
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith(("COVERAGE_", "TRIVY_", "SEMGREP_"))}
    environment["PATH"] = str(Path(sys.executable).parent) + os.pathsep + environment.get("PATH", "")
    environment["COVERAGE_FILE"] = str(root / ".coverage")
    environment["COVERAGE_RCFILE"] = str(root / ".coverage-empty-config")
    (root / ".coverage-empty-config").touch(exist_ok=True)
    try:
        return subprocess.run(args, cwd=root, env=environment, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        print("COMMAND BLOCKED", str(error), flush=True)
        return None


def version(executable, root):
    result = command([executable, "--version"], root, 15)
    if result is None or result.returncode != 0 or not result.stdout.strip():
        return None
    text = " ".join(result.stdout.splitlines())
    print("VERSION", text, flush=True)
    return text


def semgrep_result(result, expected):
    if result is None or result.returncode != 0:
        return "BLOCKED", "Semgrep failed or timed out"
    try:
        data = json.loads(result.stdout)
        findings, errors = data["results"], data["errors"]
        scanned = data["paths"]["scanned"]
        if (not isinstance(findings, list) or not isinstance(errors, list)
                or not isinstance(scanned, list) or not all(isinstance(p, str) for p in scanned)):
            raise ValueError("invalid report fields")
        if errors:
            return "BLOCKED", f"Semgrep reported {len(errors)} errors"
        missing = expected - set(scanned)
        if missing or not expected:
            return "BLOCKED", f"Semgrep missed {len(missing)} targets"
        if findings:
            return "FAIL", f"{len(findings)} Semgrep findings"
        return "PASS", f"{len(scanned)} targets scanned; no findings"
    except (KeyError, TypeError, ValueError):
        return "BLOCKED", "invalid Semgrep report"


def pinned_requirements(path):
    packages = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([A-Za-z0-9][A-Za-z0-9_.-]*)==([A-Za-z0-9][A-Za-z0-9_.+!-]*)", line)
        if not match:
            raise ValueError("requirements.txt contains an unsupported or unpinned requirement")
        name = re.sub(r"[-_.]+", "-", match.group(1)).lower()
        if name in packages:
            raise ValueError("requirements.txt contains duplicate packages")
        packages[name] = match.group(2)
    if not packages:
        raise ValueError("requirements.txt contains no pinned packages")
    return packages


def trivy_result(result, expected):
    if result is None or result.returncode != 0:
        return "BLOCKED", "Trivy failed or timed out"
    try:
        data = json.loads(result.stdout)
        if data["SchemaVersion"] != 2 or not isinstance(data["Results"], list) or not data["Results"]:
            raise ValueError("invalid results")
        manifests = []
        findings = {key: 0 for key in ("Vulnerabilities", "Secrets", "Misconfigurations", "Licenses")}
        for entry in data["Results"]:
            unknown = set(entry) - {"Target", "Class", "Type", "Packages", "Vulnerabilities",
                                    "Secrets", "Misconfigurations", "Licenses"}
            if unknown:
                return "BLOCKED", f"unknown Trivy result field: {', '.join(sorted(unknown))}"
            if entry.get("Class") not in ("lang-pkgs", "os-pkgs", "secret", "config", "license"):
                raise ValueError("unknown result class")
            for key in findings:
                issues = entry.get(key, [])
                if not isinstance(issues, list):
                    raise ValueError("invalid findings")
                findings[key] += len(issues)
            if (entry.get("Target") == "requirements.txt"
                    and entry.get("Class") == "lang-pkgs" and entry.get("Type") == "pip"):
                manifests.append(entry)
        if any(findings.values()):
            return "FAIL", (f"{findings['Vulnerabilities']} vulnerabilities, {findings['Secrets']} secrets, "
                            f"{findings['Misconfigurations']} misconfigurations, {findings['Licenses']} licenses")
        if len(manifests) != 1 or not isinstance(manifests[0].get("Packages"), list):
            return "BLOCKED", "requirements.txt packages missing from Trivy report"
        packages = {}
        for package in manifests[0]["Packages"]:
            name = re.sub(r"[-_.]+", "-", package["Name"]).lower()
            if name in packages or not isinstance(package["Version"], str):
                raise ValueError("invalid packages")
            packages[name] = package["Version"]
        if not expected or packages != expected:
            return "BLOCKED", "Trivy packages do not match pinned requirements.txt"
        return "BLOCKED", "license data for pinned packages was not produced; license scope not evidenced"
    except (KeyError, TypeError, ValueError):
        return "BLOCKED", "invalid or empty Trivy report"


def coverage_result(report, expected):
    if not isinstance(report, dict) or not expected or expected - set(report):
        return "BLOCKED", f"uninstrumented files: {', '.join(sorted(expected - set(report or {})))}"
    total_lines = total_branches = 0
    try:
        for name in expected:
            entry = report[name]
            for kind in ("lines", "branches"):
                covered, total = entry[kind]
                if type(covered) is not int or type(total) is not int or not 0 <= covered <= total:
                    raise ValueError("invalid raw counts")
                if covered != total:
                    return "FAIL", f"{name}: {kind} {covered}/{total}"
                if kind == "lines":
                    total_lines += total
                else:
                    total_branches += total
        if not total_lines or not total_branches:
            return "BLOCKED", "zero executable lines or branches"
        return "PASS", f"lines {total_lines}/{total_lines}, branches {total_branches}/{total_branches}"
    except (KeyError, TypeError, ValueError):
        return "BLOCKED", "invalid coverage counts"


def scan_semgrep(root, files, temporary):
    executable = shutil.which("semgrep")
    if not executable or not version(executable, root):
        return "BLOCKED", "Semgrep missing or version unavailable"
    targets = [name for name in files if not name.endswith(".html")]
    html_files = [name for name in files if name.endswith(".html")]
    for number, name in enumerate(html_files):
        parser = Scripts()
        parser.feed((root / name).read_text(encoding="utf-8"))
        script = "".join(parser.parts)
        if not script.strip() or parser.has_external:
            return "BLOCKED", f"inline JavaScript not fully extractable: {name}"
        # Attribute handlers are identified separately; they are never silently ignored.
        if parser.has_inline_handler:
            return "BLOCKED", f"inline event handlers are not included in Semgrep extraction: {name}"
        target = Path(temporary) / f"inline-{number}.js"
        target.write_text(script, encoding="utf-8")
        targets.append(str(target))
    args = [executable, "scan", "--config", str(root / "tools/semgrep-release.yml"),
            "--json", "--metrics=off", "--disable-version-check", "--disable-nosem",
            "--no-git-ignore", "--max-target-bytes=0", "--strict", *targets]
    result = command(args, root, 180)
    status, detail = semgrep_result(result, set(targets))
    unsupported = [name for name in files if name.endswith(".ino")]
    if status == "PASS" and unsupported:
        return "BLOCKED", f"Semgrep baseline does not cover: {', '.join(unsupported)}"
    return status, detail


def scan_trivy(root, temporary):
    executable = shutil.which("trivy")
    if not executable or not version(executable, root):
        return "BLOCKED", "Trivy missing or version unavailable; PlatformIO dependencies not covered by Trivy"
    snapshot = Path(temporary) / "trivy"
    snapshot.mkdir()
    for directory in SOURCE_DIRS:
        shutil.copytree(root / directory, snapshot / directory,
                        ignore=shutil.ignore_patterns(*SKIP_DIRS, "*.pyc"))
    generated = snapshot / "src/web_assets.hpp"
    if generated.exists():
        generated.unlink()
    shutil.copy2(root / "requirements.txt", snapshot / "requirements.txt")
    args = [executable, "fs", "--format", "json", "--scanners", "vuln,secret,misconfig,license",
            "--list-all-pkgs", "--offline-scan", "--skip-version-check", "--timeout", "120s", "."]
    result = command(args, snapshot, 150)
    status, detail = trivy_result(result, pinned_requirements(snapshot / "requirements.txt"))
    # platformio.ini pins ArduinoJson but Trivy does not identify that manifest.
    # Scanning requirements.txt alone is not dependency coverage for the firmware.
    return status, f"{detail}; PlatformIO dependencies not covered by Trivy"


def check_coverage(root, files, temporary):
    # The current runners do not instrument native headers or inline JavaScript.
    # Never infer their coverage from Python or from a saved user-provided report.
    unsupported = [name for name in files if not name.endswith(".py")]
    coverage = shutil.which("coverage")
    if not coverage or not version(coverage, root):
        return "BLOCKED", "coverage tool missing or version unavailable; native and inline JS uninstrumented"
    args = [coverage, "run", "--branch", "--source=tools,integration", "-m", "unittest",
            "discover", "-s", "tests", "-p", "test_*.py"]
    result = command(args, root, 120)
    if result is None or result.returncode:
        return "BLOCKED", "Python coverage tests failed or timed out"
    output = Path(temporary) / "coverage.json"
    args = [coverage, "json", "--data-file", str(root / ".coverage"), "-o", str(output)]
    if (generated := command(args, root, 30)) is None or generated.returncode:
        return "BLOCKED", "Python coverage report generation failed"
    try:
        data = json.loads(output.read_text(encoding="utf-8"))
        report = {}
        for name, entry in data["files"].items():
            summary = entry["summary"]
            report[name] = {"lines": (summary["covered_lines"], summary["num_statements"]),
                            "branches": (summary["covered_branches"], summary["num_branches"])}
        status, detail = coverage_result(report, {name for name in files if name.endswith(".py")})
    except (OSError, KeyError, TypeError, ValueError):
        return "BLOCKED", "invalid Python coverage report"
    if unsupported:
        return "BLOCKED", f"{len(unsupported)} non-Python owned files uninstrumented: {', '.join(unsupported)}; Python: {status} ({detail})"
    return status, detail


def crap_result(text):
    # Treat any unrecognized or incomplete EDN as blocked; never infer a pass.
    if not text.startswith("{:entries [") or not text.strip().endswith("]}"):
        return "BLOCKED", "invalid CRAP report"
    scores = re.findall(r":crap\s+([^\s,}]+)", text)
    coverage = re.findall(r":coverage\s+([^\s,}]+)", text)
    if not scores or len(scores) != len(coverage):
        return "BLOCKED", "empty or incomplete CRAP report"
    try:
        values = [float(value) for value in scores]
        percentages = [float(value) for value in coverage]
        if not all(0 <= value <= 100 for value in percentages) or not all(0 <= value < float("inf") for value in values):
            raise ValueError("invalid score")
    except ValueError:
        return "BLOCKED", "missing or invalid CRAP score/coverage"
    if max(values) >= 10:
        return "FAIL", f"CRAP score {max(values)} >= 10"
    return "PASS", f"{len(values)} scores below 10"


def dryer_result(text):
    if text.strip() == "{:candidates []}":
        return "PASS", "no duplication candidates"
    if text.startswith("{:candidates [") and text.strip().endswith("]}") and re.search(r":score\s+", text):
        return "FAIL", "duplication candidates require review"
    return "BLOCKED", "invalid duplication report"


def mutation_result(result):
    if result is None or result.returncode not in (0, 3):
        return "BLOCKED", "mutation process failed or timed out"
    matches = re.findall(r"^Total\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+\S+\s*$",
                         result.stdout, re.MULTILINE)
    if len(matches) != 1:
        return "BLOCKED", "missing or invalid mutation totals"
    killed, survived, uncovered, sites = map(int, matches[0])
    if survived or uncovered:
        return "FAIL", f"{survived} survivors, {uncovered} uncovered sites"
    if result.returncode or not killed or killed != sites:
        return "BLOCKED", "no executed sites or incomplete mutation totals"
    return "PASS", f"{killed} sites executed and killed"


def check_quality_tool(name, root, files, temporary):
    executable = shutil.which(name)
    if not executable:
        return "BLOCKED", f"{name} is not installed"
    native = [file for file in files if Path(file).suffix in {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".ino"}]
    embedded = [file for file in files if file.endswith(".html")]
    php = [file for file in files if file.endswith(".php")]
    if name == "mutator":
        return "BLOCKED", (f"{len(native)} C/C++ sources/headers, {len(embedded)} inline HTML files and "
                           f"{len(php)} PHP files unsupported; mutation not run")
    # Run quality tools against an isolated snapshot, not the source tree.
    snapshot = Path(temporary) / name
    snapshot.mkdir()
    for file in inventory(root):
        target = snapshot / file
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / file, target)
    python_files = [file for file in files if file.endswith(".py")]
    try:
        interpreter = Path(executable).read_text(encoding="utf-8").splitlines()[0][2:]
        found = command([interpreter, "-c", "import importlib.metadata as m; print(m.version(" + repr(name) + "))"], root, 15)
        tool_version = found.stdout.strip() if found and found.returncode == 0 else "unavailable"
    except (OSError, IndexError, UnicodeError):
        tool_version = "unavailable"
    print(f"VERSION {name}: {tool_version} ({executable})", flush=True)
    if not python_files:
        return "BLOCKED", f"{name}: no supported Python sources"
    if name == "dryer":
        args = [executable, "--root", str(snapshot), "--threshold", "0.8", *python_files]
        result = command(args, snapshot, 60)
        report = snapshot / ".metrics/dry.edn"
        evaluate = dryer_result
    elif name == "crapper":
        coverage = shutil.which("coverage")
        if not coverage:
            return "BLOCKED", "crapper: coverage command unavailable"
        run_args = [coverage, "run", "--branch", "--source=tools,integration", "-m", "unittest",
                    "discover", "-s", "tests", "-p", "test_*.py"]
        run_result = command(run_args, snapshot, 120)
        if run_result is None or run_result.returncode:
            return "BLOCKED", "crapper: fresh Python coverage tests failed"
        lcov = snapshot / "coverage/lcov.info"
        lcov.parent.mkdir()
        lcov_result = command([coverage, "lcov", "-o", str(lcov)], snapshot, 30)
        if lcov_result is None or lcov_result.returncode or not lcov.is_file():
            return "BLOCKED", "crapper: fresh LCOV report unavailable"
        args = [executable, "--root", str(snapshot), "--threshold", "9.999",
                "--use-existing-coverage", *python_files]
        result = command(args, snapshot, 120)
        report = snapshot / ".metrics/crap.edn"
        evaluate = crap_result
    else:
        return "BLOCKED", f"unrecognized quality tool: {name}"
    if result is None or result.returncode not in (0, 2) or not report.is_file():
        return "BLOCKED", f"{name} Python analysis failed or produced no report; C/C++ and inline JS unsupported"
    status, detail = evaluate(report.read_text(encoding="utf-8"))
    if result.returncode and status == "PASS":
        status, detail = "BLOCKED", f"{name} returned {result.returncode}"
    return "BLOCKED", (f"{len(native)} C/C++ sources/headers, {len(embedded)} inline HTML files and "
                       f"{len(php)} PHP files unsupported; Python {status}: {detail}")


def run_checks(root):
    results = {}
    try:
        before = fingerprint(root)
        files = source_files(root)
        print("FINGERPRINT before", before, flush=True)
    except (OSError, ValueError) as error:
        return {name: ("BLOCKED", f"inventory unavailable: {error}") for name in CHECKS}
    with tempfile.TemporaryDirectory(prefix="esp32-release-gate-") as temporary:
        for name, action in (("semgrep", lambda: scan_semgrep(root, files, temporary)),
                             ("trivy", lambda: scan_trivy(root, temporary)),
                             ("coverage", lambda: check_coverage(root, files, temporary))):
            try:
                results[name] = action()
            except (OSError, ValueError, UnicodeError) as error:
                results[name] = ("BLOCKED", str(error))
        for name in ("crapper", "dryer", "mutator"):
            try:
                results[name] = check_quality_tool(name, root, files, temporary)
            except (OSError, ValueError, UnicodeError) as error:
                results[name] = ("BLOCKED", str(error))
    try:
        after = fingerprint(root)
        print("FINGERPRINT after ", after, flush=True)
        results["freshness"] = (("PASS", "source, tests, configuration and firmware unchanged")
                                if before == after else ("FAIL", "inputs changed during checks"))
    except (OSError, ValueError) as error:
        results["freshness"] = ("BLOCKED", str(error))
    return results


def run(root):
    try:
        before = fingerprint(root)
        results = run_checks(root)
        if fingerprint(root) != before:
            results["freshness"] = ("FAIL", "release inputs changed during checks")
    except (OSError, ValueError) as error:
        results = {name: ("BLOCKED", f"inventory unavailable: {error}") for name in CHECKS}
    for name in CHECKS:
        status, detail = results.get(name, ("BLOCKED", "missing result"))
        print(f"{name}: {status} ({detail})", flush=True)
    return all(results.get(name, ("BLOCKED",))[0] == "PASS" for name in CHECKS)


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    try:
        with tempfile.TemporaryDirectory(prefix="esp32-release-snapshot-") as temporary:
            snapshot = Path(temporary)
            copy_snapshot(root, snapshot, inventory(root))
            passed = run(snapshot)
    except (OSError, ValueError) as error:
        for name in CHECKS:
            print(f"{name}: BLOCKED (inventory unavailable: {error})", flush=True)
        passed = False
    raise SystemExit(0 if passed else 1)
