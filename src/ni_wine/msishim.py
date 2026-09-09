"""Install and manage the msi shim that makes Native Access's own Kontakt
install work under Wine.

Kontakt's InstallAware installer hangs in Wine's MSI engine (see
shim/msi_shim.c for the mechanism).  ni-wine puts a small forwarding
msi.dll into the prefix's syswow64, keeps Wine's real msi next to it as
msi_wine.dll, and registers a DllOverride so that only the Kontakt installer
engine (`Kontakt 8 Setup PC.exe`) loads the shim — every other process keeps
using Wine's builtin.  When the installer calls MsiInstallProduct on the
Kontakt package, the shim runs a hook script (generated here) that calls
back into ni-wine, which lays the files out from the payload the installer
has already extracted, and the installer then finishes normally.  Native
Access sees an ordinary successful install.

Everything the shim does is driven by a config file next to it; ni-wine
writes it.  All state lives in three files under syswow64 (msi.dll,
msi_wine.dll, msi_shim.cfg), one registry key, and the hook script under
the state directory.  `remove()` puts the prefix back exactly as it was.
"""

from __future__ import annotations

import hashlib
import importlib.resources
import os
import shutil
import sys
from pathlib import Path

from . import config
from .util import info, warn
from .wine import Wine

ENGINE_EXE = "Kontakt 8 Setup PC.exe"
DIVERTED_PACKAGES = ("Kontakt 8 Setup PC.msi",)

SHIM_FILE = "msi.dll"
REAL_FILE = "msi_wine.dll"
CFG_FILE = "msi_shim.cfg"

_BUILTIN_MARKER = b"Wine builtin DLL"
_BUILTIN_MARKER_OFFSET = 64
_SHIM_MARKER = b"ni-wine-msi-shim/"

_OVERRIDE_KEY = rf"HKCU\Software\Wine\AppDefaults\{ENGINE_EXE}\DllOverrides"
_OVERRIDE_DATA = "native,builtin"


# --- locations -------------------------------------------------------------


def syswow64(prefix: Path) -> Path:
    return config.drive_c(prefix) / "windows/syswow64"


def shim_source() -> Path | None:
    """The shim binary shipped with this ni-wine build (or $NI_WINE_MSI_SHIM)."""
    override = os.environ.get("NI_WINE_MSI_SHIM")
    if override:
        return Path(override) if Path(override).is_file() else None
    try:
        candidate = importlib.resources.files("ni_wine") / "data" / "msi_shim32.dll"
        path = Path(str(candidate))
    except (ModuleNotFoundError, TypeError):
        return None
    return path if path.is_file() else None


def hook_script() -> Path:
    return config.state_dir() / "msi-hook.sh"


def result_file() -> Path:
    return config.state_dir() / "msi-hook.result"


def shim_log() -> Path:
    return config.state_dir() / "msi-shim.log"


def hook_log() -> Path:
    return config.state_dir() / "msi-hook.log"


def _win_path(path: Path) -> str:
    """Unix path -> the Z: drive path Wine sees."""
    return "Z:" + str(path.resolve()).replace("/", "\\")


# --- file identification (no Wine calls) -----------------------------------


def _read(path: Path, limit: int = 1 << 20) -> bytes:
    try:
        with open(path, "rb") as handle:
            return handle.read(limit)
    except OSError:
        return b""


def is_wine_builtin(path: Path) -> bool:
    data = _read(path, 128)
    return data[_BUILTIN_MARKER_OFFSET:_BUILTIN_MARKER_OFFSET + len(_BUILTIN_MARKER)] == _BUILTIN_MARKER


def is_shim(path: Path) -> bool:
    return _SHIM_MARKER in _read(path)


def _same_file(a: Path, b: Path) -> bool:
    return hashlib.sha256(_read(a)).digest() == hashlib.sha256(_read(b)).digest()


def _wine_builtin_msi(wine: Wine) -> Path | None:
    """Wine's own 32-bit msi.dll, for restoring a prefix whose copy is gone."""
    root = Path(wine.wine).resolve().parent.parent
    for candidate in (
        root / "lib/wine/i386-windows/msi.dll",
        root / "lib64/wine/i386-windows/msi.dll",
        root / "lib32/wine/i386-windows/msi.dll",
    ):
        if candidate.is_file() and is_wine_builtin(candidate):
            return candidate
    return None


# --- generated files -------------------------------------------------------


def _ni_command() -> tuple[list[str], str | None]:
    """How the hook re-enters *this* ni-wine: (command, PYTHONPATH or None).

    The `ni` this process was started as, so a newer build run from a
    checkout or a Nix result never hands the hook to an older `ni` on PATH.
    """
    argv0 = Path(sys.argv[0]) if sys.argv and sys.argv[0] else None
    if argv0 and argv0.name in ("ni", "native-access") and argv0.is_file():
        # `native-access` is the launcher entry point and takes no
        # subcommands; the hook needs its sibling `ni`.
        ni = argv0.resolve().with_name("ni")
        if ni.is_file():
            return [str(ni)], None
    package_root = Path(__file__).resolve().parent.parent
    return [sys.executable, "-m", "ni_wine.cli"], str(package_root)


