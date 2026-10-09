// Headless viewport test: renders a map with the editor's Renderer/SceneView into an offscreen
// target and writes the frame as raw RGBA. Runs on any Vulkan driver, including Mesa's lavapipe.
//   viewport_test <game root> <game.zip> <map> <out.rgba> <width> <height> <eye x y z> <target x y z>
#include "SceneView.h"
#include "gamedata.h"
#include "WorldDatabase.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    if (argc < 13) { std::puts("usage: viewport_test root game.zip map out.rgba w h ex ey ez tx ty tz"); return 1; }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, true, nullptr);
    if (!device) { std::printf("SDL_CreateGPUDevice: %s\n", SDL_GetError()); return 1; }
    std::printf("GPU driver: %s, BC1 textures %s\n", SDL_GetGPUDeviceDriver(device),
                SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER) ? "supported" : "not supported");
    const uint32_t width = uint32_t(std::atoi(argv[5])), height = uint32_t(std::atoi(argv[6]));
    {
        Renderer renderer(device);
        renderer.resize(width, height);
        tr::AssetLibrary assets(argv[1]);
        tr::GameData data(argv[2]);
        const tr::MapBundle map = tr::loadMap(assets, argv[3]);
        SceneView scene(renderer, assets, map, [&](int32_t id) { return data.classMeshName(id); },
                        [&](int32_t id) { return !data.className(id).empty(); });
        // Optional server content: set RASA_DB to a Rasa.NET World database.
        std::unique_ptr<WorldDatabase> database;
        WorldContent content;
        if (const char* dbPath = std::getenv("RASA_DB")) {
            database = std::make_unique<WorldDatabase>(dbPath);
            content = database->loadMap(database->contextIdForMap(map.name));
            scene.setServerContent(&content, database.get(), [&](int32_t id) { return data.classMeshName(id); });
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
        SDL_WaitForGPUIdle(device);
    }
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    std::puts("done");
    return 0;
}
