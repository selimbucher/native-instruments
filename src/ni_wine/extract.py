"""Extract a Kontakt 8 installer zip into a Wine drive_c layout.

NI's installer is a zip containing a 7z-extractable exe, which holds an MSI
plus an OFFLINE payload tree of hex-named directories.  The MSI's Directory/
Component/File tables describe where each payload file belongs on C:.  We
replay that mapping ourselves instead of running the installer, because the
MSI's custom actions fail under Wine.

Extraction is cached per zip checksum (the 7z step is slow), keyed under the
ni-wine cache directory.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import zipfile
from pathlib import Path

from . import config
from .util import die, guarded_rmtree, info, which_first


def _infer_root(offline_dir: Path, path_segments: list[str]) -> str | None:
    """Infer the Windows base install path for one OFFLINE payload group."""
    # AAX plugin — not usable under Wine, skip entirely.
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


def _extract_payload(zip_path: Path, cache: Path) -> None:
    """Unpack zip → exe → MSI + OFFLINE tree into *cache* (skips if done)."""
    if (cache / ".done").exists():
        info(f"using cached extraction at {cache}")
        return

    # Drop caches of other installer versions so the cache dir stays bounded.
    cache_root = cache.parent
    for old in cache_root.glob("cache-*"):
        if old != cache:
            guarded_rmtree(old)
    guarded_rmtree(cache)
    cache.mkdir(parents=True)

    info("extracting zip...")
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(cache / "zip")
    exes = list((cache / "zip").rglob("*.exe"))
    if not exes:
        die("no .exe found inside the installer zip")

    seven_zip = which_first("7z", "7zz", "7za")
    if not seven_zip:
        die("7z not found (install p7zip / 7zip)")
    info("extracting installer exe (slow, cached after first run)...")
    subprocess.run(
        [seven_zip, "x", str(exes[0]), f"-o{cache}/exe", "-y"],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    (cache / ".done").touch()


def _dump_msi_tables(msi: Path, idt_dir: Path) -> None:
    if (idt_dir / ".done").exists() and (idt_dir / "Directory.idt").exists():
        return
    idt_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["msidump", "-t", str(msi)], check=True, cwd=idt_dir, stdout=subprocess.DEVNULL
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


def extract_kontakt8(zip_path: Path, out_dir: Path, *, update: bool = False) -> None:
    """Extract *zip_path* into a drive_c layout at *out_dir*.

    With update=True existing destination files are overwritten.
    """
    zip_path = zip_path.resolve()
    zip_hash = hashlib.md5(zip_path.read_bytes()).hexdigest()[:12]
    cache = config.cache_dir() / "kontakt8" / f"cache-{zip_hash}"

    _extract_payload(zip_path, cache)

    msis = list((cache / "exe").rglob("*.msi"))
    if not msis:
        die("no .msi found in extracted installer")
    msi = msis[0].resolve()

    offline_dirs = [p for p in (cache / "exe").rglob("OFFLINE") if p.is_dir()]
    if not offline_dirs:
        die("no OFFLINE payload directory found in extracted installer")
    offline = offline_dirs[0].resolve()

    idt_dir = cache / "idt"
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
        """Walk up to the product root, collecting meaningful segments."""
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

    # --- Execute ------------------------------------------------------------
    info(f"installing to {out_dir}...")
    copied = 0
    for src, dest_dir in copy_list:
        dest = out_dir / dest_dir / src.name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists() or update:
            shutil.copy2(src, dest)
        copied += 1

    referenced = {str(src) for src, _ in copy_list}
    unmapped = sum(
        1 for f in offline.rglob("*") if f.is_file() and str(f) not in referenced
    )

    # Minimal product manifest — the full manifest the MSI would write
    # confuses NTKDaemon under Wine (it flags the install as broken).
    json_dir = out_dir / "users/Public/Documents/Native Instruments/installed_products"
    json_dir.mkdir(parents=True, exist_ok=True)
    (json_dir / "Kontakt 8.json").write_text(
        '{"InstallDir":"C:\\\\Program Files\\\\Native Instruments\\\\Kontakt 8\\\\"}'
    )

    info(f"done: {copied} files copied, {unmapped} payload files unused (expected)")
