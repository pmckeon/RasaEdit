// Headless smoke test of the whole editor: real ImGui frames (no window or platform backend), the
// real Editor with both workspaces, and the GPU viewport on any Vulkan driver (e.g. lavapipe).
// Settings come from RASAEDIT_SETTINGS if set (use one that names a COPY of the database: the test saves), else the SDL
// pref folder (settings.ini: gameRoot, database, map = adv_foreas_concordia_wilderness, workspace = server).
#define _ALLOW_KEYWORD_MACROS                // MSVC STL (xkeycheck.h) otherwise rejects the two defines below
#define private public                       // the test inspects workspace internals
#define protected public
#include "Editor.h"
#undef private
#undef protected

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilderGetNode

#include <algorithm>
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <string>

static int failures = 0;

// Viewport dumps go to RASA_SMOKE_OUT (a folder), else the temp folder.
static std::string outputPath(const char* name) {
    const char* dir = std::getenv("RASA_SMOKE_OUT");
    if (!dir || !*dir) dir = std::getenv("TEMP");
    if (!dir || !*dir) dir = "/tmp";
    return std::string(dir) + "/" + name;
}
static void check(bool ok, const char* what) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// Dockspace IDs are computed inside a frame (as the editor does), at the root of the ID stack.
static bool dockspaceExists(const char* name) {
    ImGui::NewFrame();
    const bool exists = ImGui::DockBuilderGetNode(ImGui::GetID(name)) != nullptr;
    ImGui::EndFrame();
    return exists;
}

static void frames(Editor& editor, SDL_GPUDevice* device, int count) {
    for (int k = 0; k < count; ++k) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        editor.processPendingActions();
        ImGui::NewFrame();
        editor.drawUi();
        ImGui::Render();
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
        editor.renderViewport(commands);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
    }
}

