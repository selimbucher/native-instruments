"""Build hook: compile the msi shim into the package (pip/pipx installs need a
32-bit mingw compiler).  Distribution packages (flake,
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
                raise SystemExit(
                    "ni-wine needs a 32-bit mingw-w64 compiler (i686-w64-mingw32-gcc) to "
                    "build the msi shim that lets Native Access install Kontakt; "
                    "see shim/README.md"
                )
        super().run()


setup(cmdclass={"build_py": build_py_with_shim})
