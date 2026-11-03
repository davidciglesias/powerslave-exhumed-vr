# PowerSlave Exhumed VR v0.1.1

## New
- **The left stick moves through menus**, for controllers without a D-pad (e.g. Quest Touch).
  - This works on the title and main menus, loading and level-map screens, and the pause menu.
  - Holding a direction repeats it.
  - In menus the stick no longer moves the player. After a menu opens, let the stick return to centre once before it
    starts navigating, so a held stick doesn't jump the selection.
  - The real D-pad keeps working as before.

## Install
- **PC (streamed)**: follow https://github.com/davidciglesias/powerslave-exhumed-vr#install-on-a-pc-streamed
- **Steam Frame (standalone)**: follow https://github.com/davidciglesias/powerslave-exhumed-vr#install-on-the-steam-frame-standalone
  (**untested**: the script works, but the mod hasn't been played on the Frame itself yet)

## Update
- **PC**: run the `pc_install.ps1` command again. Your `PowerslaveVR.ini` and game settings are kept.
- **Steam Frame**: run `sh frame_install.sh` again in Konsole.

## Uninstall
- **PC**: run the install command with `pc_uninstall.ps1` instead of `pc_install.ps1`.
- **Steam Frame**: download `frame_uninstall.sh` and run `sh frame_uninstall.sh` in Konsole.

## Known issues
Same as 0.1.0:
- The image looks somewhat darker than the flat game.
- A dark rectangle can show mid-view during level loads.
- The game's own health bar isn't shown. The chest panel replaces it.
- After taking the headset off and on, the pause menu can become an empty rectangle.
- No snap turn, recenter or seated-height option yet.
- Only works with Steam build 19569414 of the game.

SHA-256 (`PowerslaveExhumedVR.zip`): `f1a8b979b3de8343029f287bb5f4a9f5df664b68911fccd370b468746bd2f793`
