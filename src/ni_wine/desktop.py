"""Desktop entry, icon and the native-access:// scheme handler.

The browser login redirects to native-access://, so an x-scheme-handler has
to be registered on the Linux side (winemenubuilder is disabled, Wine won't).
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
    return importlib.resources.files("ni_wine").joinpath("data", name).read_text()


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


def _mimeapps_files() -> list[Path]:
    """User mimeapps.list files, highest precedence first (the second is the old xdg-utils
    location).
    """
    return [
        config.config_home() / "mimeapps.list",
        config.data_home() / "applications" / "mimeapps.list",
    ]


def _recorded_default(path: Path) -> str | None:
    try:
        lines = path.read_text().splitlines()
    except OSError:
        return None
    in_defaults = False
    for line in lines:
        line = line.strip()
        if line.startswith("["):
            in_defaults = line == "[Default Applications]"
        elif in_defaults and "=" in line:
            key, _, value = line.partition("=")
            if key.strip() == _SCHEME_MIME:
                # ;-separated list, first entry wins
                entries = [v.strip() for v in value.split(";") if v.strip()]
                return entries[0] if entries else None
    return None


def recorded_scheme_handler() -> tuple[Path, str] | None:
    """What mimeapps.list says, not what `xdg-mime query` says: the query goes through
    mimeinfo.cache and falls back to another association, so it can report us while a foreign
    entry is still written down and wins after the next cache rebuild.
    """
    for path in _mimeapps_files():
        value = _recorded_default(path)
        if value:
            return path, value
    return None


def scheme_handler_owner() -> tuple[str, str] | None:
    """(source, handler) for whatever owns native-access://, or None."""
    recorded = recorded_scheme_handler()
    if recorded:
        return str(recorded[0]), recorded[1]
    handler = current_scheme_handler()
    return ("xdg-mime", handler) if handler else None


def _launcher_exec() -> str:
    """Exec for the desktop entry: bare `native-access` for a system install, absolute path for
    anything under $HOME or in the Nix store (the portal's PATH lacks ~/.local/bin, and a
    checkout must not be shadowed by an older ni).
    """
    running = Path(sys.argv[0]).resolve()
    if running.name in ("ni", "native-access"):
        candidate = running.parent / "native-access"
        if candidate.is_file():
            found = shutil.which("native-access")
            if (
                found
                and Path(found).resolve() == candidate
                and Path.home() not in candidate.parents
            ):
                return "native-access"
            return str(candidate)
    found = shutil.which("native-access")
    if found and Path.home() not in Path(found).parents:
        return "native-access"
    return found or "native-access"


def _system_entry_current() -> bool:
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


def _user_icon() -> Path:
    return config.data_home() / "icons/hicolor/scalable/apps/native-access.svg"


def install_user_desktop_files(prefix: Path, *, quiet: bool = False) -> None:
    """User copy of the .desktop file and icon, only when the packaged entry can't serve: launcher
    outside the portal PATH, non-default prefix baked into Exec, stale or missing system entry.
    Otherwise the copy is removed so it doesn't shadow the package.
    """
    apps = config.data_home() / "applications"
    target = apps / config.DESKTOP_FILE_NAME
    launcher = _launcher_exec()

    if (
        launcher == "native-access"
        and prefix == Path.home() / ".wine-ni"
        and _system_entry_current()
    ):
        removed = False
        if target.is_file():
            target.unlink()
            removed = True
        icon = _user_icon()
        if icon.is_file():
            icon.unlink()
        if removed:
            _run_quiet(["update-desktop-database", str(apps)])
            if not quiet:
                info("removed the user-local desktop entry, the packaged system entry is current")
        return

    content = _packaged("native-access.desktop")
    exec_line = f'Exec=env NI_WINE_GUI=1 {launcher} --prefix "{prefix}" %u'
    content = content.replace("Exec=env NI_WINE_GUI=1 native-access %u", exec_line)
    # TryExec hides the entry while the launcher is missing (package removed)
    content = content.replace("TryExec=native-access", f"TryExec={launcher}")
    # entry and icon checked separately, a release can change just one of them
    icon = _user_icon()
    svg = _packaged("native-access.svg")
    if not (icon.is_file() and icon.read_text() == svg):
        icon.parent.mkdir(parents=True, exist_ok=True)
        icon.write_text(svg)
    if target.is_file() and target.read_text() == content:
        return
    apps.mkdir(parents=True, exist_ok=True)
    target.write_text(content)

    _run_quiet(["update-desktop-database", str(apps)])
    if not quiet:
        info(f"installed {config.DESKTOP_FILE_NAME} to {apps}")


def ensure_url_handler(prefix: Path, *, quiet: bool = False) -> bool:
    """Route native-access:// to us, taking the scheme over from a foreign owner (Bottles leaves its
    entry as default and the login callback lands in that prefix, #5). Returns success.
    """
    install_user_desktop_files(prefix, quiet=quiet)

    owner = scheme_handler_owner()
    if owner is not None and owner[1] == config.DESKTOP_FILE_NAME:
        return True

    result = _run_quiet(["xdg-mime", "default", config.DESKTOP_FILE_NAME, _SCHEME_MIME])
    if result is None:
        if not quiet:
            warn(
                "xdg-mime not found, cannot register the login URL handler, "
                "browser login callbacks will not reach Native Access"
            )
        return False
    now = scheme_handler_owner()
    if now is None or now[1] != config.DESKTOP_FILE_NAME:
        if not quiet:
            warn(
                f"could not take {config.URL_SCHEME}:// over from "
                f"{owner[1]} ({owner[0]}), browser login callbacks will "
                "open that application instead"
                if owner
                else "xdg-mime did not register the login URL handler"
            )
        return False
    if not quiet:
        if owner:
            info(f"took {config.URL_SCHEME}:// over from {owner[1]} (was set in {owner[0]})")
        else:
            info(f"registered {config.DESKTOP_FILE_NAME} as {_SCHEME_MIME} handler")
    return True
