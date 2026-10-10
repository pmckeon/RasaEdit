#include "SceneView.h"
#include "GpuTexture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

// ------------------------------------------------------------------------------------------
// Camera
// ------------------------------------------------------------------------------------------
tr::Vec3 ViewCamera::forward() const {
    return tr::Vec3{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}
tr::Vec3 ViewCamera::right() const { return normalized(cross(tr::Vec3{0, 1, 0}, forward())); }
tr::Vec3 ViewCamera::up() const { return cross(forward(), right()); }
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
std::vector<Mat4> skinMatrices(const tr::GeoModel& geo, const tr::Animation& animation, float time, const tr::GeoModel* skeleton = nullptr) {
    std::unordered_map<std::string, Mat4> skeletonPosed;
    if (skeleton) {
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
        if (const tr::AnimTrack* track = tr::findTrack(animation, bone.name)) {
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
        if (!skeletonPosed.empty()) {
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
    return skin;
}

// creature_appearance colour (ABGR, red in the low byte) as a shader hue; w = 0 when there is none.
Vec4f hueFromAbgr(uint32_t colour) {
    if (colour == 0) return Vec4f{0, 0, 0, 0};
    return Vec4f{float(colour & 0xFF) / 255.0f, float((colour >> 8) & 0xFF) / 255.0f, float((colour >> 16) & 0xFF) / 255.0f, 1.0f};
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
                     std::function<std::string(int32_t)> meshForClass, std::function<bool(int32_t)> classKnown, uint32_t terrainStep)
    : renderer_(renderer), assets_(assets), map_(map) {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> triangles, lines;
    unitCube(vertices, triangles, lines);
    unitBox_ = renderer_.createMesh(vertices, triangles, false);
    unitBoxLines_ = renderer_.createMesh(vertices, lines, true);
    auto lineMesh = [&](const std::vector<tr::Vec3>& points, const std::vector<uint32_t>& segments) {
        std::vector<MeshVertex> v;
        for (const tr::Vec3& p : points) {
            MeshVertex m;
            m.positionX = p.x; m.positionY = p.y; m.positionZ = p.z;
            m.normalY = 1;
            v.push_back(m);
        }
        return renderer_.createMesh(v, segments, true);
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
        buildTerrain(terrainStep);
        buildTerrainMaterial();
    }

    // Placements, and the set of models to load.
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    placements_.reserve(map_.map.entities.size());
    for (const tr::MapEntity& e : map_.map.entities) {
        Placement p;
        p.world = placementMatrix(e.location, e.orientation, e.scale);
        p.hasBounds = e.hasBounds;
        const tr::Bounds bounds = e.hasBounds ? e.bounds : tr::Bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 1, 0.5f}};
        p.boxWorld = multiply(p.world, boxMatrix(bounds));
        const tr::Vec3 localCentre = scaled(add(bounds.min, bounds.max), 0.5f);
        p.centre = transformPoint(p.world, localCentre);
        p.radius = std::max(0.5f, length(subtract(bounds.max, bounds.min)) * 0.5f * e.scale);
        p.classKnown = classKnown(e.classId);
        const std::string mesh = meshForClass(e.classId);
        // Animated objects ship an animation named like their model (npc_eloh_hologram_large.geo/.anm,
        // arch_eloh_hologram_platform_v01.geo/.anm, ...): pose them in its first frame. Without it the hologram's
        // skinned pieces collapse into a column.
        std::string objectAnimation;
        if (mesh.size() > 4 && tr::toLowerAscii(mesh.substr(mesh.size() - 4)) == ".geo") {
            const std::string anm = mesh.substr(0, mesh.size() - 4) + ".anm";
            if (assets_.exists(anm)) objectAnimation = anm;
        }
        p.model = modelFor(mesh, objectAnimation);      // unordered_map nodes are stable
        placements_.push_back(p);
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
        } catch (const std::exception& e) {
            mapLightsError_ = e.what();
        }
    }

    // The map's starting sky time is in seconds into its day (e.g. 3375 of 10800 = 07:30).
    if (map_.map.dayLengthSeconds > 0)
        startTimeOfDay_ = float(map_.map.startingSkyTime % map_.map.dayLengthSeconds) / float(map_.map.dayLengthSeconds);
    timeOfDay = startTimeOfDay_;
}

void SceneView::setRegionSkies(const std::vector<std::pair<int32_t, std::string>>& regionSkies) {
    skies_.clear();
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
    // too: no region names them, yet on Concordia Wilderness the game lit the world with the base sky while region 0
    // names the _tranz one (midday ambient (1.22, 1.06, 0.82) measured vs base (1.20, 1.04, 0.80), _tranz (0.81, 0.95,
    // 0.97)). How the engine picks it (RegionManagerClient::DoTransitionSky FUN_0052c2d0) is not decoded; the base is
    // preselected only when region 0's sky is a _tranz variant (UNVERIFIED beyond that map).
    for (const tr::MapRegion& r : map_.map.regions) regionPriority_[r.regionId] = r.priority;
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
        const std::string& variant = skies_[k].name;
        const bool tranz = variant.size() > 10 && variant.compare(variant.size() - 10, 10, "_tranz.sky") == 0;
        if (tranz && it->loaded) {
            for (auto& rs : regionSky_)
                if (rs.second == k) rs.second = size_t(it - skies_.begin());
            if (k == skyIndex && haveRegion0) skyIndex = size_t(it - skies_.begin());
        }
    }
}

