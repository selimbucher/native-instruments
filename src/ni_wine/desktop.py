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


def _mimeapps_files() -> list[Path]:
    """User-writable mimeapps.list files, highest precedence first.

    xdg-mime writes the first; the second is the location older xdg-utils
    used and where another tool's default may still be sitting.
    """
    return [
        config.config_home() / "mimeapps.list",
        config.data_home() / "applications" / "mimeapps.list",
    ]


def _recorded_default(path: Path) -> str | None:
    """The scheme's default in one mimeapps.list, or None."""
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
                # A ;-separated preference list; the first entry wins.
                entries = [v.strip() for v in value.split(";") if v.strip()]
                return entries[0] if entries else None
    return None


def recorded_scheme_handler() -> tuple[Path, str] | None:
    """The scheme's default as literally recorded, or None.

    `xdg-mime query` resolves through mimeinfo.cache and quietly falls back
    to some other association when the recorded default does not resolve —
    a desktop-file id containing spaces, say.  That makes the query an
    unreliable answer to "who owns this scheme": it can report us while a
    foreign entry is still what is written down, ready to win again the
    next time the cache is rebuilt.  The recorded value is the durable one.
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

    Takes the scheme over from a foreign owner rather than leaving it be.
    Another Wine manager that once ran Native Access claims it too —
    Bottles writes "<bottle>--<program>--<timestamp>.desktop" — and its
    entry sends the browser login callback into *that* tool's prefix,
    where Native Access has no NTK daemon and stalls on "Please grant
    permission to Native Access to install dependencies".
    """
    install_user_desktop_files(prefix, quiet=quiet)

    owner = scheme_handler_owner()
    if owner is not None and owner[1] == config.DESKTOP_FILE_NAME:
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
    now = scheme_handler_owner()
    if now is None or now[1] != config.DESKTOP_FILE_NAME:
        if not quiet:
            warn(
                f"could not take {config.URL_SCHEME}:// over from "
                f"{owner[1]} ({owner[0]}) — browser login callbacks will "
                "open that application instead"
                if owner
                else "xdg-mime did not register the login URL handler"
            )
        return False
    if not quiet:
        if owner:
            info(
                f"took {config.URL_SCHEME}:// over from {owner[1]} "
                f"(was set in {owner[0]})"
            )
        else:
            info(f"registered {config.DESKTOP_FILE_NAME} as {_SCHEME_MIME} handler")
    return True
