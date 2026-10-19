#include "SceneView.h"
#include "geofile.h"
#include "GpuTexture.h"
#include "dds.h"
#include "MarkerColours.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

float clientConfigValue(const std::string& gameRoot, const std::string& key, float fallback) {
    std::ifstream in(std::filesystem::u8path(gameRoot) / "client.cfg");
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        name.erase(name.find_last_not_of(" \t") + 1);
        name.erase(0, name.find_first_not_of(" \t"));
        if (tr::toLowerAscii(name) != tr::toLowerAscii(key)) continue;
        try { return std::stof(line.substr(eq + 1)); } catch (const std::exception&) { return fallback; }
    }
    return fallback;
}

// ------------------------------------------------------------------------------------------
// Camera
// ------------------------------------------------------------------------------------------
tr::Vec3 ViewCamera::forward() const {
    if (explicitAxes) return axisForward;
    return tr::Vec3{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}
tr::Vec3 ViewCamera::right() const { if (explicitAxes) return axisRight; return normalized(cross(tr::Vec3{0, 1, 0}, forward())); }
tr::Vec3 ViewCamera::up() const { if (explicitAxes) return axisUp; return cross(forward(), right()); }
Mat4 ViewCamera::viewMatrix() const { return viewLH(position, right(), up(), forward()); }
Mat4 ViewCamera::projectionMatrix(float aspect) const { return perspectiveLH(fovY, aspect, nearZ, farZ); }

// ------------------------------------------------------------------------------------------
// Construction
// ------------------------------------------------------------------------------------------
namespace {

// Same ramp as the 2D overview: low dark green, mid olive, high pale rock.
uint32_t heightColour(float t) {
    struct Rgb { float red, green, blue; };
    const Rgb low{70, 95, 50}, mid{135, 125, 90}, high{210, 200, 180};
    const Rgb& a = t < 0.5f ? low : mid;
    const Rgb& b = t < 0.5f ? mid : high;
    const float u = t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
    return packColour(uint8_t(a.red + (b.red - a.red) * u), uint8_t(a.green + (b.green - a.green) * u),
                      uint8_t(a.blue + (b.blue - a.blue) * u), 255);
}

// Bone local transform: translate * rotate * scale (column vectors), as in the .geo skeleton and .anm keys.
Mat4 boneLocal(const tr::Vec3& translation, const tr::Quat& rotation, const tr::Vec3& scale) {
    Mat4 m = rotationMatrix(rotation);
    m.row0 = {m.row0.x * scale.x, m.row0.y * scale.y, m.row0.z * scale.z, translation.x};
    m.row1 = {m.row1.x * scale.x, m.row1.y * scale.y, m.row1.z * scale.z, translation.y};
    m.row2 = {m.row2.x * scale.x, m.row2.y * scale.y, m.row2.z * scale.z, translation.z};
    return m;
}

// A contact point's frame in model space: its bone's bind pose x the point's rotation and position.
Mat4 contactPointFrame(const tr::GeoModel& geo, const tr::GeoContactPoint& cp) {
    Mat4 local = rotationMatrix(cp.rotation);
    local.row0.w = cp.position.x;
    local.row1.w = cp.position.y;
    local.row2.w = cp.position.z;
    if (cp.bone >= 0 && size_t(cp.bone) < geo.bones.size()) {
        Mat4 bind = identityMatrix();
        for (int32_t b = cp.bone; b >= 0 && size_t(b) < geo.bones.size(); b = geo.bones[size_t(b)].parent >= b ? -1 : geo.bones[size_t(b)].parent) {
            const tr::GeoBone& bone = geo.bones[size_t(b)];
            bind = multiply(boneLocal(bone.translation, bone.rotation, bone.scale), bind);
        }
        local = multiply(bind, local);
    }
    return local;
}

// Quaternion product: rotationMatrix(multiplyQuat(a, b)) = rotationMatrix(a) x rotationMatrix(b).
tr::Quat multiplyQuat(const tr::Quat& a, const tr::Quat& b) {
    return tr::Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// The rotation of an affine matrix (scale divided out of its columns) as a quaternion.
tr::Quat quatFromMatrix(const Mat4& m) {
    const float sx = std::sqrt(m.row0.x * m.row0.x + m.row1.x * m.row1.x + m.row2.x * m.row2.x);
    const float sy = std::sqrt(m.row0.y * m.row0.y + m.row1.y * m.row1.y + m.row2.y * m.row2.y);
    const float sz = std::sqrt(m.row0.z * m.row0.z + m.row1.z * m.row1.z + m.row2.z * m.row2.z);
    if (sx <= 0 || sy <= 0 || sz <= 0) return tr::Quat{};
    const float r00 = m.row0.x / sx, r01 = m.row0.y / sy, r02 = m.row0.z / sz;
    const float r10 = m.row1.x / sx, r11 = m.row1.y / sy, r12 = m.row1.z / sz;
    const float r20 = m.row2.x / sx, r21 = m.row2.y / sy, r22 = m.row2.z / sz;
    tr::Quat q;
    const float trace = r00 + r11 + r22;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = tr::Quat{(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q = tr::Quat{0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q = tr::Quat{(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q = tr::Quat{(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }
    return q;
}

// Inverse of an affine matrix (row3 = 0 0 0 1).
Mat4 affineInverse(const Mat4& m) {
    const float a = m.row0.x, b = m.row0.y, c = m.row0.z, d = m.row1.x, e = m.row1.y, f = m.row1.z, g = m.row2.x, h = m.row2.y, i = m.row2.z;
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float s = std::fabs(det) > 1e-12f ? 1.0f / det : 0.0f;
    Mat4 r;
    r.row0 = {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s, 0};
    r.row1 = {(f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s, 0};
    r.row2 = {(d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s, 0};
    r.row3 = {0, 0, 0, 1};
    const tr::Vec3 t{m.row0.w, m.row1.w, m.row2.w};
    r.row0.w = -(r.row0.x * t.x + r.row0.y * t.y + r.row0.z * t.z);
    r.row1.w = -(r.row1.x * t.x + r.row1.y * t.y + r.row1.z * t.z);
    r.row2.w = -(r.row2.x * t.x + r.row2.y * t.y + r.row2.z * t.z);
    return r;
}

// A bone's stored inverse bind as a column-vector matrix (the file has Direct3D row vectors: transpose).
Mat4 inverseBindMatrix(const tr::GeoBone& bone) {
    const tr::Vec3& x = bone.inverseBindX; const tr::Vec3& y = bone.inverseBindY;
    const tr::Vec3& z = bone.inverseBindZ; const tr::Vec3& t = bone.inverseBindT;
    Mat4 m = identityMatrix();
    m.row0 = {x.x, y.x, z.x, t.x};
    m.row1 = {x.y, y.y, z.y, t.y};
    m.row2 = {x.z, y.z, z.z, t.z};
    return m;
}

// Skinning matrix per skeleton bone: posed world * inverse(bind world), the pose sampled from the animation at time.
// Bones without a track keep their bind local transform; a track's missing components keep the bind values.
// Parents come before children in PSKE (parent index < bone index).
// skeleton (optional): pose bones from that model's skeleton, matched by name - avatar swap-set parts follow the body's
// skeleton, and some part files carry a different hierarchy (avatar_hum_m_face_asian_v04_round: Spine under Pelvis,
// without the Body Correction / Spine1 target bones), so posing them on their own chain put the head at hip height.
// boneTracks (optional): per bone of geo, its track index in animation (-1: none), as boneTrackIndices gives - findTrack per
// bone and frame was most of the per-frame cost of animated models.
// Per bone of geo: the index of its track in animation (tr::findTrack's name match), -1 for none.
std::vector<int> boneTrackIndices(const tr::GeoModel& geo, const tr::Animation& animation) {
    std::vector<int> indices;
    for (const tr::GeoBone& bone : geo.bones) {
        const tr::AnimTrack* track = tr::findTrack(animation, bone.name);
        indices.push_back(track ? int(track - animation.tracks.data()) : -1);
    }
    return indices;
}

// skeletonPosed / skeletonIndex: the pose skeleton already posed, and per bone of geo its index there (-1: none) - in place of
// posing `skeleton` here and matching bones by name.
std::vector<Mat4> skinMatrices(const tr::GeoModel& geo, const tr::Animation& animation, float time, const tr::GeoModel* skeleton = nullptr,
                               std::vector<Mat4>* posedOut = nullptr, const std::vector<int>* boneTracks = nullptr,
                               const std::vector<Mat4>* skeletonPosedIn = nullptr, const std::vector<int>* skeletonIndex = nullptr) {
    std::unordered_map<std::string, Mat4> skeletonPosed;
    const bool indexed = skeletonPosedIn && skeletonIndex;
    if (skeleton && !indexed) {
        const size_t m = skeleton->bones.size();
        std::vector<Mat4> world(m);
        for (size_t k = 0; k < m; ++k) {
            const tr::GeoBone& bone = skeleton->bones[k];
            tr::Vec3 translation = bone.translation, scale = bone.scale;
            tr::Quat rotation = bone.rotation;
            if (const tr::AnimTrack* track = tr::findTrack(animation, bone.name)) {
                const tr::AnimKey key = tr::sampleTrack(*track, time);
                if (track->hasTranslation) translation = key.translation;
                if (track->hasRotation) rotation = key.rotation;
                if (track->hasScale) scale = key.scale;
            }
            const Mat4 local = boneLocal(translation, rotation, scale);
            world[k] = bone.parent >= 0 && size_t(bone.parent) < k ? multiply(world[size_t(bone.parent)], local) : local;
            skeletonPosed.emplace(tr::toLowerAscii(bone.name), world[k]);
        }
    }
    const size_t n = geo.bones.size();
    std::vector<Mat4> bindWorld(n), posedWorld(n), skin(n);
    std::vector<tr::Vec3> rootScale(n);
    for (size_t k = 0; k < n; ++k) {
        const tr::GeoBone& bone = geo.bones[k];
        tr::Vec3 translation = bone.translation, scale = bone.scale;
        tr::Quat rotation = bone.rotation;
        const tr::AnimTrack* track = nullptr;
        if (boneTracks && k < boneTracks->size()) track = (*boneTracks)[k] >= 0 ? &animation.tracks[size_t((*boneTracks)[k])] : nullptr;
        else track = tr::findTrack(animation, bone.name);
        if (track) {
            const tr::AnimKey key = tr::sampleTrack(*track, time);
            if (track->hasTranslation) translation = key.translation;
            if (track->hasRotation) rotation = key.rotation;
            if (track->hasScale) scale = key.scale;
        }
        const Mat4 bindLocal = boneLocal(bone.translation, bone.rotation, bone.scale);
        const Mat4 posedLocal = boneLocal(translation, rotation, scale);
        const bool hasParent = bone.parent >= 0 && size_t(bone.parent) < k;
        rootScale[k] = hasParent ? rootScale[size_t(bone.parent)] : bone.scale;
        bindWorld[k] = hasParent ? multiply(bindWorld[size_t(bone.parent)], bindLocal) : bindLocal;
        posedWorld[k] = hasParent ? multiply(posedWorld[size_t(bone.parent)], posedLocal) : posedLocal;
        if (indexed) {
            if (k < skeletonIndex->size() && (*skeletonIndex)[k] >= 0 && size_t((*skeletonIndex)[k]) < skeletonPosedIn->size())
                posedWorld[k] = (*skeletonPosedIn)[size_t((*skeletonIndex)[k])];
        } else if (!skeletonPosed.empty()) {
            const auto found = skeletonPosed.find(tr::toLowerAscii(bone.name));
            if (found != skeletonPosed.end()) posedWorld[k] = found->second;
        }
        if (bone.hasInverseBind) {
            // The bone's stored inverse bind (mesh space). npc_eloh_hologram_large needs it: its mesh is not around the
            // PSKE bind pose (figure pelvis at y 1.21 vs 6.14; the rays shield 12.9 m out along z vs 0).
            skin[k] = multiply(posedWorld[k], inverseBindMatrix(bone));
            continue;
        }
        // No stored matrix: the bind skeleton shrunk by its root bone's scale (the rule that fitted the hologram figure).
        Mat4 rootScaling = identityMatrix();
        rootScaling.row0.x = rootScale[k].x;
        rootScaling.row1.y = rootScale[k].y;
        rootScaling.row2.z = rootScale[k].z;
        skin[k] = multiply(multiply(posedWorld[k], affineInverse(bindWorld[k])), rootScaling);
    }
    if (posedOut) *posedOut = posedWorld;
    return skin;
}

// creature_appearance colour (ABGR, red in the low byte) as a shader hue; w = 0 when there is none.
Vec4f hueFromAbgr(uint32_t colour) {
    if (colour == 0) return Vec4f{0, 0, 0, 0};
    return Vec4f{float(colour & 0xFF) / 255.0f, float((colour >> 8) & 0xFF) / 255.0f, float((colour >> 16) & 0xFF) / 255.0f, 1.0f};
}

// TR* deferred effects (G-buffer pixel / vertex shaders in effects.glm, disassembled 2026-10-11): which material
// parameters fill map slots 1 and 2, what each slot does, the environment mode and flag bits (see TrMaterial).
struct TrDescription {
    bool known = false;
    const char* slot1 = nullptr;      // texture parameter of slot 1
    const char* slot1Uv = nullptr;    // its UV-set parameter
    int kind1 = 0;                    // 1 modifier, 2 detail, 3 decal
    const char* slot2 = nullptr;
    const char* slot2Uv = nullptr;
    int kind2 = 0;
    int envMode = 0;                  // 1 by detail.a before the decal, 2 after it, 3 by diffuse.a
    int bits = 0;                     // 2 albedo x (2 vcol + emissive), 4 specular, 8 emissive glow
};

TrDescription trDescription(const std::string& effect) {
    auto is = [&](const char* name) { return effect.rfind(name, 0) == 0 && effect.size() > std::strlen(name) && effect[std::strlen(name)] == '.'; };
    TrDescription d;
    d.known = true;
    if (is("trdiffmapdetglossmapenvmapdecmapvertcolor") || is("trdiffmapdetglossmapdecmapenvmapvertcolor")) {
        d.slot1 = "DetailTexture"; d.slot1Uv = "DetailTexCoord"; d.kind1 = 2;
        d.slot2 = "DecalTexture"; d.slot2Uv = "DecalTexCoord"; d.kind2 = 3;
        d.envMode = is("trdiffmapdetglossmapenvmapdecmapvertcolor") ? 1 : 2;
        d.bits = 0x4;
    } else if (is("trdiffmapdiffmapdecmapvertcolor") || is("trdiffmapdiffmapdetmapvertcolor")) {
        const bool decal = is("trdiffmapdiffmapdecmapvertcolor");
        d.slot1 = "ModifierTexture"; d.slot1Uv = "ModifierTexCoord"; d.kind1 = 1;
        d.slot2 = decal ? "DecalTexture" : "DetailTexture"; d.slot2Uv = decal ? "DecalTexCoord" : "DetailTexCoord"; d.kind2 = decal ? 3 : 2;
        d.bits = 0x4 | 0x8;
    } else if (is("trdiffmapdetmapdecmapvertcolor")) {
        d.slot1 = "DetailTexture"; d.slot1Uv = "DetailTexCoord"; d.kind1 = 2;
        d.slot2 = "DecalTexture"; d.slot2Uv = "DecalTexCoord"; d.kind2 = 3;
        d.bits = 0x2;                                        // no specular (oC1 = 0, 0, 0, 1)
    } else if (is("trdiffmapdecmapvertcolor")) {
        d.slot1 = "DecalTexture"; d.slot1Uv = "DecalTexCoord"; d.kind1 = 3;
        d.bits = 0x4;
    } else if (is("trdiffmapdetmapvertcolor")) {
        d.slot1 = "DetailTexture"; d.slot1Uv = "DetailTexCoord"; d.kind1 = 2;
        d.bits = 0x4;
    } else if (is("trdiffglossmapenvmapvertcolor")) {
        d.envMode = 3;
        d.bits = 0x4;
    } else if (is("trdiffmapvertcolorwave")) {
        // psWaveDeferred[1] (disassembled 2026-10-20): diffuse x vertex colour x 2 + MatEmissive (0 on 310 of 313 shipped pieces),
        // texkill diffuse.a < 0.25 (the piece's cut-out), oC1 = (0, 0, 0, 1): no specular. Its vertex sway (gSwayRate /
        // gSwayAmplitude / gTime) is not reproduced.
        d.bits = 0x0;
    } else if (is("trdiffmapvertcolor") || is("trdiffmapglowmapvertcolor") || is("trdiffmapglowmaphuevertcolor")) {
        d.bits = 0x4;                                        // diffuse (and glow map) only; specular from MatSpecular
    } else {
        d.known = false;
    }
    return d;
}

// A map entity's hue1 as SetHue takes it (bytes / 255); (0, 0, 0, 255) is the client's "not set" value (w = 0).
Vec4f entityHue(const tr::HueColour& c) {
    if (c.red == 0 && c.green == 0 && c.blue == 0 && c.alpha == 255) return Vec4f{0, 0, 0, 0};
    return Vec4f{float(c.red) / 255.0f, float(c.green) / 255.0f, float(c.blue) / 255.0f, 1.0f};
}

void unitCube(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& triangles, std::vector<uint32_t>& lines) {
    // Corner k of [0,1]^3: x = bit 0, y = bit 1, z = bit 2.
    for (uint32_t k = 0; k < 8; ++k) {
        MeshVertex v;
        v.positionX = float(k & 0x1);
        v.positionY = float((k >> 1) & 0x1);
        v.positionZ = float((k >> 2) & 0x1);
        v.normalX = v.positionX - 0.5f;          // rough normals are enough for placeholders
        v.normalY = v.positionY - 0.5f;
        v.normalZ = v.positionZ - 0.5f;
        vertices.push_back(v);
    }
    const uint32_t faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
        triangles.insert(triangles.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
    }
    for (uint32_t k = 0; k < 8; ++k)
        for (uint32_t bit = 1; bit < 8; bit <<= 1)
            if ((k & bit) == 0) lines.insert(lines.end(), {k, k | bit});
}

}  // namespace

SceneView::SceneView(Renderer& renderer, const tr::AssetLibrary& assets, const tr::MapBundle& map,
                     std::function<std::string(int32_t)> meshForClass, std::function<bool(int32_t)> classKnown,
                     std::function<int32_t(int32_t)> cullingLayerForClass, uint32_t terrainStep)
    : renderer_(&renderer), ownRenderer_(&renderer), assets_(assets), map_(map), cullingLayerForClass_(cullingLayerForClass), meshForClass_(meshForClass),
      classKnown_(classKnown) {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> triangles, lines;
    unitCube(vertices, triangles, lines);
    unitBox_ = renderer_->createMesh(vertices, triangles, false);
    unitBoxLines_ = renderer_->createMesh(vertices, lines, true);
    auto lineMesh = [&](const std::vector<tr::Vec3>& points, const std::vector<uint32_t>& segments) {
        std::vector<MeshVertex> v;
        for (const tr::Vec3& p : points) {
            MeshVertex m;
            m.positionX = p.x; m.positionY = p.y; m.positionZ = p.z;
            m.normalY = 1;
            v.push_back(m);
        }
        return renderer_->createMesh(v, segments, true);
    };
    {
        std::vector<tr::Vec3> points;
        std::vector<uint32_t> segments;
        for (uint32_t k = 0; k < 64; ++k) {
            const float a = 6.2831853f * float(k) / 64.0f;
            points.push_back(tr::Vec3{std::sin(a), 0, std::cos(a)});
            segments.insert(segments.end(), {k, (k + 1) & 0x3F});
        }
        unitCircle_ = lineMesh(points, segments);
    }
    unitSquare_ = lineMesh({{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}}, {0, 1, 1, 2, 2, 3, 3, 0});
    // Facing arrow along -z: character models are authored facing -z (toes and face point that way), so
    // a server rotation r faces (-sin r, 0, -cos r); verified in game (rotation 0 faces south, -z).
    arrow_ = lineMesh({{0, 0, 0}, {0, 0, -1}, {-0.25f, 0, -0.7f}, {0.25f, 0, -0.7f}}, {0, 1, 1, 2, 1, 3});
    pole_ = lineMesh({{0, 0, 0}, {0, 1, 0}}, {0, 1});

    if (map_.hasTerrain) {
        if (terrainStep != 0) terrainFixedLevel_ = terrainStep >= 8 ? 3 : terrainStep >= 4 ? 2 : terrainStep >= 2 ? 1 : 0;
        buildTerrainChunks();
        buildTerrainMaterial();
    }

    // Placements, and the set of models to load.
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    placements_.reserve(map_.map.entities.size());
    for (const tr::MapEntity& e : map_.map.entities) {
        placements_.push_back(makePlacement(e));
        minX = std::min(minX, e.location.x); maxX = std::max(maxX, e.location.x);
        minZ = std::min(minZ, e.location.z); maxZ = std::max(maxZ, e.location.z);
    }

    // Start above the middle of the content, looking north (+z) and down.
    const float cx = map_.map.entities.empty() ? 0.0f : (minX + maxX) * 0.5f;
    const float cz = map_.map.entities.empty() ? 0.0f : (minZ + maxZ) * 0.5f;
    float ground = 0;
    if (!(map_.hasTerrain && map_.terrain.heightAtWorld(cx, cz, ground)))
        for (const tr::MapEntity& e : map_.map.entities) ground = std::max(ground, e.location.y);
    camera_.position = tr::Vec3{cx, ground + 120.0f, cz - 160.0f};
    camera_.yaw = 0;
    camera_.pitch = -0.55f;

    if (!map_.lightsXml.empty()) {
        try {
            mapLights_ = tr::readLights(map_.lightsXml);
            lightModifiers_ = tr::readLightModifiers(map_.lightsXml);
            loadGlobeMaps();
        } catch (const std::exception& e) {
            mapLightsError_ = e.what();
        }
    }
    reloadAudioEmitters();

    // The map's starting sky time is in seconds into its day (e.g. 3375 of 10800 = 07:30).
    if (map_.map.dayLengthSeconds > 0)
        startTimeOfDay_ = float(map_.map.startingSkyTime % map_.map.dayLengthSeconds) / float(map_.map.dayLengthSeconds);
    timeOfDay = startTimeOfDay_;
}

void SceneView::setClassHues(const std::function<bool(int32_t, tr::HueColour&, tr::HueColour&)>& classHue) {
    classHue_ = classHue;
    for (size_t k = 0; k < placements_.size() && k < map_.map.entities.size(); ++k) {
        tr::HueColour hue1 = map_.map.entities[k].hue1, hue2 = map_.map.entities[k].hue2;
        if (classHue) classHue(map_.map.entities[k].classId, hue1, hue2);
        placements_[k].hue = entityHue(hue1);
    }
}

void SceneView::setRegionSkies(const std::vector<std::pair<int32_t, std::string>>& regionSkies) {
    skies_.clear();
    skyCurrent_ = skyPrevious_ = size_t(-1);
    skyIndex = 0;
    bool haveRegion0 = false;
    for (const auto& rs : regionSkies) {
        if (rs.second.empty()) continue;
        const std::string name = tr::toLowerAscii(rs.second);
        auto it = std::find_if(skies_.begin(), skies_.end(), [&](const SkyChoice& c) { return c.name == name; });
        if (it == skies_.end()) {
            SkyChoice c;
            c.name = name;
            try {
                c.sky = tr::readSky(assets_.read(name));
                c.loaded = true;
            } catch (const std::exception& e) {
                c.error = e.what();
            }
            skies_.push_back(std::move(c));
            it = skies_.end() - 1;
        }
        it->regions.push_back(rs.first);
        if (rs.first == 0 && !haveRegion0) {
            skyIndex = size_t(it - skies_.begin());
            haveRegion0 = true;
        }
    }
    // Base skies of "_tranz" / "_war" variants (sky_foreas_wilderness_default_tranz.sky -> ..._default.sky) are offered
    // as manual choices only: no region names them. The game uses the region skies as named -
    // RegionManagerClient::DoTransitionSky (FUN_0052c2d0) takes the first region of the priority-sorted list (the camera's
    // regions plus region 0) whose sky is set and loads, and clears the sky when none has one (nothing is kept from an
    // earlier region); the transition-sky slot is unused (gamemap.py SetTransSky(False)). Seen in game: Bane dropships
    // (a layer only _tranz / _war have) in the night sky at Ranja Gorge, whose regions 9 / 49 name no sky.
    readRegionSettings();
    for (size_t k = 0; k < skies_.size(); ++k)
        for (int32_t region : skies_[k].regions) regionSky_[region] = k;
    const size_t regionSkyCount = skies_.size();
    for (size_t k = 0; k < regionSkyCount; ++k) {
        std::string base = skies_[k].name;
        for (const char* suffix : {"_tranz.sky", "_war.sky"}) {
            const std::string s = suffix;
            if (base.size() > s.size() && base.compare(base.size() - s.size(), s.size(), s) == 0) {
                base = base.substr(0, base.size() - s.size()) + ".sky";
                break;
            }
        }
        if (base == skies_[k].name || !assets_.exists(base)) continue;
        auto it = std::find_if(skies_.begin(), skies_.end(), [&](const SkyChoice& c) { return c.name == base; });
        if (it == skies_.end()) {
            SkyChoice c;
            c.name = base;
            try {
                c.sky = tr::readSky(assets_.read(base));
                c.loaded = true;
            } catch (const std::exception& e) {
                c.error = e.what();
            }
            skies_.push_back(std::move(c));
            it = skies_.end() - 1;
        }
    }
    // The skies' layer models (queueSky) load with the map's models.
    for (const SkyChoice& c : skies_)
        if (c.loaded)
            for (const tr::SkyLayer& layer : c.sky.layers)
                if (layer.enabled && !layer.geoFilename.empty() && assets_.exists(layer.geoFilename)) objectModelFor(layer.geoFilename);
}

namespace {
bool insideArea(const AreaShape& a, const tr::Vec3& p) {
    if (p.y < a.minY || p.y > a.maxY) return false;
    const float dx = p.x - a.position.x, dz = p.z - a.position.z;
    if (a.shape == 2) return std::fabs(dx) <= a.halfX && std::fabs(dz) <= a.halfZ;
    return dx * dx + dz * dz <= a.radius * a.radius;
}
}  // namespace

void SceneView::readRegionSettings() {
    for (const tr::MapRegion& r : map_.map.regions) {
        regionPriority_[r.regionId] = r.priority;
        regionFlags_[r.regionId] = r.enabledFlags;
    }
}

std::vector<int32_t> SceneView::regionsAtCamera() const {
    // regionmanager.py OnUpdateRegions: the regions the player is in (server map_region volumes), then region 0
    // (DEFAULT_REGION_ID) appended, stably sorted by MapRegion priority, highest first.
    // A volume counts as Rasa.NET's RegionManager decides (Structures/MapRegion.Contains): enabled, the height band, the shape, and
    // its underground mode - 1 only underground, 2 only on the surface, "underground" being navmesh more than 2 m below the terrain
    // (NavMeshBuilder.IsUnderTerrain, NavMeshFlags.UndergroundDepth). Here the camera is tested against the terrain (no navmesh;
    // the camera stands for the player - APPROXIMATE). Verified at Ranja Cavern (region 33, underground only): in game the name and
    // radar switch at the cave mouth (x -482 .. -476), not at the box edge (x -445).
    std::vector<int32_t> inside;
    float ground = 0;
    const bool underground = map_.hasTerrain && map_.terrain.heightAtWorld(camera_.position.x, camera_.position.z, ground) &&
                             camera_.position.y < ground - 2.0f;
    if (serverContent_)
        for (const MapRegion& r : serverContent_->regions) {
            if (!r.enabled || r.regionId == 0 || !insideArea(r.area, camera_.position)) continue;
            if ((r.underground == 1 && !underground) || (r.underground == 2 && underground)) continue;
            inside.push_back(r.regionId);
        }
    inside.push_back(0);
    const auto priority = [&](int32_t id) { const auto it = regionPriority_.find(id); return it == regionPriority_.end() ? 0 : it->second; };
    std::stable_sort(inside.begin(), inside.end(), [&](int32_t a, int32_t b) { return priority(a) > priority(b); });
    return inside;
}

size_t SceneView::activeSkyIndex() const {
    if (!skyByRegion || !serverContent_ || serverContent_->regions.empty()) return skyIndex;
    // RegionManagerClient's transition (FUN_0077d150): every region whose enabledFlags has bit 0 goes into the sky list;
    // DoTransitionSky takes the first of them whose sky loads.
    for (int32_t id : regionsAtCamera()) {
        const auto flags = regionFlags_.find(id);
        if (flags == regionFlags_.end() || (flags->second & 0x1) == 0) continue;
        const auto it = regionSky_.find(id);
        if (it != regionSky_.end() && it->second < skies_.size() && skies_[it->second].loaded) return it->second;
    }
    return skyIndex;
}

void SceneView::setRegionEnvironmentMaps(const std::vector<std::pair<int32_t, std::string>>& regionEnvironmentMaps) {
    readRegionSettings();
    regionEnvironmentMap_.clear();
    std::vector<std::string> names{"env1.dds"};
    for (const auto& re : regionEnvironmentMaps) {
        regionEnvironmentMap_[re.first] = tr::toLowerAscii(re.second);
        if (!re.second.empty()) names.push_back(tr::toLowerAscii(re.second));
    }
    for (const std::string& name : names) {
        if (environmentCubes_.count(name)) continue;
        SDL_GPUTexture* cube = nullptr;
        try {
            tr::Bytes data;
            if (assets_.tryRead(name, data)) cube = uploadDdsCubeTexture(renderer_->device(), data);
            else environmentMapErrors_[name] = "not found in the archives";
        } catch (const std::exception& e) {
            environmentMapErrors_[name] = e.what();
        }
        environmentCubes_[name] = cube;
    }
}

std::string SceneView::activeEnvironmentMap() const {
    std::string name;
    for (int32_t id : regionsAtCamera()) {
        const auto flags = regionFlags_.find(id);
        if (flags == regionFlags_.end() || (flags->second & 0x2) == 0) continue;
        const auto it = regionEnvironmentMap_.find(id);
        if (it != regionEnvironmentMap_.end()) name = it->second;
        break;
    }
    // TransitionEnvironmentMap (FUN_0052c620): an empty name becomes "Env1.dds"; a failed load also falls back to it.
    const auto loaded = [&](const std::string& n) { const auto it = environmentCubes_.find(n); return it != environmentCubes_.end() && it->second; };
    if (!name.empty() && loaded(name)) return name;
    return loaded("env1.dds") ? "env1.dds" : "";
}

void SceneView::advanceSky(float dt) {
    const size_t active = activeSkyIndex();
    const bool usable = active < skies_.size() && skies_[active].loaded;
    if (skyCurrent_ == size_t(-1) || !skyTransitions || !usable) {
        skyCurrent_ = usable ? active : size_t(-1);
        skyPrevious_ = size_t(-1);
        skyElapsed_ = skyFadeSeconds;                // the first sky (map load) is shown at once
        return;
    }
    if (active != skyCurrent_) {
        skyPreviousStart_ = std::min(1.0f, skyElapsed_ / skyFadeSeconds);   // fades out from where it is
        skyPrevious_ = skyCurrent_;
        skyCurrent_ = active;
        skyElapsed_ = 0;
    } else {
        skyElapsed_ += std::clamp(dt, 0.0f, 1.0f);       // real time (a long frame still counts, unlike the animation clock)
    }
    if (skyPrevious_ != size_t(-1) && skyPreviousStart_ - skyElapsed_ / skyFadeSeconds <= 0) skyPrevious_ = size_t(-1);
}

std::vector<SceneView::SkyWeight> SceneView::skyWeights() const {
    std::vector<SkyWeight> out;
    const auto loaded = [&](size_t i) { return i < skies_.size() && skies_[i].loaded; };
    if (skyCurrent_ == size_t(-1) || !skyTransitions) {
        if (loaded(activeSkyIndex())) out.push_back(SkyWeight{activeSkyIndex(), 1.0f});
        return out;
    }
    if (loaded(skyPrevious_)) {
        const float w = skyPreviousStart_ - skyElapsed_ / skyFadeSeconds;
        if (w > 0) out.push_back(SkyWeight{skyPrevious_, w});
    }
    if (loaded(skyCurrent_)) out.push_back(SkyWeight{skyCurrent_, std::min(1.0f, skyElapsed_ / skyFadeSeconds)});
    return out;
}

bool SceneView::skyFromRegion() const { return skyByRegion && serverContent_ && !serverContent_->regions.empty(); }

int SceneView::shadowCacheEntry(ShadowCache& cache, const tr::PlacedLight& light, uint32_t size, int views) const {
    const int kind = int(light.kind);
    const tr::Quat& q = light.worldOrientation;
    for (size_t k = 0; k < cache.entries.size(); ++k) {
        const ShadowCacheEntry& e = cache.entries[k];
        if (e.alive && e.kind == kind && e.size == size && e.views == views && e.isStatic == light.isStatic && e.range == light.range &&
            e.width == light.width && e.length == light.length && e.penumbra == light.penumbraAngle && e.position.x == light.position.x &&
            e.position.y == light.position.y && e.position.z == light.position.z && e.orientation.x == q.x && e.orientation.y == q.y &&
            e.orientation.z == q.z && e.orientation.w == q.w)
            return int(k);
    }
    constexpr int perSide = int(Renderer::shadowAtlasSize / 128u);
    const int cells = int(size / 128u);
    for (;;) {
        std::vector<uint8_t> used(size_t(perSide * perSide), 0);
        for (const ShadowCacheEntry& e : cache.entries) {
            if (!e.alive) continue;
            const int c = int(e.size / 128u);
            for (const auto& cell : e.cells)
                for (int y = cell.second; y < cell.second + c; ++y)
                    for (int x = cell.first; x < cell.first + c; ++x) used[size_t(y * perSide + x)] = 1;
        }
        std::vector<std::pair<int, int>> placed;
        for (int v = 0; v < views; ++v) {
            bool found = false;
            for (int y = 0; y + cells <= perSide && !found; y += cells)
                for (int x = 0; x + cells <= perSide && !found; x += cells) {
                    bool free = true;
                    for (int yy = y; yy < y + cells && free; ++yy)
                        for (int xx = x; xx < x + cells && free; ++xx) free = used[size_t(yy * perSide + xx)] == 0;
                    if (!free) continue;
                    for (int yy = y; yy < y + cells; ++yy)
                        for (int xx = x; xx < x + cells; ++xx) used[size_t(yy * perSide + xx)] = 1;
                    placed.emplace_back(x, y);
                    found = true;
                }
            if (!found) break;
        }
        if (int(placed.size()) == views) {
            ShadowCacheEntry e;
            e.alive = true;
            e.kind = kind;
            e.position = light.position;
            e.range = light.range;
            e.width = light.width;
            e.length = light.length;
            e.penumbra = light.penumbraAngle;
            e.orientation = q;
            e.size = size;
            e.views = views;
            e.isStatic = light.isStatic;
            e.cells = placed;
            for (size_t k = 0; k < cache.entries.size(); ++k)
                if (!cache.entries[k].alive) {
                    cache.entries[k] = e;
                    return int(k);
                }
            cache.entries.push_back(e);
            return int(cache.entries.size() - 1);
        }
        // Full: free the least recently used entry not in use this frame, and try again.
        int oldest = -1;
        for (size_t k = 0; k < cache.entries.size(); ++k) {
            const ShadowCacheEntry& e = cache.entries[k];
            if (e.alive && e.lastUsed != shadowFrame_ && (oldest < 0 || e.lastUsed < cache.entries[size_t(oldest)].lastUsed)) oldest = int(k);
        }
        if (oldest < 0) return -1;
        cache.entries[size_t(oldest)].alive = false;
        cache.entries[size_t(oldest)].cells.clear();
    }
}

SceneLighting SceneView::lighting() const {
    SceneLighting l = skyLightingAt();
    l.showLight = showLightBuffer;
    l.debugView = debugView;
    l.cameraForward = camera_.forward();
    if (!showMapLights) return l;
    // Placed lights: the enabled ones nearest the camera (distance to their range sphere), up to the shader's limit.
    // Candidates: the map's lights.xml, and lights on map entities' models (contact point "lsys" templates) placed
    // with the entity (not on culling layer 6: the engine never adds those bodies to the scene).
    std::vector<std::pair<float, tr::PlacedLight>> nearest;
    // Only lights whose volume is in view: the game draws a light pass only for lights whose volume is visible (apitrace of
    // Luna Cavern, 2026-10-14: the 27 light passes of a frame at the fountain are exactly the lights in front of the camera;
    // ranking by distance alone spent the slots on lights behind the camera).
    const float aspect = float(renderer_->width()) / float(std::max<uint32_t>(1, renderer_->height()));
    const Mat4 viewProjection = multiply(camera_.projectionMatrix(aspect), camera_.viewMatrix());
    const Vec4f& r0 = viewProjection.row0; const Vec4f& r1 = viewProjection.row1;
    const Vec4f& r2 = viewProjection.row2; const Vec4f& r3 = viewProjection.row3;
    const Vec4f planes[5] = {
        {r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w}, {r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w},
        {r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w}, {r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w},
        {r2.x, r2.y, r2.z, r2.w}};
    auto inView = [&](const tr::Vec3& c, float radius) {
        for (const Vec4f& p : planes) {
            const float n = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            if ((p.x * c.x + p.y * c.y + p.z * c.z + p.w) / n < -radius) return false;
        }
        return true;
    };
    struct ShadowCandidate {
        float value;
        tr::Vec3 position;
        float range;
    };
    std::vector<ShadowCandidate> shadowPool;
    auto consider = [&](const tr::PlacedLight& light) {
        if (!light.enabled || light.range <= 0 || light.intensity <= 0) return;
        // The light node's bounds (tabula_rasa.exe, decoded 2026-10-18) - the sphere TRasa::SubmitVisibleLights (FUN_00811410) tests
        // against the view frustum's planes (FUN_005e6960: plane distance >= -radius) and whose radius (+ 0xc) the shadow selection's
        // value subtracts:
        // - point: centre the light, radius Range (the point's Range getter reads + 0xc; its light volume is a sphere of Range);
        // - spot (FUN_00878e40): the cone's circumscribed sphere - radius = (2 R tan(a / 2) x 0.5) / sin(a) = R / (2 cos^2(a / 2)),
        //   a = the penumbra angle, centre = the light + its axis x radius (FUN_005ecb60); radius >= 1e6 (0x00c2b848) -> infinite;
        // - plane: a box (FUN_005ecac0: centre = the box's middle, radius = half its diagonal) whose x / y / max z the Width / Length /
        //   Range setters write (FUN_008799d0 / FUN_00879aa0 / FUN_00879b70: +-Width / 2, +-Length / 2, Range). Its min z was not
        //   found in the code; fitted to the column capture (tabula_rasa.3.trace, Light_29, 6,719 frames as the camera turned): the box
        //   z 0..Range - from the light forward along its +z - decides in every frame whether the light is drawn (with the previous
        //   frame's camera: the game culls with a one-frame lag), z +-Range in 6,708, a Range sphere in 6,710.
        // Checked on three apitrace captures: the firing range (the 14 lights of the final frame, the same 12 kept), the command tent
        // and the column at Light_29.
        float reach = light.range;
        tr::Vec3 centre = light.position;
        bool infinite = false;
        if (light.kind == tr::PlacedLight::Kind::Plane) {
            reach = std::sqrt(light.width * light.width * 0.25f + light.length * light.length * 0.25f + light.range * light.range * 0.25f);
            const Mat4 r = rotationMatrix(light.worldOrientation);
            centre = add(light.position, scaled(tr::Vec3{r.row0.z, r.row1.z, r.row2.z}, light.range * 0.5f));
        }
        if (light.kind == tr::PlacedLight::Kind::Spot && light.penumbraAngle > 0.0f) {
            const float c = std::cos(light.penumbraAngle * 0.5f * 0.0174533f);
            reach = c > 1e-6f ? light.range / (2.0f * c * c) : 1e30f;
            if (reach >= 1e6f) infinite = true;
            else centre = add(light.position, scaled(normalized(light.direction()), reach));
        }
        const bool visible = infinite || inView(centre, reach);
        // Shadow candidates (see the selection below): in view by that sphere and shown by the PVS (FUN_007cfc10).
        const bool spotShadow = light.kind == tr::PlacedLight::Kind::Spot && light.penumbraAngle > 0.5f && light.penumbraAngle < 179.0f;
        if (light.castsShadows && light.range > 0.6f && (spotShadow || light.kind != tr::PlacedLight::Kind::Spot) && visible) {
            const tr::Vec3 e{reach, reach, reach};
            if (infinite || map_.vis.nodes.empty() || tr::pvsBoxVisible(map_.vis, camera_.position, tr::Bounds{subtract(centre, e), add(centre, e)}))
                shadowPool.push_back(ShadowCandidate{infinite ? 0.0f : std::max(0.0f, length(subtract(light.position, camera_.position)) - reach),
                                                     light.position, light.range});
        }
        if (!visible) return;
        const float gap = infinite ? 0.0f : std::max(0.0f, length(subtract(centre, camera_.position)) - reach);
        if (gap <= drawDistance) nearest.emplace_back(gap, light);
    };
    for (const tr::PlacedLight& light : mapLights_) consider(animateLights ? modifiedLight(light) : light);
    auto considerModelLights = [&](const Model& model, const Mat4& world) {
        if (!model.loaded || model.lights.empty()) return;
        // Range is used as the property says (the point pass reads LightRange from the light's properties,
        // FUN_00875360), not scaled with the entity.
        const tr::Quat rotation = quatFromMatrix(world);
        // A light on a contact point of an animated bone follows it (e.g. prop_hum_light_rotate_v01's beacon).
        const std::vector<Mat4> animated = animatedPieceMatrices(model, lightTime);
        for (size_t k = 0; k < model.lights.size(); ++k) {
            tr::PlacedLight light = animateLights ? modifiedLight(model.lights[k]) : model.lights[k];
            const int32_t bone = k < model.lightBones.size() ? model.lightBones[k] : -1;
            if (bone >= 0 && size_t(bone) < animated.size()) {
                light.position = transformPoint(animated[size_t(bone)], light.position);
                light.worldOrientation = multiplyQuat(quatFromMatrix(animated[size_t(bone)]), light.worldOrientation);
            }
            light.position = transformPoint(world, light.position);
            light.worldOrientation = multiplyQuat(rotation, light.worldOrientation);
            consider(light);
        }
    };
    for (const Placement& p : placements_)
        if (!p.neverDrawn && p.model) considerModelLights(*p.model, p.world);
    // Server objects' models carry their contact-point lights the same way (one rule for client and server objects).
    for (const ServerPlacement& s : serverPlacements_) {
        if (s.area || !serverPlacementShown(s)) continue;
        for (const ServerBody& b : serverBodies(s, nullptr)) considerModelLights(*b.model, b.world);
    }
    // Special-FX lights of server objects' use states (SLIT; see tr::SfxLight): persistent ones only (durationMs 0) on the
    // object itself or an accessory (target 0), at contact point frame x (Position + Offset) - the same placement rule as
    // the model lights (UNVERIFIED for these); the curves are evaluated on the light clock.
    for (const ServerPlacement& s : serverPlacements_) {
        if (!showStateEffects) break;
        for (const StateLight& sl : s.stateLights) {
            const tr::SfxLight& e = sl.event;
            if (e.durationMs != 0 || e.target != 0) continue;
            Mat4 frame;
            tr::Quat cpRotation;
            if (!contactFrame(sl.body ? sl.body : s.model, e.contactPoint, 1, frame, cpRotation)) continue;
            tr::PlacedLight light;
            light.kind = e.spot ? tr::PlacedLight::Kind::Spot : tr::PlacedLight::Kind::Point;
            light.colour = tr::Vec3{tr::sfxCurveValue(e.colourR, lightTime), tr::sfxCurveValue(e.colourG, lightTime), tr::sfxCurveValue(e.colourB, lightTime)};
            light.range = tr::sfxCurveValue(e.range, lightTime);
            if (light.range < 0.06f) continue;               // the product switches the light off (FUN_00771410, 0x00c2b8f8)
            light.falloffPercent = tr::sfxCurveValue(e.falloffPercent, lightTime);
            light.intensity = tr::sfxCurveValue(e.intensity, lightTime);
            light.penumbraAngle = tr::sfxCurveValue(e.penumbraAngle, lightTime);
            light.coneAngle = tr::sfxCurveValue(e.coneAngle, lightTime);
            light.globeMap = e.globeMap;
            light.castsShadows = e.castsShadows;
            light.isStatic = false;          // the SLIT trigger (FUN_0075a510) sets IsStatic "false" on every light it makes
            light.position = transformPoint(s.world, transformPoint(frame, add(e.position, e.offset)));
            light.worldOrientation = multiplyQuat(quatFromMatrix(s.world), multiplyQuat(cpRotation, e.rotation));
            consider(light);
        }
    }
    std::sort(nearest.begin(), nearest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // Which shadow-casting spot lights get their shadow this frame - the game's selection (tabula_rasa.exe, traced 2026-10-17):
    // FUN_00806f00 gives each light the value max(0, |light - camera| - r), r = the radius of the light node's bounds (+ 0xc of
    // the struct TRasa::Light's slot 19 returns); FUN_00808390 sorts ascending (FUN_008082c0; directional lights first), takes the
    // first N = rint(12 x ShadowMaxSimultaneous + 0.5) clamped to 1..12 (constant 12.0 at 0x00c2be80) and drops those whose value
    // exceeds 25 + 50 x ShadowDistance (FUN_008475b0: 25.0 / 50.0 at 0x00c2b6c8 / 0x00c2b6d0; it reads the distance only while two
    // RQS flags are set, + 0x84 / + 0xbf - their meaning is NOT traced); the rest get no shadow (the light pass's
    // ShadowMapTextureEnable stays 0, FUN_00807d20). Verified on the firing-range trace: 12 maps rendered, and when the 11th and
    // 12th appear (frame 43) the two largest values - Light 8 and the west-lane lamp - lose their shadows for good. r = the
    // light's bounds radius (decoded, see consider()). Candidates are shadowPool (gathered in consider()): the lights
    // TRasa::SubmitVisibleLights (FUN_00811410) keeps - bounds sphere inside the frustum planes (FUN_005e6960) and shown by the
    // PVS (FUN_007cfc10).
    std::vector<bool> shadowAllowed(nearest.size(), false);
    {
        // FUN_008475b0: with deferred lighting active (render settings + 0xbf, set by FUN_007bfd10 in render mode 5) the limit is
        // 25 + 50 x ShadowDistance while the RQS option "Shadow Distance" is on, 25 when it is off (otherwise -1).
        const float maxValue = 25.0f + 50.0f * (rqsShadowDistance ? std::clamp(shadowDistance, 0.0f, 1.0f) : 0.0f);
        std::vector<ShadowCandidate> chosen;
        for (const ShadowCandidate& c : shadowPool)
            if (c.value <= maxValue) chosen.push_back(c);
        std::stable_sort(chosen.begin(), chosen.end(), [](const ShadowCandidate& a, const ShadowCandidate& b) { return a.value < b.value; });
        const int limit = std::clamp(int(std::nearbyint(12.0f * std::clamp(shadowMaxSimultaneous, 0.0f, 1.0f) + 0.5f)), 1, 12);
        lastShadowCandidates.clear();
        for (size_t c = 0; c < chosen.size(); ++c)
            lastShadowCandidates.push_back(ShadowCandidateInfo{chosen[c].value, chosen[c].position, chosen[c].range, c < size_t(limit)});
        if (chosen.size() > size_t(limit)) chosen.resize(size_t(limit));
        for (size_t k = 0; k < nearest.size(); ++k)
            for (const ShadowCandidate& c : chosen)
                if (c.position.x == nearest[k].second.position.x && c.position.y == nearest[k].second.position.y &&
                    c.position.z == nearest[k].second.position.z && c.range == nearest[k].second.range)
                    shadowAllowed[k] = nearest[k].second.castsShadows;
    }
    int shadowLightCount = 0;                                       // lights given shadow views (the casters' mask bits)
    // The renderer's kept shadow maps: maps queued in an earlier frame count as drawn once the renderer has recorded a shadow pass
    // since (a fresh renderer - fewer passes than seen - starts empty).
    ShadowCache& cache = shadowCache_[renderer_];
    {
        const uint64_t passes = renderer_->localShadowPassesRecorded();
        if (passes < cache.passesSeen) cache.entries.clear();
        cache.passesSeen = passes;
        for (ShadowCacheEntry& e : cache.entries)
            if (e.pendingVersion != 0 && passes > e.pendingPass) {
                e.drawnVersion = e.pendingVersion;
                e.pendingVersion = 0;
            }
    }
    ++shadowFrame_;
    frameShadowEntries_.clear();
    for (size_t nIndex = 0; nIndex < nearest.size(); ++nIndex) {
        const auto& n = nearest[nIndex];
        if (l.localCount >= SceneLighting::maxLocalLights) break;
        const tr::PlacedLight& light = n.second;
        const int i = l.localCount++;
        l.localPosition[i] = Vec4f{light.position.x, light.position.y, light.position.z, light.range};
        // Fade start distance: the engine sets LightFalloff = (1 - falloff) * LightRange (tabula_rasa.exe FUN_00879150 /
        // FUN_00875360), i.e. FalloffPercent is the outer part of the range that fades (0.25: full to 75%, then fade).
        // That the field is FalloffPercent is inferred from its name and use (the serializer's offsets are unclear).
        const tr::Vec3 c{std::pow(light.colour.x, lightColourGamma), std::pow(light.colour.y, lightColourGamma), std::pow(light.colour.z, lightColourGamma)};
        l.localColour[i] = Vec4f{c.x * light.intensity, c.y * light.intensity, c.z * light.intensity,
                                 (1.0f - std::clamp(light.falloffPercent, 0.0f, 1.0f)) * light.range};
        // w = 1: a deferred-only light (RenderMode 1) - forward pieces skip it (lighting.hlsli localLights).
        // viewport_test RASA_FORWARDALLLIGHTS=1: every light reaches forward pieces (the rule before 2026-10-20).
        static const bool forwardAllLights = [] { const char* v = std::getenv("RASA_FORWARDALLLIGHTS"); return v && *v == '1'; }();
        l.localBackColour[i] = Vec4f{light.backColour.x * light.intensity, light.backColour.y * light.intensity,
                                     light.backColour.z * light.intensity, light.renderMode == 1 && !forwardAllLights ? 1.0f : 0.0f};
        if (light.kind == tr::PlacedLight::Kind::Spot) {
            const tr::Vec3 d = normalized(light.direction());
            const float outer = std::max(light.penumbraAngle, light.coneAngle) * 0.5f * 0.0174533f;   // full angles (UNVERIFIED)
            l.localSpot[i] = Vec4f{d.x, d.y, d.z, std::cos(outer)};
            l.localSpotInner[i] = Vec4f{std::cos(light.coneAngle * 0.5f * 0.0174533f), 0, 0, 0};
        } else if (light.kind == tr::PlacedLight::Kind::Plane) {
            // TRPlaneLight (effects.glm, disassembled 2026-10-12; constants from FUN_0087a070): a box in the light's frame
            // (Position, WorldOrientation) with half-extents LightDimensions = (Width/2, Length/2, Range) and full strength
            // within LightFalloffVec = (Width/2 (1 - f.x), Length/2 (1 - f.y), Range (1 - f.z)), f = FalloffPercentVec.
            const tr::Vec3 dims{light.width * 0.5f, light.length * 0.5f, light.range};
            const tr::Vec3& f = light.falloffPercentVec;
            l.localPosition[i].w = std::sqrt(dims.x * dims.x + dims.y * dims.y + dims.z * dims.z);
            l.localSpot[i] = Vec4f{dims.x, dims.y, dims.z, -3.0f};
            l.localSpotInner[i] = Vec4f{dims.x * (1.0f - f.x), 0, dims.y * (1.0f - f.y), dims.z * (1.0f - f.z)};
        } else {
            l.localSpot[i] = Vec4f{0, -1, 0, -2.0f};       // point
        }
        const auto globe = light.globeMap.empty() ? globeIndex_.end() : globeIndex_.find(tr::toLowerAscii(light.globeMap));
        l.localSpotInner[i].y = globe == globeIndex_.end() ? 0.0f : float(globe->second);
        const tr::Quat& q = light.worldOrientation;
        l.localRotation[i] = Vec4f{q.x, q.y, q.z, q.w};
        if (light.kind == tr::PlacedLight::Kind::Spot && spotCookies) {
            const auto cookie = light.globeMap.empty() ? cookieIndex_.end() : cookieIndex_.find(tr::toLowerAscii(light.globeMap));
            l.localSpotInner[i].w = cookie == cookieIndex_.end() ? 0.0f : float(cookie->second);
        }
        // The light's shadow views (traced 2026-10-17; tabula_rasa.exe FUN_00809bf0 2D, FUN_00809690 cube):
        // - spot: perspective over PenumbraAngle, near 0.5 (0x00bc7768), far = Range, the light's frame (its rotation's x / y / z axes,
        //   z = the axis it shines along) - VERIFIED against the firing-range trace's maps;
        // - plane: orthographic, half extents Width / 2 x Length / 2 (the light's bounds), near 0.05 (0x00bc565c), far = Range, the
        //   light's frame (it shines along its +z);
        // - point: six 90-degree faces (0x00bc7b18), near 0.05, far = Range (rendered only while far > 0.05, 0x00c2b6f8), here along
        //   the world axes (the engine's face frame - g_ShadowView, face 4 - only orients the lookup).
        // Far = Range is the field the spot's trace confirms (+ 0x354); for plane and point lights it is the same field (UNVERIFIED).
        // The views are packed into the renderer's atlas in 128-texel units and kept there between frames (shadowCacheEntry): a
        // static light's map is drawn once and redrawn only when the casters change, a non-static light's every frame. Compared
        // with the game: spot (firing range), point (command tent) and plane (great stairs, Light_29) shadows.
        if (localShadows && shadowAllowed[nIndex]) {
            // Map size (tabula_rasa.exe FUN_00808b70, read 2026-10-17): by ShadowQuality 0 / 1 -> 128, 2 -> 256, 3 -> 512 (cube and plane
            // maps); a perspective (spot) map instead covers its width at the range, 2 R tan(penumbra / 2), at a texel size of 0.254 /
            // 0.1524 / 0.1016 / 0.1016 m (quality 0..3, 0x00c2b6d8..), rounded to the nearest multiple of 128 (at least 128) and capped
            // at 256 / 384 / 512 / 768. Checked: every map of the firing-range trace (flashlights / lamps 128, tent lamps 256, the tent's
            // point cubes 128, Light 8 256).
            const uint32_t quality = light.shadowQuality;
            uint32_t size = quality <= 1 ? 128u : quality == 3 ? 512u : 256u;
            if (light.kind == tr::PlacedLight::Kind::Spot && light.penumbraAngle > 0.0f) {
                const float texel = quality == 0 ? 0.254f : quality == 1 ? 0.1524f : 0.1016f;
                const uint32_t cap = quality == 0 ? 256u : quality == 1 ? 384u : quality == 3 ? 768u : 512u;
                const int wanted = int(std::nearbyint(std::tan(light.penumbraAngle * 0.5f * 0.0174533f) * light.range * 2.0f / texel));
                const uint32_t rounded = uint32_t(std::max(1, (wanted + 64) / 128)) * 128u;
                size = rounded < cap ? rounded : cap;
            }
            const int views = light.kind == tr::PlacedLight::Kind::Point ? 6 : 1;
            int first = -1;
            bool redraw = false;
            if (l.localShadowCount + views <= SceneLighting::maxLocalShadows && shadowLightCount < 32) {
                const int entry = shadowCacheEntry(cache, light, size, views);
                if (entry >= 0) {
                    ShadowCacheEntry& e = cache.entries[size_t(entry)];
                    e.lastUsed = shadowFrame_;
                    first = l.localShadowCount;
                    for (int v = 0; v < views; ++v)
                        l.localShadowRect[first + v] = Vec4f{float(e.cells[size_t(v)].first * 128), float(e.cells[size_t(v)].second * 128),
                                                             float(size), float(shadowLightCount)};
                    // A static light's map is drawn once (the game: when the light is created) and redrawn only when the casters
                    // change; a non-static one every frame.
                    redraw = !cacheShadowMaps || !e.isStatic || e.drawnVersion != shadowCasterVersion_;
                    frameShadowEntries_.push_back(entry);
                }
            }
            if (first >= 0) {
                auto axis = [&](const tr::Vec3& v) {
                    const Mat4 r = rotationMatrix(q);
                    return tr::Vec3{rowDot(r.row0, v.x, v.y, v.z, 0), rowDot(r.row1, v.x, v.y, v.z, 0), rowDot(r.row2, v.x, v.y, v.z, 0)};
                };
                const Vec4f params{0, light.range, std::clamp(light.shadowOpacity, 0.0f, 1.0f), light.shadowSoftness};
                auto setView = [&](int view, const Mat4& projection, const tr::Vec3& right, const tr::Vec3& up, const tr::Vec3& forward, float nearZ,
                                   const Vec4f& bias) {
                    l.localShadowMatrix[view] = multiply(projection, viewLH(light.position, right, up, forward));
                    l.localShadowView[view] = Vec4f{forward.x, forward.y, forward.z, -dot(forward, light.position)};
                    l.localShadowParams[view] = Vec4f{nearZ, params.y, params.z, params.w};
                    l.localShadowBias[view] = bias;
                    const tr::Vec3 f = camera_.forward();
                    l.localShadowCamera[view] = Vec4f{f.x, f.y, f.z, -dot(f, camera_.position)};
                };
                if (light.kind == tr::PlacedLight::Kind::Point) {
                    const tr::Vec3 faces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                    const tr::Vec3 ups[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
                    for (int v = 0; v < 6; ++v) {
                        const tr::Vec3 right = normalized(cross(ups[v], faces[v])), up = cross(faces[v], right);
                        setView(first + v, perspectiveLH(1.5707964f, 1.0f, 0.05f, light.range), right, up, faces[v], 0.05f, Vec4f{0.05f, 0.8f, 5.0f, 0});
                    }
                } else {
                    const tr::Vec3 right = normalized(axis(tr::Vec3{1, 0, 0})), up = normalized(axis(tr::Vec3{0, 1, 0})), forward = normalized(axis(tr::Vec3{0, 0, 1}));
                    if (light.kind == tr::PlacedLight::Kind::Plane)
                        setView(first, orthoLH(std::max(light.width * 0.5f, 0.01f), std::max(light.length * 0.5f, 0.01f), 0.05f, light.range), right, up,
                                forward, 0.05f, Vec4f{0, 0, 0, 1});
                    else
                        setView(first, perspectiveLH(light.penumbraAngle * 0.0174533f, 1.0f, 0.5f, light.range), right, up, forward, 0.5f,
                                Vec4f{0.05f, 0.035f, 3.0f, 0});
                }
                l.localShadowCount += views;
                l.localShadowBase[i] = Vec4f{float(first + 1), float(views), float(shadowLightCount), light.isStatic ? 0.0f : 1.0f};
                if (!redraw) l.localShadowRedraw &= ~(1u << uint32_t(shadowLightCount));
                ++shadowLightCount;
            }
        }
    }
    if (onlyLight >= 0) {                     // diagnostic: keep one placed light (its colour), darken the rest
        for (int i = 0; i < l.localCount; ++i)
            if (i != onlyLight) l.localColour[i] = Vec4f{0, 0, 0, l.localColour[i].w};
    }
    return l;
}

// The globe cube maps the map's lights and the loaded models' lights use: each DDS cube (6 faces of mip chains, DXT)
// decoded face by face to RGBA, resized to 64 x 64, uploaded as one cube map array. Rebuilt whenever a loaded model
// brings a new one (between frames: it releases the previous texture).
void SceneView::loadGlobeMaps() {
    constexpr uint32_t size = 64;
    std::vector<std::vector<tr::Rgba8>> faces;
    globeIndex_.clear();
    globeErrors_.clear();
    std::vector<const tr::PlacedLight*> all;
    for (const tr::PlacedLight& light : mapLights_) all.push_back(&light);
    for (const auto& m : models_)
        for (const tr::PlacedLight& light : m.second.lights) all.push_back(&light);
    std::vector<tr::PlacedLight> stateGlobes;           // special-FX lights only contribute their globe name
    for (const ServerPlacement& s : serverPlacements_)
        for (const StateLight& l : s.stateLights)
            if (!l.event.globeMap.empty()) {
                tr::PlacedLight light;
                light.globeMap = l.event.globeMap;
                stateGlobes.push_back(light);
            }
    for (const tr::PlacedLight& light : stateGlobes) all.push_back(&light);
    cookieIndex_.clear();
    std::vector<std::vector<tr::Rgba8>> cookies;
    for (const tr::PlacedLight* lightPointer : all) {
        const tr::PlacedLight& light = *lightPointer;
        if (light.globeMap.empty()) continue;
        const std::string name = tr::toLowerAscii(light.globeMap);
        if (globeIndex_.count(name) || cookieIndex_.count(name)) continue;
        try {
            const tr::Bytes file = assets_.read(name);
            const tr::DdsHeader header = tr::readDdsHeader(file);
            const std::vector<tr::DdsLevel> levels = tr::ddsCompressedLevels(file, header);
            if (levels.empty()) throw std::runtime_error("not a block-compressed DDS");
            size_t faceBytes = 0;
            for (const tr::DdsLevel& level : levels) faceBytes += level.size;
            if (file.size() < 128 + 6 * faceBytes) {
                // A plain 2D texture (e.g. globemap_human_flashlight.dds, 32 x 32 DXT1): a spot light's projected globe.
                tr::DdsHeader h;
                std::vector<tr::Rgba8> rgba = tr::decodeDds(file, h);
                if (h.width != size || h.height != size) rgba = resizeRgba(rgba, h.width, h.height, size, size);
                cookies.push_back(std::move(rgba));
                cookieIndex_[name] = int(cookies.size());
                continue;
            }
            for (uint32_t face = 0; face < 6; ++face) {
                tr::Bytes one(file.begin(), file.begin() + 128);
                one.insert(one.end(), file.begin() + 128 + face * faceBytes, file.begin() + 128 + (face + 1) * faceBytes);
                tr::DdsHeader h;
                std::vector<tr::Rgba8> rgba = tr::decodeDds(one, h);
                if (h.width != size || h.height != size) rgba = resizeRgba(rgba, h.width, h.height, size, size);
                faces.push_back(std::move(rgba));
            }
            globeIndex_[name] = int(faces.size() / 6);
        } catch (const std::exception& e) {
            globeErrors_ += name + ": " + e.what() + "\n";
            globeIndex_[name] = 0;
        }
    }
    renderer_->setGlobeMaps(faces, size);
    renderer_->setCookieMaps(cookies, size);
}

// A map light with its lights.xml modifiers applied at lightTime (see tr::LightModifier).
tr::PlacedLight SceneView::displayedMapLight(size_t index) const {
    if (index >= mapLights_.size()) return tr::PlacedLight{};
    return animateLights ? modifiedLight(mapLights_[index]) : mapLights_[index];
}

size_t SceneView::mapLightModifierTracks(const tr::PlacedLight& light) const {
    size_t tracks = 0;
    if (light.id == 0) return 0;                                // LightID 0 is skipped (FUN_007df550)
    for (const tr::LightModifier& m : lightModifiers_)
        if (m.lightId == light.id) tracks += m.tracks.size();
    return tracks;
}

tr::PlacedLight SceneView::modifiedLight(const tr::PlacedLight& light) const {
    if (light.id == 0 && light.modifiers.empty()) return light;
    tr::PlacedLight out = light;
    auto applyTracks = [&](const std::vector<tr::LightModifierTrack>& list) {
        for (const tr::LightModifierTrack& track : list) {
            if (!track.enabled) continue;
            const float w = tr::lightModifierWeight(track, lightTime);
            switch (track.kind) {
                case tr::LightModifierTrack::Kind::Intensity:
                    out.intensity = track.intensity1 + w * (track.intensity2 - track.intensity1);
                    break;
                case tr::LightModifierTrack::Kind::Range:
                    // TRasa::RangeModifier (vtable slot 28 FUN_00458ce0, traced 2026-10-20): the light's float property "Range"
                    // (bound by name in FUN_00458c10) = Range1 + w (Range2 - Range1), as IntensityModifier sets Intensity.
                    out.range = track.range1 + w * (track.range2 - track.range1);
                    break;
                case tr::LightModifierTrack::Kind::Color:
                    out.colour = add(track.colour1, scaled(subtract(track.colour2, track.colour1), w));
                    break;
                case tr::LightModifierTrack::Kind::Rotation: {
                    // Point lights: the modifier replaces the orientation, but the game's angle stays 0. apitrace of Luna Cavern
                    // (2026-10-14): Light_08 (LocalOrientation 22.4 degrees about y in lights.xml, rotation track one turn per
                    // 100 s) has an identity Light2World / Proj2LightView in all 3,308 frames while its colour track runs through
                    // ~23 s of modifier time. Why the angle does not advance is NOT understood. Other kinds: UNVERIFIED, as decompiled.
                    if (light.kind == tr::PlacedLight::Kind::Point) {
                        out.worldOrientation = tr::Quat{0, 0, 0, 1};
                        break;
                    }
                    const float angle = std::clamp(w * 6.2831853f, 0.0f, 6.2831853f);
                    const tr::Vec3 a = normalized(track.axis);
                    const float s = std::sin(angle * 0.5f);
                    out.worldOrientation = tr::Quat{a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
                    break;
                }
                case tr::LightModifierTrack::Kind::Position:
                    // Offset: the light's child-node translation (see "Lights on models"); for map lights added to Position.
                    out.position = add(light.position, add(track.offset1, scaled(subtract(track.offset2, track.offset1), w)));
                    break;
                default:
                    break;
            }
        }
    };
    if (light.id != 0)
        for (const tr::LightModifier& mod : lightModifiers_)
            if (mod.lightId == light.id) applyTracks(mod.tracks);
    applyTracks(light.modifiers);       // the template's own tracks (contact-point lights)
    return out;
}

SceneLighting SceneView::skyLightingAt() const {
    SceneLighting l;                                   // plain editor lighting
    const std::vector<SkyWeight> weights = skyWeights();
    if (!skyLighting || weights.empty()) return l;
    const tr::Sky& sky = skies_[weights.back().index].sky;   // fog and colours: the sky being shown (fog switches at once)
    const float t = timeOfDay;
    auto vec4 = [](const tr::Vec3& c) { return Vec4f{c.x, c.y, c.z, 1}; };
    auto luminance = [](const tr::Vec3& c) { return 0.3f * c.x + 0.59f * c.y + 0.11f * c.z; };

    // Hemispheric light: the sum (CombinedHemiLight) of the hemi lights the deferred renderer uses. tabula_rasa.exe
    // FUN_00812dc0 / FUN_00812880 take a light only when RenderMode == 1 in deferred mode (0 in the fallback renderer)
    // or RenderMode == 2; modes > 2 are mapped to a default (FUN_0086a910 -> FUN_00887550, not decoded - counted
    // here). Skies carry one hemi light per renderer (Concordia: Light_01 mode 1, Light_02 mode 0). Checked at
    // Alia Das 12:00: vertical faces away from the sun 0.55 measured vs (top + bottom) / 2 = 0.55 of Light_01 alone.
    tr::Vec3 top{0, 0, 0}, bottom{0, 0, 0};
    for (const SkyWeight& w : weights)                 // a sky change: both skies' lights, each x its fade weight
        for (const tr::SkyLight& g : skies_[w.index].sky.globalLights) {
            if (!g.hemi || g.renderMode == 0) continue;
            top = add(top, scaled(tr::skyLightColour(g, 1, t), w.weight));
            bottom = add(bottom, scaled(tr::skyLightColour(g, 2, t), w.weight));
        }
    l.hemiTop = vec4(scaled(top, ambientScale));
    l.hemiBottom = vec4(scaled(bottom, ambientScale));

    // Directional lights: every enabled layer light applies (RenderMode 99 maps to an active mode, FUN_00887550; verified
    // against in-game /dl_showlight at Alia Das 08:00, where the second layer light lights shadowed ground). The brightest
    // gets the sun shadow map; the others are drawn unshadowed (extra directional lights).
    l.sunFront = Vec4f{0, 0, 0, 1};
    l.sunBack = Vec4f{0, 0, 0, 1};
    struct Directional { tr::Vec3 towards; tr::Vec3 front; tr::Vec3 back; float shadowOpacity; bool castsShadows; };
    std::vector<Directional> directional;
    std::vector<Directional> unlit;        // directional lights black at this time: they still join the forward light's combination
    for (const SkyWeight& w : weights)
    for (const tr::SkyLayer& layer : skies_[w.index].sky.layers) {
        if (!layer.enabled || !showSun) continue;
        for (const tr::SkyLight& light : layer.lights) {
            if (light.hemi || !light.active) continue;
            const tr::Vec3 front = scaled(tr::skyLightColour(light, 3, t), w.weight);
            const tr::Vec3 back = scaled(tr::skyLightColour(light, 4, t), w.weight);
            const tr::Vec3 travel = tr::skyLayerDirection(layer, light.orientation, t);
            const Directional d{normalized(tr::Vec3{-travel.x, -travel.y, -travel.z}), front, back, tr::skyLightShadowOpacity(light, t), light.castsShadows};
            (luminance(front) <= 0.0f && luminance(back) <= 0.0f ? unlit : directional).push_back(d);
        }
    }
    std::stable_sort(directional.begin(), directional.end(), [&](const Directional& a, const Directional& b) { return luminance(a.front) > luminance(b.front); });
    // Global (non-layer) directional lights in the deferred modes: the Bane interior skies carry one (RenderMode 2, DIntensity 1,
    // Type 3, CastsShadows false - sky_bane_interior_default.sky). The game's light pass takes it like a layer light, unshadowed
    // (traced 2026-10-18: its colour gives the floor's blue: (0.11, 0.14, 0.34) x N.L 0.97 + the hemi top (0.17, 0.23, 0.36) =
    // (0.28, 0.37, 0.70) against the game's (0.29, 0.35, 0.65)). Appended after the sort: no shadow map, opacity 0.
    for (const SkyWeight& w : weights)
        for (const tr::SkyLight& g : skies_[w.index].sky.globalLights) {
            if (g.hemi || g.renderMode == 0 || !g.active) continue;
            const tr::Vec3 front = scaled(tr::skyLightColour(g, 3, t), w.weight);
            const tr::Vec3 back = scaled(tr::skyLightColour(g, 4, t), w.weight);
            const Directional d{normalized(tr::Vec3{-g.orientation.x, -g.orientation.y, -g.orientation.z}), front, back, 0.0f, false};
            (luminance(front) <= 0.0f && luminance(back) <= 0.0f ? unlit : directional).push_back(d);
        }
    // The forward light (gDirLight[0] of water, forward-phase pieces and billboards) is one TRasa::CombinedDirLight per frame
    // (LightCombiner::AddLight FUN_00883340, add FUN_00883040, finish FUN_00883120; traced 2026-10-20): it starts at colour 0 and
    // direction (0, -1, 0) (0x00c2b50c = -1); each light adds its Color to the colour, then its direction of travel (the +z axis of its
    // rotation, FUN_004f4490) x the luminance 0.3 |r| + 0.59 |g| + 0.11 |b| (FUN_00675aa0) of the colour summed SO FAR; the
    // direction is normalised at the end. Black lights count too - after a lit one they carry its weight. VERIFIED on
    // tabula_rasa.7.trace (Alia Das, .skytime 12:00, water draw 31402441): the sun (1.669 1.613 1.437, travel (-0.7068 -0.7071
    // -0.0221) as in the shadow pass) followed by _tranz's black second layer light gives (-0.5703 -0.8212 -0.0178) against the
    // game's gDirLight[0].Direction (-0.5698 -0.8216 -0.0180) - 55 degrees up where the sun is at 45. The order the engine adds the
    // lights in is NOT traced: brightest first, then the black ones (the order that fits that frame).
    {
        tr::Vec3 colour{0, 0, 0}, travel{0, -1, 0};
        auto combine = [&](const Directional& d) {
            colour = add(colour, d.front);
            const float weight = 0.3f * std::fabs(colour.x) + 0.59f * std::fabs(colour.y) + 0.11f * std::fabs(colour.z);
            travel = add(travel, scaled(d.towards, -weight));
        };
        std::vector<Directional> lit = directional;
        std::stable_sort(lit.begin(), lit.end(), [&](const Directional& a, const Directional& b) { return luminance(a.front) > luminance(b.front); });
        for (const Directional& d : lit) combine(d);
        for (const Directional& d : unlit) combine(d);
        l.forwardTowards = normalized(tr::Vec3{-travel.x, -travel.y, -travel.z});
        l.forwardColour = vec4(colour);
    }
    for (size_t k = 0; k < directional.size(); ++k) {
        const Directional& d = directional[k];
        if (k == 0) {
            l.towardsLight = d.towards;
            l.sunFront = vec4(d.front);
            l.sunBack = vec4(d.back);
            // The sun's shadow map only for a light that casts shadows (the Bane interior's directional "Light" has CastsShadows false:
            // a roof-covered sun map darkened its floor, where the game lights it).
            l.sunShadows = showShadows && d.castsShadows;
            l.shadowOpacity = d.shadowOpacity;
        } else if (l.extraDirectionalCount < SceneLighting::maxExtraDirectional) {
            const int e = l.extraDirectionalCount++;
            l.extraTowards[e] = d.towards;
            l.extraFront[e] = vec4(d.front);
            l.extraBack[e] = vec4(d.back);
        }
    }

    // Sky colour and fog: the fog frames' colour (sky domes with UseFogColor take it) and distances.
    if (!sky.fog.empty()) {
        const tr::SkyFogKey fog = tr::sampleSkyFog(sky, t);
        l.clearColour = SDL_FColor{fog.colour.x, fog.colour.y, fog.colour.z, 1.0f};
        l.fogColour = vec4(fog.colour);
        l.fog = showFog && fog.endDistance > fog.startDistance;
        l.fogStart = fog.startDistance;
        // The game's composite pass fogs with density: visibility = exp(-(density * max(d - start, 0))^2)
        // (trhdrcomposite). tabula_rasa.exe FUN_00803780: density = 2.354074 / (end - start), i.e. exp(-2.354^2) =
        // 1/256 at the end distance (else 0.0023541). Not reproduced: the engine also clamps the fog end by the view
        // distance option (100..500 m) and keeps start <= end - 50 (FUN_007fd9e0; read from decompiled code).
        l.fogDensity = fog.endDistance > fog.startDistance ? 2.354074f / (fog.endDistance - fog.startDistance) : 0.0023541f;
    }
    return l;
}

SceneView::~SceneView() {
    SDL_GPUDevice* device = renderer_->device();
    for (TerrainChunk& chunk : terrainChunks_)
        for (uint32_t level = 0; level < 4; ++level) renderer_->destroyMesh(chunkLevel(chunk, level));
    renderer_->destroyMesh(unitBox_);
    renderer_->destroyMesh(unitBoxLines_);
    renderer_->destroyMesh(unitCircle_);
    renderer_->destroyMesh(unitSquare_);
    for (auto& c : collisionMeshes_)
        for (GpuMesh* m : {&c.second.used, &c.second.usedEdges, &c.second.dropped, &c.second.droppedEdges, &c.second.ignored}) renderer_->destroyMesh(*m);
    renderer_->destroyMesh(arrow_);
    renderer_->destroyMesh(pole_);
    for (auto& m : models_)
        for (ModelPiece& piece : m.second.pieces) {
            renderer_->destroyMesh(piece.mesh);
            renderer_->destroyBuffer(piece.skinBuffer);
        }
    for (auto& t : textures_)
        if (t.second) SDL_ReleaseGPUTexture(device, t.second);
    for (auto& t : environmentCubes_)
        if (t.second) SDL_ReleaseGPUTexture(device, t.second);
    renderer_->setEnvironmentMap(nullptr);
    for (SDL_GPUTexture* t : {terrainMaterial_.layers, terrainMaterial_.layerIndices, terrainMaterial_.layerWeights})
        if (t) SDL_ReleaseGPUTexture(device, t);
}

size_t SceneView::texturesLoaded() const {
    size_t count = 0;
    for (const auto& t : textures_)
        if (t.second) ++count;
    return count;
}

// A volume DDS (the water's VolumeMapTexture) as a 3D texture; cached with the 2D textures under "volume:<name>".
SDL_GPUTexture* SceneView::volumeTexture(const std::string& name) {
    if (name.empty()) return nullptr;
    const std::string key = "volume:" + tr::toLowerAscii(name);
    const auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;
    SDL_GPUTexture* result = nullptr;
    tr::Bytes data;
    try {
        if (assets_.tryRead(tr::toLowerAscii(name), data)) result = uploadDdsVolumeTexture(renderer_->device(), data);
        else textureErrors_[key] = "not found in the archives";
    } catch (const std::exception& e) {
        result = nullptr;
        textureErrors_[key] = e.what();
    }
    textures_.emplace(key, result);
    return result;
}

SDL_GPUTexture* SceneView::texture(const std::string& name) {
    if (name.empty()) return nullptr;
    const std::string key = tr::toLowerAscii(name);
    const auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;
    SDL_GPUTexture* result = nullptr;
    tr::Bytes data;
    try {
        if (assets_.tryRead(key, data)) result = uploadDdsTexture(renderer_->device(), data);
        else textureErrors_[key] = "not found in the archives";
    } catch (const std::exception& e) {
        result = nullptr;                       // unreadable or unsupported: draw untextured
        textureErrors_[key] = e.what();
    }
    textures_.emplace(key, result);
    return result;
}

void SceneView::buildTerrainMaterial() {
    // Layer textures in one texture array (layer index = position in texlayers.tab). The shipped layer textures are DXT1,
    // 512 x 512 (121 of 148) or 1024 x 1024 (23): the array is 512 x 512 BC1 straight from the files' compressed levels
    // (1024 layers contribute their stored 512 level). The editor used to shrink every layer to 256 x 256, visibly blurring
    // the terrain. Fallback when a layer does not fit (missing, not DXT1, smaller): decoded RGBA at 512.
    const uint32_t size = 512;
    std::vector<tr::Bytes> files;
    for (const tr::TextureLayer& layer : map_.textureLayers) {
        std::string file = layer.texture;
        const size_t slash = file.find_last_of("/\\");
        if (slash != std::string::npos) file = file.substr(slash + 1);
        tr::Bytes data;
        try {
            if (!assets_.tryRead(file, data)) data.clear();
        } catch (const std::exception&) {
            data.clear();
        }
        files.push_back(std::move(data));
        terrainMaterial_.tilings.push_back(layer.tiling);
    }
    terrainMaterial_.layers = uploadDxt1Array(renderer_->device(), files, size);
    if (!terrainMaterial_.layers) {
        std::vector<std::vector<tr::Rgba8>> layers;
        for (const tr::Bytes& data : files) {
            std::vector<tr::Rgba8> pixels(size_t(size) * size, tr::Rgba8{128, 128, 128, 255});   // missing: neutral grey
            try {
                if (!data.empty()) {
                    tr::DdsHeader header;
                    const std::vector<tr::Rgba8> decoded = tr::decodeDds(data, header);
                    pixels = resizeRgba(decoded, header.width, header.height, size, size);
                }
            } catch (const std::exception&) {
            }
            layers.push_back(std::move(pixels));
        }
        terrainMaterial_.layers = uploadRgbaArray(renderer_->device(), layers, size);
    }

    // Per-sample layer indices and coverages, one texel per sample (row = z index, column = x index). Coverage as the
    // client's alpha masks (tr::slotCoverage).
    const tr::Terrain& t = map_.terrain;
    const size_t layerCount = map_.textureLayers.size();
    std::vector<tr::Rgba8> indices(size_t(t.columns) * t.rows), weights(size_t(t.columns) * t.rows);
    for (size_t k = 0; k < t.samples.size(); ++k) {
        const tr::TerrainSample& s = t.samples[k];
        // Texture array slice = the texlayers.tab entry (layer - 1); slots without one get slice 0 and no coverage.
        const auto slice = [&](int slot) -> uint8_t {
            const int entry = tr::textureLayerOf(tr::sampleLayer(s, slot));
            return entry >= 0 && size_t(entry) < layerCount ? uint8_t(entry) : 0;
        };
        const auto coverage = [&](int slot) -> uint8_t {
            const int entry = tr::textureLayerOf(tr::sampleLayer(s, slot));
            return entry >= 0 && size_t(entry) < layerCount ? tr::slotCoverage(s, slot) : 0;
        };
        indices[k] = tr::Rgba8{slice(0), slice(1), slice(2), slice(3)};
        weights[k] = tr::Rgba8{coverage(0), coverage(1), coverage(2), coverage(3)};
    }
    terrainMaterial_.layerIndices = uploadRgbaTexture(renderer_->device(), indices.data(), t.columns, t.rows);
    terrainMaterial_.layerWeights = uploadRgbaTexture(renderer_->device(), weights.data(), t.columns, t.rows);
    terrainMaterial_.originX = t.worldX(0);
    terrainMaterial_.originZ = t.worldZ(0);
    terrainMaterial_.inverseSpacing = 1.0f / t.header.spacing;
    terrainMaterial_.columns = t.columns;
    terrainMaterial_.rows = t.rows;
}

GpuMesh& SceneView::chunkLevel(TerrainChunk& chunk, uint32_t level) {
    switch (level) {
        case 0: return chunk.level0;
        case 1: return chunk.level1;
        case 2: return chunk.level2;
        default: return chunk.level3;
    }
}

void SceneView::buildTerrainChunks() {
    const tr::Terrain& t = map_.terrain;
    const uint32_t lastRow = t.boundedRows() - 1, lastColumn = t.boundedColumns() - 1;
    // Height range for the untextured view's colour ramp.
    float lowest = 1e30f, highest = -1e30f;
    for (uint32_t r = 0; r <= lastRow; r += 4)
        for (uint32_t c = 0; c <= lastColumn; c += 4)
            if (t.hasData(r, c)) {
                const float h = t.worldHeightAt(r, c);
                lowest = std::min(lowest, h);
                highest = std::max(highest, h);
            }
    terrainLowest_ = lowest < highest ? lowest : 0.0f;
    terrainRange_ = highest > lowest ? highest - lowest : 1.0f;
    constexpr uint32_t chunkSamples = 64;
    for (uint32_t r0 = 0; r0 < lastRow; r0 += chunkSamples) {
        for (uint32_t c0 = 0; c0 < lastColumn; c0 += chunkSamples) {
            TerrainChunk chunk;
            chunk.row0 = r0;
            chunk.row1 = std::min(r0 + chunkSamples, lastRow);
            chunk.col0 = c0;
            chunk.col1 = std::min(c0 + chunkSamples, lastColumn);
            float low = 1e30f, high = -1e30f;
            bool any = false;
            for (uint32_t r = chunk.row0; r <= chunk.row1; ++r)
                for (uint32_t c = chunk.col0; c <= chunk.col1; ++c)
                    if (t.hasData(r, c)) {
                        const float h = t.worldHeightAt(r, c);
                        low = std::min(low, h);
                        high = std::max(high, h);
                        any = true;
                    }
            if (!any) continue;
            const tr::Vec3 lo{t.worldX(chunk.col0), low, t.worldZ(chunk.row0)}, hi{t.worldX(chunk.col1), high, t.worldZ(chunk.row1)};
            chunk.centre = scaled(add(lo, hi), 0.5f);
            chunk.radius = length(subtract(hi, lo)) * 0.5f;
            terrainChunks_.push_back(chunk);
        }
    }
    // The coarsest level of every chunk up front (or the fixed level), so the whole map draws from the first frame.
    const uint32_t loadLevel = terrainFixedLevel_ >= 0 ? uint32_t(terrainFixedLevel_) : 3u;
    for (TerrainChunk& chunk : terrainChunks_) chunkLevel(chunk, loadLevel) = buildTerrainChunkMesh(chunk, loadLevel);
}

GpuMesh SceneView::buildTerrainChunkMesh(const TerrainChunk& chunk, uint32_t level) const {
    const tr::Terrain& t = map_.terrain;
    const uint32_t step = 1u << level;
    const uint32_t lastRow = t.boundedRows() - 1, lastColumn = t.boundedColumns() - 1;
    // Sample lines of this level, the chunk's far edges always included (they are shared with the neighbours).
    std::vector<uint32_t> rowLines, columnLines;
    for (uint32_t r = chunk.row0; r < chunk.row1; r += step) rowLines.push_back(r);
    rowLines.push_back(chunk.row1);
    for (uint32_t c = chunk.col0; c < chunk.col1; c += step) columnLines.push_back(c);
    columnLines.push_back(chunk.col1);
    // Terrain holes (verified in game 2026-10-14: Daghda's Urn Village -796 761, Pinhole Falls 321 470, Ranja Cavern's mouth): a
    // sample whose flag 0 is clear is cut out - the base pass texkills where the base mask (255 where flag 0 is set) is under 0.5
    // (TRTerrainClient SetZBufferAndFog). Its height is the stored 12-bit value like any other sample's (the .tpc reader keeps
    // height and flags apart, FUN_0094d6b0), i.e. boundsMax.y above worldHeight's, which counts flag 0 as height bit 12; so the
    // half-cells at a hole's edge keep their real slope. The vertex alpha carries flag 0; terrain.hlsl discards below 0.5.
    auto holeAt = [&](int64_t r, int64_t c) {
        return r >= 0 && c >= 0 && r <= int64_t(lastRow) && c <= int64_t(lastColumn) && t.hasData(uint32_t(r), uint32_t(c)) &&
               !tr::terrainFlag(t.sampleAt(uint32_t(r), uint32_t(c)).word, 0);
    };
    auto heightAt = [&](int64_t r, int64_t c, float fallback) {
        if (r < 0 || c < 0 || r > int64_t(lastRow) || c > int64_t(lastColumn)) return fallback;
        if (!t.hasData(uint32_t(r), uint32_t(c))) return fallback;
        return t.worldHeightAt(uint32_t(r), uint32_t(c)) + (holeAt(r, c) ? t.header.bounds.max.y : 0.0f);
    };
    const float spacing = t.header.spacing * float(step);
    const size_t rows = rowLines.size(), columns = columnLines.size();
    std::vector<MeshVertex> vertices;
    vertices.reserve(rows * columns + 2 * (rows + columns));
    for (uint32_t r : rowLines) {
        for (uint32_t c : columnLines) {
            const float h = heightAt(r, c, 0.0f);
            MeshVertex v;
            v.positionX = t.worldX(c);
            v.positionY = h;
            v.positionZ = t.worldZ(r);
            // Normal of the height field y = h(x, z): (-dh/dx, 1, -dh/dz), at this level's spacing.
            const float dx = (heightAt(r, int64_t(c) + step, h) - heightAt(r, int64_t(c) - step, h)) / (2.0f * spacing);
            const float dz = (heightAt(int64_t(r) + step, c, h) - heightAt(int64_t(r) - step, c, h)) / (2.0f * spacing);
            const tr::Vec3 n = normalized(tr::Vec3{-dx, 1.0f, -dz});
            v.normalX = n.x;
            v.normalY = n.y;
            v.normalZ = n.z;
            v.colour = (heightColour((h - terrainLowest_) / terrainRange_) & 0x00FFFFFFu) | (holeAt(r, c) ? 0u : 0xFF000000u);
            vertices.push_back(v);
        }
    }
    std::vector<uint32_t> indices;
    indices.reserve((rows - 1) * (columns - 1) * 6 + 24 * (rows + columns));
    for (size_t i = 0; i + 1 < rows; ++i) {
        for (size_t j = 0; j + 1 < columns; ++j) {
            if (!t.hasData(rowLines[i], columnLines[j]) || !t.hasData(rowLines[i + 1], columnLines[j]) || !t.hasData(rowLines[i], columnLines[j + 1]) ||
                !t.hasData(rowLines[i + 1], columnLines[j + 1]))
                continue;
            const uint32_t a = uint32_t(i * columns + j), b = a + 1, c = a + uint32_t(columns), d = c + 1;
            indices.insert(indices.end(), {a, c, d, a, d, b});
        }
    }
    // Skirts: each edge copied a little below and joined to it, so a coarser neighbour leaves no gap (the terrain
    // pipeline draws both sides). Deep enough for this level's largest interpolation error near cliffs.
    const float skirtDepth = 2.0f + 2.0f * float(step);
    auto skirt = [&](const std::vector<uint32_t>& edge) {
        for (size_t k = 0; k + 1 < edge.size(); ++k) {
            const MeshVertex& p = vertices[edge[k]];
            const MeshVertex& q = vertices[edge[k + 1]];
            const uint32_t base = uint32_t(vertices.size());
            MeshVertex pd = p, qd = q;
            pd.positionY -= skirtDepth;
            qd.positionY -= skirtDepth;
            vertices.push_back(pd);
            vertices.push_back(qd);
            indices.insert(indices.end(), {edge[k], edge[k + 1], base + 1, edge[k], base + 1, base});
        }
    };
    std::vector<uint32_t> top, bottom, left, right;
    for (size_t j = 0; j < columns; ++j) {
        top.push_back(uint32_t(j));
        bottom.push_back(uint32_t((rows - 1) * columns + j));
    }
    for (size_t i = 0; i < rows; ++i) {
        left.push_back(uint32_t(i * columns));
        right.push_back(uint32_t(i * columns + columns - 1));
    }
    if (!indices.empty()) {
        skirt(top);
        skirt(bottom);
        skirt(left);
        skirt(right);
    }
    return renderer_->createMesh(vertices, indices, false);
}

void SceneView::queueTerrain(const std::function<bool(const tr::Vec3&, float)>& visible) {
    // Level by distance from the camera to the chunk's bounding sphere; finer levels built a few per frame (nearest first),
    // and released once the camera is well beyond their range. Until a level exists the next coarser one is drawn.
    auto wantedLevel = [&](const TerrainChunk& chunk) -> uint32_t {
        if (terrainFixedLevel_ >= 0) return uint32_t(terrainFixedLevel_);
        const float d = std::max(0.0f, length(subtract(chunk.centre, camera_.position)) - chunk.radius);
        return d < terrainDetailDistance ? 0u : d < terrainDetailDistance * 2.5f ? 1u : d < terrainDetailDistance * 6.0f ? 2u : 3u;
    };
    auto releaseDistance = [&](uint32_t level) { return level == 0 ? terrainDetailDistance * 1.5f : level == 1 ? terrainDetailDistance * 3.5f : terrainDetailDistance * 8.0f; };
    std::vector<std::pair<float, size_t>> wanted;
    for (size_t k = 0; k < terrainChunks_.size(); ++k) {
        TerrainChunk& chunk = terrainChunks_[k];
        const float d = std::max(0.0f, length(subtract(chunk.centre, camera_.position)) - chunk.radius);
        if (terrainFixedLevel_ < 0)
            for (uint32_t level = 0; level < 3; ++level)
                if (chunkLevel(chunk, level).vertices && d > releaseDistance(level)) renderer_->destroyMesh(chunkLevel(chunk, level));
        const uint32_t want = wantedLevel(chunk);
        if (!chunkLevel(chunk, want).vertices) wanted.push_back({d, k});
    }
    std::sort(wanted.begin(), wanted.end());
    for (size_t n = 0; n < wanted.size() && n < terrainBuildsPerFrame; ++n) {
        TerrainChunk& chunk = terrainChunks_[wanted[n].second];
        const uint32_t want = wantedLevel(chunk);
        chunkLevel(chunk, want) = buildTerrainChunkMesh(chunk, want);
    }
    terrainFullDetail_ = 0;
    const float maxDistance = drawDistance + 64.0f;
    for (TerrainChunk& chunk : terrainChunks_) {
        if (length(subtract(chunk.centre, camera_.position)) - chunk.radius > maxDistance || !visible(chunk.centre, chunk.radius)) continue;
        const GpuMesh* mesh = nullptr;
        for (uint32_t level = wantedLevel(chunk); level < 4 && !mesh; ++level)
            if (chunkLevel(chunk, level).vertices) mesh = &chunkLevel(chunk, level);
        if (!mesh) continue;
        if (mesh == &chunk.level0) ++terrainFullDetail_;
        if (showTextures && terrainMaterial_.layers) renderer_->drawTerrain(*mesh, terrainMaterial_);
        else renderer_->draw(*mesh, identityMatrix(), Vec4f{1, 1, 1, 1}, true);     // height-tinted vertex colours
    }
}

// ------------------------------------------------------------------------------------------
// Models
// ------------------------------------------------------------------------------------------
namespace {

// Material colour parameter "r g b a" (missing components 0, alpha 1).
Vec4f parseColour(const std::string& text) {
    float c[4] = {0, 0, 0, 1};
    std::sscanf(text.c_str(), "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]);
    return Vec4f{c[0], c[1], c[2], c[3]};
}

}  // namespace

void SceneView::loadModel(const std::string& name, Model& model) {
    try {
        // name is "<mesh>", "<mesh>|<animation>" (posed: skinned pieces in the animation's first frame) or
        // "<mesh>|<animation>|<skeleton mesh>" (avatar parts: posed on the base body's skeleton). The animation may be marked:
        // "@" a map object's own animation (played), "%" a creature's idle pose - skinned on the GPU with stored vertices so the
        // creature's idle sequence can pose it per frame (ServerPlacement::idles); built and, without a per-frame palette,
        // drawn in the animation's first frame.
        const size_t bar = name.find('|');
        const size_t bar2 = bar == std::string::npos ? std::string::npos : name.find('|', bar + 1);
        const tr::GeoModel geo = tr::readGeo(assets_.read(name.substr(0, bar)));
        std::vector<Mat4> skin;                    // per skeleton bone; empty = draw as authored
        std::vector<Mat4> posedBones;              // per skeleton bone: posed world (contact points of posed models)
        bool objectAnimation = false;
        // Bone-less models load their object animation too: its material (EFCT) tracks still apply.
        if (bar != std::string::npos && (!geo.bones.empty() || name.compare(bar + 1, 1, "@") == 0)) {
            try {
                std::string animationName = name.substr(bar + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar - 1);
                const bool marked = !animationName.empty() && animationName[0] == '@';     // a map object's own animation
                const bool avatarPose = !animationName.empty() && animationName[0] == '%';  // a creature's idle pose
                if (marked || avatarPose) animationName = animationName.substr(1);
                tr::GeoModel skeletonGeo;
                if (bar2 != std::string::npos) skeletonGeo = tr::readGeo(assets_.read(name.substr(bar2 + 1)));
                const tr::Animation animation = tr::readAnimation(assets_.read(animationName));
                skin = skinMatrices(geo, animation, 0.0f, bar2 != std::string::npos ? &skeletonGeo : nullptr, &posedBones);
                if (bar2 == std::string::npos && animation.duration > 0.0f) {
                    // Map objects' own animations: blended skinned pieces are played too; creatures' idles stay posed.
                    objectAnimation = marked;
                    model.animation = animation;
                    model.skeleton.bones = geo.bones;
                    model.boneTracks = boneTrackIndices(model.skeleton, model.animation);
                    model.restInverse.clear();
                    for (const Mat4& m : skin) model.restInverse.push_back(affineInverse(m));
                }
                if (avatarPose && !skin.empty()) {
                    objectAnimation = true;                 // GPU skin buffers; no rigid-piece path (restInverse stays empty)
                    // The block above filled restInverse for a model without a skeleton geo (ambient figures): then a piece skinned
                    // wholly to one bone took the rigid path, played by animatedPieceMatrices with the FIRST idle on the light clock,
                    // while the blended pieces follow the idle sequence (poseIdle) - the firing-range target's sheet (rigid on one
                    // bone) stood ~25 cm from its own red ring (part of the blended stand piece), which then cast a ring shadow on it.
                    model.restInverse.clear();
                    model.avatarPose = true;
                    model.skeleton.bones = geo.bones;
                    if (bar2 != std::string::npos) model.poseSkeleton.bones = skeletonGeo.bones;
                }
            } catch (const std::exception& e) {
                SDL_Log("pose %s: %s; drawing the bind pose", name.c_str(), e.what());
            }
        }
        // Skinned pieces without an animation are still skinned, in the bind pose: their vertices are not stored in
        // place (prop_hum_sign_holo_v11's screen: stored y 1.06..2.01, skinned y 1.60..3.01 = the frame's screen; the
        // inverse binds carry the sign's x1.5 scale).
        const bool anySkinned = std::any_of(geo.pieces.begin(), geo.pieces.end(), [](const tr::GeoPiece& p) { return p.skinned; });
        if (skin.empty() && anySkinned && !geo.bones.empty()) skin = skinMatrices(geo, tr::Animation{}, 0.0f);
        model.restSkin = skin;
        // Static pieces hang on their bone's WORLD transform: the engine resolves the bone by name and draws the piece with the
        // bone node's matrix (FUN_00632fb0 / FUN_0067c320; the skeleton update composes parent x local, FUN_005a10b0) - the
        // posed frame-0 world for posed models, else the bind chain. (Before 2026-10-14 the editor used the bone's local
        // transform, which put e.g. the ambient figures' pistols, on hand bones, near the feet.)
        std::vector<Mat4> staticBoneWorld = posedBones;
        if (staticBoneWorld.size() != geo.bones.size() && !geo.bones.empty()) {
            staticBoneWorld.clear();
            skinMatrices(geo, tr::Animation{}, 0.0f, nullptr, &staticBoneWorld);
        }
        size_t effectPieces = 0;
        // Only full detail is drawn: lower LODs (USDA "LOD" >= 1) and swap set 99 (a low-LOD stand-in) sit on top
        // of the LOD 0 geometry. A model made only of such pieces is drawn whole.
        auto lowDetail = [](const tr::GeoPiece& piece) { return piece.lod > 0 || piece.swapId == 99; };
        const bool skipLowDetail = std::any_of(geo.pieces.begin(), geo.pieces.end(), [&](const tr::GeoPiece& piece) { return !lowDetail(piece); });
        for (const tr::GeoPiece& piece : geo.pieces) {
            if (skipLowDetail && lowDetail(piece)) continue;
            // Additive pieces (TRAdditive* effects: glows, holograms, light cones, mist planes) are drawn additively when
            // Effects are shown; they are not pickable and do not count towards the bounds. A model made only of them is
            // an effect (also shown as a cyan box with Placeholders on).
            const std::string effect = tr::toLowerAscii(piece.effectName);
            const bool namedAdditive = effect.find("additive") != std::string::npos;
            // The material's "Phase" parameter picks the render phase, whose own effect sets the blending (effects.glm
            // PhaseAdditive.fx: SRCALPHA/ONE, PhaseTranslucent.fx: SRCALPHA/INVSRCALPHA, both without depth writes;
            // PhaseOpaque.fx: no blending). 1,081 pieces set it (Additive 877, Translucent 158, Opaque 46), e.g.
            // vfx_waterfall_wake.geo's TRDiffMapDetMapVertColor foam is Additive - drawn solid it was a black sheet.
            // TRAdditive* effects are additive whatever the phase: their passes set ALPHABLENDENABLE 1, SRCBLEND SRCALPHA,
            // DESTBLEND ONE, ZWRITEENABLE 0 themselves (effects.glm, 2026-10-11), and pass states are applied after the
            // phase effect's.
            std::string phase;
            for (const tr::GeoParameter& gp : piece.parameters)
                if (gp.key == "Phase") phase = tr::toLowerAscii(gp.value);
            const bool aniso = effect.rfind("tradditiveanisospec", 0) == 0;
            // The other TRAdditive* effects (TRAdditiveDiffMapVertColor, TRAdditiveDiffMapDetMapDecMapVertColor[Hue]) share the
            // aniso vertex shader layout and fog; drawn with AnisoMaterial::plain.
            const bool plainAdditive = !aniso && effect.rfind("tradditive", 0) == 0;
            const bool additive = namedAdditive || phase == "additive";
            const bool translucent = !additive && phase == "translucent";
            if (additive) ++effectPieces;
            // Not drawn (counted as effect pieces): helper shapes - effects that sample a diffuse map but have none
            // (e.g. vfx_eloh_lilly_pollen.geo, a 207 m particle-emitter sheet whose contact point names its .pfx) - and
            // camera-facing billboard sprites (TRSphBillboard), which would show as fixed quads.
            // Refraction shells (TRRefract, e.g. around npc_eloh_hologram_large) are drawn by their own pass (refract.hlsl).
            const bool helperShape = piece.diffuseTexture.empty() && effect.find("diffmap") != std::string::npos;
            const bool refraction = effect.rfind("trrefract", 0) == 0;
            if (effect.rfind("trsphbillboard", 0) == 0 && !piece.diffuseTexture.empty() && !piece.vertices.empty()) {
                // TRSphBillboard (trsphbillboardsp_2.fxc, technique TRSphBillboard_Default, disassembled 2026-10-14): camera-facing
                // sprites, expanded per frame (queueBillboards).
                Model::Billboard bb;
                bb.vertices = piece.vertices;
                bb.indices = piece.indices;
                bb.texture = texture(piece.diffuseTexture);
                if (!piece.boneName.empty())
                    for (size_t b = 0; b < geo.bones.size(); ++b)
                        if (geo.bones[b].name == piece.boneName) {
                            if (b < staticBoneWorld.size()) bb.boneWorld = staticBoneWorld[b];
                            if (!model.restInverse.empty()) {
                                bb.animBone = int32_t(b);
                                model.animated = true;
                            }
                        }
                for (const tr::GeoParameter& gp : piece.parameters) {
                    // Material colours of semantic Diffuse / Ambient / Emissive are set x gModulateScale 0.5 (FUN_005bd040).
                    auto halved = [&](const std::string& value) {
                        const Vec4f c = parseColour(value);
                        return Vec4f{c.x * 0.5f, c.y * 0.5f, c.z * 0.5f, c.w};
                    };
                    if (gp.key == "MatDiffuse") bb.diffuse = halved(gp.value);
                    if (gp.key == "MatAmbient") bb.ambient = halved(gp.value);
                    if (gp.key == "MatEmissive") bb.emissive = halved(gp.value);
                }
                model.billboards.push_back(std::move(bb));
                ++effectPieces;
                continue;
            }
            if ((!namedAdditive && helperShape) || effect.find("billboard") != std::string::npos) {
                ++effectPieces;
                continue;
            }
            const bool water = effect.find("water") != std::string::npos;
            // "VertColor" effects: albedo = texture x vertex colour x 2 (the game's G-buffer pass, trdiffmapvertcolor).
            // The vertex colours are authored greyscale occlusion (they do not encode the map's lights).
            const bool vertexColour = effect.find("vertcolor") != std::string::npos;
            // TRAdditiveAnisoSpec* (holograms): the game's own formula, see AnisoMaterial. Additive and translucent pieces
            // keep their vertex alpha: it is the opacity (the aniso shader multiplies by it; glow fins fade out by it).
            // Glow map: added after lighting (see shaders/scene.hlsl), sampled with UV set GlowTexCoord (0 or 1).
            std::string glowName;
            int glowUvSet = 1;
            for (const tr::GeoParameter& gp : piece.parameters) {
                if (gp.key == "GlowTexture") glowName = gp.value;
                if (gp.key == "GlowTexCoord") glowUvSet = std::atoi(gp.value.c_str());
            }
            // Pal* materials (ps2x_BaseDeferred*, disassembled 2026-10-11): normal, gloss and glow maps all use UV set 0.
            const bool pal = effect.rfind("paldiff", 0) == 0;
            // Pal* G-buffer passes (every PalDiff* material except PalDiffMap, disassembled 2026-10-11) compute
            // 2 x (diffuse + (Hue - 0.5) x diffuse.a) x vertex colour, like the VertColor effects; their vertex colours are
            // 128 on avatars (x 1). Checked against in-game /dl_showdiffuse at McAllister. Their forward (translucent /
            // additive phase) shaders are not decoded and keep x 1 without vertex colour.
            // TRHair (ps2x_BaseDeferredAlphaTestNoParallaxHue, disassembled 2026-10-11) is the Pal hued normal/gloss/env shader
            // with a cut-out on the normal map's alpha.
            const bool hair = effect.rfind("trhair", 0) == 0 && !additive && !translucent;
            const bool palDeferred = (pal && effect.rfind("paldiffmap", 0) != 0 && !additive && !translucent) || hair;
            if (pal || hair) glowUvSet = 0;
            if (refraction) glowUvSet = 1;                 // TRRefract: the normal map is on UV set 1 (vertex shader: v4)
            // TR* deferred materials (stage 3): which maps fill slots 1 and 2, and their UV sets (vertex shader array element =
            // the *TexCoord parameters, e.g. vsDiffDetDecEnvDeferredArray[DetailTexCoord x 3 + DecalTexCoord]).
            const TrDescription trDesc = (!additive && !translucent && !water) ? trDescription(effect) : TrDescription{};

            auto parameterInt = [&](const char* key, int fallback) {
                for (const tr::GeoParameter& gp : piece.parameters)
                    if (gp.key == key) return std::atoi(gp.value.c_str());
                return fallback;
            };
            // TRAdditiveAnisoSpec*: vs2x_AnisoDetDec[DetailTexCoord x 3 + DecalTexCoord] (element layout, 2026-10-13).
            // vsAdditive_Array (plain TRAdditive*): the same element layout (disassembled 2026-10-14).
            const int uvSet1 = aniso || plainAdditive ? parameterInt("DetailTexCoord", 0) : trDesc.slot1Uv ? parameterInt(trDesc.slot1Uv, 0) : 0;
            const bool palDecal = palDeferred && !hair && effect.find("dec") != std::string::npos;    // Pal*Dec* effects
            const int uvSet2 = aniso || plainAdditive || palDecal ? parameterInt("DecalTexCoord", 0) : trDesc.slot2Uv ? parameterInt(trDesc.slot2Uv, 0) : 0;
            auto uvOfSet = [](const tr::GeoVertex& gv, int set) { return set == 2 ? gv.uv2 : set == 1 ? gv.uv1 : gv.uv; };
            // Static pieces attached to a bone are moved by it (as in the OBJ exporter).
            const tr::GeoBone* bone = nullptr;
            int staticBone = -1;                   // the static piece's bone index
            if (!piece.skinned && !piece.boneName.empty())
                for (size_t b = 0; b < geo.bones.size(); ++b)
                    if (geo.bones[b].name == piece.boneName) {
                        bone = &geo.bones[b];
                        staticBone = int(b);
                    }
            Mat4 boneMatrix = identityMatrix();
            if (bone && size_t(staticBone) < staticBoneWorld.size()) {
                boneMatrix = staticBoneWorld[size_t(staticBone)];
            } else if (bone) {
                boneMatrix = rotationMatrix(bone->rotation);
                const tr::Vec3 bs = bone->scale;
                boneMatrix.row0 = {boneMatrix.row0.x * bs.x, boneMatrix.row0.y * bs.y, boneMatrix.row0.z * bs.z, bone->translation.x};
                boneMatrix.row1 = {boneMatrix.row1.x * bs.x, boneMatrix.row1.y * bs.y, boneMatrix.row1.z * bs.z, bone->translation.y};
                boneMatrix.row2 = {boneMatrix.row2.x * bs.x, boneMatrix.row2.y * bs.y, boneMatrix.row2.z * bs.z, bone->translation.z};
            }
            // Skinned pieces: palette slot -> skeleton bone (by name); -1 = not in the skeleton (left in place).
            std::vector<int> palette;
            if (piece.skinned && !skin.empty())
                for (const std::string& boneName : piece.skinBones) {
                    int found = -1;
                    for (size_t k = 0; k < geo.bones.size() && found < 0; ++k)
                        if (geo.bones[k].name == boneName) found = int(k);
                    palette.push_back(found);
                }
            // Animated models: a piece whose every vertex follows one bone (weight >= 0.999) can be moved per frame.
            int rigidBone = -1;
            if (!palette.empty() && !model.restInverse.empty()) {
                bool rigid = true;
                for (const tr::GeoVertex& gv : piece.vertices) {
                    int vertexBone = -1;
                    for (uint32_t i = 0; i < 4; ++i)
                        if (tr::blendWeight(gv, i) >= 0.999f && tr::blendIndex(gv, i) < palette.size()) vertexBone = palette[tr::blendIndex(gv, i)];
                    if (vertexBone < 0 || (rigidBone >= 0 && vertexBone != rigidBone)) { rigid = false; break; }
                    rigidBone = vertexBone;
                }
                if (!rigid) rigidBone = -1;
            }
            if (staticBone >= 0 && !model.restInverse.empty()) rigidBone = staticBone;     // follows its bone in object animations
            std::vector<MeshVertex> vertices;
            vertices.reserve(piece.vertices.size());
            for (const tr::GeoVertex& gv : piece.vertices) {
                tr::Vec3 p = transformPoint(boneMatrix, gv.position);
                const tr::Vec3 origin = transformPoint(boneMatrix, tr::Vec3{0, 0, 0});
                tr::Vec3 n = subtract(transformPoint(boneMatrix, gv.normal), origin);
                tr::Vec3 tangent = subtract(transformPoint(boneMatrix, gv.tangent), origin);
                tr::Vec3 binormal = subtract(transformPoint(boneMatrix, gv.binormal), origin);
                if (!palette.empty()) {
                    // Linear blend skinning: sum of weight * (posed world * inverse bind world) over up to 4 bones.
                    tr::Vec3 sp{0, 0, 0}, sn{0, 0, 0}, st{0, 0, 0}, sb{0, 0, 0};
                    float total = 0;
                    for (uint32_t i = 0; i < 4; ++i) {
                        const float w = tr::blendWeight(gv, i);
                        const uint32_t slot = tr::blendIndex(gv, i);
                        if (w <= 0 || slot >= palette.size() || palette[slot] < 0) continue;
                        const Mat4& m = skin[size_t(palette[slot])];
                        sp = add(sp, scaled(transformPoint(m, gv.position), w));
                        const tr::Vec3 o = transformPoint(m, tr::Vec3{0, 0, 0});
                        sn = add(sn, scaled(subtract(transformPoint(m, gv.normal), o), w));
                        st = add(st, scaled(subtract(transformPoint(m, gv.tangent), o), w));
                        sb = add(sb, scaled(subtract(transformPoint(m, gv.binormal), o), w));
                        total += w;
                    }
                    if (total > 0) {
                        p = scaled(sp, 1.0f / total);
                        // TRAdditiveAnisoSpec*: the skinned vertex shader (vs2x_AnisoDetDec_Skinned, disassembled 2026-10-13)
                        // skins only the position; N.V uses the mesh-space normal x gWorldMatrix x gInvWorldScale.
                        n = aniso ? gv.normal : sn;
                        tangent = st;
                        binormal = sb;
                    }
                }
                MeshVertex v;
                v.positionX = p.x; v.positionY = p.y; v.positionZ = p.z;
                v.normalX = n.x; v.normalY = n.y; v.normalZ = n.z;
                v.tangentX = tangent.x; v.tangentY = tangent.y; v.tangentZ = tangent.z;
                v.binormalX = binormal.x; v.binormalY = binormal.y; v.binormalZ = binormal.z;
                v.colour = refraction ? packColour(255, 255, 255, uint8_t(gv.colour >> 24))    // TRRefract uses the vertex alpha only
                         : vertexColour || palDeferred ? packColour(uint8_t(gv.colour >> 16), uint8_t(gv.colour >> 8), uint8_t(gv.colour),     // ARGB
                                                     additive || translucent || trDesc.kind1 == 1 || effect.rfind("tremissive", 0) == 0 ? uint8_t(gv.colour >> 24) : 255)
                                        : packColour(255, 255, 255, 255);
                v.texU = gv.uv.x;
                v.texV = gv.uv.y;
                v.texU1 = glowUvSet == 0 ? gv.uv.x : gv.uv1.x;
                v.texV1 = glowUvSet == 0 ? gv.uv.y : gv.uv1.y;
                if (trDesc.slot1 || aniso || plainAdditive) {
                    v.texU1 = uvOfSet(gv, uvSet1).x;
                    v.texV1 = uvOfSet(gv, uvSet1).y;
                }
                v.texU2 = uvOfSet(gv, uvSet2).x;
                v.texV2 = uvOfSet(gv, uvSet2).y;
                if (effect.rfind("trdiffmapvertcolorwave", 0) == 0) {     // vsWave*: TEXCOORD1 = the sway weights
                    v.texU1 = gv.uv1.x;
                    v.texV1 = gv.uv1.y;
                }
                if (water) {
                    // TRWaterReflectRefract: normal maps on NormalMap0TexCoord / NormalMap1TexCoord, the volume on VolMapTexCoord
                    // (1, 2, 3 on the traced piece, whose shader read TEXCOORD1..3; other values UNVERIFIED).
                    auto setOf = [&](int set) { return set == 3 ? gv.uv3 : uvOfSet(gv, set); };
                    const tr::Vec2 n0 = setOf(parameterInt("NormalMap0TexCoord", 1)), n1 = setOf(parameterInt("NormalMap1TexCoord", 2));
                    const tr::Vec2 vol = setOf(parameterInt("VolMapTexCoord", 3));
                    v.texU1 = n0.x; v.texV1 = n0.y;
                    v.texU2 = n1.x; v.texV2 = n1.y;
                    v.texU3 = vol.x; v.texV3 = vol.y;
                }
                vertices.push_back(v);
            }
            if (!vertices.empty() && piece.indices.size() >= 3) {
                ModelPiece part;
                if (objectAnimation && !palette.empty() && rigidBone < 0) {
                    // GPU skinning: the mesh keeps the stored vertices (the frame-0 pose stays in `vertices` for picking and
                    // bounds); each vertex names up to 4 skeleton bones (palette slots outside the skeleton get weight 0).
                    std::vector<MeshVertex> stored = vertices;
                    std::vector<SkinVertex> skinVertices(piece.vertices.size());
                    for (size_t k = 0; k < piece.vertices.size() && k < stored.size(); ++k) {
                        const tr::GeoVertex& gv = piece.vertices[k];
                        MeshVertex& v = stored[k];
                        v.positionX = gv.position.x; v.positionY = gv.position.y; v.positionZ = gv.position.z;
                        v.normalX = gv.normal.x; v.normalY = gv.normal.y; v.normalZ = gv.normal.z;
                        v.tangentX = gv.tangent.x; v.tangentY = gv.tangent.y; v.tangentZ = gv.tangent.z;
                        v.binormalX = gv.binormal.x; v.binormalY = gv.binormal.y; v.binormalZ = gv.binormal.z;
                        SkinVertex& sv = skinVertices[k];
                        for (uint32_t i = 0; i < 4; ++i) {
                            const float w = tr::blendWeight(gv, i);
                            const uint32_t slot = tr::blendIndex(gv, i);
                            if (w <= 0 || slot >= palette.size() || palette[slot] < 0) continue;
                            const uint32_t boneIndex = uint32_t(palette[slot]);
                            switch (i) {
                                case 0: sv.bone0 = boneIndex; sv.weight0 = w; break;
                                case 1: sv.bone1 = boneIndex; sv.weight1 = w; break;
                                case 2: sv.bone2 = boneIndex; sv.weight2 = w; break;
                                default: sv.bone3 = boneIndex; sv.weight3 = w; break;
                            }
                        }
                    }
                    part.mesh = renderer_->createMesh(stored, piece.indices, false);
                    part.skinBuffer = renderer_->createSkinBuffer(skinVertices);
                    part.skinNormals = !aniso;
                    part.skinOwner = &model;
                    model.skinAnimated = true;
                } else if (model.avatarPose && staticBone >= 0 && size_t(staticBone) < skin.size()) {
                    // A posed creature's static piece (e.g. the pistol in an ambient figure's hand): skinned wholly to its bone on
                    // the GPU. Stored = inverse(skin0[bone]) x the frame-0 vertex, so skin(t)[bone] x stored = posed world(t) x local.
                    const Mat4 back = affineInverse(skin[size_t(staticBone)]);
                    std::vector<MeshVertex> stored = vertices;
                    for (MeshVertex& v : stored) {
                        const tr::Vec3 o = transformPoint(back, tr::Vec3{0, 0, 0});
                        const tr::Vec3 p = transformPoint(back, tr::Vec3{v.positionX, v.positionY, v.positionZ});
                        const tr::Vec3 n = subtract(transformPoint(back, tr::Vec3{v.normalX, v.normalY, v.normalZ}), o);
                        const tr::Vec3 t = subtract(transformPoint(back, tr::Vec3{v.tangentX, v.tangentY, v.tangentZ}), o);
                        const tr::Vec3 b = subtract(transformPoint(back, tr::Vec3{v.binormalX, v.binormalY, v.binormalZ}), o);
                        v.positionX = p.x; v.positionY = p.y; v.positionZ = p.z;
                        v.normalX = n.x; v.normalY = n.y; v.normalZ = n.z;
                        v.tangentX = t.x; v.tangentY = t.y; v.tangentZ = t.z;
                        v.binormalX = b.x; v.binormalY = b.y; v.binormalZ = b.z;
                    }
                    std::vector<SkinVertex> skinVertices(stored.size());
                    for (SkinVertex& sv : skinVertices) {
                        sv.bone0 = uint32_t(staticBone);
                        sv.weight0 = 1.0f;
                    }
                    part.mesh = renderer_->createMesh(stored, piece.indices, false);
                    part.skinBuffer = renderer_->createSkinBuffer(skinVertices);
                    part.skinNormals = !aniso;
                    part.skinOwner = &model;
                    model.skinAnimated = true;
                } else {
                    part.mesh = renderer_->createMesh(vertices, piece.indices, false);
                }
                part.water = water;
                model.hasWater = model.hasWater || water;
                if (water && effect.rfind("trwaterreflectrefract", 0) == 0) {
                    WaterMaterial& w = part.waterMaterial;
                    part.waterShader = true;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "DiffuseTexture") w.diffuse = texture(gp.value);
                        if (gp.key == "NormalMapTexture") w.normalMap = texture(gp.value);
                        if (gp.key == "NormalMap1Texture") w.normalMap1 = texture(gp.value);
                        if (gp.key == "ModifierTexture") w.modifier = texture(gp.value);
                        if (gp.key == "VolumeMapTexture") w.volume = volumeTexture(gp.value);
                        if (gp.key == "MatPower") w.matPower = float(std::atof(gp.value.c_str()));
                        if (gp.key == "ClampDepth") w.clampDepth = float(std::atof(gp.value.c_str()));
                        if (gp.key == "RefractionDisplacement") w.refractionDisplacement = float(std::atof(gp.value.c_str()));
                        if (gp.key == "VolumeDisplacement") w.volumeDisplacement = float(std::atof(gp.value.c_str()));
                        if (gp.key == "MatSpecular") w.matSpecular = parseColour(gp.value);
                        if (gp.key == "MatRefractColor") w.matRefractColour = parseColour(gp.value);
                        for (int slot = 0; slot < 4; ++slot) {      // MATRIX_COLUMNS, as the TR vertex shaders (c10.. in the trace)
                            if (gp.key != "TexCoordTransform" + std::to_string(slot)) continue;
                            float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            std::istringstream values(gp.value);
                            for (float& x : e) values >> x;
                            w.transform[slot * 2] = Vec4f{e[0], e[4], e[8], 0};
                            w.transform[slot * 2 + 1] = Vec4f{e[1], e[5], e[9], 0};
                        }
                    }
                }
                part.texture = water ? nullptr : texture(piece.diffuseTexture);    // water's diffuse map is a stand-in
                for (const tr::GeoParameter& p : piece.parameters)
                    if (p.key == "AlphaTestEnable") part.alphaTest = p.value == "true";
                // Cut-outs (effects.glm pixel shaders, 2026-10-11): AlphaTestEnable materials kill below 0.5
                // (TRDiffMapDetMapVertColor psPixelShaderDeferred[1]: texkill(diffuse.a x vertex.a x MatDiffuse.a - 0.5); the
                // editor tests the texture alpha only). TRDiffMapVertColorWave (swaying foliage, e.g.
                // flora_foreas_gen_bush03_v01; no AlphaTestEnable parameter) always kills below 0.25 (psWaveDeferred and
                // psWaveLegacy: texkill(diffuse.a - 0.25)).
                // Pal* forward (ps2x_BaseHemiDir*, the preferred static forward technique of every Pal normal-map effect,
                // disassembled 2026-10-20): alpha = MatDiffuse.a x gBlendFade (preshader mul c5.x, c2.w, c3.x).
                for (const tr::GeoParameter& gp : piece.parameters)      // MatDiffuse.a: x the fade for fading forward pieces
                    if (gp.key == "MatDiffuse") part.matDiffuseAlpha = parseColour(gp.value).w;
                if (palDeferred && !hair) {
                    part.fadeAlphaMode = 6;
                    part.palFading = true;
                }
                if (effect.rfind("trdiffmapvertcolorwave", 0) == 0) {
                    part.alphaTest = true;
                    part.alphaReference = 0.25f;
                    part.sway = true;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "gSwayRate") part.swayRate = float(std::atof(gp.value.c_str()));
                        if (gp.key == "gSwayAmplitude") part.swayAmplitude = float(std::atof(gp.value.c_str()));
                    }
                }
                // PalDiffAlpha* (all five effects; deferred pixel shaders ps2x_*AlphaTest*, verified 2026-10-12) always kill
                // diffuse.a < 0.5, whatever AlphaTestEnable says (e.g. the memory tree's foliage sets none).
                if (effect.rfind("paldiffalpha", 0) == 0) {
                    part.alphaTest = true;
                    part.alphaReference = 0.5f;
                }
                part.hued = tr::isHuedEffect(piece.effectName);
                part.colourScale = vertexColour || palDeferred ? 2.0f : 1.0f;
                part.environment = palDeferred && effect.find("env") != std::string::npos;
                part.hueUnweighted = effect.rfind("paldiffalpha", 0) == 0 && part.hued;
                part.hairCutout = hair;
                part.skyEmissive = effect.rfind("tremissive", 0) == 0;
                part.animBone = rigidBone;
                if (aniso) {
                    if (!palette.empty() && palette.front() >= 0) part.anisoNormalBone = palette.front();
                    else if (bone && bone->scale.x > 0) part.anisoStaticScale = 1.0f / bone->scale.x;    // uniform bone scales
                }
                if (refraction) {
                    // TRRefract (refract.hlsl): diffuse on UV set 0 x TexCoordTransform0, normal map on UV set 1 x
                    // TexCoordTransform1 (both MATRIX_COLUMNS in its vertex shader), ModifierTexture = the edge fade ramp,
                    // MatDiffuse x gModulateScale 0.5.
                    part.refract = true;
                    RefractMaterial& m = part.refractMaterial;
                    m.diffuse = part.texture;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "NormalMapTexture") m.normalMap = texture(gp.value);
                        else if (gp.key == "ModifierTexture") m.fade = texture(gp.value);
                        else if (gp.key == "RefractionDisplacement") m.displacement = float(std::atof(gp.value.c_str()));
                        else if (gp.key == "MatDiffuse") {
                            float c[4] = {1, 1, 1, 1};
                            std::sscanf(gp.value.c_str(), "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]);
                            m.matDiffuse = Vec4f{c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f, c[3] * 0.5f};
                        } else if (gp.key == "TexCoordTransform0" || gp.key == "TexCoordTransform1") {
                            float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            std::istringstream values(gp.value);
                            for (float& x : e) values >> x;
                            const Vec4f u{e[0], e[4], e[8], 0}, v{e[1], e[5], e[9], 0};
                            if (gp.key.back() == '0') { m.transform0u = u; m.transform0v = v; }
                            else { m.transform1u = u; m.transform1v = v; }
                        }
                    }
                }
                if (rigidBone >= 0) model.animated = true;
                if (trDesc.known) {
                    TrMaterial& m = part.trMaterial;
                    part.fadeForward = effect.rfind("trdiffmapdiffmap", 0) == 0;
                    {
                        // The preferred static forward technique's pixel shader alpha (effects.glm, disassembled 2026-10-20).
                        const std::string base = effect.size() > 3 && effect.compare(effect.size() - 3, 3, ".fx") == 0 ? effect.substr(0, effect.size() - 3) : effect;
                        if (part.fadeForward) part.fadeAlphaMode = 1;
                        else if (base == "trdiffmapdetmapvertcolor" || base == "trdiffmapglowmapvertcolor" || base == "tremissivediffmapdetmapdecmapvertcolor")
                            part.fadeAlphaMode = 2;     // PerVertex psPixelShader / psNormal: diffuse.a x v0.w x MatDiffuse.a
                        else if (base == "trdiffmapdecmapvertcolor" || base == "trdiffmapdetmapdecmapvertcolor")
                            part.fadeAlphaMode = 3;     // PerVertex psPixelShader: diffuse.a x MatDiffuse.a (v0 has no alpha)
                        else if (base == "trdiffglossmapenvmapvertcolor" || base == "trdiffmapvertcolorwave")
                            part.fadeAlphaMode = 4;     // psGlossEnvVertexLitMatAlpha / wave: v0.w x MatDiffuse.a
                        else if (base == "trdiffmapvertcolor")
                            part.fadeAlphaMode = 5;     // psNormal: 2 x diffuse.a x v0.w x MatDiffuse.a
                    }
                    part.lavaForward = effect.rfind("trlava", 0) == 0;
                    m.active = true;
                    auto textureOf = [&](const char* key) -> SDL_GPUTexture* {
                        for (const tr::GeoParameter& gp : piece.parameters)
                            if (gp.key == key) return texture(gp.value);
                        return nullptr;
                    };
                    if (trDesc.slot1) m.slot1 = textureOf(trDesc.slot1);
                    if (trDesc.slot2) m.slot2 = textureOf(trDesc.slot2);
                    // A slot whose map is missing is left out (no texture to apply).
                    m.flags = Vec4f{m.slot1 ? float(trDesc.kind1) : 0.0f, m.slot2 ? float(trDesc.kind2) : 0.0f, float(trDesc.envMode),
                                    float(trDesc.bits)};
                    if (trDesc.envMode == 1 || trDesc.envMode == 2) {
                        if (!(trDesc.kind1 == 2 ? m.slot1 : m.slot2)) m.flags.z = 0;     // gloss comes from the detail map
                    }
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "MatSpecular") m.specular = parseColour(gp.value);
                        if (gp.key == "MatEmissive") {
                            const Vec4f e = parseColour(gp.value);
                            m.emissive = Vec4f{e.x * 0.5f, e.y * 0.5f, e.z * 0.5f, e.w};       // x gModulateScale (FUN_005bd040)
                        }
                    }
                    // TRDiffGlossMapEnvMapVertColor's vertex shader array: element 0 (TextureTransformFlags0 = 0) passes the UVs
                    // untransformed.
                    const bool transforms = effect.rfind("trdiffglossmapenvmapvertcolor", 0) != 0 || parameterInt("TextureTransformFlags0", 0) != 0;
                    if (transforms)
                        for (const tr::GeoParameter& gp : piece.parameters) {
                            for (int slot = 0; slot < 3; ++slot) {
                                if (gp.key != "TexCoordTransform" + std::to_string(slot)) continue;
                                float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                                std::istringstream values(gp.value);
                                for (float& x : e) values >> x;
                                // The vertex shaders' constant table declares TexCoordTransform<n> MATRIX_COLUMNS (class 3), so D3DX
                                // uploads the row-major parameter transposed: c = column 0 (e0, e4, e8), column 1 (e1, e5, e9) -
                                // u' = e0 u + e4 v + e8, v' = e1 u + e5 v + e9 (the D3D texture-matrix convention, offsets in e8 / e9).
                                const Vec4f u{e[0], e[4], e[8], 0}, v{e[1], e[5], e[9], 0};
                                if (slot == 0) { m.transform0u = u; m.transform0v = v; }
                                if (slot == 1) { m.transform1u = u; m.transform1v = v; }
                                if (slot == 2) { m.transform2u = u; m.transform2v = v; }
                            }
                        }
                }
                // Alpha cut-out thresholds of the TR pixel shader arrays' AlphaTestEnable elements (2a - 0.9 < 0 kills below 0.45).
                if (part.alphaTest && (effect.rfind("trdiffmapvertcolor.", 0) == 0 || effect.rfind("trdiffmapdetmapdecmapvertcolor", 0) == 0))
                    part.alphaReference = 0.45f;
                for (const tr::GeoParameter& gp : piece.parameters)
                    if (gp.key == "Hue") {
                        float h[4] = {0, 0, 0, 0};
                        std::sscanf(gp.value.c_str(), "%f %f %f %f", &h[0], &h[1], &h[2], &h[3]);
                        part.materialHue = Vec4f{h[0], h[1], h[2], 1.0f};
                    }
                if (!glowName.empty() && !water) part.glow = texture(glowName);
                part.flesh = tr::toLowerAscii(piece.swapGroup) == "flesh";
                part.swapId = piece.swapId;
                part.additive = additive;
                part.translucent = translucent;
                if ((pal || hair) && !water)
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "NormalMapTexture") part.normalMap = texture(gp.value);
                        if (gp.key == "GlossMapTexture") part.glossMap = texture(gp.value);
                        if (gp.key == "DecalTexture" && palDecal) part.palDecal = texture(gp.value);
                    }
                part.forwardLit = (additive || translucent) && !namedAdditive && !aniso;
                if (part.forwardLit)
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key == "DetailTexture" && !water) part.detail = texture(gp.value);
                        if (gp.key == "DetailTexCoord") part.detailSet = std::clamp(std::atoi(gp.value.c_str()), 0, 2);
                        if (gp.key == "MatDiffuse") part.matDiffuseAlpha = parseColour(gp.value).w;
                        for (int slot = 0; slot < 3; ++slot) {
                            if (gp.key != "TexCoordTransform" + std::to_string(slot)) continue;
                            float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            std::istringstream values(gp.value);
                            for (float& x : e) values >> x;
                            // MATRIX_COLUMNS in the forward vertex shaders too (u row e0 e4 e8, v row e1 e5 e9).
                            const Vec4f u{e[0], e[4], e[8], 0}, v{e[1], e[5], e[9], 0};
                            TrMaterial& m = part.forwardUv;
                            if (slot == 0) { m.transform0u = u; m.transform0v = v; }
                            if (slot == 1) { m.transform1u = u; m.transform1v = v; }
                            if (slot == 2) { m.transform2u = u; m.transform2v = v; }
                        }
                    }
                // Effects whose (non-debug) passes set CULLMODE = NONE, from the compiled effects in effects.glm (fx_2_0
                // state tables, 2026-10-10): every other model effect keeps PalGlobalEffect's CCW (back faces culled).
                part.twoSided = effect.rfind("paldiffmap.", 0) == 0 || effect.rfind("trdiffmapnolight.", 0) == 0 ||
                                effect.rfind("trlava.", 0) == 0 || effect.rfind("trbeam.", 0) == 0;
                if (aniso || plainAdditive) {
                    part.aniso = true;
                    AnisoMaterial& m = part.anisoMaterial;
                    m.plain = plainAdditive;
                    if (plainAdditive && effect.find("hue") != std::string::npos) m.hue = part.materialHue.w > 0.5f ? part.materialHue : Vec4f{0.5f, 0.5f, 0.5f, 1};
                    m.diffuse = part.texture;
                    for (const tr::GeoParameter& p : piece.parameters) {
                        if (p.key == "DetailTexture") m.detail = texture(p.value);
                        if (p.key == "DecalTexture") m.decal = texture(p.value);
                        if (p.key == "LightTexture") m.light = texture(p.value);
                        if (p.key == "MatSpecular") m.specular = parseColour(p.value);
                        if (p.key == "MatDiffuse") m.diffuseAlpha = parseColour(p.value);
                        // TexCoordTransform<n>: MATRIX_COLUMNS in vs2x_AnisoDetDec as in the TR shaders (u row e0 e4 e8, v row
                        // e1 e5 e9).
                        for (int slot = 0; slot < 3; ++slot) {
                            if (p.key != "TexCoordTransform" + std::to_string(slot)) continue;
                            float e[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            std::istringstream values(p.value);
                            for (float& x : e) values >> x;
                            const Vec4f u{e[0], e[4], e[8], 0}, v{e[1], e[5], e[9], 0};
                            if (slot == 0) { m.transform0u = u; m.transform0v = v; }
                            if (slot == 1) { m.transform1u = u; m.transform1v = v; }
                            if (slot == 2) { m.transform2u = u; m.transform2v = v; }
                        }
                    }
                }
                part.group = piece.swapGroup;
                // Material animation: the model animation's EFCT tracks whose group is this piece's GPCE group.
                for (size_t t = 0; t < model.animation.materialTracks.size(); ++t) {
                    const tr::MaterialTrack& track = model.animation.materialTracks[t];
                    if (track.group != piece.swapGroup || track.keys.empty()) continue;
                    if (track.parameter == "TexCoordTransform0") part.uvTrack0 = int(t);
                    if (track.parameter == "TexCoordTransform1") part.uvTrack1 = int(t);
                    if (track.parameter == "TexCoordTransform2") part.uvTrack2 = int(t);
                    if (track.parameter == "TexCoordTransform3") part.uvTrack3 = int(t);
                }
                if ((part.uvTrack0 >= 0 || part.uvTrack1 >= 0 || part.uvTrack2 >= 0 || part.uvTrack3 >= 0) &&
                    (part.aniso || part.refract || part.trMaterial.active || part.waterShader || part.forwardLit))
                    model.materialAnimated = true;
                model.pieces.push_back(part);
                if (additive) continue;
                model.hasSolid = true;
                const uint32_t base = uint32_t(model.positions.size());
                for (const MeshVertex& v : vertices) model.positions.push_back(tr::Vec3{v.positionX, v.positionY, v.positionZ});
                for (size_t k = 0; k + 2 < piece.indices.size(); k += 3)
                    if (piece.indices[k] < vertices.size() && piece.indices[k + 1] < vertices.size() && piece.indices[k + 2] < vertices.size()) {
                        model.triangles.insert(model.triangles.end(), {base + piece.indices[k], base + piece.indices[k + 1], base + piece.indices[k + 2]});
                        model.triangleTwoSided.push_back(part.twoSided ? 1 : 0);
                    }
            }
        }
        for (const tr::GeoContactPoint& cp : geo.contactPoints) {
            Mat4 frame = contactPointFrame(geo, cp);
            if (cp.bone >= 0 && size_t(cp.bone) < posedBones.size()) {
                Mat4 local = rotationMatrix(cp.rotation);
                local.row0.w = cp.position.x;
                local.row1.w = cp.position.y;
                local.row2.w = cp.position.z;
                frame = multiply(posedBones[size_t(cp.bone)], local);
            }
            model.contactFrames.push_back(Model::ContactFrame{cp.id, tr::parseConnectionPointName(cp), frame, cp.rotation});
            if (cp.bone >= 0) {                     // re-posed per frame on animated creatures (avatar accessories)
                Mat4 local = rotationMatrix(cp.rotation);
                local.row0.w = cp.position.x;
                local.row1.w = cp.position.y;
                local.row2.w = cp.position.z;
                model.contactFrames.back().bone = cp.bone;
                model.contactFrames.back().local = local;
            }
        }
        // Particle systems on contact points ("psys" "<cp>,<file>.pfx"): the point's frame, on its bone's bind pose.
        for (const tr::ParticleAttachment& a : tr::particleAttachments(geo)) {
            const tr::GeoContactPoint* cp = tr::findContactPoint(geo, a.contactPoint);
            if (!cp) continue;                 // 7 of the shipped references name a point the model lacks
            model.particles.push_back(Model::ParticleMount{contactPointFrame(geo, *cp), a.file});
        }
        // Lights on contact points ("lsys" "<cp>,<file>.ltmp"; tabula_rasa.exe FUN_00816bf0 = SceneBody::AddLightToCP):
        // the pairs that directly follow the lsys pair between "beginlight" and "endlight" are set on the light by
        // property name (FUN_00884cf0 -> FUN_00431d70), e.g. "Range 10". The template's Offset / LocalOrientation are the
        // translation / rotation of the light's own child transform node (Property_LightImplPropsPoint_OffsetRef sets
        // light + 0x234 node + 0xA0, LocalOrientation reads the same node), placed here under the contact point's frame.
        // Position sets a different node; the templates' Position is their authoring location (point_bane_rotatingligh
        // t_orange: 243, 9, 113, 90 m from the Bootcamp lamp whose light shows at the lamp in game), so it is not used -
        // how the attach (FUN_0088c610) parents the nodes is not fully traced. Light Mods are not applied.
        // WorldOrientation is that authoring node's rotation and is not used either: the rotation is the contact point's
        // x LocalOrientation. Checked 2026-10-15 at Bootcamp's medical tent (prop_hum_lamp_v02, hps_lit1 rotation
        // (0.5, -0.5, 0.5, 0.5) -> spot_hum_int_owhite_florescent.ltmp, WorldOrientation 90 degrees about x): the contact
        // point alone aims the spot down, and the game's /dl_showlight (session image 268) shows the tent lit from above;
        // with WorldOrientation applied too the spot shone sideways.
        for (const tr::GeoContactPoint& cp : geo.contactPoints) {
            for (size_t k = 0; k < cp.values.size(); ++k) {
                if (tr::toLowerAscii(cp.values[k].key) != "lsys") continue;
                const std::string& value = cp.values[k].value;
                const size_t comma = value.find(',');
                if (comma == std::string::npos) continue;
                const tr::GeoContactPoint* target = tr::findContactPoint(geo, uint32_t(std::strtoul(value.c_str(), nullptr, 10)));
                if (!target) continue;
                std::map<std::string, std::string> overrides;
                if (k + 1 < cp.values.size() && tr::toLowerAscii(cp.values[k + 1].key) == "beginlight")
                    for (size_t j = k + 2; j < cp.values.size() && tr::toLowerAscii(cp.values[j].key) != "endlight"; ++j)
                        overrides[cp.values[j].key] = cp.values[j].value;
                tr::Bytes data;
                if (!assets_.tryRead(value.substr(comma + 1), data)) continue;
                std::vector<tr::PlacedLight> lights;
                try {
                    lights = tr::readLights(data, overrides);
                } catch (const std::exception&) {
                    continue;
                }
                // The template's own <Mods> (its LightID names one of its lights) travel with the light.
                const std::vector<tr::LightModifier> templateMods = tr::readLightModifiers(data);
                for (tr::PlacedLight& light : lights)
                    for (const tr::LightModifier& mod : templateMods)
                        if (mod.lightId == light.id)
                            light.modifiers.insert(light.modifiers.end(), mod.tracks.begin(), mod.tracks.end());
                const Mat4 frame = contactPointFrame(geo, *target);
                for (tr::PlacedLight light : lights) {
                    light.position = transformPoint(frame, light.offset);
                    light.worldOrientation = multiplyQuat(target->rotation, light.localOrientation);
                    model.lights.push_back(light);
                    model.lightBones.push_back(target->bone);
                }
            }
        }
        model.effectOnly = !model.hasSolid && effectPieces > 0;
        if (!model.positions.empty()) {
            model.bounds = tr::Bounds{model.positions.front(), model.positions.front()};
            for (const tr::Vec3& v : model.positions) {
                model.bounds.min = tr::Vec3{std::min(model.bounds.min.x, v.x), std::min(model.bounds.min.y, v.y), std::min(model.bounds.min.z, v.z)};
                model.bounds.max = tr::Vec3{std::max(model.bounds.max.x, v.x), std::max(model.bounds.max.y, v.y), std::max(model.bounds.max.z, v.z)};
            }
        }
        model.loaded = true;
        ++modelsLoaded_;
    } catch (const std::exception& e) {
        model.failure = e.what();
        model.failed = true;
        ++modelsFailed_;
    }
}

void SceneView::loadPending(double budgetSeconds) {
    const auto start = std::chrono::steady_clock::now();
    bool newGlobe = false;
    while (!pending_.empty()) {
        const std::string name = pending_.front();
        pending_.pop_front();
        Model& model = models_[name];
        loadModel(name, model);
        invalidateShadowMaps();                 // a model that casts may have appeared
        for (const tr::PlacedLight& light : model.lights)
            if (!light.globeMap.empty() && !globeIndex_.count(tr::toLowerAscii(light.globeMap))) newGlobe = true;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > budgetSeconds) break;
    }
    if (newGlobe) loadGlobeMaps();
}

const tr::ParticleSystem* SceneView::particleSystem(const std::string& file) {
    ParticleFile& f = particleFiles_[tr::toLowerAscii(file)];
    if (!f.loaded && f.error.empty()) {
        try {
            f.system = tr::readParticleSystem(assets_.read(file));
            f.loaded = true;
        } catch (const std::exception& e) {
            f.error = e.what();
            SDL_Log("particle system %s: %s", file.c_str(), e.what());
        }
    }
    return f.loaded ? &f.system : nullptr;
}

size_t SceneView::liveParticles() const {
    size_t n = 0;
    for (const LiveParticles& l : liveParticles_) n += l.instance->particleCount();
    return n;
}

void SceneView::setStandaloneParticles(const std::string& file) {
    standaloneFile_ = file;
    standalone_.reset();
    standaloneTime_ = 0;
}

bool SceneView::standaloneParticleBounds(tr::Vec3& centre, float& radius) const {
    return standalone_ && standalone_->worldBounds(centre, radius);
}

std::vector<std::string> SceneView::liveParticleList() const {
    std::vector<std::string> out;
    for (const LiveParticles& l : liveParticles_) {
        const tr::Vec3 at = l.server ? serverPlacements_[l.placement].position : placements_[l.placement].centre;
        char line[256];
        std::snprintf(line, sizeof line, "%s %zu at %.1f %.1f %.1f: %zu particles", l.server ? "server" : "entity", l.placement, at.x, at.y,
                      at.z, l.instance->particleCount());
        out.push_back(line);
    }
    return out;
}

GeoPointSource SceneView::geoPointSource() {
    return [this](const std::string& geo) -> const std::vector<std::vector<tr::Vec3>>* {
        auto it = geoPoints_.find(geo);
        if (it == geoPoints_.end()) {
            std::vector<std::vector<tr::Vec3>> pieces;
            try {
                const tr::GeoModel model = tr::readGeo(assets_.read(geo));
                for (const tr::GeoPiece& piece : model.pieces) {
                    std::vector<tr::Vec3> points;
                    points.reserve(piece.vertices.size());
                    for (const tr::GeoVertex& v : piece.vertices) points.push_back(v.position);
                    pieces.push_back(std::move(points));
                }
            } catch (const std::exception&) {
            }
            it = geoPoints_.emplace(geo, std::move(pieces)).first;
        }
        return &it->second;
    };
}

void SceneView::advanceParticles(float dt) {
    if (!standaloneFile_.empty() && showParticles) {
        if (!standalone_)
            if (const tr::ParticleSystem* system = particleSystem(standaloneFile_)) {
                standalone_ = std::make_unique<ParticleInstance>(*system, identityMatrix(), 1u);
                standalone_->setGeoPoints(geoPointSource());
            }
        if (standalone_ && animate) {
            const float step = std::clamp(dt, 0.0f, 0.1f);
            standalone_->update(step);
            standaloneTime_ += step;
        }
    }
    if (!showParticles || !showEntities) {
        liveParticles_.clear();
        return;
    }
    dt = animate ? std::clamp(dt, 0.0f, 0.1f) : 0.0f;     // paused: the systems keep their particles where they are
    // Drop systems the camera has left behind (with some slack so they do not flicker at the edge).
    const float keep = particleDistance * 1.25f;
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(), [&](const LiveParticles& l) {
                             const tr::Vec3 centre = l.server ? serverPlacements_[l.placement].position : placements_[l.placement].centre;
                             const tr::Vec3 d = subtract(centre, camera_.position);
                             return dot(d, d) > keep * keep;
                         }),
                         liveParticles_.end());
    // Special-FX particle attaches of server objects' use states (target 0 only; see tr::SfxParticleAttach).
    std::set<std::pair<size_t, size_t>> liveServer;
    for (const LiveParticles& l : liveParticles_)
        if (l.server && l.body < 0) liveServer.insert({l.placement, l.mount});
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        const ServerPlacement& s = serverPlacements_[k];
        if (s.stateParticles.empty() || !showStateEffects) continue;
        const tr::Vec3 d = subtract(s.position, camera_.position);
        if (dot(d, d) > particleDistance * particleDistance) continue;
        for (size_t m = 0; m < s.stateParticles.size(); ++m) {
            if (liveServer.count({k, m})) continue;
            const StateParticle& sp = s.stateParticles[m];
            if (sp.event.target != 0) continue;
            Mat4 frame;
            tr::Quat rotation;
            if (!contactFrame(sp.body ? sp.body : s.model, sp.event.contactPoint, 1, frame, rotation)) continue;
            const tr::ParticleSystem* system = particleSystem(sp.event.file);
            if (!system) continue;
            LiveParticles l;
            l.placement = k;
            l.mount = m;
            l.server = true;
            l.instance = std::make_unique<ParticleInstance>(*system, multiply(s.world, frame), uint32_t(k * 6271u + m * 15485863u + 7u));
            l.instance->setGeoPoints(geoPointSource());
            for (int step = 0; step < 90; ++step) l.instance->update(1.0f / 30.0f);
            liveParticles_.push_back(std::move(l));
        }
    }
    // Server objects' own models (and accessories): their contact-point particle systems, as map entities' below.
    std::set<std::pair<size_t, int>> liveBodies;
    for (const LiveParticles& l : liveParticles_)
        if (l.server && l.body >= 0) liveBodies.insert({l.placement, l.body});
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        const ServerPlacement& s = serverPlacements_[k];
        if (s.area || !serverPlacementShown(s)) continue;
        const tr::Vec3 d = subtract(s.position, camera_.position);
        if (dot(d, d) > particleDistance * particleDistance) continue;
        const std::vector<ServerBody> bodies = serverBodies(s, nullptr);
        for (size_t b = 0; b < bodies.size(); ++b) {
            const Model& model = *bodies[b].model;
            if (!model.loaded || model.particles.empty() || liveBodies.count({k, int(b)})) continue;
            for (size_t m = 0; m < model.particles.size(); ++m) {
                const tr::ParticleSystem* system = particleSystem(model.particles[m].file);
                if (!system) continue;
                LiveParticles l;
                l.placement = k;
                l.mount = m;
                l.server = true;
                l.body = int(b);
                l.instance = std::make_unique<ParticleInstance>(*system, multiply(bodies[b].world, model.particles[m].local),
                                                                uint32_t(k * 3571u + b * 7193u + m * 104729u + 11u));
                l.instance->setGeoPoints(geoPointSource());
                for (int step = 0; step < 90; ++step) l.instance->update(1.0f / 30.0f);
                liveParticles_.push_back(std::move(l));
            }
        }
    }
    std::vector<uint8_t> live(placements_.size(), 0);
    for (const LiveParticles& l : liveParticles_)
        if (!l.server) live[l.placement] = 1;
    for (size_t k = 0; k < placements_.size(); ++k) {
        const Placement& p = placements_[k];
        if (live[k] || !p.model || !p.model->loaded || p.model->particles.empty()) continue;
        const tr::Vec3 d = subtract(p.centre, camera_.position);
        if (dot(d, d) > particleDistance * particleDistance) continue;
        for (size_t m = 0; m < p.model->particles.size(); ++m) {
            const tr::ParticleSystem* system = particleSystem(p.model->particles[m].file);
            if (!system) continue;
            LiveParticles l;
            l.placement = k;
            l.mount = m;
            l.instance = std::make_unique<ParticleInstance>(*system, multiply(p.world, p.model->particles[m].local), uint32_t(k * 7919u + m * 104729u + 1u));
            l.instance->setGeoPoints(geoPointSource());
            // Warm-up: 3 s at 30 steps per second, so looping effects are already running (the files' own warm-up
            // fields are not understood).
            for (int step = 0; step < 90; ++step) l.instance->update(1.0f / 30.0f);
            liveParticles_.push_back(std::move(l));
        }
    }
    for (LiveParticles& l : liveParticles_) l.instance->update(dt);
}

