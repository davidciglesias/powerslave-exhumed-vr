#!/bin/sh
# Install PowerSlave Exhumed VR for playing PowerSlave Exhumed on the Steam Frame itself (no PC).
#   sh frame_install.sh [--uninstall] [game folder]
# Run it in the Frame's desktop terminal (Konsole) with the game installed, started once and not running. It downloads
# this mod's release zip from GitHub and puts it in the game folder: the game's own SDL2.dll is renamed to SDL2_real.dll
# and the mod's SDL2.dll (which loads it) takes its place, next to openxr_loader.dll and PowerslaveVR.ini. It also sets
# the game's controller bindings and video settings for VR in kexengine.cfg inside the game's Proton prefix (your
# previous file is kept as kexengine.cfg.before-vr). Re-running it updates the mod and keeps your PowerslaveVR.ini.
# --uninstall puts the game's SDL2.dll back, removes the mod's files and restores kexengine.cfg.before-vr.
# Set PVR_ZIP to a local PowerslaveExhumedVR.zip (or a URL) to install that instead of the latest release.
set -eu

REPO=davidciglesias/powerslave-exhumed-vr
MOD_URL=${PVR_ZIP:-https://github.com/$REPO/releases/latest/download/PowerslaveExhumedVR.zip}
APPID=1678430
EXE=anubis_Shipping_Steam_x64.exe
EXPECTED_BUILD=689a8448   # PE timestamp of the game build the mod was made for (Steam build 19569414)

UNINSTALL=0
GAME=""
for a in "$@"; do
    case "$a" in
        --uninstall) UNINSTALL=1 ;;
        *) GAME="$a" ;;
    esac
done

# Find the game in any Steam library; its Proton prefix lives in the same library.
LIB=""
if [ -z "$GAME" ]; then
    STEAM="$HOME/.local/share/Steam"
    for lib in "$STEAM" $(grep -o '"path"[[:space:]]*"[^"]*"' "$STEAM/steamapps/libraryfolders.vdf" 2>/dev/null | sed 's/.*"\([^"]*\)"$/\1/'); do
        if [ -f "$lib/steamapps/common/PowerSlave Exhumed/$EXE" ]; then GAME="$lib/steamapps/common/PowerSlave Exhumed"; LIB="$lib"; break; fi
    done
else
    LIB=$(cd "$GAME/../../.." 2>/dev/null && pwd || true)
fi
if [ -z "$GAME" ] || [ ! -f "$GAME/$EXE" ]; then
    echo "PowerSlave Exhumed not found. Install it in Steam first, or pass its folder: sh frame_install.sh \"/path/to/PowerSlave Exhumed\"" >&2
    exit 1
fi
if command -v pgrep >/dev/null && pgrep -f "[a]nubis_Shipping_Steam_x64" >/dev/null; then echo "The game is running; quit it first." >&2; exit 1; fi
echo "Game folder: $GAME"

CFG_DIR=${PVR_CFG_DIR:-"$LIB/steamapps/compatdata/$APPID/pfx/drive_c/users/steamuser/Saved Games/Nightdive Studios/Powerslave EX"}   # PVR_CFG_DIR: for testing
CFG="$CFG_DIR/kexengine.cfg"
BACKUP="$CFG.before-vr"

# The mod's SDL2.dll forwards every export to SDL2_real.dll, so that name is inside it; the game's own one doesn't have it.
is_ours() { [ -f "$1" ] && grep -aq SDL2_real "$1"; }

if [ "$UNINSTALL" = 1 ]; then
    if [ -f "$GAME/SDL2_real.dll" ]; then
        mv -f "$GAME/SDL2_real.dll" "$GAME/SDL2.dll"
    elif is_ours "$GAME/SDL2.dll"; then
        echo "SDL2_real.dll is missing: run Verify integrity of game files in Steam to get the game's SDL2.dll back." >&2
    fi
    rm -f "$GAME/openxr_loader.dll" "$GAME/PowerslaveVR.ini" "$GAME/PowerslaveVR.log" "$GAME/PowerslaveVR_hand.raw"
    if [ -s "$BACKUP" ]; then
        mv -f "$BACKUP" "$CFG"
        echo "Restored your previous game settings ($CFG)."
    elif [ -f "$BACKUP" ]; then
        # the installer made kexengine.cfg from default.cfg; without it the game uses default.cfg again
        rm -f "$CFG" "$BACKUP"
        echo "Removed the VR game settings (the game uses its defaults again)."
    fi
    echo "Removed PowerSlave Exhumed VR."
    exit 0
