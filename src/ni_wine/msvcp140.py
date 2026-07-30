"""Replace Wine's msvcp140 stubs with the genuine Microsoft runtime DLLs.

Wine's builtin msvcp140.dll is missing symbols Kontakt needs.  We pull the
real DLLs out of the official VC++ 2022 redistributable and force them via
DLL overrides.  vcruntime140 must come along: the native msvcp140 links
against it at the PE level and rejects Wine's builtin stub.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
from pathlib import Path

from . import config
from .util import die, download, info, warn
from .wine import Wine

_DLLS = [
    "msvcp140",
    "msvcp140_1",
    "msvcp140_2",
    "msvcp140_atomic_wait",
    "msvcp140_codecvt_ids",
    "concrt140",
    "vcruntime140",
    "vcruntime140_1",
    "vcruntime140_threads",
]


def fix_msvcp140(wine: Wine) -> None:
    system32 = config.drive_c(wine.prefix) / "windows/system32"
    if not system32.is_dir():
        die("Wine prefix not initialized — run `ni setup` first")
    if not shutil.which("cabextract"):
        die("cabextract not found (needed to unpack the VC++ redistributable)")

    with tempfile.TemporaryDirectory(prefix="ni-wine-vcredist-") as tmp_str:
        tmp = Path(tmp_str)
        info("downloading VC++ 2022 x64 redistributable...")
        redist = download(config.VCREDIST_URL, tmp / "vc_redist.x64.exe")

        info("extracting cabinets...")
        subprocess.run(
            ["cabextract", "-d", str(tmp / "stage1"), str(redist)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        inner_cab = None
        for candidate in sorted((tmp / "stage1").glob("a*")):
            listing = subprocess.run(
                ["cabextract", "-l", str(candidate)],
                capture_output=True,
                text=True,
            )
            if "msvcp140.dll_amd64" in listing.stdout:
                inner_cab = candidate
                break
        if inner_cab is None:
            die("could not find the amd64 DLL cabinet inside vc_redist.x64.exe")

        subprocess.run(
            ["cabextract", "-d", str(tmp / "stage2"), str(inner_cab)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        for dll in _DLLS:
            src = tmp / "stage2" / f"{dll}.dll_amd64"
            if src.is_file():
                info(f"installing {dll}.dll")
                shutil.copy2(src, system32 / f"{dll}.dll")
            else:
                warn(f"{dll}.dll not found in redistributable, skipping")

    info("setting DLL overrides...")
    for dll in _DLLS:
        wine.reg_add(r"HKCU\Software\Wine\DllOverrides", dll, "native,builtin")
    info("msvcp140 fix applied")
