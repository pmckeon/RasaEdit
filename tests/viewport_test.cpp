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
                        [&](int32_t id) { return !data.className(id).empty(); });
        {
            std::vector<std::pair<int32_t, std::string>> regionSkies;
            for (const tr::MapRegion& r : map.map.regions)
                if (r.sky > 0) regionSkies.emplace_back(r.regionId, data.stringById(r.sky));
            scene.setRegionSkies(regionSkies);
            // RASA_TIME=<fraction of the day>, RASA_SKY=<index>, RASA_NOSKY=1 / RASA_NOFOG=1 for lighting checks.
            if (const char* t = envValue("RASA_TIME")) scene.timeOfDay = float(std::atof(t));
            if (const char* k = envValue("RASA_SKY")) scene.skyIndex = size_t(std::atoi(k));
            if (envValue("RASA_NOSKY")) scene.skyLighting = false;
            if (envValue("RASA_NOFOG")) scene.showFog = false;
            if (envValue("RASA_NOSUN")) scene.showSun = false;
            if (envValue("RASA_NOLIGHTS")) scene.showMapLights = false;
            std::printf("map lights: %zu%s%s\n", scene.mapLights().size(), scene.mapLightsError().empty() ? "" : " - ", scene.mapLightsError().c_str());
            for (size_t k = 0; k < scene.skies().size(); ++k) {
                const SceneView::SkyChoice& c = scene.skies()[k];
                std::printf("sky %zu%s: %s (%zu regions)%s%s\n", k, k == scene.skyIndex ? " *" : "", c.name.c_str(), c.regions.size(),
                            c.loaded ? "" : " - not usable: ", c.error.c_str());
            }
            const SceneLighting l = scene.lighting();
            std::printf("time %.3f: hemi top %.2f %.2f %.2f bottom %.2f %.2f %.2f, sun %.2f %.2f %.2f towards %.2f %.2f %.2f, fog %s from %.0f m density %.4f, %d placed lights in use\n",
                        scene.timeOfDay, l.hemiTop.x, l.hemiTop.y, l.hemiTop.z, l.hemiBottom.x, l.hemiBottom.y, l.hemiBottom.z, l.sunFront.x,
                        l.sunFront.y, l.sunFront.z, l.towardsLight.x, l.towardsLight.y, l.towardsLight.z, l.fog ? "on" : "off", l.fogStart, l.fogDensity, l.localCount);
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
        std::printf("models: %zu loaded, %zu failed, %zu total; textures %zu\n", scene.modelsLoaded(), scene.modelsFailed(), scene.modelsTotal(), scene.texturesLoaded());
        ViewCamera& cam = scene.camera();
        cam.position = tr::Vec3{float(std::atof(argv[7])), float(std::atof(argv[8])), float(std::atof(argv[9]))};
        const tr::Vec3 to = subtract(tr::Vec3{float(std::atof(argv[10])), float(std::atof(argv[11])), float(std::atof(argv[12]))}, cam.position);
        cam.yaw = std::atan2(to.x, to.z);
        cam.pitch = std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z));
        if (argc > 13) scene.showPlaceholders = false;
        scene.queueDraws(Selection{});
        std::printf("draws queued: %zu (entities drawn %zu)\n", renderer.queuedDraws(), scene.drawnEntities());
        {
            tr::Vec3 rayOrigin, rayDirection, ground;
            scene.viewRay(0.0f, 0.0f, rayOrigin, rayDirection);
            if (scene.groundPoint(rayOrigin, rayDirection, 0.0f, ground))
                std::printf("ground under the view centre: %.2f, %.2f, %.2f (terrain height there %.2f)\n", ground.x, ground.y, ground.z,
                            scene.groundHeight(ground.x, ground.z, -9999.0f));
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
