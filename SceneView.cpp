#include "SceneView.h"
#include "GpuTexture.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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
        p.model = modelFor(mesh);                       // unordered_map nodes are stable
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
void SceneView::loadModel(const std::string& name, Model& model) {
    try {
        const tr::GeoModel geo = tr::readGeo(assets_.read(name));
        size_t effectPieces = 0;
        for (const tr::GeoPiece& piece : geo.pieces) {
            // Additive quads (light cones, mist and splash emitters) are effect placeholders, not geometry:
            // the game draws particles for them. They are counted so the entity can be shown as an effect.
            const std::string effect = tr::toLowerAscii(piece.effectName);
            if (effect.find("additive") != std::string::npos) {
                ++effectPieces;
                continue;
            }
            const bool water = effect.find("water") != std::string::npos;
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
            std::vector<MeshVertex> vertices;
            vertices.reserve(piece.vertices.size());
            for (const tr::GeoVertex& gv : piece.vertices) {
                const tr::Vec3 p = transformPoint(boneMatrix, gv.position);
                const tr::Vec3 n = subtract(transformPoint(boneMatrix, gv.normal), transformPoint(boneMatrix, tr::Vec3{0, 0, 0}));
                MeshVertex v;
                v.positionX = p.x; v.positionY = p.y; v.positionZ = p.z;
                v.normalX = n.x; v.normalY = n.y; v.normalZ = n.z;
                v.colour = packColour(255, 255, 255, 255);
                v.texU = gv.uv.x;
                v.texV = gv.uv.y;
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
                model.pieces.push_back(part);
                const uint32_t base = uint32_t(model.positions.size());
                for (const MeshVertex& v : vertices) model.positions.push_back(tr::Vec3{v.positionX, v.positionY, v.positionZ});
                for (size_t k = 0; k + 2 < piece.indices.size(); k += 3)
                    if (piece.indices[k] < vertices.size() && piece.indices[k + 1] < vertices.size() && piece.indices[k + 2] < vertices.size())
                        model.triangles.insert(model.triangles.end(), {base + piece.indices[k], base + piece.indices[k + 1], base + piece.indices[k + 2]});
            }
        }
        model.effectOnly = model.pieces.empty() && effectPieces > 0;
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
    renderer_.beginFrame(viewProjection, normalized(tr::Vec3{-0.35f, 0.85f, -0.4f}), SDL_FColor{0.55f, 0.66f, 0.78f, 1.0f});
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
            if (p.model && p.model->loaded && !p.model->pieces.empty()) {
                const Vec4f tint = selected ? Vec4f{1.0f, 0.9f, 0.45f, 1} : dimMapEntities ? Vec4f{0.55f, 0.55f, 0.58f, 1}
                                   : showTextures ? Vec4f{1, 1, 1, 1} : Vec4f{0.8f, 0.8f, 0.8f, 1};
                for (const ModelPiece& piece : p.model->pieces) {
                    if (piece.water) renderer_.drawTranslucent(piece.mesh, p.world, Vec4f{0.22f, 0.42f, 0.58f, selected ? 0.75f : 0.55f});
                    else renderer_.draw(piece.mesh, p.world, tint, true, showTextures ? piece.texture : nullptr, showTextures && piece.alphaTest);
                }
            } else if (p.model && p.model->loaded && p.model->effectOnly) {
                // Particle effect (mist, smoke, splashes, holograms): cyan wireframe of its bounds.
                if (showEffects && p.hasBounds && !selected) renderer_.draw(unitBoxLines_, p.boxWorld, Vec4f{0.3f, 0.9f, 1.0f, 1}, false);
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
        const bool hasModel = s.model && s.model->loaded && !s.model->pieces.empty();
        if (hasModel) {
            const Vec4f tint = selected ? Vec4f{1.0f, 0.9f, 0.45f, 1} : Vec4f{1, 1, 1, 1};
            for (const ModelPiece& piece : s.model->pieces) {
                if (piece.water) renderer_.drawTranslucent(piece.mesh, s.world, Vec4f{0.22f, 0.42f, 0.58f, 0.55f});
                else renderer_.draw(piece.mesh, s.world, tint, true, showTextures ? piece.texture : nullptr, showTextures && piece.alphaTest);
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

void SceneView::focusOn(const Selection& selection) {
    if (selection.kind == SelectionKind::MapEntity) {
        if (selection.index >= placements_.size()) return;
        const Placement& p = placements_[selection.index];
        camera_.position = subtract(p.centre, scaled(camera_.forward(), std::max(8.0f, p.radius * 3.0f)));
        return;
    }
    for (const ServerPlacement& s : serverPlacements_) {
        if (!sameSelection(s.selection, selection)) continue;
        const tr::Vec3 centre = s.area ? s.shape.position : tr::Vec3{s.position.x, s.position.y + 1.0f, s.position.z};
        const float size = s.area ? std::max(s.shape.radius, std::max(s.shape.halfX, s.shape.halfZ)) : std::max(s.ringRadius, 2.0f);
        camera_.position = subtract(centre, scaled(camera_.forward(), std::max(10.0f, size * 2.5f)));
        return;
    }
}

SceneView::Model* SceneView::modelFor(const std::string& mesh) {
    if (mesh.empty()) return nullptr;
    auto inserted = models_.emplace(tr::toLowerAscii(mesh), Model());
    if (inserted.second) pending_.push_back(inserted.first->first);     // loaded progressively
    return &inserted.first->second;
}

void SceneView::refreshServerContent() { setServerContent(serverContent_, serverDatabase_, serverMeshForClass_); }

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

void SceneView::setServerContent(const WorldContent* content, const WorldDatabase* database, std::function<std::string(int32_t)> meshForClass) {
    serverContent_ = content;
    serverDatabase_ = database;
    serverMeshForClass_ = meshForClass;
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
    if (m.effectOnly) return "particle effect (no geometry; shown as a cyan box)";
    if (m.pieces.empty()) return "model has no geometry";
    return m.hasWater ? "water surface" : "model";
}
