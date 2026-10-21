#include "Beams.h"

#include "Renderer.h"

#include <algorithm>
#include <cmath>

namespace {

// Ken Perlin's classic gradient noise as the game's FUN_007e5e30 / FUN_007e56f0 build and evaluate it (B = 256, N = 4096): p a
// shuffled permutation, g2 random unit 2D gradients, each value (rand() % 512 - 256) / 256. The game fills its tables from the C
// runtime's rand() at start-up; the editor uses an MSVC-style rand seeded with 1, so the values differ (UNVERIFIED), the shape does not.
struct NoiseTables {
    int p[256 + 256 + 2];
    float g2[256 + 256 + 2][2];
    NoiseTables() {
        uint32_t state = 1;
        auto rand = [&state]() {
            state = state * 214013u + 2531011u;
            return int((state >> 16) & 0x7fff);
        };
        auto value = [&]() { return float(rand() % 512 - 256) / 256.0f; };
        float g1[256], g3[256][3];
        for (int i = 0; i < 256; ++i) {
            p[i] = i;
            g1[i] = value();
            for (int k = 0; k < 2; ++k) g2[i][k] = value();
            float s = std::sqrt(g2[i][0] * g2[i][0] + g2[i][1] * g2[i][1]);
            if (s > 0) for (float& c : g2[i]) c /= s;
            for (int k = 0; k < 3; ++k) g3[i][k] = value();
            (void)g1;
        }
        for (int i = 255; i > 0; --i) {
            const int k = p[i], j = rand() % 256;
            p[i] = p[j];
            p[j] = k;
        }
        for (int i = 0; i < 256 + 2; ++i) {
            p[256 + i] = p[i];
            g2[256 + i][0] = g2[i][0];
            g2[256 + i][1] = g2[i][1];
        }
    }
};

const NoiseTables& noiseTables() {
    static const NoiseTables tables;
    return tables;
}

float noise2(float x, float y) {
    const NoiseTables& t = noiseTables();
    const float tx = x + 4096.0f, ty = y + 4096.0f;
    const int ix = int(tx), iy = int(ty);
    const int bx0 = ix & 0xff, bx1 = (bx0 + 1) & 0xff, by0 = iy & 0xff, by1 = (by0 + 1) & 0xff;
    const float rx0 = tx - float(ix), rx1 = rx0 - 1.0f, ry0 = ty - float(iy), ry1 = ry0 - 1.0f;
    const int i = t.p[bx0], j = t.p[bx1];
    const int b00 = t.p[i + by0], b10 = t.p[j + by0], b01 = t.p[i + by1], b11 = t.p[j + by1];
    const float sx = rx0 * rx0 * (3.0f - 2.0f * rx0), sy = ry0 * ry0 * (3.0f - 2.0f * ry0);
    auto at = [&](int b, float rx, float ry) { return rx * t.g2[b][0] + ry * t.g2[b][1]; };
    const float a = at(b00, rx0, ry0) + sx * (at(b10, rx1, ry0) - at(b00, rx0, ry0));
    const float b = at(b01, rx0, ry1) + sx * (at(b11, rx1, ry1) - at(b01, rx0, ry1));
    return a + sy * (b - a);
}

tr::Vec3 lerp3(const tr::Vec3& a, const tr::Vec3& b, float t) { return add(a, scaled(subtract(b, a), t)); }

}  // namespace

BeamInstance::BeamInstance(const tr::SfxBeam& event, const tr::Vec3& start, const tr::Vec3& end, float time, uint32_t seed)
    : event_(event), startTime_(time), random_(seed ? seed : 1) {
    seed_ = random();
    points_.push_back(Point{start, true});
    points_.push_back(Point{end, true});
    build(0, end, 1, event.branchChance, end, 0);
}

float BeamInstance::random() {
    random_ = uint32_t((uint64_t(random_) * 48271u) % 2147483647u);   // Park-Miller (the editor's stream, not the game's)
    return float(random_) / 2147483647.0f;
}

