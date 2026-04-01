#!/usr/bin/env python3
"""
extract_kontakt8.py - Extract Kontakt 8 installer to a Wine drive_c layout

Usage:
    python3 extract_kontakt8.py Kontakt_8_Installer.zip
    OUT=/tmp/k8_test python3 extract_kontakt8.py Kontakt_8_Installer.zip
    K8_CACHE=/tmp/k8_cache python3 extract_kontakt8.py Kontakt_8_Installer.zip

Re-runs skip the slow extraction step. Delete K8_CACHE to force re-extract.
"""

import os, sys, shutil, subprocess, zipfile
from pathlib import Path

ZIP   = Path(sys.argv[1]) if len(sys.argv) > 1 else None
OUT   = Path(os.environ.get("OUT",      "/tmp/k8_drive_c"))
UPDATE = os.environ.get("K8_UPDATE", "") == "1"  # if set, overwrite existing files
# Cache dir is determined after ZIP is validated (keyed on zip checksum)

if not ZIP:
    print("Usage: OUT=<dir> python3 extract_kontakt8.py <Kontakt_8_Installer.zip>", file=sys.stderr)
    sys.exit(1)

# Cache keyed on zip checksum so different versions never collide
import hashlib, glob
zip_hash = hashlib.md5(ZIP.read_bytes()).hexdigest()[:12]
CACHE = Path(os.environ.get("K8_CACHE", f"/tmp/k8_cache_{zip_hash}"))

# ---------------------------------------------------------------------------
# Hex-decode NI's MSI key encoding (5c=\, 20=space, 2d=-, ...)
# ---------------------------------------------------------------------------
HEX_MAP = {
    "5c": "/", "20": " ", "2d": "-", "2e": ".", "28": "(", "29": ")",
    "5b": "[", "5d": "]", "40": "@", "2b": "+", "2c": ",", "27": "'",
    "21": "!", "26": "&", "23": "#", "25": "%",
}
def decode_key(key: str) -> str:
    for h, c in HEX_MAP.items():
        key = key.replace(h, c)
    return key

# ---------------------------------------------------------------------------
# Infer Windows install root from OFFLINE directory contents
# Called once per product root after OFFLINE subdir is known
# ---------------------------------------------------------------------------
def infer_root(offline_dir: Path, path_segments: list[str]) -> str | None:
    """
    Infer the Windows base install path from:
    - path_segments: decoded dir names between product root and OFFLINE
    - files inside the OFFLINE hex subdirs
    """
    # AAX plugin - skip entirely
    if "Contents" in path_segments and "x64" in path_segments:
        return None

    # Path starts with "Kontakt 8" subdir -> Common Files/NI
    if path_segments and path_segments[0] == "Kontakt 8":
        return "Program Files/Common Files/Native Instruments"

    # Path starts with "Documentation" -> NI program files
    if path_segments and path_segments[0] == "Documentation":
        return "Program Files/Native Instruments/Kontakt 8"

    # No path segments - infer from files in the OFFLINE subdirs
    sample_files = [f.name for f in offline_dir.rglob("*") if f.is_file()]
    sample_names = " ".join(sample_files)

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

# ---------------------------------------------------------------------------
# 1. Extract zip -> exe -> MSI + OFFLINE  (cached)
# ---------------------------------------------------------------------------
if (CACHE / ".done").exists():
    print(f"==> Using cached extraction at {CACHE}")
else:
    # Clean up other version caches to avoid filling /tmp
    for old_dir in glob.glob("/tmp/k8_cache_*"):
        if old_dir != str(CACHE):
            shutil.rmtree(old_dir, ignore_errors=True)
    shutil.rmtree(CACHE, ignore_errors=True)
    CACHE.mkdir(parents=True)
    print("==> Extracting zip...")
    with zipfile.ZipFile(ZIP) as zf:
        zf.extractall(CACHE / "zip")
    exes = list((CACHE / "zip").rglob("*.exe"))
    if not exes:
        print("ERROR: No .exe found in zip", file=sys.stderr); sys.exit(1)
    print("==> Extracting installer exe (slow, cached after first run)...")
    subprocess.run(["7z", "x", str(exes[0]), f"-o{CACHE}/exe", "-y"],
                   check=True, stdout=subprocess.DEVNULL)
    (CACHE / ".done").touch()

