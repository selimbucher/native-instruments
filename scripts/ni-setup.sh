#!/usr/bin/env bash
set -euo pipefail

export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-ni}"
export WINEARCH="${WINEARCH:-win64}"

echo "==> Initializing Wine prefix (dismissing Mono installer)..."
xvfb-dismiss 98 "Wine Mono Installer" Escape wineboot -i || true

echo "==> Disabling winemenubuilder (no .desktop files)..."
wine reg add 'HKCU\Software\Wine\DllOverrides' \
  /v 'winemenubuilder.exe' /t REG_SZ /d "" /f 2>/dev/null

echo "==> Removing home folder symlinks from Wine prefix..."
for link in \
  "$WINEPREFIX/drive_c/users/$USER/Desktop" \
  "$WINEPREFIX/drive_c/users/$USER/Documents" \
  "$WINEPREFIX/drive_c/users/$USER/My Documents" \
  "$WINEPREFIX/drive_c/users/$USER/Downloads" \
  "$WINEPREFIX/drive_c/users/$USER/Music" \
  "$WINEPREFIX/drive_c/users/$USER/My Music" \
  "$WINEPREFIX/drive_c/users/$USER/Pictures" \
  "$WINEPREFIX/drive_c/users/$USER/My Pictures" \
  "$WINEPREFIX/drive_c/users/$USER/Videos" \
  "$WINEPREFIX/drive_c/users/$USER/My Videos" \
  "$WINEPREFIX/drive_c/users/$USER/Templates" \
; do
  if [ -L "$link" ]; then
    rm "$link"
    mkdir -p "$link"
  fi
done

echo "==> Installing vcrun2022..."
xvfb-run --auto-servernum winetricks --unattended vcrun2022

echo "==> Installing PowerShell..."
xvfb-run --auto-servernum winetricks --unattended powershell

echo "==> Downloading Native Access installer..."
NA_INSTALLER="/tmp/Native-Access_2.exe"
CURL_ARGS="-L --progress-bar -o $NA_INSTALLER"
[ -f "$NA_INSTALLER" ] && CURL_ARGS="$CURL_ARGS -z $NA_INSTALLER"
curl $CURL_ARGS "https://www.native-instruments.com/fileadmin/downloads/Native-Access_2.exe"

echo "==> Installing Native Access..."
xvfb-dismiss 98 "Warning" Return wine "$NA_INSTALLER"
wineserver -k || true

echo "==> Installing NTKDaemon..."
NTK_INSTALLER=$(ls "$WINEPREFIX/drive_c/Program Files/Native Instruments/Native Access/resources/daemon/win/NTKDaemon "*.exe 2>/dev/null | head -1)
if [ -z "$NTK_INSTALLER" ]; then
  echo "Error: NTKDaemon installer not found" >&2
  exit 1
fi
xvfb-run --auto-servernum wine "$NTK_INSTALLER" /s
wineserver -k || true

echo "==> Done. Prefix ready at $WINEPREFIX"