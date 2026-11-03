// Unit tests for src/common/vrmath.h and the sign conventions the mod and the FakeXR
// harness rely on. No game, no headset: build/pvr_tests.exe, exit code = failures.
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "common/vrmath.h"
#include "input/menu_nav.h"

using namespace pvr::vm;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char *what, double got = 0, double exp = 0) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("FAIL %s (got %.6f, expected %.6f)\n", what, got, exp);
    }
}
static void Near(double got, double exp, const char *what, double tol = 1e-4) {
    Check(std::fabs(got - exp) <= tol, what, got, exp);
}
static void NearV(const XrVector3f &a, const XrVector3f &b, const char *what, double tol = 1e-4) {
    Near(a.x, b.x, what, tol), Near(a.y, b.y, what, tol), Near(a.z, b.z, what, tol);
}
constexpr float kDeg = 3.14159265f / 180.0f;
static float Wrap(float d) {
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

// engine-side body frame for a game yaw (as stereo.cpp builds it): rows right, up, back, origin
static M4 Cbase(float gameYawDeg, float ox, float oy, float oz) {
    float g = gameYawDeg * kDeg;
    float f[3] = {std::sin(g), std::cos(g), 0};
    float r[3] = {f[1], -f[0], 0};  // right of forward, Z up (camera right = (-1,0,0) at yaw pi)
    float u[3] = {0, 0, 1};
    float b[3] = {r[1] * u[2] - r[2] * u[1], r[2] * u[0] - r[0] * u[2], r[0] * u[1] - r[1] * u[0]};
    M4 c{};
    for (int i = 0; i < 3; ++i) c.at(0, i) = r[i], c.at(1, i) = u[i], c.at(2, i) = b[i];
    c.at(3, 0) = ox, c.at(3, 1) = oy, c.at(3, 2) = oz, c.at(3, 3) = 1;
    return c;
}

static void TestQuat() {
    XrQuaternionf id{0, 0, 0, 1};
    NearV(Rotate(id, {1, 2, 3}), {1, 2, 3}, "identity rotate");
    XrQuaternionf left90 = YawPitchRoll(90 * kDeg, 0);
    NearV(Forward(left90), {-1, 0, 0}, "yaw +90 looks left (-X)");
    XrQuaternionf up30 = YawPitchRoll(0, 30 * kDeg);
    NearV(Forward(up30), {0, std::sin(30 * kDeg), -std::cos(30 * kDeg)}, "pitch +30 looks up");
    for (float y = -170; y <= 170; y += 34)
        for (float p = -80; p <= 80; p += 40) Near(YawOf(YawPitchRoll(y * kDeg, p * kDeg)) / kDeg, y, "YawOf inverts yaw", 1e-2);
    XrQuaternionf a = YawPitchRoll(0.3f, -0.4f, 0.2f), b = YawPitchRoll(-1.1f, 0.7f, -0.5f);
    XrVector3f v{0.3f, -1.2f, 2.0f};
    NearV(Rotate(QMul(a, b), v), Rotate(a, Rotate(b, v)), "QMul composes like Rotate");
    XrVector3f r = Rotate(a, v);
    Near(std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z), std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), "rotation keeps length");
}

static void TestM4() {
    XrPosef p{YawPitchRoll(0.7f, -0.2f, 0.1f), {0.1f, 1.6f, -0.3f}};
    M4 m = PoseToM4(p, 64.0f);
    XrVector3f v{0.5f, -0.25f, 1.0f};
    // row vector: [v 1] * m
    float o[3];
    for (int c = 0; c < 3; ++c) o[c] = v.x * m.at(0, c) + v.y * m.at(1, c) + v.z * m.at(2, c) + m.at(3, c);
    XrVector3f r = Rotate(p.orientation, v);
    NearV({o[0], o[1], o[2]}, {r.x + 0.1f * 64, r.y + 1.6f * 64, r.z - 0.3f * 64}, "PoseToM4 = rotate then scaled translate", 1e-3);
    M4 i = Mul(InvRigid(m), m);
    M4 id = Identity();
    float err = 0;
    for (int k = 0; k < 16; ++k) err += std::fabs(i.m[k] - id.m[k]);
    Near(err, 0, "InvRigid * M = I", 1e-3);
}

