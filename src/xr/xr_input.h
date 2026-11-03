#pragma once
#include <openxr/openxr.h>

namespace pvr::xr {
// Call after xrCreateSession, before the session begins.
bool InputInit(XrInstance inst, XrSession sess);
// Call once per frame after xrWaitFrame; publishes the virtual pad state.
// headYaw: HMD yaw relative to the tracking space (radians, positive = turned left).
void InputSync(XrSession sess, bool focused, float headYaw);
// Locate controller poses in `base` at time t (after InputSync).
void LocateHands(XrSpace base, XrTime t);
// 0 = left aim, 1 = right aim, 2 = left grip, 3 = right grip. False if not tracked.
bool HandPose(int which, XrPosef &out);
// FakeXR harness: override a hand pose (nullptr = not tracked).
void SetHandPose(int which, const XrPosef *p);
}  // namespace pvr::xr
