#pragma once

// Special-FX accessory beams (`ACBM`, tr::SfxBeam): lightning, lasers and energy streams between two contact points of a body,
// as the game builds and draws them (tabula_rasa.exe, decompiled 2026-10-20).
//
// Trigger (TRasa::SfxAccessoryBeamEvent FUN_0076d3c0): for the bodies the two target bytes select, each end is the contact point
// whose connection point (type 1, usage 1) is contactPoint1 / contactPoint2 (FUN_00924880; 0 = the body itself); the pair gives one
// root segment, built by FUN_0076ea10 with the end's world position as `target`:
//   - a segment from a to b (positions now): nothing when |b - a| is 0; else n = min(maxSubdivisions, max(1, 4 x trunc(|b - a|)))
//     pieces (FUN_0081e280: n + 1 points, the inner ones on the straight line), appended to the beam's list;
//   - for points i = n - 1 down to 1, with c = the chance passed in and multiplied by 0.75 (0x00c2b8e0) after each point: when a
//     uniform random u < c, a branch from point i: L = (0.75 u1 + 0.25) x c x branchLength (0.25 at 0x00c2b620), r = (2 u2 - 1,
//     u4, 2 u3 - 1) (randoms drawn in the order u1, u2, u3, u4), end = lerp(point i, target + L r, branchPull) (FUN_00456dc0), a
//     fixed point; the branch is built the same way (recursively) with chance c x branchDecay, starting at point i itself (the same
//     point object: it follows that point as it moves).
//   The beam (TRasa::SegmentedBeamManager / VfxManager FUN_007f0f20, effect TRBeam.fx): StartTime = now, TileRate, AnimRate; a
//   random seed s in [0, 1) (FUN_007880d0 x 2^-32); duration (ms x 0.001; 0 = forever).
// Per frame (FUN_007f1670): per segment in list order, the inner points' positions = lerp(first, last, k / (count - 1)) with the
// ends where they are now (FUN_0081e000; a segment's length is measured once, on its first frame - one shorter than 1e-5 is not
// drawn); dir = normalize(last - first) and two axes across it (x and z when dir is +y, else cross products with +y); inner point
// k gets the offset A sin(pi t) (n1 axis1 + n2 axis2), t = k / (count - 1), A = noiseAmplitude, n1 / n2 = 2D gradient noise
// (FUN_007e56f0, Ken Perlin's classic noise2 with B = 256, N = 4096 and s-curve t^2 (3 - 2t)) at (s x 100 x k, noiseSpeed x time -
// (k - 1) x 0.1) and at x + 1 (only when s > 0), written into the point (so branches follow it). Each pair of neighbouring points
// gives a quad (4 vertices of 11 floats, FUN_007f5050): this point, the point before it (direction), width, alpha (1, or 0 at the
// free end of a branch - a point without a contact point), corner 0..3, the U step and the U at the earlier point.
// TRBeam (vs inline, disassembled 2026-10-20): in view space the corner moves by width x (-0.5, -0.5, 0.5, 0.5)[corner] along the
// perpendicular of the projected direction to the neighbouring point (even corners: the earlier point, towards the one before it;
// odd: this point, towards the earlier one); U = (u + (0, step, 0, step)[corner]) x TileRate + (StartTime - gTime) x AnimRate,
// V = (0, 0, 1, 1)[corner]; colour (1, 1, 1, alpha); pixel = texture.rgb x alpha, texture alpha; pass ZWRITEENABLE 0, blend ONE /
// ONE, CULLMODE NONE, FOGENABLE 0. Step = 0.25 / (count - 1), x the segment length unless tileOncePerSegment (0.25 at 0x00c2b620).
// UNVERIFIED: the noise tables (the game fills them from rand() at start-up, FUN_007e5e30 - the editor seeds an MSVC-style rand
// with 1) and the random stream (the game's FUN_007880d0 generator; the editor's own), the order of the two cross products; NOT
// compared with a game frame.
#include "GpuMath.h"
#include "particles.h"

#include <cstdint>
#include <vector>

struct MeshVertex;

class BeamInstance {
public:
    // start / end: the two contact points' world positions when the beam is made; time: the clock it starts on (StartTime).
    BeamInstance(const tr::SfxBeam& event, const tr::Vec3& start, const tr::Vec3& end, float time, uint32_t seed);
    // Moves the two ends (their contact points this frame), applies the noise at `time` and builds the ribbons for a camera at
    // eye with the given right / up axes (view space x / y), appended to out as a triangle list.
    void update(const tr::Vec3& start, const tr::Vec3& end, float time, const tr::Vec3& eye, const tr::Vec3& right, const tr::Vec3& up,
                std::vector<MeshVertex>& out);
    const tr::SfxBeam& event() const { return event_; }
    float startTime() const { return startTime_; }
    size_t segmentCount() const { return segments_.size(); }

private:
    struct Point {
        tr::Vec3 position;          // this frame's (inner points: after the noise)
        bool anchored = false;      // a contact point (one of the two ends) - a free branch end fades out
    };
    struct Segment {
        std::vector<size_t> points; // indices into points_; the first may be another segment's point
        float length = 0;           // measured on the first frame (0: not yet)
    };
    void build(size_t from, const tr::Vec3& to, size_t toPoint, float chance, const tr::Vec3& target, int depth);
    float random();
    tr::SfxBeam event_;
    std::vector<Point> points_;     // 0 = start, 1 = end
    std::vector<Segment> segments_;
    float startTime_ = 0;
    float seed_ = 0;                // the beam's random seed s
    uint32_t random_ = 1;
};