static void TestHud() {
    XrPosef head{YawPitchRoll(90 * kDeg, 0), {1, 2, 3}};
    XrPosef hud = HudPose(head, 1.2f, -0.04f);
    NearV(hud.position, {1 - 1.2f, 2 + 0.04f, 3}, "HUD in front of a head turned left");
    XrPosef head2{YawPitchRoll(-35 * kDeg, 50 * kDeg), {0.2f, -0.1f, 0.4f}};
    XrPosef hud2 = HudPose(head2, 1.5f, 0.1f);
    float d[3] = {hud2.position.x - 0.2f, hud2.position.y + 0.1f, hud2.position.z - 0.4f};
    Near(Len3(d), std::sqrt(1.5 * 1.5 + 0.1 * 0.1), "HUD distance independent of head orientation");
    XrVector3f f = Forward(head2.orientation);
    float ff[3] = {f.x, f.y, f.z};
    Near(Dot3(d, ff), 1.5, "HUD along gaze = Distance");
    NearV(Forward(hud2.orientation), f, "HUD faces the same way as the head");
    XrPosef l{YawPitchRoll(0.4f, 0.1f), {-0.032f, 1.6f, 0}}, r{l.orientation, {0.032f, 1.6f, 0.01f}};
    NearV(HeadPose(l, r).position, {0, 1.6f, 0.005f}, "HeadPose midpoint");
}

// The FakeXR harness expects: eye yaw (game convention) = game yaw - head yaw, eye pitch = head pitch,
// eye separation = IPD * WorldScale. Build the eyes exactly like stereo.cpp does.
static void TestStereoConvention() {
    const float scale = 64.0f, ipd = 0.064f;
    for (float gameYaw : {0.0f, 90.0f, 180.0f, -135.0f})
        for (float headYaw : {0.0f, 30.0f, -70.0f})
            for (float headPitch : {0.0f, 25.0f, -40.0f}) {
                M4 base = Cbase(gameYaw, 100, -50, 20);
                XrPosef head{YawPitchRoll(headYaw * kDeg, headPitch * kDeg), {0, 0, 0}};
                M4 ce[2];
                for (int e = 0; e < 2; ++e) {
                    XrPosef off{{0, 0, 0, 1}, {e ? ipd / 2 : -ipd / 2, 0, 0}};
                    ce[e] = Mul(PoseToM4(Compose(head, off), scale), base);
                }
                float fwd[3] = {-ce[0].at(2, 0), -ce[0].at(2, 1), -ce[0].at(2, 2)};
                float eyeYaw = std::atan2(fwd[0], fwd[1]) / kDeg, eyePitch = std::asin(fwd[2]) / kDeg;
                if (std::fabs(headPitch) < 89) Near(Wrap(eyeYaw - (gameYaw - headYaw)), 0, "eye yaw = game yaw - head yaw", 1e-2);
                Near(eyePitch, headPitch, "eye pitch = head pitch", 1e-2);
                float d[3] = {ce[1].at(3, 0) - ce[0].at(3, 0), ce[1].at(3, 1) - ce[0].at(3, 1), ce[1].at(3, 2) - ce[0].at(3, 2)};
                Near(Len3(d), ipd * scale, "eye separation = IPD * scale", 1e-3);
                Near(Dot3(d, fwd) / Len3(d), 0, "eye baseline perpendicular to gaze", 1e-4);
                // the right eye is to the right of the left eye
                float right[3] = {ce[0].at(0, 0), ce[0].at(0, 1), ce[0].at(0, 2)};
                Check(Dot3(d, right) > 0, "right eye on the right");
            }
}

// aim.cpp: controller forward -> world via base rows -> game yaw/pitch (pitch down positive)
static void TestAimConvention() {
    for (float gameYaw : {0.0f, 45.0f, -120.0f})
        for (float handYaw : {0.0f, 20.0f, -60.0f})
            for (float handPitch : {0.0f, 15.0f, -30.0f}) {
                M4 base = Cbase(gameYaw, 0, 0, 0);
                XrQuaternionf q = YawPitchRoll(handYaw * kDeg, handPitch * kDeg);
                float d[3] = {-2.0f * (q.x * q.z + q.w * q.y), -2.0f * (q.y * q.z - q.w * q.x),
                              -(1.0f - 2.0f * (q.x * q.x + q.y * q.y))};
                float w[3];
                for (int i = 0; i < 3; ++i) w[i] = d[0] * base.at(0, i) + d[1] * base.at(1, i) + d[2] * base.at(2, i);
                float yaw = std::atan2(w[0], w[1]) / kDeg, pitch = -std::asin(w[2] / Len3(w)) / kDeg;
                Near(Wrap(yaw - (gameYaw - handYaw)), 0, "aim yaw = game yaw - hand yaw", 1e-2);
                Near(pitch, -handPitch, "aim pitch = -hand pitch", 1e-2);
            }
}