msis = list((CACHE / "exe").rglob("*.msi"))
if not msis:
    print("ERROR: No .msi found", file=sys.stderr); sys.exit(1)
MSI = msis[0]

offlines = [p for p in (CACHE / "exe").rglob("OFFLINE") if p.is_dir()]
if not offlines:
    print("ERROR: No OFFLINE dir found", file=sys.stderr); sys.exit(1)
OFFLINE = offlines[0]

print(f"==> MSI:     {MSI}")
print(f"==> OFFLINE: {OFFLINE}")

# ---------------------------------------------------------------------------
# 2. Dump MSI tables (cached)
# ---------------------------------------------------------------------------
IDT_DIR = CACHE / "idt"
if not (IDT_DIR / ".done").exists() or not (IDT_DIR / "Directory.idt").exists():
    IDT_DIR.mkdir(parents=True, exist_ok=True)
    subprocess.run(["msidump", "-t", str(MSI)], check=True, cwd=IDT_DIR,
                   stdout=subprocess.DEVNULL)
    (IDT_DIR / ".done").touch()
print(f"==> IDT dir: {IDT_DIR}")

def read_idt(name: str) -> list[list[str]]:
    rows = []
    with open(IDT_DIR / f"{name}.idt", encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f):
            if i < 3: continue
            line = line.rstrip("\r\n")
            if line:
                rows.append(line.split("\t"))
    return rows

# ---------------------------------------------------------------------------
# 3. Parse Directory.idt
#    DefaultDir format: "SHORT~1|LongName:." where ":." is source/dest pair
#    We only want the LongName part, stripping the ":." suffix.
# ---------------------------------------------------------------------------
print("==> Parsing directory table...")

parent_map:   dict[str, str] = {}   # key -> parent key
longname_map: dict[str, str] = {}   # key -> long folder name

for row in read_idt("Directory"):
    if len(row) < 3: continue
    key, parent, defaultdir = row[0], row[1], row[2]
    parent_map[key] = parent
    # Long name is after | if present, else use full value
    longname = defaultdir.split("|", 1)[1] if "|" in defaultdir else defaultdir
    # Strip ":." or ":" suffix (source path specifier)
    if ":" in longname:
        longname = longname.split(":")[0]
    longname_map[key] = longname

# Walk up the parent chain to build full dest path for a key
def get_path_segments(key: str) -> list[str]:
    """Walk up from key to product root, collecting meaningful path segments."""
    parts = []
    k = key
    visited: set[str] = set()
    while k and k not in ("TARGETDIR", "SourceDir", "") and k not in visited:
        visited.add(k)
        name = longname_map.get(k, k)
        if name not in (".", "OFFLINE", "SourceDir", "GlobalAssemblyCache"):
            parts.append(name)
        # Stop at product root (parent is TARGETDIR)
        if parent_map.get(k, "") in ("TARGETDIR", "SourceDir", ""):
            break
        k = parent_map.get(k, "")
    parts.reverse()
    # Strip product root entry (first component, e.g. "P4CACAB4_1" name which is ".")
    # Already excluded by the "." filter above, so parts starts with real path
    return parts

# ---------------------------------------------------------------------------
# 4. Build OFFLINE subpath -> dest_dir map
#    An OFFLINE dir is any directory whose longname is exactly "OFFLINE".
#    Its children are hex1 dirs, grandchildren are hex2 dirs.
#    The dest is the path of the OFFLINE dir's parent.
# ---------------------------------------------------------------------------
offline_to_dest: dict[str, str] = {}   # "HEX1/HEX2" -> dest_dir

offline_keys = {k for k, v in longname_map.items() if v == "OFFLINE"}

# Build reverse parent map for fast children lookup
children_map: dict[str, list[str]] = {}
for k, p in parent_map.items():
    children_map.setdefault(p, []).append(k)

