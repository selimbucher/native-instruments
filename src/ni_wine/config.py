from __future__ import annotations

import os
from pathlib import Path

APP_NAME = "ni-wine"

# The download button on native-instruments.com/pages/native-access. The old
# fileadmin URL redirects to an HTML page since the Shopify move (Sept 2026),
# storage.googleapis.com/ni-assets denies access since Oct 2026. Setup checks
# that it really got an exe.
NA_INSTALLER_URL = "https://assets.native-instruments.com/downloads/Native-Access_2.exe"
VCREDIST_URL = "https://aka.ms/vs/17/release/vc_redist.x64.exe"

# The URL scheme Native Access registers for its browser-login callback.
URL_SCHEME = "native-access"

# .desktop file id and icon name, also the window class of our dialogs (ui.py)
APP_ID = "native-access"
DESKTOP_FILE_NAME = f"{APP_ID}.desktop"

NA_EXE_WIN = r"C:\Program Files\Native Instruments\Native Access\Native Access.exe"


def default_prefix() -> Path:
    return Path(os.environ.get("NI_WINE_PREFIX", str(Path.home() / ".wine-ni")))


def cache_dir() -> Path:
    base = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache")))
    return base / APP_NAME


def state_dir() -> Path:
    base = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local" / "state")))
    return base / APP_NAME


def data_home() -> Path:
    return Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share")))


def config_home() -> Path:
    return Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))


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
    users = drive_c(prefix) / "users"
    if users.is_dir():
        for entry in sorted(users.iterdir()):
            if entry.name != "Public" and entry.is_dir():
                return entry.name
    return os.environ.get("USER", "user")


def na_roaming_dir(prefix: Path) -> Path:
    return (
        drive_c(prefix)
        / "users"
        / wine_user(prefix)
        / "AppData/Roaming/Native Instruments/Native Access"
    )


def updater_dir(prefix: Path) -> Path:
    return drive_c(prefix) / "users" / wine_user(prefix) / "AppData/Local/nativeaccess2-updater"


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
