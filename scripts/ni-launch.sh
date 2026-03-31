#!/usr/bin/env bash
set -euo pipefail

export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-ni}"

wine "$WINEPREFIX/drive_c/Program Files/Common Files/Native Instruments/NTK/NTKDaemon.exe"
wine "$WINEPREFIX/drive_c/users/$USER/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Native Access.lnk"
