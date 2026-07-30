"""Wine invocation, hidden X displays, registry management, prefix tweaks."""

from __future__ import annotations

import contextlib
import os
import shutil
import subprocess
import time
from collections.abc import Iterator
from pathlib import Path

from . import config
from .util import die, info, warn


def find_wine() -> str:
    wine = os.environ.get("WINE") or shutil.which("wine")
    if not wine:
        die("wine not found on PATH (set $WINE to override)")
    return wine


def find_wineserver(wine: str) -> str | None:
    candidate = Path(wine).parent / "wineserver"
    if candidate.is_file():
        return str(candidate)
    return shutil.which("wineserver")


class Wine:
    """A wine installation bound to one prefix."""

    def __init__(self, prefix: Path) -> None:
        self.prefix = prefix
        self.wine = find_wine()
        self.wineserver = find_wineserver(self.wine)

    def env(self, extra: dict[str, str] | None = None) -> dict[str, str]:
        env = os.environ.copy()
        env["WINEPREFIX"] = str(self.prefix)
        env["WINEARCH"] = "win64"
        if extra:
            env.update(extra)
        return env

    def run(
        self,
        args: list[str],
        *,
        extra_env: dict[str, str] | None = None,
        display: str | None = None,
        check: bool = False,
        quiet: bool = True,
    ) -> subprocess.CompletedProcess:
        env = self.env(extra_env)
        if display is not None:
            env["DISPLAY"] = display
            env.pop("WAYLAND_DISPLAY", None)
        out = subprocess.DEVNULL if quiet else None
        return subprocess.run(
            [self.wine, *args], env=env, check=check, stdout=out, stderr=out
        )

    def reg_add(self, key: str, value: str, data: str, *, reg_type: str = "REG_SZ") -> bool:
        args = ["reg", "add", key]
        if value:
            args += ["/v", value]
        else:
            args += ["/ve"]
        args += ["/t", reg_type, "/d", data, "/f"]

        for attempt in (1, 2):
            result = subprocess.run(
                [self.wine, *args],
                env=self.env(),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
            )
            if result.returncode == 0:
                return True
            if attempt == 1:
                # A stale wineserver (e.g. from a previous wine version)
                # makes every wine call fail; kick it and try once more.
                self.kill_server()
                time.sleep(1)
        tail = (result.stderr or "").strip().splitlines()
        warn(
            f"failed to set registry value {key}\\{value or '(default)'}"
            + (f": {tail[-1]}" if tail else "")
        )
        return False

    def kill_server(self) -> None:
        if not self.wineserver:
            return
        subprocess.run(
            [self.wineserver, "-k"],
            env=self.env(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def wait_server(self, timeout: float = 30) -> None:
        """Block until wineserver has exited (all wine processes done)."""
        if not self.wineserver:
            time.sleep(2)
            return
        subprocess.run(
            [self.wineserver, "-w"],
            env=self.env(),
            timeout=timeout,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    # -- registry file inspection (no wineserver round-trip) ----------------

    def user_reg_text(self) -> str:
        reg = self.prefix / "user.reg"
        try:
            return reg.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""


def _start_xvfb(xvfb: str, size: str) -> tuple[subprocess.Popen, str] | None:
    """Try display numbers well above any real session's.

    Deliberately NOT `-displayfd`: its free-display scan starts at :0 and can
    unlink the socket of a live session display whose lock file is missing
    (common with XWayland).  An explicit high number plus an existence
    pre-check never goes near the session display.
    """
    for number in range(90, 111):
        if (
            Path(f"/tmp/.X{number}-lock").exists()
            or Path(f"/tmp/.X11-unix/X{number}").exists()
        ):
            continue
        proc = subprocess.Popen(
            [xvfb, f":{number}", "-screen", "0", size, "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        socket_path = Path(f"/tmp/.X11-unix/X{number}")
        for _ in range(50):
            if proc.poll() is not None:
                break  # lost a startup race with another server — next number
            if socket_path.exists():
                return proc, f":{number}"
            time.sleep(0.1)
        if proc.poll() is None:
            proc.terminate()
    return None


@contextlib.contextmanager
def hidden_display(size: str = "1280x1024x24") -> Iterator[str | None]:
    """Start a throwaway Xvfb server and yield its DISPLAY string.

    Yields None (meaning: use the real display) when Xvfb is unavailable —
    setup still works, the installer windows are just visible.
    """
    xvfb = shutil.which("Xvfb")
    if not xvfb:
        warn("Xvfb not found: installer windows will be visible")
        yield None
        return

    started = _start_xvfb(xvfb, size)
    if started is None:
        warn("Xvfb failed to start: installer windows will be visible")
        yield None
        return

    proc, display = started
    try:
        yield display
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


# --- Prefix tweaks ---------------------------------------------------------

# Wine shows tray icons in a floating standalone window on desktops without
# an XEmbed tray (Hyprland & other Wayland compositors).  The window comes
# from explorer.exe; modern Wine (9.0.1/9.2+) turns it off via the Explorer
# key as a DWORD, Wine <= 8.21 used a string under X11 Driver (kept for old
# distro wines, ignored by new ones), and NoTrayItemsDisplay (Wine >= 9.22)
# additionally suppresses icons from XEmbed docking.
_TWEAKS: list[tuple[str, str, str, str, str]] = [
    # (marker in user.reg, key, value name, data, type)
    (
        '"winemenubuilder.exe"=""',
        r"HKCU\Software\Wine\DllOverrides",
        "winemenubuilder.exe",
        "",
        "REG_SZ",
    ),
    (
        '"ShowSystray"=dword:00000000',
        r"HKCU\Software\Wine\Explorer",
        "ShowSystray",
        "0",
        "REG_DWORD",
    ),
    (
        '"ShowSystray"="N"',
        r"HKCU\Software\Wine\X11 Driver",
        "ShowSystray",
        "N",
        "REG_SZ",
    ),
    (
        '"NoTrayItemsDisplay"=dword:00000001',
        r"HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer",
        "NoTrayItemsDisplay",
        "1",
        "REG_DWORD",
    ),
]

TRAY_DISABLED_MARKER = '"ShowSystray"=dword:00000000'


def apply_prefix_tweaks(wine: Wine) -> bool:
    """Idempotently apply registry tweaks.  Returns True if anything changed.

    Cheap when nothing is missing (a text scan of user.reg, no wineserver).
    Callers should kill the wineserver after a change: explorer.exe only
    reads these values at startup.
    """
    reg_text = wine.user_reg_text()
    changed = False

    for marker, key, value, data, reg_type in _TWEAKS:
        if marker not in reg_text:
            wine.reg_add(key, value, data, reg_type=reg_type)
            changed = True

    # Native Access's MSI registers its login-callback URL scheme under the
    # literal, unexpanded name "${product.uri.scheme}" when run under Wine.
    # Register the real scheme so browser logins can reach the app.
    command = f'"{config.NA_EXE_WIN}" "%1"'
    scheme_key = rf"HKCU\Software\Classes\{config.URL_SCHEME}"
    if f'[Software\\\\Classes\\\\{config.URL_SCHEME}\\\\shell\\\\open\\\\command]' not in reg_text:
        wine.reg_add(scheme_key, "", f"URL:{config.URL_SCHEME}")
        wine.reg_add(scheme_key, "URL Protocol", "")
        wine.reg_add(rf"{scheme_key}\shell\open\command", "", command)
        info(f"registered {config.URL_SCHEME}:// URL scheme in the prefix")
        changed = True

    return changed
