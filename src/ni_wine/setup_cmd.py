"""Create the Wine prefix and install Native Access into it."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from . import config, daemon, msishim
from .msvcp140 import fix_msvcp140
from .desktop import ensure_url_handler
from .powershell import install_powershell, install_profile
from .ui import Progress
from .util import die, download, guarded_rmtree, info, warn
from .wine import (
    Wine,
    apply_prefix_tweaks,
    check_wine_version,
    foreign_prefix_users,
    hidden_display,
    prefix_booted_by_ni_wine,
    record_prefix_build,
)

# Windows special folders Wine symlinks into $HOME; we replace them with
# real directories so installers can't touch the actual home folder.
_USER_DIR_LINKS = [
    "Desktop",
    "Documents",
    "My Documents",
    "Downloads",
    "Music",
    "My Music",
    "Pictures",
    "My Pictures",
    "Videos",
    "My Videos",
    "Templates",
]


def _unlink_home_symlinks(prefix: Path) -> None:
    user_dir = config.drive_c(prefix) / "users" / config.wine_user(prefix)
    for name in _USER_DIR_LINKS:
        link = user_dir / name
        if link.is_symlink():
            link.unlink()
            link.mkdir(parents=True, exist_ok=True)
    (config.drive_c(prefix) / "users/Public/Downloads").mkdir(
        parents=True, exist_ok=True
    )


def _require_win32(prefix: Path) -> None:
    # A 64-bit-only Wine build (Ubuntu's wine64 without wine32, Nix's
    # wine64Packages) boots a prefix whose syswow64 has no PE binaries;
    # winetricks then dies at its first 32-bit call with an opaque exit 1.
    if (config.drive_c(prefix) / "windows/syswow64/regedit.exe").is_file():
        return
    die(
        "this Wine build has no 32-bit support (C:\\windows\\syswow64 is "
        "empty), which the VC++ runtime installer needs.\n"
        "Install a WoW64-capable build and run `ni reinstall`:\n"
        "  Arch: wine-staging    Debian/Ubuntu: winehq-staging, or wine32\n"
        "  NixOS: wineWow64Packages.<variant>"
    )


def _is_windows_exe(path: Path) -> bool:
    try:
        with open(path, "rb") as handle:
            return handle.read(2) == b"MZ"
    except OSError:
        return False


def _winetricks(wine: Wine, verb: str, display: str | None) -> None:
    winetricks = shutil.which("winetricks")
    if not winetricks:
        die("winetricks not found on PATH")
    env = wine.env({"WINE": wine.wine})
    if display is not None:
        env["DISPLAY"] = display
        env.pop("WAYLAND_DISPLAY", None)
    # Silent on success, but keep the output: winetricks fails for wildly
    # different reasons (64-bit-only Wine, Wine regressions, dead mirrors)
    # and a bare exit code has proven undiagnosable in bug reports.
    log_path = config.cache_dir() / f"winetricks-{verb}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with open(log_path, "wb") as log:
        result = subprocess.run(
            [winetricks, "--unattended", verb],
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
    if result.returncode != 0:
        tail = log_path.read_text(errors="replace").splitlines()
        print("\n".join(tail[-15:]), file=sys.stderr, flush=True)
        die(f"winetricks {verb} failed (exit {result.returncode}) "
            f"— full log: {log_path}")


def _discard_incomplete_prefix(wine: Wine) -> None:
    """Rebuild from scratch if a previous setup was interrupted.

    A setup killed partway leaves the prefix booted (the build marker is
    written right after wineboot) but without Native Access.  Re-running
    the Wine steps over that half-built prefix fails in ways that are hard
    to diagnose -- msiexec reports success while installing nothing, and
    even a manual VC++ redist install hangs -- so the only reliable
    recovery is to wipe and start clean.  A prefix that already has Native
    Access, or one ni-wine never booted (someone pointed NI_WINE_PREFIX at
    an existing Wine prefix), is left untouched.
    """
    prefix = wine.prefix
    if not prefix_booted_by_ni_wine(prefix) or config.na_exe(prefix).is_file():
        return
    hosts = foreign_prefix_users(prefix)
    if hosts:
        die(
            f"{len(hosts)} plugin host(s) (yabridge) are using {prefix} — "
            "close your DAW, then run `ni setup` again."
        )
    warn("a previous setup was interrupted — rebuilding the Wine prefix from scratch")
    wine.kill_server()
    try:
        wine.wait_server()
    except subprocess.TimeoutExpired:
        die(
            "Wine processes from the interrupted setup are still running — "
            "close them and run `ni setup` again"
        )
    guarded_rmtree(prefix)


def run_setup(prefix: Path, *, ui: bool = False) -> None:
    wine = Wine(prefix)
    check_wine_version(wine)
    _discard_incomplete_prefix(wine)
    config.drive_c(prefix).mkdir(parents=True, exist_ok=True)

    with Progress("Native Instruments Setup", enabled=ui) as progress, \
            hidden_display() as display:

        progress.step("Initializing Wine prefix...", 5)
        # DISPLAY="" keeps wineboot's "updating configuration" dialog off
        # the user's desktop when no hidden display is available.
        wine.run(
            ["wineboot", "-i"],
            extra_env={"WINEDLLOVERRIDES": "mscoree,mshtml="},
            display=display or "",
        )
        _require_win32(prefix)
        record_prefix_build(wine)

        progress.step("Applying prefix tweaks...", 12)
        apply_prefix_tweaks(wine)

        progress.step("Cleaning up home folder symlinks...", 18)
        _unlink_home_symlinks(prefix)

        progress.step("Installing vcrun2022...", 25)
        _winetricks(wine, "vcrun2022", display)

        progress.step("Installing PowerShell...", 40)
        install_powershell(wine, display)
        # The wrapper's own profile.ps1 hangs the Native Access installer and
        # breaks NA's daemon install; ours answers both correctly.  A no-op
        # (profile already current) is fine — install_powershell has already
        # guaranteed pwsh.exe is present.
        install_profile(prefix)

        progress.step("Downloading Native Access...", 55)
        installer_path = config.cache_dir() / "Native-Access_2.exe"
        if installer_path.exists() and not _is_windows_exe(installer_path):
            # A stale HTML page cached under the .exe name would otherwise be
            # kept forever by the If-Modified-Since logic.
            installer_path.unlink()
        installer = download(
            config.NA_INSTALLER_URL, installer_path, label="Native Access installer"
        )
        if not _is_windows_exe(installer):
            installer.unlink(missing_ok=True)
            die(
                f"the download from {config.NA_INSTALLER_URL} is not a Windows "
                "executable (NI probably moved the installer again) — please "
                "report this at https://github.com/selimbucher/native-instruments/issues"
            )

        progress.step("Installing Native Access...", 65)
        # NSIS silent mode: no wizard, and the compatibility warning dialog
        # is skipped.  The payload still needs a window driver (fails with
        # DISPLAY=""), so a display, hidden or real, stays attached.
        result = wine.run([str(installer), "/S"], display=display)
        wine.kill_server()
        if not config.na_exe(prefix).is_file():
            die(
                "the Native Access installer finished (exit code "
                f"{result.returncode}) without installing Native Access; run "
                "again with NI_WINE_DEBUG=1 to see Wine's output"
            )

        progress.step("Installing NTKDaemon...", 78)
        # Native Access cannot install the daemon itself under Wine (its
        # PowerShell-based elevation fails), so this step must succeed here.
        problem = daemon.install(wine, prefix, display=display)
        if problem:
            die(problem)

        progress.step("Fixing msvcp140 DLLs...", 90)
        fix_msvcp140(wine)

        progress.step("Installing the Kontakt installer hook...", 93)
        problem = msishim.ensure(wine, prefix, quiet=True)
        if problem:
            warn(f"Kontakt installer hook not installed: {problem}")

        progress.step("Registering login URL handler...", 96)
        if not ensure_url_handler(prefix, quiet=True):
            warn("could not register the native-access:// URL handler")

        progress.step("Done!", 100)
    info("setup complete — run `native-access` (or `ni launch`) to start")
