// Headless viewport test: renders a map with the editor's Renderer/SceneView into an offscreen
// target and writes the frame as raw RGBA. Runs on any Vulkan driver, including Mesa's lavapipe.
//   viewport_test <game root> <game.zip> <map> <out.rgba> <width> <height> <eye x y z> <target x y z>
#include "Avatar.h"
#include "SceneView.h"
#include "gamedata.h"
#include "WorldDatabase.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <string>

// An environment variable's value, or nullptr when it is unset or empty (PowerShell leaves cleared variables empty).
static const char* envValue(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

int main(int argc, char** argv) {
    if (argc < 13) { std::puts("usage: viewport_test root game.zip map out.rgba w h ex ey ez tx ty tz"); return 1; }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GPUDevice* device = createGpuDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, true);
    if (!device) { std::printf("SDL_CreateGPUDevice: %s\n", SDL_GetError()); return 1; }
    std::printf("GPU driver: %s, BC1 textures %s\n", SDL_GetGPUDeviceDriver(device),
                SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER) ? "supported" : "not supported");
    const uint32_t width = uint32_t(std::atoi(argv[5])), height = uint32_t(std::atoi(argv[6]));
    std::setvbuf(stdout, nullptr, _IONBF, 0);          // keep the output if something goes wrong
    try {
        Renderer renderer(device);
        renderer.resize(width, height);
        tr::AssetLibrary assets(argv[1]);
        tr::GameData data(argv[2]);
        const tr::MapBundle map = tr::loadMap(assets, argv[3]);
        SceneView scene(renderer, assets, map, [&](int32_t id) { return data.classMeshName(id); },
                        [&](int32_t id) { return !data.className(id).empty(); }, [&](int32_t id) { return data.classCullingLayer(id); },
                        envValue("RASA_TERRAINSTEP") ? uint32_t(std::atoi(envValue("RASA_TERRAINSTEP"))) : 0u);   // 0 = automatic
        {
            std::vector<std::pair<int32_t, std::string>> regionSkies;
            for (const tr::MapRegion& r : map.map.regions)
                if (r.sky > 0) regionSkies.emplace_back(r.regionId, data.stringById(r.sky));
            if (envValue("RASA_LISTHUES")) {             // distinct per-entity map hues (hue1 / hue2 RGBA bytes)
                std::map<std::string, int> counts;
                for (const tr::MapEntity& e : map.map.entities) {
                    char key[64];
                    std::snprintf(key, sizeof key, "%3u %3u %3u %3u / %3u %3u %3u %3u", e.hue1.red, e.hue1.green, e.hue1.blue, e.hue1.alpha,
                                  e.hue2.red, e.hue2.green, e.hue2.blue, e.hue2.alpha);
                    ++counts[key];
                }
                for (const auto& c : counts) std::printf("map hue %s: %d entities\n", c.first.c_str(), c.second);
            }
            if (envValue("RASA_LISTREGIONS"))
                for (const tr::MapRegion& r : map.map.regions) {
                    const auto name = [&](int32_t id) -> std::string {
                        if (id <= 0) return "";
                        try { return data.stringById(id); } catch (const std::exception&) { return "?"; }
                    };
                    std::printf("region %d priority %d flags 0x%x env '%s' sky '%s'\n", r.regionId, r.priority, unsigned(r.enabledFlags),
                                name(r.environmentMap).c_str(), name(r.sky).c_str());
                }
            scene.setRegionSkies(regionSkies);
            std::vector<std::pair<int32_t, std::string>> regionEnvironmentMaps;
            for (const tr::MapRegion& r : map.map.regions)
                regionEnvironmentMaps.emplace_back(r.regionId, r.environmentMap > 0 ? data.stringById(r.environmentMap) : std::string());
            scene.setRegionEnvironmentMaps(regionEnvironmentMaps);
            scene.setClassHues([&](int32_t classId, tr::HueColour& hue1, tr::HueColour& hue2) { return data.classHue(classId, hue1, hue2); });
            scene.setUsableStatePackages([&](int32_t classId, int32_t stateId) { return data.usableStatePackage(classId, stateId); });
            // RASA_TIME=<fraction of the day>, RASA_SKY=<index>, RASA_NOSKY=1 / RASA_NOFOG=1 for lighting checks.
            if (const char* t = envValue("RASA_TIME")) scene.timeOfDay = float(std::atof(t));
            if (const char* k = envValue("RASA_SKY")) { scene.skyIndex = size_t(std::atoi(k)); scene.skyByRegion = false; }
            if (envValue("RASA_NOSKY")) scene.skyLighting = false;
            if (envValue("RASA_NOFOG")) scene.showFog = false;
            if (envValue("RASA_NOSUN")) scene.showSun = false;
            if (envValue("RASA_NOSHADOWS")) scene.showShadows = false;
            if (const char* g = envValue("RASA_LIGHTGAMMA")) scene.lightColourGamma = float(std::atof(g));
            if (envValue("RASA_NOLIGHTS")) scene.showMapLights = false;
            if (envValue("RASA_SHOWLIGHT")) scene.showLightBuffer = true;
            if (envValue("RASA_NOENV")) scene.environmentMaps = false;
            if (const char* lt = envValue("RASA_LIGHTTIME")) scene.lightTime = float(std::atof(lt));   // light modifier time, s
            if (envValue("RASA_NOLIGHTMODS")) scene.animateLights = false;
            if (envValue("RASA_SHOWNORMALS")) scene.debugView = 2;                  // like the game's /dl_shownormals
            if (envValue("RASA_SHOWSPEC")) scene.debugView = 3;
            if (envValue("RASA_SHOWDIFFUSE")) scene.debugView = 4;                  // like the game's /dl_showdiffuse                     // like the game's /dl_showspecintensity     // like the game's /dl_showlight
            if (const char* a = envValue("RASA_AMBIENT")) scene.ambientScale = float(std::atof(a));
            if (envValue("RASA_NOMAPS")) scene.materialMaps = false;                // normal / gloss maps off   // scales the sky hemi light
            std::printf("map lights: %zu%s%s\n", scene.mapLights().size(), scene.mapLightsError().empty() ? "" : " - ", scene.mapLightsError().c_str());
            for (size_t k = 0; k < scene.skies().size(); ++k) {
                const SceneView::SkyChoice& c = scene.skies()[k];
                std::string regions;
                for (int32_t r : c.regions) regions += (regions.empty() ? "" : " ") + std::to_string(r);
                std::printf("sky %zu%s: %s (regions: %s)%s%s\n", k, k == scene.skyIndex ? " *" : "", c.name.c_str(), regions.c_str(),
                            c.loaded ? "" : " - not usable: ", c.error.c_str());
            }
            const SceneLighting l = scene.lighting();
            std::printf("time %.3f: hemi top %.2f %.2f %.2f bottom %.2f %.2f %.2f, sun %.2f %.2f %.2f towards %.2f %.2f %.2f, fog %s from %.0f m density %.4f, %d placed lights in use\n",
                        scene.timeOfDay, l.hemiTop.x, l.hemiTop.y, l.hemiTop.z, l.hemiBottom.x, l.hemiBottom.y, l.hemiBottom.z, l.sunFront.x,
                        l.sunFront.y, l.sunFront.z, l.towardsLight.x, l.towardsLight.y, l.towardsLight.z, l.fog ? "on" : "off", l.fogStart, l.fogDensity, l.localCount);
        }
        // RASA_GEOINFO=<model.geo>: per piece its vertex elements, vertex colour range (ARGB bytes) and material parameters.
        if (const char* geoName = envValue("RASA_GEOINFO")) {
            const tr::GeoModel geo = tr::readGeo(assets.read(geoName));
            for (size_t k = 0; k < geo.pieces.size(); ++k) {
                const tr::GeoPiece& piece = geo.pieces[k];
                uint32_t lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
                tr::Vec3 pmin{1e30f, 1e30f, 1e30f}, pmax{-1e30f, -1e30f, -1e30f};
                for (const tr::GeoVertex& v : piece.vertices) {
                    pmin = tr::Vec3{std::min(pmin.x, v.position.x), std::min(pmin.y, v.position.y), std::min(pmin.z, v.position.z)};
                    pmax = tr::Vec3{std::max(pmax.x, v.position.x), std::max(pmax.y, v.position.y), std::max(pmax.z, v.position.z)};
                }
                for (const tr::GeoVertex& v : piece.vertices)
                    for (int c = 0; c < 4; ++c) {
                        const uint32_t byte = (v.colour >> (24 - 8 * c)) & 0xFF;
                        lo[c] = std::min(lo[c], byte);
                        hi[c] = std::max(hi[c], byte);
                    }
                std::printf("piece %zu %s (%zu vertices): colour A %u-%u R %u-%u G %u-%u B %u-%u; elements", k, piece.effectName.c_str(),
                            piece.vertices.size(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3]);
                for (const tr::VertexElement& e : piece.layout) std::printf(" usage%u.%u type%u", e.usage, e.usageIndex, e.type);
                std::printf("\n    bone %s, stored positions (%.2f %.2f %.2f) - (%.2f %.2f %.2f)\n", piece.boneName.c_str(), pmin.x, pmin.y, pmin.z,
                            pmax.x, pmax.y, pmax.z);
                // RASA_GEOVERTS=<piece>: every vertex of that piece (position, normal, uv, uv1, ARGB colour).
                if (const char* dump = envValue("RASA_GEOVERTS"); dump && size_t(std::atoi(dump)) == k)
                    for (const tr::GeoVertex& v : piece.vertices)
                        std::printf("    v %.3f %.3f %.3f n %.3f %.3f %.3f uv %.3f %.3f uv1 %.3f %.3f c %08x\n", v.position.x, v.position.y, v.position.z,
                                    v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y, v.uv1.x, v.uv1.y, v.colour);
                // Winding: Direct3D's default cull (CCW) keeps triangles that are clockwise on screen, whose face normal
                // in this left-handed space is cross(b - a, c - a). Count those facing away from the piece's centre and
                // those agreeing with the stored vertex normals.
                const tr::Vec3 centre = scaled(add(pmin, pmax), 0.5f);
                size_t outward = 0, agree = 0, triangles = 0;
                for (size_t t = 0; t + 2 < piece.indices.size(); t += 3) {
                    const uint32_t i0 = piece.indices[t], i1 = piece.indices[t + 1], i2 = piece.indices[t + 2];
                    if (i0 >= piece.vertices.size() || i1 >= piece.vertices.size() || i2 >= piece.vertices.size()) continue;
                    const tr::Vec3& a = piece.vertices[i0].position;
                    const tr::Vec3& b = piece.vertices[i1].position;
                    const tr::Vec3& c = piece.vertices[i2].position;
                    const tr::Vec3 n = cross(subtract(b, a), subtract(c, a));
                    const tr::Vec3 mid = scaled(add(add(a, b), c), 1.0f / 3.0f);
                    const tr::Vec3 stored = add(add(piece.vertices[i0].normal, piece.vertices[i1].normal), piece.vertices[i2].normal);
                    ++triangles;
                    if (dot(n, subtract(mid, centre)) > 0) ++outward;
                    if (dot(n, stored) > 0) ++agree;
                }
                std::printf("    triangles %zu: %zu face away from the centre, %zu agree with the vertex normals\n", triangles, outward, agree);
                for (const tr::GeoParameter& p : piece.parameters) std::printf("    %s = %s\n", p.key.c_str(), p.value.c_str());
            }
        }
        // RASA_PKGSCAN=1: parse every special-FX package; counts of accessory / particle / light events and failures.
        if (envValue("RASA_PKGSCAN")) {
            size_t packages = 0, failed = 0, accessories = 0, particles = 0, lights = 0;
            for (const std::string& name : assets.namesWithExtension(".pkg")) {
                ++packages;
                try {
                    const tr::SfxPackage p = tr::readSfxPackage(assets.read(name));
                    accessories += p.accessories.size();
                    particles += p.particles.size();
                    lights += p.lights.size();
                } catch (const std::exception& e) {
                    if (++failed <= 5) std::printf("pkg %s: %s\n", name.c_str(), e.what());
                }
            }
            std::printf("pkg scan: %zu packages, %zu failed; %zu accessories, %zu particle attaches, %zu lights\n", packages, failed,
                        accessories, particles, lights);
        }
        // RASA_GEOSCAN=1: every model's pieces whose vertex colours are all zero (colour and alpha), with names.
        if (envValue("RASA_GEOSCAN")) {
            std::map<std::string, size_t> byName;
            for (const std::string& name : assets.namesWithExtension(".geo")) {
                tr::GeoModel geo;
                try {
                    geo = tr::readGeo(assets.read(name));
                } catch (const std::exception&) {
                    continue;
                }
                for (const tr::GeoPiece& piece : geo.pieces) {
                    if (piece.vertices.empty()) continue;
                    bool hasColour = false, allZero = true;
                    for (const tr::VertexElement& e : piece.layout) hasColour = hasColour || (e.usage == 10 && e.usageIndex == 0);
                    for (const tr::GeoVertex& v : piece.vertices) allZero = allZero && v.colour == 0;
                    if (!hasColour || !allZero) continue;
                    ++byName[tr::toLowerAscii(piece.boneName) + " | " + piece.effectName];
                    std::printf("zero-colour piece: %s bone '%s' group '%s' %s (%zu vertices)\n", name.c_str(), piece.boneName.c_str(),
                                piece.swapGroup.c_str(), piece.effectName.c_str(), piece.vertices.size());
                }
            }
            for (const auto& n : byName) std::printf("zero-colour summary: %4zu  %s\n", n.second, n.first.c_str());
        }
        // RASA_NEAR=x,z,radius: map entities within the radius (horizontal), with model, position and scale.
        if (const char* near = envValue("RASA_NEAR")) {
            float x = 0, z = 0, radius = 20;
            std::sscanf(near, "%f,%f,%f", &x, &z, &radius);
            for (size_t k = 0; k < map.map.entities.size(); ++k) {
                const tr::MapEntity& e = map.map.entities[k];
                const float dx = e.location.x - x, dz = e.location.z - z;
                if (dx * dx + dz * dz > radius * radius) continue;
                std::printf("near %zu %s at %.1f %.1f %.1f scale %.2f\n", k, data.classMeshName(e.classId).c_str(), e.location.x, e.location.y, e.location.z,
                            e.scale);
            }
        }
        // RASA_FINDFEATURES=1: map entities whose models use material features worth checking in game - TR texture transforms
        // with an offset in the third row, TR environment reflection (MatSpecular.z > 0), modifier blending, decals on UV
        // set 2, TRHair. One line per (feature, model) with the nearest-to-first entity position and the entity count.
        if (envValue("RASA_FINDFEATURES")) {
            std::map<std::string, tr::GeoModel> geos;
            std::map<std::string, std::pair<size_t, tr::Vec3>> found;      // "feature | model | detail" -> count, first position
            for (const tr::MapEntity& e : map.map.entities) {
                const std::string mesh = tr::toLowerAscii(data.classMeshName(e.classId));
                if (mesh.empty() || mesh == "default.geo") continue;
                auto it = geos.find(mesh);
                if (it == geos.end()) {
                    tr::GeoModel geo;
                    try { geo = tr::readGeo(assets.read(mesh)); } catch (const std::exception&) {}
                    it = geos.emplace(mesh, std::move(geo)).first;
                }
                for (const tr::GeoPiece& piece : it->second.pieces) {
                    const std::string effect = tr::toLowerAscii(piece.effectName);
                    if (effect.rfind("tr", 0) != 0 || effect.find("additive") != std::string::npos) continue;
                    std::string phase;
                    for (const tr::GeoParameter& gp : piece.parameters)
                        if (gp.key == "Phase") phase = tr::toLowerAscii(gp.value);
                    if (phase == "additive" || phase == "translucent") continue;
                    std::vector<std::string> features;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        if (gp.key.rfind("TexCoordTransform", 0) == 0) {
                            float m[16] = {};
                            std::istringstream values(gp.value);
                            for (float& x : m) values >> x;
                            if (m[8] != 0 || m[9] != 0) features.push_back("offset transform " + gp.key + " = " + gp.value);
                        }
                        if (gp.key == "MatSpecular" && effect.find("env") != std::string::npos) {
                            float s[4] = {};
                            std::istringstream values(gp.value);
                            for (float& x : s) values >> x;
                            if (s[2] > 0.2f) features.push_back("environment reflection, MatSpecular " + gp.value);
                        }
                        if (gp.key == "DecalTexCoord" && gp.value == "2") features.push_back("decal on UV set 2");
                    }
                    if (effect.find("diffmapdiffmap") != std::string::npos) features.push_back("modifier blend (second diffuse by vertex alpha)");
                    if (effect.rfind("trhair", 0) == 0) features.push_back("hair cut-out");
                    for (const std::string& f : features) {
                        auto& slot = found[f.substr(0, f.find(',') == std::string::npos ? f.size() : f.size()) + " | " + mesh + " | " + piece.effectName];
                        if (slot.first++ == 0) slot.second = e.location;
                    }
                }
            }
            for (const auto& f : found)
                std::printf("feature %s | %zu entities, e.g. at %.1f %.1f %.1f\n", f.first.c_str(), f.second.first, f.second.second.x, f.second.second.y,
                            f.second.second.z);
        }
        // RASA_PARAMSCAN=<effect prefix>: value counts of every material parameter of pieces whose effect starts with the
        // prefix (lower case, e.g. "trdiff"), over every model; texture names are summarised as "<texture>".
        if (const char* prefix = envValue("RASA_PARAMSCAN")) {
            std::map<std::string, std::map<std::string, size_t>> counts;     // "effect | key" -> value -> pieces
            for (const std::string& name : assets.namesWithExtension(".geo")) {
                tr::GeoModel geo;
                try {
                    geo = tr::readGeo(assets.read(name));
                } catch (const std::exception&) {
                    continue;
                }
                for (const tr::GeoPiece& piece : geo.pieces) {
                    const std::string effect = tr::toLowerAscii(piece.effectName);
                    if (effect.rfind(prefix, 0) != 0) continue;
                    for (const tr::GeoParameter& gp : piece.parameters) {
                        const bool isTexture = gp.key.size() > 7 && gp.key.compare(gp.key.size() - 7, 7, "Texture") == 0;
                        ++counts[effect + " | " + gp.key][isTexture ? (gp.value.empty() ? "<none>" : "<texture>") : gp.value];
                    }
                }
            }
            for (const auto& c : counts) {
                std::vector<std::pair<size_t, std::string>> values;
                for (const auto& v : c.second) values.emplace_back(v.second, v.first);
                std::sort(values.rbegin(), values.rend());
                std::printf("%s:", c.first.c_str());
                for (size_t k = 0; k < values.size() && k < 6; ++k) std::printf("  [%zu] %s", values[k].first, values[k].second.c_str());
                if (values.size() > 6) std::printf("  (+%zu more)", values.size() - 6);
                std::printf("\n");
            }
        }
        // RASA_LISTPARTICLES=1: the map entities whose models carry particle systems, with their positions.
        if (envValue("RASA_LISTPARTICLES")) {
            std::map<std::string, std::vector<tr::ParticleAttachment>> byMesh;
            for (const tr::MapEntity& e : map.map.entities) {
                const std::string mesh = data.classMeshName(e.classId);
                if (mesh.empty() || byMesh.count(mesh)) continue;
                try {
                    byMesh[mesh] = tr::particleAttachments(tr::readGeo(assets.read(mesh)));
                } catch (const std::exception&) {
                    byMesh[mesh] = {};
                }
            }
            for (size_t k = 0; k < map.map.entities.size(); ++k) {
                const tr::MapEntity& e = map.map.entities[k];
                const auto found = byMesh.find(data.classMeshName(e.classId));
                if (found == byMesh.end()) continue;
                for (const tr::ParticleAttachment& a : found->second)
                    std::printf("particles: entity %zu %s at %.1f %.1f %.1f: cp %u %s\n", k, found->first.c_str(), e.location.x, e.location.y,
                                e.location.z, a.contactPoint, a.file.c_str());
            }
        }
        // Optional server content: set RASA_DB to a Rasa.NET World database.
        std::unique_ptr<WorldDatabase> database;
        WorldContent content;
        if (const char* dbPath = envValue("RASA_DB")) {
            database = std::make_unique<WorldDatabase>(dbPath);
            content = database->loadMap(database->contextIdForMap(map.name));
            scene.setServerContent(&content, database.get(), [&](int32_t id) { return data.classMeshName(id); },
                                   [&](const Creature& c) { return resolveAvatar(data, *database, c); });
            // RASA_SAVETEST=1 (use a COPY of the database): edit the arrival points - move the first, add one to its pool, delete
            // the last - save, reload and check the rows; then restore and save again.
            if (envValue("RASA_SAVETEST") && content.arrivals.size() >= 2) {
                const WorldContent original = content;
                WorldContent edited = content;
                edited.arrivals.front().position.x += 1.5f;
                edited.arrivals.front().comment += " (moved)";
                SpawnArrival added = edited.arrivals.front();
                added.id = 0;
                added.kind = 2;
                added.entityId = 1234567;
                added.comment = "save test";
                edited.arrivals.push_back(added);
                const int64_t deletedId = edited.arrivals[edited.arrivals.size() - 2].id;
                edited.arrivals.erase(edited.arrivals.end() - 2);
                WorldDatabase::SaveSummary summary;
                std::string error;
                const bool saved = database->save(original, edited, summary, error);
                const WorldContent reloaded = database->loadMap(content.mapContextId);
                bool moved = false, addedFound = false, deletedGone = true;
                for (const SpawnArrival& r : reloaded.arrivals) {
                    if (r.id == original.arrivals.front().id) moved = std::fabs(r.position.x - edited.arrivals.front().position.x) < 1e-3f &&
                                                                    r.comment == edited.arrivals.front().comment;
                    if (r.comment == "save test" && r.kind == 2 && r.entityId == 1234567 && r.poolId == added.poolId) addedFound = true;
                    if (r.id == deletedId) deletedGone = false;
                }
                std::printf("save test: saved %d (%d added, %d changed, %d deleted%s%s), moved %d, added %d, deleted %d, %zu -> %zu arrivals\n", saved,
                            summary.inserted, summary.updated, summary.deleted, error.empty() ? "" : ": ", error.c_str(), moved, addedFound,
                            deletedGone, original.arrivals.size(), reloaded.arrivals.size());
                // Undo: delete the added row, restore the moved and deleted ones.
                WorldContent restore = reloaded;
                restore.arrivals.erase(std::remove_if(restore.arrivals.begin(), restore.arrivals.end(),
                                                      [](const SpawnArrival& r) { return r.comment == "save test"; }),
                                       restore.arrivals.end());
                for (SpawnArrival& r : restore.arrivals)
                    if (r.id == original.arrivals.front().id) r = original.arrivals.front();
                for (const SpawnArrival& r : original.arrivals)
                    if (r.id == deletedId) { SpawnArrival back = r; back.id = 0; restore.arrivals.push_back(back); }
                const bool restored = database->save(reloaded, restore, summary, error);
                std::printf("save test restore: %d (%d added, %d changed, %d deleted)\n", restored, summary.inserted, summary.updated, summary.deleted);
                content = database->loadMap(content.mapContextId);
                scene.setServerContent(&content, database.get(), [&](int32_t id) { return data.classMeshName(id); },
                                       [&](const Creature& c) { return resolveAvatar(data, *database, c); });
            }
            // RASA_SAVETEST also round-trips the creature definitions: change the first creature with stats and appearance (level,
            // stat row removed, a slot recoloured), add a new creature, save, reload and check (the copy keeps the changes).
            if (envValue("RASA_SAVETEST")) {
                const std::vector<CreatureDefinition> before = database->creatureDefinitions();
                std::vector<CreatureDefinition> edited = before;
                CreatureDefinition* target = nullptr;
                for (CreatureDefinition& d : edited)
                    if (d.hasStat && !d.appearance.empty()) { target = &d; break; }
                if (target) {
                    const int32_t id = target->creature.id;
                    target->creature.level += 1;
                    target->creature.action8 = database->creatureActions().empty() ? 0 : database->creatureActions().front().id;
                    target->hasStat = false;
                    target->appearance.front().colour = 0xff112233;
                    const int32_t expectLevel = target->creature.level, expectAction8 = target->creature.action8;
                    CreatureDefinition added = *target;
                    added.creature.id = edited.back().creature.id + 1;
                    added.creature.comment = "save test creature";
                    added.hasStat = true;
                    added.stat = CreatureStat{1, 2, 3, 4, 5};
                    edited.push_back(added);
                    WorldDatabase::SaveSummary summary;
                    std::string error;
                    const bool saved = database->saveCreatures(before, edited, summary, error);
                    const Creature* c = database->creature(id);
                    const Creature* n = database->creature(added.creature.id);
                    const CreatureStat* ns = database->creatureStat(added.creature.id);
                    const bool ok = c && c->level == expectLevel && c->action8 == expectAction8 && !database->creatureStat(id) &&
                                    !database->appearance(id).empty() && database->appearance(id).front().colour == 0xff112233u && n &&
                                    n->comment == "save test creature" && ns && ns->armor == 5 &&
                                    database->appearance(added.creature.id).size() == added.appearance.size();
                    std::printf("creature save test: saved %d (%d added, %d changed, %d deleted%s%s), checks %s\n", saved, summary.inserted,
                                summary.updated, summary.deleted, error.empty() ? "" : ": ", error.c_str(), ok ? "passed" : "FAILED");
                }
                // Generic tables (missions): edit, delete and add rows of two tables, save, reload and compare row for row.
                {
                    const DbTable objectives = database->loadTable("mission_objective_definition");
                    const DbTable triggers = database->loadTable("mission_trigger");
                    DbTable objectives2 = objectives, triggers2 = triggers;
                    const int comment = objectives2.column("comment"), objectiveId = objectives2.column("objective_id");
                    objectives2.rows.front()[size_t(comment)] = DbValue::text("generic save test");
                    std::vector<DbValue> added = objectives2.rows.front();
                    added[size_t(objectiveId)] = DbValue::integer(9999);
                    objectives2.rows.push_back(added);
                    triggers2.rows.erase(triggers2.rows.begin());
                    WorldDatabase::SaveSummary summary;
                    std::string error;
                    const bool saved = database->saveTables({{&objectives, &objectives2}, {&triggers, &triggers2}}, summary, error);
                    const DbTable objectives3 = database->loadTable("mission_objective_definition");
                    const DbTable triggers3 = database->loadTable("mission_trigger");
                    auto sameRows = [](const DbTable& a, const DbTable& b) {
                        if (a.rows.size() != b.rows.size()) return false;
                        for (const auto& row : a.rows)
                            if (std::find(b.rows.begin(), b.rows.end(), row) == b.rows.end()) return false;
                        return true;
                    };
                    std::printf("generic table save test: saved %d (%d added, %d changed, %d deleted%s%s), objectives %s, triggers %s; key %zu columns\n",
                                saved, summary.inserted, summary.updated, summary.deleted, error.empty() ? "" : ": ", error.c_str(),
                                sameRows(objectives2, objectives3) ? "match" : "DIFFER", sameRows(triggers2, triggers3) ? "match" : "DIFFER",
                                objectives.key.size());
                }
                // NPC / vendor rows: a vendor gets a flat price, loses its first stock item and gains one, a greeting and a package.
                std::vector<CreatureDefinition> before2 = database->creatureDefinitions();
                std::vector<CreatureDefinition> edited2 = before2;
                for (CreatureDefinition& d : edited2) {
                    if (!d.hasVendor || d.vendorItems.size() < 2 || d.hasVendorPrice) continue;
                    const int32_t id = d.creature.id;
                    std::vector<int32_t> stock(d.vendorItems.begin() + 1, d.vendorItems.end());
                    stock.push_back(13066);
                    d.vendorItems = stock;
                    d.hasVendorPrice = true;
                    d.vendorPrice = 77;
                    d.hasGreeting = true;
                    d.greetingId = 120;
                    d.hasNpcPackage = true;
                    d.npcPackageId = 113;
                    d.npcPackageComment = "save test package";
                    WorldDatabase::SaveSummary summary;
                    std::string error;
                    const bool saved = database->saveCreatures(before2, edited2, summary, error);
                    bool ok = false;
                    for (const CreatureDefinition& r : database->creatureDefinitions())
                        if (r.creature.id == id)
                            ok = r.hasVendorPrice && r.vendorPrice == 77 && r.vendorItems == stock && r.hasGreeting && r.greetingId == 120 &&
                                 r.hasNpcPackage && r.npcPackageId == 113 && r.npcPackageComment == "save test package";
                    std::printf("vendor save test (creature %d): saved %d (%d added, %d changed, %d deleted%s%s), checks %s; greeting 120 = \"%s\"\n", id,
                                saved, summary.inserted, summary.updated, summary.deleted, error.empty() ? "" : ": ", error.c_str(),
                                ok ? "passed" : "FAILED", data.npcGreeting(120).c_str());
                    break;
                }
            }
            // Avatar assembly of the first few spawned creatures that wear swap-set items.
            int listed = 0;
            for (const SpawnPool& pool : content.spawnPools) {
                const Creature* c = database->creature(pool.slot1.creatureId);
                if (!c || listed >= 4) continue;
                const AvatarLook look = resolveAvatar(data, *database, *c);
                if (look.parts.empty()) continue;
                ++listed;
                std::printf("avatar %d %s: base %s, skin %08x, pose %s\n", c->id, c->comment.c_str(), look.baseMesh.c_str(), look.skinColour,
                            look.poseAnimation.empty() ? "(bind)" : look.poseAnimation.c_str());
                for (const AvatarPart& part : look.parts)
                    std::printf("    slot %d -> swap set %u (%s) from %s, colour %08x\n", part.slotId, part.swapsetId,
                                data.swapsetName(int32_t(part.swapsetId)).c_str(), part.mesh.c_str(), part.colour);
                for (const AvatarAccessory& a : look.accessories)
                    std::printf("    accessory %s on connection point %d (%s)\n", a.mesh.c_str(), a.connectionPoint,
                                tr::connectionPointIdName(uint32_t(a.connectionPoint)).c_str());
            }
            std::printf("server content: %zu spawn pools, %zu teleporters, %zu objects, %zu links\n", content.spawnPools.size(),
                        content.teleporters.size(), content.objects.size(), content.mapLinks.size());
        }
        scene.loadPending(1e9);
        {
            size_t layer6 = 0;
            for (size_t k = 0; k < map.map.entities.size(); ++k) layer6 += scene.entityCullingLayer(k) == 6 ? 1 : 0;
            std::printf("entities on culling layer 6 (never drawn): %zu\n", layer6);
        }
        std::printf("models: %zu loaded, %zu failed, %zu total; textures %zu\n", scene.modelsLoaded(), scene.modelsFailed(), scene.modelsTotal(), scene.texturesLoaded());
        ViewCamera& cam = scene.camera();
        cam.position = tr::Vec3{float(std::atof(argv[7])), float(std::atof(argv[8])), float(std::atof(argv[9]))};
        const tr::Vec3 to = subtract(tr::Vec3{float(std::atof(argv[10])), float(std::atof(argv[11])), float(std::atof(argv[12]))}, cam.position);
        cam.yaw = std::atan2(to.x, to.z);
        cam.pitch = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));
        if (const char* fov = envValue("RASA_FOV")) cam.fovY = float(std::atof(fov)) * 3.14159265f / 180.0f;   // vertical, degrees
        if (argc > 13) scene.showPlaceholders = false;
        if (!scene.skies().empty()) {
            const SceneLighting active = scene.lighting();
            std::printf("sky at the camera: %s%s; hemi top %.2f %.2f %.2f, sun %.2f %.2f %.2f\n", scene.skies()[std::min(scene.activeSkyIndex(), scene.skies().size() - 1)].name.c_str(),
                        scene.skyFromRegion() ? " (by region)" : "", active.hemiTop.x, active.hemiTop.y, active.hemiTop.z, active.sunFront.x, active.sunFront.y, active.sunFront.z);
        }
        std::printf("environment cube at the camera: %s\n", scene.activeEnvironmentMap().c_str());
        for (const auto& e : scene.environmentMapErrors()) std::printf("environment cube %s failed: %s\n", e.first.c_str(), e.second.c_str());
        // Particle systems: started (with their warm-up) and run for RASA_PARTICLES seconds more (default 0.5);
        // RASA_NOPARTICLES=1 leaves them out.
        if (envValue("RASA_NOPARTICLES")) scene.showParticles = false;
        if (envValue("RASA_NOSTATEFX")) scene.showStateEffects = false;   // no use-state particles / lights of server objects
        if (envValue("RASA_NOTERRAIN")) scene.showTerrain = false;
        scene.terrainBuildsPerFrame = SIZE_MAX;      // a single frame: every chunk at its distance's detail
        if (envValue("RASA_TWOSIDED")) renderer.cullBackFaces = false;
        if (envValue("RASA_NOENTITIES")) scene.showEntities = false;
        const float particleSeconds = envValue("RASA_PARTICLES") ? float(std::atof(envValue("RASA_PARTICLES"))) : 0.5f;
        // Terrain clutter: every cell around the camera at once, models loaded before the frame (RASA_NOCLUTTER=1: none).
        if (envValue("RASA_NOCLUTTER")) scene.showClutter = false;
        if (const char* flora = envValue("RASA_FLORA")) scene.clutterDensity = float(std::atof(flora));   // density factor
        else {
            scene.clutterCellsPerFrame = 1000;
            scene.updateClutterCells();
            scene.loadPending(1e9);
            std::printf("terrain clutter: %zu instances around the camera\n", scene.clutterInstances());
            // RASA_LISTCLUTTER=<metres>: every clutter instance within that distance (horizontally) of the camera.
            if (const char* within = envValue("RASA_LISTCLUTTER")) {
                const float r = float(std::atof(within));
                const tr::Terrain& t = map.terrain;
                const uint32_t cellsX = tr::tribbleCellsX(t), cellsZ = tr::tribbleCellsZ(t);
                for (uint32_t cell = 0; cell < cellsX * cellsZ; ++cell) {
                    const float cx0 = t.header.bounds.min.x + float(cell % cellsX) * 32.0f * t.header.spacing;
                    const float cz0 = t.header.bounds.min.z + float(cell / cellsX) * 32.0f * t.header.spacing;
                    const tr::Vec3& c = scene.camera().position;
                    if (c.x < cx0 - r - 1 || c.x > cx0 + 33 + r || c.z < cz0 - r - 1 || c.z > cz0 + 33 + r) continue;
                    for (const tr::TribbleInstance& in : tr::placeTribbleCell(t, map.textureLayers, map.tribbleSets, cell)) {
                        const float dx = in.position.x - c.x, dz = in.position.z - c.z;
                        if (dx * dx + dz * dz > r * r) continue;
                        std::printf("clutter cell %u %s: %.2f %.2f %.2f scale %.2f set %s layer %u entry %u rank %u of %u\n", cell, map.tribbleSets[in.set].models[in.model].model.c_str(),
                                    in.position.x, in.position.y, in.position.z, in.scale, map.tribbleSets[in.set].name.c_str(), in.layer, in.model, in.rank, in.containerSize);
                    }
                }
            }
        }
        scene.advanceParticles(0.0f);
        for (float t = 0; t < particleSeconds; t += 1.0f / 30.0f) scene.advanceParticles(1.0f / 30.0f);
        std::printf("particle systems running: %zu, %zu particles\n", scene.liveParticleSystems(), scene.liveParticles());
        for (const auto& e : scene.textureErrors()) std::printf("texture error: %s: %s\n", e.first.c_str(), e.second.c_str());
        // RASA_SELECT=<map entity index>: draw that entity highlighted, as the editor shows a selection.
        Selection highlighted{};
        if (const char* group = envValue("RASA_HIDEGROUP")) scene.hiddenGroup = group;   // skip model pieces of one GPCE group
        if (envValue("RASA_NOSKINANIM")) scene.animateSkins = false;   // keep blended skinned pieces in frame 0
        if (const char* only = envValue("RASA_ONLY")) scene.onlyEntity = std::atoi(only);   // draw one map entity alone
        if (const char* sel = envValue("RASA_SELECT")) highlighted = Selection{SelectionKind::MapEntity, size_t(std::atoi(sel))};
        scene.queueDraws(highlighted);
        std::printf("draws queued: %zu (entities drawn %zu), particle vertices %zu\n", renderer.queuedDraws(), scene.drawnEntities(), renderer.queuedParticleVertices());
        std::printf("clutter drawn: %zu\n", scene.drawnClutter());
        std::printf("terrain chunks: %zu, %zu at full detail\n", scene.terrainChunkCount(), scene.terrainChunksAtFullDetail());
        {
            tr::Vec3 rayOrigin, rayDirection, ground;
            scene.viewRay(0.0f, 0.0f, rayOrigin, rayDirection);
            if (scene.groundPoint(rayOrigin, rayDirection, 0.0f, ground))
            {
                std::printf("ground under the view centre: %.2f, %.2f, %.2f (terrain height there %.2f)\n", ground.x, ground.y, ground.z,
                            scene.groundHeight(ground.x, ground.z, -9999.0f));
                // The sample there: layers (texlayers.tab name) with stored weight and the client's coverage.
                const tr::Terrain& t = map.terrain;
                const long column = std::lround((ground.x - t.worldX(0)) / t.header.spacing);
                const long row = std::lround((ground.z - t.worldZ(0)) / t.header.spacing);
                if (map.hasTerrain && column >= 0 && row >= 0 && column < long(t.columns) && row < long(t.rows)) {
                    const tr::TerrainSample& s = t.sampleAt(uint32_t(row), uint32_t(column));
                    std::printf("terrain sample (row %ld, column %ld), category %d:", row, column, int(tr::surfaceCategory(s.word)));
                    for (int k = 0; k < 4; ++k) {
                        const tr::LayerBlend& b = tr::sampleLayer(s, k);
                        const int entry = tr::textureLayerOf(b);
                        std::printf(" [%d %s w%d c%d]", int(b.layerIndex),
                                    entry >= 0 && size_t(entry) < map.textureLayers.size() ? map.textureLayers[entry].name.c_str() : "-", int(b.weight),
                                    int(tr::slotCoverage(s, k)));
                    }
                    std::printf("\n");
                }
            }
        }
        const Selection hit = scene.pick(0.0f, 0.0f);    // centre of the view
        const int picked = hit.kind == SelectionKind::MapEntity ? int(hit.index) : -1;
        if (hit.kind != SelectionKind::MapEntity && hit.kind != SelectionKind::None)
            std::printf("pick at centre: server object kind %d index %zu\n", int(hit.kind), hit.index);
        else if (picked >= 0)
            std::printf("pick at centre: entity %d, class %d (%s) at %.1f, %.1f, %.1f\n", picked, map.map.entities[size_t(picked)].classId,
                        data.className(map.map.entities[size_t(picked)].classId).c_str(), map.map.entities[size_t(picked)].location.x,
                        map.map.entities[size_t(picked)].location.y, map.map.entities[size_t(picked)].location.z);
        else
            std::puts("pick at centre: nothing");

        SDL_GPUTransferBufferCreateInfo info = {};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = width * height * 4;
        SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(device, &info);
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
        renderer.record(commands);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureRegion source = {};
        source.texture = renderer.colourTarget();
        source.w = width;
        source.h = height;
        source.d = 1;
        SDL_GPUTextureTransferInfo destination = {};
        destination.transfer_buffer = download;
        destination.pixels_per_row = width;
        destination.rows_per_layer = height;
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
        const void* pixels = SDL_MapGPUTransferBuffer(device, download, false);
        FILE* f = std::fopen(argv[4], "wb");
        std::fwrite(pixels, 1, width * height * 4, f);
        std::fclose(f);
        SDL_UnmapGPUTransferBuffer(device, download);
        SDL_ReleaseGPUTransferBuffer(device, download);

        // Focus check: from a camera looking 30 degrees up (the old failure case), "show in viewport" must not put
        // the camera under the terrain. Targets below the terrain surface (caves, interiors) are counted apart.
        {
            size_t tried = 0, underground = 0, caveTargets = 0, listed = 0;
            auto check = [&](const Selection& s) {
                tr::Vec3 target;
                float distance = 0, targetGround = 0;
                if (!scene.focusTarget(s, target, distance)) return;
                if (!map.hasTerrain || !map.terrain.heightAtWorld(target.x, target.z, targetGround)) return;
                if (target.y < targetGround - 3.0f) { ++caveTargets; return; }
                cam.yaw = 0.7f;
                cam.pitch = 0.5f;
                scene.focusOn(s);
                ++tried;
                float h = 0;
                if (map.terrain.heightAtWorld(cam.position.x, cam.position.z, h) && cam.position.y < h + 0.5f) {
                    ++underground;
                    if (listed++ < 5)
                        std::printf("  under: kind %d #%zu target %.1f %.1f %.1f (ground %.1f), camera %.1f %.1f %.1f (ground %.1f), distance %.1f\n",
                                    int(s.kind), s.index, target.x, target.y, target.z, targetGround, cam.position.x, cam.position.y,
                                    cam.position.z, h, distance);
                }
            };
            for (size_t k = 0; k < map.map.entities.size(); ++k) check(Selection{SelectionKind::MapEntity, k});
            for (size_t k = 0; k < content.spawnPools.size(); ++k) check(Selection{SelectionKind::SpawnPool, k});
            std::printf("focus check: %zu targets, camera under the terrain for %zu (%zu targets below the surface skipped)\n",
                        tried, underground, caveTargets);
        }
        SDL_WaitForGPUIdle(device);
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());               // report instead of aborting (a Debug Error dialog)
        SDL_DestroyGPUDevice(device);
        SDL_Quit();
        return 2;
    }
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    std::puts("done");
    return 0;
}
