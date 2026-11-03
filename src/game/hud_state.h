#pragma once

namespace pvr::hudstate {
// Player numbers for the VR gauges, read from the game while its HUD is drawn.
struct Stats {
    int hp = 0, hpMax = 100;     // hpMax: one health container (100 or 200 by game mode) * containers
    int ammo = -1, ammoMax = 0;  // ammo -1 / ammoMax 0: weapon without ammo
    int weapon = -1;
    int air = 0, airMax = 0;  // airMax = largest value seen (full lungs)
    bool underwater = false;
};
// Called from the HUD draw hook with the HUD object (kexSlaveHudLocal*).
void Capture(void *hud);
// Latest values if the HUD was drawn recently (i.e. we are in gameplay).
bool Get(Stats &out);
}  // namespace pvr::hudstate