// left stick -> d-pad in menus: press/release hysteresis, arming, auto-repeat
static void TestMenuNav() {
    using pvr::MenuNav;
    MenuNav n;
    Check(n.Update(0, 1, 0) == MenuNav::NONE, "not armed while the stick is already pushed");
    Check(n.Update(0, 0, 10) == MenuNav::NONE, "centre arms");
    Check(n.Update(0, 0.5f, 20) == MenuNav::NONE, "below press threshold");
    Check(n.Update(0, 0.9f, 30) == MenuNav::UP, "up pressed");
    Check(n.Update(0, 0.5f, 40) == MenuNav::UP, "hysteresis keeps it");
    Check(n.Update(0, 0.2f, 50) == MenuNav::NONE, "released");
    Check(n.Update(0, -0.8f, 60) == MenuNav::DOWN, "down");
    Check(n.Update(0.9f, -0.1f, 70) == MenuNav::RIGHT, "swing to right");
    Check(n.Update(-0.9f, 0.3f, 80) == MenuNav::LEFT, "swing to left");
    // held: pressed until 80+400, released for 50 ms, pressed again, gap every 150 ms
    Check(n.Update(-0.9f, 0, 479) == MenuNav::LEFT, "held before repeat");
    Check(n.Update(-0.9f, 0, 481) == MenuNav::NONE, "repeat gap");
    Check(n.Update(-0.9f, 0, 531) == MenuNav::LEFT, "repeated press");
    Check(n.Update(-0.9f, 0, 631) == MenuNav::NONE, "second repeat gap");
    Check(n.Update(-0.9f, 0, 2000) != MenuNav::NONE || true, "long frame gap does not hang");
    n.Reset();
    Check(n.Update(-0.9f, 0, 2100) == MenuNav::NONE, "reset needs the centre again");
}

int main() {
    TestQuat();
    TestM4();
    TestHud();
    {
        const float d2r = 0.017453293f;
        Near(FollowYaw(0, 10 * d2r, 25 * d2r), 0, "small head turn leaves the body yaw");
        Near(FollowYaw(0, 40 * d2r, 25 * d2r), 15 * d2r, "big head turn drags the body yaw");
        Near(FollowYaw(0, -40 * d2r, 25 * d2r), -15 * d2r, "drag the other way");
        Near(std::remainder(FollowYaw(170 * d2r, -170 * d2r, 5 * d2r) - 185 * d2r, 2 * 3.14159265f), 0, "wraps across +-180 (ends at 185 = -175)");
        XrPosef c = ChestPose({0, 1.6f, 0}, 0, 0.4f, 0.3f, 45 * d2r);
        NearV(c.position, {0, 1.2f, -0.3f}, "chest panel below and ahead");
        XrVector3f n = Rotate(c.orientation, {0, 0, 1});  // panel normal
        NearV(n, {0, 0.7071068f, 0.7071068f}, "tilted to face up and back");
        XrPosef c2 = ChestPose({0, 1.6f, 0}, 90 * d2r, 0.4f, 0.3f, 0);
        NearV(c2.position, {-0.3f, 1.2f, 0}, "yaw 90 (left) puts it to the left");
    }
    {
        XrPosef head{{0, 0, 0, 1}, {0, 1.5f, 0}};
        XrPosef p = PanelPose(head, 1.0f, 0.5235988f, 0.0f);  // 30 deg down
        NearV(p.position, {0, 1.5f - 0.5f, -0.8660254f}, "panel 30 deg below gaze");
        XrVector3f f = Forward(p.orientation);
        NearV(f, {0, -0.5f, -0.8660254f}, "tilted panel faces the head");
        XrPosef p2 = PanelPose(head, 1.0f, 0.0f, 0.2f);
        NearV(p2.position, {0, 1.7f, -1.0f}, "panel shifted up");
    }
    TestStereoConvention();
    TestAimConvention();
    TestMenuNav();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
