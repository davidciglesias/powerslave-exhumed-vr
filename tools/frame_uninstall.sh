#!/bin/sh
# Uninstall PowerSlave Exhumed VR from the Steam Frame: puts the game's SDL2.dll back, removes the mod's files from the
# game folder and restores your previous kexengine.cfg (kexengine.cfg.before-vr).
#   sh frame_uninstall.sh [game folder]
# Runs frame_install.sh --uninstall: the copy next to this script if there is one, else the one from the latest release.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -f "$HERE/frame_install.sh" ]; then
    exec sh "$HERE/frame_install.sh" --uninstall "$@"
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsSL -o "$TMP/frame_install.sh" https://github.com/davidciglesias/powerslave-exhumed-vr/releases/latest/download/frame_install.sh
sh "$TMP/frame_install.sh" --uninstall "$@"