std::vector<Mat4> SceneView::animatedPieceMatrices(const Model& model, float time) const {
    std::vector<Mat4> matrices;
    if (!model.animated || model.animation.duration <= 0.0f) return matrices;
    // Looping is assumed (the object animation's playback mode is not decoded); the phase is the editor's clock.
    const float t = std::fmod(std::max(time, 0.0f), model.animation.duration);
    const std::vector<Mat4> skin = skinMatrices(model.skeleton, model.animation, t, nullptr, nullptr, &model.boneTracks);
    matrices.resize(skin.size());
    for (size_t k = 0; k < skin.size() && k < model.restInverse.size(); ++k) matrices[k] = multiply(skin[k], model.restInverse[k]);
    return matrices;
}

bool SceneView::animatedUvTransforms(const Model& model, const ModelPiece& piece, float time, UvTransforms& out) const {
    if (!model.materialAnimated || model.animation.duration <= 0.0f) return false;
    if (piece.uvTrack0 < 0 && piece.uvTrack1 < 0 && piece.uvTrack2 < 0 && piece.uvTrack3 < 0) return false;
    // Same clock and loop as the bone animation (the controller wraps by the animation's length).
    const float t = std::fmod(std::max(time, 0.0f), model.animation.duration);
    auto apply = [&](int track, Vec4f& u, Vec4f& v) {
        if (track < 0) return;
        tr::Vec3 ru, rv;
        tr::materialKeyTransform(tr::sampleMaterialTrack(model.animation.materialTracks[size_t(track)], t), ru, rv);
        u = Vec4f{ru.x, ru.y, ru.z, 0};
        v = Vec4f{rv.x, rv.y, rv.z, 0};
    };
    apply(piece.uvTrack0, out.u0, out.v0);
    apply(piece.uvTrack1, out.u1, out.v1);
    apply(piece.uvTrack2, out.u2, out.v2);
    apply(piece.uvTrack3, out.u3, out.v3);
    return true;
}

