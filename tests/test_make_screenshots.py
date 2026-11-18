"""The screenshot tool: pure parts only. These tests never start Firefox or leave loopback."""
import base64
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import zlib
from unittest import mock

ROOT = Path(os.path.abspath(__file__)).parents[1]
spec = importlib.util.spec_from_file_location("make_screenshots", ROOT / "tools" / "make_screenshots.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

SIGNATURE = b"\x89PNG\r\n\x1a\n"
CSP_TAG = re.compile(r'<meta http-equiv="Content-Security-Policy"[^>]*>')


def chunk(kind, payload=b""):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))


def png_with(*kinds):
    header = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    payloads = {b"IHDR": header, b"IDAT": zlib.compress(b"\0\0\0\0"), b"tEXt": b"Comment\0synthetic"}
    return SIGNATURE + b"".join(chunk(kind, payloads.get(kind, b"\0\0\0\0")) for kind in kinds)


def frame(message):
    """A Marionette wire frame: the byte length of the JSON body, a colon, the body."""
    body = json.dumps(message).encode()
    return str(len(body)).encode() + b":" + body


def chunk_kinds(data):
    kinds, index = [], len(SIGNATURE)
    while index < len(data):
        size = struct.unpack(">I", data[index:index + 4])[0]
        kinds.append(data[index + 4:index + 8])
        index += 12 + size
    return kinds


class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.fixture = (ROOT / "tests" / "ui_fixture.html").read_text(encoding="utf-8")
        self.harness = tool.build_harness(self.fixture)

    def test_csp_meta_is_removed_and_the_test_toolbar_is_hidden(self):
        self.assertIn("Content-Security-Policy", self.fixture)
        self.assertNotIn("Content-Security-Policy", self.harness)
        self.assertIn("#synthetic-toolbar{display:none!important}", self.harness)

    def test_nothing_but_the_csp_meta_and_the_scaffold_differs_from_the_fixture(self):
        self.assertEqual(self.harness.replace(tool.SCAFFOLD, "", 1), CSP_TAG.sub("", self.fixture, count=1))

    def test_the_scaffold_hides_the_toolbar_and_selects_login_page_and_scenario_and_nothing_else(self):
        self.assertEqual(re.findall(r"<style>(.*?)</style>", tool.SCAFFOLD, re.S),
                         ["#synthetic-toolbar{display:none!important}"])
        script = re.findall(r"<script>(.*?)</script>", tool.SCAFFOLD, re.S)
        self.assertEqual(len(script), 1)
        self.assertEqual(script[0].strip().splitlines(), [
            "document.addEventListener('DOMContentLoaded',function(){",
            "var wait=setInterval(function(){",
            "if(!document.querySelector('#login-build').textContent)return;",
            "clearInterval(wait);",
            "var mode=new URLSearchParams(location.search).get('scenario');",
            "var button=document.querySelector('[data-fixture-mode=\"'+mode+'\"]');",
            "if(button)button.click();",
            "document.querySelector('#fixture-login').click();",
            "document.querySelector('#login-button').click();",
            "},50);",
            "});",
        ])
        self.assertEqual(re.sub(r"<style>.*?</style>|<script>.*?</script>", "", tool.SCAFFOLD, flags=re.S).strip(), "")
        self.assertEqual(self.harness.count(tool.SCAFFOLD + "</body>"), 1)

    @unittest.skipUnless(shutil.which("node"), "node is needed to run the scaffold script")
    def test_the_scaffold_logs_in_only_after_the_page_has_finished_its_own_start_up(self):
        """The page's boot() locks the session when /api/session answers; logging in earlier is undone."""
        script = re.findall(r"<script>(.*?)</script>", tool.SCAFFOLD, re.S)[0]
        driver = (
            "const calls=[],build={textContent:''};"
            "const elements={'[data-fixture-mode=\"high\"]':{click(){calls.push('scenario')}},"
            "'#fixture-login':{click(){calls.push('fill')}},'#login-button':{click(){calls.push('login')}},"
            "'#login-build':build};"
            "global.location={search:'?scenario=high&cb=1'};const listeners=[];"
            "global.document={addEventListener(name,handler){listeners.push([name,handler])},"
            "querySelector(selector){return elements[selector]}};"
            "let tick=null;global.setInterval=handler=>{tick=handler;return 1};global.clearInterval=()=>{tick=null};"
            + script +
            ";listeners[0][1]();tick();const before=calls.slice();"
            "build.textContent='Firmware 0.2.2';tick();"
            "console.log(JSON.stringify({event:listeners[0][0],before,after:calls,stopped:tick===null}));"
        )
        result = subprocess.run(["node", "-e", driver], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"event": "DOMContentLoaded", "before": [],
                                                      "after": ["scenario", "fill", "login"], "stopped": True})

    def test_unexpected_fixture_layout_is_rejected(self):
        for broken in (CSP_TAG.sub("", self.fixture), self.fixture + "</body>", self.fixture.replace("</body>", "")):
            with self.subTest(length=len(broken)), self.assertRaises(tool.ScreenshotError):
                tool.build_harness(broken)


