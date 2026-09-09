"""Keep ni-wine's PowerShell profile installed in the prefix.

winetricks' `powershell` verb installs PowerShell 7 plus a powershell.exe
wrapper whose profile.ps1 overrides Get-CimInstance and Start-Process in ways
that break Native Access (see data/profile.ps1 for the details).  ni-wine
replaces that profile with its own; the wrapper strips -NoProfile, so the
replacement is in effect for every PowerShell call Native Access makes.
"""

from __future__ import annotations

import importlib.resources
from pathlib import Path

from . import config

PROFILE_REL = "Program Files/PowerShell/7/profile.ps1"
BACKUP_SUFFIX = ".winetricks"


def profile_path(prefix: Path) -> Path:
    return config.drive_c(prefix) / PROFILE_REL


def wanted_profile() -> str:
    return importlib.resources.files("ni_wine").joinpath("data", "profile.ps1").read_text()


def pwsh_installed(prefix: Path) -> bool:
    return profile_path(prefix).parent.joinpath("pwsh.exe").is_file()


def profile_current(prefix: Path) -> bool:
    try:
        return profile_path(prefix).read_text() == wanted_profile()
    except OSError:
        return False


def install_profile(prefix: Path) -> bool:
    """Write ni-wine's profile.ps1 if it differs.  Returns True if changed.

    The winetricks original is kept once as profile.ps1.winetricks.
    """
    if not pwsh_installed(prefix) or profile_current(prefix):
        return False
    target = profile_path(prefix)
    backup = target.with_name(target.name + BACKUP_SUFFIX)
    if target.exists() and not backup.exists():
        backup.write_bytes(target.read_bytes())
    target.write_text(wanted_profile())
    return True
