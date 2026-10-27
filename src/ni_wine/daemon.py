"""Keep the NTK daemon installed and running before Native Access starts.

Without a daemon NA tries to reinstall it through PowerShell Start-Process
-Verb runAs, which fails with the winetricks profile.ps1, and NA sits on
"Please grant permission to Native Access to install dependencies" forever.
So install the daemon from the installer NA ships, `net start` it and
refuse to launch NA if it isn't there.

NA connects to the daemon by localhost port, not by prefix, so a daemon of
another prefix would answer with that prefix's login. foreign() catches those.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

from . import config
from .util import info, warn
from .wine import Wine

SERVICE = "NTKDaemonService"

# Where NA and the daemon write their logs (shown in error messages).
LOG_DIR_WIN = r"C:\users\Public\Documents\Native Instruments\Logs"

_STUCK_SCREEN = (
    "Native Access needs the daemon at startup; without it, it hangs on "
    "'Please grant permission to Native Access to install dependencies'"
)


# --- State -----------------------------------------------------------------


def installed(prefix: Path) -> bool:
    return config.ntk_daemon_exe(prefix).is_file()


def service_registered(prefix: Path) -> bool:
    """True if the service key exists in system.reg (no wineserver needed)."""
    try:
        text = (prefix / "system.reg").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return f"\\\\Services\\\\{SERVICE}]" in text


def _prefix_of(pid: int) -> str | None:
    """Prefix from /proc/<pid>/cwd. Unlike WINEPREFIX this notices a prefix recreated at the same
    path.
    """
    try:
        cwd = os.readlink(f"/proc/{pid}/cwd")
    except OSError:
        return None
    marker = "/drive_c/"
    if marker not in cwd:
        return None
    prefix = cwd.split(marker, 1)[0]
    return prefix + " (deleted)" if cwd.endswith("(deleted)") else prefix


def _same_prefix(found: str, prefix: Path) -> bool:
    if found.endswith("(deleted)"):
        return False
    try:
        return os.path.samefile(found, prefix)
    except OSError:
        return False


def _daemons() -> list[tuple[int, str | None]]:
    """(pid, prefix) of every NTKDaemon.exe on the machine (see _prefix_of)."""
    result = subprocess.run(["pgrep", "-f", r"NTKDaemon\.exe"], capture_output=True, text=True)
    found: list[tuple[int, str | None]] = []
    for pid in result.stdout.split():
        try:
            cmdline = Path(f"/proc/{pid}/cmdline").read_bytes()
        except OSError:
            continue  # gone already
        if b"NTKDaemon.exe" not in cmdline or b"pgrep" in cmdline:
            continue  # a shell whose command line mentions the name
        found.append((int(pid), _prefix_of(int(pid))))
    return found


def running(prefix: Path) -> bool:
    """True if an NTKDaemon.exe of this prefix runs, an unreadable cwd counts as a match."""
    return any(found is None or _same_prefix(found, prefix) for _, found in _daemons())


def foreign(prefix: Path) -> list[tuple[int, str]]:
    """Daemons of other prefixes, they would answer NA on localhost with their own login."""
    return [
        (pid, found)
        for pid, found in _daemons()
        if found is not None and not _same_prefix(found, prefix)
    ]


def foreign_problem(prefix: Path) -> str | None:
    others = foreign(prefix)
    if not others:
        return None
    lines = []
    for pid, other in others:
        if other.endswith("(deleted)"):
            lines.append(
                f"an NTK daemon of a prefix that no longer exists is still running "
                f"(pid {pid}, was {other[:-10]}); it would answer for this prefix "
                "with the old machine identity and licences"
            )
        else:
            lines.append(
                f"an NTK daemon from another Wine prefix is running (pid {pid}, prefix "
                f"{other}); Native Access would connect to it and use that prefix's "
                "login and licences"
            )
    hint = (
        "Stop it first: `WINEPREFIX=<that prefix> wine net stop "
        f"{SERVICE}` with the Wine build that runs it (or kill the process if the "
        "prefix is gone), then try again"
    )
    return "\n".join([*lines, hint])


def find_installer(prefix: Path) -> Path | None:
    """The NTKDaemon installer Native Access ships in its resources."""
    ntk_dir = config.ntk_installer_dir(prefix)
    if not ntk_dir.is_dir():
        return None
    candidates = sorted(ntk_dir.glob("NTKDaemon *.exe"))
    return candidates[-1] if candidates else None


# --- Actions ---------------------------------------------------------------


def install(wine: Wine, prefix: Path, *, display: str | None = None) -> str | None:
    """Run NA's bundled NTKDaemon installer (a WiX bundle) silently. Returns a problem or None."""
    installer = find_installer(prefix)
    if installer is None:
        return (
            f"NTKDaemon installer not found under {config.ntk_installer_dir(prefix)}, "
            "is Native Access installed? Run `ni setup`."
        )
    info(f"installing NTK daemon from {installer.name}...")
    result = wine.run([str(installer), "/s"], display=display)
    wine.kill_server()  # so the registry hits disk
    try:
        wine.wait_server()
    except subprocess.TimeoutExpired:
        warn("wineserver did not exit after the NTKDaemon install")

    missing = []
    if not installed(prefix):
        missing.append(f"{config.ntk_daemon_exe(prefix)} is missing")
    if not service_registered(prefix):
        missing.append(f"service {SERVICE} is not registered")
    if missing:
        return (
            f"NTKDaemon installer exited with {result.returncode} but "
            + " and ".join(missing)
            + f". Retry with NI_WINE_DEBUG=1 to see Wine's output, the "
            f"installer log (if any) is under {LOG_DIR_WIN}\\NTK in the prefix."
        )
    return None


