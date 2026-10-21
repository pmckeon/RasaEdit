#include "PvsBuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

constexpr uint32_t kViewSize = 512;             // the builder's render target (0x200 x 0x200)
constexpr float kNear = 0.025f;                 // 0x00c2bf30 (also the scout's margin)
constexpr float kFar = 350.0f;                  // 0x00c2b560
constexpr uint32_t kPixelThreshold = 26;        // more than 0x1a pixels (FUN_0085b7b0)
constexpr float kProbeLength = 15.0f;           // profile + 0x2c
constexpr float kProbeMinimum = kNear * 1.5f;   // the scout's 0.025 x 1.5 (double at 0x00c2b570)
constexpr float kTerrainClearance = 0.2f;       // 0x00bc5438
constexpr size_t kMaxCandidates = 1000;         // 0x3e8
constexpr size_t kMaxCameras = 50;              // stops after more than 0x31

// Park-Miller minimal standard with multiplier 48271 (TRasa::BasicRandomGenerator, FUN_00957260; Schrage's method).
int32_t parkMiller(int32_t& state) {
    const int64_t s = state;
    int64_t next = 48271LL * (s % 44488) - 3399LL * (s / 44488);
    if (next < 0) next += 0x7fffffff;
    state = int32_t(next);
    return int32_t(next % 0x7fffffff);
}

bool overlaps(const tr::Bounds& a, const tr::Bounds& b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y && a.min.z <= b.max.z && b.min.z <= a.max.z;
}

bool contains(const tr::Bounds& b, const tr::Vec3& p) {
    return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y && p.z >= b.min.z && p.z <= b.max.z;
}

// General 4 x 4 inverse (Gauss-Jordan with partial pivoting).
Mat4 inverse(const Mat4& m) {
    float a[4][8] = {{m.row0.x, m.row0.y, m.row0.z, m.row0.w, 1, 0, 0, 0}, {m.row1.x, m.row1.y, m.row1.z, m.row1.w, 0, 1, 0, 0},
                     {m.row2.x, m.row2.y, m.row2.z, m.row2.w, 0, 0, 1, 0}, {m.row3.x, m.row3.y, m.row3.z, m.row3.w, 0, 0, 0, 1}};
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (pivot != c)
            for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const float d = a[c][c];
        if (std::fabs(d) < 1e-20f) return identityMatrix();
        for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const float f = a[r][c];
            if (f != 0)
                for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
        }
    }
    Mat4 out;
    out.row0 = {a[0][4], a[0][5], a[0][6], a[0][7]};
    out.row1 = {a[1][4], a[1][5], a[1][6], a[1][7]};
    out.row2 = {a[2][4], a[2][5], a[2][6], a[2][7]};
    out.row3 = {a[3][4], a[3][5], a[3][6], a[3][7]};
    return out;
}

