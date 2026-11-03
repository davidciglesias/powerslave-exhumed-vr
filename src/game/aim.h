#pragma once
namespace pvr::aim {
// Hooks the player weapon tick so weapons fire along the right controller. Needs MH_Initialize().
void Install();
// Last aim applied in the weapon tick (game convention: yaw about Z, pitch down positive).
struct AimStats {
    unsigned long long count = 0;
    float yaw = 0, pitch = 0;          // what we set
    float actorYaw = 0, actorPitch = 0;  // the actor's own rotation (restored afterwards)
};
AimStats LastAim();
}  // namespace pvr::aim
