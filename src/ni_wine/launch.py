"""Launch Native Access, repairing common launch blockers first."""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
from pathlib import Path

from . import config, daemon, kontakt, msishim
from .desktop import ensure_url_handler
from .powershell import install_profile
from .setup_cmd import run_setup
from .util import die, guarded_rmtree, info, warn
from .wine import Wine, apply_prefix_tweaks, foreign_prefix_users, prefix_in_use

_OFFLINE_MESSAGE = (
    "No connection to native-instruments.com — Native Access has no offline "
    "mode and will show 'Loading products failed'. Installed instruments and "
    "plugins keep working offline."
)


def native_access_running() -> bool:
    return (
        subprocess.run(
            ["pgrep", "-f", r"Native Access\.exe"], stdout=subprocess.DEVNULL
        ).returncode
        == 0
    )


def clear_updater_residue(prefix: Path) -> bool:
    """Drop Native Access's self-update download cache (can be ~370 MB).

    electron-updater leaves the downloaded installer behind after a
    successful update and re-validates/re-downloads on demand, so deleting
    the directory is always safe — as long as Native Access isn't running.
    """
    updater = config.updater_dir(prefix)
    if not updater.is_dir() or not any(updater.iterdir()):
        return False
    if native_access_running():
        return False
    info("clearing Native Access self-update leftovers")
    guarded_rmtree(updater)
    return True


def _screen_size() -> tuple[int, int] | None:
    """Usable desktop extent in the coordinate space NA saves positions in.

    Under XWayland that is the compositor's logical space, so prefer
    Hyprland's own answer (physical size / scale) and fall back to X11
    geometry, which matches on unscaled displays.
    """
    if shutil.which("hyprctl") and os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
        result = subprocess.run(
            ["hyprctl", "monitors", "-j"], capture_output=True, text=True
        )
        try:
            monitors = json.loads(result.stdout)
            width = max(
                int(m["x"] + m["width"] / m["scale"]) for m in monitors
            )
            height = max(
                int(m["y"] + m["height"] / m["scale"]) for m in monitors
            )
            return width, height
        except (json.JSONDecodeError, KeyError, ValueError, ZeroDivisionError):
            pass
    xdotool = shutil.which("xdotool")
    if xdotool and os.environ.get("DISPLAY"):
        result = subprocess.run(
            [xdotool, "getdisplaygeometry"], capture_output=True, text=True
        )
        try:
            width, height = map(int, result.stdout.split())
            return width, height
        except ValueError:
            pass
    return None


def fix_offscreen_window_state(prefix: Path) -> None:
    """Pull Native Access's remembered window position back on-screen.

    NA restores its saved window position unconditionally; after a monitor
    or scale change that position can be entirely off-screen — the app then
    runs healthy but invisible.
    """
    screen = _screen_size()
    if screen is None:
        return
    screen_w, screen_h = screen
    roaming = config.na_roaming_dir(prefix)
    if not roaming.is_dir():
        return
    for state_file in roaming.glob("*.json"):
        try:
            state = json.loads(state_file.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        if not (
            isinstance(state, dict)
            and {"x", "y", "width", "height"} <= state.keys()
            and all(isinstance(state[k], int) for k in ("x", "y"))
        ):
            continue
        # Keep at least a grabbable corner of the window visible.
        new_x = min(max(0, state["x"]), max(0, screen_w - 100))
        new_y = min(max(0, state["y"]), max(0, screen_h - 100))
        if (new_x, new_y) != (state["x"], state["y"]):
            state["x"], state["y"] = new_x, new_y
            state_file.write_text(json.dumps(state, indent="\t"))
            info("moved Native Access's saved window position back on-screen")


def check_online(timeout: float = 3.0) -> bool:
    try:
        with socket.create_connection(
            ("api.native-instruments.com", 443), timeout=timeout
        ):
            return True
    except OSError:
        return False


def _warn_offline() -> None:
    warn(_OFFLINE_MESSAGE)
    if shutil.which("notify-send"):
        subprocess.run(
            ["notify-send", "--app-name=Native Access", "Native Access", _OFFLINE_MESSAGE],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def run_launch(prefix: Path, url: str | None = None) -> int:
    if not config.na_exe(prefix).is_file():
        info("Native Access not installed — running setup...")
        run_setup(prefix, ui=True)

    wine = Wine(prefix)
    # explorer.exe reads the tray settings only at startup -- but never
    # take the server down under Kontakt or a plugin host (we may have been
    # started by Kontakt's Activate button).
    if apply_prefix_tweaks(wine) and not prefix_in_use(prefix):
        wine.kill_server()
    clear_updater_residue(prefix)
    ensure_url_handler(prefix, quiet=True)
    if not native_access_running():
        fix_offscreen_window_state(prefix)

    if not check_online():
        _warn_offline()

    # Prefixes set up by older ni-wine still run the wrapper's profile.ps1.
    if install_profile(prefix):
        info("installed ni-wine's PowerShell profile in the prefix")

    # Native Access must find its daemon running (see daemon.py); refuse to
    # launch into the stuck screen otherwise.  On the first start after boot
    # the daemon still needs time to build its product list; NA may briefly
    # show "library empty" with a working Retry.
    problem = daemon.ensure(wine, prefix)
    if problem:
        die(problem)

    # Lets Native Access install/update Kontakt like any other product.
    problem = msishim.ensure(wine, prefix)
    if problem:
        warn(f"Kontakt installer hook not armed: {problem}")

    # A Kontakt laid out by an older ni-wine lacks the registry values the
    # daemon judges installs by; Native Access would offer to install it again.
    if kontakt.repair_registry(prefix):
        info("registered the existing Kontakt 8 install for Native Access")

    args = [str(config.na_exe(prefix))]
    if url:
        args.append(url)

    debug = "NI_WINE_DEBUG" in os.environ
    extra_env: dict[str, str] = {} if debug else {"WINEDEBUG": "-all"}

    # NA's Electron runtime crashes with "open EBADF" if any stdio fd is
    # closed (desktop launchers don't guarantee them), so give it /dev/null
    # instead of inheriting ours, unless debug output was asked for.
    result = wine.run(args, extra_env=extra_env, quiet=not debug)
    return result.returncode


def run_reinstall(prefix: Path, *, assume_yes: bool = False) -> None:
    print()
    print(f"Warning: this wipes the Wine prefix at {prefix}.")
    print("All installed libraries and instruments stored there will be removed.")
    print()
    if not assume_yes:
        try:
            confirmation = input("Type YES to continue: ")
        except EOFError:
            confirmation = ""
        if confirmation != "YES":
            info("aborted")
            return

    hosts = foreign_prefix_users(prefix)
    if hosts:
        die(
            f"{len(hosts)} plugin host(s) (yabridge) are using {prefix} — "
            "close your DAW first."
        )
    info("killing Wine session...")
    wine = Wine(prefix)
    wine.kill_server()
    # Wait until wineserver has fully exited: it rewrites the registry
    # files on shutdown, which would resurrect parts of the deleted prefix.
    try:
        wine.wait_server()
    except subprocess.TimeoutExpired:
        die("wine processes did not exit — close them and retry")

    info("removing Wine prefix...")
    guarded_rmtree(prefix)

    info("running setup...")
    run_setup(prefix, ui=True)


def ensure_prefix_exists(prefix: Path) -> None:
    if not config.drive_c(prefix).is_dir():
        die(f"no Wine prefix at {prefix} — run `ni setup` first")
