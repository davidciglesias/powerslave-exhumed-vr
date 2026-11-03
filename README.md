# PowerSlave Exhumed VR

An unofficial VR mod for Nightdive's **PowerSlave Exhumed** (Steam): true stereo 6DOF rendering of the game world,
weapons held and aimed with the right controller, head-relative movement, and the HUD and menus moved into VR
(health/ammo/air on a chest panel, messages below your gaze, menus on their own panel).

Made for the **Steam Frame** through SteamVR (OpenXR). Other SteamVR headsets should work too, but haven't been tested yet.
This is a first release (0.1.0): very playable, with some rough edges listed under [Known issues](#known-issues).

## What you need

- PowerSlave Exhumed **from Steam**, current build (Steam build 19569414). The mod hooks this exact build. On any other
  build the mod only shows the game on a flat screen in the headset (no 3D, no hand aiming) instead of crashing. Then wait for a mod update.
- SteamVR as the active OpenXR runtime.
- Steam Frame controllers (Frame layout), or other OpenXR controllers with A/B/X/Y (fewer buttons mapped).

You can play in two ways:
- **PC (streamed)**: the game runs on a Windows PC and streams to the headset (Steam Link / SteamVR).
- **Steam Frame (standalone)**: the game runs on the Frame itself, through Proton. **Untested so far**: the
  install script is ready, but nobody has played the mod this way yet. Reports are welcome.

## Install on a PC (streamed)

1. Install PowerSlave Exhumed in Steam. Quit it if it's running.
2. Open PowerShell (Start menu > "PowerShell") and run:

   ```
   powershell -ExecutionPolicy Bypass -Command "& ([scriptblock]::Create((irm https://github.com/davidciglesias/powerslave-exhumed-vr/releases/latest/download/pc_install.ps1)))"
   ```

   The script ([`tools/pc_install.ps1`](tools/pc_install.ps1), read it first if you like) finds the game in your Steam
   libraries, downloads this mod's `PowerslaveExhumedVR.zip` and installs it into the game folder. The game's
   `SDL2.dll` becomes `SDL2_real.dll`, and the mod's `SDL2.dll` loads it and adds VR. It also sets the game's
   controller bindings and video settings for VR in `Saved Games\Nightdive Studios\Powerslave EX\kexengine.cfg` (see
   [Game settings](#game-settings)). Your previous file is kept as `kexengine.cfg.before-vr`. If the game isn't found,
   pass `-GameFolder "D:\...\PowerSlave Exhumed"`. Run the script again to update the mod: your `PowerslaveVR.ini`
   is kept.
3. Connect the headset to SteamVR, then launch the game from Steam as usual. No launch options are needed.

**Uninstall:** run the same command with `pc_uninstall.ps1` instead of `pc_install.ps1` (or `pc_install.ps1 -Uninstall`).
It puts the game's `SDL2.dll` back, removes the mod's files and restores `kexengine.cfg.before-vr`.

**By hand:** in the game folder, rename `SDL2.dll` to `SDL2_real.dll`. Then extract `SDL2.dll`, `openxr_loader.dll`
and `PowerslaveVR.ini` from the zip into the game folder, and make the [Game settings](#game-settings) changes yourself.

## Install on the Steam Frame (standalone)

1. Install PowerSlave Exhumed on the Frame from your Steam library. **Start it once** (that creates its Proton
   prefix), then quit.
2. Open the Frame's desktop, start the terminal (Konsole) and run:

   ```
   curl -fsSLO https://github.com/davidciglesias/powerslave-exhumed-vr/releases/latest/download/frame_install.sh
   sh frame_install.sh
   ```

   The script ([`tools/frame_install.sh`](tools/frame_install.sh)) does the same as the PC one, inside the game's
   Proton prefix. It also sets `[Audio] Device` to empty, which means the default output: on the headset, that already
   is the headset. Pass the game folder as an argument if it isn't found. Run it again to update; your
   `PowerslaveVR.ini` is kept.
3. Launch the game from the headset's library. No launch options are needed: Proton loads the mod's `SDL2.dll` from
   the game folder by itself.

**Uninstall:**
```
curl -fsSLO https://github.com/davidciglesias/powerslave-exhumed-vr/releases/latest/download/frame_uninstall.sh
sh frame_uninstall.sh
```
(or `sh frame_install.sh --uninstall`).

## Controls (Steam Frame controllers)

| Input | Action |
|---|---|
| Left stick | Move, relative to where you look |
| Right stick (left/right) | Turn |
| Right trigger | Fire (aim with the right controller) |
| Left trigger, B | Use / open |
| A | Jump |
| X | Weapon wheel |
| Y | Inventory |
| View (left) | Map |
| Left / right bumper | Previous / next weapon |
| D-pad up / down | Map zoom in / out |
| Menu (right) | Pause menu |
| Left grip + View | Toggle a flat screen view (e.g. to fix a menu) |

Other controllers: A/B on the right, X/Y on the left, Menu on the left. There's no D-pad, View or bumpers.

## Game settings

The install scripts change these in `kexengine.cfg`. If the game has no `kexengine.cfg` yet, the script creates one
from the game's own `default.cfg`. Once the file exists, the game no longer reads `default.cfg`.
- all `bind joy_*` lines are replaced with the VR bindings above;
- `r_rhirenderfamily "rhi.d3d11"`: the mod works with the D3D11 renderer (the game defaults to Vulkan);
- `v_windowmode "0"`, `v_width "2048"`, `v_height "2048"`: the game window's resolution is the resolution each eye is
  rendered at. Keep it square. Lower it (e.g. 1600) if the frame rate struggles.

## Settings

`PowerslaveVR.ini` in the game folder (comments in the file explain each key). The ones you're most likely to touch:
- `[Render] WorldScale`: game units per real meter (64). Higher makes the world look smaller.
- `[Weapon]`: where the weapon sits on the right controller (`Forward`, `Up`, `Width`), its 3D thickness.
- `[Hud]`: menu panel size/distance (`MenuWidth`, `Distance`), message band height below the gaze (`MsgPitch`).
- `[Gauges]`: the chest panel (health, ammo, air underwater): `Width`, `Down`, `Forward`, `Tilt`, and `Deadzone`, how
  far you turn your head before the panel turns with you.
- `[Audio] Device`: game audio goes to the first output whose name contains this (PC default: Steam Streaming
  Speakers, so sound plays on a streamed Frame). Leave it empty for the Windows default.
- `[General] EnableXR=0` turns the mod off without uninstalling.

A log is written to `PowerslaveVR.log` in the game folder.

## Known issues

- The image looks somewhat darker than the flat game, menus included.
- A dark rectangle can show in the middle of the view during level loads and full-screen flashes. Damage and pickup
  flashes appear on the message band, not over the whole view.
- The original status bar is replaced by the chest panel. The game's own health bar isn't shown at the mod's square
  render size.
- After taking the headset off and on, the pause menu can turn into an empty rectangle. Restart the game if it does.
- The sky moves slightly with your head, and its texture edge can be visible when looking straight up. Sprites tilt
  slightly with the head.
- No snap turn, recenter or seated-height option yet.
- Steam Frame standalone hasn't been tested yet (see above).

## Building from source

Windows, Visual Studio 2026 (C++ workload, bundled CMake/Ninja), Python 3.12. Put MinHook in
`third_party/minhook-master` and the OpenXR SDK release (Windows) in `third_party/openxr`, then run
`tools\build.ps1`. It needs the game installed, because `tools\gen_def.py` reads the real `SDL2.dll` exports to
generate the forwarders. It builds `build\SDL2.dll` and the unit tests `build\pvr_tests.exe`. `tools\deploy.ps1`
copies a dev build into the game, and `tools\package.ps1` makes the release zip in `dist\`. A headset-free test
harness (`[Debug] FakeXR=1`, scripts in `tests/scripts`) is described in `src/xr/fake_xr.cpp`.

## Credits and license

MIT, see [LICENSE](LICENSE). Uses MinHook and the OpenXR loader, see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
PowerSlave Exhumed is © Nightdive Studios. This mod isn't affiliated with or endorsed by Nightdive Studios or Valve,
and contains none of the game's files.