class ShotTableTests(unittest.TestCase):
    def test_every_shot_names_a_real_page_and_a_real_scenario(self):
        production = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
        fixture_api = (ROOT / "tests" / "ui_fixture_api.js").read_text(encoding="utf-8")
        for file, page, scenario in tool.SHOTS:
            with self.subTest(file=file):
                self.assertIn(f'id="page-{page}"', production)
                self.assertIn(f'data-fixture-mode="{scenario}"', fixture_api)

    def test_file_names_are_unique_png_names(self):
        names = [file for file, _page, _scenario in tool.SHOTS]
        self.assertEqual(len(names), len(set(names)))
        self.assertTrue(all(re.fullmatch(r"[a-z0-9-]+\.png", name) for name in names))

    def test_readme_references_exactly_the_shots_and_all_exist(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        referenced = set(re.findall(r"docs/screenshots/([A-Za-z0-9._-]+)", readme))
        produced = {file for file, _page, _scenario in tool.SHOTS}
        self.assertEqual(referenced, produced)
        on_disk = {path.name for path in (ROOT / "docs" / "screenshots").iterdir()}
        self.assertEqual(on_disk, produced)


class PngTests(unittest.TestCase):
    def test_only_critical_chunks_are_kept_byte_for_byte(self):
        original = png_with(b"IHDR", b"tEXt", b"pHYs", b"IDAT", b"tIME", b"IEND")
        cleaned = tool.strip_png_metadata(original)
        self.assertEqual(chunk_kinds(cleaned), [b"IHDR", b"IDAT", b"IEND"])
        self.assertEqual(cleaned, png_with(b"IHDR", b"IDAT", b"IEND"))

    def test_data_that_is_not_a_png_is_rejected(self):
        with self.assertRaises(tool.ScreenshotError):
            tool.strip_png_metadata(b"GIF89a")


class CommittedImageTests(unittest.TestCase):
    def test_committed_screenshots_are_clean_pngs_of_the_documented_width(self):
        for file, _page, _scenario in tool.SHOTS:
            with self.subTest(file=file):
                data = (ROOT / "docs" / "screenshots" / file).read_bytes()
                self.assertTrue(data.startswith(SIGNATURE))
                self.assertEqual(struct.unpack(">I", data[16:20])[0], 1540)
                self.assertEqual(chunk_kinds(data), [b"IHDR"] + [b"IDAT"] * (len(chunk_kinds(data)) - 2) + [b"IEND"])


class FirefoxLookupTests(unittest.TestCase):
    def test_missing_explicit_path_is_a_clear_error_without_fallback(self):
        with mock.patch.object(tool.shutil, "which", return_value="/bin/sh"):
            with self.assertRaisesRegex(tool.ScreenshotError, "Firefox not found at /nonexistent/firefox"):
                tool.find_firefox("/nonexistent/firefox")

    def test_explicit_path_wins(self):
        with tempfile.NamedTemporaryFile() as fake:
            self.assertEqual(tool.find_firefox(fake.name), Path(fake.name))

    def test_default_lookup_uses_path(self):
        with tempfile.NamedTemporaryFile() as fake, mock.patch.object(tool.shutil, "which", return_value=fake.name):
            self.assertEqual(tool.find_firefox(None), Path(fake.name))

    def test_no_firefox_anywhere_is_a_clear_error(self):
        with mock.patch.object(tool.shutil, "which", return_value=None), \
                mock.patch.object(tool.Path, "is_file", return_value=False):
            with self.assertRaisesRegex(tool.ScreenshotError, "Install Firefox"):
                tool.find_firefox(None)

    def test_main_exits_non_zero_and_writes_nothing_when_firefox_is_missing(self):
        with tempfile.TemporaryDirectory() as out, contextlib.redirect_stderr(io.StringIO()) as errors:
            code = tool.main(["--firefox", "/nonexistent/firefox", "--out", str(Path(out) / "shots")])
            self.assertFalse((Path(out) / "shots").exists())
        self.assertEqual(code, 1)
        self.assertIn("Firefox not found", errors.getvalue())


class FreePortTests(unittest.TestCase):
    def test_free_port_returns_an_open_tcp_port_number(self):
        port = tool.free_port()
        self.assertIsInstance(port, int)
        self.assertTrue(0 < port < 65536)


class MainTests(unittest.TestCase):
    def test_images_are_written_into_a_new_directory_and_reruns_overwrite_them(self):
        images = {file: png_with(b"IHDR", b"IDAT", b"IEND") for file, _page, _scenario in tool.SHOTS}
        with tempfile.TemporaryDirectory() as parent, mock.patch.object(tool, "find_firefox", return_value=Path("firefox")), \
                mock.patch.object(tool, "render_all", return_value=images), contextlib.redirect_stdout(io.StringIO()) as shown:
            out = Path(parent) / "nested" / "shots"
            self.assertEqual(tool.main(["--out", str(out)]), 0)
            self.assertEqual(tool.main(["--out", str(out)]), 0)
            self.assertEqual({path.name for path in out.iterdir()}, set(images))
            self.assertEqual((out / "overview.png").read_bytes(), images["overview.png"])
        self.assertIn(f"{out / 'overview.png'}: {len(images['overview.png'])} bytes", shown.getvalue())

    def test_help_describes_the_tool(self):
        with contextlib.redirect_stdout(io.StringIO()) as shown, self.assertRaises(SystemExit) as stop:
            tool.main(["--help"])
        self.assertEqual(stop.exception.code, 0)
        self.assertIn("Render the README screenshots", shown.getvalue())


class CleanupTests(unittest.TestCase):
    @staticmethod
    def get(address, path):
        with socket.create_connection(address, timeout=5) as client:
            client.sendall(f"GET {path} HTTP/1.0\r\n\r\n".encode())
            return client.makefile("rb").read()

    def test_server_is_stopped_when_the_browser_fails(self):
        addresses, real_serve = [], tool.serve

        @contextlib.contextmanager
        def spy(directory):
            with real_serve(directory) as address:
                addresses.append(address)
                yield address

        @contextlib.contextmanager
        def failing(firefox, work):
            raise tool.ScreenshotError("browser failed")
            yield

        with mock.patch.object(tool, "serve", spy), mock.patch.object(tool, "headless_firefox", failing):
            with self.assertRaisesRegex(tool.ScreenshotError, "browser failed"):
                tool.render_all(Path("firefox"))
        with self.assertRaises(OSError):
            socket.create_connection(addresses[0], timeout=1)

    def test_server_thread_is_daemonized(self):
        real_thread = threading.Thread
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(tool.threading, "Thread", side_effect=real_thread) as create:
            with tool.serve(Path(directory)):
                self.assertIs(create.call_args.kwargs["daemon"], True)

    def test_server_listens_on_loopback_only(self):
        with tempfile.TemporaryDirectory() as directory, tool.serve(Path(directory)) as (host, port):
            self.assertEqual(host, "127.0.0.1")
            self.assertTrue(0 < port < 65536)

    def test_server_serves_the_harness_directory(self):
        with tempfile.TemporaryDirectory() as directory, tool.serve(Path(directory)) as address:
            (Path(directory) / "harness.html").write_text("synthetic")
            reply = self.get(address, "/harness.html")
        self.assertTrue(reply.endswith(b"synthetic"))

    def test_the_throwaway_profile_and_everything_else_in_the_work_directory_are_not_served(self):
        with tempfile.TemporaryDirectory() as name:
            work = Path(name)
            (work / "profile").mkdir()
            (work / "profile" / "user.js").write_text("secret-pref")
            (work / "outside.txt").write_text("secret-outside")
            site = tool.write_harness(work)
            with tool.serve(site) as address:
                replies = [self.get(address, path) for path in
                           ("/profile/user.js", "/../profile/user.js", "/%2e%2e/outside.txt", "/outside.txt")]
                served = self.get(address, "/harness.html")
        self.assertIn(b"synthetic-toolbar", served)
        for reply in replies:
            self.assertNotIn(b"secret", reply)
            self.assertIn(b" 404 ", reply.split(b"\r\n", 1)[0])

    def test_render_all_serves_only_the_harness_not_the_profile_or_work_directory(self):
        addresses, replies, real_serve = [], [], tool.serve

        @contextlib.contextmanager
        def spy(directory):
            with real_serve(directory) as address:
                addresses.append(address)
                yield address

        @contextlib.contextmanager
        def fake_firefox(firefox, work):
            (work / "profile").mkdir()
            (work / "profile" / "user.js").write_text("secret-pref")
            (work / "outside.txt").write_text("secret-outside")
            replies.extend(self.get(addresses[0], path) for path in
                           ("/harness.html", "/profile/user.js", "/outside.txt"))
            yield object()

        with mock.patch.object(tool, "serve", spy), mock.patch.object(tool, "headless_firefox", fake_firefox), \
                mock.patch.object(tool, "capture", return_value=b"image"):
            tool.render_all(Path("firefox"))
        self.assertIn(b"synthetic-toolbar", replies[0])
        for reply in replies[1:]:
            self.assertNotIn(b"secret", reply)
            self.assertIn(b" 404 ", reply.split(b"\r\n", 1)[0])

    def test_browser_is_killed_when_it_ignores_the_quit_request(self):
        process = mock.MagicMock()
        process.wait.side_effect = [subprocess.TimeoutExpired("firefox", 15), 0]
        with tempfile.TemporaryDirectory() as work, mock.patch.object(tool.subprocess, "Popen", return_value=process), \
                mock.patch.object(tool.Marionette, "connect", side_effect=tool.ScreenshotError("no marionette")):
            with self.assertRaises(tool.ScreenshotError):
                with tool.headless_firefox(Path("firefox"), Path(work)):
                    pass
            profile = (Path(work) / "profile" / "user.js").read_text()
        process.kill.assert_called_once()
        self.assertIn('user_pref("layout.css.devPixelsPerPx", "2.0");', profile)


class ProfileTests(unittest.TestCase):
    """The throwaway profile: what Firefox is told before it starts."""

    @staticmethod
    def written_prefs():
        process = mock.MagicMock()
        process.wait.return_value = 0
        with tempfile.TemporaryDirectory() as work, mock.patch.object(tool.subprocess, "Popen", return_value=process), \
                mock.patch.object(tool.Marionette, "connect", side_effect=tool.ScreenshotError("stop")):
            with contextlib.suppress(tool.ScreenshotError), tool.headless_firefox(Path("firefox"), Path(work)):
                pass
            text = (Path(work) / "profile" / "user.js").read_text()
        return {key: json.loads(value) for key, value in re.findall(r'user_pref\("([^"]+)", (.+)\);', text)}

    def test_everything_but_loopback_goes_to_a_proxy_nothing_listens_on(self):
        prefs = self.written_prefs()
        self.assertEqual(prefs["network.proxy.type"], 1)
        self.assertTrue(prefs["network.proxy.share_proxy_settings"])
        dead = {prefs[f"network.proxy.{kind}_port"] for kind in ("http", "ssl", "socks")}
        self.assertEqual(len(dead), 1)
        for kind in ("http", "ssl", "socks"):
            self.assertEqual(prefs[f"network.proxy.{kind}"], "127.0.0.1")
        port = dead.pop()
        self.assertNotEqual(port, prefs["marionette.port"])
        with self.assertRaises(OSError):
            socket.create_connection(("127.0.0.1", port), timeout=1)
        self.assertEqual(prefs["network.proxy.no_proxies_on"], "127.0.0.1,localhost,::1")

    def test_scrollbars_overlay_the_page_and_the_browser_stays_quiet(self):
        prefs = self.written_prefs()
        self.assertEqual(prefs["ui.useOverlayScrollbars"], 1)
        self.assertEqual(prefs["layout.css.devPixelsPerPx"], "2.0")
        for quiet in ("app.update.auto", "app.update.enabled", "datareporting.policy.dataSubmissionEnabled",
                      "toolkit.telemetry.enabled", "network.captive-portal-service.enabled",
                      "network.connectivity-service.enabled", "network.prefetch-next",
                      "browser.safebrowsing.malware.enabled", "browser.safebrowsing.phishing.enabled",
                      "browser.safebrowsing.downloads.enabled", "extensions.update.enabled",
                      "extensions.systemAddon.update.enabled", "dom.push.enabled", "app.normandy.enabled",
                      "app.shield.optoutstudies.enabled", "media.gmp-manager.updateEnabled",
                      "media.gmp-gmpopenh264.enabled", "browser.newtabpage.enabled",
                      "browser.newtabpage.activity-stream.feeds.system.topstories",
                      "browser.newtabpage.activity-stream.feeds.telemetry",
                      "browser.newtabpage.activity-stream.telemetry"):
            self.assertIs(prefs[quiet], False, quiet)
        self.assertEqual(prefs["network.trr.mode"], 5)
        self.assertIs(prefs["network.dns.disablePrefetch"], True)
        self.assertEqual(prefs["services.settings.server"], "http://127.0.0.1:1")
        self.assertEqual(prefs["browser.region.network.url"], "")


class CommandLineTests(unittest.TestCase):
    """The Firefox process is launched with the throwaway profile and the headless flags."""

    @staticmethod
    def launched_args():
        captured = {}

        def popen(command, *args, **kwargs):
            captured["command"] = command
            process = mock.MagicMock()
            process.wait.return_value = 0
            return process

        with tempfile.TemporaryDirectory() as work, mock.patch.object(tool.subprocess, "Popen", side_effect=popen), \
                mock.patch.object(tool.Marionette, "connect", side_effect=tool.ScreenshotError("stop")):
            with contextlib.suppress(tool.ScreenshotError), tool.headless_firefox(Path("firefox"), Path(work)):
                pass
        return captured["command"], Path(work) / "profile"

    def test_the_process_uses_the_throwaway_profile_and_headless_flags(self):
        command, profile = self.launched_args()
        self.assertEqual(command[0], str(Path("firefox")))
        self.assertIn("--headless", command)
        self.assertIn("--no-remote", command)
        self.assertIn("--marionette", command)
        self.assertIn("--profile", command)
        self.assertEqual(command[command.index("--profile") + 1], str(profile))


class MarionetteTests(unittest.TestCase):
    def serve_replies(self, replies, split_replies=False):
        near, far = socket.socketpair()
        self.sent = []

        def server():
            far.sendall(frame({"applicationType": "gecko", "marionetteProtocol": 3}))
            for reply in replies:
                request = far.recv(65536)
                self.sent.append(json.loads(request.partition(b":")[2]))
                framed = frame(reply)
                if split_replies:
                    far.sendall(framed[:12])
                    time.sleep(0.2)
                    far.sendall(framed[12:])
                else:
                    far.sendall(framed)
            far.close()

        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 5)
        return tool.Marionette(near)

    def test_every_command_has_its_own_increasing_id_and_the_given_parameters(self):
        client = self.serve_replies([[1, 1, None, {}], [1, 2, None, {}]])
        self.addCleanup(client.sock.close)
        client.command("WebDriver:Navigate", {"url": "http://127.0.0.1/"})
        client.command("WebDriver:NewSession")
        self.assertEqual(self.sent, [[0, 1, "WebDriver:Navigate", {"url": "http://127.0.0.1/"}],
                                     [0, 2, "WebDriver:NewSession", {}]])

    def test_a_reply_split_across_reads_is_reassembled(self):
        client = self.serve_replies([[1, 1, None, {"value": "x" * 200}]], split_replies=True)
        self.addCleanup(client.sock.close)
        self.assertEqual(client.run("return 'x'"), "x" * 200)

    def test_script_result_is_returned(self):
        client = self.serve_replies([[1, 1, None, {"value": 1012}]])
        self.addCleanup(client.sock.close)
        self.assertEqual(client.run("return 1012"), 1012)

    def test_remote_error_becomes_a_screenshot_error(self):
        client = self.serve_replies([[1, 1, {"error": "timeout"}, None]])
        self.addCleanup(client.sock.close)
        with self.assertRaisesRegex(tool.ScreenshotError, "WebDriver:Navigate failed"):
            client.command("WebDriver:Navigate", {"url": "http://127.0.0.1/"})

    def test_connection_closed_before_a_reply_is_a_screenshot_error(self):
        client = self.serve_replies([])
        self.addCleanup(client.sock.close)
        with self.assertRaisesRegex(tool.ScreenshotError, "connection closed"):
            client.receive()

    def test_a_browser_that_stops_answering_is_a_screenshot_error(self):
        near, far = socket.socketpair()
        self.addCleanup(far.close)
        self.addCleanup(near.close)
        far.sendall(frame({"applicationType": "gecko"}))
        client = tool.Marionette(near)
        near.settimeout(0.05)
        with self.assertRaisesRegex(tool.ScreenshotError, "Marionette connection failed"):
            client.command("WebDriver:Navigate", {"url": "http://127.0.0.1/"})

    def test_a_command_that_cannot_be_sent_is_a_screenshot_error(self):
        client = tool.Marionette.__new__(tool.Marionette)
        client.sock, client.buffer, client.counter = mock.MagicMock(), b"", 0
        client.sock.sendall.side_effect = BrokenPipeError("gone")
        with self.assertRaisesRegex(tool.ScreenshotError, "Marionette connection failed"):
            client.command("WebDriver:Navigate")

    def test_main_reports_a_browser_crash_without_a_traceback(self):
        near, far = socket.socketpair()
        far.sendall(frame({"applicationType": "gecko"}))
        far.close()
        client = tool.Marionette(near)
        self.addCleanup(near.close)
        with mock.patch.object(tool, "find_firefox", return_value=Path("firefox")), \
                mock.patch.object(tool, "render_all", side_effect=lambda firefox: client.command("WebDriver:Navigate")), \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            code = tool.main(["--out", "unused"])
        self.assertEqual(code, 1)
        self.assertIn("make_screenshots: ", errors.getvalue())
        self.assertFalse(Path("unused").exists())