def _hook_text(prefix: Path) -> str:
    parts, pythonpath = _ni_command()
    command = " ".join(f'"{part}"' for part in parts)
    env = f'export PYTHONPATH="{pythonpath}"\n' if pythonpath else ""
    return (
        "#!/bin/sh\n"
        "# Generated by ni-wine; run from inside Wine by the msi shim when the\n"
        "# Kontakt installer reaches its MSI step.  $1 is the package path.\n"
        "# stdio is whatever Wine handed the installer (often a dead pipe), so\n"
        "# everything goes to the hook log.\n"
        f"{env}"
        f'exec {command} --prefix "{prefix}" kontakt8 apply-installer "$@" '
        f'>>"{hook_log()}" 2>&1 </dev/null\n'
    )


def _cfg_text() -> str:
    lines = [f"divert={name}" for name in DIVERTED_PACKAGES]
    lines += [
        f"hook={hook_script()}",
        f"result={_win_path(result_file())}",
        f"log={_win_path(shim_log())}",
        "timeout=7200",
    ]
    return "\r\n".join(lines) + "\r\n"


def _put(src: Path, dst: Path) -> None:
    """Copy *src* over *dst* as a plain writable file (sources may be read-only,
    e.g. in the Nix store, and a previous copy may have kept that mode)."""
    if dst.exists():
        dst.chmod(0o644)
    shutil.copyfile(src, dst)
    dst.chmod(0o644)


def _write_if_changed(path: Path, text: str, *, executable: bool = False) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        if path.read_text() == text:
            return False
    except OSError:
        pass
    path.write_text(text)
    if executable:
        path.chmod(0o755)
    return True


# --- state -----------------------------------------------------------------


def override_registered(wine: Wine) -> bool:
    """Asks the live registry: user.reg on disk lags behind by up to a minute."""
    return wine.reg_query(_OVERRIDE_KEY, "msi") == _OVERRIDE_DATA


def installed(prefix: Path) -> bool:
    return is_shim(syswow64(prefix) / SHIM_FILE)


def current(prefix: Path) -> bool:
    source = shim_source()
    return bool(source) and installed(prefix) and _same_file(syswow64(prefix) / SHIM_FILE, source)


def describe(prefix: Path, wine: Wine | None = None) -> str:
    sw = syswow64(prefix)
    if not sw.is_dir():
        return "no prefix"
    parts = []
    if installed(prefix):
        parts.append("shim installed" + ("" if current(prefix) else " (outdated)"))
    elif is_wine_builtin(sw / SHIM_FILE):
        parts.append("not installed (stock Wine msi)")
    else:
        parts.append("not installed (unknown msi.dll in syswow64)")
    parts.append("real msi kept" if (sw / REAL_FILE).is_file() else "no msi_wine.dll")
    parts.append("config present" if (sw / CFG_FILE).is_file() else "no config")
    parts.append("hook script present" if hook_script().is_file() else "no hook script")
    if wine is not None:
        parts.append("override registered" if override_registered(wine) else "no override")
    return ", ".join(parts)


# --- install / remove ------------------------------------------------------


def ensure(wine: Wine, prefix: Path, *, quiet: bool = False) -> str | None:
    """Install or refresh the shim.  Returns a problem description, or None."""
    source = shim_source()
    if source is None:
        return "this ni-wine build has no msi_shim32.dll (the Kontakt installer hook)"
    sw = syswow64(prefix)
    if not sw.is_dir():
        return f"no syswow64 in {prefix} — run `ni setup`"
    shim = sw / SHIM_FILE
    real = sw / REAL_FILE
    changed = False

    if is_wine_builtin(shim):
        # Stock prefix, or Wine was updated and rewrote its builtin over the
        # shim: keep this (matching) copy as the real msi and install the shim.
        _put(shim, real)
        _put(source, shim)
        changed = True
    elif is_shim(shim):
        if not real.is_file() or not is_wine_builtin(real):
            fallback = _wine_builtin_msi(wine)
            if fallback is None:
                return (
                    f"{real} is missing and Wine's own msi.dll could not be found — "
                    "run `ni kontakt8 hook remove` after reinstalling Wine"
                )
            _put(fallback, real)
            changed = True
        if not _same_file(shim, source):
            _put(source, shim)
            changed = True
    else:
        return f"{shim} is neither Wine's msi nor ni-wine's shim — not touching it"

    changed |= _write_if_changed(sw / CFG_FILE, _cfg_text())
    changed |= _write_if_changed(hook_script(), _hook_text(prefix), executable=True)

    if not override_registered(wine):
        if not wine.reg_add(_OVERRIDE_KEY, "msi", _OVERRIDE_DATA, kill_on_failure=False):
            return "could not register the msi DllOverride for the Kontakt installer"
        changed = True

    if changed and not quiet:
        info("Kontakt installer hook (msi shim) installed in the prefix")
    return None


def remove(wine: Wine, prefix: Path) -> None:
    """Put the prefix back to stock: Wine's msi, no config, no override."""
    sw = syswow64(prefix)
    shim = sw / SHIM_FILE
    real = sw / REAL_FILE
    if is_shim(shim):
        if real.is_file() and is_wine_builtin(real):
            _put(real, shim)
        else:
            fallback = _wine_builtin_msi(wine)
            if fallback is None:
                warn(f"cannot restore Wine's msi.dll in {sw}: no copy found; "
                     "reinstall Wine or re-run `wineboot -u` for this prefix")
            else:
                _put(fallback, shim)
    real.unlink(missing_ok=True)
    (sw / CFG_FILE).unlink(missing_ok=True)
    hook_script().unlink(missing_ok=True)
    result_file().unlink(missing_ok=True)
    if override_registered(wine):
        wine.reg_delete(_OVERRIDE_KEY)
    info("Kontakt installer hook removed — the prefix uses Wine's msi again")