namespace {
bool insideArea(const AreaShape& a, const tr::Vec3& p) {
    if (p.y < a.minY || p.y > a.maxY) return false;
    const float dx = p.x - a.position.x, dz = p.z - a.position.z;
    if (a.shape == 2) return std::fabs(dx) <= a.halfX && std::fabs(dz) <= a.halfZ;
    return dx * dx + dz * dz <= a.radius * a.radius;
}
}  // namespace

size_t SceneView::activeSkyIndex() const {
    if (!skyByRegion || !serverContent_ || serverContent_->regions.empty()) return skyIndex;
    // Regions containing the camera, plus region 0 (the client always adds DEFAULT_REGION_ID), by priority.
    std::vector<int32_t> inside{0};
    for (const MapRegion& r : serverContent_->regions)
        if (r.enabled && insideArea(r.area, camera_.position)) inside.push_back(r.regionId);
    const auto priority = [&](int32_t id) { const auto it = regionPriority_.find(id); return it == regionPriority_.end() ? 0 : it->second; };
    std::stable_sort(inside.begin(), inside.end(), [&](int32_t a, int32_t b) { return priority(a) > priority(b); });
    for (int32_t id : inside) {
        const auto it = regionSky_.find(id);
        if (it != regionSky_.end() && it->second < skies_.size() && skies_[it->second].loaded) return it->second;
    }
    return skyIndex;
}

bool SceneView::skyFromRegion() const { return skyByRegion && serverContent_ && !serverContent_->regions.empty(); }

SceneLighting SceneView::lighting() const {
    SceneLighting l = skyLightingAt();
    l.showLight = showLightBuffer;
    if (!showMapLights) return l;
    // Placed lights: the enabled ones nearest the camera (distance to their range sphere), up to the shader's limit.
    std::vector<std::pair<float, const tr::PlacedLight*>> nearest;
    for (const tr::PlacedLight& light : mapLights_) {
        if (!light.enabled || light.range <= 0 || light.intensity <= 0) continue;
        const float gap = std::max(0.0f, length(subtract(light.position, camera_.position)) - light.range);
        if (gap <= drawDistance) nearest.emplace_back(gap, &light);
    }
    std::sort(nearest.begin(), nearest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& n : nearest) {
        if (l.localCount >= SceneLighting::maxLocalLights) break;
        const tr::PlacedLight& light = *n.second;
        const int i = l.localCount++;
        l.localPosition[i] = Vec4f{light.position.x, light.position.y, light.position.z, light.range};
        // Fade start distance: the engine sets LightFalloff = (1 - falloff) * LightRange (tabula_rasa.exe FUN_00879150 /
        // FUN_00875360), i.e. FalloffPercent is the outer part of the range that fades (0.25: full to 75%, then fade).
        // That the field is FalloffPercent is inferred from its name and use (the serializer's offsets are unclear).
        l.localColour[i] = Vec4f{light.colour.x * light.intensity, light.colour.y * light.intensity, light.colour.z * light.intensity,
                                 (1.0f - std::clamp(light.falloffPercent, 0.0f, 1.0f)) * light.range};
        if (light.kind == tr::PlacedLight::Kind::Spot) {
            const tr::Vec3 d = normalized(light.direction());
            const float outer = std::max(light.penumbraAngle, light.coneAngle) * 0.5f * 0.0174533f;   // full angles (UNVERIFIED)
            l.localSpot[i] = Vec4f{d.x, d.y, d.z, std::cos(outer)};
            l.localSpotInner[i] = Vec4f{std::cos(light.coneAngle * 0.5f * 0.0174533f), 0, 0, 0};
        } else {
            l.localSpot[i] = Vec4f{0, -1, 0, -2.0f};       // point (plane lights approximated as points)
        }
    }
    return l;
}

