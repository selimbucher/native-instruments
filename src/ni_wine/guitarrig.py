"""Guitar Rig's preset database, AppData\\Local\\Native Instruments\\Guitar Rig N\\Database.

grNdb holds the presets, User.kad the tag vocabulary. A first start cut short (plugin scan,
closed window) leaves User.kad an empty skeleton and grNdb at 0 bytes, and later starts keep
it: GR6 crashes on the first imported preset, GR7 imports nothing. Removing the folder heals it.

The database takes one process (the lock file next to it), a second one saves nothing. yabridge
runs VST2 instances, scanner children and sandboxed instances in separate processes, a plugin
group keeps them in one.
"""

from __future__ import annotations

import fnmatch
import os
import tomllib
from pathlib import Path

from . import config
from .util import guarded_rmtree
from .wine import foreign_prefix_users

VERSIONS = (6, 7)

# yabridge.toml sits next to the bridged .so files, the first matching glob wins
_YABRIDGE_DIRS = (".vst/yabridge", ".vst3/yabridge")


def installed(prefix: Path, version: int) -> bool:
    return (
        config.drive_c(prefix) / f"Program Files/Common Files/VST3/Guitar Rig {version}.vst3"
    ).exists()


def database_dir(prefix: Path, version: int) -> Path:
    return (
        config.drive_c(prefix)
        / "users"
        / config.wine_user(prefix)
        / f"AppData/Local/Native Instruments/Guitar Rig {version}/Database"
    )


def broken_database(prefix: Path, version: int) -> Path | None:
    """The Database folder if a cut-short first start left it half-made."""
    folder = database_dir(prefix, version)
    db = folder / f"gr{version}db"
    kad = folder / "User.kad"
    if not (db.exists() or kad.exists()):
        return None
    if db.is_file() and db.stat().st_size > 0:
        return None
    if kad.is_file() and b"<attribute " in kad.read_bytes():
        return None
    return folder


def running(prefix: Path) -> bool:
    """A Guitar Rig standalone, or a DAW's plugin hosts (one of them may be Guitar Rig)."""
    if foreign_prefix_users(prefix):
        return True
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            # Wine names the process after the exe, cut to 15 chars: "Guitar Rig 6.ex"
            comm = Path(f"/proc/{entry.name}/comm").read_text()
        except OSError:
            continue
        if comm.startswith("Guitar Rig "):
            return True
    return False


def reset(prefix: Path) -> list[int]:
    """Remove half-made databases, Guitar Rig rebuilds them on its next start."""
    fixed = []
    for version in VERSIONS:
        folder = broken_database(prefix, version)
        if folder is None:
            continue
        guarded_rmtree(folder, prefix=prefix)
        fixed.append(version)
    return fixed


def _group_of(sections: dict, name: str) -> str:
    for pattern, options in sections.items():
        if fnmatch.fnmatchcase(name, pattern):
            return options.get("group", "") if isinstance(options, dict) else ""
    return ""


def yabridge_groups(home: Path | None = None) -> dict[Path, dict[str, str]]:
    """Per yabridge.toml, the Guitar Rig plugins next to it and the group each is in ('' = none)."""
    result: dict[Path, dict[str, str]] = {}
    for rel in _YABRIDGE_DIRS:
        folder = (home or Path.home()) / rel
        if not folder.is_dir():
            continue
        names = sorted(
            p.name
            for p in folder.iterdir()
            if p.name.startswith("Guitar Rig ") and p.suffix in (".so", ".vst3")
        )
        if not names:
            continue
        toml = folder / "yabridge.toml"
        try:
            sections = tomllib.loads(toml.read_text())
        except (OSError, tomllib.TOMLDecodeError):
            sections = {}
        result[toml] = {name: _group_of(sections, name) for name in names}
    return result


def group_plugins(home: Path | None = None) -> list[Path]:
    """Append a plugin group per Guitar Rig version where yabridge.toml has none."""
    written = []
    for toml, plugins in yabridge_groups(home).items():
        text = toml.read_text() if toml.is_file() else ""
        lines = []
        for name, group in plugins.items():
            if group or f'["{name}"]' in text:
                continue
            version = name.rsplit(".", 1)[0].rsplit(" ", 1)[1]
            lines += [f'["{name}"]', f'group = "guitar-rig-{version}"', ""]
        if not lines:
            continue
        if text and not text.endswith("\n"):
            text += "\n"
        if text:
            text += "\n"
        text += "# ni-wine: Guitar Rig saves presets from one process only\n" + "\n".join(lines)
        toml.write_text(text)
        written.append(toml)
    return written