const std::vector<Mat4>& SceneView::currentSkin(const Model& model) const {
    if (model.animation.duration <= 0.0f || model.skeleton.bones.empty()) return model.restSkin;
    if (model.frameSkinTime != lightTime) {          // recomputed only while the clock runs
        const float t = std::fmod(std::max(lightTime, 0.0f), model.animation.duration);
        model.frameSkin = skinMatrices(model.skeleton, model.animation, t, nullptr, nullptr, &model.boneTracks);
        model.frameSkinTime = lightTime;
    }
    return model.frameSkin;
}

Mat4 SceneView::anisoNormalMatrix(const Model* model, const ModelPiece& piece, const Mat4& placementWorld, const Mat4& pieceWorld) const {
    auto scaleOf = [](const Mat4& m) { return std::sqrt(m.row0.x * m.row0.x + m.row1.x * m.row1.x + m.row2.x * m.row2.x); };
    auto scaled3 = [](Mat4 m, float s) {
        m.row0 = Vec4f{m.row0.x * s, m.row0.y * s, m.row0.z * s, 0};
        m.row1 = Vec4f{m.row1.x * s, m.row1.y * s, m.row1.z * s, 0};
        m.row2 = Vec4f{m.row2.x * s, m.row2.y * s, m.row2.z * s, 0};
        m.row3 = Vec4f{0, 0, 0, 1};
        return m;
    };
    if (model && piece.anisoNormalBone >= 0) {
        // Skinned: (placement x skin[palette 0]) / the entity's scale - the game's c94 x c106.
        const std::vector<Mat4>& skin = currentSkin(*model);
        if (size_t(piece.anisoNormalBone) < skin.size()) {
            const float s = scaleOf(placementWorld);
            return scaled3(multiply(placementWorld, skin[size_t(piece.anisoNormalBone)]), s > 0 ? 1.0f / s : 1.0f);
        }
    }
    // Static (or unknown palette): the piece's world over its own scale, x 1 / the bone scale baked into the normals - unit length.
    const float s = scaleOf(pieceWorld);
    return scaled3(pieceWorld, (s > 0 ? 1.0f / s : 1.0f) * piece.anisoStaticScale);
}

