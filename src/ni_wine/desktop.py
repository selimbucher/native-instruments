"""Linux desktop integration: launcher entry, icon, and the URL scheme handler.

Native Access's browser-based login redirects to native-access:// — for that
to reach the app running under Wine, an x-scheme-handler must be registered
on the Linux side (winemenubuilder is disabled, so Wine never does it).
"""

from __future__ import annotations

import importlib.resources
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


def _ni_exec() -> str:
    """The `ni` the desktop entry should run.

    A system install stays a bare name (any PATH finds it, and the entry
    keeps working across package upgrades).  Anything under $HOME or in a
    Nix store path gets its absolute path: the desktop portal's PATH does
    not include pipx's ~/.local/bin, and a checkout or `nix shell` build
    must not be silently replaced by an older `ni` on PATH.
    """
    running = Path(sys.argv[0]).resolve()
    if running.name in ("ni", "native-access"):
        candidate = running.parent / "ni"
        if candidate.is_file():
            found = shutil.which("ni")
            if found and Path(found).resolve() == candidate and Path.home() not in candidate.parents:
                return "ni"
            return str(candidate)
    found = shutil.which("ni")
    if found and Path.home() not in Path(found).parents:
        return "ni"
    return found or "ni"


def install_user_desktop_files(prefix: Path, *, quiet: bool = False) -> None:
    """Install our .desktop file and icon into the user's XDG data dir.

    The Exec line names the prefix explicitly: the browser starts the login
    callback without our environment, so `NI_WINE_PREFIX` would be lost and
    the callback would open Native Access in the default prefix instead.
    Whichever prefix was launched last owns the handler.

    Idempotent and cheap: nothing is rewritten when already up to date.
    """
    apps = config.data_home() / "applications"
    target = apps / config.DESKTOP_FILE_NAME
    content = _packaged("native-access.desktop")
    exec_line = f'Exec={_ni_exec()} --prefix "{prefix}" launch %u'
    content = content.replace("Exec=native-access %u", exec_line)
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

    Always maintains a user-local copy of the .desktop file: a system copy
    from an older package version may lack the MimeType/%u wiring, and the
    user-local file shadows it under the same desktop-file ID.
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