class ConnectTests(unittest.TestCase):
    """Marionette starts a moment after the browser: connection is retried until a deadline."""

    @staticmethod
    def greeted_socket():
        near, far = socket.socketpair()
        far.sendall(frame({"applicationType": "gecko"}))
        return near, far

    def test_connection_is_retried_until_the_browser_listens(self):
        near, far = self.greeted_socket()
        self.addCleanup(near.close)
        self.addCleanup(far.close)
        attempts = [OSError("refused"), OSError("refused"), near]

        def connect(address, timeout):
            result = attempts.pop(0)
            if isinstance(result, OSError):
                raise result
            return result

        with mock.patch.object(tool.socket, "create_connection", connect), mock.patch.object(tool.time, "sleep") as sleep:
            client = tool.Marionette.connect(1234)
        self.assertIs(client.sock, near)
        self.assertEqual(sleep.call_count, 2)

    def test_a_listener_that_closes_before_the_greeting_is_retried(self):
        closed, far_closed = socket.socketpair()
        far_closed.close()
        near, far = self.greeted_socket()
        self.addCleanup(near.close)
        self.addCleanup(far.close)
        sockets = [closed, near]
        with mock.patch.object(tool.socket, "create_connection", lambda address, timeout: sockets.pop(0)), \
                mock.patch.object(tool.time, "sleep"):
            client = tool.Marionette.connect(1234)
        self.assertIs(client.sock, near)

    def test_a_listener_sending_a_malformed_greeting_is_retried(self):
        bad, bad_peer = socket.socketpair()
        bad_peer.sendall(b"x:{}")
        near, far = self.greeted_socket()
        self.addCleanup(bad_peer.close)
        self.addCleanup(near.close)
        self.addCleanup(far.close)
        sockets = [bad, near]
        with mock.patch.object(tool.socket, "create_connection", lambda address, timeout: sockets.pop(0)), \
                mock.patch.object(tool.time, "sleep"):
            client = tool.Marionette.connect(1234)
        self.assertIs(client.sock, near)
        self.assertEqual(bad.fileno(), -1)

    def test_giving_up_after_the_deadline_is_a_clear_error(self):
        clock = iter([0.0, 1.0, 100.0])
        with mock.patch.object(tool.socket, "create_connection", side_effect=OSError("refused")), \
                mock.patch.object(tool.time, "monotonic", lambda: next(clock)), mock.patch.object(tool.time, "sleep"):
            with self.assertRaisesRegex(tool.ScreenshotError, "Marionette in time"):
                tool.Marionette.connect(1234, timeout=60)

    def test_a_connection_is_still_retried_at_the_exact_deadline(self):
        near, far = self.greeted_socket()
        self.addCleanup(near.close)
        self.addCleanup(far.close)
        attempts = [OSError("refused"), near]
        clock = iter([0.0, 60.0])

        def connect(address, timeout):
            result = attempts.pop(0)
            if isinstance(result, OSError):
                raise result
            return result

        with mock.patch.object(tool.socket, "create_connection", connect), \
                mock.patch.object(tool.time, "monotonic", lambda: next(clock)), mock.patch.object(tool.time, "sleep"):
            client = tool.Marionette.connect(1234, timeout=60)
        self.assertIs(client.sock, near)

    def test_close_asks_firefox_to_quit_and_still_closes_the_socket_on_failure(self):
        near, far = socket.socketpair()
        self.addCleanup(far.close)
        client = tool.Marionette.__new__(tool.Marionette)
        client.sock, client.buffer, client.counter = near, b"", 0
        far.close()  # the quit request now fails; close() must not raise
        client.close()
        with self.assertRaises(OSError):
            near.send(b"x")