void SceneView::setPieceSkin(const ModelPiece& piece) {
    // Foliage sway (TRDiffMapVertColorWave) applies to this piece's draws until the skin is cleared (SkinScope / clearSkin).
    // viewport_test RASA_SWAYSCALE=<k>: the amplitude x k (a diagnostic to make the centimetre sway visible).
    static const float swayScale = [] { const char* v = std::getenv("RASA_SWAYSCALE"); return v && *v ? float(std::atof(v)) : 1.0f; }();
    struct SwayOnExit {                 // set after the skin: clearSkin() below resets the sway too
        Renderer& renderer;
        Vec4f sway;
        ~SwayOnExit() { renderer.setSway(sway); }
    } swayOnExit{*renderer_, piece.sway && showTextures ? Vec4f{piece.swayRate, piece.swayAmplitude * swayScale, lightTime, 1.0f} : Vec4f{0, 0, 0, 0}};
    if (!piece.skinBuffer || !piece.skinOwner) {
        renderer_->clearSkin();
        return;
    }
    const Model& model = *piece.skinOwner;
    const auto posed = skinOverride_.find(&model);          // a creature's idle, posed for the placement being drawn
    if (posed != skinOverride_.end()) {
        renderer_->setSkin(piece.skinBuffer, posed->second, piece.skinNormals);
        return;
    }
    if (model.boneFrame != frameNumber_) {
        model.boneBase = renderer_->addBones(currentSkin(model));
        model.boneFrame = frameNumber_;
    }
    renderer_->setSkin(piece.skinBuffer, model.boneBase, piece.skinNormals);
}

