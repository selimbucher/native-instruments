"""Capture an authenticated download URL from the NI website.

Native Instruments puts product downloads behind a login, and the download
links appear only after the page's scripts run — so we open the downloads
page in a real browser and watch the DOM for the link.

Two strategies, picked by what's installed:

- Chromium family: a throwaway extension's content script posts the link
  to a localhost HTTP server.
- Firefox family: release Firefox refuses unsigned extensions, but ships
  Marionette (the automation protocol behind geckodriver) — we poll the
  page for the link over a local TCP session instead.

Either way the browser runs with its own profile directory (kept in the
ni-wine state dir): that guarantees a dedicated process we can wait on,
keeps ni-wine out of the user's browsing profile, and lets the NI login
survive between runs.
"""

from __future__ import annotations

import http.server
import json
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

from . import config
from .util import die, info, which_first

CHROMIUM_CANDIDATES = (
    "chromium",
    "chromium-browser",
    "google-chrome-stable",
    "google-chrome",
    "brave-browser",
    "brave",
    "vivaldi-stable",
    "vivaldi",
    "microsoft-edge-stable",
    "microsoft-edge",
)

FIREFOX_CANDIDATES = (
    "firefox",
    "firefox-esr",
    "librewolf",
    "waterfox",
)

# Ubuntu's chromium is snap-only and not necessarily on PATH.
_SNAP_CHROMIUM = Path("/snap/bin/chromium")


def probe_browser() -> tuple[str, str] | None:
    """Return (kind, path) of a usable browser, or None.

    kind is "chromium" or "firefox".
    """
    browser = which_first(*CHROMIUM_CANDIDATES)
    if browser:
        return ("chromium", browser)
    if _SNAP_CHROMIUM.is_file():
        return ("chromium", str(_SNAP_CHROMIUM))
    browser = which_first(*FIREFOX_CANDIDATES)
    if browser:
        return ("firefox", browser)
    return None


def _is_confined(browser: str) -> bool:
    """Snap-packaged browsers can only read non-hidden paths under $HOME."""
    return "/snap/" in browser


def _login_hint() -> None:
    info("Log in to Native Instruments in the browser window that just opened.")
    info("It closes automatically once the download link is found.")


# --- Chromium strategy: throwaway extension + localhost HTTP server --------


class _CaptureHandler(http.server.BaseHTTPRequestHandler):
    def _cors(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def do_OPTIONS(self) -> None:  # noqa: N802 (http.server API)
        self.send_response(200)
        self._cors()
        self.end_headers()

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", 0))
        url = self.rfile.read(length).decode().strip()
        self.send_response(200)
        self._cors()
        self.end_headers()
        server: _CaptureServer = self.server  # type: ignore[assignment]
        server.captured_url = url
        server.captured.set()

    def log_message(self, *args: object) -> None:
        pass


class _CaptureServer(http.server.HTTPServer):
    allow_reuse_address = True
    captured_url: str = ""
    captured: threading.Event

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), _CaptureHandler)
        self.captured = threading.Event()


def _write_extension(directory: Path, port: int, link_substring: str) -> None:
    manifest = {
        "manifest_version": 3,
        "name": "NI URL Capture",
        "version": "1.0",
        "content_scripts": [
            {
                "matches": ["https://www.native-instruments.com/*/account/downloads/*"],
                "js": ["capture.js"],
                "run_at": "document_idle",
            }
        ],
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2))
    (directory / "capture.js").write_text(
        f"""\
(function poll() {{
  var links = document.querySelectorAll('a[href*="{link_substring}"]');
  if (!links.length) {{ setTimeout(poll, 1000); return; }}
  fetch('http://127.0.0.1:{port}', {{ method: 'POST', body: links[0].href }});
}})();
"""
    )


