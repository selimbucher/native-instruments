"""Build hook: compile the msi shim into the package when a 32-bit mingw
compiler is available (pip/pipx installs).  Distribution packages (flake,
PKGBUILD) build it explicitly instead; see shim/README.md."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py

HERE = Path(__file__).resolve().parent
SHIM = HERE / "shim"
TARGET = HERE / "src/ni_wine/data/msi_shim32.dll"


class build_py_with_shim(build_py):
    def run(self) -> None:
        if not TARGET.is_file():
            compiler = shutil.which("i686-w64-mingw32-gcc")
            if compiler:
                subprocess.run(
                    ["make", "-C", str(SHIM), f"CC={compiler}", f"PYTHON={sys.executable}"],
                    check=True,
                )
                shutil.copy2(SHIM / "msi_shim32.dll", TARGET)
            else:
                print(
                    "warning: i686-w64-mingw32-gcc not found — building without the msi "
                    "shim; Native Access will not be able to install Kontakt "
                    "(see shim/README.md)",
                    file=sys.stderr,
                )
        super().run()


setup(cmdclass={"build_py": build_py_with_shim})