void BeamInstance::build(size_t from, const tr::Vec3& to, size_t toPoint, float chance, const tr::Vec3& target, int depth) {
    const tr::Vec3 a = points_[from].position;
    const float len = length(subtract(to, a));
    if (len == 0.0f || depth > 16) return;
    int n = std::max(1, 4 * int(len));
    n = std::max(1, std::min(int(event_.maxSubdivisions), n));
    Segment segment;
    segment.points.push_back(from);
    for (int k = 1; k < n; ++k) {
        points_.push_back(Point{lerp3(a, to, float(k) / float(n)), false});
        segment.points.push_back(points_.size() - 1);
    }
    if (toPoint == size_t(-1)) {
        points_.push_back(Point{to, false});
        toPoint = points_.size() - 1;
    }
    segment.points.push_back(toPoint);
    const std::vector<size_t> segmentPoints = segment.points;
    segments_.push_back(std::move(segment));
    float c = chance;
    for (int i = int(segmentPoints.size()) - 2; i > 0; --i) {
        if (random() < c) {
            const float u1 = random(), u2 = random(), u3 = random(), u4 = random();
            const float l = (0.75f * u1 + 0.25f) * c * event_.branchLength;
            const tr::Vec3 r{2.0f * u2 - 1.0f, u4, 2.0f * u3 - 1.0f};
            const tr::Vec3 p = points_[segmentPoints[size_t(i)]].position;
            const tr::Vec3 end = lerp3(p, add(target, scaled(r, l)), event_.branchPull);
            build(segmentPoints[size_t(i)], end, size_t(-1), c * event_.branchDecay, target, depth + 1);
        }
        c *= 0.75f;
    }
}

void BeamInstance::update(const tr::Vec3& start, const tr::Vec3& end, float time, const tr::Vec3& eye, const tr::Vec3& right,
                          const tr::Vec3& up, std::vector<MeshVertex>& out) {
    points_[0].position = start;
    points_[1].position = end;
    const float scroll = (startTime_ - time) * event_.animRate;
    for (Segment& s : segments_) {
        const size_t count = s.points.size();
        if (count < 2) continue;
        const tr::Vec3 first = points_[s.points.front()].position, last = points_[s.points.back()].position;
        if (s.length == 0.0f) s.length = length(subtract(last, first));
        if (s.length < 1e-5f) continue;
        // Two axes across the segment (x and z when it points straight up).
        const tr::Vec3 dir = normalized(subtract(last, first));
        tr::Vec3 axis1{1, 0, 0}, axis2{0, 0, 1};
        if (!(std::fabs(dir.x) < 1e-6f && std::fabs(dir.y - 1.0f) < 1e-6f && std::fabs(dir.z) < 1e-6f)) {
            axis1 = normalized(cross(dir, tr::Vec3{0, 1, 0}));
            axis2 = normalized(cross(dir, axis1));
        }
        const float inverse = 1.0f / float(count - 1);
        const float step = 0.25f * inverse * (event_.tileOncePerSegment ? 1.0f : s.length);
        float endAlpha = 1.0f;
        tr::Vec3 previous = first, beforePrevious = first;
        for (size_t i = 1; i < count; ++i) {
            Point& point = points_[s.points[i]];
            tr::Vec3 current;
            if (i == count - 1) {
                current = last;
                if (!point.anchored) endAlpha = 0.0f;
            } else {
                current = lerp3(first, last, float(i) * inverse);
                if (seed_ > 0.0f) {
                    const float envelope = event_.noiseAmplitude * std::sin(float(i) * inverse * 3.14159265f);
                    const float t = event_.noiseSpeed * time - float(i - 1) * 0.1f;
                    const float x = seed_ * 100.0f * float(i);
                    current = add(current, add(scaled(axis1, envelope * noise2(x, t)), scaled(axis2, envelope * noise2(x + 1.0f, t))));
                }
                point.position = current;
            }
            if (i == 1) beforePrevious = subtract(first, subtract(current, first));
            // The four corners: even ones at the earlier point (direction from the point before it), odd ones at this point.
            MeshVertex corners[4];
            for (int c = 0; c < 4; ++c) {
                const tr::Vec3& position = (c & 1) ? current : previous;
                const tr::Vec3& other = (c & 1) ? previous : beforePrevious;
                const float px = dot(subtract(position, eye), right), py = dot(subtract(position, eye), up);
                float dx = dot(subtract(other, eye), right) - px, dy = dot(subtract(other, eye), up) - py;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d > 0) { dx /= d; dy /= d; }
                const float offset = event_.width * (c < 2 ? -0.5f : 0.5f);
                const tr::Vec3 world = add(position, add(scaled(right, -dy * offset), scaled(up, dx * offset)));
                MeshVertex& v = corners[c];
                v.positionX = world.x;
                v.positionY = world.y;
                v.positionZ = world.z;
                const float u = ((c & 1) ? step : 0.0f) + float(i) * step;
                v.texU = u * event_.tileRate + scroll;
                v.texV = c < 2 ? 0.0f : 1.0f;
                const float alpha = (c & 1) ? endAlpha : 1.0f;
                v.colour = packColour(255, 255, 255, uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
            }
            for (int index : {1, 0, 2, 1, 2, 3}) out.push_back(corners[index]);
            beforePrevious = previous;
            previous = current;
        }
    }
}
