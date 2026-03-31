#!/usr/bin/env bash
set -euo pipefail

ARCHIVE_URL="https://slimmer.ch/Kontakt_8.tar.xz"
ARCHIVE_FILE="/tmp/Kontakt_8.tar.xz"
EXTRACT_DIR="/tmp/Kontakt_8"
DEST="$HOME/.wine-ni/drive_c"

ACTIVATE=false

# Parse args
for arg in "$@"; do
  case "$arg" in
    -a|--activate)
      ACTIVATE=true
      ;;
    *)
      echo "Unknown argument: $arg" >&2
      echo "Usage: $0 [-a|--activate]" >&2
      exit 1
      ;;
  esac
done

# Download if not already cached
if [ ! -f "$ARCHIVE_FILE" ]; then
  echo "Downloading $ARCHIVE_URL ..."
  curl -L --progress-bar -o "$ARCHIVE_FILE" "$ARCHIVE_URL"
else
  echo "Archive already exists at $ARCHIVE_FILE, skipping download."
fi

# Extract
echo "Extracting to $EXTRACT_DIR ..."
rm -rf "$EXTRACT_DIR"
mkdir -p "$EXTRACT_DIR"
tar -xf "$ARCHIVE_FILE" -C "$EXTRACT_DIR"

SRC="$EXTRACT_DIR"

mkdir -p "$DEST"

if $ACTIVATE; then
  echo "Activate mode: copying drive_c/users -> $DEST/users"
  mkdir -p "$DEST/users"
  cp -r "$SRC/users/." "$DEST/users"
else
  echo "Full copy: $SRC -> $DEST"
  cp -r "$SRC/." "$DEST"
fi

echo "Done."