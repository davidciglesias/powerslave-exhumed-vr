# PowerSlave Exhumed VR v0.1.0

First release of an unofficial VR mod for PowerSlave Exhumed (Steam), made for the Steam Frame through SteamVR.

## Features
- **True stereo 6DOF**: the world is rendered once per eye from your real head position.
- **Weapons in your right hand**: each weapon sits on the right controller as a 3D (voxel-extruded) model of its sprite,
  and fires where you point it.
- **Head-relative movement** on the left stick, turning on the right stick.
- **HUD moved into VR**:
  - Health, ammo and air (underwater) on a chest panel you see by looking down. It follows your body, not every head
    movement.
  - Pickup messages on a band just below your gaze.
  - Pause and other menus on their own panel.
- **Steam Frame controller layout**: every button is mapped (see the README's Controls table). Other OpenXR
  controllers get a reduced mapping.
- Game audio goes to the headset when streaming (`[Audio] Device`).

## Install
- **PC (streamed)**: follow https://github.com/davidciglesias/powerslave-exhumed-vr#install-on-a-pc-streamed
- **Steam Frame (standalone)**: follow https://github.com/davidciglesias/powerslave-exhumed-vr#install-on-the-steam-frame-standalone
  (**untested**: the script works, but the mod hasn't been played on the Frame itself yet)

## Uninstall
- **PC**: run the install command with `pc_uninstall.ps1` instead of `pc_install.ps1`.
- **Steam Frame**: download `frame_uninstall.sh` and run `sh frame_uninstall.sh` in Konsole.

Both put the game's `SDL2.dll` back and restore your previous `kexengine.cfg`.

## Known issues
- The image looks somewhat darker than the flat game.
- A dark rectangle can show mid-view during level loads. Damage and pickup flashes show on the message band only.
- The game's own health bar isn't shown. The chest panel replaces it.
- After taking the headset off and on, the pause menu can become an empty rectangle. Restart the game.
- The sky shifts slightly with head movement. Sprites tilt slightly with the head.
- No snap turn, recenter or seated-height option yet.
- Only works with Steam build 19569414 of the game. On other builds the mod falls back to a flat screen in the headset.

SHA-256 (`PowerslaveExhumedVR.zip`): `9e2709aeddbad6182bbd8a4e43365a7e84876b6e81a355f15e91811fa7181a63`
