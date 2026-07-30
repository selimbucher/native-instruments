"""Create the Wine prefix and install Native Access into it."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from . import config
from .msvcp140 import fix_msvcp140
from .desktop import ensure_url_handler
from .ui import Progress
from .util import die, download, info, warn
from .wine import Wine, apply_prefix_tweaks, auto_dismiss, hidden_display

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
        wine.run(
            ["wineboot", "-i"],
            extra_env={"WINEDLLOVERRIDES": "mscoree,mshtml="},
            display=display,
        )

        progress.step("Applying prefix tweaks...", 12)
        apply_prefix_tweaks(wine)

        progress.step("Cleaning up home folder symlinks...", 18)
        _unlink_home_symlinks(prefix)

        progress.step("Installing vcrun2022...", 25)
        _winetricks(wine, "vcrun2022", display)

        progress.step("Installing PowerShell...", 40)
        _winetricks(wine, "powershell", display)

        progress.step("Downloading Native Access...", 55)
        installer = download(
            config.NA_INSTALLER_URL,
            config.cache_dir() / "Native-Access_2.exe",
            label="Native Access installer",
        )

        progress.step("Installing Native Access...", 65)
        # The installer pops a compatibility warning; auto-press Return on it.
        with auto_dismiss(display, "Warning", "Return"):
            wine.run([str(installer)], display=display)
        wine.kill_server()

        progress.step("Installing NTKDaemon...", 78)
        ntk_dir = config.ntk_installer_dir(prefix)
        ntk_installers = sorted(ntk_dir.glob("NTKDaemon *.exe")) if ntk_dir.is_dir() else []
        if not ntk_installers:
            die(f"NTKDaemon installer not found under {ntk_dir}")
        wine.run([str(ntk_installers[0]), "/s"], display=display)
        wine.kill_server()

        progress.step("Fixing msvcp140 DLLs...", 90)
        fix_msvcp140(wine)

        progress.step("Registering login URL handler...", 96)
        if not ensure_url_handler(quiet=True):
            warn("could not register the native-access:// URL handler")

        progress.step("Done!", 100)
    info("setup complete — run `native-access` (or `ni launch`) to start")
