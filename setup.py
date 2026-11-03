"""Build hook: compile the msi shim and the NI controller support into the package (pip/pipx
installs need 32- and 64-bit mingw compilers). Distribution packages build them themselves, see
shim/README.md and hardware/README.md."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py

HERE = Path(__file__).resolve().parent
SHIM = HERE / "shim"
HARDWARE = HERE / "hardware"
DATA = HERE / "src/ni_wine/data"


def _compiler(name: str, purpose: str) -> str:
    compiler = shutil.which(name)
    if not compiler:
        raise SystemExit(f"ni-wine needs a mingw-w64 compiler ({name}) to build {purpose}")
    return compiler


class build_py_with_binaries(build_py):
    def run(self) -> None:
        if not (DATA / "msi_shim32.dll").is_file():
            compiler = _compiler(
                "i686-w64-mingw32-gcc",
                "the msi shim that lets Native Access install Kontakt, see shim/README.md",
            )
            subprocess.run(
                ["make", "-C", str(SHIM), f"CC={compiler}", f"PYTHON={sys.executable}"],
                check=True,
            )
            shutil.copy2(SHIM / "msi_shim32.dll", DATA / "msi_shim32.dll")
        if not (DATA / "nimk3.sys").is_file():
            compiler = _compiler(
                "x86_64-w64-mingw32-gcc",
                "the NI controller support, see hardware/README.md",
            )
            subprocess.run(
                ["make", "-C", str(HARDWARE), "install", f"CC={compiler}", f"DATADIR={DATA}"],
                check=True,
            )
        super().run()


setup(cmdclass={"build_py": build_py_with_binaries})