SceneLighting SceneView::skyLightingAt() const {
    SceneLighting l;                                   // plain editor lighting
    const size_t active = activeSkyIndex();
    if (!skyLighting || active >= skies_.size() || !skies_[active].loaded) return l;
    const tr::Sky& sky = skies_[active].sky;
    const float t = timeOfDay;
    auto vec4 = [](const tr::Vec3& c) { return Vec4f{c.x, c.y, c.z, 1}; };
    auto luminance = [](const tr::Vec3& c) { return 0.3f * c.x + 0.59f * c.y + 0.11f * c.z; };

    // Hemispheric light: the sum (CombinedHemiLight) of the hemi lights the deferred renderer uses. tabula_rasa.exe
    // FUN_00812dc0 / FUN_00812880 take a light only when RenderMode == 1 in deferred mode (0 in the fallback renderer)
    // or RenderMode == 2; modes > 2 are mapped to a default (FUN_0086a910 -> FUN_00887550, not decoded - counted
    // here). Skies carry one hemi light per renderer (Concordia: Light_01 mode 1, Light_02 mode 0). Checked at
    // Alia Das 12:00: vertical faces away from the sun 0.55 measured vs (top + bottom) / 2 = 0.55 of Light_01 alone.
    tr::Vec3 top{0, 0, 0}, bottom{0, 0, 0};
    for (const tr::SkyLight& g : sky.globalLights) {
        if (!g.hemi || g.renderMode == 0) continue;
        top = add(top, tr::skyLightColour(g, 1, t));
        bottom = add(bottom, tr::skyLightColour(g, 2, t));
    }
    l.hemiTop = vec4(scaled(top, ambientScale));
    l.hemiBottom = vec4(scaled(bottom, ambientScale));

    // Directional light: the brightest layer light at this time (the sun by day, the moon by night).
    l.sunFront = Vec4f{0, 0, 0, 1};
    l.sunBack = Vec4f{0, 0, 0, 1};
    float best = -1;
    for (const tr::SkyLayer& layer : sky.layers) {
        if (!layer.enabled || !showSun) continue;
        for (const tr::SkyLight& light : layer.lights) {
            if (light.hemi || !light.active) continue;
            const tr::Vec3 front = tr::skyLightColour(light, 3, t);
            if (luminance(front) <= best) continue;
            best = luminance(front);
            const tr::Vec3 travel = tr::skyLayerDirection(layer, light.orientation, t);
            l.towardsLight = normalized(tr::Vec3{-travel.x, -travel.y, -travel.z});
            l.sunFront = vec4(front);
            l.sunShadows = showShadows;
            l.sunBack = vec4(tr::skyLightColour(light, 4, t));
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
    SDL_GPUDevice* device = renderer_.device();
    renderer_.destroyMesh(terrain_);
    renderer_.destroyMesh(unitBox_);
    renderer_.destroyMesh(unitBoxLines_);
    renderer_.destroyMesh(unitCircle_);
    renderer_.destroyMesh(unitSquare_);
    renderer_.destroyMesh(arrow_);
    renderer_.destroyMesh(pole_);
    for (auto& m : models_)
        for (ModelPiece& piece : m.second.pieces) renderer_.destroyMesh(piece.mesh);
    for (auto& t : textures_)
        if (t.second) SDL_ReleaseGPUTexture(device, t.second);
    for (SDL_GPUTexture* t : {terrainMaterial_.layers, terrainMaterial_.layerIndices, terrainMaterial_.layerWeights})
        if (t) SDL_ReleaseGPUTexture(device, t);
}

size_t SceneView::texturesLoaded() const {
    size_t count = 0;
    for (const auto& t : textures_)
        if (t.second) ++count;
    return count;
}

SDL_GPUTexture* SceneView::texture(const std::string& name) {
    if (name.empty()) return nullptr;
    const std::string key = tr::toLowerAscii(name);
    const auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;
    SDL_GPUTexture* result = nullptr;
    tr::Bytes data;
    try {
        if (assets_.tryRead(key, data)) result = uploadDdsTexture(renderer_.device(), data);
        else textureErrors_[key] = "not found in the archives";
    } catch (const std::exception& e) {
        result = nullptr;                       // unreadable or unsupported: draw untextured
        textureErrors_[key] = e.what();
    }
    textures_.emplace(key, result);
    return result;
}

void SceneView::buildTerrainMaterial() {
    // Layer textures, resized to a common size for the texture array (layer index = position in texlayers.tab).
    const uint32_t size = 256;
    std::vector<std::vector<tr::Rgba8>> layers;
    for (const tr::TextureLayer& layer : map_.textureLayers) {
        std::string file = layer.texture;
        const size_t slash = file.find_last_of("/\\");
        if (slash != std::string::npos) file = file.substr(slash + 1);
        std::vector<tr::Rgba8> pixels(size_t(size) * size, tr::Rgba8{128, 128, 128, 255});   // missing: neutral grey
        tr::Bytes data;
        try {
            if (assets_.tryRead(file, data)) {
                tr::DdsHeader header;
                const std::vector<tr::Rgba8> decoded = tr::decodeDds(data, header);
                pixels = resizeRgba(decoded, header.width, header.height, size, size);
            }
        } catch (const std::exception&) {
        }
        layers.push_back(std::move(pixels));
        terrainMaterial_.tilings.push_back(layer.tiling);
    }
    terrainMaterial_.layers = uploadRgbaArray(renderer_.device(), layers, size);

    // Per-sample layer indices and weights, one texel per sample (row = z index, column = x index).
    const tr::Terrain& t = map_.terrain;
    std::vector<tr::Rgba8> indices(size_t(t.columns) * t.rows), weights(size_t(t.columns) * t.rows);
    for (size_t k = 0; k < t.samples.size(); ++k) {
        const tr::TerrainSample& s = t.samples[k];
        indices[k] = tr::Rgba8{s.layer0.layerIndex, s.layer1.layerIndex, s.layer2.layerIndex, s.layer3.layerIndex};
        weights[k] = tr::Rgba8{s.layer0.weight, s.layer1.weight, s.layer2.weight, s.layer3.weight};
    }
    terrainMaterial_.layerIndices = uploadRgbaTexture(renderer_.device(), indices.data(), t.columns, t.rows);
    terrainMaterial_.layerWeights = uploadRgbaTexture(renderer_.device(), weights.data(), t.columns, t.rows);
    terrainMaterial_.originX = t.worldX(0);
    terrainMaterial_.originZ = t.worldZ(0);
    terrainMaterial_.inverseSpacing = 1.0f / t.header.spacing;
    terrainMaterial_.columns = t.columns;
    terrainMaterial_.rows = t.rows;
}

void SceneView::buildTerrain(uint32_t step) {
    const tr::Terrain& t = map_.terrain;
    const uint32_t rows = (t.boundedRows() - 1) / step + 1;
    const uint32_t columns = (t.boundedColumns() - 1) / step + 1;
    float lowest = 1e30f, highest = -1e30f;
    for (uint32_t i = 0; i < rows; ++i)
        for (uint32_t j = 0; j < columns; ++j)
            if (t.hasData(i * step, j * step)) {
                const float h = t.worldHeightAt(i * step, j * step);
                lowest = std::min(lowest, h);
                highest = std::max(highest, h);
            }
    const float range = highest > lowest ? highest - lowest : 1.0f;
    auto heightAt = [&](int64_t i, int64_t j, float fallback) {
        if (i < 0 || j < 0 || i >= int64_t(rows) || j >= int64_t(columns)) return fallback;
        const uint32_t r = uint32_t(i) * step, c = uint32_t(j) * step;
        return t.hasData(r, c) ? t.worldHeightAt(r, c) : fallback;
    };
    const float spacing = t.header.spacing * float(step);
    std::vector<MeshVertex> vertices(size_t(rows) * columns);
    for (uint32_t i = 0; i < rows; ++i) {
        for (uint32_t j = 0; j < columns; ++j) {
            const float h = heightAt(i, j, 0.0f);
            MeshVertex& v = vertices[size_t(i) * columns + j];
            v.positionX = t.worldX(j * step);
            v.positionY = h;
            v.positionZ = t.worldZ(i * step);
            // Normal of the height field y = h(x, z): (-dh/dx, 1, -dh/dz).
            const float dx = (heightAt(i, int64_t(j) + 1, h) - heightAt(i, int64_t(j) - 1, h)) / (2.0f * spacing);
            const float dz = (heightAt(int64_t(i) + 1, j, h) - heightAt(int64_t(i) - 1, j, h)) / (2.0f * spacing);
            const tr::Vec3 n = normalized(tr::Vec3{-dx, 1.0f, -dz});
            v.normalX = n.x;
            v.normalY = n.y;
            v.normalZ = n.z;
            v.colour = heightColour((h - lowest) / range);
        }
    }
    std::vector<uint32_t> indices;
    indices.reserve(size_t(rows - 1) * (columns - 1) * 6);
    for (uint32_t i = 0; i + 1 < rows; ++i) {
        for (uint32_t j = 0; j + 1 < columns; ++j) {
            if (!t.hasData(i * step, j * step) || !t.hasData((i + 1) * step, j * step) || !t.hasData(i * step, (j + 1) * step) ||
                !t.hasData((i + 1) * step, (j + 1) * step))
                continue;
            const uint32_t a = i * columns + j, b = a + 1, c = a + columns, d = c + 1;
            indices.insert(indices.end(), {a, c, d, a, d, b});
        }
    }
    terrain_ = renderer_.createMesh(vertices, indices, false);
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
        // "<mesh>|<animation>|<skeleton mesh>" (avatar parts: posed on the base body's skeleton).
        const size_t bar = name.find('|');
        const size_t bar2 = bar == std::string::npos ? std::string::npos : name.find('|', bar + 1);
        const tr::GeoModel geo = tr::readGeo(assets_.read(name.substr(0, bar)));
        std::vector<Mat4> skin;                    // per skeleton bone; empty = draw as authored
        if (bar != std::string::npos && !geo.bones.empty()) {
            try {
                const std::string animationName = name.substr(bar + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar - 1);
                tr::GeoModel skeletonGeo;
                if (bar2 != std::string::npos) skeletonGeo = tr::readGeo(assets_.read(name.substr(bar2 + 1)));
                skin = skinMatrices(geo, tr::readAnimation(assets_.read(animationName)), 0.0f, bar2 != std::string::npos ? &skeletonGeo : nullptr);
            } catch (const std::exception& e) {
                SDL_Log("pose %s: %s; drawing the bind pose", name.c_str(), e.what());
            }
        }
        // Skinned pieces without an animation are still skinned, in the bind pose: their vertices are not stored in
        // place (prop_hum_sign_holo_v11's screen: stored y 1.06..2.01, skinned y 1.60..3.01 = the frame's screen; the
        // inverse binds carry the sign's x1.5 scale).
        const bool anySkinned = std::any_of(geo.pieces.begin(), geo.pieces.end(), [](const tr::GeoPiece& p) { return p.skinned; });
        if (skin.empty() && anySkinned && !geo.bones.empty()) skin = skinMatrices(geo, tr::Animation{}, 0.0f);
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
            const bool additive = effect.find("additive") != std::string::npos;
            if (additive) ++effectPieces;
            // Not drawn (counted as effect pieces): helper shapes - effects that sample a diffuse map but have none
            // (e.g. vfx_eloh_lilly_pollen.geo, a 207 m particle-emitter sheet whose contact point names its .pfx) - and
            // camera-facing billboard sprites (TRSphBillboard), which would show as fixed quads.
            // Also refraction shells (TRRefract, e.g. around npc_eloh_hologram_large): they only distort what is behind
            // them, and drawn solid they hid the additive hologram inside.
            const bool helperShape = piece.diffuseTexture.empty() && effect.find("diffmap") != std::string::npos;
            const bool refraction = effect.rfind("trrefract", 0) == 0;
            if (!additive && (helperShape || refraction || effect.find("billboard") != std::string::npos)) {
                ++effectPieces;
                continue;
            }
            const bool water = effect.find("water") != std::string::npos;
            // "VertColor" effects: albedo = texture x vertex colour x 2 (the game's G-buffer pass, trdiffmapvertcolor).
            // The vertex colours are authored greyscale occlusion (they do not encode the map's lights).
            const bool vertexColour = effect.find("vertcolor") != std::string::npos;
            // TRAdditiveAnisoSpec* (holograms): the game's own formula, see AnisoMaterial. Additive pieces keep their
            // vertex alpha: it is the opacity (the aniso shader multiplies by it; glow fins fade out by it).
            const bool aniso = effect.rfind("tradditiveanisospec", 0) == 0;
            // Glow map: added after lighting (see shaders/scene.hlsl), sampled with UV set GlowTexCoord (0 or 1).
            std::string glowName;
            int glowUvSet = 1;
            for (const tr::GeoParameter& gp : piece.parameters) {
                if (gp.key == "GlowTexture") glowName = gp.value;
                if (gp.key == "GlowTexCoord") glowUvSet = std::atoi(gp.value.c_str());
            }
            // Static pieces attached to a bone are moved by it (as in the OBJ exporter).
            const tr::GeoBone* bone = nullptr;
            if (!piece.skinned && !piece.boneName.empty())
                for (const tr::GeoBone& b : geo.bones)
                    if (b.name == piece.boneName) bone = &b;
            Mat4 boneMatrix = identityMatrix();
            if (bone) {
                boneMatrix = rotationMatrix(bone->rotation);
                boneMatrix.row0 = {boneMatrix.row0.x * bone->scale.x, boneMatrix.row0.y * bone->scale.y, boneMatrix.row0.z * bone->scale.z, bone->translation.x};
                boneMatrix.row1 = {boneMatrix.row1.x * bone->scale.x, boneMatrix.row1.y * bone->scale.y, boneMatrix.row1.z * bone->scale.z, bone->translation.y};
                boneMatrix.row2 = {boneMatrix.row2.x * bone->scale.x, boneMatrix.row2.y * bone->scale.y, boneMatrix.row2.z * bone->scale.z, bone->translation.z};
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
            std::vector<MeshVertex> vertices;
            vertices.reserve(piece.vertices.size());
            for (const tr::GeoVertex& gv : piece.vertices) {
                tr::Vec3 p = transformPoint(boneMatrix, gv.position);
                tr::Vec3 n = subtract(transformPoint(boneMatrix, gv.normal), transformPoint(boneMatrix, tr::Vec3{0, 0, 0}));
                if (!palette.empty()) {
                    // Linear blend skinning: sum of weight * (posed world * inverse bind world) over up to 4 bones.
                    tr::Vec3 sp{0, 0, 0}, sn{0, 0, 0};
                    float total = 0;
                    for (uint32_t i = 0; i < 4; ++i) {
                        const float w = tr::blendWeight(gv, i);
                        const uint32_t slot = tr::blendIndex(gv, i);
                        if (w <= 0 || slot >= palette.size() || palette[slot] < 0) continue;
                        const Mat4& m = skin[size_t(palette[slot])];
                        sp = add(sp, scaled(transformPoint(m, gv.position), w));
                        sn = add(sn, scaled(subtract(transformPoint(m, gv.normal), transformPoint(m, tr::Vec3{0, 0, 0})), w));
                        total += w;
                    }
                    if (total > 0) {
                        p = scaled(sp, 1.0f / total);
                        n = sn;
                    }
                }
                MeshVertex v;
                v.positionX = p.x; v.positionY = p.y; v.positionZ = p.z;
                v.normalX = n.x; v.normalY = n.y; v.normalZ = n.z;
                v.colour = vertexColour ? packColour(uint8_t(gv.colour >> 16), uint8_t(gv.colour >> 8), uint8_t(gv.colour),     // ARGB
                                                     additive ? uint8_t(gv.colour >> 24) : 255)
                                        : packColour(255, 255, 255, 255);
                v.texU = gv.uv.x;
                v.texV = gv.uv.y;
                v.texU1 = glowUvSet == 0 ? gv.uv.x : gv.uv1.x;
                v.texV1 = glowUvSet == 0 ? gv.uv.y : gv.uv1.y;
                vertices.push_back(v);
            }
            if (!vertices.empty() && piece.indices.size() >= 3) {
                ModelPiece part;
                part.mesh = renderer_.createMesh(vertices, piece.indices, false);
                part.water = water;
                model.hasWater = model.hasWater || water;
                part.texture = water ? nullptr : texture(piece.diffuseTexture);    // water's diffuse map is a stand-in
                for (const tr::GeoParameter& p : piece.parameters)
                    if (p.key == "AlphaTestEnable") part.alphaTest = p.value == "true";
                part.hued = tr::isHuedEffect(piece.effectName);
                part.colourScale = vertexColour ? 2.0f : 1.0f;
                if (!glowName.empty() && !water) part.glow = texture(glowName);
                part.flesh = tr::toLowerAscii(piece.swapGroup) == "flesh";
                part.swapId = piece.swapId;
                part.additive = additive;
                if (aniso) {
                    part.aniso = true;
                    AnisoMaterial& m = part.anisoMaterial;
                    m.diffuse = part.texture;
                    for (const tr::GeoParameter& p : piece.parameters) {
                        if (p.key == "DetailTexture") m.detail = texture(p.value);
                        if (p.key == "DecalTexture") m.decal = texture(p.value);
                        if (p.key == "LightTexture") m.light = texture(p.value);
                        if (p.key == "DetailTexCoord") m.detailUv1 = std::atoi(p.value.c_str()) == 1;
                        if (p.key == "DecalTexCoord") m.decalUv1 = std::atoi(p.value.c_str()) == 1;
                        if (p.key == "MatSpecular") m.specular = parseColour(p.value);
                        if (p.key == "MatDiffuse") m.diffuseAlpha = parseColour(p.value);
                    }
                }
                model.pieces.push_back(part);
                if (additive) continue;
                model.hasSolid = true;
                const uint32_t base = uint32_t(model.positions.size());
                for (const MeshVertex& v : vertices) model.positions.push_back(tr::Vec3{v.positionX, v.positionY, v.positionZ});
                for (size_t k = 0; k + 2 < piece.indices.size(); k += 3)
                    if (piece.indices[k] < vertices.size() && piece.indices[k + 1] < vertices.size() && piece.indices[k + 2] < vertices.size())
                        model.triangles.insert(model.triangles.end(), {base + piece.indices[k], base + piece.indices[k + 1], base + piece.indices[k + 2]});
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
    while (!pending_.empty()) {
        const std::string name = pending_.front();
        pending_.pop_front();
        loadModel(name, models_[name]);
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > budgetSeconds) break;
    }
}

// ------------------------------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------------------------------
void SceneView::queueDraws(const Selection& selection) {
    const int selectedEntity = selection.kind == SelectionKind::MapEntity ? int(selection.index) : -1;
    const float aspect = float(renderer_.width()) / float(std::max<uint32_t>(1, renderer_.height()));
    const Mat4 viewProjection = multiply(camera_.projectionMatrix(aspect), camera_.viewMatrix());
    renderer_.beginFrame(viewProjection, camera_.position, lighting());
    if (showTerrain && terrain_.vertices) {
        if (showTextures && terrainMaterial_.layers) renderer_.drawTerrain(terrain_, terrainMaterial_);
        else renderer_.draw(terrain_, identityMatrix(), Vec4f{1, 1, 1, 1}, true);     // height-tinted vertex colours
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

    // Map entities and server objects are lit and tinted alike; only the selection is highlighted.
    auto modelTint = [&](bool selected) {
        return selected ? Vec4f{1.0f, 0.9f, 0.45f, 1} : showTextures ? Vec4f{1, 1, 1, 1} : Vec4f{0.8f, 0.8f, 0.8f, 1};
    };
    auto scaledTint = [](const Vec4f& t, float s) { return Vec4f{t.x * s, t.y * s, t.z * s, t.w}; };   // VertColor x2
    // Additive pieces; holograms (aniso) use the game's own material when textures are shown.
    auto drawEffect = [&](const ModelPiece& piece, const Mat4& world) {
        if (piece.aniso && showTextures) renderer_.drawAnisoAdditive(piece.mesh, world, piece.anisoMaterial);
        else renderer_.drawAdditive(piece.mesh, world, scaledTint(Vec4f{1, 1, 1, 1}, piece.colourScale), showTextures ? piece.texture : nullptr);
    };
    auto waterTint = [](bool selected) { return Vec4f{0.22f, 0.42f, 0.58f, selected ? 0.75f : 0.55f}; };

    drawnEntities_ = 0;
    if (showEntities) {
        const float maxDistance2 = drawDistance * drawDistance;
        for (size_t k = 0; k < placements_.size(); ++k) {
            const Placement& p = placements_[k];
            const tr::Vec3 d = subtract(p.centre, camera_.position);
            if (dot(d, d) > maxDistance2 + p.radius * p.radius * 4 && int(k) != selectedEntity) continue;
            if (!visible(p.centre, p.radius)) continue;
            ++drawnEntities_;
            const bool selected = int(k) == selectedEntity;
            if (p.model && p.model->loaded && showEffects) {      // additive pieces: glows, holograms, light cones
                for (const ModelPiece& piece : p.model->pieces)
                    if (piece.additive) drawEffect(piece, p.world);
            }
            if (p.model && p.model->loaded && p.model->hasSolid) {
                const Vec4f tint = modelTint(selected);
                for (const ModelPiece& piece : p.model->pieces) {
                    if (piece.additive) continue;
                    if (piece.water) renderer_.drawTranslucent(piece.mesh, p.world, waterTint(selected));
                    else renderer_.draw(piece.mesh, p.world, scaledTint(tint, piece.colourScale), true, showTextures ? piece.texture : nullptr, showTextures && piece.alphaTest,
                                        Vec4f{0, 0, 0, 0}, showTextures ? piece.glow : nullptr);
                }
            } else if (p.model && p.model->loaded && p.model->effectOnly) {
                // Effect-only entity (particle emitters, light cones): with Placeholders on, a cyan wireframe of its bounds.
                if (showEffects && showPlaceholders && p.hasBounds && !selected) renderer_.draw(unitBoxLines_, p.boxWorld, Vec4f{0.3f, 0.9f, 1.0f, 1}, false);
            } else if (showPlaceholders && p.hasBounds) {
                // Wireframe, so large invisible volumes don't hide the scene.
                // Red: unknown class; orange: model failed to load or is missing; blue: class has no model.
                const Vec4f tint = !p.classKnown ? Vec4f{0.95f, 0.3f, 0.25f, 1} : p.model ? Vec4f{0.95f, 0.6f, 0.2f, 1} : Vec4f{0.45f, 0.6f, 1.0f, 1};
                if (!selected) renderer_.draw(unitBoxLines_, p.boxWorld, tint, false);
            }
            if (selected) renderer_.draw(unitBoxLines_, p.boxWorld, Vec4f{1.0f, 0.85f, 0.1f, 1}, false);
        }
    }

    // Server content (World database).
    const Vec4f highlight{1.0f, 0.85f, 0.1f, 1};
    const float maxDistance2 = drawDistance * drawDistance;
    for (const ServerPlacement& s : serverPlacements_) {
        const SelectionKind kind = s.selection.kind;
        if (kind == SelectionKind::Region ? !showRegions : kind == SelectionKind::MissionArea ? !showMissionAreas : !showServer) continue;
        const bool selected = sameSelection(s.selection, selection);
        const Vec4f colour = selected ? highlight : s.colour;
        if (s.area) {
            const tr::Vec3& c = s.shape.position;
            const bool box = s.shape.shape == 2;
            const float rx = box ? s.shape.halfX : s.shape.radius, rz = box ? s.shape.halfZ : s.shape.radius;
            Mat4 m = identityMatrix();
            m.row0 = {rx, 0, 0, c.x};
            m.row1 = {0, 1, 0, c.y + 0.3f};
            m.row2 = {0, 0, rz, c.z};
            renderer_.draw(box ? unitSquare_ : unitCircle_, m, colour, false);
            Mat4 pole = identityMatrix();
            pole.row0 = {1, 0, 0, c.x};
            pole.row1 = {0, 25, 0, c.y};
            pole.row2 = {0, 0, 1, c.z};
            renderer_.draw(pole_, pole, colour, false);
            continue;
        }
        const tr::Vec3 d = subtract(s.position, camera_.position);
        if (!selected && dot(d, d) > maxDistance2) continue;
        if (!visible(s.position, std::max(s.ringRadius, 3.0f))) continue;
        const bool hasModel = s.model && s.model->loaded && (s.model->hasSolid || s.model->effectOnly);
        if (hasModel) {
            const Vec4f tint = modelTint(selected);
            auto drawPiece = [&](const ModelPiece& piece, const Vec4f& partHue) {
                if (piece.additive) {
                    if (showEffects) drawEffect(piece, s.world);
                    return;
                }
                if (piece.water) {
                    renderer_.drawTranslucent(piece.mesh, s.world, waterTint(selected));
                    return;
                }
                const Vec4f hue = !showTextures || !piece.hued ? Vec4f{0, 0, 0, 0} : piece.flesh ? s.skinHue : partHue;
                renderer_.draw(piece.mesh, s.world, scaledTint(tint, piece.colourScale), true, showTextures ? piece.texture : nullptr, showTextures && piece.alphaTest, hue,
                               showTextures ? piece.glow : nullptr);
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
            for (const ModelPiece& piece : s.model->pieces)
                if (piece.swapId >= 32 || ((replaced >> piece.swapId) & 0x1) == 0)
                    drawPiece(piece, skinned && piece.swapId >= 1 && piece.swapId <= 8 ? Vec4f{0.5f, 0.5f, 0.5f, 1} : Vec4f{0, 0, 0, 0});
            for (const AvatarDraw& part : s.avatarParts) {
                if (!part.model || !part.model->loaded) continue;
                for (const ModelPiece& piece : part.model->pieces)
                    if (piece.swapId == part.swapsetId) drawPiece(piece, part.hue);
            }
        } else {
            renderer_.draw(unitBoxLines_, multiply(s.world, boxMatrix(tr::Bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 2, 0.5f}})), colour, false);
        }
        if (s.ringRadius > 0) {
            Mat4 ring = identityMatrix();
            ring.row0 = {s.ringRadius, 0, 0, s.position.x};
            ring.row1 = {0, 1, 0, s.position.y + 0.15f};
            ring.row2 = {0, 0, s.ringRadius, s.position.z};
            renderer_.draw(unitCircle_, ring, colour, false);
        }
        if (s.facing) {
            Mat4 arrow = s.world;                       // yaw placement scaled to 3 m, raised slightly
            arrow.row0 = {arrow.row0.x * 3, arrow.row0.y * 3, arrow.row0.z * 3, arrow.row0.w};
            arrow.row1 = {arrow.row1.x * 3, arrow.row1.y * 3, arrow.row1.z * 3, arrow.row1.w + 0.3f};
            arrow.row2 = {arrow.row2.x * 3, arrow.row2.y * 3, arrow.row2.z * 3, arrow.row2.w};
            renderer_.draw(arrow_, arrow, colour, false);
        }
        if (selected && hasModel) renderer_.draw(unitBoxLines_, multiply(s.world, boxMatrix(s.model->bounds)), highlight, false);
    }
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

Selection SceneView::pick(float ndcX, float ndcY, bool pickEntities, bool pickServer) const {
    const float aspect = float(renderer_.width()) / float(std::max<uint32_t>(1, renderer_.height()));
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
            bestT = c.t;
            best = int(c.index);
        }
    }
    Selection result;
    if (best >= 0) result = Selection{SelectionKind::MapEntity, size_t(best)};

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
        float t = 1e30f;
        bool hit = false;
        if (s.area) {
            hit = raySphere(tr::Vec3{s.shape.position.x, s.shape.position.y + 2.0f, s.shape.position.z}, 3.0f, t);
        } else if (s.model && s.model->loaded && !s.model->triangles.empty()) {
            const Mat4& w = s.world;                    // yaw rotation, unit scale: local = R^T (p - t)
            auto toLocal = [&](const tr::Vec3& v) {
                return tr::Vec3{w.row0.x * v.x + w.row1.x * v.y + w.row2.x * v.z, w.row0.y * v.x + w.row1.y * v.y + w.row2.y * v.z,
                                w.row0.z * v.x + w.row1.z * v.y + w.row2.z * v.z};
            };
            hit = rayHitsModel(*s.model, toLocal(subtract(camera_.position, s.position)), toLocal(direction), t);
        } else {
            hit = raySphere(tr::Vec3{s.position.x, s.position.y + 1.0f, s.position.z}, std::max(1.5f, std::min(s.ringRadius, 3.0f)), t);
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

void SceneView::focusOn(const Selection& selection) {
    tr::Vec3 target;
    float distance = 0;
    if (focusTarget(selection, target, distance)) frameTarget(target, distance);
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
    const float aspect = float(renderer_.width()) / float(std::max<uint32_t>(1, renderer_.height()));
    const float tanHalf = std::tan(camera_.fovY * 0.5f);
    origin = camera_.position;
    direction = normalized(add(camera_.forward(), add(scaled(camera_.right(), ndcX * tanHalf * aspect), scaled(camera_.up(), ndcY * tanHalf))));
}

float SceneView::groundHeight(float x, float z, float fallback) const {
    float h = fallback;
    if (map_.hasTerrain && map_.terrain.heightAtWorld(x, z, h)) return h;
    return fallback;
}

bool SceneView::groundPoint(const tr::Vec3& origin, const tr::Vec3& direction, float fallbackY, tr::Vec3& point) const {
    if (map_.hasTerrain) {
        // March along the ray (finer near the camera), then refine the crossing by bisection.
        float previous = 0.0f;
        const float maxDistance = std::max(drawDistance * 1.5f, 500.0f);
        for (float t = 0.5f; t < maxDistance; t += std::max(0.5f, t * 0.01f)) {
            const tr::Vec3 p = add(origin, scaled(direction, t));
            float h;
            if (!map_.terrain.heightAtWorld(p.x, p.z, h) || p.y > h) {
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

void SceneView::setServerContent(const WorldContent* content, const WorldDatabase* database, std::function<std::string(int32_t)> meshForClass,
                                 AvatarResolver avatarFor) {
    serverContent_ = content;
    serverDatabase_ = database;
    serverMeshForClass_ = meshForClass;
    serverAvatarFor_ = avatarFor;
    serverPlacements_.clear();
    if (!content) return;
    auto yawPlacement = [](const tr::Vec3& position, float rotation) {
        return placementMatrix(position, tr::Quat{0, std::sin(rotation * 0.5f), 0, std::cos(rotation * 0.5f)}, 1.0f);
    };
    for (size_t k = 0; k < content->spawnPools.size(); ++k) {
        const SpawnPool& pool = content->spawnPools[k];
        const Creature* creature = database ? database->creature(pool.slot1.creatureId) : nullptr;
        const bool npc = creature && database->classIsNpc(creature->classId);
        const bool vendor = creature && database->classIsVendor(creature->classId);
        ServerPlacement s;
        s.selection = Selection{SelectionKind::SpawnPool, k};
        s.position = pool.position;
        s.world = yawPlacement(pool.position, pool.rotation);
        s.model = creature ? modelFor(meshForClass(creature->classId)) : nullptr;
        if (creature && avatarFor) {
            const AvatarLook look = avatarFor(*creature);
            if (!look.baseMesh.empty()) s.model = modelFor(look.baseMesh, look.poseAnimation);
            s.skinHue = hueFromAbgr(look.skinColour);
            for (const AvatarPart& part : look.parts)
                s.avatarParts.push_back(AvatarDraw{modelFor(part.mesh, look.poseAnimation, look.baseMesh), part.swapsetId, hueFromAbgr(part.colour)});
        }
        s.colour = vendor ? Vec4f{1.0f, 0.8f, 0.2f, 1} : npc ? Vec4f{0.35f, 1.0f, 0.45f, 1} : Vec4f{1.0f, 0.35f, 0.3f, 1};
        s.ringRadius = std::max(pool.radius, 1.5f);
        s.facing = true;
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->teleporters.size(); ++k) {
        const Teleporter& t = content->teleporters[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Teleporter, k};
        s.position = t.position;
        s.world = yawPlacement(t.position, t.rotation);
        s.model = modelFor(meshForClass(t.classId));
        s.colour = Vec4f{0.3f, 0.9f, 1.0f, 1};
        s.ringRadius = 2.0f;
        s.facing = true;
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->objects.size(); ++k) {
        const PlacedObject& o = content->objects[k];
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Object, k};
        s.position = o.position;
        s.world = yawPlacement(o.position, o.rotation);
        s.model = modelFor(meshForClass(o.classId));
        s.colour = Vec4f{1.0f, 0.95f, 0.3f, 1};
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
        s.colour = l.enabled ? Vec4f{1.0f, 0.35f, 1.0f, 1} : Vec4f{0.6f, 0.4f, 0.6f, 1};
        s.ringRadius = std::max(l.radius, 1.0f);
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->regions.size(); ++k) {
        ServerPlacement s;
        s.selection = Selection{SelectionKind::Region, k};
        s.area = true;
        s.shape = content->regions[k].area;
        s.position = s.shape.position;
        s.colour = content->regions[k].underground ? Vec4f{0.6f, 0.5f, 0.95f, 1} : Vec4f{0.4f, 0.85f, 0.4f, 1};
        serverPlacements_.push_back(s);
    }
    for (size_t k = 0; k < content->missionAreas.size(); ++k) {
        ServerPlacement s;
        s.selection = Selection{SelectionKind::MissionArea, k};
        s.area = true;
        s.shape = content->missionAreas[k].area;
        s.position = s.shape.position;
        s.colour = Vec4f{1.0f, 0.6f, 0.2f, 1};
        serverPlacements_.push_back(s);
    }
}

std::string SceneView::modelStatus(size_t entityIndex) const {
    if (entityIndex >= placements_.size()) return "";
    const Placement& p = placements_[entityIndex];
    if (!p.classKnown) return "unknown class (not in the game data)";
    if (!p.model) return "class has no model";
    const Model& m = *p.model;
    if (m.failed) return "model failed to load: " + m.failure;
    if (!m.loaded) return "loading...";
    if (m.effectOnly) return "effect: additive pieces only (drawn with Effects on; a cyan box with Placeholders)";
    if (!m.hasSolid) return "model has no geometry";
    return m.hasWater ? "water surface" : "model";
}
