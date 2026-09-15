"""Wine invocation, hidden X displays, registry management, prefix tweaks."""

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
    """The wine of the wineserver already running *prefix*, if there is one.

    A prefix can only be used by one Wine build at a time; a second one
    fails with "version mismatch".  On NixOS the build that started the
    session (say, yabridge's, from the DAW) is rarely the one on our PATH,
    so follow the running server: its cwd is the prefix's server directory
    and its binary sits next to the matching wine.
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
        out = subprocess.run(
            [wine, "--version"], capture_output=True, text=True, timeout=30
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    line = out.stdout.strip().splitlines()
    return line[0] if line else None


def wine_version_problem(build: str | None) -> tuple[str, str] | None:
    """Classify a `wine --version` string against Native Access's needs.

    The single source of truth for the version floor (verified 2026-09):
    wine 8.0 cannot install the PowerShell MSI; Native Access itself
    crashes deterministically on 9.x and commonly fails to get a GL
    context on 10.x; 11.x works.  Returns None if the build is fine, else
    ("error", msg) for a build that will not work or ("warn", msg) for one
    that may not.  Both the setup/launch gate and `ni doctor` consume this.
    """
    if build is None:
        return ("warn", "could not determine the Wine version")
    match = re.search(r"wine-(\d+)\.", build)
    if not match:
        return ("warn", f"could not parse the Wine version from {build!r}")
    major = int(match.group(1))
    if major < 10:
        return ("error",
                f"{build} is too old — Native Access needs Wine >= 11 "
                "(Debian/Ubuntu: install winehq-staging or winehq-stable "
                "from the WineHQ repository)")
    if major == 10:
        return ("warn",
                f"{build}: Native Access often fails to start under Wine 10 "
                "— Wine >= 11 is recommended")
    return None


def check_wine_version(wine: Wine) -> None:
    """Refuse (or warn about) Wine builds Native Access cannot run on."""
    problem = wine_version_problem(wine_build_id(wine.wine))
    if problem is None:
        return
    severity, message = problem
    running = session_wine(wine.prefix)
    if running and _same_binary(running, wine.wine):
        # find_wine followed a session another app started (a DAW hosting
        # plugins through yabridge, say) — the fix is on that side.
        message += (
            "\nThis Wine was taken from the session already running the "
            "prefix — upgrade the Wine of whatever started it (yabridge?), "
            "or close that application first."
        )
    if severity == "error":
        die(message)
    warn(message)


# Wine normally migrates a prefix when the Wine build changes, but it
# detects the change by comparing file mtimes against .update-timestamp —
# and on Nix every store file has mtime 1, so the check can never fire
# there.  Running an un-migrated prefix under a different build breaks
# loudly (missing DLL forwards, respawning explorer.exe), so we track the
# build ourselves and force the migration wineboot skips.
_BUILD_MARKER = ".ni-wine-build"


def record_prefix_build(wine: Wine) -> None:
    build = wine_build_id(wine.wine)
    if build:
        (wine.prefix / _BUILD_MARKER).write_text(build)


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
        # A DAW (via yabridge) or Kontakt's Activate button may be holding
        # this prefix; wineboot -u would reboot Wine underneath it.  Skip —
        # the running session's own Wine already matches what started it.
        return
    info(f"Wine build changed ({recorded or 'unknown'} -> {build}) — "
         "updating the prefix...")
    wine.run(["wineboot", "-u"], display="")
    record_prefix_build(wine)


class Wine:
    """A wine installation bound to one prefix."""

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
        return subprocess.run(
            [self.wine, *args], env=env, check=check, stdout=out, stderr=out
        )

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
            # No pipes: on a cold prefix the first wine call boots services
            # that inherit our stdio and would hold a pipe open indefinitely.
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
                # A stale wineserver (e.g. from a previous wine version)
                # makes every wine call fail; kick it and try once more.
                self.kill_server()
                time.sleep(1)
        tail = stderr.strip().splitlines()
        warn(
            f"failed to set registry value {key}\\{value or '(default)'}"
            + (f": {tail[-1]}" if tail else "")
        )
        return False

    def reg_query(self, key: str, value: str) -> str | None:
        """The data of *value* under *key*, or None (asks wineserver)."""
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
        return self._reg_text("user.reg")

    def system_reg_text(self) -> str:
        return self._reg_text("system.reg")

    def _reg_text(self, name: str) -> str:
        try:
            return (self.prefix / name).read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""


def prefix_in_use(prefix: Path) -> bool:
    """Is any Wine process running inside *prefix* (NA, Kontakt, a daemon...)?

    Judged by the working directory Wine gives its processes (inside the
    prefix's drive_c), which stays valid when WINEPREFIX is unset or stale.
    """
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
    """PIDs of yabridge plugin hosts running inside *prefix*.

    A DAW's bridged plugins share the prefix's wineserver; killing that
    server (kill_server, reinstall) takes them — and the DAW — down with it.
    """
    result = subprocess.run(
        ["pgrep", "-f", "yabridge-host"], capture_output=True, text=True
    )
    wanted = str(prefix.expanduser().resolve())
    pids = []
    for pid in result.stdout.split():
        try:
            environ = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
            stat = Path(f"/proc/{pid}/stat").read_text()
        except OSError:
            continue
        # Hosts whose DAW is gone get re-parented to init/systemd; they are
        # leftovers, not a live session.
        ppid = int(stat.rsplit(")", 1)[1].split()[1])
        if ppid <= 1:
            continue
        for entry in environ:
            if entry.startswith(b"WINEPREFIX="):
                value = entry[len(b"WINEPREFIX="):].decode(errors="replace")
                if str(Path(value).expanduser().resolve()) == wanted:
                    pids.append(int(pid))
                break
    return pids


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

    from . import urlscheme  # avoids an import cycle

    if urlscheme.ensure(wine):
        changed = True

    return changed


def _reg_default(reg_text: str, key: str) -> str | None:
    """The default value of *key* (e.g. Software\\Classes\\x) in a .reg dump."""
    escaped = key.replace("\\", "\\\\")
    match = re.search(
        rf"^\[{re.escape(escaped)}\] \d+\n(?:#[^\n]*\n)*@=\"((?:[^\"\\]|\\.)*)\"",
        reg_text, re.MULTILINE,
    )
    if not match:
        return None
    return match.group(1).replace('\\"', '"').replace("\\\\", "\\")
