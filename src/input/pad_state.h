#pragma once
#include <cstdint>
#include <mutex>

namespace pvr {
// Mirrors SDL_GameControllerButton / SDL_GameControllerAxis numbering.
enum PadButton {
    PB_A, PB_B, PB_X, PB_Y, PB_BACK, PB_GUIDE, PB_START, PB_LSTICK, PB_RSTICK, PB_LSHOULDER, PB_RSHOULDER,
    PB_DPAD_UP, PB_DPAD_DOWN, PB_DPAD_LEFT, PB_DPAD_RIGHT, PB_MISC1, PB_PADDLE1, PB_PADDLE2, PB_PADDLE3,
    PB_PADDLE4, PB_TOUCHPAD, PB_COUNT
};
enum PadAxis { PA_LEFTX, PA_LEFTY, PA_RIGHTX, PA_RIGHTY, PA_TRIGGERLEFT, PA_TRIGGERRIGHT, PA_COUNT };

struct PadState {
    bool connected = false;
    bool buttons[PB_COUNT] = {};
    int16_t axes[PA_COUNT] = {};
};

// Written by the XR thread (Present hook), read by the game's SDL event pump.
void PadPublish(const PadState &s);
PadState PadSnapshot();
}  // namespace pvr
