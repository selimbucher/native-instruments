"""Create the Wine prefix and install Native Access into it."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from . import config, daemon, msishim
from .msvcp140 import fix_msvcp140
from .desktop import ensure_url_handler
from .powershell import install_profile
from .ui import Progress
from .util import die, download, info, warn
from .wine import Wine, apply_prefix_tweaks, hidden_display

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
    result = subprocess.run(
        [winetricks, "--unattended", verb],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if result.returncode != 0:
        die(f"winetricks {verb} failed (exit {result.returncode})")


def run_setup(prefix: Path, *, ui: bool = False) -> None:
    wine = Wine(prefix)
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

        progress.step("Applying prefix tweaks...", 12)
        apply_prefix_tweaks(wine)

        progress.step("Cleaning up home folder symlinks...", 18)
        _unlink_home_symlinks(prefix)

        progress.step("Installing vcrun2022...", 25)
        _winetricks(wine, "vcrun2022", display)

        progress.step("Installing PowerShell...", 40)
        _winetricks(wine, "powershell", display)
        # The wrapper's own profile.ps1 hangs the Native Access installer and
        # breaks NA's daemon install; ours answers both correctly.
        if not install_profile(prefix):
            die(f"PowerShell profile not installed — is pwsh.exe missing under {prefix}?")

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
