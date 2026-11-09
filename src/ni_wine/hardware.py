"""NI controller support: nimk3.sys on the MK3 display and audio interfaces, the nirescan and
nimidi services in the prefix (hardware/README.md), and the ni-maschine user service
(maschine/README.md)."""

from __future__ import annotations

import hashlib
import importlib.resources
import os
import re
import shutil
import subprocess
from pathlib import Path

from . import config
from .util import info
from .wine import Wine, prefix_in_use

INF = "maschine-mk3.inf"
DRIVER = "nimk3.sys"
SERVICES = ("nirescan", "nimidi")
# the MK3 interfaces nimk3 drives: the displays and the audio interface's Ctrl MIDI port
INTERFACE_IDS = (r"USB\VID_17CC&PID_1600&MI_05", r"USB\VID_17CC&PID_1600&MI_00")
DAEMON = "ni-maschine"
# nimidi.exe runs the path in this value to reach ni-maschine
DAEMON_KEY = r"HKLM\Software\ni-wine"
UNIT = "ni-maschine.service"

_DRIVER_SERVICE = "nimk3"
_INSTALLER = "nidrvinst.exe"


def _enum_keys(interface_id: str) -> re.Pattern[str]:
    """Bodies of the device keys of an interface in system.reg."""
    enum = re.escape(interface_id.replace("\\", "\\\\"))
    return re.compile(
        rf"^\[System\\\\ControlSet001\\\\Enum\\\\{enum}\\\\[^\]\\]+\][^\n]*\n((?:[^\[\n][^\n]*\n)*)",
        re.MULTILINE | re.IGNORECASE,
    )


def _source(name: str) -> Path | None:
    try:
        path = Path(str(importlib.resources.files("ni_wine") / "data" / name))
    except (ModuleNotFoundError, TypeError):
        return None
    return path if path.is_file() else None


def _targets(prefix: Path) -> dict[str, Path]:
    windows = config.drive_c(prefix) / "windows"
    return {
        INF: windows / "inf" / INF,
        DRIVER: windows / "system32/drivers" / DRIVER,
        **{f"{name}.exe": windows / "system32" / f"{name}.exe" for name in SERVICES},
    }


def _digest(path: Path) -> bytes | None:
    try:
        return hashlib.sha256(path.read_bytes()).digest()
    except OSError:
        return None


def _system_reg(prefix: Path) -> str:
    try:
        return (prefix / "system.reg").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def outdated_files(prefix: Path) -> list[str]:
    """Shipped files missing from the prefix or different there."""
    return [
        name
        for name, target in _targets(prefix).items()
        if _digest(target) is None or _digest(target) != _digest(_source(name) or target)
    ]


def unregistered_services(prefix: Path) -> list[str]:
    text = _system_reg(prefix)
    return [name for name in SERVICES if f"\\\\Services\\\\{name}]" not in text]


def misbound_interfaces(prefix: Path) -> dict[str, str]:
    """MK3 interfaces Wine bound to something other than nimk3, with that service ("" for no
    driver). An interface Wine never bound gets nimk3 by itself."""
    found = {}
    text = _system_reg(prefix)
    for interface_id in INTERFACE_IDS:
        for body in _enum_keys(interface_id).findall(text):
            if not re.search(r'^"Driver"=', body, re.MULTILINE):
                continue
            match = re.search(r'^"Service"="([^"]*)"', body, re.MULTILINE)
            service = match.group(1) if match else ""
            if service.lower() != _DRIVER_SERVICE:
                found[interface_id] = service
    return found


def daemon_executable() -> str | None:
    """ni-maschine next to `ni`. A PATH entry is preferred when it leads to the same file, it
    stays valid across updates (Nix store paths don't)."""
    installed = Path(__file__).resolve().parents[4] / "bin" / DAEMON
    on_path = shutil.which(DAEMON)
    if not installed.is_file():
        return on_path
    if on_path and Path(on_path).resolve() == installed.resolve():
        return on_path
    return str(installed)


def daemon_registered(prefix: Path, executable: str) -> bool:
    return f'"{DAEMON}"="{executable}"' in _system_reg(prefix)


def unit_path() -> Path:
    return config.config_home() / "systemd" / "user" / UNIT


def unit_text(executable: str, prefix: Path) -> str:
    return (
        "[Unit]\n"
        "Description=Maschine MK3 MIDI mode (ni-wine)\n"
        "\n"
        "[Service]\n"
        f'ExecStart="{executable}"\n'
        f'Environment="NI_WINE_PREFIX={prefix}"\n'
        "Restart=on-failure\n"
        "\n"
        "[Install]\n"
        "WantedBy=default.target\n"
    )


def _systemctl(*args: str) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(
            ["systemctl", "--user", *args], capture_output=True, text=True, timeout=30
        )
    except (OSError, subprocess.TimeoutExpired):
        return None


def service_running() -> bool:
    result = _systemctl("is-active", UNIT)
    return result is not None and result.stdout.strip() == "active"


