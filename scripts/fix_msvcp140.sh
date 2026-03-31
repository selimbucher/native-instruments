#!/usr/bin/env bash
# fix-wine-msvcp140.sh
# Replaces Wine's stub msvcp140.dll with genuine Microsoft DLLs
# from the official VC++ 2022 redistributable.
#
# Usage: ./fix-wine-msvcp140.sh [WINEPREFIX]
# Default WINEPREFIX: ~/.wine-ni
#
# On NixOS:
#   nix-shell -p curl cabextract --run "./fix-wine-msvcp140.sh ~/.wine-ni"

set -euo pipefail

WINEPREFIX="${1:-$HOME/.wine-ni}"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

echo "==> Wine prefix: $WINEPREFIX"

# Check dependencies
for cmd in curl cabextract; do
  if ! command -v "$cmd" &>/dev/null; then
    echo "ERROR: '$cmd' not found. On NixOS run:"
    echo "  nix-shell -p curl cabextract --run \"$0 $WINEPREFIX\""
    exit 1
  fi
done

# Download VC++ 2022 redistributable
echo "==> Downloading VC++ 2022 x64 redistributable..."
curl -L --progress-bar \
  "https://aka.ms/vs/17/release/vc_redist.x64.exe" \
  -o "$TMPDIR/vc_redist.x64.exe"

# Extract outer cabinet (bootstrapper payload)
echo "==> Extracting outer cabinet..."
cabextract -d "$TMPDIR/stage1" "$TMPDIR/vc_redist.x64.exe" 2>/dev/null

# Find the inner cabinet containing amd64 DLLs
echo "==> Locating amd64 DLL cabinet..."
INNER_CAB=""
for f in "$TMPDIR/stage1"/a*; do
  if cabextract -l "$f" 2>/dev/null | grep -q "msvcp140.dll_amd64"; then
    INNER_CAB="$f"
    break
  fi
done

if [[ -z "$INNER_CAB" ]]; then
  echo "ERROR: Could not find inner cabinet with msvcp140.dll_amd64"
  exit 1
fi

echo "==> Found cabinet: $INNER_CAB"

# Extract DLLs from inner cabinet
echo "==> Extracting DLLs..."
cabextract -d "$TMPDIR/stage2" "$INNER_CAB" 2>/dev/null

SYSTEM32="$WINEPREFIX/drive_c/windows/system32"

copy_dll() {
  local src_name="$1"
  local dst_name="$2"
  local src="$TMPDIR/stage2/${src_name}"
  if [[ -f "$src" ]]; then
    echo "==> Installing $dst_name"
    cp "$src" "$SYSTEM32/$dst_name"
  else
    echo "WARNING: $src_name not found in cabinet, skipping"
  fi
}

# msvcp140 family - Wine's stubs are missing functions Kontakt needs
copy_dll "msvcp140.dll_amd64"             "msvcp140.dll"
copy_dll "msvcp140_1.dll_amd64"           "msvcp140_1.dll"
copy_dll "msvcp140_2.dll_amd64"           "msvcp140_2.dll"
copy_dll "msvcp140_atomic_wait.dll_amd64" "msvcp140_atomic_wait.dll"
copy_dll "msvcp140_codecvt_ids.dll_amd64" "msvcp140_codecvt_ids.dll"
copy_dll "concrt140.dll_amd64"            "concrt140.dll"

# vcruntime140 must also be native because the native msvcp140.dll
# links against it at the PE level and won't accept Wine's builtin stub
copy_dll "vcruntime140.dll_amd64"         "vcruntime140.dll"
copy_dll "vcruntime140_1.dll_amd64"       "vcruntime140_1.dll"
copy_dll "vcruntime140_threads.dll_amd64" "vcruntime140_threads.dll"

# Set DLL overrides to native,builtin for all replaced DLLs
echo "==> Setting DLL overrides..."
for dll in msvcp140 msvcp140_1 msvcp140_2 msvcp140_atomic_wait \
           msvcp140_codecvt_ids concrt140 \
           vcruntime140 vcruntime140_1 vcruntime140_threads; do
  WINEPREFIX="$WINEPREFIX" wine reg add \
    "HKCU\Software\Wine\DllOverrides" \
    /v "$dll" /t REG_SZ /d "native,builtin" /f \
    2>/dev/null || true
done

echo ""
echo "Done."
