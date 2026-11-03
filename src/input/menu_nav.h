#pragma once
// Left stick -> d-pad while a menu is up (controllers without a d-pad, e.g. Quest Touch). Header-only so the unit tests
// can drive it with a fake clock.
#include <cmath>
#include <cstdint>

namespace pvr {

struct MenuNav {
    enum Dir { NONE = -1, UP, DOWN, LEFT, RIGHT };
    static constexpr float kPress = 0.6f;     // deflection that presses a direction
    static constexpr float kRelease = 0.35f;  // ... and that releases it again
    static constexpr uint64_t kDelay = 400;   // ms held before auto-repeat starts
    static constexpr uint64_t kRepeat = 150;  // ms between repeats
    static constexpr uint64_t kGap = 50;      // ms released between two repeated presses (so the game sees a new press)

    int dir = NONE;
    bool armed = false;  // the stick must come back to the centre after a menu opens
    uint64_t next = 0;   // next repeat (start of its release gap)

    void Reset() { dir = NONE, armed = false; }

    // x right +, y up +, -1..1. Returns the direction pressed right now (NONE = nothing).
    int Update(float x, float y, uint64_t nowMs) {
        float ax = std::fabs(x), ay = std::fabs(y);
        if (!armed) {
            if (ax < kRelease && ay < kRelease) armed = true;
            return NONE;
        }
        int want = NONE;
        if (ax >= kPress || ay >= kPress) want = ay >= ax ? (y > 0 ? UP : DOWN) : (x > 0 ? RIGHT : LEFT);
        if (dir != NONE) {
            float along = dir == UP ? y : dir == DOWN ? -y : dir == RIGHT ? x : -x;
            if (want != NONE && want != dir) {
                dir = NONE;  // swung to another direction: start over with it below
            } else if (along < kRelease) {
                dir = NONE;
                return NONE;
            }
        }
        if (dir == NONE) {
            if (want == NONE) return NONE;
            dir = want;
            next = nowMs + kDelay;
            return dir;
        }
        while (nowMs >= next + kGap) next += kRepeat;  // gap over: pressed again until the next repeat
        if (nowMs >= next && nowMs < next + kGap) return NONE;
        return dir;
    }
};

}  // namespace pvr
