"""Paths and constants shared across commands."""

from __future__ import annotations

import os
from pathlib import Path

APP_NAME = "ni-wine"

NA_INSTALLER_URL = (
    "https://www.native-instruments.com/fileadmin/downloads/Native-Access_2.exe"
)
VCREDIST_URL = "https://aka.ms/vs/17/release/vc_redist.x64.exe"
NI_DOWNLOADS_PAGE = (
    "https://www.native-instruments.com/en/account/downloads/"
    "0e504595-40d8-4982-978e-a242f036912d"
)
KONTAKT8_ZIP_NAME = "Kontakt_8_Installer.zip"

# The URL scheme Native Access registers for its browser-login callback.
URL_SCHEME = "native-access"
DESKTOP_FILE_NAME = "native-access.desktop"

NA_EXE_WIN = r"C:\Program Files\Native Instruments\Native Access\Native Access.exe"


def default_prefix() -> Path:
    return Path(os.environ.get("NI_WINE_PREFIX", str(Path.home() / ".wine-ni")))


def cache_dir() -> Path:
    base = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache")))
    return base / APP_NAME


def state_dir() -> Path:
    base = Path(
        os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local" / "state"))
    )
    return base / APP_NAME


def data_home() -> Path:
    return Path(
        os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share"))
    )


# --- Locations inside the Wine prefix -------------------------------------


def drive_c(prefix: Path) -> Path:
    return prefix / "drive_c"


def na_exe(prefix: Path) -> Path:
    return drive_c(prefix) / "Program Files/Native Instruments/Native Access/Native Access.exe"


def ntk_daemon_exe(prefix: Path) -> Path:
    return drive_c(prefix) / "Program Files/Common Files/Native Instruments/NTK/NTKDaemon.exe"


def ntk_installer_dir(prefix: Path) -> Path:
    return drive_c(prefix) / "Program Files/Native Instruments/Native Access/resources/daemon/win"


def wine_user(prefix: Path) -> str:
    """The Windows user name inside the prefix."""
    users = drive_c(prefix) / "users"
    if users.is_dir():
        for entry in sorted(users.iterdir()):
            if entry.name != "Public" and entry.is_dir():
                return entry.name
    return os.environ.get("USER", "user")


def na_roaming_dir(prefix: Path) -> Path:
    """Native Access's Electron app-data directory (window state, prefs)."""
    return (
        drive_c(prefix)
        / "users"
        / wine_user(prefix)
        / "AppData/Roaming/Native Instruments/Native Access"
    )


def updater_dir(prefix: Path) -> Path:
    """electron-updater's download cache for Native Access self-updates."""
    return (
        drive_c(prefix)
        / "users"
        / wine_user(prefix)
        / "AppData/Local/nativeaccess2-updater"
    )


KONTAKT8_PREFIX_PATHS = [
    "Program Files/Native Instruments/Kontakt 8",
    "Program Files/Common Files/Native Instruments/Kontakt 8",
    "Program Files/Common Files/VST3/Kontakt 8.vst3",
]
KONTAKT8_PRODUCT_JSON = (
    "users/Public/Documents/Native Instruments/installed_products/Kontakt 8.json"
)


def kontakt8_exe(prefix: Path) -> Path:
    return drive_c(prefix) / "Program Files/Native Instruments/Kontakt 8/Kontakt 8.exe"