def ensure_service(prefix: Path) -> str | None:
    """Install, enable and start ni-maschine as a user service. Returns a problem, or None."""
    executable = daemon_executable()
    if not executable:
        return f"this ni-wine build lacks {DAEMON}"
    text = unit_text(executable, prefix)
    path = unit_path()
    try:
        changed = path.read_text() != text
    except OSError:
        changed = True
    if changed:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        if _systemctl("daemon-reload") is None:
            return f"no systemd user session, start {DAEMON} yourself"
        _systemctl("enable", UNIT)
        _systemctl("restart", UNIT)
    elif not service_running():
        _systemctl("enable", "--now", UNIT)
    if not service_running():
        return f"{UNIT} doesn't run, see `journalctl --user -u {UNIT}`"
    return None


def connected_controllers(
    hidraw: Path = Path("/sys/class/hidraw"), dev: Path = Path("/dev")
) -> dict[str, bool]:
    """NI HID devices plugged in ("<name> (/dev/hidrawN)") and whether this user can open them."""
    found = {}
    for node in sorted(hidraw.glob("hidraw*")):
        try:
            uevent = (node / "device" / "uevent").read_text()
        except OSError:
            continue
        fields = {}
        for line in uevent.splitlines():
            key, _, value = line.partition("=")
            fields[key] = value
        if not fields.get("HID_ID", "").upper().startswith("0003:000017CC:"):
            continue
        path = dev / node.name
        name = fields.get("HID_NAME", "NI device")
        found[f"{name} ({path})"] = os.access(path, os.R_OK | os.W_OK)
    return found


def describe(prefix: Path) -> str:
    parts = [f"{name} missing or outdated" for name in outdated_files(prefix)]
    parts += [f"{name} service not registered" for name in unregistered_services(prefix)]
    for interface_id, service in misbound_interfaces(prefix).items():
        what = "MK3 display" if interface_id.endswith("MI_05") else "MK3 audio interface"
        parts.append(f"{what} bound to {service or 'no driver'}")
    executable = daemon_executable()
    if not executable:
        parts.append(f"{DAEMON} missing")
    else:
        if not daemon_registered(prefix, executable):
            parts.append(f"{DAEMON} path not registered")
        if not service_running():
            parts.append(f"{UNIT} not running")
    return ", ".join(parts)


def _wine(wine: Wine, args: list[str]) -> int:
    # no pipes: the services Wine starts inherit stdio and would keep them open
    return subprocess.run(
        [wine.wine, *args],
        env=wine.env({"WINEDEBUG": "-all"}),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode


def ensure(wine: Wine, prefix: Path, *, quiet: bool = False) -> str | None:
    """Install or refresh the driver, the services, the interface bindings, the ni-maschine path
    and service. Restarts an idle prefix after a change. Returns a problem, or None."""
    sources = {name: _source(name) for name in (*_targets(prefix), _INSTALLER)}
    missing = [name for name, path in sources.items() if path is None]
    if missing:
        return f"this ni-wine build lacks {', '.join(missing)}"
    if not (config.drive_c(prefix) / "windows/system32").is_dir():
        return f"no Wine prefix at {prefix}, run `ni setup`"

    running = prefix_in_use(prefix)
    changed = False

    for name in outdated_files(prefix):
        target = _targets(prefix)[name]
        target.parent.mkdir(parents=True, exist_ok=True)
        # a new file, not a rewrite: a running prefix has the old one mapped
        staged = target.with_name(target.name + ".new")
        staged.write_bytes(sources[name].read_bytes())
        os.replace(staged, target)
        changed = True

    # live registry, system.reg lags behind a running prefix
    for name in unregistered_services(prefix):
        key = rf"HKLM\System\CurrentControlSet\Services\{name}"
        if wine.reg_query(key, "ImagePath") is not None:
            continue
        binary = rf"C:\windows\system32\{name}.exe"
        if _wine(wine, ["sc", "create", name, "binPath=", binary, "start=", "auto"]):
            return f"registering the {name} service failed"
        changed = True

    for interface_id in misbound_interfaces(prefix):
        code = _wine(wine, [str(sources[_INSTALLER]), interface_id, rf"C:\windows\inf\{INF}"])
        if code:
            return f"binding nimk3 to {interface_id} failed (exit code {code})"
        changed = True

    # read whenever nimidi connects, no restart needed
    executable = daemon_executable()
    if (
        executable
        and not daemon_registered(prefix, executable)
        and wine.reg_query(DAEMON_KEY, DAEMON) != executable
        and not wine.reg_add(DAEMON_KEY, DAEMON, executable, kill_on_failure=False)
    ):
        return f"registering the {DAEMON} path failed"
    if problem := ensure_service(prefix):
        return problem

    if changed:
        if not running:
            wine.kill_server()
            try:
                wine.wait_server()
            except subprocess.TimeoutExpired:
                pass
        if not quiet:
            info(
                "NI controller support installed in the prefix"
                + (", active once Wine restarts" if running else "")
            )
    return None