// Avatar accessories: the accessory's plug (HPP_<name>, usage 2) is put on the body's socket (HPS_<name>, usage 1)
// of the same connection point (TRasa::EntityBody::FinalizeAddAccessory FUN_00502060 links the two through a
// gfxConnection (FUN_00625300; evaluated by FUN_005e2b80, phyCPConnection). Traced 2026-10-13: the evaluator copies the
// accessory node (FUN_004d7be0), sets its scale to 1 (FLD1 + FUN_00437d20 at 005e2d20) and gives it the inverted
// plug rotation (D3DXQuaternionInverse, FUN_004cbcf0) to carry the plug offset; it then writes only position
// (FUN_00437c70) and rotation (FUN_004cbcf0) to the accessory node, whose scale FUN_0081d820 sets to the body node's
// (+0xB8..0xC0, vtable +0x10 mode 3). So the frames compose without bone scale; the scale is the entity's (in s.world).
// hp_hum_pistol_physical_holster needs this: its plug is on its only bone, scale 0.0117 (the mesh is stored 26 m wide),
// and inverting the scaled frame drew it 85x too large (user report 2026-10-13). Full arithmetic (read 2026-10-13):
// A = body bone world (vtable +8) composed with the socket's local rotation / position (FUN_0044ee60: position through
// A's scaled matrix, rotations multiplied); B = accessory bone world, made relative to the accessory root (offset
// rotated by the inverse root rotation with scale 1) and composed with the plug's local rotation / position; E =
// inverse(rotation B), position = inverse(rotation B)(-B.pos), scale 1; result position = A.pos + rotation A (E.pos)
// (FUN_0044f000: D3DXMatrixTransformation's scaled matrix, then x 1/scale (FUN_0044ed70) - rotation only), rotation =
// E then A. That is s.world x rigid(socket) x inverse(rigid(plug)) here, exactly for uniform scales.
static Mat4 rigidFrame(Mat4 m) {                // the frame without its axes' scale
    for (int c = 0; c < 3; ++c) {
        float* x = c == 0 ? &m.row0.x : c == 1 ? &m.row0.y : &m.row0.z;
        float* y = c == 0 ? &m.row1.x : c == 1 ? &m.row1.y : &m.row1.z;
        float* z = c == 0 ? &m.row2.x : c == 1 ? &m.row2.y : &m.row2.z;
        const float n = std::sqrt(*x * *x + *y * *y + *z * *z);
        if (n > 1e-8f) { *x /= n; *y /= n; *z /= n; }
    }
    return m;
}

bool SceneView::bodyAccessoryWorld(const ServerPlacement& s, const ServerPlacement::BodyAccessory& a, const std::vector<Mat4>* posedBones,
                                   Mat4& world) const {
    if (!a.model || !a.model->loaded) return false;
    Mat4 socket, plug;
    tr::Quat ignored;
    if (!contactFrame(s.model, a.connectionPoint, 1, socket, ignored, posedBones)) return false;
    if (!contactFrame(a.model, a.connectionPoint, 2, plug, ignored)) plug = identityMatrix();
    world = multiply(s.world, multiply(rigidFrame(socket), affineInverse(rigidFrame(plug))));
    return true;
}

std::vector<SceneView::ServerBody> SceneView::serverBodies(const ServerPlacement& s, const std::vector<Mat4>* posedBones) const {
    std::vector<ServerBody> bodies;
    if (s.model && s.model->loaded) bodies.push_back(ServerBody{s.model, s.world});
    for (const Model* accessory : s.accessories)
        if (accessory && accessory->loaded) bodies.push_back(ServerBody{accessory, s.world});
    for (const ServerPlacement::BodyAccessory& a : s.bodyAccessories) {
        Mat4 world;
        if (bodyAccessoryWorld(s, a, posedBones, world)) bodies.push_back(ServerBody{a.model, world});
    }
    return bodies;
}

std::vector<std::string> SceneView::debugServerFigures(const Selection& selection) const {
    std::vector<std::string> out;
    for (const ServerPlacement& s : serverPlacements_) {
        if (!sameSelection(s.selection, selection)) continue;
        char line[256];
        std::snprintf(line, sizeof line, "  %.2f %.2f %.2f  model %p, %zu avatar parts", s.position.x, s.position.y, s.position.z,
                      static_cast<const void*>(s.model), s.avatarParts.size());
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> SceneView::debugServerBodyEffects() const {
    std::vector<std::string> lines;
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        const ServerPlacement& s = serverPlacements_[k];
        size_t lights = 0, systems = 0;
        for (const ServerBody& b : serverBodies(s, nullptr)) {
            lights += b.model->lights.size();
            systems += b.model->particles.size();
        }
        if (lights == 0 && systems == 0) continue;
        char line[160];
        std::snprintf(line, sizeof line, "server placement %zu (kind %d) at %.1f %.1f %.1f: %zu model lights, %zu particle systems", k,
                      int(s.selection.kind), s.position.x, s.position.y, s.position.z, lights, systems);
        lines.push_back(line);
    }
    return lines;
}

// The accessories drawn on server placements (a teleporter's state-56 accessories, e.g. the waypoint column): per accessory model
// whether it is animated, its material tracks, and per piece the forward-phase flag and the UV track each TexCoordTransform uses.
std::vector<std::string> SceneView::debugServerAccessories() const {
    std::vector<std::string> lines;
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        const ServerPlacement& s = serverPlacements_[k];
        if (s.accessories.empty()) continue;
        char head[200];
        std::snprintf(head, sizeof head, "server placement %zu (kind %d) at %.1f %.1f %.1f: %zu accessories", k,
                      int(s.selection.kind), s.position.x, s.position.y, s.position.z, s.accessories.size());
        lines.push_back(head);
        for (size_t a = 0; a < s.accessories.size(); ++a) {
            const Model* m = s.accessories[a];
            if (!m) { lines.push_back("  accessory null"); continue; }
            char line[240];
            std::snprintf(line, sizeof line, "  accessory %zu: loaded %d animated %d materialAnimated %d pieces %zu material tracks %zu duration %.2f s",
                          a, int(m->loaded), int(m->animated), int(m->materialAnimated), m->pieces.size(),
                          m->animation.materialTracks.size(), m->animation.duration);
            lines.push_back(line);
            for (size_t p = 0; p < m->pieces.size(); ++p) {
                const ModelPiece& piece = m->pieces[p];
                char pl[200];
                std::snprintf(pl, sizeof pl, "    piece %zu: forwardLit %d uvTrack %d %d %d", p, int(piece.forwardLit),
                              piece.uvTrack0, piece.uvTrack1, piece.uvTrack2);
                lines.push_back(pl);
                // The UV transforms the animation gives this piece at the current light time (what the draw uses).
                UvTransforms uv{};
                const bool animated = animatedUvTransforms(*m, piece, lightTime, uv);
                char ul[240];
                std::snprintf(ul, sizeof ul, "      t %.2f s: animated %d  transform0 u (%.4f %.4f %.4f) v (%.4f %.4f %.4f)  transform1 u (%.4f %.4f %.4f) v (%.4f %.4f %.4f)",
                              lightTime, int(animated), uv.u0.x, uv.u0.y, uv.u0.z, uv.v0.x, uv.v0.y, uv.v0.z,
                              uv.u1.x, uv.u1.y, uv.u1.z, uv.v1.x, uv.v1.y, uv.v1.z);
                lines.push_back(ul);
            }
        }
    }
    return lines;
}

std::vector<std::string> SceneView::debugServerPose(float x, float z) {
    std::vector<std::string> lines;
    size_t best = serverPlacements_.size();
    float bestD = 1e30f;
    for (size_t k = 0; k < serverPlacements_.size(); ++k) {
        const ServerPlacement& s = serverPlacements_[k];
        if (s.idles.empty()) continue;
        const float d = (s.position.x - x) * (s.position.x - x) + (s.position.z - z) * (s.position.z - z);
        if (d < bestD) { bestD = d; best = k; }
    }
    if (best == serverPlacements_.size()) return lines;
    const ServerPlacement& s = serverPlacements_[best];
    const bool posed = poseIdle(s);
    skinOverride_.clear();
    char line[200];
    std::snprintf(line, sizeof line, "pose: placement %zu at %.2f %.2f %.2f, posed %d, animation %s, t %.3f", best, s.position.x, s.position.y,
                  s.position.z, int(posed), s.idle.current >= 0 ? s.idles[size_t(s.idle.current)].name.c_str() : "-", lightTime - s.idle.start);
    lines.push_back(line);
    if (!posed || !s.model) return lines;
    // Skinned vertices weighted >= 0.9 to the right hand bone, skinned on the CPU with this frame's palette: centroid (world).
    try {
        std::string mesh;
        for (const auto& m : models_)
            if (&m.second == s.model) mesh = m.first.substr(0, m.first.find('|'));
        const tr::GeoModel geo = tr::readGeo(assets_.read(mesh));
        for (const tr::GeoPiece& piece : geo.pieces) {
            if (!piece.skinned) continue;
            tr::Vec3 sum{0, 0, 0}, rest{0, 0, 0};
            size_t n = 0;
            for (const tr::GeoVertex& gv : piece.vertices) {
                const uint32_t slot = tr::blendIndex(gv, 0);
                if (tr::blendWeight(gv, 0) < 0.9f || slot >= piece.skinBones.size() || piece.skinBones[slot] != "Bip01 R Hand") continue;
                size_t bone = 0;
                while (bone < geo.bones.size() && geo.bones[bone].name != "Bip01 R Hand") ++bone;
                if (bone >= s.idle.baseSkin.size()) continue;
                sum = add(sum, transformPoint(multiply(s.world, s.idle.baseSkin[bone]), gv.position));
                rest = add(rest, gv.position);
                ++n;
            }
            if (!n) continue;
            char l[200];
            const tr::Vec3 c = scaled(sum, 1.0f / float(n)), r = scaled(rest, 1.0f / float(n));
            std::snprintf(l, sizeof l, "pose: piece %s: %zu right-hand vertices, centroid %.3f %.3f %.3f (stored %.3f %.3f %.3f)", piece.pieceName.c_str(), n,
                          c.x, c.y, c.z, r.x, r.y, r.z);
            lines.push_back(l);
        }
    } catch (const std::exception&) {
    }
    for (size_t b = 0; b < s.model->skeleton.bones.size() && b < idlePosed_.size(); ++b) {
        const tr::Vec3 p = transformPoint(multiply(s.world, idlePosed_[b]), tr::Vec3{0, 0, 0});
        std::snprintf(line, sizeof line, "pose: %3zu %-28s %8.3f %8.3f %8.3f", b, s.model->skeleton.bones[b].name.c_str(), p.x, p.y, p.z);
        lines.push_back(line);
    }
    return lines;
}

bool SceneView::serverPlacementShown(const ServerPlacement& s) const {
    const SelectionKind kind = s.selection.kind;
    return kind == SelectionKind::Region ? showRegions : kind == SelectionKind::MissionArea ? showMissionAreas : showServer;
}

// TRSphBillboard sprites (vsSphBillboardDirHemiPnt[0] + psBillboardDefault, disassembled 2026-10-14): the centre (POSITION1) goes
// through the world matrix (entity x the piece's bone); the corner offset (POSITION, world units - the entity's scale does not size
// the sprite) is added as offset.x x R + offset.y x toCamera - offset.z x normalize(cross(toCamera, R)), R = the view matrix's first
// column (camera right), toCamera = normalize(eye - centre) - a sprite turned to face the camera about its centre. Colour = the
// texture x (MatDiffuse x gDirLight[0].Color x sat(N . -direction) + MatAmbient x lerp(hemi bottom, top, (N.y + 1) / 2) +
// MatEmissive), N = the vertex normal untransformed, clamped (oD0); alpha = texture alpha x MatDiffuse.a. Pass states: CULLMODE NONE,
// FOGENABLE 0, ALPHATEST off; the blend comes from the piece's phase (Additive: SRCALPHA / ONE). The shader array's element [1]
// adds one point light (gPointLight[0]) - when the engine picks it is NOT traced; the editor draws element [0]. The forward
// gDirLight / gHemiLight are taken as the frame's sun and hemi light (forward combined lights - see "Sky lights and forward
// pieces").
void SceneView::queueBillboards(const Model& model, const Mat4& placementWorld, const std::vector<Mat4>& animatedMatrices, const SceneLighting& light) {
    const tr::Vec3 right = camera_.right();
    std::vector<MeshVertex> quads;
    for (const Model::Billboard& bb : model.billboards) {
        Mat4 world = multiply(placementWorld, bb.boneWorld);
        if (bb.animBone >= 0 && size_t(bb.animBone) < animatedMatrices.size())
            world = multiply(placementWorld, multiply(animatedMatrices[size_t(bb.animBone)], bb.boneWorld));
        quads.clear();
        for (uint32_t index : bb.indices) {
            if (index >= bb.vertices.size()) continue;
            const tr::GeoVertex& gv = bb.vertices[index];
            const tr::Vec3 centre = transformPoint(world, gv.position1);
            const tr::Vec3 toCamera = normalized(subtract(camera_.position, centre));
            const tr::Vec3 up = normalized(cross(toCamera, right));
            const tr::Vec3 p = add(add(add(centre, scaled(right, gv.position.x)), scaled(toCamera, gv.position.y)), scaled(up, -gv.position.z));
            const tr::Vec3& n = gv.normal;
            const float facing = std::clamp(dot(n, normalized(light.forwardTowards)), 0.0f, 1.0f);
            const float h = (n.y + 1.0f) * 0.5f;
            auto channel = [&](float diffuse, float sun, float top, float bottom, float ambient, float emissive) {
                return std::clamp(diffuse * sun * facing + ambient * (bottom + (top - bottom) * h) + emissive, 0.0f, 1.0f);
            };
            const float r = channel(bb.diffuse.x, light.forwardColour.x, light.hemiTop.x, light.hemiBottom.x, bb.ambient.x, bb.emissive.x);
            const float g = channel(bb.diffuse.y, light.forwardColour.y, light.hemiTop.y, light.hemiBottom.y, bb.ambient.y, bb.emissive.y);
            const float b = channel(bb.diffuse.z, light.forwardColour.z, light.hemiTop.z, light.hemiBottom.z, bb.ambient.z, bb.emissive.z);
            MeshVertex v{};
            v.positionX = p.x; v.positionY = p.y; v.positionZ = p.z;
            v.normalX = toCamera.x; v.normalY = toCamera.y; v.normalZ = toCamera.z;
            v.texU = gv.uv.x;
            v.texV = gv.uv.y;
            v.colour = packColour(uint8_t(r * 255.0f + 0.5f), uint8_t(g * 255.0f + 0.5f), uint8_t(b * 255.0f + 0.5f),
                                  uint8_t(std::clamp(bb.diffuse.w, 0.0f, 1.0f) * 255.0f + 0.5f));
            quads.push_back(v);
        }
        renderer_->drawParticles(quads, showTextures ? bb.texture : nullptr, 0, false);   // additive (Phase Additive), no fog
    }
}

// Sky layers (decoded 2026-10-14 in tabula_rasa.exe): a .sky's SkyLayers become TRasa::SkyLayer objects, each with a body of its
// GeoFilename model (FUN_00531910 -> FUN_005daf60, FUN_00822f30 copies the layer data in) drawn in the Sky phase (TRasa::SkyPhase,
// FUN_0087fa90: Sky.fx - SRCALPHA / INVSRCALPHA, no depth writes, alpha test > 1/255, no fog, stencil EQUAL 255, i.e. only the
// background). Per frame (FUN_008223b0 -> FUN_00885af0, time = fraction of the day): rotation = RotsPerDay turns about the
// normalised RotationAxis combined with the initial yaw / pitch / roll (the editor uses the same rotation as its layer lights,
// tr::skyLayerDirection, whose roll sign was fitted to the game's sun); position = the given position (taken as the camera's -
// the argument comes through a virtual call, NOT traced; the layer models are 0.2 to 100 m across, scale 1) + the offset keys
// (FUN_00885f90). Colour (FUN_00886000): MatDiffuse = (2 x (base + key.rgb) [x 2 x the fog colour with UseFogColor], key.a) and
// MatEmissive = (1, 1, 1, 1), both through the parameter setter that multiplies Diffuse / Emissive semantics by gModulateScale
// 0.5 (FUN_0057ea60 -> FUN_005bd040); base = DiffuseColor (FUN_00823470) - and the engine adds the GREEN base to blue too (reads
// + 0x68 twice), so the moon's (0.5, 0.5, 0) base is grey. Pieces: TREmissive* (psNormal: texture x vertex colour x
// MatDiffuse.rgb x 4 + (MatEmissive - 0.5) x 2, opaque - the material's AlphaBlendEnable is false) and TRAdditive* (sun, moon,
// stars, sunset: their own additive shaders with MatDiffuse.a = the key alpha). Not drawn: other effects (e.g. the
// TRDiffMapVertColor fog ring of sky_concordia_fog_ring.geo), cloud textures (CloudTexture1 / 2 are empty in every shipped
// sky), the detail / decal layers of TREmissiveDiffMapDetMapDecMapVertColor, and the transition fade between skies.
void SceneView::queueSky() {
    if (!showSkyDome || !skyLighting) return;
    for (const SkyWeight& w : skyWeights()) queueSkyLayers(skies_[w.index].sky, w.weight);
}

// One sky's layer models; `fade` scales the layers' alpha (a sky change cross-fades, see skyTransitions).
void SceneView::queueSkyLayers(const tr::Sky& sky, float fade) {
    const float t = timeOfDay;
    const tr::Vec3 fog{frameLighting_.fogColour.x, frameLighting_.fogColour.y, frameLighting_.fogColour.z};
    for (const tr::SkyLayer& layer : sky.layers) {
        if (!layer.enabled || layer.geoFilename.empty()) continue;
        Model* model = objectModelFor(layer.geoFilename);
        if (!model || !model->loaded) continue;
        // Rotation: the layer's basis vectors through the same rotation as its lights.
        const tr::Vec3 ex = tr::skyLayerDirection(layer, tr::Vec3{1, 0, 0}, t);
        const tr::Vec3 ey = tr::skyLayerDirection(layer, tr::Vec3{0, 1, 0}, t);
        const tr::Vec3 ez = tr::skyLayerDirection(layer, tr::Vec3{0, 0, 1}, t);
        const tr::Vec3 offset = tr::sampleLayerOffset(layer, t);
        Mat4 world = identityMatrix();
        world.row0 = Vec4f{ex.x, ey.x, ez.x, camera_.position.x + offset.x};
        world.row1 = Vec4f{ex.y, ey.y, ez.y, camera_.position.y + offset.y};
        world.row2 = Vec4f{ex.z, ey.z, ez.z, camera_.position.z + offset.z};
        const tr::SkyColourKey key = tr::sampleLayerColour(layer, t);
        // MatDiffuse as the shader sees it (x gModulateScale 0.5): (base + key) [x 2 fog]; blue takes the green base (engine).
        tr::Vec3 diffuse{layer.baseColour.x + key.colour.x, layer.baseColour.y + key.colour.y, layer.baseColour.y + key.colour.z};
        if (layer.useFogColour) diffuse = tr::Vec3{diffuse.x * 2 * fog.x, diffuse.y * 2 * fog.y, diffuse.z * 2 * fog.z};
        const Vec4f emissiveTint{diffuse.x * 4, diffuse.y * 4, diffuse.z * 4, key.alpha * fade};
        const std::vector<Mat4> animatedMatrices = animatedPieceMatrices(*model, lightTime);
        for (const ModelPiece& piece : model->pieces) {
            if (!hiddenGroup.empty() && (hiddenGroup[0] == '!' ? piece.group != hiddenGroup.substr(1) : piece.group == hiddenGroup)) continue;
            const Mat4 pieceWorld = piece.animBone >= 0 && size_t(piece.animBone) < animatedMatrices.size()
                                        ? multiply(world, animatedMatrices[size_t(piece.animBone)]) : world;
            struct SkinScope {
                Renderer& renderer;
                ~SkinScope() { renderer.clearSkin(); }
            } skinScope{*renderer_};
            setPieceSkin(piece);
            if (piece.skyEmissive && !piece.additive) {
                renderer_->drawSkyEmissive(piece.mesh, pieceWorld, emissiveTint, showTextures ? piece.texture : nullptr, !piece.twoSided);
            } else if (piece.aniso && showTextures && showEffects) {
                AnisoMaterial m = piece.anisoMaterial;
                m.diffuseAlpha.w = key.alpha * fade;     // MatDiffuse.a = the key alpha (gModulateScale leaves alpha alone)
                m.normalMatrix = anisoNormalMatrix(model, piece, world, pieceWorld);
                UvTransforms uv{m.transform0u, m.transform0v, m.transform1u, m.transform1v, m.transform2u, m.transform2v};
                if (animatedUvTransforms(*model, piece, lightTime, uv)) {
                    m.transform0u = uv.u0; m.transform0v = uv.v0; m.transform1u = uv.u1; m.transform1v = uv.v1;
                    m.transform2u = uv.u2; m.transform2v = uv.v2;
                }
                renderer_->drawAnisoAdditive(piece.mesh, pieceWorld, m, !piece.twoSided, true);
            }
        }
    }
}

// ------------------------------------------------------------------------------------------
// Creature idles
// ------------------------------------------------------------------------------------------
const tr::Animation* SceneView::idleAnimation(const std::string& name) {
    auto found = idleAnimations_.find(name);
    if (found == idleAnimations_.end()) {
        std::unique_ptr<tr::Animation> animation;
        try {
            animation = std::make_unique<tr::Animation>(tr::readAnimation(assets_.read(name)));
        } catch (const std::exception& e) {
            SDL_Log("idle %s: %s", name.c_str(), e.what());
        }
        found = idleAnimations_.emplace(name, std::move(animation)).first;
    }
    return found->second.get();
}

const std::vector<int>& SceneView::idleTracks(const Model& model, const tr::Animation& animation) {
    const auto key = std::make_pair(&model, &animation);
    auto found = idleTracks_.find(key);
    if (found == idleTracks_.end()) found = idleTracks_.emplace(key, boneTrackIndices(model.skeleton, animation)).first;
    return found->second;
}

// Per bone of an avatar part, its index in the base body's skeleton (by name, case-insensitive, as skinMatrices matches them).
const std::vector<int>& SceneView::poseBoneIndices(const Model& part, const Model& base) {
    const auto key = std::make_pair(&part, &base);
    auto found = poseBoneIndices_.find(key);
    if (found == poseBoneIndices_.end()) {
        std::unordered_map<std::string, int> byName;
        for (size_t k = 0; k < base.skeleton.bones.size(); ++k) byName.emplace(tr::toLowerAscii(base.skeleton.bones[k].name), int(k));
        std::vector<int> indices;
        for (const tr::GeoBone& bone : part.skeleton.bones) {
            const auto b = byName.find(tr::toLowerAscii(bone.name));
            indices.push_back(b == byName.end() ? -1 : b->second);
        }
        found = poseBoneIndices_.emplace(key, std::move(indices)).first;
    }
    return found->second;
}

// The client plays a creature's "Idle - Non Combat" family as an ambient family: chained, from the first frame, and each time an
// animation ends the next is drawn by weighted random over the family's frequencies (characteranimationmgr.py
// UpdateAnimations -> PlayAnimationFamily(chain=1, randStartFrame=0) -> ChainAnimationFamily, _SelectAnimation). The editor uses
// its own random stream (Park-Miller, seeded by the placement) on the light clock, so the order is not the game's; a clock that
// runs backwards restarts the sequence (with the clock stopped - SceneView::animate off - the creature holds its pose). Fills
// skinOverride_ (the base model's and each part's palette) and idlePosed_ (the base skeleton's posed bones, for accessory sockets).
// False: draw the models' stored first frame.
bool SceneView::poseIdle(const ServerPlacement& s) {
    if (s.idles.empty() || !s.model || !s.model->avatarPose || s.model->skeleton.bones.empty()) return false;
    IdleSequence& q = s.idle;
    int32_t total = 0;
    for (const tr::GameData::FamilyAnimation& a : s.idles) total += std::max(a.frequency, 0);
    auto pick = [&]() {
        q.random = uint32_t(uint64_t(q.random) * 48271u % 2147483647u);
        if (total <= 0) return int32_t(0);
        int32_t r = int32_t(double(q.random) / 2147483647.0 * double(total));
        for (size_t k = 0; k < s.idles.size(); ++k) {
            r -= std::max(s.idles[k].frequency, 0);
            if (r < 0) return int32_t(k);
        }
        return int32_t(s.idles.size() - 1);
    };
    const float now = std::max(lightTime, 0.0f);
    if (q.current < 0 || now < q.last) {
        q.random = uint32_t((size_t(s.selection.kind) * 7919u + s.selection.index) % 2147483646u) + 1u;
        q.random = uint32_t(uint64_t(q.random) * 48271u % 2147483647u);
        q.delay = float(s.idleDelayLower + int32_t(double(q.random) / 2147483647.0 * double(s.idleDelayUpper - s.idleDelayLower))) * 0.001f;
        q.start = q.delay;                // objectanimationmgr.py: a delayed first animation starts after the delay
        q.current = pick();
    }
    q.last = now;
    // objectanimationmgr.py _AnimationComplete by type: 2 looping - one pick, looped; 3 recurring one-shot - played once, the
    // next pick starts its length + the delay after it (the delay is drawn once per PlayObjectAnimation); 1 / 5 one-shot and 4
    // move-to-last-frame - one pick played once. A finished one-shot holds its last frame (UNVERIFIED: the engine's
    // non-looping playback end is not traced), as does the gap of a recurring delay; before a delayed first animation the
    // models' stored first frame is drawn.
    if (now < q.start) return false;
    const tr::Animation* animation = idleAnimation(s.idles[size_t(q.current)].name);
    float t = 0;
    if (s.idleType == 2) {
        if (!animation || animation->duration <= 0.0f) return false;
        t = std::fmod(now - q.start, animation->duration);
    } else if (s.idleType == 3) {
        animation = nullptr;
        for (int guard = 0; guard < 100000; ++guard) {
            animation = idleAnimation(s.idles[size_t(q.current)].name);
            const float duration = animation ? animation->duration : 0.0f;
            if (duration > 0.0f && now - q.start < duration + q.delay) break;
            q.start += duration + q.delay;
            q.current = pick();
            animation = nullptr;
        }
        if (!animation) return false;
        t = std::min(now - q.start, animation->duration);
    } else {
        if (!animation || animation->duration <= 0.0f) return false;
        t = s.idleType == 4 ? animation->duration : std::min(now - q.start, animation->duration);
    }
    const Model& base = *s.model;
    // The palettes are kept while the pose does not change (the clock stopped, or the same time drawn twice); parts that finish
    // loading invalidate them.
    size_t loadedParts = 0;
    for (const AvatarDraw& part : s.avatarParts)
        if (part.model && part.model->loaded) ++loadedParts;
    if (q.cacheTime != t || q.cacheAnimation != animation || q.cacheParts != loadedParts) {
        q.baseSkin = skinMatrices(base.skeleton, *animation, t, nullptr, &q.posed, &idleTracks(base, *animation));
        q.partSkins.clear();
        for (const AvatarDraw& part : s.avatarParts) {
            const Model* m = part.model;
            if (!m || !m->loaded || !m->avatarPose || m->skeleton.bones.empty()) continue;
            bool seen = false;
            for (const auto& p : q.partSkins) seen = seen || p.first == m;
            if (!seen)
                q.partSkins.emplace_back(m, skinMatrices(m->skeleton, *animation, t, nullptr, nullptr, &idleTracks(*m, *animation), &q.posed,
                                                         &poseBoneIndices(*m, base)));
        }
        q.cacheTime = t;
        q.cacheAnimation = animation;
        q.cacheParts = loadedParts;
    }
    idlePosed_ = q.posed;
    skinOverride_[&base] = renderer_->addBones(q.baseSkin);
    for (const auto& p : q.partSkins)
        if (!skinOverride_.count(p.first)) skinOverride_[p.first] = renderer_->addBones(p.second);
    return true;
}

// ------------------------------------------------------------------------------------------
// Special-FX accessories of usable objects
// ------------------------------------------------------------------------------------------
SceneView::StateEffects SceneView::stateEffects(int32_t classId, int32_t stateId) {
    StateEffects effects;
    if (!usablePackageFor_) return effects;
    std::string package;
    try { package = usablePackageFor_(classId, stateId); } catch (const std::exception&) { return effects; }
    if (!package.empty()) addPackageEffects(package, nullptr, 0, effects);
    return effects;
}

// A package's accessories, particle attaches and lights; an accessory's nested package attaches to the accessory's model.
void SceneView::addPackageEffects(const std::string& package, Model* body, int depth, StateEffects& effects) {
    if (depth > 3) return;
    try {
        const tr::SfxPackage p = tr::readSfxPackage(assets_.read(package));
        for (const tr::SfxParticleAttach& a : p.particles) effects.particles.push_back(StateParticle{body, a});
        for (const tr::SfxLight& l : p.lights) effects.lights.push_back(StateLight{body, l});
        // Accessories are drawn at the placement (their own attachment fields are not decoded).
        for (const tr::SfxAccessory& a : p.accessories) {
            Model* accessory = a.mesh.empty() ? nullptr : objectModelFor(a.mesh, a.animation);
            if (accessory) effects.accessories.push_back(accessory);
            if (!a.package.empty()) addPackageEffects(a.package, accessory ? accessory : body, depth + 1, effects);
        }
    } catch (const std::exception& e) {
        SDL_Log("special fx %s: %s", package.c_str(), e.what());
    }
}

bool SceneView::contactFrame(const Model* body, uint32_t id, uint32_t usage, Mat4& frame, tr::Quat& rotation, const std::vector<Mat4>* posedBones) {
    frame = identityMatrix();
    rotation = tr::Quat{};
    if (id == 0) return true;
    if (!body || !body->loaded) return false;
    for (const Model::ContactFrame& c : body->contactFrames)
        if (c.connection.valid && c.connection.type == 1 && c.connection.usage == usage && c.connection.id == id) {
            frame = c.frame;
            if (posedBones && c.bone >= 0 && size_t(c.bone) < posedBones->size()) frame = multiply((*posedBones)[size_t(c.bone)], c.local);
            rotation = c.rotation;
            return true;
        }
    return false;
}

// ------------------------------------------------------------------------------------------
// Terrain clutter
// ------------------------------------------------------------------------------------------
size_t SceneView::clutterInstances() const {
    size_t n = 0;
    for (const ClutterCell& cell : clutterCells_)
        for (const ClutterGroup& group : cell.groups) n += group.instances.size();
    return n;
}