fi

if [ -z "${PVR_CFG_DIR:-}" ] && [ ! -d "$LIB/steamapps/compatdata/$APPID/pfx/drive_c" ]; then
    echo "The game's Proton prefix doesn't exist yet. Start PowerSlave Exhumed once from Steam, quit it, then run this again." >&2
    exit 1
fi

# Warn when the game build differs: the mod then starts without VR rendering rather than crashing.
PE=$(od -An -tu4 -j60 -N4 "$GAME/$EXE" | tr -d ' ')
STAMP=$(od -An -tx4 -j$((PE + 8)) -N4 "$GAME/$EXE" | tr -d ' ')
if [ "$STAMP" != "$EXPECTED_BUILD" ]; then
    echo "This game build (0x$STAMP) is not the one the mod was made for (0x$EXPECTED_BUILD): the mod will only show a flat screen until it is updated." >&2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "Downloading PowerSlave Exhumed VR..."
case "$MOD_URL" in
    http*) curl -fL --retry 3 -o "$TMP/mod.zip" "$MOD_URL" ;;
    *) cp "$MOD_URL" "$TMP/mod.zip" ;;
esac
unzip -qo "$TMP/mod.zip" SDL2.dll openxr_loader.dll PowerslaveVR.ini -d "$TMP/mod"

# SDL2.dll that isn't ours is the game's (first install, or Steam restored it after an update/verify).
is_ours "$GAME/SDL2.dll" || mv -f "$GAME/SDL2.dll" "$GAME/SDL2_real.dll"
cp -f "$TMP/mod/SDL2.dll" "$TMP/mod/openxr_loader.dll" "$GAME/"
if [ ! -f "$GAME/PowerslaveVR.ini" ]; then
    # On the headset the game's audio already plays on the headset: use the default output device.
    sed 's/^Device=Steam Streaming Speakers/Device=/' "$TMP/mod/PowerslaveVR.ini" > "$GAME/PowerslaveVR.ini"
fi

# kexengine.cfg without its controller binds and the VR cvars, plus ours. If the game has no kexengine.cfg yet, it is made
# from the game's default.cfg (the game skips default.cfg once kexengine.cfg exists, so it must hold every bind).
# Binds: joy_N = SDL button N of the mod's virtual pad, joy_15/16 = left/right trigger. Cvars: D3D11 renderer (the mod
# hooks it), windowed, square 2048x2048 (= the resolution each eye is rendered at).
mkdir -p "$CFG_DIR"
if [ -f "$CFG" ]; then
    [ -f "$BACKUP" ] || cp "$CFG" "$BACKUP"
    tr -d '\r' < "$CFG" > "$TMP/base.cfg"
else
    unzip -p "$GAME/PowerslaveEX.kpf" default.cfg | tr -d '\r' > "$TMP/base.cfg"
    [ -s "$TMP/base.cfg" ] || { echo "default.cfg not found in the game's PowerslaveEX.kpf" >&2; exit 1; }
    [ -f "$BACKUP" ] || : > "$BACKUP"   # empty backup = there was no kexengine.cfg
fi
{
    grep -Ev '^[[:space:]]*bind[[:space:]]+joy_[0-9]+[[:space:]]|^[[:space:]]*seta?[[:space:]]+(r_rhirenderfamily|v_windowmode|v_width|v_height)[[:space:]]|^[[:space:]]*$' "$TMP/base.cfg" || true
    cat <<'EOF'
bind joy_0 "jump"
bind joy_1 "+use"
bind joy_2 "*weaponwheel"
bind joy_3 "inventoryscreen"
bind joy_4 "automap"
bind joy_9 "+weapprev"
bind joy_10 "+weapnext"
bind joy_11 "mapzoomin"
bind joy_12 "mapzoomout"
bind joy_15 "+use"
bind joy_16 "attack"
seta r_rhirenderfamily "rhi.d3d11"
seta v_windowmode "0"
seta v_width "2048"
seta v_height "2048"
EOF
} > "$TMP/new.cfg"
cp "$TMP/new.cfg" "$CFG"
echo "Game settings set for VR: $CFG"

echo
echo "Done. Launch PowerSlave Exhumed from the headset's library (no launch options needed)."
