"""Install PowerShell into the prefix and keep ni-wine's profile in it.

This used to be winetricks' `powershell` verb (PowerShell 7 plus a
powershell.exe wrapper), but Ubuntu 24.04 ships a winetricks from before
the verb existed, and winetricks skips checksum enforcement in unattended
mode.  So ni-wine installs the same pieces itself: the pinned PowerShell 7
MSI and the wrapper exes the verb would have used.

The wrapper's own profile.ps1 overrides Get-CimInstance and Start-Process
in ways that break Native Access (see data/profile.ps1 for the details).
ni-wine writes its own profile instead; the wrapper strips -NoProfile, so
the replacement is in effect for every PowerShell call Native Access makes.
"""

from __future__ import annotations

import hashlib
import importlib.resources
import shutil
from pathlib import Path

from . import config
from .util import die, download, info
from .wine import Wine

PROFILE_REL = "Program Files/PowerShell/7/profile.ps1"
BACKUP_SUFFIX = ".winetricks"

_PWSH_VERSION = "7.4.11"
_PWSH_SHA256 = "9579011c463a3ad6abf890736a97e2fbba9a7b4e09ce851576ccf263e15bdc97"
_PWSH_MSI = f"PowerShell-{_PWSH_VERSION}-win-x64.msi"
_PWSH_URL = (
    "https://github.com/PowerShell/PowerShell/releases/download/"
    f"v{_PWSH_VERSION}/{_PWSH_MSI}"
)
_WRAPPER_URL = (
    "https://codeberg.org/Synchro/powershell-wrapper-for-wine/releases/download/latest"
)
# On a 64-bit prefix system32 holds the 64-bit binaries, syswow64 the 32-bit.
_WRAPPER_EXES = [("powershell64.exe", "system32"), ("powershell32.exe", "syswow64")]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _fetch_msi() -> Path:
    msi = config.cache_dir() / _PWSH_MSI
    for retry_left in (True, False):
        download(_PWSH_URL, msi, label="PowerShell 7")
        if _sha256(msi) == _PWSH_SHA256:
            return msi
        msi.unlink()
        if retry_left:
            info("PowerShell MSI failed its checksum — downloading again...")
    die(f"the PowerShell download from {_PWSH_URL} keeps failing its checksum")


def install_powershell(wine: Wine, display: str | None = None) -> None:
    """Install PowerShell 7 and the powershell.exe wrapper into the prefix."""
    msi = _fetch_msi()
    result = wine.run(
        [
            "msiexec", "/quiet", "/i", str(msi),
            "ENABLE_PSREMOTING=0", "REGISTER_MANIFEST=1", "DISABLE_TELEMETRY=1",
            "USE_MU=0", "ENABLE_MU=0", "LAUNCHAPPONEXIT=0",
        ],
        display=display,
    )
    if result.returncode != 0:
        die(
            f"the PowerShell installer failed (msiexec exit {result.returncode})"
            " — run again with NI_WINE_DEBUG=1 to see Wine's output"
        )
    for name, system_dir in _WRAPPER_EXES:
        exe = download(f"{_WRAPPER_URL}/{name}", config.cache_dir() / name, label=name)
        with open(exe, "rb") as handle:
            if handle.read(2) != b"MZ":
                exe.unlink()
                die(f"the download of {name} is not a Windows executable")
        target = (
            config.drive_c(wine.prefix)
            / "windows" / system_dir / "WindowsPowerShell/v1.0/powershell.exe"
        )
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(exe, target)
    wine.reg_add(r"HKCU\Software\Wine\DllOverrides", "powershell.exe", "native")
    # msiexec can report success while installing nothing on a damaged
    # prefix; make sure the interpreter is really there before moving on.
    if not pwsh_installed(wine.prefix):
        die(
            "the PowerShell installer reported success but pwsh.exe is "
            f"missing under {wine.prefix} — run `ni reinstall`"
        )


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
