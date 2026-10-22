#!/usr/bin/env bash
# Copies the built plugin into the user OBS plugins directory for local testing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${1:-$ROOT/build}"

SO="$BUILD_DIR/obs-widget-browser.so"
if [[ ! -f "$SO" ]]; then
    echo "error: $SO not found. Build first: cmake --build build" >&2
    exit 1
fi

DEST="${OBS_PLUGIN_DIR:-$HOME/.config/obs-studio/plugins}/obs-widget-browser"
rm -rf "$DEST"
mkdir -p "$DEST/bin/64bit" "$DEST/data/locale"
cp "$SO" "$DEST/bin/64bit/"
cp "$ROOT/data/locale/"*.ini "$DEST/data/locale/"

echo "Installed to $DEST"
echo "Restart OBS Studio to pick up the plugin."