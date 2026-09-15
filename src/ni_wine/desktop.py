"""Linux desktop integration: launcher entry, icon, and the URL scheme handler.

Native Access's browser-based login redirects to native-access:// — for that
to reach the app running under Wine, an x-scheme-handler must be registered
on the Linux side (winemenubuilder is disabled, so Wine never does it).
"""

from __future__ import annotations

import importlib.resources
import os
import shutil
import subprocess
import sys
from pathlib import Path

from . import config
from .util import info, warn

_SCHEME_MIME = f"x-scheme-handler/{config.URL_SCHEME}"


def _packaged(name: str) -> str:
    return (
        importlib.resources.files("ni_wine").joinpath("data", name).read_text()
    )


def _run_quiet(args: list[str]) -> subprocess.CompletedProcess | None:
    if not shutil.which(args[0]):
        return None
    return subprocess.run(args, capture_output=True, text=True)


def current_scheme_handler() -> str | None:
    result = _run_quiet(["xdg-mime", "query", "default", _SCHEME_MIME])
    if result is None:
        return None
    handler = result.stdout.strip()
    return handler or None


def _launcher_exec() -> str:
    """The `native-access` the desktop entry should run.

    A system install stays a bare name (any PATH finds it, and the entry
    keeps working across package upgrades).  Anything under $HOME or in a
    Nix store path gets its absolute path: the desktop portal's PATH does
    not include pipx's ~/.local/bin, and a checkout or `nix shell` build
    must not be silently replaced by an older `ni` on PATH.
    """
    running = Path(sys.argv[0]).resolve()
    if running.name in ("ni", "native-access"):
        candidate = running.parent / "native-access"
        if candidate.is_file():
            found = shutil.which("native-access")
            if found and Path(found).resolve() == candidate and Path.home() not in candidate.parents:
                return "native-access"
            return str(candidate)
    found = shutil.which("native-access")
    if found and Path.home() not in Path(found).parents:
        return "native-access"
    return found or "native-access"


def _system_entry_current() -> bool:
    """A system-wide copy of our .desktop file with the exact current
    content exists.  Every package channel installs the data file verbatim,
    so byte-equality means "same version, all wiring present"."""
    data_dirs = os.environ.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share")
    packaged = _packaged("native-access.desktop")
    home = config.data_home()
    for base in filter(None, data_dirs.split(":")):
        if Path(base) == home:
            continue
        entry = Path(base) / "applications" / config.DESKTOP_FILE_NAME
        try:
            if entry.is_file() and entry.read_text() == packaged:
                return True
        except OSError:
            continue
    return False


def install_user_desktop_files(prefix: Path, *, quiet: bool = False) -> None:
    """Install our .desktop file and icon into the user's XDG data dir —
    but only when they add something over the packaged system entry.

    The user copy exists for three reasons: a launcher outside the portal
    PATH (pipx, nix shell), a non-default prefix that must be baked into
    Exec (the browser login callback runs without our environment), or a
    stale/absent system entry.  When none apply, the copy would only
    shadow the packaged entry and linger after the package is removed —
    so it is deleted instead.

    Idempotent and cheap: nothing is rewritten when already up to date.
    """
    apps = config.data_home() / "applications"
    target = apps / config.DESKTOP_FILE_NAME
    launcher = _launcher_exec()

    if (launcher == "native-access"
            and prefix == Path.home() / ".wine-ni"
            and _system_entry_current()):
        removed = False
        if target.is_file():
            target.unlink()
            removed = True
        icon = config.data_home() / "icons/hicolor/scalable/apps/native-access.svg"
        if icon.is_file():
            icon.unlink()
        if removed:
            _run_quiet(["update-desktop-database", str(apps)])
            if not quiet:
                info("removed the user-local desktop entry — the packaged "
                     "system entry is current")
        return

    content = _packaged("native-access.desktop")
    exec_line = f'Exec=env NI_WINE_GUI=1 {launcher} --prefix "{prefix}" %u'
    content = content.replace("Exec=env NI_WINE_GUI=1 native-access %u", exec_line)
    # TryExec hides the entry from menus while the launcher is missing
    # (e.g. after the package is removed but this user copy lingers).
    content = content.replace("TryExec=native-access", f"TryExec={launcher}")
    if target.is_file() and target.read_text() == content:
        return
    apps.mkdir(parents=True, exist_ok=True)
    target.write_text(content)

    icons = config.data_home() / "icons/hicolor/scalable/apps"
    icons.mkdir(parents=True, exist_ok=True)
    (icons / "native-access.svg").write_text(_packaged("native-access.svg"))

    _run_quiet(["update-desktop-database", str(apps)])
    if not quiet:
        info(f"installed {config.DESKTOP_FILE_NAME} to {apps}")


def ensure_url_handler(prefix: Path, *, quiet: bool = False) -> bool:
    """Make sure native-access:// URLs are routed to us. Returns success.

    Maintains a user-local copy of the .desktop file only when the
    packaged system entry doesn't cover us (see
    install_user_desktop_files); both live under the same desktop-file
    ID, so the scheme handler resolves either way.
    """
    install_user_desktop_files(prefix, quiet=quiet)

    if current_scheme_handler():
        return True

    result = _run_quiet(
        ["xdg-mime", "default", config.DESKTOP_FILE_NAME, _SCHEME_MIME]
    )
    if result is None:
        if not quiet:
            warn(
                "xdg-mime not found — cannot register the login URL handler; "
                "browser login callbacks will not reach Native Access"
            )
        return False
    if not quiet:
        info(f"registered {config.DESKTOP_FILE_NAME} as {_SCHEME_MIME} handler")
    return current_scheme_handler() is not None
