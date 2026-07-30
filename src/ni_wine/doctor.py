"""Diagnose (and optionally repair) the ni-wine environment.

Covers the failure modes users actually hit:
- missing runtime dependencies
- prefix not set up / Native Access missing
- tray-icon window tweak not applied
- native-access:// login callback not routed (no x-scheme-handler)
- browser login prompts (optionally pre-authorize the NI origins)
- leftover Native Access self-update downloads (~370 MB)
"""

from __future__ import annotations

import json
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path

from . import config
from .browser import probe_browser
from .desktop import current_scheme_handler, ensure_url_handler
from .launch import native_access_running, clear_updater_residue
from .util import which_first
from .wine import TRAY_DISABLED_MARKER, Wine, apply_prefix_tweaks

# Chromium persists "Always allow <origin> to open <scheme> links" per
# origin+scheme in the profile's Preferences JSON.  A deny is never stored
# (removed from Chromium in 2020), so the only useful repair is seeding the
# allow for NI's login origins — then the login callback opens without any
# prompt.  Linux Chromium does not tamper-protect this key; the only rule is
# to edit while the browser is closed (it rewrites Preferences every ~10 s).
_NI_LOGIN_ORIGINS = (
    "https://www.native-instruments.com",
    "https://auth.native-instruments.com",
)

_BROWSER_CONFIG_ROOTS: dict[str, str] = {
    "chromium": ".config/chromium",
    "chrome": ".config/google-chrome",
    "brave": ".config/BraveSoftware/Brave-Browser",
    "edge": ".config/microsoft-edge",
    "vivaldi": ".config/vivaldi",
}


@dataclass
class Check:
    label: str
    ok: bool
    detail: str = ""
    required: bool = True

    def render(self) -> str:
        mark = "ok" if self.ok else ("!!" if self.required else "--")
        line = f"  [{mark}] {self.label}"
        if self.detail:
            line += f" — {self.detail}"
        return line


def _dependency_checks() -> list[Check]:
    deps: list[tuple[str, tuple[str, ...], bool, str]] = [
        ("wine", ("wine",), True, "runs Native Access"),
        ("winetricks", ("winetricks",), True,
         "installs vcrun2022/powershell during setup (Debian: enable contrib)"),
        ("cabextract", ("cabextract",), True, "unpacks the VC++ redistributable"),
        ("7z", ("7z", "7zz", "7za"), True, "unpacks the Kontakt installer (package: 7zip)"),
        ("msidump", ("msidump",), True, "reads MSI tables (package: msitools)"),
        ("pgrep", ("pgrep",), True, "process checks (package: procps)"),
        ("Xvfb", ("Xvfb",), True, "hides installer windows during setup"),
        ("xdotool", ("xdotool",), True, "auto-dismisses installer dialogs"),
        ("yad/zenity", ("yad", "zenity"), True, "graphical setup progress"),
    ]
    checks = []
    for label, names, required, purpose in deps:
        found = which_first(*names)
        checks.append(
            Check(label, bool(found), found or f"not found ({purpose})", required)
        )
    browser = probe_browser()
    checks.append(Check(
        "web browser (chromium- or firefox-family)",
        browser is not None,
        browser[1] if browser else
        "not found (captures download URLs from the NI website)",
    ))
    return checks


def _prefix_checks(prefix: Path) -> list[Check]:
    checks = [
        Check(f"Wine prefix at {prefix}", config.drive_c(prefix).is_dir(),
              "" if config.drive_c(prefix).is_dir() else "run `ni setup`")
    ]
    if not config.drive_c(prefix).is_dir():
        return checks

    checks.append(
        Check("Native Access installed", config.na_exe(prefix).is_file(),
              "" if config.na_exe(prefix).is_file() else "run `ni setup`")
    )
    checks.append(
        Check("NTKDaemon installed", config.ntk_daemon_exe(prefix).is_file(),
              "" if config.ntk_daemon_exe(prefix).is_file() else "run `ni setup`")
    )
    checks.append(
        Check("Kontakt 8 installed", config.kontakt8_exe(prefix).is_file(),
              "" if config.kontakt8_exe(prefix).is_file()
              else "optional — `ni kontakt8 install`", required=False)
    )

    reg_text = ""
    try:
        reg_text = (prefix / "user.reg").read_text(errors="replace")
    except OSError:
        pass
    tray_off = TRAY_DISABLED_MARKER in reg_text
    checks.append(
        Check("tray-icon window disabled", tray_off,
              "" if tray_off else "run `ni doctor --fix`")
    )
    scheme_registered = (
        f"[Software\\\\Classes\\\\{config.URL_SCHEME}\\\\shell\\\\open\\\\command]"
        in reg_text
    )
    checks.append(
        Check("native-access:// registered in prefix", scheme_registered,
              "" if scheme_registered else "run `ni doctor --fix`")
    )

    updater = config.updater_dir(prefix)
    residue = updater.is_dir() and any(updater.iterdir())
    checks.append(
        Check("no self-update leftovers", not residue,
              "" if not residue
              else "old update downloads waste disk — run `ni doctor --fix`",
              required=False)
    )
    return checks