void SceneView::updateClutterCells() {
    const tr::Terrain& t = map_.terrain;
    if (t.columns == 0 || map_.tribbleSets.empty()) return;
    const uint32_t cellsX = tr::tribbleCellsX(t), cellsZ = tr::tribbleCellsZ(t);
    // FUN_0073b930: the camera's sample / 32, then the cells within tribbleCellRange of it.
    const int64_t column = int64_t((camera_.position.x - t.header.bounds.min.x) / t.header.spacing);
    const int64_t row = int64_t((camera_.position.z - t.header.bounds.min.z) / t.header.spacing);
    const int64_t cx = column >> 5, cz = row >> 5;
    auto wanted = [&](uint32_t index) {
        const int64_t x = index % cellsX, z = index / cellsX;
        return std::llabs(x - cx) <= tribbleCellRange && std::llabs(z - cz) <= tribbleCellRange;
    };
    clutterCells_.erase(std::remove_if(clutterCells_.begin(), clutterCells_.end(), [&](const ClutterCell& c) { return !wanted(c.index); }),
                        clutterCells_.end());
    int built = 0;
    for (int64_t z = std::max<int64_t>(cz - tribbleCellRange, 0); z <= std::min<int64_t>(cz + tribbleCellRange, int64_t(cellsZ) - 1); ++z)
        for (int64_t x = std::max<int64_t>(cx - tribbleCellRange, 0); x <= std::min<int64_t>(cx + tribbleCellRange, int64_t(cellsX) - 1); ++x) {
            const uint32_t index = uint32_t(z * cellsX + x);
            if (std::any_of(clutterCells_.begin(), clutterCells_.end(), [&](const ClutterCell& c) { return c.index == index; })) continue;
            if (built++ >= clutterCellsPerFrame) return;
            ClutterCell cell;
            cell.index = index;
            std::map<uint32_t, size_t> groupOf;
            tr::Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (const tr::TribbleInstance& instance : tr::placeTribbleCell(t, map_.textureLayers, map_.tribbleSets, index)) {
                auto found = groupOf.find(instance.container);           // one group per container (mesh)
                if (found == groupOf.end()) {
                    const tr::TribbleModel& model = map_.tribbleSets[instance.set].models[instance.model];
                    ClutterGroup group;
                    group.model = modelFor(model.model);
                    group.drawDistance = instance.containerDrawDistance;
                    const uint32_t key = instance.container;
                    found = groupOf.emplace(key, cell.groups.size()).first;
                    cell.groups.push_back(std::move(group));
                }
                ClutterInstance c;
                c.model = cell.groups[found->second].model;
                c.world = placementMatrix(instance.position, instance.rotation, instance.scale);
                c.position = instance.position;
                c.scale = instance.scale;
                cell.groups[found->second].instances.push_back(c);
                lo = tr::Vec3{std::min(lo.x, c.position.x), std::min(lo.y, c.position.y), std::min(lo.z, c.position.z)};
                hi = tr::Vec3{std::max(hi.x, c.position.x), std::max(hi.y, c.position.y), std::max(hi.z, c.position.z)};
            }
            if (!cell.groups.empty()) {
                cell.centre = scaled(add(lo, hi), 0.5f);
                cell.radius = length(subtract(hi, lo)) * 0.5f;
            }
            clutterCells_.push_back(std::move(cell));
        }
}

// ------------------------------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------------------------------
// A point in a bone's bind frame -> model space through the skeleton's bind chain (PSKE: child = parent x T x R x S), the frame
// the client places a bone's walkable surface in: WalkableSurfaceManager::AddWalkableSurface (FUN_00925d70) updates the skeleton from
// its bind values (FUN_005a10b0) and takes the bone's world matrix (FUN_005a1330) - not the stored inverse bind, which is identity on
// e.g. arch_hum_tent_generic_01_v02 whose "collision" bone sits 4 m along z (found 2026-10-19: the collision drew 4 m off).
static tr::Vec3 boneBindPoint(const tr::GeoFile& file, size_t bone, tr::Vec3 p) {
    for (size_t guard = 0; bone < file.bones.size() && guard < 256; ++guard) {
        const tr::GeoFileBone& b = file.bones[bone];
        p = tr::Vec3{p.x * b.scale.x, p.y * b.scale.y, p.z * b.scale.z};
        const tr::Quat& q = b.rotation;
        const tr::Vec3 t{2 * (q.y * p.z - q.z * p.y), 2 * (q.z * p.x - q.x * p.z), 2 * (q.x * p.y - q.y * p.x)};
        p = tr::Vec3{p.x + q.w * t.x + (q.y * t.z - q.z * t.y) + b.translation.x, p.y + q.w * t.y + (q.z * t.x - q.x * t.z) + b.translation.y,
                     p.z + q.w * t.z + (q.x * t.y - q.y * t.x) + b.translation.z};
        if (b.parent < 0 || size_t(b.parent) >= file.bones.size() || size_t(b.parent) == bone) break;
        bone = size_t(b.parent);
    }
    return p;
}

// A model's collision as the client uses it (tabula_rasa.exe WalkableSurfaceManager::AddWalkableSurfaces, FUN_00925590: every bone
// volume of type 5 = BVWS, provided the bone's property group - the group whose id is the bone index - names a WSName), in model space:
// the volume's coordinates are in the bone's bind frame (boneBindPoint). Built once per
// model, at most buildBudget a frame (each reads the .geo again).
const SceneView::CollisionMesh* SceneView::collisionFor(const Model* model, size_t& buildBudget) {
    auto it = collisionMeshes_.find(model);
    if (it != collisionMeshes_.end()) return &it->second;
    if (buildBudget == 0 || !model->loaded) return nullptr;
    --buildBudget;
    CollisionMesh& c = collisionMeshes_[model];
    c.built = true;
    std::string name;
    for (const auto& m : models_)
        if (&m.second == model) name = m.first;
    name = name.substr(0, name.find_first_of("|%"));
    tr::GeoFile file;
    try {
        file = tr::readGeoFile(assets_.read(name));
    } catch (const std::exception&) {
        return &c;
    }
    struct Built {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> triangles, edges;
    } used, dropped, ignored;
    auto vertex = [](const tr::Vec3& p, const tr::Vec3& n) {
        MeshVertex v;
        v.positionX = p.x; v.positionY = p.y; v.positionZ = p.z;
        v.normalX = n.x; v.normalY = n.y; v.normalZ = n.z;
        return v;
    };
    for (size_t b = 0; b < file.bones.size(); ++b) {
        const tr::GeoFileBone& bone = file.bones[b];
        if (bone.boundingVolume.empty()) continue;
        bool named = false;
        for (const tr::GeoPropertyGroup& g : file.properties)
            if (g.id == int32_t(b))
                for (const tr::GeoParameter& p : g.pairs) named = named || p.key == "WSName";
        auto place = [&](const tr::Vec3& v) { return boneBindPoint(file, b, v); };
        std::vector<tr::GeoBoneVolume> volumes;
        try { volumes = tr::readBoneVolumes(bone.boundingVolume); } catch (const std::exception&) { continue; }
        for (const tr::GeoBoneVolume& v : volumes) {
            if (v.kind == tr::fourcc("BVWS")) {
                Built& out = named ? used : dropped;
                std::vector<tr::Vec3> points;
                for (const tr::Vec3& p : v.vertices) points.push_back(place(p));
                // Vertex normals (area-weighted face normals; (b - a) x (c - a) faces outward on the shipped surfaces) for the 3 cm push.
                std::vector<tr::Vec3> normals(points.size(), tr::Vec3{0, 0, 0});
                for (size_t t = 0; t + 2 < v.indices.size(); t += 3) {
                    const tr::Vec3 &p0 = points[v.indices[t]], &p1 = points[v.indices[t + 1]], &p2 = points[v.indices[t + 2]];
                    const tr::Vec3 e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z}, e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
                    const tr::Vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
                    for (int q = 0; q < 3; ++q) {
                        tr::Vec3& acc = normals[v.indices[t + q]];
                        acc = tr::Vec3{acc.x + n.x, acc.y + n.y, acc.z + n.z};
                    }
                }
                const uint32_t first = uint32_t(out.vertices.size());
                for (size_t q = 0; q < points.size(); ++q) {
                    tr::Vec3 n = normals[q];
                    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    n = len > 0 ? tr::Vec3{n.x / len, n.y / len, n.z / len} : tr::Vec3{0, 1, 0};
                    out.vertices.push_back(vertex(tr::Vec3{points[q].x + n.x * 0.03f, points[q].y + n.y * 0.03f, points[q].z + n.z * 0.03f}, n));
                }
                std::set<std::pair<uint32_t, uint32_t>> seen;
                for (size_t t = 0; t + 2 < v.indices.size(); t += 3) {
                    for (int q = 0; q < 3; ++q) {
                        out.triangles.push_back(first + v.indices[t + q]);
                        uint32_t i0 = v.indices[t + q], i1 = v.indices[t + (q + 1) % 3];
                        if (i0 > i1) std::swap(i0, i1);
                        if (seen.insert({i0, i1}).second) out.edges.insert(out.edges.end(), {first + i0, first + i1});
                    }
                }
            } else if (v.kind == tr::fourcc("BVBX") || v.kind == tr::fourcc("BVSP")) {
                const uint32_t first = uint32_t(ignored.vertices.size());
                if (v.kind == tr::fourcc("BVBX")) {
                    for (int corner = 0; corner < 8; ++corner)
                        ignored.vertices.push_back(vertex(place(tr::Vec3{v.centre.x + (corner & 1 ? 1 : -1) * v.halfExtents.x,
                                                                          v.centre.y + (corner & 2 ? 1 : -1) * v.halfExtents.y,
                                                                          v.centre.z + (corner & 4 ? 1 : -1) * v.halfExtents.z}), tr::Vec3{0, 1, 0}));
                    for (uint32_t c0 = 0; c0 < 8; ++c0)
                        for (uint32_t bit : {1u, 2u, 4u})
                            if (!(c0 & bit)) ignored.edges.insert(ignored.edges.end(), {first + c0, first + (c0 | bit)});
                } else {
                    // Three rings in the bone frame's planes.
                    for (int plane = 0; plane < 3; ++plane)
                        for (uint32_t s = 0; s < 32; ++s) {
                            const float ang = 6.2831853f * float(s) / 32.0f, ca = std::cos(ang) * v.sphereRadius, sa = std::sin(ang) * v.sphereRadius;
                            const tr::Vec3 o = plane == 0 ? tr::Vec3{ca, sa, 0} : plane == 1 ? tr::Vec3{ca, 0, sa} : tr::Vec3{0, ca, sa};
                            ignored.vertices.push_back(vertex(place(tr::Vec3{v.centre.x + o.x, v.centre.y + o.y, v.centre.z + o.z}), tr::Vec3{0, 1, 0}));
                            ignored.edges.insert(ignored.edges.end(), {first + uint32_t(plane) * 32 + s, first + uint32_t(plane) * 32 + (s + 1) % 32});
                        }
                }
            }
        }
    }
    if (!used.triangles.empty()) {
        c.used = renderer_->createMesh(used.vertices, used.triangles, false);
        c.usedEdges = renderer_->createMesh(used.vertices, used.edges, true);
    }
    if (!dropped.triangles.empty()) {
        c.dropped = renderer_->createMesh(dropped.vertices, dropped.triangles, false);
        c.droppedEdges = renderer_->createMesh(dropped.vertices, dropped.edges, true);
    }
    if (!ignored.edges.empty()) c.ignored = renderer_->createMesh(ignored.vertices, ignored.edges, true);
    return &c;
}

void SceneView::queueDraws(const Selection& selection) {
    ++frameNumber_;
    auto sectionStart = std::chrono::steady_clock::now();
    auto endSection = [&](int section) {
        const auto t = std::chrono::steady_clock::now();
        queueSectionMs_[section] = std::chrono::duration<double, std::milli>(t - sectionStart).count();
        sectionStart = t;
    };
    const int selectedEntity = selection.kind == SelectionKind::MapEntity ? int(selection.index) : -1;
    const float aspect = float(renderer_->width()) / float(std::max<uint32_t>(1, renderer_->height()));
    const Mat4 viewProjection = multiply(camera_.projectionMatrix(aspect), camera_.viewMatrix());
    {
        // Toggles that change the placed-light shadow casters redraw the kept maps (see invalidateShadowMaps).
        uint64_t signature = uint64_t(showEntities) | uint64_t(showServer) << 1 | uint64_t(showTerrain) << 2 | uint64_t(localShadowTerrain) << 3 |
                             uint64_t(renderer_->cullBackFaces) << 4 | uint64_t(uint32_t(onlyEntity + 1)) << 8;
        for (const auto& g : garrisonViews) signature = signature * 1000003u ^ (uint64_t(g.first) * 31u + uint64_t(uint32_t(g.second)));
        if (signature != shadowCasterSignature_) {
            shadowCasterSignature_ = signature;
            invalidateShadowMaps();
        }
    }
    frameLighting_ = lighting();
    renderer_->beginFrame(viewProjection, camera_.position, frameLighting_);
    renderer_->setCameraAxes(camera_.right(), camera_.up(), camera_.forward());
    renderer_->setProjection(camera_.projectionMatrix(aspect));
    {
        const auto cube = environmentCubes_.find(activeEnvironmentMap());
        renderer_->setEnvironmentMap(cube == environmentCubes_.end() || !environmentMaps ? nullptr : cube->second);
    }

    // Frustum test against the clip-space planes of viewProjection (Gribb/Hartmann, depth 0..1).
    const Vec4f& r0 = viewProjection.row0; const Vec4f& r1 = viewProjection.row1;
    const Vec4f& r2 = viewProjection.row2; const Vec4f& r3 = viewProjection.row3;
    const Vec4f planes[5] = {
        {r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w}, {r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w},
        {r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w}, {r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w},
        {r2.x, r2.y, r2.z, r2.w}};
    auto visible = [&](const tr::Vec3& c, float radius) {
        for (const Vec4f& p : planes) {
            const float n = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            if ((p.x * c.x + p.y * c.y + p.z * c.z + p.w) / n < -radius) return false;
        }
        return true;
    };
    const auto terrainStart = std::chrono::steady_clock::now();
    if (showTerrain && !terrainChunks_.empty()) queueTerrain(visible);
    queueTerrainMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - terrainStart).count();

    // Map entities and server objects are lit and tinted alike; only the selection is highlighted.
    auto modelTint = [&](bool selected) {
        return selected ? Vec4f{1.0f, 0.9f, 0.45f, 1} : showTextures ? Vec4f{1, 1, 1, 1} : Vec4f{0.8f, 0.8f, 0.8f, 1};
    };
    auto scaledTint = [](const Vec4f& t, float s) { return Vec4f{t.x * s, t.y * s, t.z * s, t.w}; };   // VertColor x2
    // Additive pieces; holograms (aniso) use the game's own material when textures are shown.
    struct SkinScope {                 // GPU skinning applies to the piece being drawn only
        Renderer& renderer;
        ~SkinScope() { renderer.clearSkin(); }
    };
    // A forward-phase piece's TexCoordTransforms: the stored ones, replaced by the model animation's material tracks.
    auto forwardTransforms = [&](const Model* model, const ModelPiece& piece) {
        TrMaterial m = piece.forwardUv;
        UvTransforms uv{m.transform0u, m.transform0v, m.transform1u, m.transform1v, m.transform2u, m.transform2v};
        if (model && animatedUvTransforms(*model, piece, lightTime, uv)) {
            m.transform0u = uv.u0; m.transform0v = uv.v0; m.transform1u = uv.u1; m.transform1v = uv.v1;
            m.transform2u = uv.u2; m.transform2v = uv.v2;
        }
        return m;
    };
    auto drawEffect = [&](const Model* model, const ModelPiece& piece, const Mat4& world, const Mat4& placementWorld) {   // model: material tracks
        SkinScope skinScope{*renderer_};
        setPieceSkin(piece);
        if (piece.aniso && showTextures) {
            AnisoMaterial m = piece.anisoMaterial;
            m.normalMatrix = anisoNormalMatrix(model ? model : piece.skinOwner, piece, placementWorld, world);
            UvTransforms uv{m.transform0u, m.transform0v, m.transform1u, m.transform1v, m.transform2u, m.transform2v};
            if (model && animatedUvTransforms(*model, piece, lightTime, uv)) {
                m.transform0u = uv.u0; m.transform0v = uv.v0; m.transform1u = uv.u1; m.transform1v = uv.v1;
                m.transform2u = uv.u2; m.transform2v = uv.v2;
            }
            renderer_->drawAnisoAdditive(piece.mesh, world, m, !piece.twoSided);
        }
        else if (piece.forwardLit) {
            const TrMaterial uv = forwardTransforms(model, piece);
            renderer_->drawAdditive(piece.mesh, world, scaledTint(Vec4f{1, 1, 1, piece.matDiffuseAlpha}, piece.colourScale), showTextures ? piece.texture : nullptr,
                                   true, showTextures ? piece.detail : nullptr, piece.detailSet, !piece.twoSided, &uv);
        }
        else renderer_->drawAdditive(piece.mesh, world, scaledTint(Vec4f{1, 1, 1, 1}, piece.colourScale), showTextures ? piece.texture : nullptr, false, nullptr,
                                    false, !piece.twoSided);
    };
    auto waterTint = [](bool selected) { return Vec4f{0.22f, 0.42f, 0.58f, selected ? 0.75f : 0.55f}; };

    // Debug (hiddenGroup): "<group>" hides that GPCE group, "!<group>" draws only that group.
    auto groupHidden = [&](const ModelPiece& piece) {
        if (hiddenGroup.empty()) return false;
        if (hiddenGroup[0] == '!') return piece.group != hiddenGroup.substr(1);
        return piece.group == hiddenGroup;
    };
    // A placed model's pieces, as map entities draw them (entityHue: the entity's hue when set, w = 1).
    // fading: the body is in the game's distance fade band (FUN_00856c00: 0.01 < alpha < 0.999). The engine then sets MatDiffuse.a to
    // the alpha and moves Opaque pieces to the Translucent phase (FUN_007ffd80 / FUN_007ffb90), drawn with their forward technique.
    // Verified in the fountain capture (tabula_rasa.2.trace, 2026-10-14) for the TRDiffMapDiffMap* effects - the four fading bodies
    // there (770, 772, 1105, 1106), each effect with one static RenderMode 0 technique: blended SRCALPHA / INVSRCALPHA, no depth
    // writes, CULLMODE CCW, fog on; vertex shader: hemi + gDirLight[0] N.L only (no point lights) x vertex colour; pixel shader:
    // lerp(diffuse, modifier, vertex alpha) [+ decal / detail] x 2 x light, alpha = lerp(diffuse.a, modifier.a, vertex alpha) -
    // MatDiffuse.a is not read, so these bodies show their textures' alpha, not a fade. Other effects have several forward techniques
    // whose runtime choice is not decoded (the engine validates them, FUN_005c1d40); their pieces stay opaque here (UNVERIFIED).
    auto drawModelPieces = [&](const Model& model, const Mat4& placementWorld, const Vec4f& entityHue, bool selected, float fade = 1.0f) {
        const bool fading = fade < 0.999f;
        const std::vector<Mat4> animatedMatrices = animatedPieceMatrices(model, lightTime);
        auto pieceWorld = [&](const ModelPiece& piece) {
            return piece.animBone >= 0 && size_t(piece.animBone) < animatedMatrices.size() ? multiply(placementWorld, animatedMatrices[size_t(piece.animBone)]) : placementWorld;
        };
        if (showEffects) {     // additive pieces: glows, holograms, light cones; billboard sprites
            for (const ModelPiece& piece : model.pieces)
                if (piece.additive && !groupHidden(piece)) drawEffect(&model, piece, pieceWorld(piece), placementWorld);
            if (!model.billboards.empty()) queueBillboards(model, placementWorld, animatedMatrices, frameLighting_);
        }
        if (!model.hasSolid) return;
        const Vec4f tint = modelTint(selected);
        SkinScope skinScope{*renderer_};
        for (const ModelPiece& piece : model.pieces) {
            if (piece.additive || groupHidden(piece)) continue;
            setPieceSkin(piece);
            const Mat4 world = pieceWorld(piece);
            if (piece.refract) {
                if (showTextures && showEffects) {
                    RefractMaterial m = piece.refractMaterial;
                    UvTransforms uv{m.transform0u, m.transform0v, m.transform1u, m.transform1v, Vec4f{1, 0, 0, 0}, Vec4f{0, 1, 0, 0}};
                    if (animatedUvTransforms(model, piece, lightTime, uv)) {
                        m.transform0u = uv.u0; m.transform0v = uv.v0; m.transform1u = uv.u1; m.transform1v = uv.v1;
                    }
                    renderer_->drawRefract(piece.mesh, world, m);
                }
                continue;
            }
            if (piece.water && piece.waterShader && showTextures && !selected) {
                WaterMaterial m = piece.waterMaterial;
                UvTransforms uv{m.transform[0], m.transform[1], m.transform[2], m.transform[3], m.transform[4], m.transform[5], m.transform[6], m.transform[7]};
                if (animatedUvTransforms(model, piece, lightTime, uv)) {
                    m.transform[0] = uv.u0; m.transform[1] = uv.v0; m.transform[2] = uv.u1; m.transform[3] = uv.v1;
                    m.transform[4] = uv.u2; m.transform[5] = uv.v2; m.transform[6] = uv.u3; m.transform[7] = uv.v3;
                }
                renderer_->drawWater(piece.mesh, world, m);
            } else if (piece.water) renderer_->drawTranslucent(piece.mesh, world, waterTint(selected));
            else if (piece.translucent) {
                const TrMaterial uv = forwardTransforms(&model, piece);
                renderer_->drawTranslucent(piece.mesh, world, scaledTint(Vec4f{tint.x, tint.y, tint.z, tint.w * piece.matDiffuseAlpha}, piece.colourScale),
                                          showTextures ? piece.texture : nullptr, showTextures ? piece.detail : nullptr, piece.detailSet, !piece.twoSided,
                                          piece.forwardLit ? &uv : nullptr);
            }
            else {
                Vec4f hue = !showTextures || !piece.hued ? Vec4f{0, 0, 0, 0} : entityHue.w > 0.5f ? entityHue : piece.materialHue;
                if (hue.w > 0.5f && piece.hueUnweighted) hue.w = 2.0f;
                TrMaterial trm = piece.trMaterial;
                UvTransforms uv{trm.transform0u, trm.transform0v, trm.transform1u, trm.transform1v, trm.transform2u, trm.transform2v};
                if (trm.active && animatedUvTransforms(model, piece, lightTime, uv)) {
                    trm.transform0u = uv.u0; trm.transform0v = uv.v0; trm.transform1u = uv.u1; trm.transform1v = uv.v1;
                    trm.transform2u = uv.u2; trm.transform2v = uv.v2;
                }
                // RASA_FADEMODES=0 (viewport_test): only the TRDiffMapDiffMap* rule (mode 1), as before 2026-10-20.
                static const bool onlyMode1 = [] { const char* v = std::getenv("RASA_FADEMODES"); return v && *v == '0'; }();
                if (fading && piece.fadeAlphaMode != 0 && (!onlyMode1 || piece.fadeAlphaMode == 1) && showTextures && materialMaps && trm.active) {
                    // MatDiffuse.a = the fade x the material's own alpha (FUN_007ffd80); the mode picks how the shader uses it.
                    const Vec4f fadeTint = scaledTint(Vec4f{tint.x, tint.y, tint.z, tint.w * piece.matDiffuseAlpha * fade}, piece.colourScale);
                    renderer_->drawFadingForward(piece.mesh, world, fadeTint, piece.texture, trm, !piece.twoSided, piece.fadeAlphaMode,
                                                 piece.alphaTest ? piece.alphaReference : 0.0f);
                    ++fadingForwardPieces_;
                    continue;
                }
                if (showLightBuffer && piece.lavaForward) continue;       // forward-only: not in the game's light buffer
                Vec4f drawTint = scaledTint(tint, piece.colourScale);
                if (fading && piece.palFading && !onlyMode1 && showTextures && materialMaps) {
                    // Pal* forward: translucent with the full Pal material; alpha = MatDiffuse.a (the fade x the material's alpha)
                    // x gBlendFade (its runtime value is NOT traced: 1).
                    renderer_->setNextFading(6);
                    drawTint.w = tint.w * piece.matDiffuseAlpha * fade;
                    ++fadingForwardPieces_;
                }
                renderer_->draw(piece.mesh, world, drawTint, true, showTextures ? piece.texture : nullptr, showTextures && piece.alphaTest,
                                hue, showTextures ? piece.glow : nullptr, piece.twoSided, piece.alphaReference,
                                showTextures && materialMaps ? piece.normalMap : nullptr, showTextures && materialMaps ? piece.glossMap : nullptr,
                                showTextures && materialMaps && piece.environment,
                                showTextures && materialMaps && trm.active ? &trm : nullptr,
                                showTextures && piece.hairCutout, showTextures && materialMaps ? piece.palDecal : nullptr);
            }
        }
    };

    endSection(0);
    drawnEntities_ = 0;
    fadingForwardPieces_ = 0;
    if (showEntities) {
        const float maxDistance2 = drawDistance * drawDistance;
        for (size_t k = 0; k < placements_.size(); ++k) {
            const Placement& p = placements_[k];
            if (onlyEntity >= 0 && int(k) != onlyEntity) continue;
            if (entityFirst >= 0 && (int(k) < entityFirst || int(k) >= entityLast)) continue;
            if (gameDistances && int(k) != selectedEntity && entityDistanceAlpha(p) <= 0.01f) continue;     // culled by the game
            const tr::Vec3 d = subtract(p.centre, camera_.position);
            if (dot(d, d) > maxDistance2 + p.radius * p.radius * 4 && int(k) != selectedEntity) continue;
            if (!visible(p.centre, p.radius)) continue;
            ++drawnEntities_;
            const bool selected = int(k) == selectedEntity;
            const float fade = gameDistances ? entityDistanceAlpha(p) : 1.0f;
            if (p.model && p.model->loaded) drawModelPieces(*p.model, p.world, p.hue, selected, selected ? 1.0f : fade);
            if (p.model && p.model->loaded && p.model->hasSolid) {
                // drawn above
            } else if (p.model && p.model->loaded && p.model->effectOnly) {
                // Effect-only entity (particle emitters, light cones): with Placeholders on, a cyan wireframe of its bounds.
                if (showEffects && showPlaceholders && p.hasBounds && !selected) renderer_->draw(unitBoxLines_, p.boxWorld, markerColour::placeholder, false);
            } else if (showPlaceholders && p.hasBounds) {
                // Wireframe, so large invisible volumes don't hide the scene.
                // Red: unknown class; orange: model failed to load or is missing; blue: class has no model.
                const Vec4f tint = !p.classKnown ? Vec4f{0.95f, 0.3f, 0.25f, 1} : p.model ? Vec4f{0.95f, 0.6f, 0.2f, 1} : Vec4f{0.45f, 0.6f, 1.0f, 1};
                if (!selected) renderer_->draw(unitBoxLines_, p.boxWorld, tint, false);
            }
            if (selected) renderer_->draw(unitBoxLines_, p.boxWorld, Vec4f{1.0f, 0.85f, 0.1f, 1}, false);
        }
    }

    // Placed-light shadow casters (the lights given shadow views in lighting()): the map's bodies (not on culling layer 6) and terrain
    // chunks within each light's range and not wholly behind it, whatever the camera sees - the game renders a light's map from the
    // light (once, for a static light). Characters only for non-static lights: a static light's caster filter drops actors
    // (TRasa::GraphicsUtil::DropActorsObjs, switched by + 0x339 = IsStatic - see tr::PlacedLight::isStatic). Their opaque pieces
    // only, as the main pass draws them (alpha cut-outs kept).
    lastActorShadowCasters = 0;
    lastShadowMapsDrawn = 0;
    if (frameLighting_.localShadowCount > 0) {
        // One entry per shadow light (localShadowBase z = its mask bit): where its casters can be - its reach sphere (a spot's range,
        // a plane's box radius in localPosition.w, a point's range) and, for spots and planes, in front of it (a spot's axis; a plane
        // shines along its +z, the light's axis is not kept here, so planes test the sphere only).
        struct ShadowSlot {
            tr::Vec3 position;
            float range = 0;
            tr::Vec3 axis;
            bool directed = false;
            bool used = false;
            bool actors = false;     // a non-static light: characters cast into it
        };
        std::vector<ShadowSlot> slots(32);
        lastShadowMapsDrawn = 0;
        for (int i = 0; i < frameLighting_.localCount; ++i) {
            const Vec4f& base = frameLighting_.localShadowBase[i];
            if (base.x < 0.5f) continue;
            const size_t s = size_t(base.z + 0.5f);
            if (s >= slots.size()) continue;
            if (((frameLighting_.localShadowRedraw >> uint32_t(s)) & 0x1) == 0) continue;     // kept from an earlier frame: no casters
            ++lastShadowMapsDrawn;
            if (s < frameShadowEntries_.size()) {
                ShadowCacheEntry& e = shadowCache_[renderer_].entries[size_t(frameShadowEntries_[s])];
                e.pendingVersion = shadowCasterVersion_;
                e.pendingPass = renderer_->localShadowPassesRecorded();
            }
            const Vec4f& p = frameLighting_.localPosition[i];
            const Vec4f& a = frameLighting_.localSpot[i];
            slots[s] = ShadowSlot{tr::Vec3{p.x, p.y, p.z}, p.w, tr::Vec3{a.x, a.y, a.z}, a.w > -1.5f, true, base.w > 0.5f};
        }
        uint32_t actorLights = 0;
        for (size_t s = 0; s < slots.size(); ++s)
            if (slots[s].used && slots[s].actors) actorLights |= 1u << s;
        auto maskFor = [&](const tr::Vec3& centre, float radius) {
            uint32_t mask = 0;
            for (size_t s = 0; s < slots.size(); ++s) {
                if (!slots[s].used) continue;
                const tr::Vec3 d = subtract(centre, slots[s].position);
                if (length(d) - radius > slots[s].range || (slots[s].directed && dot(d, slots[s].axis) < -radius)) continue;
                mask |= 1u << s;
            }
            return mask;
        };
        if (showEntities)
            for (size_t k = 0; k < placements_.size(); ++k) {
                const Placement& p = placements_[k];
                if (p.neverDrawn || !p.model || !p.model->loaded || !p.model->hasSolid) continue;
                const uint32_t mask = maskFor(p.centre, p.radius);
                if (mask == 0) continue;
                const std::vector<Mat4> animated = animatedPieceMatrices(*p.model, lightTime);
                SkinScope skinScope{*renderer_};
                for (const ModelPiece& piece : p.model->pieces) {
                    if (piece.additive || piece.refract || piece.water || piece.translucent || groupHidden(piece)) continue;
                    setPieceSkin(piece);
                    const Mat4 world = piece.animBone >= 0 && size_t(piece.animBone) < animated.size() ? multiply(p.world, animated[size_t(piece.animBone)]) : p.world;
                    renderer_->drawLocalShadowCaster(piece.mesh, world, piece.alphaTest && piece.texture ? piece.texture : nullptr, piece.alphaReference, false,
                                                     piece.twoSided, mask);
                }
            }
        // Server objects cast too: the firing-range flashlight's map in the trace holds the two ambient soldiers (skinned draws at their
        // positions - their model carries the targets). Spawn-pool creatures and avatars are actors here - only non-static lights take
        // them; ambient figures, usables, destructibles, teleporters and other objects are not actors. Posed with their current idle,
        // as drawn; an avatar as assembled (base suit minus the swap sets its loaded parts replace, the parts, its body accessories).
        if (showServer)
            for (const ServerPlacement& s : serverPlacements_) {
                if (s.area || !serverPlacementShown(s) || garrisonSuspended(s)) continue;
                const bool actor = s.selection.kind == SelectionKind::SpawnPool || !s.avatarParts.empty() || s.skinHue.w > 0.5f;
                if ((actor && actorLights == 0) || !s.model || !s.model->loaded || !s.model->hasSolid || serverModelCulled(s)) continue;
                const tr::Vec3 centre = transformPoint(s.world, scaled(add(s.model->bounds.min, s.model->bounds.max), 0.5f));
                const uint32_t mask = maskFor(centre, length(subtract(s.model->bounds.max, s.model->bounds.min)) * 0.5f) & (actor ? actorLights : ~0u);
                if (mask == 0) continue;
                struct IdleScope {
                    SceneView& view;
                    ~IdleScope() { view.skinOverride_.clear(); }
                } idleScope{*this};
                if (actor) ++lastActorShadowCasters;
                const bool idling = poseIdle(s);
                auto casterPieces = [&](const Model& model, const Mat4& placement, auto&& keep) {
                    const std::vector<Mat4> animated = animatedPieceMatrices(model, lightTime);
                    SkinScope skinScope{*renderer_};
                    for (const ModelPiece& piece : model.pieces) {
                        if (piece.additive || piece.refract || piece.water || piece.translucent || groupHidden(piece) || !keep(piece)) continue;
                        setPieceSkin(piece);
                        const Mat4 world = piece.animBone >= 0 && size_t(piece.animBone) < animated.size() ? multiply(placement, animated[size_t(piece.animBone)]) : placement;
                        renderer_->drawLocalShadowCaster(piece.mesh, world, piece.alphaTest && piece.texture ? piece.texture : nullptr, piece.alphaReference, false,
                                                         piece.twoSided, mask);
                    }
                };
                const auto all = [](const ModelPiece&) { return true; };
                if (!s.avatarParts.empty()) {
                    uint32_t replaced = 0;
                    for (const AvatarDraw& part : s.avatarParts)
                        if (part.model && part.model->loaded && !part.model->pieces.empty() && part.swapsetId < 32) replaced |= 1u << part.swapsetId;
                    casterPieces(*s.model, s.world, [&](const ModelPiece& piece) { return piece.swapId >= 32 || ((replaced >> piece.swapId) & 0x1) == 0; });
                    for (const AvatarDraw& part : s.avatarParts)
                        if (part.model && part.model->loaded)
                            casterPieces(*part.model, s.world, [&](const ModelPiece& piece) { return piece.swapId == part.swapsetId; });
                } else {
                    casterPieces(*s.model, s.world, all);
                }
                for (const Model* accessory : s.accessories)
                    if (accessory && accessory->loaded && accessory->hasSolid) casterPieces(*accessory, s.world, all);
                for (const ServerPlacement::BodyAccessory& a : s.bodyAccessories) {
                    Mat4 world;
                    if (a.model && a.model->loaded && a.model->hasSolid && bodyAccessoryWorld(s, a, idling ? &idlePosed_ : nullptr, world))
                        casterPieces(*a.model, world, all);
                }
            }
        if (showTerrain && localShadowTerrain)
            for (TerrainChunk& chunk : terrainChunks_) {
                const uint32_t mask = maskFor(chunk.centre, chunk.radius);
                if (mask == 0) continue;
                const GpuMesh* mesh = nullptr;
                for (uint32_t level = 0; level < 4 && !mesh; ++level)
                    if (chunkLevel(chunk, level).vertices) mesh = &chunkLevel(chunk, level);
                if (mesh) renderer_->drawLocalShadowCaster(*mesh, identityMatrix(), nullptr, 0.0f, true, true, mask);
            }
    }

    endSection(1);
    // Terrain clutter (casts sun shadows: in-game /dl_showlight at Concordia noon shows grass and flower shadows).
    drawnClutter_ = 0;
    if (showClutter && showTerrain && map_.hasTerrain) {
        updateClutterCells();
        for (const ClutterCell& cell : clutterCells_) {
            if (cell.groups.empty() || !visible(cell.centre, cell.radius + 5.0f)) continue;
            const float cellDistance = length(subtract(cell.centre, camera_.position));
            for (const ClutterGroup& group : cell.groups) {
                if (!group.model || !group.model->loaded || cellDistance > group.drawDistance + cell.radius) continue;
                const tr::Vec3 localCentre = scaled(add(group.model->bounds.min, group.model->bounds.max), 0.5f);
                const float localRadius = length(subtract(group.model->bounds.max, group.model->bounds.min)) * 0.5f;
                // The instances are in the container's shuffled order; the first trunc(N x density) are drawn.
                const auto shown = size_t(double(group.instances.size()) * double(clutterDensity));
                for (size_t k = 0; k < shown && k < group.instances.size(); ++k) {
                    const ClutterInstance& c = group.instances[k];
                    if (!visible(transformPoint(c.world, localCentre), localRadius * c.scale)) continue;
                    drawModelPieces(*group.model, c.world, Vec4f{0, 0, 0, 0}, false);
                    ++drawnClutter_;
                }
            }
        }
    }

    endSection(2);
    // A standalone system (the Assets preview of a .pfx).
    if (standalone_ && showParticles) {
        const tr::Vec3 right = camera_.right(), up = camera_.up();
        const tr::ParticleSystem& system = standalone_->system();
        std::vector<size_t> order(system.groups.size());
        for (size_t g = 0; g < order.size(); ++g) order[g] = g;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return system.groups[a].zBias < system.groups[b].zBias; });
        for (size_t g : order) {
            particleScratch_.clear();
            standalone_->buildQuads(g, right, up, particleScratch_);
            if (particleScratch_.empty()) continue;
            const tr::ParticleGroup& group = system.groups[g];
            renderer_->drawParticles(particleScratch_, showTextures && !group.texture.empty() ? texture(group.texture) : nullptr, tr::particleBlendType(group));
        }
    }
    // Particle systems (camera-facing sprites need the view's axes), drawn far to near by the engine's sort key for a group
    // (FUN_0066f650, traced 2026-10-20): depth x (1 - ZBias x 1e-5) - so of the groups of one system, those with a higher ZBias
    // draw later, on top. The depth is taken as the distance from the eye to the system's origin (which depth the engine stores
    // at + 0x60 is NOT traced); equal keys keep the system's group order.
    if (showEntities && showParticles) {
        const tr::Vec3 right = camera_.right(), up = camera_.up();
        struct GroupDraw {
            const ParticleInstance* instance;
            size_t group;
            float key;
        };
        std::vector<GroupDraw> draws;
        for (const LiveParticles& l : liveParticles_) {
            // In view by where its particles are (columns and beams reach far above their emitter - the control-point flag's
            // beam vanished when looking up past the pole).
            tr::Vec3 centre;
            float radius = 0;
            if (!l.instance->worldBounds(centre, radius) || !visible(centre, radius)) continue;
            const Mat4& w = l.instance->world();
            const float depth = length(subtract(tr::Vec3{w.row0.w, w.row1.w, w.row2.w}, camera_.position));
            const tr::ParticleSystem& system = l.instance->system();
            for (size_t g = 0; g < system.groups.size(); ++g)
                draws.push_back(GroupDraw{l.instance.get(), g, depth * (1.0f - float(system.groups[g].zBias) * 1e-5f)});
        }
        std::stable_sort(draws.begin(), draws.end(), [](const GroupDraw& a, const GroupDraw& b) { return a.key > b.key; });
        for (const GroupDraw& d : draws) {
            particleScratch_.clear();
            d.instance->buildQuads(d.group, right, up, particleScratch_);
            if (particleScratch_.empty()) continue;
            const tr::ParticleGroup& group = d.instance->system().groups[d.group];
            renderer_->drawParticles(particleScratch_, showTextures && !group.texture.empty() ? texture(group.texture) : nullptr, tr::particleBlendType(group));
        }
    }

    endSection(3);
    queueSky();
    endSection(4);

    // Server content (World database).
    const Vec4f highlight = markerColour::selected;
    const float maxDistance2 = drawDistance * drawDistance;
    for (const ServerPlacement& s : serverPlacements_) {
        const SelectionKind kind = s.selection.kind;
        if (kind == SelectionKind::Region ? !showRegions : kind == SelectionKind::MissionArea ? !showMissionAreas : !showServer) continue;
        const bool selected = sameSelection(s.selection, selection);
        if (!selected && garrisonSuspended(s)) continue;     // the side not shown at its point: nothing drawn
        const Vec4f colour = selected ? highlight : s.colour;
        if (s.area) {
            const tr::Vec3& c = s.shape.position;
            const bool box = s.shape.shape == 2;
            const float rx = box ? s.shape.halfX : s.shape.radius, rz = box ? s.shape.halfZ : s.shape.radius;
            Mat4 m = identityMatrix();
            m.row0 = {rx, 0, 0, c.x};
            m.row1 = {0, 1, 0, c.y + 0.3f};
            m.row2 = {0, 0, rz, c.z};
            renderer_->draw(box ? unitSquare_ : unitCircle_, m, colour, false);
            Mat4 pole = identityMatrix();
            pole.row0 = {1, 0, 0, c.x};
            pole.row1 = {0, 25, 0, c.y};
            pole.row2 = {0, 0, 1, c.z};
            renderer_->draw(pole_, pole, colour, false);
            continue;
        }
        // The link line (pole_ stretched from the marker to its target) is tested on its own: it stays in view when the marker
        // is behind the camera or beyond the draw distance (e.g. standing at a control point with its linked pools around).
        if (s.linked && (!s.pointLink || selected || sameSelection(s.pointSelection, selection))) {
            const tr::Vec3 l = subtract(s.linkTarget, s.position);
            if (visible(add(s.position, scaled(l, 0.5f)), length(l) * 0.5f + 0.5f)) {
                Mat4 line = identityMatrix();
                line.row0 = {1, l.x, 0, s.position.x};
                line.row1 = {0, l.y, 0, s.position.y + 0.3f};
                line.row2 = {0, l.z, 1, s.position.z};
                renderer_->draw(pole_, line, colour, false);
            }
        }
        const tr::Vec3 d = subtract(s.position, camera_.position);
        if (!selected && dot(d, d) > maxDistance2) continue;
        // In view: the marker / ring, or any loaded model drawn at the placement (its own and the use-state accessories - e.g. the
        // Alia Das waypoint's field column, tens of metres tall, which vanished when only its foot was tested).
        bool inView = visible(s.position, std::max(s.ringRadius, 3.0f));
        auto modelInView = [&](const Model* m) {
            if (inView || !m || !m->loaded) return;
            const tr::Vec3 centre = transformPoint(s.world, scaled(add(m->bounds.min, m->bounds.max), 0.5f));
            inView = visible(centre, length(subtract(m->bounds.max, m->bounds.min)) * 0.5f);
        };
        modelInView(s.model);
        for (const Model* accessory : s.accessories) modelInView(accessory);
        if (!inView) continue;
        const bool hasModel = s.model && s.model->loaded && (s.model->hasSolid || s.model->effectOnly) && (selected || !serverModelCulled(s));
        if (hasModel) {
            const Vec4f tint = modelTint(selected);
            struct IdleScope {                  // the creature's idle palettes apply to this placement only
                SceneView& view;
                ~IdleScope() { view.skinOverride_.clear(); }
            } idleScope{*this};
            const bool idling = poseIdle(s);
            Mat4 drawWorld = s.world;          // accessories on connection points draw elsewhere
            auto drawPiece = [&](const ModelPiece& piece, const Vec4f& partHue) {
                SkinScope skinScope{*renderer_};
                setPieceSkin(piece);
                if (piece.additive) {
                    if (showEffects) drawEffect(nullptr, piece, drawWorld, drawWorld);   // avatar parts come from several models: no material tracks
                    return;
                }
                if (piece.water) {
                    renderer_->drawTranslucent(piece.mesh, drawWorld, waterTint(selected));
                    return;
                }
                if (piece.translucent) {
                    renderer_->drawTranslucent(piece.mesh, drawWorld, scaledTint(Vec4f{tint.x, tint.y, tint.z, tint.w * piece.matDiffuseAlpha}, piece.colourScale),
                                              showTextures ? piece.texture : nullptr, showTextures ? piece.detail : nullptr, piece.detailSet, !piece.twoSided,
                                              piece.forwardLit ? &piece.forwardUv : nullptr);   // avatar parts: no material tracks
                    return;
                }
                Vec4f hue = !showTextures || !piece.hued ? Vec4f{0, 0, 0, 0} : piece.flesh ? s.skinHue : partHue;
                if (hue.w > 0.5f && piece.hueUnweighted) hue.w = 2.0f;
                renderer_->draw(piece.mesh, drawWorld, scaledTint(tint, piece.colourScale), true, showTextures ? piece.texture : nullptr, showTextures && piece.alphaTest, hue,
                               showTextures ? piece.glow : nullptr, piece.twoSided, piece.alphaReference,
                               showTextures && materialMaps ? piece.normalMap : nullptr, showTextures && materialMaps ? piece.glossMap : nullptr,
                               showTextures && materialMaps && piece.environment,
                               showTextures && materialMaps && piece.trMaterial.active ? &piece.trMaterial : nullptr,
                               showTextures && piece.hairCutout, showTextures && materialMaps ? piece.palDecal : nullptr);
            };
            // Avatars: the base suit minus the swap sets replaced by loaded parts, then each part's swap set
            // from its suit (a part still loading, or failed, leaves the base pieces in place).
            uint32_t replaced = 0;
            for (const AvatarDraw& part : s.avatarParts)
                if (part.model && part.model->loaded && !part.model->pieces.empty() && part.swapsetId < 32) replaced |= 1u << part.swapsetId;
            // Base swap sets with no item: the client calls HueSwapset(swapset, None, skin) for swap sets 1..8 when the
            // creature has a skin (face) colour (creature.py defaults, actor.py _ProcessSwapsets); the engine then gives
            // their non-flesh pieces grey 0.5 (tabula_rasa.exe FUN_004fcc60: an unset hue becomes (0.5, 0.5, 0.5, 1))
            // and flesh pieces the skin colour (FUN_00800340). Other pieces keep no hue here.
            const bool skinned = s.skinHue.w > 0.5f;
            const bool avatar = !s.avatarParts.empty() || skinned;
            if (avatar) {
                for (const ModelPiece& piece : s.model->pieces)
                    if (piece.swapId >= 32 || ((replaced >> piece.swapId) & 0x1) == 0)
                        drawPiece(piece, skinned && piece.swapId >= 1 && piece.swapId <= 8 ? Vec4f{0.5f, 0.5f, 0.5f, 1} : Vec4f{0, 0, 0, 0});
            } else {
                drawModelPieces(*s.model, s.world, Vec4f{0, 0, 0, 0}, selected);   // objects: the same path as map entities
            }
            for (const AvatarDraw& part : s.avatarParts) {
                if (!part.model || !part.model->loaded) continue;
                for (const ModelPiece& piece : part.model->pieces)
                    if (piece.swapId == part.swapsetId) drawPiece(piece, part.hue);
            }
            for (const Model* accessory : s.accessories)
                if (accessory && accessory->loaded) drawModelPieces(*accessory, s.world, Vec4f{0, 0, 0, 0}, selected);
            for (const ServerPlacement::BodyAccessory& a : s.bodyAccessories) {
                if (!bodyAccessoryWorld(s, a, idling ? &idlePosed_ : nullptr, drawWorld)) continue;
                // The appearance colour is not a hue here (traced 2026-10-14): AddAccessory's RGBA reaches FinalizeAddAccessory
                // (4th argument) -> FUN_0081d640, which, unless it is the default (0, 0, 0, 1) at 0x00d088a8, sets it on every
                // piece's parameters of semantic "Diffuse" and "Ambient" (FUN_0081d6f0 -> FUN_005bdc60 = GetParameterBySemantic)
                // - MatDiffuse / MatAmbient, which the Pal deferred G-buffer shaders do not read. So the pieces keep their
                // material's stored Hue (McAllister's pistol colour 0x00000001 made it black before; in game it is grey metal).
                // MatDiffuse.a of TR-effect accessories (alpha in some TR shaders) is not reproduced.
                drawModelPieces(*a.model, drawWorld, Vec4f{0, 0, 0, 0}, selected);       // pieces keep their material Hue
            }
            drawWorld = s.world;
        } else {
            renderer_->draw(unitBoxLines_, multiply(s.world, boxMatrix(tr::Bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 2, 0.5f}})), colour, false);
        }
        if (s.ringRadius > 0) {
            Mat4 ring = identityMatrix();
            ring.row0 = {s.ringRadius, 0, 0, s.position.x};
            ring.row1 = {0, 1, 0, s.position.y + 0.15f};
            ring.row2 = {0, 0, s.ringRadius, s.position.z};
            renderer_->draw(unitCircle_, ring, colour, false);
        }
        if (s.facing) {
            Mat4 arrow = s.world;                       // yaw placement scaled to 3 m, raised slightly
            arrow.row0 = {arrow.row0.x * 3, arrow.row0.y * 3, arrow.row0.z * 3, arrow.row0.w};
            arrow.row1 = {arrow.row1.x * 3, arrow.row1.y * 3, arrow.row1.z * 3, arrow.row1.w + 0.3f};
            arrow.row2 = {arrow.row2.x * 3, arrow.row2.y * 3, arrow.row2.z * 3, arrow.row2.w};
            renderer_->draw(arrow_, arrow, colour, false);
        }
        if (selected && hasModel) renderer_->draw(unitBoxLines_, multiply(s.world, boxMatrix(s.model->bounds)), highlight, false);
    }

    // Map lights (lights.xml): a 0.5 m box at each light; the selected one also shows its reach - three range circles for a
    // point light, the axis out to the range for a spot, the box it lights for a plane light (half extents Width/2, Length/2,
    // Range in its frame, as TRPlaneLight tests).
    if (showLightMarkers || selection.kind == SelectionKind::MapLight) {
        for (size_t k = 0; k < mapLights_.size(); ++k) {
            const bool selected = selection.kind == SelectionKind::MapLight && selection.index == k;
            if (!showLightMarkers && !selected) continue;
            const tr::PlacedLight light = displayedMapLight(k);
            const tr::Vec3 d = subtract(light.position, camera_.position);
            if (!selected && (dot(d, d) > maxDistance2 || !visible(light.position, 0.5f))) continue;
            const Vec4f colour = selected ? highlight : markerColour::light;
            const tr::Vec3& p = light.position;
            renderer_->draw(unitBoxLines_, boxMatrix(tr::Bounds{tr::Vec3{p.x - 0.25f, p.y - 0.25f, p.z - 0.25f},
                                                               tr::Vec3{p.x + 0.25f, p.y + 0.25f, p.z + 0.25f}}), colour, false);
            if (!selected) continue;
            const float r = light.range;
            if (light.kind == tr::PlacedLight::Kind::Point) {
                Mat4 ring = identityMatrix();               // unitCircle_ lies in XZ; also map it onto XY and YZ
                ring.row0 = {r, 0, 0, p.x};
                ring.row1 = {0, 1, 0, p.y};
                ring.row2 = {0, 0, r, p.z};
                renderer_->draw(unitCircle_, ring, colour, false);
                ring.row1 = {0, 0, r, p.y};
                ring.row2 = {0, 1, 0, p.z};
                renderer_->draw(unitCircle_, ring, colour, false);
                ring.row0 = {0, 1, 0, p.x};
                ring.row1 = {r, 0, 0, p.y};
                ring.row2 = {0, 0, r, p.z};
                renderer_->draw(unitCircle_, ring, colour, false);
            } else if (light.kind == tr::PlacedLight::Kind::Spot) {
                const tr::Vec3 axis = scaled(normalized(light.direction()), r);
                Mat4 line = identityMatrix();               // pole_ (0..1 along +y) stretched along the axis
                line.row0 = {1, axis.x, 0, p.x};
                line.row1 = {0, axis.y, 0, p.y};
                line.row2 = {0, axis.z, 1, p.z};
                renderer_->draw(pole_, line, colour, false);
            } else {
                const tr::Vec3 half{light.width * 0.5f, light.length * 0.5f, r};
                Mat4 frame = rotationMatrix(light.worldOrientation);
                frame.row0.w = p.x;
                frame.row1.w = p.y;
                frame.row2.w = p.z;
                renderer_->draw(unitBoxLines_, multiply(frame, boxMatrix(tr::Bounds{tr::Vec3{-half.x, -half.y, -half.z}, half})), colour, false);
            }
        }
    }

    // Collision (showCollision): map entities' walkable surfaces in the game's placement - cyan fill (additive, no depth writes) and
    // edges, red for surfaces the game drops, grey outlines for box / sphere volumes; brighter on the selected entity. A few models'
    // meshes are built a frame.
    if (showCollision) {
        renderer_->clearSkin();
        size_t buildBudget = collisionBuildsPerFrame;
        for (size_t k = 0; k < placements_.size(); ++k) {
            const Placement& p = placements_[k];
            if (!p.model) continue;
            const tr::Vec3 d = subtract(p.centre, camera_.position);
            const bool selected = selection.kind == SelectionKind::MapEntity && selection.index == k;
            if (!selected && (dot(d, d) > maxDistance2 || !visible(p.centre, p.radius))) continue;
            const CollisionMesh* c = collisionFor(p.model, buildBudget);
            if (!c) continue;
            const bool flagged = k < map_.map.entities.size() && (map_.map.entities[k].flags & 0x2) != 0;
            const Vec4f& usedColour = flagged ? markerColour::collisionDropped : markerColour::collision;
            const float fill = selected ? 0.35f : 0.12f;
            auto scaled = [&](const Vec4f& colour) { return Vec4f{colour.x * fill, colour.y * fill, colour.z * fill, 1}; };
            renderer_->drawAdditive(c->used, p.world, scaled(usedColour), nullptr, false, nullptr, 0, true);
            renderer_->draw(c->usedEdges, p.world, selected ? highlight : usedColour, false);
            renderer_->drawAdditive(c->dropped, p.world, scaled(markerColour::collisionDropped), nullptr, false, nullptr, 0, true);
            renderer_->draw(c->droppedEdges, p.world, selected ? highlight : markerColour::collisionDropped, false);
            renderer_->draw(c->ignored, p.world, selected ? highlight : markerColour::collisionIgnored, false);
        }
    }

    // Sound emitters (audioemitters.xml): a 0.4 m box at each; the selected one shows its MinDistance and MaxDistance as spheres
    // (three circles each).
    if (showSoundMarkers || selection.kind == SelectionKind::AudioEmitter) {
        auto sphere = [&](const tr::Vec3& p, float r, const Vec4f& colour) {
            if (r <= 0) return;
            Mat4 ring = identityMatrix();                   // unitCircle_ lies in XZ; also map it onto XY and YZ
            ring.row0 = {r, 0, 0, p.x};
            ring.row1 = {0, 1, 0, p.y};
            ring.row2 = {0, 0, r, p.z};
            renderer_->draw(unitCircle_, ring, colour, false);
            ring.row1 = {0, 0, r, p.y};
            ring.row2 = {0, 1, 0, p.z};
            renderer_->draw(unitCircle_, ring, colour, false);
            ring.row0 = {0, 1, 0, p.x};
            ring.row1 = {r, 0, 0, p.y};
            ring.row2 = {0, 0, r, p.z};
            renderer_->draw(unitCircle_, ring, colour, false);
        };
        for (size_t k = 0; k < audioEmitters_.size(); ++k) {
            const bool selected = selection.kind == SelectionKind::AudioEmitter && selection.index == k;
            if (!showSoundMarkers && !selected) continue;
            const tr::AudioEmitter& e = audioEmitters_[k];
            const tr::Vec3& p = e.position;
            const tr::Vec3 d = subtract(p, camera_.position);
            if (!selected && (dot(d, d) > maxDistance2 || !visible(p, 0.4f))) continue;
            const Vec4f colour = selected ? highlight : markerColour::sound;
            renderer_->draw(unitBoxLines_, boxMatrix(tr::Bounds{tr::Vec3{p.x - 0.2f, p.y - 0.2f, p.z - 0.2f}, tr::Vec3{p.x + 0.2f, p.y + 0.2f, p.z + 0.2f}}),
                           colour, false);
            if (!selected) continue;
            sphere(p, e.minDistance, colour);
            sphere(p, e.maxDistance, Vec4f{colour.x, colour.y, colour.z, 0.5f});
        }
    }

    // The client's map-screen markers (with the Server layer): a 6 m pole and a 1 m ring, a line to the bound object.
    if (showServer) {
        for (const ClientMapMarker& m : clientMapMarkers_) {
            const tr::Vec3 d = subtract(m.position, camera_.position);
            if (dot(d, d) > maxDistance2) continue;
            tr::Vec3 target;
            const bool bound = clientMarkerTarget(m.entityId, target);
            if (!visible(m.position, 6.0f) && !(bound && visible(target, 3.0f))) continue;
            const Vec4f& colour = bound ? markerColour::mapScreenMarker : markerColour::mapScreenMarkerUnbound;
            Mat4 pole = identityMatrix();
            pole.row0 = {1, 0, 0, m.position.x};
            pole.row1 = {0, 6, 0, m.position.y};
            pole.row2 = {0, 0, 1, m.position.z};
            renderer_->draw(pole_, pole, colour, false);
            Mat4 ring = identityMatrix();
            ring.row0 = {1, 0, 0, m.position.x};
            ring.row1 = {0, 1, 0, m.position.y + 6.0f};
            ring.row2 = {0, 0, 1, m.position.z};
            renderer_->draw(unitCircle_, ring, colour, false);
            if (bound) {
                const tr::Vec3 t = subtract(target, m.position);
                Mat4 line = identityMatrix();
                line.row0 = {1, t.x, 0, m.position.x};
                line.row1 = {0, t.y, 0, m.position.y + 0.3f};
                line.row2 = {0, t.z, 1, m.position.z};
                renderer_->draw(pole_, line, colour, false);
            }
        }
    }
    endSection(5);
}

