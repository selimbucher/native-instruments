"""NI controller support in the prefix: nimk3.sys, the nirescan service and the MK3 display
binding (hardware/README.md)."""

from __future__ import annotations

import hashlib
import importlib.resources
import os
import re
import subprocess
from pathlib import Path

from . import config
from .util import info
from .wine import Wine, prefix_in_use

INF = "maschine-mk3.inf"
DRIVER = "nimk3.sys"
SERVICE = "nirescan"
DISPLAY_ID = r"USB\VID_17CC&PID_1600&MI_05"

_DRIVER_SERVICE = "nimk3"
_INSTALLER = "nidrvinst.exe"
_SERVICE_KEY = rf"HKLM\System\CurrentControlSet\Services\{SERVICE}"
_DISPLAY_KEY = re.compile(
    r"^\[System\\\\ControlSet001\\\\Enum\\\\USB\\\\VID_17CC&PID_1600&MI_05\\\\[^\]]+\][^\n]*\n"
    r"((?:[^\[\n][^\n]*\n)*)",
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
        f"{SERVICE}.exe": windows / "system32" / f"{SERVICE}.exe",
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


def service_registered(prefix: Path) -> bool:
    return f"\\\\Services\\\\{SERVICE}]" in _system_reg(prefix)


def foreign_displays(prefix: Path) -> list[str]:
    """Services other than nimk3 bound to an MK3 display interface."""
    services = []
    for body in _DISPLAY_KEY.findall(_system_reg(prefix)):
        match = re.search(r'^"Service"="([^"]*)"', body, re.MULTILINE)
        if match and match.group(1).lower() != _DRIVER_SERVICE:
            services.append(match.group(1))
    return services


def describe(prefix: Path) -> str:
    parts = [f"{name} missing or outdated" for name in outdated_files(prefix)]
    if not service_registered(prefix):
        parts.append(f"{SERVICE} service not registered")
    parts += [f"MK3 display bound to {service}" for service in foreign_displays(prefix)]
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
    """Install or refresh the driver, the service and the display binding. Restarts an idle
    prefix after a change. Returns a problem, or None."""
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
    if not service_registered(prefix) and wine.reg_query(_SERVICE_KEY, "ImagePath") is None:
        binary = rf"C:\windows\system32\{SERVICE}.exe"
        if _wine(wine, ["sc", "create", SERVICE, "binPath=", binary, "start=", "auto"]):
            return f"registering the {SERVICE} service failed"
        changed = True

    if foreign_displays(prefix):
        code = _wine(wine, [str(sources[_INSTALLER]), DISPLAY_ID, rf"C:\windows\inf\{INF}"])
        if code:
            return f"binding the MK3 display driver failed (exit code {code})"
        changed = True

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