// Writes "<w> <h>\n" then RGBA rows; returns how many distinct colours the image has (0 on failure).
static size_t saveTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture, uint32_t w, uint32_t h, const char* path) {
    SDL_GPUTransferBufferCreateInfo info = {};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = w * h * 4;
    SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(device, &info);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureRegion source = {};
    source.texture = texture;
    source.w = w;
    source.h = h;
    source.d = 1;
    SDL_GPUTextureTransferInfo destination = {};
    destination.transfer_buffer = download;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    const uint32_t* pixels = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(device, download, false));
    std::vector<uint32_t> colours(pixels, pixels + size_t(w) * h);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "%u %u\n", w, h);
        std::fwrite(pixels, 1, w * h * 4, f);
        std::fclose(f);
    } else {
        std::printf("cannot write %s\n", path);
    }
    SDL_UnmapGPUTransferBuffer(device, download);
    SDL_ReleaseGPUTransferBuffer(device, download);
    std::sort(colours.begin(), colours.end());
    return size_t(std::unique(colours.begin(), colours.end()) - colours.begin());
}
static void saveViewport(EditorContext& ctx, SDL_GPUDevice* device, const char* path) {
    saveTexture(device, ctx.renderer->colourTarget(), ctx.renderer->width(), ctx.renderer->height(), path);
}

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (!device) { std::printf("no GPU device: %s\n", SDL_GetError()); return 1; }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600, 900);
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;   // as the SDL GPU backend declares
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    {
        Editor editor(nullptr, device);
        EditorContext& ctx = *editor.context_;
        ServerWorkspace& server = *editor.server_;
        frames(editor, device, 3);
        check(ctx.map != nullptr, "map reopened from settings");
        check(editor.active_ == editor.server_.get(), "starts in the Server workspace (from settings)");
        check(dockspaceExists("ServerDockSpace"), "server layout built");
        check(server.database_ && !server.database_->readOnly(), "server database open read-write");
        {
            // Every spawnpool row of Concordia (1220), counted from the table itself (the upstream world keeps growing: 251 in
            // the 2026-10-08 world, 263 after the 2026-10-13 update).
            size_t rows = 0;
            if (server.database_) {
                const DbTable pools = server.database_->loadTable("spawnpool");
                const int map = pools.column("map_context_id");
                for (const auto& r : pools.rows) rows += r[size_t(map)].asInt() == 1220 ? 1 : 0;
            }
            std::printf("      Concordia spawn pools: %zu loaded, %zu rows\n", server.serverContent_.spawnPools.size(), rows);
            check(rows > 0 && server.serverContent_.spawnPools.size() == rows, "every Concordia spawn pool loaded");
        }
        if (const char* names[] = {"Viewport###Server.Viewport", "Overview###Server.Overview"}; true)
            for (const char* n : names) {
                ImGuiWindow* w = ImGui::FindWindowByName(n);
                std::printf("      %-28s exists %d active %d hidden %d docked %d selectedTab %s\n", n, w != nullptr, w ? w->Active : 0, w ? w->Hidden : 0,
                            w && w->DockNode ? 1 : 0,
                            w && w->DockNode ? (w->DockNode->SelectedTabId == w->TabId ? "this" : "other") : "-");
            }
        check(server.viewport_.visible_, "server viewport drawn");
        check(ctx.scene && ctx.scene->showServer, "server viewport shows the server overlay");
        // Radar regions: Concordia has sub-region radar maps; inside Ranja Cavern (map_region 33, a box at -550 208 -415) the
        // client's rule picks region 33, outside it region 0; the overview draws with the region chosen by hand too.
        if (ctx.scene) {
            std::printf("      radar regions:");
            for (int32_t id : ctx.radarRegions) std::printf(" %d", id);
            std::printf("\n");
            const ViewCamera saved = ctx.scene->camera();
            ctx.scene->camera().position = tr::Vec3{-550.0f, 205.0f, -415.0f};
            const int32_t inside = ctx.radarRegionAtCamera();
            ctx.scene->camera().position = tr::Vec3{0.0f, 400.0f, 0.0f};
            const int32_t outside = ctx.radarRegionAtCamera();
            RadarMap* cavern = ctx.radarFor(33);
            std::printf("      radar at Ranja Cavern %d, elsewhere %d; region 33 map %d of %d tiles\n", inside, outside,
                        cavern ? cavern->tilesPresent() : -1, cavern ? cavern->tilesTotal() : -1);
            check(inside == 33 && outside == 0 && cavern && cavern->tilesPresent() > 0, "radar region follows the camera (Ranja Cavern 33)");
            server.overview_.radarChoice_ = 33;
            frames(editor, device, 2);
            server.overview_.radarChoice_ = -1;
            ctx.scene->camera() = saved;
        }
        frames(editor, device, 60);                                  // let models load
        saveViewport(ctx, device, outputPath("smoke_server.rgba").c_str());

        // Edit through the viewport hooks: select pool 0, move it, check the unsaved state through undo and redo.
        server.selection_ = Selection{SelectionKind::SpawnPool, 0};
        const tr::Vec3 before = server.serverContent_.spawnPools[0].position;
        server.beginEdit();
        server.selectedPosition()->x += 3.0f;
        server.contentChanged();
        check(server.hasUnsavedChanges(), "edit marks unsaved changes");
        server.undo();
        check(!server.hasUnsavedChanges() && server.serverContent_.spawnPools[0].position.x == before.x, "undo restores the saved state");
        server.redo();
        check(server.hasUnsavedChanges() && server.serverContent_.spawnPools[0].position.x == before.x + 3.0f, "redo reapplies the edit");

        // Requesting another map with unsaved changes must ask, not load.
        ctx.queueMap("adv_bootcamp");
        frames(editor, device, 2);
        check(editor.promptAction_ == Editor::PromptAction::OpenMap && ctx.currentMapName == "adv_foreas_concordia_wilderness",
              "map switch with unsaved changes opens the prompt instead");
        editor.promptAction_ = Editor::PromptAction::None;          // (cancel)

        check(server.saveChanges() && !server.hasUnsavedChanges(), "save succeeds and clears the unsaved state");

        // Creatures panel: bring its tab to the front, select McAllister, draw (the editor widgets run), edit and save.
        auto showTab = [&](const char* name) {
            ImGuiWindow* w = ImGui::FindWindowByName(name);
            if (w && w->DockNode) w->DockNode->SelectedTabId = w->TabId;
            return w != nullptr;
        };
        check(showTab("Creatures###Server.Creatures"), "creatures panel exists");
        server.creatureSelected_ = 510203;
        frames(editor, device, 3);
        CreatureDefinition* mcallister = nullptr;
        for (CreatureDefinition& d : server.creatureDefs_)
            if (d.creature.id == 510203) mcallister = &d;
        check(mcallister && server.creatureDisplayName(*mcallister) == "Major McAllister", "creature list resolves McAllister's in-game name");
        if (mcallister) {
            const int32_t level = mcallister->creature.level;
            mcallister->creature.level = level + 1;
            frames(editor, device, 2);
            check(server.creaturesChanged() && server.hasUnsavedChanges(), "creature edit marks unsaved changes");
            check(server.saveCreatures() && !server.creaturesChanged(), "creature save succeeds");
            const Creature* saved = server.database_->creature(510203);
            check(saved && saved->level == level + 1, "saved creature level reads back");
        }

        // Missions panel: select the first mission, draw its tree, edit an objective comment and save.
        check(showTab("Missions###Server.Missions"), "missions panel exists");
        check(server.missions_.loaded(), "mission tables loaded");
        if (server.missions_.loaded()) {
            const DbTable& missions = server.missions_.tables_.front().edited;
            server.missions_.selectedMission_ = missions.rows.front()[size_t(missions.column("mission_id"))].asInt();
            server.missions_.selectedRevision_ = missions.rows.front()[size_t(missions.column("content_revision"))].t;
            frames(editor, device, 3);
            DbTable* objectives = nullptr;
            for (auto& t : server.missions_.tables_)
                if (t.edited.name == "mission_objective_definition") objectives = &t.edited;
            if (objectives && !objectives->rows.empty()) {
                objectives->rows.front()[size_t(objectives->column("comment"))] = DbValue::text("smoke test objective");
                frames(editor, device, 2);
                check(server.missions_.changed() && server.hasUnsavedChanges(), "mission edit marks unsaved changes");
                std::string message;
                check(server.missions_.save(*server.database_, message) && !server.missions_.changed(), "mission save succeeds");
                const DbTable reloaded = server.database_->loadTable("mission_objective_definition");
                bool found = false;
                for (const auto& row : reloaded.rows) found = found || row[size_t(reloaded.column("comment"))].t == "smoke test objective";
                check(found, "saved mission row reads back");
            }
        }

        // Switch to the client workspace.
        editor.setActive(editor.client_.get());
        frames(editor, device, 3);
        check(dockspaceExists("ClientDockSpace"), "client layout built");
        check(editor.client_->viewport_.visible_, "client viewport drawn");
        check(!ctx.scene->showServer, "client viewport hides the server overlay");
        check(editor.client_->visibleEntities_.size() == ctx.map->map.entities.size(), "client entity table filled");
        saveViewport(ctx, device, outputPath("smoke_client.rgba").c_str());

        // Assets panel previews: a model (rendered by its own scene and renderer), a texture and a sound.
        {
            ImGuiWindow* w = ImGui::FindWindowByName("Assets###Client.Assets");
            auto showAssets = [&]() {           // as a click on its tab (the tab bar keeps its own selection)
                if (!w || !w->DockNode) return;
                w->DockNode->SelectedTabId = w->TabId;
                if (w->DockNode->TabBar) w->DockNode->TabBar->NextSelectedTabId = w->TabId;
            };
            showAssets();
            AssetPreview& preview = editor.client_->assets_.preview_;
            preview.select(ctx, "npc_eloh_hologram_large.geo");
            frames(editor, device, 30);
            check(preview.kind_ == AssetPreview::Kind::Model && preview.scene_ && preview.scene_->modelsLoaded() == 1 && preview.modelFitted_,
                  "model preview loads and frames the model");
            const size_t colours = preview.renderer_ ? saveTexture(device, preview.renderer_->colourTarget(), preview.renderer_->width(),
                                                                   preview.renderer_->height(), outputPath("smoke_preview_model.rgba").c_str())
                                                     : 0;
            std::printf("      model preview %ux%u, %zu colours, radius %.1f m\n", preview.renderer_ ? preview.renderer_->width() : 0,
                        preview.renderer_ ? preview.renderer_->height() : 0, colours, preview.distance_);
            check(colours > 100, "model preview draws something");
            showAssets();
            preview.select(ctx, "arch_eloh_obelisk_v02.geo");
            frames(editor, device, 30);
            check(preview.scene_ && preview.scene_->modelsLoaded() == 1 && preview.modelFitted_, "second model preview loads");
            if (preview.renderer_)
                saveTexture(device, preview.renderer_->colourTarget(), preview.renderer_->width(), preview.renderer_->height(),
                            outputPath("smoke_preview_model2.rgba").c_str());
            preview.select(ctx, "hum_m_nude_v00_df.dds");
            frames(editor, device, 2);
            check(preview.kind_ == AssetPreview::Kind::Texture && preview.texture_ && preview.textureWidth_ == 512, "texture preview decodes the DDS");
            preview.select(ctx, "MissionAccomplished.wav");
            frames(editor, device, 2);
            check(preview.kind_ == AssetPreview::Kind::Sound && preview.soundFrames_ > 0 && preview.soundSpec_.channels == 2, "WAV preview loads the sound");
            // Ogg Vorbis and MP3 through SDL_mixer's libvorbis / mpg123 decoders (no audio device is opened: nothing plays).
            for (const char* sound : {"Mis_BootCamp_deployment.ogg", "AFSOutpost.mp3"}) {
                preview.select(ctx, sound);
                frames(editor, device, 2);
                std::printf("      %s: %d Hz, %d channels, %lld frames\n", sound, preview.soundSpec_.freq, preview.soundSpec_.channels,
                            (long long)preview.soundFrames_);
                check(preview.kind_ == AssetPreview::Kind::Sound && preview.soundSpec_.freq > 0, sound);
            }
            preview.select(ctx, "sky_foreas_bootcamp_d11.sky");
            frames(editor, device, 2);
            check(preview.kind_ == AssetPreview::Kind::Text && preview.text_.find("SkyData") != std::string::npos, "text preview shows the .sky");
        }

        // Back to server: its selection and options survive the switch.
        editor.setActive(editor.server_.get());
        frames(editor, device, 2);
        check(server.selection_.kind == SelectionKind::SpawnPool && ctx.scene->showServer, "server selection and layers kept across switches");
        SDL_WaitForGPUIdle(device);
        editor.releaseGpuResources();
    }
    ImGui::DestroyContext();
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
