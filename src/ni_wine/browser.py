"""Capture an authenticated download URL from the NI website.

Native Instruments puts product downloads behind a login.  We open the
downloads page in a Chromium-family browser together with a throwaway
extension that posts the download link to a local HTTP server as soon as
it appears on the page.

The browser runs with its own profile directory (kept in the ni-wine state
dir) — this guarantees a dedicated browser process we can wait on, keeps
ni-wine out of the user's browsing profile, and lets the NI login survive
between runs.
"""

from __future__ import annotations

import http.server
import json
import subprocess
import tempfile
import threading
from pathlib import Path

from . import config
from .util import die, info, which_first

BROWSER_CANDIDATES = (
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

# Ubuntu's chromium is snap-only and not necessarily on PATH.
_SNAP_CHROMIUM = Path("/snap/bin/chromium")


def probe_browser() -> str | None:
    browser = which_first(*BROWSER_CANDIDATES)
    if browser:
        return browser
    if _SNAP_CHROMIUM.is_file():
        return str(_SNAP_CHROMIUM)
    return None


def find_browser() -> str:
    browser = probe_browser()
    if browser is None:
        die(
            "no Chromium-family browser found (tried: "
            + ", ".join(BROWSER_CANDIDATES)
            + ").\nInstall chromium, or pass the URL directly: ni kontakt8 install <url>"
        )
    return browser


def _is_confined(browser: str) -> bool:
    """Snap-packaged browsers can only read non-hidden paths under $HOME."""
    return "/snap/" in browser


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


def capture_download_url(
    page: str = config.NI_DOWNLOADS_PAGE,
    link_substring: str = config.KONTAKT8_ZIP_NAME,
) -> str:
    """Open the NI downloads page and return the captured download URL."""
    browser = find_browser()
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
        info("Log in to Native Instruments in the browser window that just opened.")
        info("It closes automatically once the download link is found.")
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
    info("download URL captured")
    return server.captured_url
