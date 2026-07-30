"""Install, update, and uninstall Kontakt 8 in the Wine prefix.

Kontakt's own installer fails under Wine, so we extract its payload
ourselves (see extract.py) and copy the resulting drive_c tree into the
prefix.
"""

from __future__ import annotations

import shutil
import subprocess
import time
from pathlib import Path

from . import config
from .browser import capture_download_url
from .extract import extract_kontakt8
from .util import download, guarded_rmtree, info
from .wine import Wine


def _zip_path() -> Path:
    return config.cache_dir() / config.KONTAKT8_ZIP_NAME


def _stage_dir() -> Path:
    return config.cache_dir() / "kontakt8" / "stage"


def _obtain_zip(url: str | None, *, reuse_cached: bool) -> Path:
    zip_path = _zip_path()
    if url is None:
        if reuse_cached and zip_path.exists():
            info(f"using previously downloaded {zip_path.name}")
            return zip_path
        url = capture_download_url()
    info("downloading Kontakt 8...")
    return download(url, zip_path, label="Kontakt 8 installer")


def _copy_into_prefix(prefix: Path, stage: Path) -> None:
    info("copying to Wine prefix...")
    config.drive_c(prefix).mkdir(parents=True, exist_ok=True)
    shutil.copytree(stage, config.drive_c(prefix), dirs_exist_ok=True)


def _wait_for_native_access_exit() -> None:
    running = subprocess.run(
        ["pgrep", "-f", r"Native Access\.exe"], stdout=subprocess.DEVNULL
    )
    if running.returncode == 0:
        info("Native Access is running — close it to continue...")
        while (
            subprocess.run(
                ["pgrep", "-f", r"Native Access\.exe"], stdout=subprocess.DEVNULL
            ).returncode
            == 0
        ):
            time.sleep(2)
        # On quit, NA may spawn its self-updater; give it a moment before
        # we take the wineserver down.
        info("Native Access closed, waiting for Wine to settle...")
        time.sleep(5)


def _remove_kontakt_files(prefix: Path, *, include_product_json: bool) -> None:
    for rel in config.KONTAKT8_PREFIX_PATHS:
        guarded_rmtree(config.drive_c(prefix) / rel)
    if include_product_json:
        (config.drive_c(prefix) / config.KONTAKT8_PRODUCT_JSON).unlink(missing_ok=True)


def install(prefix: Path, url: str | None = None) -> None:
    if config.kontakt8_exe(prefix).is_file():
        info("Kontakt 8 is already installed — use `ni kontakt8 update` to update")
        return

    zip_path = _obtain_zip(url, reuse_cached=False)
    stage = _stage_dir()
    guarded_rmtree(stage)

    info("extracting...")
    extract_kontakt8(zip_path, stage, update=False)

    Wine(prefix).kill_server()
    _copy_into_prefix(prefix, stage)

    info("cleaning up...")
    guarded_rmtree(stage)
    zip_path.unlink(missing_ok=True)
    info("Kontakt 8 installed")


def update(prefix: Path, url: str | None = None) -> None:
    zip_path = _obtain_zip(url, reuse_cached=True)
    stage = _stage_dir()
    guarded_rmtree(stage)

    info("extracting (update, overwrite enabled)...")
    extract_kontakt8(zip_path, stage, update=True)

    _wait_for_native_access_exit()
    Wine(prefix).kill_server()

    info("removing old Kontakt 8 files...")
    _remove_kontakt_files(prefix, include_product_json=False)

    _copy_into_prefix(prefix, stage)

    info("cleaning up...")
    guarded_rmtree(stage)
    zip_path.unlink(missing_ok=True)
    info("Kontakt 8 updated")


def uninstall(prefix: Path) -> None:
    info("removing Kontakt 8 files from the Wine prefix...")
    _remove_kontakt_files(prefix, include_product_json=True)
    info("Kontakt 8 uninstalled")