// Icosphere vertices (2 subdivisions: 162 directions) - stand-in for the engine's gfxSphereBuilder probe set (FUN_008585f0).
std::vector<tr::Vec3> sphereDirections() {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<tr::Vec3> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                               {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    std::vector<uint32_t> f = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                               3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (tr::Vec3& p : v) p = normalized(p);
    for (int level = 0; level < 2; ++level) {
        std::vector<uint32_t> next;
        std::vector<std::pair<uint64_t, uint32_t>> mids;
        auto midpoint = [&](uint32_t a, uint32_t b) {
            const uint64_t key = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
            for (const auto& m : mids)
                if (m.first == key) return m.second;
            v.push_back(normalized(scaled(add(v[a], v[b]), 0.5f)));
            mids.emplace_back(key, uint32_t(v.size() - 1));
            return uint32_t(v.size() - 1);
        };
        for (size_t i = 0; i < f.size(); i += 3) {
            const uint32_t a = f[i], b = f[i + 1], c = f[i + 2];
            const uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        f.swap(next);
    }
    return v;
}

}  // namespace

PvsBuilder::PvsBuilder(SDL_GPUDevice* device, SceneView& scene, const tr::MapBundle& map, const tr::VisFile& vis)
    : scene_(scene), map_(map), vis_(vis) {
    leafCount_ = vis.leafCount;
    leafNodes_.assign(leafCount_, -1);
    for (size_t n = 0; n < vis.nodes.size(); ++n)
        if (vis.nodes[n].isLeaf() && vis.nodes[n].leafIndex < leafCount_) leafNodes_[vis.nodes[n].leafIndex] = int32_t(n);
    probeDirections_ = sphereDirections();
    if (map.hasTerrain) {
        const tr::TerrainHeader& h = map.terrain.header;
        const float spacing = h.spacing > 0 ? h.spacing : 1.0f;
        terrainW_ = int32_t((h.bounds.max.x - h.bounds.min.x) / spacing + 0.1f);
        terrainH_ = int32_t((h.bounds.max.z - h.bounds.min.z) / spacing + 0.1f);
    }
    renderer_ = std::make_unique<Renderer>(device);
    renderer_->resize(kViewSize, kViewSize);
    std::vector<Renderer::PvsNodeGpu> nodes(vis.nodes.size());
    for (size_t n = 0; n < vis.nodes.size(); ++n) {
        const tr::VisNode& v = vis.nodes[n];
        Renderer::PvsNodeGpu& g = nodes[n];
        g.minX = v.bounds.min.x; g.minY = v.bounds.min.y; g.minZ = v.bounds.min.z; g.split = v.splitPosition();
        g.maxX = v.bounds.max.x; g.maxY = v.bounds.max.y; g.maxZ = v.bounds.max.z; g.pad = 0;
        g.axis = int32_t(v.splitAxis);
        g.child0 = v.child0;
        g.child1 = v.child1;
        g.leaf = v.isLeaf() ? int32_t(v.leafIndex) : -1;
    }
    renderer_->setPvsTree(nodes, leafCount_);
    refreshBodies();
}

PvsBuilder::~PvsBuilder() = default;

tr::Bounds PvsBuilder::entityWorldBox(const tr::MapEntity& e) {
    const tr::Bounds local = e.hasBounds ? e.bounds : tr::Bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 1, 0.5f}};
    const Mat4 world = placementMatrix(e.location, e.orientation, e.scale);
    tr::Bounds out{tr::Vec3{1e30f, 1e30f, 1e30f}, tr::Vec3{-1e30f, -1e30f, -1e30f}};
    for (int k = 0; k < 8; ++k) {
        const tr::Vec3 c{(k & 0x1) ? local.max.x : local.min.x, (k & 0x2) ? local.max.y : local.min.y, (k & 0x4) ? local.max.z : local.min.z};
        const tr::Vec3 w = transformPoint(world, c);
        out.min = tr::Vec3{std::min(out.min.x, w.x), std::min(out.min.y, w.y), std::min(out.min.z, w.z)};
        out.max = tr::Vec3{std::max(out.max.x, w.x), std::max(out.max.y, w.y), std::max(out.max.z, w.z)};
    }
    return out;
}

void PvsBuilder::collectLeaves(int32_t node, const tr::Bounds& box, std::vector<uint32_t>& out) const {
    if (node < 0 || size_t(node) >= vis_.nodes.size()) return;
    const tr::VisNode& n = vis_.nodes[size_t(node)];
    if (!overlaps(n.bounds, box)) return;
    if (n.isLeaf()) {
        if (n.leafIndex < leafCount_) out.push_back(n.leafIndex);
        return;
    }
    collectLeaves(n.child0, box, out);
    collectLeaves(n.child1, box, out);
}

std::vector<uint32_t> PvsBuilder::leavesOverlapping(const tr::Bounds& box) const {
    std::vector<uint32_t> out;
    if (!vis_.nodes.empty()) collectLeaves(0, box, out);
    return out;
}

void PvsBuilder::refreshBodies() {
    bodies_.clear();
    leafBodies_.assign(leafCount_, {});
    const std::vector<tr::MapEntity>& entities = map_.map.entities;
    for (size_t i = 0; i < entities.size(); ++i) {
        if (!scene_.entityHasModel(i)) continue;          // not in the graphics scene (no model, culling layer 6)
        Body b;
        b.entity = uint32_t(i);
        b.box = entityWorldBox(entities[i]);
        scene_.entityBounds(i, b.centre, b.radius);
        const uint32_t index = uint32_t(bodies_.size());
        bodies_.push_back(b);
        for (uint32_t leaf : leavesOverlapping(b.box)) leafBodies_[leaf].push_back(index);
    }
    buildBvh();
}