for ok in offline_keys:
    parent_key = parent_map.get(ok, "")
    segments = get_path_segments(parent_key)
    hex1_keys = [k for k in children_map.get(ok, [])
                 if longname_map.get(k, "") not in (".", "OFFLINE", "")]
    if not hex1_keys:
        continue
    sample_hex1 = longname_map.get(hex1_keys[0], "")
    sample_offline_dir = OFFLINE / sample_hex1 if sample_hex1 else OFFLINE
    windows_root = infer_root(sample_offline_dir, segments)
    if not windows_root:
        continue
    rest = "/".join(segments)
    dest_dir = f"{windows_root}/{rest}" if rest else windows_root
    for hex1_key in hex1_keys:
        hex1_name = longname_map.get(hex1_key, "")
        if not hex1_name or hex1_name in (".", "OFFLINE"):
            continue
        for hex2_key in children_map.get(hex1_key, []):
            hex2_name = longname_map.get(hex2_key, "")
            if not hex2_name or hex2_name in (".", "OFFLINE"):
                continue
            offline_to_dest[f"{hex1_name}/{hex2_name}"] = dest_dir

print(f"==> Mapped {len(offline_to_dest)} OFFLINE source dirs")

# ---------------------------------------------------------------------------
# 5. Parse Component.idt -> component -> directory key
# ---------------------------------------------------------------------------
print("==> Parsing component table...")
comp_to_dir: dict[str, str] = {}
for row in read_idt("Component"):
    if len(row) >= 3:
        comp_to_dir[row[0]] = row[2]

# ---------------------------------------------------------------------------
# 6. Parse File.idt -> build copy list: (src_path, dest_dir, filename)
#    Each file entry maps to exactly one (hex1, hex2, dest) via its component.
#    We preserve all entries including duplicate filenames going to different dirs.
# ---------------------------------------------------------------------------
print("==> Parsing file table...")
# list of (src_path, dest_dir) — all copies to perform
copy_list: list[tuple[Path, str]] = []

for row in read_idt("File"):
    if len(row) < 3: continue
    _filekey, comp, filename = row[0], row[1], row[2]
    longname = filename.split("|", 1)[1] if "|" in filename else filename

    dir_key = comp_to_dir.get(comp, "")
    if not dir_key: continue

    # Walk up from dir_key to find hex2 (child of OFFLINE) and hex1 (child of OFFLINE's parent)
    k = dir_key
    visited: set[str] = set()
    while k and k not in ("TARGETDIR", "SourceDir", "") and k not in visited:
        visited.add(k)
        p = parent_map.get(k, "")
        if longname_map.get(p, "") == "OFFLINE":
            # k is hex1_key, dir_key is hex2_key
            hex1 = longname_map.get(k, "")
            hex2 = longname_map.get(dir_key, "")
            dest = offline_to_dest.get(f"{hex1}/{hex2}")
            if dest:
                src = OFFLINE / hex1 / hex2 / longname
                if src.exists():
                    copy_list.append((src, dest))
            break
        k = p

print(f"==> Built {len(copy_list)} copy operations")

# ---------------------------------------------------------------------------
# 7. Execute copies
# ---------------------------------------------------------------------------
print(f"==> Installing to {OUT}...")
copied = missing = unmapped = 0

for src, dest_dir in copy_list:
    dest = OUT / dest_dir / src.name
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.exists() or UPDATE:
        shutil.copy2(src, dest)
    copied += 1

# Count unmapped: OFFLINE files not referenced by any File table entry
referenced = {str(src) for src, _ in copy_list}
for f in OFFLINE.rglob("*"):
    if f.is_file() and str(f) not in referenced:
        unmapped += 1

# ---------------------------------------------------------------------------
# 9. Write installed_products JSON
# ---------------------------------------------------------------------------
json_dir = OUT / "users/Public/Documents/Native Instruments/installed_products"
json_dir.mkdir(parents=True, exist_ok=True)
(json_dir / "Kontakt 8.json").write_text(
    '{"InstallDir":"C:\\\\Program Files\\\\Native Instruments\\\\Kontakt 8\\\\"}'
)

print()
print("==> Done.")
print(f"    Copied:   {copied}")
print(f"    Missing:  {missing}  (dest known but file not in OFFLINE)")
print(f"    Unmapped: {unmapped}  (in OFFLINE but no dest — likely unused)")
print(f"    Output:   {OUT}")
