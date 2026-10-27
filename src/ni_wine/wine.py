from __future__ import annotations

import contextlib
import os
import re
import shutil
import subprocess
import tempfile
import time
from collections.abc import Iterator
from pathlib import Path

from . import config
from .util import die, info, warn


def session_wine(prefix: Path) -> str | None:
    """Wine of the wineserver already running this prefix. Only one build can use a prefix at a
    time, and on NixOS the one that started the session (yabridge's) is rarely the one on PATH.
    The server's cwd is the server dir, its wine sits next to it.
    """
    try:
        st = os.stat(prefix)
    except OSError:
        return None
    server_dir = f"/tmp/.wine-{os.getuid()}/server-{st.st_dev:x}-{st.st_ino:x}"
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            if os.readlink(f"/proc/{entry.name}/cwd") != server_dir:
                continue
            exe = Path(os.readlink(f"/proc/{entry.name}/exe"))
        except OSError:
            continue
        if exe.name.startswith("wineserver") and (exe.parent / "wine").is_file():
            return str(exe.parent / "wine")
    return None


def find_wine(prefix: Path | None = None) -> str:
    wine = os.environ.get("WINE")
    if wine:
        return wine
    on_path = shutil.which("wine")
    running = session_wine(prefix) if prefix else None
    if running and (not on_path or not _same_binary(running, on_path)):
        if running not in _announced:
            _announced.add(running)
            info(f"using {running}: it already runs this prefix's Wine session")
        return running
    if not on_path:
        die("wine not found on PATH (set $WINE to override)")
    return on_path


_announced: set[str] = set()


def _same_binary(a: str, b: str) -> bool:
    try:
        return os.path.samefile(a, b)
    except OSError:
        return False


def find_wineserver(wine: str) -> str | None:
    candidate = Path(wine).parent / "wineserver"
    if candidate.is_file():
        return str(candidate)
    return shutil.which("wineserver")