void PvsBuilder::buildBvh() {
    triangles_.clear();
    bvh_.clear();
    std::vector<tr::Vec3> points;
    for (const Body& b : bodies_) scene_.entityWorldTriangles(b.entity, points);
    std::vector<uint32_t> order(points.size() / 3);
    std::vector<tr::Vec3> centres(order.size());
    for (uint32_t i = 0; i < order.size(); ++i) {
        order[i] = i;
        centres[i] = scaled(add(add(points[i * 3], points[i * 3 + 1]), points[i * 3 + 2]), 1.0f / 3.0f);
    }
    if (order.empty()) return;
    // Median split on the longest axis of the triangle centres; leaves of up to 4 triangles; children stored as a pair.
    struct Task { uint32_t node, begin, end; };
    std::vector<Task> stack;
    bvh_.push_back({});
    stack.push_back({0, 0, uint32_t(order.size())});
    while (!stack.empty()) {
        const Task task = stack.back();
        stack.pop_back();
        tr::Bounds box{tr::Vec3{1e30f, 1e30f, 1e30f}, tr::Vec3{-1e30f, -1e30f, -1e30f}};
        tr::Bounds centreBox = box;
        for (uint32_t k = task.begin; k < task.end; ++k) {
            for (int v = 0; v < 3; ++v) {
                const tr::Vec3& p = points[order[k] * 3 + v];
                box.min = tr::Vec3{std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
                box.max = tr::Vec3{std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
            }
            const tr::Vec3& c = centres[order[k]];
            centreBox.min = tr::Vec3{std::min(centreBox.min.x, c.x), std::min(centreBox.min.y, c.y), std::min(centreBox.min.z, c.z)};
            centreBox.max = tr::Vec3{std::max(centreBox.max.x, c.x), std::max(centreBox.max.y, c.y), std::max(centreBox.max.z, c.z)};
        }
        bvh_[task.node].box = box;
        const tr::Vec3 extent = subtract(centreBox.max, centreBox.min);
        if (task.end - task.begin <= 4 || std::max(extent.x, std::max(extent.y, extent.z)) <= 0.0f) {
            bvh_[task.node].first = uint32_t(triangles_.size());
            bvh_[task.node].count = task.end - task.begin;
            for (uint32_t k = task.begin; k < task.end; ++k) {
                const tr::Vec3& a = points[order[k] * 3];
                triangles_.push_back({a, subtract(points[order[k] * 3 + 1], a), subtract(points[order[k] * 3 + 2], a)});
            }
            continue;
        }
        const uint32_t axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
        const uint32_t middle = (task.begin + task.end) / 2;
        std::nth_element(order.begin() + task.begin, order.begin() + middle, order.begin() + task.end,
                         [&](uint32_t a, uint32_t b) { return tr::component(centres[a], axis) < tr::component(centres[b], axis); });
        const uint32_t left = uint32_t(bvh_.size());
        bvh_.push_back({});
        bvh_.push_back({});
        bvh_[task.node].first = left + 1;
        bvh_[task.node].count = 0;
        stack.push_back({left + 1, middle, task.end});
        stack.push_back({left, task.begin, middle});
    }
}

// The nearest triangle hit before maxT (two-sided, as a collision test).
bool PvsBuilder::nearestHit(const tr::Vec3& origin, const tr::Vec3& direction, float maxT, float& t) const {
    if (bvh_.empty()) return false;
    const tr::Vec3 inv{1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z};
    auto boxHit = [&](const tr::Bounds& b, float limit) {
        const float tx0 = (b.min.x - origin.x) * inv.x, tx1 = (b.max.x - origin.x) * inv.x;
        const float ty0 = (b.min.y - origin.y) * inv.y, ty1 = (b.max.y - origin.y) * inv.y;
        const float tz0 = (b.min.z - origin.z) * inv.z, tz1 = (b.max.z - origin.z) * inv.z;
        const float enter = std::max(std::max(std::min(tx0, tx1), std::min(ty0, ty1)), std::max(std::min(tz0, tz1), 0.0f));
        const float leave = std::min(std::min(std::max(tx0, tx1), std::max(ty0, ty1)), std::min(std::max(tz0, tz1), limit));
        return enter <= leave;
    };
    float best = maxT;
    bool found = false;
    uint32_t stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const BvhNode& node = bvh_[stack[--top]];
        if (!boxHit(node.box, best)) continue;
        if (node.count == 0) {
            if (top < 126) {
                stack[top++] = node.first;
                stack[top++] = node.first - 1;
            }
            continue;
        }
        for (uint32_t k = node.first; k < node.first + node.count; ++k) {
            const Triangle& tri = triangles_[k];
            const tr::Vec3 p = cross(direction, tri.edge2);
            const float determinant = dot(tri.edge1, p);
            if (std::fabs(determinant) < 1e-12f) continue;
            const float inverse = 1.0f / determinant;
            const tr::Vec3 s = subtract(origin, tri.a);
            const float u = dot(s, p) * inverse;
            if (u < 0.0f || u > 1.0f) continue;
            const tr::Vec3 q = cross(s, tri.edge1);
            const float v = dot(direction, q) * inverse;
            if (v < 0.0f || u + v > 1.0f) continue;
            const float hit = dot(tri.edge2, q) * inverse;
            if (hit > 0.0f && hit < best) {
                best = hit;
                found = true;
            }
        }
    }
    if (found) t = best;
    return found;
}

// The terrain cell under (x, z) as FUN_0056aa20 reads it: the grid index truncated, inside the W x H grid, a patch with data; the
// height from the 12-bit value (holes keep it - flag 0 is read apart from the height, see CLAUDE.md "Sample word and flags").
bool PvsBuilder::terrainCell(float x, float z, float& height, bool& solid) const {
    if (!map_.hasTerrain) return false;
    const tr::Terrain& t = map_.terrain;
    const float spacing = t.header.spacing > 0 ? t.header.spacing : 1.0f;
    const int32_t i = int32_t((x - t.header.bounds.min.x) / spacing);
    const int32_t j = int32_t((z - t.header.bounds.min.z) / spacing);
    if (i < 0 || j < 0 || i >= terrainW_ || j >= terrainH_ || uint32_t(i) >= t.columns || uint32_t(j) >= t.rows) return false;
    if (!t.hasData(uint32_t(j), uint32_t(i))) return false;
    const uint16_t word = t.sampleAt(uint32_t(j), uint32_t(i)).word;
    solid = tr::terrainFlag(word, 0);
    height = tr::worldHeight(t.header, word) + (solid ? 0.0f : t.header.bounds.max.y);
    return true;
}

// FUN_00569f20: inside the terrain's x / z bounds, the four cells around the point must exist; bilinear height.
bool PvsBuilder::terrainInterpolated(float x, float z, float& height) const {
    if (!map_.hasTerrain) return false;
    const tr::TerrainHeader& h = map_.terrain.header;
    if (x < h.bounds.min.x || x > h.bounds.max.x || z < h.bounds.min.z || z > h.bounds.max.z) return false;
    const float spacing = h.spacing > 0 ? h.spacing : 1.0f;
    const float fx = (x - h.bounds.min.x) / spacing, fz = (z - h.bounds.min.z) / spacing;
    const float cx = std::floor(fx), cz = std::floor(fz);
    float h00, h10, h01, h11;
    bool solid;
    const float x0 = h.bounds.min.x + cx * spacing, z0 = h.bounds.min.z + cz * spacing;
    if (!terrainCell(x0 + 0.5f * spacing, z0 + 0.5f * spacing, h00, solid)) return false;
    if (!terrainCell(x0 + 0.5f * spacing, z0 + 1.5f * spacing, h01, solid)) return false;
    if (!terrainCell(x0 + 1.5f * spacing, z0 + 1.5f * spacing, h11, solid)) return false;
    if (!terrainCell(x0 + 1.5f * spacing, z0 + 0.5f * spacing, h10, solid)) return false;
    const float u = fx - cx, w = fz - cz;
    height = (h00 * (1 - u) + h10 * u) * (1 - w) + (h01 * (1 - u) + h11 * u) * w;
    return true;
}

// A point off the terrain counts only inside a body: one whose box holds it (the scene query) and whose sphere does.
bool PvsBuilder::insideObjectSphere(uint32_t leaf, const tr::Vec3& p) const {
    for (uint32_t b : leafBodies_[leaf]) {
        const Body& body = bodies_[b];
        if (!contains(body.box, p)) continue;
        if (length(subtract(p, body.centre)) < body.radius) return true;
    }
    return false;
}

// Every probe direction must hit something within 15 m, and nothing closer than 0.0375 m.
bool PvsBuilder::probesEnclosed(uint32_t leaf, const tr::Vec3& p) const {
    (void)leaf;
    for (const tr::Vec3& d : probeDirections_) {
        float t = 0;
        if (!nearestHit(p, d, kProbeLength, t)) return false;
        if (t < kProbeMinimum) return false;
    }
    return true;
}

std::vector<tr::Vec3> PvsBuilder::scout(uint32_t leaf, int type) const {
    std::vector<tr::Vec3> out;
    const int32_t node = leafNode(leaf);
    if (node < 0) return out;
    const tr::Bounds box = vis_.nodes[size_t(node)].bounds;
    // Profile type -> spacing and the minimum points per axis (the switch in FUN_00859000).
    float spacing = 1.0f, minimum = 2.0f;
    switch (type) {
        case 1: spacing = 10.0f; minimum = 1.0f; break;
        case 2: spacing = 5.0f; minimum = 5.0f; break;
        case 3: spacing = 1.0f; minimum = 8.0f; break;
        case 4: spacing = 0.5f; minimum = 15.0f; break;
        default: break;
    }
    const tr::Vec3 size = subtract(box.max, box.min);
    auto count = [&](float extent) {
        float c = extent / spacing;
        if (c <= minimum) c = minimum;
        if (c >= 100.0f) c = 100.0f;
        return int32_t(c);
    };
    int32_t cx = count(size.x), cy = count(size.y), cz = count(size.z);
    if (leafBodies_[leaf].empty() && double(cx) * cy * cz > 50.0) {
        cx = std::min(cx, 4);
        cy = std::min(cy, 3);
        cz = std::min(cz, 4);
    }
    const float margin = std::max(2.0f * kNear, 0.01f);
    const float stepX = (size.x - margin) / float(cx), stepY = (size.y - margin) / float(cy), stepZ = (size.z - margin) / float(cz);
    if (!(stepX > 0 && stepY > 0 && stepZ > 0)) return out;
    const float startX = box.min.x + kNear, startY = box.min.y + kNear, startZ = box.min.z + kNear;
    const float endX = startX + stepX * float(cx - 1), endY = startY + stepY * float(cy - 1), endZ = startZ + stepZ * float(cz - 1);

    // First pass: the grid (y, then z, then x) filtered by the terrain / object rule.
    std::vector<Candidate> grid;
    std::vector<Candidate> kept;
    const tr::Bounds terrainBounds = map_.hasTerrain ? map_.terrain.header.bounds : tr::Bounds{};
    for (float y = startY; y <= endY; y += stepY) {
        for (float z = startZ; z <= endZ; z += stepZ) {
            for (float x = startX; x <= endX; x += stepX) {
                Candidate c;
                c.p = tr::Vec3{x, y, z};
                grid.push_back(c);
                if (map_.hasTerrain && contains(terrainBounds, c.p)) {
                    float h = 0;
                    bool solid = false;
                    if (terrainCell(x, z, h, solid) && solid && y > h) {
                        c.aboveTerrain = true;
                        c.height = y - h;
                        kept.push_back(c);
                        continue;
                    }
                }
                if (insideObjectSphere(leaf, c.p)) kept.push_back(c);
            }
        }
    }
    // More than 1,000: shuffled (j = random % N for each i), the first 1,000 replace the grid's first 1,000 entries and the
    // rest of the grid follows (the game copies over the start of the grid list and keeps iterating to its end).
    std::vector<Candidate> list;
    if (kept.size() > kMaxCandidates) {
        int32_t state = leaf != 0 ? int32_t(leaf) : 342;
        const int32_t n = int32_t(kept.size());
        for (int32_t i = 0; i < n; ++i) {
            const int32_t j = n == 1 ? 0 : parkMiller(state) % n;
            std::swap(kept[size_t(i)], kept[size_t(j)]);
        }
        list.assign(kept.begin(), kept.begin() + kMaxCandidates);
        if (grid.size() > kMaxCandidates) list.insert(list.end(), grid.begin() + kMaxCandidates, grid.end());
    } else {
        list.swap(kept);
    }

    // Second pass: validity, inside the leaf, at most 50.
    for (const Candidate& c : list) {
        bool valid = false;
        if (!c.aboveTerrain) {
            valid = probesEnclosed(leaf, c.p);
        } else if (map_.hasTerrain && c.height >= kTerrainClearance) {
            valid = true;
            const float offsets[2] = {-kTerrainClearance, kTerrainClearance};
            for (int a = 0; a < 2 && valid; ++a)
                for (int b = 0; b < 2 && valid; ++b) {
                    float h = 0;
                    valid = terrainInterpolated(c.p.x + offsets[a], c.p.z + offsets[b], h) && c.p.y - h > kTerrainClearance;
                }
        }
        if (!valid || !contains(box, c.p)) continue;
        out.push_back(c.p);
        if (out.size() >= kMaxCameras) break;
    }
    return out;
}

std::vector<tr::Vec3> PvsBuilder::cameras(uint32_t leaf) const {
    // "Scouting Camera": profile type 2, then up to two retries with finer grids (BuildPVS's loop, types 2..4).
    for (int type = 2; type <= 4; ++type) {
        std::vector<tr::Vec3> c = scout(leaf, type);
        if (!c.empty()) return c;
    }
    return {};
}

bool PvsBuilder::beginRow(uint32_t leaf) {
    job_ = RowJob{};
    job_.leaf = leaf;
    lastViews = 0;
    lastViewSeconds = 0;
    const auto scoutStart = std::chrono::steady_clock::now();
    job_.points = cameras(leaf);
    lastScoutSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - scoutStart).count();
    job_.active = !job_.points.empty();
    if (job_.active) renderer_->clearPvsRow();
    return job_.active;
}