class WaitForTests(unittest.TestCase):
    def test_polls_until_the_script_is_truthy(self):
        browser = mock.MagicMock()
        browser.run.side_effect = [False, False, True]
        with mock.patch.object(tool.time, "sleep") as sleep:
            tool.wait_for(browser, "return ready")
        self.assertEqual(browser.run.call_count, 3)
        self.assertEqual(sleep.call_count, 2)

    def test_a_page_can_finish_at_the_exact_deadline(self):
        browser = mock.MagicMock()
        browser.run.side_effect = [False, True]
        clock = iter([0.0, 40.0])
        with mock.patch.object(tool.time, "monotonic", lambda: next(clock)), mock.patch.object(tool.time, "sleep"):
            tool.wait_for(browser, "return ready", timeout=40)
        self.assertEqual(browser.run.call_count, 2)

    def test_a_page_that_never_renders_is_a_clear_error(self):
        browser = mock.MagicMock()
        browser.run.return_value = False
        clock = iter([0.0, 10.0, 50.0])
        with mock.patch.object(tool.time, "monotonic", lambda: next(clock)), mock.patch.object(tool.time, "sleep"):
            with self.assertRaisesRegex(tool.ScreenshotError, "did not finish rendering"):
                tool.wait_for(browser, "return ready", timeout=40)