def wine_build_id(wine: str) -> str | None:
    """`wine --version` output, e.g. "wine-11.14 (Staging)", or None."""
    try:
        out = subprocess.run([wine, "--version"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    line = out.stdout.strip().splitlines()
    return line[0] if line else None


def wine_version_problem(build: str | None) -> tuple[str, str] | None:
    """Check a `wine --version` string against what NA needs: 8.0 can't install the PowerShell MSI,
    NA crashes on 9.x and mostly gets no GL context on 10.x, 11.x works (verified 2026-09).
    Returns None, ("error", msg) or ("warn", msg).
    """
    if build is None:
        return ("warn", "could not determine the Wine version")
    match = re.search(r"wine-(\d+)\.(\d+)", build)
    if not match:
        return ("warn", f"could not parse the Wine version from {build!r}")
    major, minor = int(match.group(1)), int(match.group(2))
    if (major, minor) == (11, 6):
        return (
            "warn",
            f"{build} has a known VC++ installer regression (WineHQ "
            "bug 59632, fixed in 11.7). If setup fails at vcrun2022, "
            "upgrade Wine",
        )
    if major < 10:
        return (
            "error",
            f"{build} is too old, Native Access needs Wine >= 11 "
            "(Debian/Ubuntu: install winehq-staging or winehq-stable "
            "from the WineHQ repository)",
        )
    if major == 10:
        return (
            "warn",
            f"{build}: Native Access often fails to start under Wine 10, Wine >= 11 is recommended",
        )
    return None


def check_wine_version(wine: Wine) -> None:
    """Die or warn on Wine builds Native Access can't run on."""
    problem = wine_version_problem(wine_build_id(wine.wine))
    if problem is None:
        return
    severity, message = problem
    running = session_wine(wine.prefix)
    if running and _same_binary(running, wine.wine):
        # find_wine followed a session started by another app (yabridge from a DAW)
        message += (
            "\nThis Wine comes from the session already running the prefix. "
            "Upgrade the Wine of whatever started it (yabridge?), "
            "or close that application first."
        )
    if severity == "error":
        die(message)
    warn(message)


# Wine migrates a prefix when the build changes, detected by comparing file
# mtimes with .update-timestamp. On Nix every store file has mtime 1, so that
# never fires, and an unmigrated prefix under a new build breaks (missing DLL
# forwards, explorer.exe respawning). So track the build ourselves.
_BUILD_MARKER = ".ni-wine-build"


def record_prefix_build(wine: Wine) -> None:
    build = wine_build_id(wine.wine)
    if build:
        (wine.prefix / _BUILD_MARKER).write_text(build)


def prefix_booted_by_ni_wine(prefix: Path) -> bool:
    """True once ni-wine ran wineboot here, tells our half-built prefix from a foreign one."""
    return (prefix / _BUILD_MARKER).exists()


def ensure_prefix_build(wine: Wine) -> None:
    """Run the prefix migration Wine itself cannot detect on Nix."""
    if not config.drive_c(wine.prefix).is_dir():
        return
    build = wine_build_id(wine.wine)
    if build is None:
        return
    marker = wine.prefix / _BUILD_MARKER
    try:
        recorded = marker.read_text().strip()
    except OSError:
        recorded = ""
    if recorded == build:
        return
    if prefix_in_use(wine.prefix):
        # a DAW (yabridge) or Kontakt may be using the prefix, don't reboot Wine under it
        return
    info(f"Wine build changed ({recorded or 'unknown'} -> {build}), updating the prefix...")
    wine.run(["wineboot", "-u"], display="")
    # wineboot ran without a display and the screen size is fixed when the
    # session starts (1024x768 without one). The daemon would keep that
    # session alive and NA would never paint past 1024x768, so end it here.
    wine.kill_server()
    wine.wait_server()
    record_prefix_build(wine)


class Wine:
    def __init__(self, prefix: Path) -> None:
        self.prefix = prefix
        self.wine = find_wine(prefix)
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
        return subprocess.run([self.wine, *args], env=env, check=check, stdout=out, stderr=out)

    def reg_add(
        self,
        key: str,
        value: str,
        data: str,
        *,
        reg_type: str = "REG_SZ",
        kill_on_failure: bool = True,
    ) -> bool:
        args = ["reg", "add", key]
        if value:
            args += ["/v", value]
        else:
            args += ["/ve"]
        args += ["/t", reg_type, "/d", data, "/f"]

        for attempt in (1, 2):
            # no pipes: on a cold prefix the first wine call starts services
            # that inherit our stdio and would keep a pipe open forever
            with tempfile.TemporaryFile(mode="w+", encoding="utf-8", errors="replace") as err:
                result = subprocess.run(
                    [self.wine, *args],
                    env=self.env(),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=err,
                )
                err.seek(0)
                stderr = err.read()
            if result.returncode == 0:
                return True
            if attempt == 1 and kill_on_failure:
                # a stale wineserver from another wine version fails every call
                self.kill_server()
                time.sleep(1)
        tail = stderr.strip().splitlines()
        warn(
            f"failed to set registry value {key}\\{value or '(default)'}"
            + (f": {tail[-1]}" if tail else "")
        )
        return False

    def reg_query(self, key: str, value: str) -> str | None:
        with tempfile.TemporaryFile(mode="w+", encoding="utf-8", errors="replace") as out:
            result = subprocess.run(
                [self.wine, "reg", "query", key, "/v", value],
                env=self.env(),
                stdin=subprocess.DEVNULL,
                stdout=out,
                stderr=subprocess.DEVNULL,
            )
            out.seek(0)
            stdout = out.read()
        if result.returncode != 0:
            return None
        for line in stdout.splitlines():
            parts = line.split(None, 2)
            if len(parts) == 3 and parts[0].lower() == value.lower():
                return parts[2].strip()
        return None

    def reg_delete(self, key: str) -> bool:
        result = subprocess.run(
            [self.wine, "reg", "delete", key, "/f"],
            env=self.env(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return result.returncode == 0

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
        return self._reg_text("user.reg")

    def system_reg_text(self) -> str:
        return self._reg_text("system.reg")

    def _reg_text(self, name: str) -> str:
        try:
            return (self.prefix / name).read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""


def prefix_in_use(prefix: Path) -> bool:
    """Any Wine process with its cwd inside drive_c, works with WINEPREFIX unset or stale."""
    try:
        wanted = os.stat(prefix)
    except OSError:
        return False
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            cwd = os.readlink(f"/proc/{entry.name}/cwd")
            if "/drive_c" not in cwd:
                continue
            found = os.stat(cwd.split("/drive_c", 1)[0])
        except OSError:
            continue
        if (found.st_dev, found.st_ino) == (wanted.st_dev, wanted.st_ino):
            return True
    return False


def foreign_prefix_users(prefix: Path) -> list[int]:
    """PIDs of yabridge plugin hosts in the prefix.

    Killing the wineserver takes them and the DAW down.
    """
    result = subprocess.run(["pgrep", "-f", "yabridge-host"], capture_output=True, text=True)
    wanted = str(prefix.expanduser().resolve())
    pids = []
    for pid in result.stdout.split():
        try:
            environ = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
            stat = Path(f"/proc/{pid}/stat").read_text()
        except OSError:
            continue
        # hosts whose DAW is gone got reparented to init, leftovers
        ppid = int(stat.rsplit(")", 1)[1].split()[1])
        if ppid <= 1:
            continue
        for entry in environ:
            if entry.startswith(b"WINEPREFIX="):
                value = entry[len(b"WINEPREFIX=") :].decode(errors="replace")
                if str(Path(value).expanduser().resolve()) == wanted:
                    pids.append(int(pid))
                break
    return pids


def _start_xvfb(xvfb: str, size: str) -> tuple[subprocess.Popen, str] | None:
    """Xvfb on a free display well above any session's. Not -displayfd, its scan starts at :0 and
    can unlink a live session display's socket when the lock file is missing (XWayland).
    """
    for number in range(90, 111):
        if Path(f"/tmp/.X{number}-lock").exists() or Path(f"/tmp/.X11-unix/X{number}").exists():
            continue
        proc = subprocess.Popen(
            [xvfb, f":{number}", "-screen", "0", size, "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        socket_path = Path(f"/tmp/.X11-unix/X{number}")
        for _ in range(50):
            if proc.poll() is not None:
                break  # lost a race with another server, next number
            if socket_path.exists():
                return proc, f":{number}"
            time.sleep(0.1)
        if proc.poll() is None:
            proc.terminate()
    return None


@contextlib.contextmanager
def hidden_display(size: str = "1280x1024x24") -> Iterator[str | None]:
    """Throwaway Xvfb display, or None (use the real one) when Xvfb is missing."""
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

# Without an XEmbed tray (Hyprland and other Wayland compositors) Wine shows
# tray icons in a floating explorer.exe window. Wine >= 9.2 turns that off
# with the Explorer DWORD, <= 8.21 with the X11 Driver string (kept for old
# distro wines), NoTrayItemsDisplay (>= 9.22) also stops XEmbed docking.
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
    """Apply the missing registry tweaks, True if anything changed. Text scan of user.reg; callers
    kill the wineserver after a change, explorer.exe reads these at startup only.
    """
    reg_text = wine.user_reg_text()
    changed = False

    for marker, key, value, data, reg_type in _TWEAKS:
        if marker not in reg_text:
            wine.reg_add(key, value, data, reg_type=reg_type)
            changed = True

    from . import urlscheme  # avoids an import cycle

    if urlscheme.ensure(wine):
        changed = True

    return changed


def _reg_default(reg_text: str, key: str) -> str | None:
    escaped = key.replace("\\", "\\\\")
    match = re.search(
        rf"^\[{re.escape(escaped)}\] \d+\n(?:#[^\n]*\n)*@=\"((?:[^\"\\]|\\.)*)\"",
        reg_text,
        re.MULTILINE,
    )
    if not match:
        return None
    return match.group(1).replace('\\"', '"').replace("\\\\", "\\")
