#pragma once
namespace pvr::stereo {
// Hooks the gameplay world draw so it renders once per eye. Requires MH_Initialize().
void Install();
// Latest body frame used for rendering: rows 0-2 = tracking-space X/Y/Z axes in world
// (Z-up game units), row 3 = world position of the tracking origin. False before first frame.
bool BaseFrame(float rows[4][3]);

// Numbers from the last stereo world draw, for invariant checks (FakeXR harness).
struct FrameStats {
    unsigned long long count = 0;  // stereo draws so far
    float eyeOrigin[2][3];         // world (game units)
    float eyeFwd[2][3];            // world, unit camera forward per eye
    float eyeUp[2][3];
    float gameFwd[3];              // the game's own camera forward (unit)
    float base[4][3];              // see BaseFrame
    float k;                       // engine camera-z scale/flip
};
bool LastStats(FrameStats &out);
}  // namespace pvr::stereo
