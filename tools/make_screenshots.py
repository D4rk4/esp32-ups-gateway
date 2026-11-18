#!/usr/bin/env python3
"""Render the README screenshots from the UI test fixture (synthetic data only).

    python3 tools/make_screenshots.py [--firefox PATH] [--out DIR]

Firefox is required. The harness is built from tests/ui_fixture.html in a
temporary directory: the CSP meta tag is removed, the synthetic test toolbar is
hidden, and the page logs in and selects the scenario itself. It is served on
127.0.0.1 only and driven over Marionette by headless Firefox. Product markup,
styles and text are not changed. Nothing is written to the repository except
the PNG files in --out (default docs/screenshots/).

The images illustrate the interface; they are not a hardware verification.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import functools
import http.server
import json
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests" / "ui_fixture.html"
DEFAULT_OUT = ROOT / "docs" / "screenshots"

# The single source of the shot list: file name, page (URL fragment), scenario.
# A scenario is the fixture test-button mode: "normal" or "high" (high load).
SHOTS = (
    ("overview.png", "home", "normal"),
    ("alarm-status-high-load.png", "alarms", "high"),
    ("device-status.png", "status", "normal"),
    ("event-log.png", "events", "normal"),
    ("history-graph.png", "data", "normal"),
    ("firmware-update.png", "firmware", "normal"),
)

# One rule for every image: 770 CSS px wide at device pixel ratio 2, and a viewport
# exactly as tall as the content (the page background is fixed, so a shorter
# viewport with a full-page capture would tile the gradient).
WIDTH = 770
SCALE = "2.0"
BOTTOM_MARGIN = 12  # CSS px below the main block
PROBE_HEIGHT = 600  # provisional viewport height: only used to get the final width before measuring
# body margin:3px plus min-height:100vh makes scrollHeight exactly 6 px taller than the viewport.
BODY_OVERFLOW = 6
# Everything the size checks need, read in one go: the viewport and the content-box widths, the
# viewport and document heights, and where the main block ends.
LAYOUT = (
    "var d=document.documentElement;"
    "return {width:innerWidth,client:d.clientWidth,height:innerHeight,scroll:d.scrollHeight,"
    "bottom:Math.ceil(document.querySelector('#main').getBoundingClientRect().bottom+scrollY)};"
)

# Route proxied traffic through an unused loopback port and exempt the local harness.
# Quiet Firefox background services as well; the proxy is not an OS-level network sandbox.
PREFS = {
    "network.proxy.type": 1,
    "network.proxy.share_proxy_settings": True,
    "network.proxy.http": "127.0.0.1",
    "network.proxy.ssl": "127.0.0.1",
    "network.proxy.socks": "127.0.0.1",
    "network.proxy.no_proxies_on": "127.0.0.1,localhost,::1",
    "layout.css.devPixelsPerPx": SCALE,
    "app.update.auto": False,
    "app.update.enabled": False,
    "datareporting.policy.dataSubmissionEnabled": False,
    "toolkit.telemetry.enabled": False,
    "network.captive-portal-service.enabled": False,
    "network.connectivity-service.enabled": False,
    "network.trr.mode": 5,
    "network.dns.disablePrefetch": True,
    "network.prefetch-next": False,
    "browser.safebrowsing.malware.enabled": False,
    "browser.safebrowsing.phishing.enabled": False,
    "browser.safebrowsing.downloads.enabled": False,
    "extensions.update.enabled": False,
    "extensions.systemAddon.update.enabled": False,
    "services.settings.server": "http://127.0.0.1:1",
    "dom.push.enabled": False,
    "app.normandy.enabled": False,
    "app.shield.optoutstudies.enabled": False,
    "browser.region.network.url": "",
    "media.gmp-manager.updateEnabled": False,
    "media.gmp-gmpopenh264.enabled": False,
    "browser.newtabpage.enabled": False,
    "browser.newtabpage.activity-stream.feeds.system.topstories": False,
    "browser.newtabpage.activity-stream.feeds.telemetry": False,
    "browser.newtabpage.activity-stream.telemetry": False,
    # Overlay scrollbars take no layout width, whatever the machine's own scrollbar setting is.
    "ui.useOverlayScrollbars": 1,
}

CSP_META = '<meta http-equiv="Content-Security-Policy"'
SCAFFOLD = (
    "<style>#synthetic-toolbar{display:none!important}</style>\n"
    # The page runs its own boot() on load: it calls /api/session and locks the session when that
    # answers, so the scaffold must wait for that start-up before it logs in, or the login is undone.
    "<script>document.addEventListener('DOMContentLoaded',function(){\n"
    "var wait=setInterval(function(){\n"
    "if(!document.querySelector('#login-build').textContent)return;\n"
    "clearInterval(wait);\n"
    "var mode=new URLSearchParams(location.search).get('scenario');\n"
    "var button=document.querySelector('[data-fixture-mode=\"'+mode+'\"]');\n"
    "if(button)button.click();\n"
    "document.querySelector('#fixture-login').click();\n"
    "document.querySelector('#login-button').click();\n"
    "},50);\n"
    "});</script>\n"
)


class ScreenshotError(RuntimeError):
    """The screenshots cannot be produced; the message says why."""


def build_harness(fixture: str) -> str:
    """The fixture with its CSP meta removed and the scaffold appended before </body>."""
    if fixture.count(CSP_META) != 1 or fixture.count("</body>") != 1:
        raise ScreenshotError("Unexpected fixture layout: expected one CSP meta tag and one </body>")
    start = fixture.index(CSP_META)
    end = fixture.index(">", start) + 1
    page = fixture[:start] + fixture[end:]
    return page.replace("</body>", SCAFFOLD + "</body>", 1)


def page_url(address, page: str, scenario: str, cache_bust: int) -> str:
    host, port = address
    # A per-shot cache-buster forces a fresh document load: the fixture's clock and state
    # are seeded at page-load time, so a hash-only change would reuse the same document.
    return f"http://{host}:{port}/harness.html?scenario={scenario}&cb={cache_bust}#{page}"


def ready_script(page: str) -> str:
    """True once the page is shown, filled in and no loading placeholder remains."""
    return (
        f"var p=document.querySelector('#page-{page}');"
        "return !!p&&!p.hidden&&p.innerText.length>100&&document.body.innerText.indexOf('Loading')<0;"
    )


def strip_png_metadata(data: bytes) -> bytes:
    """Keep only the IHDR, IDAT and IEND chunks of a PNG."""
    signature = b"\x89PNG\r\n\x1a\n"
    if not data.startswith(signature):
        raise ScreenshotError("Firefox returned data that is not a PNG")
    kept, index = [signature], len(signature)
    while index < len(data):
        size = struct.unpack(">I", data[index:index + 4])[0]
        chunk = data[index:index + 12 + size]
        if chunk[4:8] in (b"IHDR", b"IDAT", b"IEND"):
            kept.append(chunk)
        index += 12 + size
    return b"".join(kept)


def find_firefox(requested: str | None) -> Path:
    """The Firefox executable: the requested path, else PATH, else the macOS app."""
    if requested:
        candidates = [requested]
    else:
        candidates = [shutil.which("firefox"), "/Applications/Firefox.app/Contents/MacOS/firefox"]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    raise ScreenshotError(
        "Firefox not found" + (f" at {requested}" if requested else "")
        + ". Install Firefox or pass --firefox PATH."
    )


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


@contextlib.contextmanager
def serve(directory: Path):
    """Serve a directory on 127.0.0.1 and stop the server on exit."""
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *arguments):
            pass
    handler = functools.partial(Quiet, directory=str(directory))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        yield server.server_address
    finally:
        server.shutdown()
        server.server_close()


class Marionette:
    """A minimal Marionette client; messages are length-prefixed JSON over loopback."""

    def __init__(self, sock: socket.socket) -> None:
        self.sock, self.buffer, self.counter = sock, b"", 0
        self.receive()  # the server greeting

    @classmethod
    def connect(cls, port: int, timeout: float = 60.0) -> "Marionette":
        deadline = time.monotonic() + timeout
        while True:
            sock = None
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=60)
                return cls(sock)
            except (OSError, ValueError, ScreenshotError):
                if sock:
                    sock.close()
                if time.monotonic() > deadline:
                    raise ScreenshotError("Firefox did not start Marionette in time") from None
                time.sleep(0.5)

    def receive(self) -> list:
        while b":" not in self.buffer:
            self.buffer += self._read(65536)
        length, _, rest = self.buffer.partition(b":")
        while len(rest) < int(length):
            rest += self._read(1 << 20)
        self.buffer = rest[int(length):]
        return json.loads(rest[:int(length)])

    def _read(self, size: int) -> bytes:
        try:
            data = self.sock.recv(size)
        except OSError as error:
            raise ScreenshotError(f"Marionette connection failed: {error}") from error
        if not data:
            raise ScreenshotError("Marionette connection closed")
        return data

    def command(self, name: str, parameters: dict | None = None):
        self.counter += 1
        message = json.dumps([0, self.counter, name, parameters or {}]).encode()
        try:
            self.sock.sendall(str(len(message)).encode() + b":" + message)
        except OSError as error:
            raise ScreenshotError(f"Marionette connection failed: {error}") from error
        reply = self.receive()
        if reply[2]:
            raise ScreenshotError(f"Firefox command {name} failed: {reply[2]}")
        return reply[3]

    def run(self, source: str):
        return self.command("WebDriver:ExecuteScript", {"script": source, "args": []})["value"]

    def close(self) -> None:
        with contextlib.suppress(Exception):
            self.command("Marionette:Quit", {"flags": ["eForceQuit"]})
        self.sock.close()


@contextlib.contextmanager
def headless_firefox(firefox: Path, work: Path):
    """Start headless Firefox with a throwaway profile; always stop it on exit."""
    port, dead_proxy, profile = free_port(), free_port(), work / "profile"
    profile.mkdir()
    prefs = dict(PREFS, **{"marionette.port": port},
                 **{f"network.proxy.{kind}_port": dead_proxy for kind in ("http", "ssl", "socks")})
    (profile / "user.js").write_text(
        "".join(f"user_pref({json.dumps(k)}, {json.dumps(v)});\n" for k, v in prefs.items()),
        encoding="ascii",
    )
    command = [str(firefox), "--headless", "--no-remote", "--marionette",
               "--profile", str(profile), f"--window-size={WIDTH},600"]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    browser = None
    try:
        browser = Marionette.connect(port)
        browser.command("WebDriver:NewSession", {"capabilities": {}})
        yield browser
    finally:
        if browser:
            browser.close()
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def wait_for(browser: Marionette, source: str, timeout: float = 40.0) -> None:
    deadline = time.monotonic() + timeout
    while not browser.run(source):
        if time.monotonic() > deadline:
            raise ScreenshotError("The page did not finish rendering in time")
        time.sleep(0.5)


def fit_viewport(browser: Marionette, height: int) -> None:
    """Resize the window until the inner viewport is exactly WIDTH x height CSS px."""
    browser.command("WebDriver:SetWindowRect", {"width": WIDTH, "height": 400})
    request = height + 400 - browser.run("return innerHeight")
    for _ in range(4):
        browser.command("WebDriver:SetWindowRect", {"width": WIDTH, "height": request})
        time.sleep(1.5)
        actual = browser.run("return innerHeight")
        if actual == height and browser.run("return innerWidth") == WIDTH:
            return
        request += height - actual
    raise ScreenshotError(f"Could not size the viewport to {WIDTH}x{height}")


def layout_problems(layout: dict) -> list[str]:
    """What is wrong with a measured layout; empty when the viewport is exactly the image we want."""
    problems = []
    if layout["width"] != WIDTH or layout["client"] != WIDTH:
        problems.append(f"width is {layout['width']} and client width {layout['client']}, expected {WIDTH}")
    if layout["scroll"] - layout["height"] > BODY_OVERFLOW:
        problems.append(f"scroll height {layout['scroll']} exceeds the height {layout['height']} by more than {BODY_OVERFLOW}")
    if layout["bottom"] + BOTTOM_MARGIN != layout["height"]:
        problems.append(f"content bottom {layout['bottom']} plus {BOTTOM_MARGIN} differs from height {layout['height']}")
    return problems


def capture(browser: Marionette, url: str, page: str) -> bytes:
    """Load the page, wait until it is rendered, size the viewport to it, check it and capture."""
    browser.command("WebDriver:Navigate", {"url": url})
    wait_for(browser, ready_script(page))
    time.sleep(3)  # let charts and the freshness line settle
    fit_viewport(browser, PROBE_HEIGHT)  # the width is final from here on
    layout = browser.run(LAYOUT)
    for _ in range(2):  # the second pass corrects a content height that moved after the resize
        fit_viewport(browser, layout["bottom"] + BOTTOM_MARGIN)
        layout = browser.run(LAYOUT)
        problems = layout_problems(layout)
        if not problems:
            break
    else:
        raise ScreenshotError(f"{page}: unexpected layout ({'; '.join(problems)}): {layout}")
    shot = browser.command("WebDriver:TakeScreenshot", {"full": False, "hash": False, "scroll": False})
    return strip_png_metadata(base64.b64decode(shot["value"]))


def write_harness(work: Path) -> Path:
    """Write the harness into a dedicated subdirectory of the work directory."""
    site = work / "harness"
    site.mkdir()
    (site / "harness.html").write_text(build_harness(FIXTURE.read_text(encoding="utf-8")), encoding="utf-8")
    return site


def render_all(firefox: Path) -> dict[str, bytes]:
    """Render every shot; the browser and the server are stopped before returning."""
    with tempfile.TemporaryDirectory(prefix="ups-screenshots-") as name:
        work = Path(name)
        site = write_harness(work)
        with serve(site) as address, headless_firefox(firefox, work) as browser:
            return {file: capture(browser, page_url(address, page, scenario, index), page)
                    for index, (file, page, scenario) in enumerate(SHOTS)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--firefox", help="path to the Firefox executable (default: PATH, then the macOS app)")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output directory (default: docs/screenshots)")
    arguments = parser.parse_args(argv)
    try:
        images = render_all(find_firefox(arguments.firefox))
    except ScreenshotError as error:
        print(f"make_screenshots: {error}", file=sys.stderr)
        return 1
    arguments.out.mkdir(parents=True, exist_ok=True)
    for file, data in images.items():
        (arguments.out / file).write_bytes(data)
        print(f"{arguments.out / file}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