bool SceneView::clientMarkerTarget(int64_t entityId, tr::Vec3& position) const {
    if (!serverContent_ || entityId == 0) return false;
    // A control point's own marker (control_point.marker_entity_id), else a map_marker binding (object kind 1 teleporter, 2 Kraftwerk).
    for (const PlacedObject& o : serverContent_->objects)
        if (o.kind == ObjectKind::ControlPoint && o.controlPoint.markerEntityId == entityId) {
            position = o.position;
            return true;
        }
    for (const MapMarkerBinding& b : serverContent_->mapMarkers) {
        if (b.markerEntityId != entityId) continue;
        if (b.objectKind == 1)
            for (const Teleporter& t : serverContent_->teleporters)
                if (t.id == b.objectId) {
                    position = t.position;
                    return true;
                }
        if (b.objectKind == 2)
            for (const PlacedObject& o : serverContent_->objects)
                if (o.kind == ObjectKind::Kraftwerk && o.id == b.objectId) {
                    position = o.position;
                    return true;
                }
    }
    return false;
}

namespace {

// Moller-Trumbore ray / triangle test; returns the ray parameter or a negative value for a miss.
float rayTriangle(const tr::Vec3& origin, const tr::Vec3& direction, const tr::Vec3& a, const tr::Vec3& b, const tr::Vec3& c) {
    const tr::Vec3 edge1 = subtract(b, a), edge2 = subtract(c, a);
    const tr::Vec3 p = cross(direction, edge2);
    const float determinant = dot(edge1, p);
    if (std::fabs(determinant) < 1e-12f) return -1.0f;
    const float inverse = 1.0f / determinant;
    const tr::Vec3 s = subtract(origin, a);
    const float u = dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const tr::Vec3 q = cross(s, edge1);
    const float v = dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    return dot(edge2, q) * inverse;
}

}  // namespace

namespace {

// Picking a drawn box outline: the ray must pass within a few pixels of one of the box's 12 edges (front or back - the
// back edges are drawn too). Clicks inside the outline go through to whatever is behind it, so a marker box never hides
// the model it stands on. origin/dir are in the box's local space with |dir| = 1 / scale, so the ray parameter t is the
// world distance and local lengths x scale are world lengths; tolerance: 1.2 % of the distance, at least 5 cm.
bool rayHitsBoxOutline(const tr::Vec3& origin, const tr::Vec3& dir, const tr::Vec3& lo, const tr::Vec3& hi, float scale, float& t) {
    bool hit = false;
    float best = 1e30f;
    const float dd = dot(dir, dir);
    if (dd < 1e-18f) return false;
    for (uint32_t axis = 0; axis < 3; ++axis) {               // edges along this axis: 4 per axis
        const uint32_t a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        for (uint32_t corner = 0; corner < 4; ++corner) {
            float start[3], end[3];
            start[axis] = tr::component(lo, axis);
            end[axis] = tr::component(hi, axis);
            start[a1] = end[a1] = (corner & 1) ? tr::component(hi, a1) : tr::component(lo, a1);
            start[a2] = end[a2] = (corner & 2) ? tr::component(hi, a2) : tr::component(lo, a2);
            const tr::Vec3 q0{start[0], start[1], start[2]};
            const tr::Vec3 e = subtract(tr::Vec3{end[0], end[1], end[2]}, q0);
            // Closest points of the ray origin + s dir (s > 0) and the segment q0 + u e (u in 0..1).
            const tr::Vec3 w0 = subtract(origin, q0);
            const float b = dot(dir, e), c = dot(e, e), d = dot(dir, w0), f = dot(e, w0);
            const float denom = dd * c - b * b;
            float sRay = denom > 1e-12f ? (b * f - c * d) / denom : 0.0f;
            float u = c > 1e-12f ? (b * sRay + f) / c : 0.0f;
            u = std::clamp(u, 0.0f, 1.0f);
            sRay = std::max(0.0f, (dot(e, scaled(dir, 1.0f)) * u - d) / dd);
            const tr::Vec3 gap = subtract(add(origin, scaled(dir, sRay)), add(q0, scaled(e, u)));
            const float distance = length(gap) * scale;
            const float tolerance = std::max(0.05f, sRay * 0.012f);
            if (sRay > 0.0f && distance <= tolerance && sRay < best) {
                best = sRay;
                hit = true;
            }
        }
    }
    if (hit) t = best;
    return hit;
}

}  // namespace

Selection SceneView::pick(float ndcX, float ndcY, bool pickEntities, bool pickServer) const {
    const float aspect = float(renderer_->width()) / float(std::max<uint32_t>(1, renderer_->height()));
    const float tanHalf = std::tan(camera_.fovY * 0.5f);
    const tr::Vec3 direction = normalized(add(camera_.forward(), add(scaled(camera_.right(), ndcX * tanHalf * aspect),
                                                                     scaled(camera_.up(), ndcY * tanHalf))));
    // 1. Every entity box the ray crosses (in each entity's local space; the ray parameter t is the
    //    same in local and world space because the transform is affine).
    struct Candidate {
        float t;
        size_t index;
        tr::Vec3 origin;
        tr::Vec3 dir;
    };
    std::vector<Candidate> candidates;
    for (size_t k = 0; k < placements_.size() && showEntities && pickEntities; ++k) {
        const tr::MapEntity& e = map_.map.entities[k];
        const Placement& p = placements_[k];
        const bool hasGeometry = p.model && p.model->loaded && !p.model->triangles.empty();
        const bool isEffect = p.model && p.model->loaded && p.model->effectOnly;
        if (!e.hasBounds || (!hasGeometry && (isEffect ? !showEffects : !showPlaceholders))) continue;
        if (gameDistances && entityDistanceAlpha(p) <= 0.01f) continue;     // not drawn: culled by the game's distance rule
        const Mat4 r = rotationMatrix(e.orientation);
        auto toLocal = [&](const tr::Vec3& v) {            // R^T v
            return tr::Vec3{r.row0.x * v.x + r.row1.x * v.y + r.row2.x * v.z, r.row0.y * v.x + r.row1.y * v.y + r.row2.y * v.z,
                            r.row0.z * v.x + r.row1.z * v.y + r.row2.z * v.z};
        };
        const float inverseScale = e.scale != 0 ? 1.0f / e.scale : 1.0f;
        const tr::Vec3 origin = scaled(toLocal(subtract(camera_.position, e.location)), inverseScale);
        const tr::Vec3 dir = scaled(toLocal(direction), inverseScale);
        bool inside = true;
        float tNear = 0, tFar = 1e30f;
        bool hit = true;
        for (uint32_t axis = 0; axis < 3 && hit; ++axis) {
            const float o = tr::component(origin, axis), d = tr::component(dir, axis);
            const float lo = tr::component(e.bounds.min, axis), hi = tr::component(e.bounds.max, axis);
            if (o < lo || o > hi) inside = false;
            if (std::fabs(d) < 1e-9f) { hit = o >= lo && o <= hi; continue; }
            float t0 = (lo - o) / d, t1 = (hi - o) / d;
            if (t0 > t1) std::swap(t0, t1);
            tNear = std::max(tNear, t0);
            tFar = std::min(tFar, t1);
            hit = tNear <= tFar;
        }
        // A box containing the camera only counts if its geometry is actually hit.
        if (!hit || tNear > drawDistance || (inside && !hasGeometry)) continue;
        candidates.push_back(Candidate{tNear, k, origin, dir});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.t < b.t; });

    // 2. Nearest actual hit: triangles for models, the box itself for placeholders.
    int best = -1;
    float bestT = 1e30f;
    for (const Candidate& c : candidates) {
        if (c.t >= bestT) break;                            // no remaining box can be closer
        const Placement& p = placements_[c.index];
        if (p.model && p.model->loaded && !p.model->triangles.empty()) {
            const std::vector<tr::Vec3>& v = p.model->positions;
            const std::vector<uint32_t>& t = p.model->triangles;
            for (size_t k = 0; k + 2 < t.size(); k += 3) {
                const float hitT = rayTriangle(c.origin, c.dir, v[t[k]], v[t[k + 1]], v[t[k + 2]]);
                if (hitT > 0.0f && hitT < bestT) {
                    bestT = hitT;
                    best = int(c.index);
                }
            }
        } else {
            // Placeholder / effect boxes: by their outline (see rayHitsBoxOutline).
            const tr::MapEntity& e = map_.map.entities[c.index];
            float t = 0;
            if (rayHitsBoxOutline(c.origin, c.dir, e.bounds.min, e.bounds.max, e.scale != 0 ? e.scale : 1.0f, t) && t < bestT) {
                bestT = t;
                best = int(c.index);
            }
        }
    }
    Selection result;
    if (best >= 0) result = Selection{SelectionKind::MapEntity, size_t(best)};
    // Map light markers (0.5 m boxes): by their outline, like the other marker boxes.
    for (size_t k = 0; k < mapLights_.size() && showLightMarkers && pickEntities; ++k) {
        const tr::Vec3 p = displayedMapLight(k).position;
        float t = 0;
        if (rayHitsBoxOutline(subtract(camera_.position, p), direction, tr::Vec3{-0.25f, -0.25f, -0.25f}, tr::Vec3{0.25f, 0.25f, 0.25f}, 1.0f, t) &&
            t < bestT && t < drawDistance) {
            bestT = t;
            result = Selection{SelectionKind::MapLight, k};
        }
    }

    // Sound emitter markers (0.4 m boxes), the same way.
    for (size_t k = 0; k < audioEmitters_.size() && showSoundMarkers && pickEntities; ++k) {
        const tr::Vec3 p = audioEmitters_[k].position;
        float t = 0;
        if (rayHitsBoxOutline(subtract(camera_.position, p), direction, tr::Vec3{-0.2f, -0.2f, -0.2f}, tr::Vec3{0.2f, 0.2f, 0.2f}, 1.0f, t) &&
            t < bestT && t < drawDistance) {
            bestT = t;
            result = Selection{SelectionKind::AudioEmitter, k};
        }
    }

    // 3. Server objects: their model's triangles, else a sphere around the marker / area centre.
    auto raySphere = [&](const tr::Vec3& centre, float radius, float& t) {
        const tr::Vec3 oc = subtract(camera_.position, centre);
        const float b = dot(oc, direction), cc = dot(oc, oc) - radius * radius;
        const float disc = b * b - cc;
        if (disc < 0) return false;
        t = -b - std::sqrt(disc);
        return t > 0;
    };
    for (const ServerPlacement& s : serverPlacements_) {
        const SelectionKind kind = s.selection.kind;
        if (!pickServer) break;
        if (kind == SelectionKind::Region ? !showRegions : kind == SelectionKind::MissionArea ? !showMissionAreas : !showServer) continue;
        if (garrisonSuspended(s)) continue;             // not drawn unselected: picked from the lists
        float t = 1e30f;
        bool hit = false;
        if (s.area) {
            hit = raySphere(tr::Vec3{s.shape.position.x, s.shape.position.y + 2.0f, s.shape.position.z}, 3.0f, t);
        } else if (s.model && s.model->loaded && !s.model->triangles.empty() && !serverModelCulled(s)) {
            const Mat4& w = s.world;                    // yaw rotation, unit scale: local = R^T (p - t)
            auto toLocal = [&](const tr::Vec3& v) {
                return tr::Vec3{w.row0.x * v.x + w.row1.x * v.y + w.row2.x * v.z, w.row0.y * v.x + w.row1.y * v.y + w.row2.y * v.z,
                                w.row0.z * v.x + w.row1.z * v.y + w.row2.z * v.z};
            };
            hit = rayHitsModel(*s.model, toLocal(subtract(camera_.position, s.position)), toLocal(direction), t);
        } else {
            // Model-less markers: their drawn box (1 x 2 x 1 m) by its outline, so a marker on top of an NPC (the
            // Bootcamp hospital respawn point on Doctor Maisha) does not take the clicks meant for the NPC.
            const Mat4& w = s.world;
            auto toLocal = [&](const tr::Vec3& v) {
                return tr::Vec3{w.row0.x * v.x + w.row1.x * v.y + w.row2.x * v.z, w.row0.y * v.x + w.row1.y * v.y + w.row2.y * v.z,
                                w.row0.z * v.x + w.row1.z * v.y + w.row2.z * v.z};
            };
            hit = rayHitsBoxOutline(toLocal(subtract(camera_.position, s.position)), toLocal(direction), tr::Vec3{-0.5f, 0, -0.5f},
                                    tr::Vec3{0.5f, 2, 0.5f}, 1.0f, t);
        }
        // The ring at the marker's foot (drawn at +0.15 m): a click within a few pixels of it selects the marker too.
        if (!s.area && s.ringRadius > 0 && std::fabs(direction.y) > 1e-6f) {
            const float ringY = s.position.y + 0.15f;
            const float tRing = (ringY - camera_.position.y) / direction.y;
            if (tRing > 0.0f && tRing < t) {
                const tr::Vec3 p = add(camera_.position, scaled(direction, tRing));
                const float dx = p.x - s.position.x, dz = p.z - s.position.z;
                const float tolerance = std::max(0.05f, tRing * 0.012f);
                if (std::fabs(std::sqrt(dx * dx + dz * dz) - s.ringRadius) <= tolerance) {
                    t = tRing;
                    hit = true;
                }
            }
        }
        if (hit && t < bestT && t < drawDistance) {
            bestT = t;
            result = s.selection;
        }
    }
    return result;
}

bool SceneView::rayHitsModel(const Model& model, const tr::Vec3& origin, const tr::Vec3& dir, float& t) const {
    // Slab test against the model bounds first.
    float tNear = 0, tFar = 1e30f;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        const float o = tr::component(origin, axis), d = tr::component(dir, axis);
        const float lo = tr::component(model.bounds.min, axis), hi = tr::component(model.bounds.max, axis);
        if (std::fabs(d) < 1e-9f) {
            if (o < lo || o > hi) return false;
            continue;
        }
        float t0 = (lo - o) / d, t1 = (hi - o) / d;
        if (t0 > t1) std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        if (tNear > tFar) return false;
    }
    bool hit = false;
    const std::vector<tr::Vec3>& v = model.positions;
    const std::vector<uint32_t>& tri = model.triangles;
    for (size_t k = 0; k + 2 < tri.size(); k += 3) {
        // With back-face culling, only what is drawn can be hit (e.g. not the outward-facing light barrier box around
        // the Caves of Donn room, seen from inside).
        const bool twoSided = k / 3 < model.triangleTwoSided.size() && model.triangleTwoSided[k / 3] != 0;
        if (renderer_->cullBackFaces && !twoSided && dot(cross(subtract(v[tri[k + 1]], v[tri[k]]), subtract(v[tri[k + 2]], v[tri[k]])), dir) >= 0.0f) continue;
        const float hitT = rayTriangle(origin, dir, v[tri[k]], v[tri[k + 1]], v[tri[k + 2]]);
        if (hitT > 0.0f && hitT < t) {
            t = hitT;
            hit = true;
        }
    }
    return hit;
}

void SceneView::frameTarget(const tr::Vec3& target, float distance) {
    // Keep the heading, look down at the target from a fixed angle (keeping the current pitch could put the
    // camera below the target, i.e. under the terrain).
    camera_.pitch = -0.35f;                       // about 20 degrees down
    tr::Vec3 eye = subtract(target, scaled(camera_.forward(), distance));
    // On terrain maps, keep the camera above the ground and the target in sight - unless the target itself is
    // more than 3 m below the terrain surface (a cave), where the surface is not the ground the target stands on;
    // objects only partly sunk into the ground (rocks, foundations) still count as on the surface.
    float ground = 0;
    const bool targetOnSurface = map_.hasTerrain && map_.terrain.heightAtWorld(target.x, target.z, ground) && target.y >= ground - 3.0f;
    if (targetOnSurface) {
        auto blocked = [&](const tr::Vec3& from) {
            for (int k = 0; k <= 16; ++k) {                // the eye and points along the sight line, short of the target
                const float t = float(k) / 16.0f * 0.9f;
                const tr::Vec3 p = add(from, scaled(subtract(target, from), t));
                float h = 0;
                if (map_.terrain.heightAtWorld(p.x, p.z, h) && p.y < h + (k == 0 ? 1.5f : 0.3f)) return true;
            }
            return false;
        };
        const float lift = 2.0f + distance * 0.05f;   // up to 100 lifts: enough for map-edge cliffs of several hundred metres
        for (int step = 0; step < 100 && blocked(eye); ++step) eye.y += lift;
        const tr::Vec3 to = subtract(target, eye);
        camera_.pitch = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));     // re-aim after raising
    }
    camera_.position = eye;
}

