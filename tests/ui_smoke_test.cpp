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
#include <filesystem>
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
            // The in-game switch at the cave mouth (2026-10-14 capture, Debug Tracker positions where the player stood still): the
            // name and radar say Concordia Wilderness at x -489.92 / -484.42 / -481.37 and Ranja Cavern at -478.72 / -475.07.
            struct Spot { float x, y, z; int32_t region; };
            const Spot spots[] = {{-489.92090f, 213.91177f, -405.16742f, 0}, {-484.42346f, 214.04274f, -404.67984f, 0},
                                  {-481.37015f, 214.71899f, -404.62796f, 0}, {-478.71671f, 214.82982f, -404.46054f, 33},
                                  {-475.07239f, 214.95740f, -403.85135f, 33}};
            bool allMatch = true;
            for (const Spot& spot : spots) {
                ctx.scene->camera().position = tr::Vec3{spot.x, spot.y, spot.z};
                const int32_t got = ctx.radarRegionAtCamera();
                float ground = 0;
                ctx.map->terrain.heightAtWorld(spot.x, spot.z, ground);
                std::printf("      x %.2f y %.2f: terrain %.2f -> region %d (game %d)\n", spot.x, spot.y, ground, got, spot.region);
                allMatch = allMatch && got == spot.region;
            }
            check(allMatch, "radar region switches at the cave mouth as in game");
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

        // Battle cries: give the selected creature its own package (scope 2) and its class one (scope 1), draw, save, read back,
        // then remove both again.
        if (server.database_->hasTable("creature_battlecry")) {
            CreatureDefinition* d = nullptr;
            for (CreatureDefinition& c : server.creatureDefs_)
                if (c.creature.id == server.creatureSelected_) d = &c;
            if (d) {
                const int32_t id = d->creature.id, classId = d->creature.classId;
                const bool hadClass = server.classCries_.count(classId) != 0;
                const int32_t oldClass = hadClass ? server.classCries_[classId] : 0;
                d->hasBattlecry = true;
                d->battlecryPackage = 12;                    // Thrax soldier
                server.classCries_[classId] = 9;             // English female, cute
                frames(editor, device, 2);
                check(server.creaturesChanged() && server.saveCreatures(), "battle cry save succeeds");
                const auto defs = server.database_->creatureDefinitions();
                bool own = false;
                for (const CreatureDefinition& c : defs) own = own || (c.creature.id == id && c.hasBattlecry && c.battlecryPackage == 12);
                const auto& classes = server.database_->classBattlecries();
                const auto row = classes.find(classId);
                check(own && row != classes.end() && row->second == 9, "battle cry rows read back (own 12, class 9)");
                for (CreatureDefinition& c : server.creatureDefs_)
                    if (c.creature.id == id) c.hasBattlecry = false;
                if (hadClass) server.classCries_[classId] = oldClass;
                else server.classCries_.erase(classId);
                check(server.saveCreatures() && !server.creaturesChanged() &&
                          (server.database_->classBattlecries().count(classId) != 0) == hadClass,
                      "battle cry rows removed again");
            }
            // Every battle cry's sounds are in the archives; one decodes (loaded with SDL_mixer, not played).
            size_t sounds = 0, missing = 0;
            std::string example;
            for (int32_t package : ctx.gameData->battlecryPackages())
                for (const auto& cry : ctx.gameData->battlecryCries(package))
                    for (const auto& s : ctx.gameData->audioSetSounds(cry.audioSet)) {
                        ++sounds;
                        if (!ctx.assets->exists(s.file)) {
                            ++missing;
                            std::printf("      missing battle cry sound %s (package %d, set %d)\n", s.file.c_str(), package, cry.audioSet);
                        } else if (example.empty()) {
                            example = s.file;
                        }
                    }
            bool decodes = false;
            if (!example.empty() && MIX_Init()) {
                const tr::Bytes file = ctx.assets->read(example);
                SDL_IOStream* io = SDL_IOFromConstMem(file.data(), file.size());
                MIX_Audio* audio = io ? MIX_LoadAudio_IO(nullptr, io, true, true) : nullptr;
                SDL_AudioSpec spec{};
                decodes = audio && MIX_GetAudioFormat(audio, &spec) && spec.freq > 0;
                if (audio) MIX_DestroyAudio(audio);
                MIX_Quit();
            }
            std::printf("      battle cry sounds: %zu, %zu missing; %s decodes %d\n", sounds, missing, example.c_str(), decodes ? 1 : 0);
            check(sounds > 0 && missing == 0 && decodes, "battle cry sounds present and playable");
        }

        // Loot panel: the shipped tables pass the server's checks; a weight edit saves and reads back; a new profile (group, item,
        // binding) saves and deletes again; a rule break blocks the save.
        check(showTab("Loot###Server.Loot"), "loot panel exists");
        if (server.loot_.loaded()) {
            LootEditor& loot = server.loot_;
            const auto initial = loot.validate(*server.database_);
            for (size_t k = 0; k < initial.size() && k < 5; ++k) std::printf("      loot problem: %s\n", initial[k].c_str());
            check(initial.empty(), "shipped loot tables pass the server's checks");
            loot.selected_ = loot.profiles_.edited.rows.front()[size_t(loot.profiles_.edited.column("profile_key"))].t;
            frames(editor, device, 3);
            DbTable& items = loot.items_.edited;
            const int weightColumn = items.column("weight");
            const int64_t weight = items.rows.front()[size_t(weightColumn)].asInt();
            items.rows.front()[size_t(weightColumn)] = DbValue::integer(weight + 7);
            std::string message;
            check(loot.changed() && server.hasUnsavedChanges() && loot.save(*server.database_, message) && !loot.changed(), "loot weight edit saves");
            const DbTable reread = server.database_->loadTable("loot_item");
            check(reread.rows.front()[size_t(weightColumn)].asInt() == weight + 7, "loot weight reads back");
            // A new profile with one group, one item and a binding on this map.
            const std::string key = "smoke_test_profile";
            const size_t profileCount = loot.profiles_.saved.rows.size();
            auto row =[](const DbTable& t) { return std::vector<DbValue>(t.columns.size(), DbValue::integer(0)); };
            std::vector<DbValue> profile = row(loot.profiles_.edited);
            for (const char* c : {"profile_key", "description", "content_revision", "provenance", "evidence_notes"})
                profile[size_t(loot.profiles_.edited.column(c))] = DbValue::text(c == std::string("profile_key") ? key : "smoke");
            loot.profiles_.edited.rows.push_back(profile);
            std::vector<DbValue> group = row(loot.groups_.edited);
            group[size_t(loot.groups_.edited.column("profile_key"))] = DbValue::text(key);
            group[size_t(loot.groups_.edited.column("group_key"))] = DbValue::text("consumable");
            group[size_t(loot.groups_.edited.column("kind"))] = DbValue::integer(3);
            group[size_t(loot.groups_.edited.column("chance_basis_points"))] = DbValue::integer(5000);
            group[size_t(loot.groups_.edited.column("minimum_rolls"))] = DbValue::integer(1);
            group[size_t(loot.groups_.edited.column("maximum_rolls"))] = DbValue::integer(1);
            loot.groups_.edited.rows.push_back(group);
            std::vector<DbValue> item = row(items);
            item[size_t(items.column("profile_key"))] = DbValue::text(key);
            item[size_t(items.column("group_key"))] = DbValue::text("consumable");
            item[size_t(items.column("item_template_id"))] = DbValue::integer(44917);         // medpack, a Normal-quality consumable
            for (const char* c : {"weight", "minimum_quantity", "maximum_quantity"}) item[size_t(items.column(c))] = DbValue::integer(1);
            items.rows.push_back(item);
            const int32_t creature = server.database_->creatureList().front().id;
            std::vector<DbValue> binding = row(loot.bindings_.edited);
            binding[size_t(loot.bindings_.edited.column("map_context_id"))] = DbValue::integer(server.serverContent_.mapContextId);
            binding[size_t(loot.bindings_.edited.column("creature_id"))] = DbValue::integer(creature);
            binding[size_t(loot.bindings_.edited.column("profile_key"))] = DbValue::text(key);
            const bool alreadyBound = !loot.profileFor(server.serverContent_.mapContextId, creature).empty();
            if (!alreadyBound) loot.bindings_.edited.rows.push_back(binding);
            loot.selected_ = key;
            frames(editor, device, 3);
            const auto added = loot.validate(*server.database_);
            for (size_t k = 0; k < added.size() && k < 5; ++k) std::printf("      loot problem: %s\n", added[k].c_str());
            check(added.empty() && loot.save(*server.database_, message), "new loot profile saves");
            check(loot.profiles_.saved.rows.size() == loot.profiles_.edited.rows.size() &&
                      (alreadyBound || loot.profileFor(server.serverContent_.mapContextId, creature) == key),
                  "new loot profile and binding read back");
            // A Junk group may pick at most once: the editor refuses to save that.
            for (auto& g : loot.groups_.edited.rows)
                if (g[size_t(loot.groups_.edited.column("profile_key"))].t == key) {
                    g[size_t(loot.groups_.edited.column("kind"))] = DbValue::integer(1);
                    g[size_t(loot.groups_.edited.column("maximum_rolls"))] = DbValue::integer(2);
                }
            check(!loot.save(*server.database_, message) && loot.changed(), "loot rule break blocks the save");
            loot.revert();
            // Delete it again.
            auto drop = [&](DbTable& t) {
                t.rows.erase(std::remove_if(t.rows.begin(), t.rows.end(), [&](const std::vector<DbValue>& r) { return r[size_t(t.column("profile_key"))].t == key; }),
                             t.rows.end());
            };
            drop(loot.bindings_.edited);
            drop(items);
            drop(loot.groups_.edited);
            drop(loot.profiles_.edited);
            check(loot.save(*server.database_, message) && server.database_->loadTable("loot_profile").rows.size() == profileCount,
                  "loot profile deleted again");
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

        // Map editing: move, rotate, duplicate, delete, undo; save into a COPY of the map folder (never the game's) and read back.
        {
            ClientWorkspace& client = *editor.client_;
            namespace fs = std::filesystem;
            const fs::path copyFolder = fs::path(outputPath("smoke_map")) / ctx.map->name;
            std::error_code ec;
            fs::remove_all(copyFolder.parent_path(), ec);
            fs::create_directories(copyFolder, ec);
            for (const auto& f : fs::directory_iterator(ctx.map->folder, ec))
                if (tr::toLowerAscii(f.path().extension().string()) == ".map") fs::copy_file(f.path(), copyFolder / f.path().filename(), ec);
            const std::string realFolder = ctx.map->folder;
            ctx.map->folder = copyFolder.string();
            const size_t count = ctx.map->map.entities.size();
            client.selection_ = Selection{SelectionKind::MapEntity, 0};
            const tr::Vec3 before = ctx.map->map.entities[0].location;
            client.beginEdit();
            client.selectedPosition()->x += 5.0f;
            client.contentChanged();
            *client.selectedRotation() += 1.5707963f;            // 90 degrees about the vertical
            client.contentChanged();
            client.duplicateSelected();
            check(ctx.map->map.entities.size() == count + 1 && client.hasUnsavedChanges(), "map entity moved, rotated and duplicated");
            client.deleteSelected();                              // the duplicate (selected after duplicating)
            check(ctx.map->map.entities.size() == count, "duplicate deleted");
            client.undo();
            check(ctx.map->map.entities.size() == count + 1, "undo brings the duplicate back");
            client.redo();
            frames(editor, device, 2);
            const tr::MapEntity edited = ctx.map->map.entities[0];
            const bool saved = client.saveMap();
            const fs::path written = copyFolder / (ctx.map->name + ".map");
            tr::MapFile reread;
            try { reread = tr::readMap(written.string()); } catch (const std::exception&) {}
            fs::path original = written;
            original += ".rasaedit-original";
            const bool backup = fs::exists(original, ec);
            std::printf("      map save: %d, %zu entities read back, entity 0 x %.2f -> %.2f, backup %d\n", saved ? 1 : 0, reread.entities.size(),
                        before.x, reread.entities.empty() ? 0.0f : reread.entities[0].location.x, backup ? 1 : 0);
            check(saved && !client.hasUnsavedChanges() && reread.entities.size() == count && reread.entities[0].location.x == edited.location.x &&
                      reread.entities[0].orientation.y == edited.orientation.y && backup,
                  "map saved to a copy, read back with the edits, original kept");
            // Lights: move light 0, duplicate it; save writes lights.xml (copied into the test folder first) and keeps the original.
            fs::copy_file(fs::path(realFolder) / "lights.xml", copyFolder / "lights.xml", ec);
            const size_t lightCount = ctx.scene->mapLights().size();
            if (lightCount > 0) {
                client.selection_ = Selection{SelectionKind::MapLight, 0};
                const tr::Vec3 lightBefore = ctx.scene->mapLights()[0].position;
                client.beginEdit();
                tr::Vec3* lp = client.selectedPosition();
                if (lp) lp->x += 4.0f;
                client.contentChanged();
                client.duplicateSelected();
                const bool duplicated = ctx.scene->mapLights().size() == lightCount + 1 && client.selection_.index == lightCount;
                const bool lightSaved = client.saveMap();
                std::vector<tr::PlacedLight> reread;
                try { reread = tr::readLights(tr::readFile((copyFolder / "lights.xml").string())); } catch (const std::exception&) {}
                const bool lightBackup = fs::exists(copyFolder / "lights.xml.rasaedit-original", ec);
                std::printf("      lights: %zu -> %zu read back, light 0 x %.2f -> %.2f, backup %d\n", lightCount, reread.size(), lightBefore.x,
                            reread.empty() ? 0.0f : reread[0].position.x, lightBackup ? 1 : 0);
                check(lp && duplicated && lightSaved && reread.size() == lightCount + 1 && std::fabs(reread[0].position.x - (lightBefore.x + 4.0f)) < 1e-3f &&
                          lightBackup,
                      "light moved, duplicated, saved to a copy and read back");
            }
            ctx.map->folder = realFolder;
        }

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