size_t PvsBuilder::viewsLeft() const { return job_.active ? job_.points.size() * 6 - job_.nextView : 0; }

bool PvsBuilder::stepRow(size_t maxViews) {
    if (!job_.active) return true;
    const auto start = std::chrono::steady_clock::now();
    // The scene draws into the PVS renderer with only what the game's builder renders: map bodies and terrain.
    ViewCamera& camera = scene_.camera();
    const ViewCamera savedCamera = camera;
    const bool server = scene_.showServer, clutter = scene_.showClutter, placeholders = scene_.showPlaceholders,
               lights = scene_.showLightMarkers, sounds = scene_.showSoundMarkers, regions = scene_.showRegions,
               areas = scene_.showMissionAreas, distances = scene_.gameDistances, entities = scene_.showEntities,
               terrain = scene_.showTerrain;
    const int only = scene_.onlyEntity;
    scene_.showServer = scene_.showClutter = scene_.showPlaceholders = false;
    scene_.showLightMarkers = scene_.showSoundMarkers = scene_.showRegions = scene_.showMissionAreas = false;
    scene_.gameDistances = scene_.showEntities = scene_.showTerrain = true;
    scene_.onlyEntity = -1;
    scene_.setDrawTarget(renderer_.get());

    // The cube faces (the builder's view table: yaw / pitch in steps of 90 degrees).
    struct Face { tr::Vec3 forward, up; };
    const Face faces[6] = {{{0, 0, 1}, {0, 1, 0}}, {{1, 0, 0}, {0, 1, 0}}, {{-1, 0, 0}, {0, 1, 0}},
                           {{0, 0, -1}, {0, 1, 0}}, {{0, -1, 0}, {0, 0, 1}}, {{0, 1, 0}, {0, 0, -1}}};
    const Mat4 projection = perspectiveLH(1.5707963f, 1.0f, kNear, kFar);
    const size_t total = job_.points.size() * 6;
    for (size_t done = 0; done < maxViews && job_.nextView < total; ++done, ++job_.nextView) {
        const tr::Vec3& p = job_.points[job_.nextView / 6];
        const Face& f = faces[job_.nextView % 6];
        camera.position = p;
        camera.explicitAxes = true;
        camera.axisForward = f.forward;
        camera.axisUp = f.up;
        camera.axisRight = cross(f.up, f.forward);
        camera.fovY = 1.5707963f;
        camera.nearZ = kNear;
        camera.farZ = kFar;
        scene_.queueDraws(Selection{});
        const Mat4 viewProjection = multiply(projection, camera.viewMatrix());
        renderer_->recordPvsView(inverse(viewProjection), p, pixelThreshold);
        ++lastViews;
    }

    scene_.setDrawTarget(nullptr);
    camera = savedCamera;
    scene_.showServer = server; scene_.showClutter = clutter; scene_.showPlaceholders = placeholders;
    scene_.showLightMarkers = lights; scene_.showSoundMarkers = sounds; scene_.showRegions = regions;
    scene_.showMissionAreas = areas; scene_.gameDistances = distances; scene_.showEntities = entities; scene_.showTerrain = terrain;
    scene_.onlyEntity = only;
    lastViewSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return job_.nextView >= total;
}

std::vector<uint32_t> PvsBuilder::finishRow() {
    if (!job_.active) return {};
    job_.active = false;
    std::vector<uint32_t> row = renderer_->downloadPvsRow();
    if (job_.leaf < leafCount_) row[job_.leaf >> 5] |= 1u << (job_.leaf & 0x1F);     // the leaf itself, always
    return row;
}

std::vector<uint32_t> PvsBuilder::computeRow(uint32_t leaf) {
    if (!beginRow(leaf)) return {};
    while (!stepRow(SIZE_MAX)) {}
    return finishRow();
}