def stale_locks(prefix: Path) -> list[Path]:
    """boost::interprocess lock files no process has open. Leftovers under
    ProgramData/boost_interprocess/<boot stamp>/ are harmless on Windows (new stamp every boot)
    but permanent under Wine, and each one costs the product scan three 5 minute timeouts while
    NA shows an empty library.
    """
    root = config.drive_c(prefix) / "ProgramData/boost_interprocess"
    if not root.is_dir():
        return []
    candidates = [f for f in root.glob("*/*.lock") if f.is_file()]
    if not candidates:
        return []
    held: set[str] = set()
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            for fd in os.scandir(f"/proc/{entry.name}/fd"):
                try:
                    held.add(os.readlink(fd.path))
                except OSError:
                    pass
        except OSError:
            continue
    return [f for f in candidates if str(f) not in held]


def clear_stale_locks(prefix: Path) -> int:
    removed = 0
    for f in stale_locks(prefix):
        try:
            f.unlink()
            removed += 1
        except OSError:
            pass
    return removed


def start(wine: Wine, prefix: Path, *, timeout: float = 90) -> str | None:
    """`net start` the service and wait for the daemon process. Started directly the daemon dies at
    the service controller handshake.
    """
    info("starting NTK daemon service...")
    env = wine.env()
    if "NI_WINE_DEBUG" not in os.environ:
        env["WINEDEBUG"] = "-all"
    # a file, not a pipe: the services Wine spawns inherit net.exe's stdio
    # and would keep a pipe open long after net.exe exited
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8", errors="replace") as out:
        status: str
        try:
            result = subprocess.run(
                [wine.wine, "net", "start", SERVICE],
                env=env,
                timeout=timeout,
                stdout=out,
                stderr=subprocess.STDOUT,
            )
            status = f"exited with {result.returncode}"
        except subprocess.TimeoutExpired:
            status = f"did not return within {timeout:.0f}s"
        out.seek(0)
        output = " ".join(out.read().split())

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if running(prefix):
            return None
        time.sleep(0.5)

    if "version mismatch" in output:
        return (
            f"the prefix is already running under a different Wine build than {wine.wine} "
            '("version mismatch"); close the program using it (your DAW\'s plugins?) or run '
            "with WINE= pointing at that build"
        )
    detail = f": {output[:200]}" if output else ""
    return f"`net start {SERVICE}` {status} and no NTKDaemon.exe process appeared{detail}"


def ensure(wine: Wine, prefix: Path) -> str | None:
    """Daemon installed, registered and running, or a description of what's wrong."""
    if not config.na_exe(prefix).is_file():
        return None  # nothing to guard yet, setup installs both
    problem = foreign_problem(prefix)
    if problem:
        return problem
    if not installed(prefix) or not service_registered(prefix):
        what = "not installed" if not installed(prefix) else "not registered as a service"
        info(f"NTK daemon is {what}, installing it")
        problem = install(wine, prefix)
        if problem:
            return f"{problem}\n{_STUCK_SCREEN}."
    if running(prefix):
        return None
    removed = clear_stale_locks(prefix)
    if removed:
        info(f"removed {removed} stale lock file(s) left by a killed NI process")
    problem = start(wine, prefix)
    if problem:
        return (
            f"{problem}.\n{_STUCK_SCREEN}. Check {LOG_DIR_WIN}\\NTK\\daemon.log in "
            f"the prefix and run `ni doctor`."
        )
    return None


# --- Diagnostics -----------------------------------------------------------


def probe_elevation(wine: Wine, prefix: Path, *, timeout: float = 30) -> bool | None:
    """Replay NA's own daemon install call (the cmd.exe/powershell.exe line sudo-prompt runs). Needs
    a TTY, pwsh's console host crashes without one, returns None then.
    """
    if not sys.stdin.isatty():
        return None
    user = config.wine_user(prefix)
    tag = f"ni-wine-probe-{uuid.uuid4().hex[:8]}"
    unix_dir = config.drive_c(prefix) / "users" / user / "Temp" / tag
    win_dir = rf"C:\users\{user}\Temp\{tag}"
    try:
        unix_dir.mkdir(parents=True)
        (unix_dir / "command.bat").write_bytes(b"@echo off\r\necho ok\r\n")
        (unix_dir / "execute.bat").write_bytes(
            (
                "@echo off\r\n"
                f'call "{win_dir}\\command.bat" > "{win_dir}\\stdout" 2> "{win_dir}\\stderr"\r\n'
                f'(echo %ERRORLEVEL%) > "{win_dir}\\status"\r\n'
            ).encode()
        )
        # exactly what @vscode/sudo-prompt runs through child_process.exec
        command = (
            f"powershell.exe Start-Process -FilePath \"'{win_dir}\\execute.bat'\" "
            "-WindowStyle hidden -Verb runAs"
        )
        env = wine.env({"WINEDEBUG": "-all"})
        try:
            subprocess.run(
                [wine.wine, "cmd.exe", "/d", "/s", "/c", command],
                env=env,
                timeout=timeout,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except subprocess.TimeoutExpired:
            return False
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if (unix_dir / "status").is_file():
                return True
            time.sleep(0.5)
        return False
    except OSError:
        return None
    finally:
        for name in ("command.bat", "execute.bat", "stdout", "stderr", "status"):
            (unix_dir / name).unlink(missing_ok=True)
        try:
            unix_dir.rmdir()
        except OSError:
            pass
