"""Keep the NTK daemon installed, registered, and running.

Native Access talks to its NTK daemon (a Windows service) over ZeroMQ.  At
startup it asks the daemon for its version; if nothing answers it tries to
read the installed version with `wmic datafile … get Version`, which Wine's
wmic does not implement, concludes "Daemon version is undefined" and
reinstalls the daemon through @vscode/sudo-prompt:

    cmd.exe /d /s /c "powershell.exe Start-Process
        -FilePath "'C:\\users\\…\\Temp\\<uuid>\\execute.bat'" -WindowStyle hidden -Verb runAs"

With the profile.ps1 of the winetricks PowerShell wrapper that call fails
before any elevation happens: its Start-Process shim receives the path with
the quotes still in it, throws "File not found", powershell.exe exits
non-zero, sudo-prompt reports "User did not grant permission." and Native
Access sits on *Please grant permission to Native Access to install
dependencies* forever.  (The elevate.exe in NA's resources is unrelated —
electron-updater uses it for NA's own self-update — and Wine never reads
EnableLUA.)  ni-wine replaces that profile (see powershell.py), which makes
NA's own path work again, but the daemon should still be up *before* Native
Access starts: that skips a 30 s reinstall on every launch and does not
depend on PowerShell at all.  This module makes it a guarantee rather than
a hope: install the daemon from the installer NA ships when it is missing,
start the service, verify it is actually running, and report a usable error
instead of letting NA hang.
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


def running(prefix: Path) -> bool:
    """True if an NTKDaemon.exe belonging to *prefix* is running.

    pgrep alone would also match a daemon in some other Wine prefix; the
    process environment (Wine keeps WINEPREFIX in it) tells them apart.
    Unreadable environments count as a match, erring on the side of "up".
    """
    result = subprocess.run(
        ["pgrep", "-f", r"NTKDaemon\.exe"], capture_output=True, text=True
    )
    if result.returncode != 0:
        return False
    wanted = str(prefix.expanduser().resolve())
    for pid in result.stdout.split():
        try:
            environ = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
        except OSError:
            return True
        for entry in environ:
            if entry.startswith(b"WINEPREFIX="):
                value = entry[len(b"WINEPREFIX="):].decode(errors="replace")
                if str(Path(value).expanduser().resolve()) == wanted:
                    return True
                break
        else:
            # No WINEPREFIX in the environment: that daemon lives in ~/.wine.
            if wanted == str((Path.home() / ".wine").resolve()):
                return True
    return False


def find_installer(prefix: Path) -> Path | None:
    """The NTKDaemon installer Native Access ships in its resources."""
    ntk_dir = config.ntk_installer_dir(prefix)
    if not ntk_dir.is_dir():
        return None
    candidates = sorted(ntk_dir.glob("NTKDaemon *.exe"))
    return candidates[-1] if candidates else None


# --- Actions ---------------------------------------------------------------


def install(wine: Wine, prefix: Path, *, display: str | None = None) -> str | None:
    """Run NA's bundled NTKDaemon installer silently and verify the result.

    Returns a problem description, or None on success.  The installer (a WiX
    bundle) registers the service and starts it; the wineserver is shut down
    afterwards so the registry hits disk and can be checked.
    """
    installer = find_installer(prefix)
    if installer is None:
        return (
            f"NTKDaemon installer not found under {config.ntk_installer_dir(prefix)} "
            "— is Native Access installed?  Run `ni setup`."
        )
    info(f"installing NTK daemon from {installer.name}...")
    result = wine.run([str(installer), "/s"], display=display)
    wine.kill_server()
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
            + f".  Retry with NI_WINE_DEBUG=1 to see Wine's output; the "
            f"installer log (if any) is under {LOG_DIR_WIN}\\NTK in the prefix."
        )
    return None


def start(wine: Wine, prefix: Path, *, timeout: float = 90) -> str | None:
    """Start the service and wait until the daemon process is actually up.

    The daemon must go through the service manager — executed directly it
    dies at the service-controller handshake.  `net start` blocks until the
    service reports running; the process check afterwards catches a service
    that reported running and then died.
    """
    info("starting NTK daemon service...")
    env = wine.env()
    if "NI_WINE_DEBUG" not in os.environ:
        env["WINEDEBUG"] = "-all"
    # Output goes to a file, not a pipe: the service processes Wine spawns
    # inherit net.exe's stdio and would keep a pipe open (and us blocked)
    # long after net.exe itself has exited.
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

    detail = f": {output[:200]}" if output else ""
    return f"`net start {SERVICE}` {status} and no NTKDaemon.exe process appeared{detail}"


def ensure(wine: Wine, prefix: Path) -> str | None:
    """Make sure the daemon is installed, registered and running.

    Returns None when it is, otherwise a description of what went wrong and
    what to do about it (callers decide whether that is fatal).
    """
    if not config.na_exe(prefix).is_file():
        return None  # nothing to guard yet; setup will install both
    if not installed(prefix) or not service_registered(prefix):
        what = "not installed" if not installed(prefix) else "not registered as a service"
        info(f"NTK daemon is {what} — installing it")
        problem = install(wine, prefix)
        if problem:
            return f"{problem}\n{_STUCK_SCREEN}."
    if running(prefix):
        return None
    problem = start(wine, prefix)
    if problem:
        return (
            f"{problem}.\n{_STUCK_SCREEN}.  Check {LOG_DIR_WIN}\\NTK\\daemon.log in "
            f"the prefix and run `ni doctor`."
        )
    return None


# --- Diagnostics -----------------------------------------------------------


def probe_elevation(wine: Wine, prefix: Path, *, timeout: float = 30) -> bool | None:
    """Replay Native Access's own daemon-install call and see if it works.

    Builds the same batch files sudo-prompt writes and runs the identical
    cmd.exe/powershell.exe command line.  Success means the batch ran (its
    status file appeared).  Needs a terminal: pwsh's console host crashes
    without one, which would be a false negative, so returns None (skipped)
    when stdin is not a TTY.
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
        # Byte-for-byte what @vscode/sudo-prompt runs through child_process.exec.
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