def _capture_chromium(browser: str, page: str, link_substring: str) -> str:
    if _is_confined(browser):
        # A snap browser cannot read dot-directories; stage everything in a
        # visible directory under $HOME for the duration of the capture.
        staging_root: Path | None = Path.home() / "ni-wine-browser"
        profile = staging_root / "profile"
    else:
        staging_root = None
        profile = config.state_dir() / "capture-profile"
    profile.mkdir(parents=True, exist_ok=True)

    server = _CaptureServer()
    port = server.server_address[1]
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    with tempfile.TemporaryDirectory(
        prefix="ni-wine-ext-", dir=staging_root
    ) as ext_dir:
        _write_extension(Path(ext_dir), port, link_substring)
        _login_hint()
        info(
            "If the browser asks to 'access other apps and services on this "
            "device', click Allow — that is how the link reaches ni-wine."
        )
        proc = subprocess.Popen(
            [
                browser,
                f"--app={page}",
                f"--user-data-dir={profile}",
                f"--load-extension={ext_dir}",
                f"--disable-extensions-except={ext_dir}",
                "--no-first-run",
                "--no-default-browser-check",
                # The capture extension posts the link to 127.0.0.1, which
                # newer Chromium gates behind a local-network-access prompt.
                "--disable-features=LocalNetworkAccessChecks,PrivateNetworkAccessChecks",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            while proc.poll() is None and not server.captured.wait(timeout=1):
                pass
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
            server.shutdown()
            server.server_close()

    if not server.captured_url:
        die("browser closed before a download link was captured")
    return server.captured_url


# --- Firefox strategy: Marionette session, no extension needed -------------


class _MarionetteError(Exception):
    pass


class _Marionette:
    """Minimal Marionette client (protocol 3: `length:json` framing)."""

    def __init__(self, port: int, timeout: float = 30) -> None:
        self._sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self._sock.settimeout(timeout)
        self._msg_id = 0
        handshake = self._recv()
        if handshake.get("marionetteProtocol") != 3:
            raise _MarionetteError(f"unsupported protocol: {handshake}")

    def _recv(self) -> dict | list:
        length_bytes = b""
        while True:
            ch = self._sock.recv(1)
            if not ch:
                raise ConnectionError("marionette connection closed")
            if ch == b":":
                break
            length_bytes += ch
        remaining = int(length_bytes)
        body = b""
        while remaining:
            chunk = self._sock.recv(remaining)
            if not chunk:
                raise ConnectionError("marionette connection closed mid-message")
            body += chunk
            remaining -= len(chunk)
        return json.loads(body)

    def request(self, command: str, params: dict) -> dict:
        self._msg_id += 1
        body = json.dumps([0, self._msg_id, command, params]).encode()
        self._sock.sendall(str(len(body)).encode() + b":" + body)
        while True:
            reply = self._recv()
            if isinstance(reply, list) and reply[0] == 1 and reply[1] == self._msg_id:
                error, result = reply[2], reply[3]
                if error is not None:
                    raise _MarionetteError(error.get("error", "unknown error"))
                return result or {}

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _marionette_connect(port: int, proc: subprocess.Popen) -> _Marionette:
    while proc.poll() is None:
        try:
            client = _Marionette(port)
            client.request("WebDriver:NewSession", {})
            return client
        except (OSError, ConnectionError):
            time.sleep(0.5)
    die("browser closed before a download link was captured")


def _capture_firefox(
    browser: str, page: str, link_substring: str, *, headless: bool = False
) -> str:
    profile = config.state_dir() / "firefox-profile"
    profile.mkdir(parents=True, exist_ok=True)
    port = _free_port()
    (profile / "user.js").write_text(
        f'user_pref("marionette.port", {port});\n'
        'user_pref("remote.log.level", "Fatal");\n'
        'user_pref("browser.shell.checkDefaultBrowser", false);\n'
        'user_pref("browser.aboutwelcome.enabled", false);\n'
    )

    args = [browser, "--marionette", "--profile", str(profile),
            "--no-remote", "--new-instance"]
    if headless:
        args.append("--headless")
    args.append(page)

    _login_hint()
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    script = (
        f"const a = document.querySelector('a[href*=\"{link_substring}\"]');"
        "return a ? a.href : '';"
    )
    client: _Marionette | None = None
    try:
        client = _marionette_connect(port, proc)
        while proc.poll() is None:
            try:
                value = client.request(
                    "WebDriver:ExecuteScript", {"script": script, "args": []}
                ).get("value")
                if value:
                    return str(value)
            except _MarionetteError:
                # Browsing context lost (login navigations, process swaps) —
                # reattach to whatever window is current.
                try:
                    handles = client.request("WebDriver:GetWindowHandles", {})
                    first = (handles.get("value") or [None])[0]
                    if first:
                        client.request("WebDriver:SwitchToWindow", {"handle": first})
                except (_MarionetteError, OSError, ConnectionError):
                    pass
            except (OSError, ConnectionError):
                client.close()
                client = _marionette_connect(port, proc)
            time.sleep(1)
        die("browser closed before a download link was captured")
    finally:
        if client is not None:
            client.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()


# --- Entry point -----------------------------------------------------------


def capture_download_url(
    page: str = config.NI_DOWNLOADS_PAGE,
    link_substring: str = config.KONTAKT8_ZIP_NAME,
) -> str:
    """Open the NI downloads page and return the captured download URL."""
    probed = probe_browser()
    if probed is None:
        die(
            "no supported browser found (chromium- or firefox-family).\n"
            "Install one, or pass the URL directly: ni kontakt8 install <url>"
        )
    kind, browser = probed
    if kind == "firefox":
        url = _capture_firefox(browser, page, link_substring)
    else:
        url = _capture_chromium(browser, page, link_substring)
    info("download URL captured")
    return url