def _handler_check() -> Check:
    handler = current_scheme_handler()
    return Check(
        "native-access:// URL handler (Linux)",
        handler is not None,
        handler or "browser logins cannot reach the app — run `ni doctor --fix`",
    )


# --- Browser login pre-authorization ---------------------------------------


def _default_browser_root() -> tuple[str, Path] | None:
    """Map the xdg default browser to its Chromium config directory."""
    xdg_settings = which_first("xdg-settings")
    if not xdg_settings:
        return None
    result = subprocess.run(
        [xdg_settings, "get", "default-web-browser"],
        capture_output=True,
        text=True,
    )
    desktop_id = result.stdout.strip().lower()
    if not desktop_id:
        return None
    for token in ("brave", "chromium", "chrome", "edge", "vivaldi"):
        if token in desktop_id:
            root = Path.home() / _BROWSER_CONFIG_ROOTS[token]
            if root.is_dir():
                return token, root
            return None
    return None


def _browser_running(root: Path) -> bool:
    lock = root / "SingletonLock"
    try:
        target = os.readlink(lock)
    except OSError:
        return False
    _, _, pid_str = target.rpartition("-")
    try:
        os.kill(int(pid_str), 0)
        return True
    except (ValueError, ProcessLookupError, PermissionError):
        return False


def _allowed_origins(pref_file: Path) -> list[str]:
    try:
        prefs = json.loads(pref_file.read_text())
    except (OSError, json.JSONDecodeError):
        return []
    pairs = prefs.get("protocol_handler", {}).get("allowed_origin_protocol_pairs", {})
    return [
        origin
        for origin, schemes in pairs.items()
        if isinstance(schemes, dict) and schemes.get(config.URL_SCHEME)
    ]


def _seed_allow(pref_file: Path) -> bool:
    """Pre-authorize the NI login origins for our scheme. Returns changed."""
    try:
        text = pref_file.read_text()
        prefs = json.loads(text)
    except (OSError, json.JSONDecodeError):
        return False
    pairs = prefs.setdefault("protocol_handler", {}).setdefault(
        "allowed_origin_protocol_pairs", {}
    )
    changed = False
    for origin in _NI_LOGIN_ORIGINS:
        entry = pairs.setdefault(origin, {})
        if not entry.get(config.URL_SCHEME):
            entry[config.URL_SCHEME] = True
            changed = True
    if changed:
        pref_file.with_name("Preferences.ni-wine-backup").write_text(text)
        pref_file.write_text(json.dumps(prefs, separators=(",", ":")))
    return changed


def _browser_checks(fix: bool) -> list[Check]:
    found = _default_browser_root()
    if found is None:
        return [Check(
            "browser login authorization",
            True,
            "default browser is not Chromium-family or not detected — "
            "approve the 'Open Native Access?' prompt manually on login",
            required=False,
        )]
    name, root = found
    pref_file = root / "Default" / "Preferences"
    origins = _allowed_origins(pref_file) if pref_file.is_file() else []

    if all(origin in origins for origin in _NI_LOGIN_ORIGINS):
        return [Check(f"{name}: login callback pre-authorized", True,
                      required=False)]

    if not fix:
        return [Check(
            f"{name}: login callback not pre-authorized",
            True,
            "the browser will ask on login — `ni doctor --fix` can pre-authorize it",
            required=False,
        )]

    if _browser_running(root):
        return [Check(
            f"{name}: login callback authorization",
            False,
            f"close {name} and re-run `ni doctor --fix`",
            required=False,
        )]
    if pref_file.is_file() and _seed_allow(pref_file):
        return [Check(
            f"{name}: login callback pre-authorized", True,
            "NI login origins may now open Native Access without asking",
            required=False,
        )]
    return [Check(
        f"{name}: login callback authorization", False,
        "could not update browser preferences", required=False,
    )]


def run_doctor(prefix: Path, *, fix: bool = False) -> int:
    if fix:
        if config.drive_c(prefix).is_dir():
            wine = Wine(prefix)
            if apply_prefix_tweaks(wine) and not native_access_running():
                # explorer.exe reads the tray settings only at startup.
                wine.kill_server()
            clear_updater_residue(prefix)
        ensure_url_handler()

    checks = [
        *_dependency_checks(),
        *_prefix_checks(prefix),
        _handler_check(),
        *_browser_checks(fix),
    ]

    print("ni-wine doctor")
    for check in checks:
        print(check.render())

    failures = [c for c in checks if not c.ok and c.required]
    if failures:
        print(f"\n{len(failures)} problem(s) found."
              + ("" if fix else "  Some are fixable with `ni doctor --fix`."))
        return 1
    print("\nAll good.")
    return 0
