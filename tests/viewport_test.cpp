// Headless viewport test: renders a map with the editor's Renderer/SceneView into an offscreen
// target and writes the frame as raw RGBA. Runs on any Vulkan driver, including Mesa's lavapipe.
//   viewport_test <game root> <game.zip> <map> <out.rgba> <width> <height> <eye x y z> <target x y z>
#include "Avatar.h"
#include "SceneView.h"
#include "gamedata.h"
#include "WorldDatabase.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
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
        if (envValue("RASA_NOTERRAIN")) scene.showTerrain = false;
        scene.terrainBuildsPerFrame = SIZE_MAX;      // a single frame: every chunk at its distance's detail
        if (envValue("RASA_TWOSIDED")) renderer.cullBackFaces = false;
        if (envValue("RASA_NOENTITIES")) scene.showEntities = false;
        const float particleSeconds = envValue("RASA_PARTICLES") ? float(std::atof(envValue("RASA_PARTICLES"))) : 0.5f;
        scene.advanceParticles(0.0f);
        for (float t = 0; t < particleSeconds; t += 1.0f / 30.0f) scene.advanceParticles(1.0f / 30.0f);
        std::printf("particle systems running: %zu, %zu particles\n", scene.liveParticleSystems(), scene.liveParticles());
        for (const auto& e : scene.textureErrors()) std::printf("texture error: %s: %s\n", e.first.c_str(), e.second.c_str());
        scene.queueDraws(Selection{});
        std::printf("draws queued: %zu (entities drawn %zu), particle vertices %zu\n", renderer.queuedDraws(), scene.drawnEntities(), renderer.queuedParticleVertices());
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
