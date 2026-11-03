from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from . import config, daemon, hardware, msishim
from .desktop import ensure_url_handler
from .msvcp140 import fix_msvcp140
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

# Wine symlinks these into $HOME, replace them with real directories so
# installers can't touch the home folder
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
    (config.drive_c(prefix) / "users/Public/Downloads").mkdir(parents=True, exist_ok=True)


def _require_win32(prefix: Path) -> None:
    # a 64-bit-only Wine (Ubuntu wine64 without wine32, Nix wine64Packages)
    # leaves syswow64 without PE binaries, winetricks then dies with exit 1
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
    # keep the log: winetricks fails for all kinds of reasons (64-bit-only
    # Wine, Wine regressions, dead mirrors) and the exit code alone is useless
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
        die(f"winetricks {verb} failed (exit {result.returncode}), full log: {log_path}")


def _discard_incomplete_prefix(wine: Wine) -> None:
    """Wipe a prefix ni-wine booted but never got Native Access into: setup was interrupted, and
    re-running the Wine steps on it fails in odd ways (msiexec reports success and installs
    nothing). Foreign prefixes are left alone.
    """
    prefix = wine.prefix
    if not prefix_booted_by_ni_wine(prefix) or config.na_exe(prefix).is_file():
        return
    hosts = foreign_prefix_users(prefix)
    if hosts:
        die(
            f"{len(hosts)} plugin host(s) (yabridge) are using {prefix}, "
            "close your DAW and run `ni setup` again."
        )
    warn("a previous setup was interrupted, rebuilding the Wine prefix")
    wine.kill_server()
    try:
        wine.wait_server()
    except subprocess.TimeoutExpired:
        die("Wine processes from the interrupted setup are still running, close them first")
    guarded_rmtree(prefix, prefix=prefix)


def run_setup(prefix: Path, *, ui: bool = False) -> None:
    wine = Wine(prefix)
    check_wine_version(wine)
    _discard_incomplete_prefix(wine)
    # resolve(): the prefix may be a symlink to another disk whose target is
    # missing (never created, or just wiped), mkdir through a dangling link fails
    config.drive_c(prefix.resolve()).mkdir(parents=True, exist_ok=True)

    with Progress("Native Instruments Setup", enabled=ui) as progress, hidden_display() as display:
        progress.step("Initializing Wine prefix...", 5)
        # DISPLAY="" keeps wineboot's "updating configuration" dialog off the desktop
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
        # the winetricks profile.ps1 hangs the NA installer and breaks the
        # daemon install, ours doesn't (False when already current is fine)
        install_profile(prefix)

        progress.step("Downloading Native Access...", 55)
        installer_path = config.cache_dir() / "Native-Access_2.exe"
        if installer_path.exists() and not _is_windows_exe(installer_path):
            # a cached HTML error page would be kept forever by If-Modified-Since
            installer_path.unlink()
        installer = download(
            config.NA_INSTALLER_URL, installer_path, label="Native Access installer"
        )
        if not _is_windows_exe(installer):
            installer.unlink(missing_ok=True)
            die(
                f"the download from {config.NA_INSTALLER_URL} is not a Windows "
                "executable (NI probably moved the installer again), please "
                "report this at https://github.com/selimbucher/native-instruments/issues"
            )

        progress.step("Installing Native Access...", 65)
        # NSIS silent mode: no wizard, no compatibility warning. Still needs
        # a display, hidden or real (fails with DISPLAY="")
        result = wine.run([str(installer), "/S"], display=display)
        wine.kill_server()
        if not config.na_exe(prefix).is_file():
            die(
                "the Native Access installer finished (exit code "
                f"{result.returncode}) without installing Native Access; run "
                "again with NI_WINE_DEBUG=1 to see Wine's output"
            )

        progress.step("Installing NTKDaemon...", 78)
        # NA can't install the daemon itself under Wine (its PowerShell
        # elevation fails), so it has to happen here
        problem = daemon.install(wine, prefix, display=display)
        if problem:
            die(problem)

        progress.step("Fixing msvcp140 DLLs...", 90)
        fix_msvcp140(wine)

        progress.step("Installing the Kontakt installer hook...", 93)
        problem = msishim.ensure(wine, prefix, quiet=True)
        if problem:
            warn(f"Kontakt installer hook not installed: {problem}")

        progress.step("Installing NI controller support...", 94)
        problem = hardware.ensure(wine, prefix, quiet=True)
        if problem:
            warn(f"NI controller support not installed: {problem}")

        progress.step("Registering login URL handler...", 96)
        if not ensure_url_handler(prefix, quiet=True):
            warn("could not register the native-access:// URL handler")

        progress.step("Done!", 100)
    info("setup complete, run `native-access` (or `ni launch`) to start")
