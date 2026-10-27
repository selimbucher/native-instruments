"""Lay out Kontakt 8's files from its installer payload into drive_c.

NI's InstallAware installer carries an MSI plus an OFFLINE payload tree of
hex-named directories. The MSI's Directory/Component/File tables say where
each payload file belongs on C:. We replay that mapping since Wine's MSI
engine hangs on the package (shim/msi_shim.c).

plan_installer_dir(msi) plans from the payload the installer has unpacked
(the msi shim calls back into ni-wine at that point) and returns a Plan,
nothing in the prefix changes until Plan.execute.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

from . import config
from .util import die, info, which_first


def _infer_root(offline_dir: Path, path_segments: list[str]) -> str | None:
    """Infer the Windows base install path for one OFFLINE payload group."""
    # AAX plugin, useless under Wine.
    if "Contents" in path_segments and "x64" in path_segments:
        return None

    if path_segments and path_segments[0] == "Kontakt 8":
        return "Program Files/Common Files/Native Instruments"
    if path_segments and path_segments[0] == "Documentation":
        return "Program Files/Native Instruments/Kontakt 8"

    sample_files = [f.name for f in offline_dir.rglob("*") if f.is_file()]
    if any(f.endswith(".vst3") for f in sample_files):
        return "Program Files/Common Files/VST3"
    if any(f.endswith(".exe") for f in sample_files):
        return "Program Files/Native Instruments/Kontakt 8"
    if any(f.endswith(".json") for f in sample_files):
        return "users/Public/Documents/Native Instruments"
    if any("REX" in f or "sqlite" in f for f in sample_files):
        return "Program Files/Common Files/Native Instruments"
    if any(f.endswith(".rtf") for f in sample_files):
        return "Program Files/Native Instruments/Kontakt 8"
    return None


def _dump_msi_tables(msi: Path, idt_dir: Path) -> None:
    if (idt_dir / ".done").exists() and (idt_dir / "Directory.idt").exists():
        return
    idt_dir.mkdir(parents=True, exist_ok=True)
    msidump = which_first("msidump")
    if not msidump:
        die("msidump not found (install msitools)")
    subprocess.run(
        [msidump, "-t", str(msi)],
        check=True,
        cwd=idt_dir,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    (idt_dir / ".done").touch()


def _read_idt(idt_dir: Path, name: str) -> list[list[str]]:
    rows: list[list[str]] = []
    with open(idt_dir / f"{name}.idt", encoding="utf-8", errors="replace") as f:
        for line_number, line in enumerate(f):
            if line_number < 3:
                continue
            line = line.rstrip("\r\n")
            if line:
                rows.append(line.split("\t"))
    return rows


def _file_hash(path: Path) -> str:
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        while chunk := handle.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()[:12]


def _offline_in(root: Path) -> Path | None:
    for candidate in (root / "OFFLINE", root / "data" / "OFFLINE"):
        if candidate.is_dir():
            return candidate
    try:
        entries = [e for e in root.iterdir() if e.is_dir()]
    except OSError:
        return None
    for entry in entries:
        nested = entry / "OFFLINE"
        if nested.is_dir():
            return nested
    return None


def _find_offline(msi: Path) -> Path | None:
    r"""The OFFLINE payload tree for an msi: next to it after 7z, or in a sibling
    %TEMP%\mia<hex>.tmp when InstallAware unpacked it (newest first).
    """
    found = _offline_in(msi.parent)
    if found is not None:
        return found
    siblings = [d for d in msi.parent.parent.glob("mia*") if d.is_dir() and d != msi.parent]
    siblings.sort(key=lambda d: d.stat().st_mtime, reverse=True)
    for sibling in siblings:
        found = _offline_in(sibling)
        if found is not None:
            return found
    return None


KONTAKT8_INSTALL_DIR = "C:\\Program Files\\Native Instruments\\Kontakt 8\\"
KONTAKT8_CONTENT_DIR = "C:\\Program Files\\Common Files\\Native Instruments\\Kontakt 8\\"
KONTAKT8_VST3_DIR = "C:\\Program Files\\Common Files\\VST3\\"

# What the MSI's Registry table writes under HKLM\Software\Native Instruments\Kontakt 8.
# The daemon's evaluateInstallationState wants InstallDir/ContentDir, its
# install state roll forward reads the rest. InstallAAX64Dir is left out,
# no plugin host on Linux uses it.
KONTAKT8_REGISTRY = (
    ("InstallDir", KONTAKT8_INSTALL_DIR),
    ("ContentDir", KONTAKT8_CONTENT_DIR),
    ("ContentVersion", "4.0"),
    ("InstallVST364Dir", KONTAKT8_VST3_DIR),
)


def write_product_record(out_dir: Path) -> None:
    """The install record the InstallAware wrapper writes, read by the daemon."""
    json_dir = out_dir / "users/Public/Documents/Native Instruments/installed_products"
    json_dir.mkdir(parents=True, exist_ok=True)
    (json_dir / "Kontakt 8.json").write_text(
        json.dumps(
            {
                "ContentDir": KONTAKT8_CONTENT_DIR,
                "ContentVersion": "4.0.0",
                "InstallDir": KONTAKT8_INSTALL_DIR,
            }
        )
    )


class Plan:
    def __init__(self, copy_list: list[tuple[Path, str]], offline: Path) -> None:
        self.copy_list = copy_list
        self.offline = offline

    def execute(self, out_dir: Path, *, update: bool) -> None:
        info(f"installing to {out_dir}...")
        copied = 0
        for src, dest_dir in self.copy_list:
            dest = out_dir / dest_dir / src.name
            dest.parent.mkdir(parents=True, exist_ok=True)
            if not dest.exists() or update:
                # copy then rename, a plugin host that has the old file mapped keeps its inode
                tmp = dest.with_name(dest.name + ".ni-wine-tmp")
                shutil.copy2(src, tmp)
                os.replace(tmp, dest)
            copied += 1

        referenced = {str(src) for src, _ in self.copy_list}
        unmapped = sum(
            1 for f in self.offline.rglob("*") if f.is_file() and str(f) not in referenced
        )

        write_product_record(out_dir)

        info(f"done: {copied} files copied, {unmapped} payload files unused (expected)")


def _plan(msi: Path, offline: Path, idt_dir: Path) -> Plan:
    _dump_msi_tables(msi, idt_dir)

    # --- Directory table: key -> (parent, long name) -----------------------
    info("parsing directory table...")
    parent_map: dict[str, str] = {}
    longname_map: dict[str, str] = {}
    for row in _read_idt(idt_dir, "Directory"):
        if len(row) < 3:
            continue
        key, parent, defaultdir = row[0], row[1], row[2]
        parent_map[key] = parent
        longname = defaultdir.split("|", 1)[1] if "|" in defaultdir else defaultdir
        if ":" in longname:
            longname = longname.split(":")[0]
        longname_map[key] = longname

    def path_segments(key: str) -> list[str]:
        parts: list[str] = []
        current = key
        visited: set[str] = set()
        while current and current not in ("TARGETDIR", "SourceDir", "") and current not in visited:
            visited.add(current)
            name = longname_map.get(current, current)
            if name not in (".", "OFFLINE", "SourceDir", "GlobalAssemblyCache"):
                parts.append(name)
            if parent_map.get(current, "") in ("TARGETDIR", "SourceDir", ""):
                break
            current = parent_map.get(current, "")
        parts.reverse()
        return parts

    # --- Map OFFLINE hex1/hex2 dirs to Windows destinations ----------------
    children_map: dict[str, list[str]] = {}
    for key, parent in parent_map.items():
        children_map.setdefault(parent, []).append(key)

    offline_to_dest: dict[str, str] = {}
    offline_keys = {k for k, v in longname_map.items() if v == "OFFLINE"}
    for offline_key in offline_keys:
        segments = path_segments(parent_map.get(offline_key, ""))
        hex1_keys = [
            k
            for k in children_map.get(offline_key, [])
            if longname_map.get(k, "") not in (".", "OFFLINE", "")
        ]
        if not hex1_keys:
            continue
        sample_hex1 = longname_map.get(hex1_keys[0], "")
        sample_dir = offline / sample_hex1 if sample_hex1 else offline
        windows_root = _infer_root(sample_dir, segments)
        if not windows_root:
            continue
        rest = "/".join(segments)
        dest_dir = f"{windows_root}/{rest}" if rest else windows_root
        for hex1_key in hex1_keys:
            hex1 = longname_map.get(hex1_key, "")
            if not hex1 or hex1 in (".", "OFFLINE"):
                continue
            for hex2_key in children_map.get(hex1_key, []):
                hex2 = longname_map.get(hex2_key, "")
                if not hex2 or hex2 in (".", "OFFLINE"):
                    continue
                offline_to_dest[f"{hex1}/{hex2}"] = dest_dir
    info(f"mapped {len(offline_to_dest)} OFFLINE source dirs")

    # --- Component + File tables -> copy list ------------------------------
    comp_to_dir: dict[str, str] = {}
    for row in _read_idt(idt_dir, "Component"):
        if len(row) >= 3:
            comp_to_dir[row[0]] = row[2]

    copy_list: list[tuple[Path, str]] = []
    for row in _read_idt(idt_dir, "File"):
        if len(row) < 3:
            continue
        _file_key, component, filename = row[0], row[1], row[2]
        longname = filename.split("|", 1)[1] if "|" in filename else filename
        dir_key = comp_to_dir.get(component, "")
        if not dir_key:
            continue
        current = dir_key
        visited: set[str] = set()
        while current and current not in ("TARGETDIR", "SourceDir", "") and current not in visited:
            visited.add(current)
            parent = parent_map.get(current, "")
            if longname_map.get(parent, "") == "OFFLINE":
                hex1 = longname_map.get(current, "")
                hex2 = longname_map.get(dir_key, "")
                dest = offline_to_dest.get(f"{hex1}/{hex2}")
                if dest:
                    src = offline / hex1 / hex2 / longname
                    if src.exists():
                        copy_list.append((src, dest))
                break
            current = parent
    info(f"built {len(copy_list)} copy operations")
    if not copy_list:
        die("the installer's MSI tables map to no files, layout changed?")
    return Plan(copy_list, offline)


def _setup_exe_near(msi: Path) -> Path | None:
    """The setup exe the daemon unpacked msi from, if still around
    (%TEMP%\\<Product>_Installer\\<name> Setup PC.exe)."""
    temp = msi.parent.parent
    candidates = [
        exe
        for exe in temp.glob("*_Installer/*.exe")
        if exe.is_file() and exe.stat().st_size > 50 * 2**20
    ]
    candidates.sort(key=lambda exe: exe.stat().st_size, reverse=True)
    return candidates[0] if candidates else None


def _pristine_msi(setup_exe: Path) -> Path | None:
    """The untouched MSI from inside the setup exe (a 7z-readable SFX). InstallAware rewrites the
    one it hands to MsiInstallProduct and msitools can't read that. Cached per setup exe.
    """
    seven_zip = which_first("7z", "7zz", "7za")
    if not seven_zip:
        return None
    stat = setup_exe.stat()
    out = config.cache_dir() / "kontakt8" / f"pristine-{stat.st_size}-{int(stat.st_mtime)}"
    existing = list(out.glob("*.msi"))
    if existing:
        return existing[0]
    out.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [seven_zip, "e", str(setup_exe), f"-o{out}", "*.msi", "-r", "-y"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    found = list(out.glob("*.msi"))
    if result.returncode != 0 or not found:
        return None
    return found[0]


def plan_installer_dir(msi: Path) -> Plan:
    """Plan the layout of a payload the installer already unpacked. Tables come from the pristine
    MSI in the setup exe when available, otherwise from msi.
    """
    msi = msi.resolve()
    if not msi.is_file():
        die(f"{msi} does not exist")
    offline = _find_offline(msi)
    if offline is None:
        die(f"no OFFLINE payload directory found for {msi}")
    tables = msi
    setup_exe = _setup_exe_near(msi)
    if setup_exe is not None:
        pristine = _pristine_msi(setup_exe)
        if pristine is not None:
            info(f"reading tables from the pristine MSI in {setup_exe.name}")
            tables = pristine
    idt_dir = config.cache_dir() / "kontakt8" / f"idt-{_file_hash(tables)}"
    return _plan(tables, offline, idt_dir)