class FakeBrowser:
    """A browser whose inner height is the window height minus a fixed 100 px of chrome.

    It starts 683 px wide, like the real window, and models a scrollbar gutter, the page's
    constant vertical overflow and a content height that depends on the width.
    """

    CHROME = 100

    def chrome(self, window):
        return self.CHROME

    def __init__(self, bottom=lambda width: 1000, gutter=0, overflow=None):
        self.window, self.width, self.calls = 600, 683, []
        self.bottom, self.gutter = bottom, gutter
        self.overflow = tool.BODY_OVERFLOW if overflow is None else overflow

    def command(self, name, parameters=None):
        self.calls.append((name, parameters))
        if name == "WebDriver:SetWindowRect":
            self.window, self.width = parameters["height"], parameters["width"]
        if name == "WebDriver:TakeScreenshot":
            self.viewport = self.window - self.chrome(self.window)
            return {"value": base64.b64encode(png_with(b"IHDR", b"tEXt", b"IDAT", b"IEND")).decode()}
        return {}

    def run(self, source):
        inner = self.window - self.chrome(self.window)
        if source == tool.LAYOUT:
            return {"width": self.width, "client": self.width - self.gutter, "height": inner,
                    "scroll": inner + self.overflow, "bottom": self.bottom(self.width)}
        if "innerHeight" in source:
            return inner
        if "innerWidth" in source:
            return self.width
        return True

    def screenshots(self):
        return [call for call in self.calls if call[0] == "WebDriver:TakeScreenshot"]


