#pragma once
// Header-only math shared by the mod and the unit tests (tests/test_math.cpp).
// Conventions:
//  - M4 is row-vector (p' = p * M), translation in row 3, m[r*4+c] (matches the engine).
//  - Quaternions are OpenXR (x,y,z,w), column-vector rotation.
//  - OpenXR tracking space: +X right, +Y up, -Z forward.
#include <openxr/openxr.h>

#include <cmath>

namespace pvr::vm {

struct M4 {
    float m[16];
    float &at(int r, int c) { return m[r * 4 + c]; }
    float at(int r, int c) const { return m[r * 4 + c]; }
};

inline M4 Identity() {
    M4 o{};
    o.at(0, 0) = o.at(1, 1) = o.at(2, 2) = o.at(3, 3) = 1;
    return o;
}

inline M4 Mul(const M4 &a, const M4 &b) {
    M4 o{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.at(r, k) * b.at(k, c);
            o.at(r, c) = s;
        }
    return o;
}

// inverse of [R 0; t 1] with R orthogonal (proper or not)
inline M4 InvRigid(const M4 &a) {
    M4 o{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o.at(r, c) = a.at(c, r);
    for (int c = 0; c < 3; ++c)
        o.at(3, c) = -(a.at(3, 0) * o.at(0, c) + a.at(3, 1) * o.at(1, c) + a.at(3, 2) * o.at(2, c));
    o.at(3, 3) = 1;
    return o;
}

inline float Len3(const float *v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
inline float Dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void Norm3(float *v) {
    float l = Len3(v);
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}

// Pose (LOCAL space) as row-vector matrix pose->local, translation scaled (e.g. to game units).
inline M4 PoseToM4(const XrPosef &p, float scale) {
    float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    // column-vector rotation matrix R; row-vector form uses R^T
    float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                     {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                     {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    M4 o{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o.at(r, c) = R[c][r];
    o.at(3, 0) = p.position.x * scale;
    o.at(3, 1) = p.position.y * scale;
    o.at(3, 2) = p.position.z * scale;
    o.at(3, 3) = 1;
    return o;
}

// ---- quaternions / poses ----
inline XrVector3f Rotate(const XrQuaternionf &q, const XrVector3f &v) {
    // v' = v + 2w(u x v) + 2 u x (u x v)
    float tx = 2.0f * (q.y * v.z - q.z * v.y), ty = 2.0f * (q.z * v.x - q.x * v.z), tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

inline XrQuaternionf QMul(const XrQuaternionf &a, const XrQuaternionf &b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline XrQuaternionf AxisAngle(float ax, float ay, float az, float rad) {
    float s = std::sin(rad * 0.5f);
    return {ax * s, ay * s, az * s, std::cos(rad * 0.5f)};
}

// yaw about +Y (positive = turn left), then pitch about local +X (positive = look up), then roll about -Z
inline XrQuaternionf YawPitchRoll(float yaw, float pitch, float roll = 0.0f) {
    return QMul(QMul(AxisAngle(0, 1, 0, yaw), AxisAngle(1, 0, 0, pitch)), AxisAngle(0, 0, -1, roll));
}

inline XrVector3f Forward(const XrQuaternionf &q) { return Rotate(q, {0, 0, -1}); }

// yaw of a forward direction, positive = turned left (same as BeginNextFrame's head yaw)
inline float YawOf(const XrQuaternionf &q) {
    XrVector3f f = Forward(q);
    return std::atan2(-f.x, -f.z);
}

inline XrPosef Compose(const XrPosef &a, const XrPosef &b) {  // a * b (b expressed in a's frame)
    XrVector3f p = Rotate(a.orientation, b.position);
    return {QMul(a.orientation, b.orientation), {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z}};
}

// Head pose from the two eye poses: mid-point position, left-eye orientation.
inline XrPosef HeadPose(const XrPosef &l, const XrPosef &r) {
    return {l.orientation,
            {(l.position.x + r.position.x) * 0.5f, (l.position.y + r.position.y) * 0.5f,
             (l.position.z + r.position.z) * 0.5f}};
}

// Head-locked HUD quad: `distance` in front of the head, `down` below it (negative = raised).
inline XrPosef HudPose(const XrPosef &head, float distance, float down) {
    XrPosef off{{0, 0, 0, 1}, {0.0f, -down, -distance}};
    return Compose(head, off);
}

// Head-locked panel tilted down by `pitchDown` (rad) around the head's x axis, `distance` along
// that tilted gaze, then shifted `up` along the panel's own up axis. The panel faces the head.
inline XrPosef PanelPose(const XrPosef &head, float distance, float pitchDown, float up) {
    XrQuaternionf q = AxisAngle(1, 0, 0, -pitchDown);
    XrVector3f p = Rotate(q, {0.0f, up, -distance});
    return Compose(head, XrPosef{q, p});
}

// Body yaw that follows the head lazily: only the part of the difference beyond `deadzone` (rad)
// is applied, so small head turns leave it alone.
inline float FollowYaw(float body, float head, float deadzone) {
    const float pi = 3.14159265f;
    float d = std::remainder(head - body, 2 * pi);
    if (d > deadzone) return body + (d - deadzone);
    if (d < -deadzone) return body + (d + deadzone);
    return body;
}

// Chest panel: below the head by `down`, `forward` ahead along the body yaw, horizon-level, tilted
// back by `tilt` (rad; 0 = vertical, facing the body, 90 deg = lying flat, facing up).
inline XrPosef ChestPose(const XrVector3f &head, float yaw, float down, float forward, float tilt) {
    XrQuaternionf q = QMul(AxisAngle(0, 1, 0, yaw), AxisAngle(1, 0, 0, -tilt));
    XrVector3f f = Rotate(AxisAngle(0, 1, 0, yaw), {0, 0, -forward});
    return {q, {head.x + f.x, head.y - down, head.z + f.z}};
}

}  // namespace pvr::vm