bool SceneView::focusTarget(const Selection& selection, tr::Vec3& target, float& distance) const {
    if (selection.kind == SelectionKind::AudioEmitter) {
        if (selection.index >= audioEmitters_.size()) return false;
        target = audioEmitters_[selection.index].position;
        distance = std::clamp(audioEmitters_[selection.index].maxDistance * 1.5f, 6.0f, 150.0f);
        return true;
    }
    if (selection.kind == SelectionKind::MapLight) {
        if (selection.index >= mapLights_.size()) return false;
        const tr::PlacedLight light = displayedMapLight(selection.index);
        target = light.position;
        distance = std::clamp(light.range * 1.5f, 6.0f, 150.0f);
        return true;
    }
    if (selection.kind == SelectionKind::MapEntity) {
        if (selection.index >= placements_.size()) return false;
        const Placement& p = placements_[selection.index];
        target = p.centre;
        distance = std::max(8.0f, p.radius * 3.0f);
        return true;
    }
    for (const ServerPlacement& s : serverPlacements_) {
        if (!sameSelection(s.selection, selection)) continue;
        target = s.area ? s.shape.position : tr::Vec3{s.position.x, s.position.y + 1.0f, s.position.z};
        const float size = s.area ? std::max(s.shape.radius, std::max(s.shape.halfX, s.shape.halfZ)) : std::max(s.ringRadius, 2.0f);
        distance = std::max(10.0f, size * 2.5f);
        return true;
    }
    return false;
}

bool SceneView::fitEntityToModel(size_t entityIndex) {
    if (entityIndex >= placements_.size()) return false;
    Placement& p = placements_[entityIndex];
    if (!p.model || !p.model->loaded) return false;
    const tr::Bounds& b = p.model->bounds;
    p.hasBounds = true;
    p.boxWorld = multiply(p.world, boxMatrix(b));
    p.centre = transformPoint(p.world, scaled(add(b.min, b.max), 0.5f));
    const float scale = length(subtract(transformPoint(p.world, tr::Vec3{1, 0, 0}), transformPoint(p.world, tr::Vec3{0, 0, 0})));
    p.radius = std::max(0.05f, length(subtract(b.max, b.min)) * 0.5f * scale);
    return true;
}

std::vector<std::pair<std::string, Mat4>> SceneView::debugWorldSkinMatrices(size_t entityIndex, float time) const {
    std::vector<std::pair<std::string, Mat4>> out;
    if (entityIndex >= placements_.size()) return out;
    const Placement& p = placements_[entityIndex];
    if (!p.model || !p.model->loaded || p.model->skeleton.bones.empty()) return out;
    const Model& m = *p.model;
    const float t = m.animation.duration > 0.0f ? std::fmod(std::max(time, 0.0f), m.animation.duration) : 0.0f;
    const std::vector<Mat4> skin = skinMatrices(m.skeleton, m.animation, t, nullptr, nullptr, &m.boneTracks);
    for (size_t k = 0; k < skin.size(); ++k) out.emplace_back(m.skeleton.bones[k].name, multiply(p.world, skin[k]));
    return out;
}

SceneView::Placement SceneView::makePlacement(const tr::MapEntity& e) {
    Placement p;
    p.world = placementMatrix(e.location, e.orientation, e.scale);
    p.hasBounds = e.hasBounds;
    const tr::Bounds bounds = e.hasBounds ? e.bounds : tr::Bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 1, 0.5f}};
    p.boxWorld = multiply(p.world, boxMatrix(bounds));
    const tr::Vec3 localCentre = scaled(add(bounds.min, bounds.max), 0.5f);
    p.centre = transformPoint(p.world, localCentre);
    p.radius = std::max(0.5f, length(subtract(bounds.max, bounds.min)) * 0.5f * e.scale);
    p.classKnown = classKnown_ ? classKnown_(e.classId) : true;
    p.cullingLayer = e.cullLayer != 0 ? e.cullLayer : cullingLayerForClass_ ? cullingLayerForClass_(e.classId) : 3;
    p.neverDrawn = p.cullingLayer == 6;
    const std::string mesh = p.neverDrawn || !meshForClass_ ? std::string() : meshForClass_(e.classId);
    // With the model's own animation, played on the light clock (without it the hologram's skinned pieces collapse into a column).
    p.model = objectModelFor(mesh);      // unordered_map nodes are stable
    tr::HueColour hue1 = e.hue1, hue2 = e.hue2;
    if (classHue_) classHue_(e.classId, hue1, hue2);              // physicalentityclasshue wins when the class has a row (setClassHues)
    p.hue = entityHue(hue1);
    return p;
}

void SceneView::refreshEntity(size_t entityIndex) {
    invalidateShadowMaps();                     // placed-light shadow casters changed
    if (entityIndex >= placements_.size() || entityIndex >= map_.map.entities.size()) return;
    placements_[entityIndex] = makePlacement(map_.map.entities[entityIndex]);
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(),
                                        [&](const LiveParticles& l) { return !l.server && l.placement == entityIndex; }),
                         liveParticles_.end());
}

void SceneView::setEntityAnimation(size_t entityIndex, const std::string& animation) {
    invalidateShadowMaps();                     // placed-light shadow casters changed
    if (entityIndex >= placements_.size() || entityIndex >= map_.map.entities.size()) return;
    refreshEntity(entityIndex);
    if (!animation.empty() && meshForClass_) {
        const std::string mesh = meshForClass_(map_.map.entities[entityIndex].classId);
        if (!mesh.empty()) placements_[entityIndex].model = modelFor(mesh, "@" + animation);
    }
    lightTime = 0;
}

void SceneView::reloadAudioEmitters() {
    audioEmitters_.clear();
    audioEmittersError_.clear();
    if (map_.audioEmittersXml.empty()) return;
    try {
        audioEmitters_ = tr::readAudioEmitters(map_.audioEmittersXml);
    } catch (const std::exception& e) {
        audioEmittersError_ = e.what();
    }
}

void SceneView::reloadMapLights() {
    invalidateShadowMaps();                     // placed-light shadow casters changed
    mapLights_.clear();
    lightModifiers_.clear();
    mapLightsError_.clear();
    if (map_.lightsXml.empty()) return;
    try {
        mapLights_ = tr::readLights(map_.lightsXml);
        lightModifiers_ = tr::readLightModifiers(map_.lightsXml);
        for (const tr::PlacedLight& light : mapLights_)
            if (!light.globeMap.empty() && !globeIndex_.count(tr::toLowerAscii(light.globeMap))) {
                loadGlobeMaps();
                break;
            }
    } catch (const std::exception& e) {
        mapLightsError_ = e.what();
    }
}

void SceneView::rebuildEntities() {
    invalidateShadowMaps();                     // placed-light shadow casters changed
    placements_.clear();
    placements_.reserve(map_.map.entities.size());
    for (const tr::MapEntity& e : map_.map.entities) placements_.push_back(makePlacement(e));
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(), [](const LiveParticles& l) { return !l.server; }),
                         liveParticles_.end());
}

bool SceneView::entityModelBounds(size_t entityIndex, tr::Bounds& bounds) const {
    if (entityIndex >= placements_.size()) return false;
    const Model* m = placements_[entityIndex].model;
    if (!m || !m->loaded) return false;
    bounds = m->bounds;
    return true;
}

bool SceneView::entityBounds(size_t entityIndex, tr::Vec3& centre, float& radius) const {
    if (entityIndex >= placements_.size()) return false;
    centre = placements_[entityIndex].centre;
    radius = placements_[entityIndex].radius;
    return true;
}

void SceneView::focusOn(const Selection& selection) {
    tr::Vec3 target;
    float distance = 0;
    if (focusTarget(selection, target, distance)) frameTarget(target, distance);
}

// An object's animation, from the model itself: the .geo's "anim" property (GBOD property table; 1,097 of 7,240 models name
// one, only 173 of them X.anm for X.geo - e.g. arch_forean_eloh_temple_basin_v02_water_v01.geo -> water_foreas_pond.anm,
// which the game plays: in the fountain trace (2026-10-14) the water's TexCoordTransform1 / 2 equal that animation's EFCT
// keys at one time, t = 21.88 of 23.33 s, and no animationdata table names it). No name matching: the 270 X.geo / X.anm
// pairs without the property (projectiles, ability effects, ...) are not played. Cached per mesh.
std::string SceneView::objectAnimationFor(const std::string& mesh) {
    if (mesh.size() <= 4 || tr::toLowerAscii(mesh.substr(mesh.size() - 4)) != ".geo") return {};
    const auto cached = objectAnimations_.find(mesh);
    if (cached != objectAnimations_.end()) return cached->second;
    std::string animation;
    try {
        const tr::GeoModel geo = tr::readGeo(assets_.read(mesh));
        for (const tr::GeoProperty& property : geo.properties)
            if (tr::toLowerAscii(property.key) == "anim" && assets_.exists(property.value)) animation = property.value;
    } catch (const std::exception&) {
    }
    objectAnimations_[mesh] = animation;
    return animation;
}

// The game's distance fade of a body (tabula_rasa.exe FUN_00856c00, called per body by TRasa::GraphicsUtil::DistanceCullObjs
// FUN_007ff400 and FUN_00856f10; decompiled 2026-10-14, constants read from the exe): the culling layer gives a distance - 2: 100
// + 200 f, 3: 100, 4: 10, 5: 25, 6: never drawn, 7: 50, 8: 100 + 50 f, 9: 100 + 100 f, 10: 100 + 150 f, 11: 75, others: no
// limit (f = RQS "Far CullLayer Scalar") - max = min(distance x max(view scalar, 1), 250); with d = the distance from the eye
// to the bounding sphere's centre and R its radius (x the node scale): opaque while d <= 0.9 max, else alpha = clamp(1 - (d - R
// - 0.9 max) / (0.1 max), 0, 1); at or below 0.01 the body is culled. A fading body (alpha < 0.999) gets MatDiffuse.a = alpha on
// its pieces and moves to the Translucent phase (FUN_008165f0 -> FUN_007ffd80 -> FUN_007ffb90). Checked against the fountain
// trace (2026-10-14) with view scalar 1 and f = 1: no entity predicted culled is drawn, and the four predicted fading in view
// (770, 772, 1105, 1106) are exactly the map objects drawn in the forward pass after the sky. Not reproduced: the fade band's
// look (the forward shaders differ per material; the two checked ignore MatDiffuse.a) - fading entities are drawn opaque.
float SceneView::entityDistanceAlpha(const Placement& p) const { return distanceAlpha(p.cullingLayer, p.centre, p.radius); }

// Rasa.NET ControlPoints (ApplyPools): only the holder's garrison spawns - the pools linked as the other side's (control_point_link
// kind 1 Bane / 2 AFS) are suspended. An open-world point is always held by one side (owner 0 Bane / 1 AFS, from default_owner
// until it changes hands; the server reads anything but 1 as Bane).
bool SceneView::garrisonSuspended(const ServerPlacement& s) const { return garrisonHidden(s.garrisonPoint, s.garrisonKind); }

bool SceneView::garrisonHidden(int64_t pointId, int32_t kind) const {
    if (kind == 0) return false;
    const int view = garrisonViewOf(pointId);
    int32_t shown = view == 1 ? 1 : view == 2 ? 2 : 0;
    if (shown == 0) {
        const auto owner = pointDefaultOwner_.find(pointId);
        shown = owner != pointDefaultOwner_.end() && owner->second == 1 ? 2 : 1;
    }
    return kind != shown;
}

bool SceneView::serverModelCulled(const ServerPlacement& s) const {
    if (!gameDistances || s.cullingLayer == 0 || s.area) return false;
    const float radius = s.model && s.model->loaded ? std::max(0.5f, length(subtract(s.model->bounds.max, s.model->bounds.min)) * 0.5f) : 1.0f;
    return distanceAlpha(s.cullingLayer, tr::Vec3{s.position.x, s.position.y + 1.0f, s.position.z}, radius) <= 0.01f;
}

float SceneView::distanceAlpha(int32_t layer, const tr::Vec3& centre, float radius) const {
    if (layer == 6) return 0.0f;
    const float f = farCullLayerScalar;
    const float layerDistance = layer == 2 ? 100 + 200 * f : layer == 3 ? 100 : layer == 4 ? 10 : layer == 5 ? 25 : layer == 7 ? 50
                              : layer == 8 ? 100 + 50 * f : layer == 9 ? 100 + 100 * f : layer == 10 ? 100 + 150 * f : layer == 11 ? 75 : 0;
    if (layerDistance <= 0) return 1.0f;
    const float maxDistance = std::min(layerDistance * std::max(viewDistanceMultiplier, 1.0f), 250.0f);
    const float start = 0.9f * maxDistance;
    const float d = length(subtract(centre, camera_.position));
    if (d <= start) return 1.0f;
    return std::clamp(1.0f - (d - radius - start) / (maxDistance - start), 0.0f, 1.0f);
}

// Any object's model (map entities, server objects, special-FX accessories, avatar accessories) with its animation played:
// the animation a special-FX package names for it, else the model's own (objectAnimationFor). "@" marks an object animation in
// the model key (creatures' idles are only posed).
SceneView::Model* SceneView::objectModelFor(const std::string& mesh, const std::string& packageAnimation) {
    if (mesh.empty()) return nullptr;
    const std::string animation = packageAnimation.empty() ? objectAnimationFor(mesh) : packageAnimation;
    return modelFor(mesh, animation.empty() ? std::string() : "@" + animation);
}

SceneView::Model* SceneView::modelFor(const std::string& mesh, const std::string& poseAnimation, const std::string& skeletonMesh) {
    // default.geo is the engine's stand-in mesh (system classes: ABV boxes, no-walk shapes, proxies, appearance items);
    // the game never shows it, so such entities are placeholders here.
    if (mesh.empty() || tr::toLowerAscii(mesh) == "default.geo") return nullptr;
    // Posed models are separate entries: "<mesh>|<animation>" (see loadModel).
    std::string key = poseAnimation.empty() ? mesh : mesh + "|" + poseAnimation;
    if (!poseAnimation.empty() && !skeletonMesh.empty() && tr::toLowerAscii(skeletonMesh) != tr::toLowerAscii(mesh)) key += "|" + skeletonMesh;
    key = tr::toLowerAscii(key);
    auto inserted = models_.emplace(key, Model());
    if (inserted.second) pending_.push_back(inserted.first->first);     // loaded progressively
    return &inserted.first->second;
}

void SceneView::refreshServerContent() { setServerContent(serverContent_, serverDatabase_, serverMeshForClass_, serverAvatarFor_); }

void SceneView::viewRay(float ndcX, float ndcY, tr::Vec3& origin, tr::Vec3& direction) const {
    const float aspect = float(renderer_->width()) / float(std::max<uint32_t>(1, renderer_->height()));
    const float tanHalf = std::tan(camera_.fovY * 0.5f);
    origin = camera_.position;
    direction = normalized(add(camera_.forward(), add(scaled(camera_.right(), ndcX * tanHalf * aspect), scaled(camera_.up(), ndcY * tanHalf))));
}

float SceneView::groundHeight(float x, float z, float fallback) const {
    float h = fallback;
    if (map_.hasTerrain && map_.terrain.heightAtWorld(x, z, h)) return h;
    return fallback;
}

bool SceneView::solidTerrainHeight(float x, float z, float& height) const {
    if (!map_.hasTerrain || !map_.terrain.heightAtWorld(x, z, height)) return false;
    // Flag 0 clear = a hole (see "Terrain holes"): the bilinear mix of the four samples' flags, as the vertex alpha interpolates.
    const tr::Terrain& t = map_.terrain;
    const float fc = (x - t.header.bounds.min.x) / t.header.spacing, fr = (z - t.header.bounds.min.z) / t.header.spacing;
    const auto c0 = static_cast<uint32_t>(fc), r0 = static_cast<uint32_t>(fr);
    auto flag = [&](uint32_t r, uint32_t c) { return tr::terrainFlag(t.sampleAt(r, c).word, 0) ? 1.0f : 0.0f; };
    const float tc = fc - float(c0), tr = fr - float(r0);
    const float top = flag(r0, c0) * (1 - tc) + flag(r0, c0 + 1) * tc, bottom = flag(r0 + 1, c0) * (1 - tc) + flag(r0 + 1, c0 + 1) * tc;
    return top * (1 - tr) + bottom * tr >= 0.5f;
}

float SceneView::surfaceHeightBelow(float x, float z, float fromY, float fallback, int excludeEntity) const {
    float terrain = 0;
    const bool solid = solidTerrainHeight(x, z, terrain) && terrain <= fromY;
    const float reach = solid ? fromY - terrain : std::max(drawDistance * 1.5f, 500.0f);
    float t = 0;
    if (modelSurfaceHit(tr::Vec3{x, fromY, z}, tr::Vec3{0, -1, 0}, reach, t, excludeEntity)) return fromY - t;
    return solid ? terrain : fallback;
}

bool SceneView::groundPoint(const tr::Vec3& origin, const tr::Vec3& direction, float fallbackY, tr::Vec3& point) const {
    if (map_.hasTerrain) {
        // March along the ray (finer near the camera), then refine the crossing by bisection. Holes let the ray through.
        float previous = 0.0f;
        const float maxDistance = std::max(drawDistance * 1.5f, 500.0f);
        for (float t = 0.5f; t < maxDistance; t += std::max(0.5f, t * 0.01f)) {
            const tr::Vec3 p = add(origin, scaled(direction, t));
            float h;
            if (!solidTerrainHeight(p.x, p.z, h) || p.y > h) {
                previous = t;
                continue;
            }
            float lo = previous, hi = t;
            for (int k = 0; k < 24; ++k) {
                const float mid = (lo + hi) * 0.5f;
                const tr::Vec3 m = add(origin, scaled(direction, mid));
                float hm;
                if (map_.terrain.heightAtWorld(m.x, m.z, hm) && m.y <= hm) hi = mid;
                else lo = mid;
            }
            point = add(origin, scaled(direction, hi));
            return true;
        }
    }
    if (std::fabs(direction.y) < 1e-6f) return false;
    const float t = (fallbackY - origin.y) / direction.y;
    if (t <= 0) return false;
    point = add(origin, scaled(direction, t));
    return true;
}

bool SceneView::modelSurfaceHit(const tr::Vec3& origin, const tr::Vec3& direction, float maxT, float& t, int excludeEntity) const {
    if (!showEntities) return false;
    float best = maxT;
    bool found = false;
    for (size_t k = 0; k < placements_.size(); ++k) {
        const Placement& p = placements_[k];
        if (!p.model || !p.model->loaded || p.model->triangles.empty() || p.model->effectOnly) continue;
        if (onlyEntity >= 0 && int(k) != onlyEntity) continue;
        if (int(k) == excludeEntity) continue;
        if (gameDistances && entityDistanceAlpha(p) <= 0.01f) continue;     // not drawn
        if (rayHitsEntityModel(k, origin, direction, best)) found = true;
    }
    if (found) t = best;
    return found;
}

// The nearest hit of a ray with one map entity's model triangles before best (updated); false if none.
bool SceneView::rayHitsEntityModel(size_t k, const tr::Vec3& origin, const tr::Vec3& direction, float& best) const {
    const tr::MapEntity& e = map_.map.entities[k];
    const Placement& p = placements_[k];
    if (!p.model || !p.model->loaded || p.model->triangles.empty() || p.model->effectOnly) return false;
    bool found = false;
    // In the entity's local space (scaled; t stays the world-space t of the normalised direction, the transform being affine).
    const Mat4 r = rotationMatrix(e.orientation);
    auto toLocal = [&](const tr::Vec3& v) {
        return tr::Vec3{r.row0.x * v.x + r.row1.x * v.y + r.row2.x * v.z, r.row0.y * v.x + r.row1.y * v.y + r.row2.y * v.z,
                        r.row0.z * v.x + r.row1.z * v.y + r.row2.z * v.z};
    };
    const float inverseScale = e.scale != 0 ? 1.0f / e.scale : 1.0f;
    const tr::Vec3 o = scaled(toLocal(subtract(origin, e.location)), inverseScale);
    const tr::Vec3 d = scaled(toLocal(direction), inverseScale);
    const tr::Bounds& b = p.model->bounds;
    float tNear = 0, tFar = best;
    bool hit = true;
    for (uint32_t axis = 0; axis < 3 && hit; ++axis) {
        const float oa = tr::component(o, axis), da = tr::component(d, axis);
        const float lo = tr::component(b.min, axis), hi = tr::component(b.max, axis);
        if (std::fabs(da) < 1e-9f) { hit = oa >= lo && oa <= hi; continue; }
        float t0 = (lo - oa) / da, t1 = (hi - oa) / da;
        if (t0 > t1) std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        hit = tNear <= tFar;
    }
    if (!hit) return false;
    const std::vector<tr::Vec3>& v = p.model->positions;
    const std::vector<uint32_t>& tri = p.model->triangles;
    for (size_t i = 0; i + 2 < tri.size(); i += 3) {
        const float hitT = rayTriangle(o, d, v[tri[i]], v[tri[i + 1]], v[tri[i + 2]]);
        if (hitT > 0.0f && hitT < best) {
            best = hitT;
            found = true;
        }
    }
    return found;
}

bool SceneView::entityWorldTriangles(size_t k, std::vector<tr::Vec3>& out) const {
    if (k >= placements_.size() || k >= map_.map.entities.size()) return false;
    const Placement& p = placements_[k];
    if (!p.model || !p.model->loaded || p.model->triangles.empty() || p.model->effectOnly || p.neverDrawn) return false;
    const tr::MapEntity& e = map_.map.entities[k];
    const Mat4 world = placementMatrix(e.location, e.orientation, e.scale);
    const std::vector<tr::Vec3>& v = p.model->positions;
    for (uint32_t index : p.model->triangles) out.push_back(transformPoint(world, v[index]));
    return true;
}

bool SceneView::probeHit(const tr::Vec3& origin, const tr::Vec3& direction, float maxT, float& t, const std::vector<uint32_t>& entities) const {
    float best = maxT;
    bool found = false;
    for (uint32_t k : entities)
        if (k < placements_.size() && !placements_[k].neverDrawn && rayHitsEntityModel(k, origin, direction, best)) found = true;
    if (found) t = best;
    return found;
}

bool SceneView::surfacePoint(const tr::Vec3& origin, const tr::Vec3& direction, float fallbackY, tr::Vec3& point, int excludeEntity) const {
    tr::Vec3 ground;
    const bool onGround = groundPoint(origin, direction, fallbackY, ground);
    const float groundT = onGround ? length(subtract(ground, origin)) : std::max(drawDistance * 1.5f, 500.0f);
    float t = 0;
    if (modelSurfaceHit(origin, direction, groundT, t, excludeEntity)) {
        point = add(origin, scaled(direction, t));
        return true;
    }
    if (onGround) point = ground;
    return onGround;
}

void SceneView::setServerContent(const WorldContent* content, const WorldDatabase* database, std::function<std::string(int32_t)> meshForClass,
                                 AvatarResolver avatarFor) {
    invalidateShadowMaps();                     // placed-light shadow casters changed
    serverContent_ = content;
    serverDatabase_ = database;
    serverMeshForClass_ = meshForClass;
    serverAvatarFor_ = avatarFor;
    serverPlacements_.clear();
    liveParticles_.erase(std::remove_if(liveParticles_.begin(), liveParticles_.end(), [](const LiveParticles& l) { return l.server; }),
                         liveParticles_.end());
    struct GlobeReload {
        SceneView& view;
        ~GlobeReload() {
            for (const ServerPlacement& s : view.serverPlacements_)
                for (const StateLight& l : s.stateLights)
                    if (!l.event.globeMap.empty() && !view.globeIndex_.count(tr::toLowerAscii(l.event.globeMap))) {
                        view.loadGlobeMaps();
                        return;
                    }
        }
    } globeReload{*this};
    pointDefaultOwner_.clear();
    poolGarrisons_.clear();
    if (!content) return;
    poolGarrisons_.resize(content->spawnPools.size());
    for (const PlacedObject& o : content->objects)
        if (o.kind == ObjectKind::ControlPoint) pointDefaultOwner_[o.id] = o.controlPoint.defaultOwner;
    auto yawPlacement = [](const tr::Vec3& position, float rotation) {
        return placementMatrix(position, tr::Quat{0, std::sin(rotation * 0.5f), 0, std::cos(rotation * 0.5f)}, 1.0f);
    };
    // A creature's figure: its class mesh, or the assembled avatar with its idles and accessories.
    auto layerOf = [&](int32_t classId) { return cullingLayerForClass_ ? cullingLayerForClass_(classId) : 3; };
    auto dressCreature = [&](ServerPlacement& s, const Creature* creature) {
        if (creature) s.cullingLayer = layerOf(creature->classId);
        s.model = creature ? modelFor(meshForClass(creature->classId)) : nullptr;
        if (!creature || !avatarFor) return;
        const AvatarLook look = avatarFor(*creature);
        // "%": built for GPU skinning so the idle sequence can pose it per frame (poseIdle).
        const std::string pose = look.poseAnimation.empty() ? std::string() : "%" + look.poseAnimation;
        if (!look.baseMesh.empty()) s.model = modelFor(look.baseMesh, pose);
        s.skinHue = hueFromAbgr(look.skinColour);
        for (const AvatarPart& part : look.parts)
            s.avatarParts.push_back(AvatarDraw{modelFor(part.mesh, pose, look.baseMesh), part.swapsetId, hueFromAbgr(part.colour)});
        s.idles = look.idles;
        for (const AvatarAccessory& a : look.accessories)
            s.bodyAccessories.push_back(ServerPlacement::BodyAccessory{objectModelFor(a.mesh), uint32_t(a.connectionPoint), hueFromAbgr(a.colour)});
    };
    // Spawn pools: one figure per creature the pool can spawn at most. Rasa.NET (SpawnPoolManager.CreateListOfCreatures) makes,
    // slot by slot, a random count between min_count and max_count of each slot's creature (slots with creature 0 or max 0
    // skipped, 64 in all at most) and puts each at a random walkable point within the pool's radius (SpawnPoint; radius 0: the
    // point +- 2 m when the pool spawns several). The editor cannot repeat those draws: it shows max_count of every slot, the
    // first figure at the pool's point (raw, as stored) and the others spread evenly over the radius (a sunflower spiral; 2 m for
    // radius 0) at the terrain height when that is within 3 m of the pool's (as SnapToGround's navmesh rule), else the pool's.
    // Every figure selects the pool; the ring and facing arrow belong to the first.
    for (size_t k = 0; k < content->spawnPools.size(); ++k) {
        const SpawnPool& pool = content->spawnPools[k];
        std::vector<const Creature*> spawned;
        for (int slot = 0; slot < 6 && spawned.size() < 64; ++slot) {
            const SpawnSlot& entry = spawnSlot(pool, slot);
            if (entry.creatureId == 0 || entry.maxCount <= 0 || entry.minCount < 0 || entry.maxCount < entry.minCount) continue;
            const Creature* creature = database ? database->creature(entry.creatureId) : nullptr;
            if (!creature) continue;                    // the server skips a missing creature too
            for (int32_t n = 0; n < entry.maxCount && spawned.size() < 64; ++n) spawned.push_back(creature);
        }
        const Creature* first = spawned.empty() ? (database ? database->creature(pool.slot1.creatureId) : nullptr) : spawned.front();
        const bool npc = first && database->classIsNpc(first->classId);
        const bool vendor = first && database->classIsVendor(first->classId);
        ServerPlacement s;
        s.selection = Selection{SelectionKind::SpawnPool, k};
        s.position = pool.position;
        s.world = yawPlacement(pool.position, pool.rotation);
        dressCreature(s, first);
        s.colour = vendor ? markerColour::vendor : npc ? markerColour::npc : markerColour::hostileCreature;
        s.ringRadius = std::max(pool.radius, 1.5f);
        s.facing = true;
        serverPlacements_.push_back(s);
        const float spread = pool.radius > 0 ? pool.radius : 2.0f;
        for (size_t n = 1; n < spawned.size(); ++n) {
            const float r = spread * std::sqrt(float(n) / float(spawned.size() - 1));
            const float angle = pool.rotation + float(n) * 2.3999632f;         // golden angle
            tr::Vec3 p{pool.position.x + r * std::sin(angle), pool.position.y, pool.position.z + r * std::cos(angle)};
            const float ground = groundHeight(p.x, p.z, p.y);
            if (std::fabs(ground - p.y) <= 3.0f) p.y = ground;
            ServerPlacement extra;
            extra.selection = s.selection;
            extra.position = p;
            extra.world = yawPlacement(p, pool.rotation);
            dressCreature(extra, spawned[n]);
            extra.colour = s.colour;
            extra.extraFigure = true;
            serverPlacements_.push_back(extra);
        }
    }
    for (size_t k = 0; k < content->teleporters.size(); ++k) {
        const Teleporter& t = content->teleporters[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Teleporter, k};
        s.cullingLayer = layerOf(t.classId);
        s.position = t.position;
        s.world = yawPlacement(t.position, t.rotation);
        s.model = objectModelFor(meshForClass(t.classId));
        StateEffects fx = stateEffects(t.classId, 56);
        s.accessories = fx.accessories;
        s.stateParticles = fx.particles;
        s.stateLights = fx.lights;
        s.colour = markerColour::teleporter;
        s.ringRadius = 2.0f;
        s.facing = true;
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->objects.size(); ++k) {
        const PlacedObject& o = content->objects[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Object, k};
        s.cullingLayer = layerOf(o.classId);
        s.position = o.position;
        s.world = yawPlacement(o.position, o.rotation);
        s.model = objectModelFor(meshForClass(o.classId));
        if (o.kind == ObjectKind::AmbientNpc) {
            // Rasa.NET creates ambient figures in USE_SS_STATE_0 (44) (Managers/AmbientNpcs.cs); the client's UsableState puts on
            // the state's package and plays the state's animation (usable.py UsableState.__call__: AttachStateSFX, PlayAnimation).
            StateEffects fx = stateEffects(o.classId, 44);
            s.accessories = fx.accessories;
            s.stateParticles = fx.particles;
            s.stateLights = fx.lights;
            tr::GameData::UsableAnimation animation;
            if (usableAnimationFor_) {
                try { animation = usableAnimationFor_(o.classId, 44); } catch (const std::exception&) {}
            }
            if (!animation.animations.empty()) {
                s.model = modelFor(meshForClass(o.classId), "%" + animation.animations.front().name);
                s.idles = animation.animations;
                s.idleType = animation.type;
                s.idleDelayLower = animation.delayLower;
                s.idleDelayUpper = animation.delayUpper;
            }
        }
        if (o.kind == ObjectKind::Emitter) {
            // FXPackageEmitter: the body is particle_proxy.geo (no pieces, client/augmentations/emitter.py); while on, the package is
            // attached to it (fxpackageemitter.py TurnOn: body.AttachFXPackage(stringtable[package], [], [])).
            s.model = nullptr;
            std::string package;
            if (o.emitter.isOn && stringFor_) {
                try { package = stringFor_(int32_t(o.emitter.packageId)); } catch (const std::exception&) {}
            }
            if (package.size() > 4 && package.compare(package.size() - 4, 4, ".pkg") == 0) {
                // The package's events attach to the proxy's contact points: particle_proxy.geo has no pieces but carries every
                // HPS_ connection point (VFX1, Head, Right_Hand, ...) at its origin, 1 cm up - e.g. smokeplume_fire.pkg attaches
                // vfx_smokeplume.pfx to connection point 28 (HPS_VFX1).
                StateEffects fx;
                addPackageEffects(package, objectModelFor("particle_proxy.geo"), 0, fx);
                s.accessories = fx.accessories;
                s.stateParticles = fx.particles;
                s.stateLights = fx.lights;
            }
        }
        s.colour = markerColour::ofObject(o.kind);
        s.ringRadius = 1.5f;
        s.facing = o.kind != ObjectKind::Logos;
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->mapLinks.size(); ++k) {
        const MapLink& l = content->mapLinks[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::MapLink, k};
        s.position = l.position;
        s.world = yawPlacement(l.position, 0);
        s.colour = l.enabled ? markerColour::mapLink : markerColour::mapLinkDisabled;
        s.ringRadius = std::max(l.radius, 1.0f);
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->arrivals.size(); ++k) {
        const SpawnArrival& a = content->arrivals[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Arrival, k};
        s.position = a.position;
        s.world = yawPlacement(a.position, a.rotation);
        s.colour = markerColour::arrival;
        s.ringRadius = 1.0f;
        s.facing = true;
        for (const SpawnPool& pool : content->spawnPools)
            if (pool.id == a.poolId) {
                s.linked = true;
                s.linkTarget = pool.position;
            }
        serverPlacements_.push_back(s);
    }
    // Control point links: a line from each linked pool / teleporter / footlocker to its control point (drawn while either is
    // selected), and each garrison pool's side (Rasa.NET ControlPoints.Load finds a pool's point by its link - the last wins).
    for (const ControlPointLink& link : content->controlPointLinks) {
        const PlacedObject* point = nullptr;
        size_t pointIndex = 0;
        for (size_t k = 0; k < content->objects.size(); ++k)
            if (content->objects[k].kind == ObjectKind::ControlPoint && content->objects[k].id == link.controlPointId) {
                point = &content->objects[k];
                pointIndex = k;
            }
        if (!point) continue;
        Selection target{};
        if (link.kind == 1 || link.kind == 2) {
            for (size_t k = 0; k < content->spawnPools.size(); ++k)
                if (content->spawnPools[k].id == link.objectId) target = Selection{SelectionKind::SpawnPool, k};
        } else if (link.kind == 3 || link.kind == 4) {
            for (size_t k = 0; k < content->teleporters.size(); ++k)
                if (content->teleporters[k].id == link.objectId) target = Selection{SelectionKind::Teleporter, k};
        } else if (link.kind == 6) {
            for (size_t k = 0; k < content->objects.size(); ++k)
                if (content->objects[k].kind == ObjectKind::Footlocker && content->objects[k].id == link.objectId) target = Selection{SelectionKind::Object, k};
        }
        if (target.kind == SelectionKind::None) continue;
        bool lineSet = false;
        for (ServerPlacement& s : serverPlacements_) {
            if (s.selection.kind != target.kind || s.selection.index != target.index) continue;
            if (link.kind == 1 || link.kind == 2) {
                s.garrisonPoint = link.controlPointId;
                s.garrisonKind = link.kind;
                poolGarrisons_[target.index] = PoolGarrison{link.controlPointId, link.kind};
            }
            if (!lineSet && !s.extraFigure) {
                s.linked = true;
                s.linkTarget = point->position;
                s.pointLink = true;
                s.pointSelection = Selection{SelectionKind::Object, pointIndex};
                lineSet = true;
            }
        }
    }
    // Patrol steps: a line from each to the next of its pool, and from the last back to the first (the circuit Rasa.NET's
    // Patrols walks); a facing arrow where the step turns the creature.
    for (size_t k = 0; k < content->patrolSteps.size(); ++k) {
        const PatrolStep& p = content->patrolSteps[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::PatrolStep, k};
        s.position = p.position;
        s.world = yawPlacement(p.position, p.hasFacing ? p.facing : 0.0f);
        s.colour = markerColour::patrol;
        s.ringRadius = 0.5f;
        s.facing = p.hasFacing;
        size_t next = k + 1;
        if (next >= content->patrolSteps.size() || content->patrolSteps[next].poolId != p.poolId) {
            next = k;                                         // the last step: back to the pool's first
            while (next > 0 && content->patrolSteps[next - 1].poolId == p.poolId) --next;
        }
        if (next != k) {
            s.linked = true;
            s.linkTarget = content->patrolSteps[next].position;
        }
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->regions.size(); ++k) {
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Region, k};
        s.area = true;
        s.shape = content->regions[k].area;
        s.position = s.shape.position;
        s.colour = content->regions[k].underground == 1 ? markerColour::regionUnderground : markerColour::region;
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->missionAreas.size(); ++k) {
        ServerPlacement s;
        s.selection = Selection{SelectionKind::MissionArea, k};
        s.area = true;
        s.shape = content->missionAreas[k].area;
        s.position = s.shape.position;
        s.colour = markerColour::missionArea;
        serverPlacements_.push_back(s);
    }
}

std::string SceneView::modelStatus(size_t entityIndex) const {
    if (entityIndex >= placements_.size()) return "";
    const Placement& p = placements_[entityIndex];
    if (!p.classKnown) return "unknown class (not in the game data)";
    if (p.neverDrawn) return "never drawn in game (culling layer 6: sound emitters, stand-ins)";
    if (!p.model) return "class has no model";
    const Model& m = *p.model;
    if (m.failed) return "model failed to load: " + m.failure;
    if (!m.loaded) return "loading...";
    if (m.effectOnly) return "effect: additive pieces only (drawn with Effects on; a cyan box with Placeholders)";
    if (!m.hasSolid) return "model has no geometry";
    return m.hasWater ? "water surface" : "model";
}