class CaptureTests(unittest.TestCase):
    URL = "http://127.0.0.1:1/harness.html?scenario=normal&cb=0#home"

    def capture(self, browser):
        with mock.patch.object(tool.time, "sleep"):
            return tool.capture(browser, self.URL, "home")

    def test_viewport_matches_the_content_height_and_metadata_is_stripped(self):
        browser = FakeBrowser(bottom=lambda width: 1000)
        data = self.capture(browser)
        self.assertEqual(browser.viewport, 1000 + tool.BOTTOM_MARGIN)
        self.assertEqual(chunk_kinds(data), [b"IHDR", b"IDAT", b"IEND"])
        self.assertIn(("WebDriver:Navigate", {"url": self.URL}), browser.calls)

    def test_the_content_height_is_measured_after_the_width_is_final(self):
        browser = FakeBrowser(bottom=lambda width: 1055 if width == tool.WIDTH else 1052)
        self.capture(browser)
        self.assertEqual(browser.viewport, 1055 + tool.BOTTOM_MARGIN)

    def test_a_content_height_that_changes_after_the_resize_is_corrected_once(self):
        heights = iter([1000, 1005])
        last = [1005]

        def bottom(width):
            last[0] = next(heights, last[0])
            return last[0]

        browser = FakeBrowser(bottom=bottom)
        self.capture(browser)
        self.assertEqual(browser.viewport, 1005 + tool.BOTTOM_MARGIN)
        self.assertEqual(len(browser.screenshots()), 1)

    def test_a_scrollbar_gutter_is_an_error_naming_the_page_and_no_image_is_taken(self):
        browser = FakeBrowser(gutter=15)
        with self.assertRaisesRegex(tool.ScreenshotError, r"home.*client.*755.*width.*770"):
            self.capture(browser)
        self.assertEqual(browser.screenshots(), [])

    def test_vertical_overflow_beyond_the_known_body_margins_is_an_error_and_no_image_is_taken(self):
        browser = FakeBrowser(overflow=7)
        with self.assertRaisesRegex(tool.ScreenshotError, r"home.*scroll"):
            self.capture(browser)
        self.assertEqual(browser.screenshots(), [])

    def test_the_known_body_margin_overflow_is_accepted(self):
        browser = FakeBrowser(overflow=6)
        self.capture(browser)
        self.assertEqual(len(browser.screenshots()), 1)

    def test_main_content_outside_the_viewport_is_an_error_and_no_image_is_taken(self):
        class Clipped(FakeBrowser):
            def run(self, source):
                layout = super().run(source)
                if source == tool.LAYOUT:
                    layout["bottom"] = layout["height"] + 1
                return layout

        browser = Clipped()
        with self.assertRaisesRegex(tool.ScreenshotError, r"home.*bottom"):
            self.capture(browser)
        self.assertEqual(browser.screenshots(), [])

    def test_main_content_ending_before_the_viewport_bottom_is_an_error_and_no_image_is_taken(self):
        class TooShort(FakeBrowser):
            def run(self, source):
                layout = super().run(source)
                if source == tool.LAYOUT:
                    layout["bottom"] = layout["height"] - 100
                return layout

        browser = TooShort()
        with self.assertRaisesRegex(tool.ScreenshotError, r"home.*bottom"):
            self.capture(browser)
        self.assertEqual(browser.screenshots(), [])

    def test_content_that_keeps_growing_is_an_error_and_no_image_is_taken(self):
        counter = iter(range(1000, 2000, 50))
        browser = FakeBrowser(bottom=lambda width: next(counter))
        with self.assertRaisesRegex(tool.ScreenshotError, r"home.*bottom"):
            self.capture(browser)
        self.assertEqual(browser.screenshots(), [])

    def test_the_screenshot_is_the_viewport_only_so_the_fixed_background_does_not_tile(self):
        browser = FakeBrowser()
        self.capture(browser)
        self.assertIn(("WebDriver:TakeScreenshot", {"full": False, "hash": False, "scroll": False}), browser.calls)

    def test_a_constant_window_chrome_needs_one_resize_after_the_probe(self):
        browser = FakeBrowser()
        with mock.patch.object(tool.time, "sleep"):
            tool.fit_viewport(browser, 1000)
        resizes = [call for call in browser.calls if call[0] == "WebDriver:SetWindowRect"]
        self.assertEqual(resizes, [("WebDriver:SetWindowRect", {"width": tool.WIDTH, "height": 400}),
                                   ("WebDriver:SetWindowRect", {"width": tool.WIDTH, "height": 1100})])

    def test_a_window_whose_chrome_changes_with_its_size_is_corrected(self):
        class Shifty(FakeBrowser):
            def chrome(self, window):
                return 80 if window <= 400 else 100

        browser = Shifty()
        with mock.patch.object(tool.time, "sleep"):
            tool.fit_viewport(browser, 1000)
        self.assertEqual(browser.window - browser.chrome(browser.window), 1000)
        self.assertEqual(browser.calls[-1], ("WebDriver:SetWindowRect", {"width": tool.WIDTH, "height": 1100}))

    def test_a_viewport_that_cannot_be_sized_is_an_error(self):
        browser = FakeBrowser()
        browser.run = lambda source: 5 if "innerHeight" in source else tool.WIDTH
        with mock.patch.object(tool.time, "sleep"), self.assertRaisesRegex(tool.ScreenshotError, "viewport"):
            tool.fit_viewport(browser, 1000)

    def test_page_url_names_the_page_and_scenario_and_is_unique_per_shot(self):
        base = ("127.0.0.1", 8080)
        self.assertEqual(tool.page_url(base, "alarms", "high", 7),
                         "http://127.0.0.1:8080/harness.html?scenario=high&cb=7#alarms")
        self.assertIn("#page-alarms", tool.ready_script("alarms"))
        self.assertIn("indexOf('Loading')<0", tool.ready_script("alarms"))
        # Each shot must reload a fresh document: the part before the fragment differs, so the
        # fixture re-seeds its clock (started=Date.now()) and state instead of carrying over.
        documents = [tool.page_url(base, page, scenario, index).split("#", 1)[0]
                     for index, (_file, page, scenario) in enumerate(tool.SHOTS)]
        self.assertEqual(len(set(documents)), len(tool.SHOTS))
        self.assertEqual(documents[3:], ["http://127.0.0.1:8080/harness.html?scenario=normal&cb=3",
                                         "http://127.0.0.1:8080/harness.html?scenario=normal&cb=4",
                                         "http://127.0.0.1:8080/harness.html?scenario=normal&cb=5"])

    def test_render_all_loads_every_shot_from_its_own_document(self):
        urls = []

        class Recorder:
            def command(self, name, parameters=None):
                if name == "WebDriver:Navigate":
                    urls.append(parameters["url"])
                return {}

        @contextlib.contextmanager
        def fake_firefox(firefox, work):
            yield Recorder()

        with mock.patch.object(tool, "headless_firefox", fake_firefox), \
                mock.patch.object(tool, "capture", lambda browser, url, page: browser.command(
                    "WebDriver:Navigate", {"url": url}) and b""):
            tool.render_all(Path("firefox"))
        self.assertEqual(len({url.split("#", 1)[0] for url in urls}), len(tool.SHOTS))


if __name__ == "__main__":
    unittest.main()
