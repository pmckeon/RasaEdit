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

        // FX emitter (map_emitter): add one, give it a package, draw its inspector and viewport, save, read back, delete, save.
        if (server.database_->hasTable("map_emitter")) {
            server.addServerObject(SelectionKind::Object, ObjectKind::Emitter);
            const size_t index = server.selection_.index;
            server.serverContent_.objects[index].emitter.packageId = 12534;      // smokeplume_fire.pkg
            server.serverContent_.objects[index].name = "smoke test emitter";
            server.contentChanged();
            frames(editor, device, 3);
            check(server.saveChanges(), "FX emitter saves");
            bool found = false;
            for (const PlacedObject& o : server.database_->loadMap(server.serverContent_.mapContextId).objects)
                found = found || (o.kind == ObjectKind::Emitter && o.name == "smoke test emitter" && o.emitter.packageId == 12534 && o.emitter.isOn);
            check(found, "FX emitter reads back");
            for (size_t k = 0; k < server.serverContent_.objects.size(); ++k)
                if (server.serverContent_.objects[k].kind == ObjectKind::Emitter && server.serverContent_.objects[k].name == "smoke test emitter")
                    server.selection_ = Selection{SelectionKind::Object, k};
            server.beginEdit();
            server.deleteSelected();
            check(server.saveChanges() && server.database_->loadMap(server.serverContent_.mapContextId).objects.size() == server.serverContent_.objects.size(),
                  "FX emitter deleted again");
            server.selection_ = Selection{SelectionKind::SpawnPool, 0};       // as before (a later check expects the pool selected)
        }

        // Control point links: select a control point, draw its inspector (links listed), add a boss link, save, read back, remove it.
        if (server.database_->hasTable("control_point_link")) {
            size_t pointIndex = server.serverContent_.objects.size();
            for (size_t k = 0; k < server.serverContent_.objects.size(); ++k)
                if (server.serverContent_.objects[k].kind == ObjectKind::ControlPoint) pointIndex = k;
            check(pointIndex < server.serverContent_.objects.size() && !server.serverContent_.controlPointLinks.empty(),
                  "Concordia's control points and their links loaded");
            if (pointIndex < server.serverContent_.objects.size()) {
                const int64_t pointId = server.serverContent_.objects[pointIndex].id;
                server.selection_ = Selection{SelectionKind::Object, pointIndex};
                frames(editor, device, 2);
                const size_t before = server.serverContent_.controlPointLinks.size();
                server.beginEdit();
                server.serverContent_.controlPointLinks.push_back(ControlPointLink{pointId, 5, 22});
                server.contentChanged();
                frames(editor, device, 2);
                check(server.saveChanges() && server.database_->loadMap(server.serverContent_.mapContextId).controlPointLinks.size() == before + 1,
                      "control point link saves and reads back");
                server.beginEdit();
                server.serverContent_.controlPointLinks.pop_back();
                server.contentChanged();
                check(server.saveChanges() && server.database_->loadMap(server.serverContent_.mapContextId).controlPointLinks.size() == before,
                      "control point link removed again");
            }
            server.selection_ = Selection{SelectionKind::SpawnPool, 0};
        }

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

        // Class flags: add one to McAllister's class (KINGDOM_FUNGUS 35, which it does not have), draw, save, read back, remove, save.
        if (server.database_->hasTable("creature_class_flag") && mcallister) {
            const int32_t classId = mcallister->creature.classId;
            const bool had = server.classFlags_.count(classId) && server.classFlags_[classId].count(35);
            if (!had) {
                server.classFlags_[classId].insert(35);
                showTab("Creatures###Server.Creatures");
                frames(editor, device, 2);
                const bool saved = server.creaturesChanged() && server.saveCreatures();
                const auto& flags = server.database_->classFlags();
                const auto row = flags.find(classId);
                check(saved && row != flags.end() && row->second.count(35), "class flag saves and reads back");
                // The panel's flag names (GameData::moduleConstants, what drawClassFlags lists).
                bool biological = false;
                size_t names = 0;
                try {
                    const auto constants = ctx.gameData->moduleConstants("generated.client.constant.creatureflag");
                    names = constants.size();
                    for (const auto& [value, name] : constants) biological = biological || (value == 5 && name == "BIOLOGICAL");
                } catch (const std::exception&) {}
                check(biological && names >= 100, "creature flag names read from the client (5 BIOLOGICAL)");
                server.classFlags_[classId].erase(35);
                if (server.classFlags_[classId].empty()) server.classFlags_.erase(classId);
                const bool removed = server.saveCreatures() && !server.creaturesChanged();
                const auto& after = server.database_->classFlags();
                const auto again = after.find(classId);
                check(removed && (again == after.end() || !again->second.count(35)), "class flag removed again");
            }
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
        // Item modules: the panel loads the four tables, the shipped data has no errors, an effect line renders as the client's tooltip,
        // an effect edit saves and reads back, an effect of a missing module is refused, then everything is put back.
        if (server.database_->hasTable("module_class")) {
            check(showTab("Crafting###Server.Crafting") && server.modules_.loaded(), "crafting panel exists and loads the modules");
            size_t errors = 0;
            for (const auto& p : server.modules_.validate()) errors += p.error ? 1 : 0;
            check(errors == 0, "shipped module tables have no errors");
            server.modules_.selected_ = 100001;
            frames(editor, device, 2);
            const std::vector<size_t> rows = server.modules_.effectRows(100001);
            const std::string line = rows.empty() ? "" : server.modules_.effectLine(ctx, rows.front(), 30);
            std::printf("      module 100001 effect line at level 30: %s\n", line.c_str());
            check(!rows.empty() && !line.empty() && line.find("%(") == std::string::npos && line.find("(the effect has no tooltip") == std::string::npos,
                  "module effect line built from the client's tooltip");
            if (!rows.empty()) {
                DbTable& e = server.modules_.effects_.edited;
                auto& row = e.rows[rows.front()];
                const int c = e.column("flat_value");
                const DbValue before = row[size_t(c)];
                row[size_t(c)] = DbValue::real(before.asReal() + 2.5);
                std::string message;
                check(server.modules_.changed() && server.modules_.save(*server.database_, message), "module effect edit saves");
                const DbTable reread = server.database_->loadTable("module_effect");
                bool found = false;
                for (const auto& r : reread.rows)
                    if (r[size_t(reread.column("id"))].asInt() == server.modules_.effects_.edited.rows[rows.front()][size_t(e.column("id"))].asInt())
                        found = std::fabs(r[size_t(c)].asReal() - (before.asReal() + 2.5)) < 1e-9;
                check(found, "module effect edit reads back");
                // An effect of a missing module: refused.
                auto orphan = server.modules_.effects_.edited.rows.front();
                orphan[size_t(e.column("id"))] = DbValue::integer(999999);
                orphan[size_t(e.column("module_id"))] = DbValue::integer(987654321);
                server.modules_.effects_.edited.rows.push_back(orphan);
                check(!server.modules_.save(*server.database_, message), "an effect of a missing module is refused");
                server.modules_.effects_.edited.rows.pop_back();
                server.modules_.effects_.edited.rows[rows.front()][size_t(c)] = before;
                check(server.modules_.save(*server.database_, message) && !server.modules_.changed(), "module effect put back");
            }
        }

        // Recipes: loaded, the client's own recipes read, the shipped rows compared with them; an edit shows as a difference, saves,
        // reads back, and "reset to the client's" puts it back.
        if (server.database_->hasTable("recipe")) {
            check(server.recipes_.loaded() && server.recipes_.errors(*server.database_).empty(), "recipes load with no errors");
            std::vector<tr::ClientRecipe> client;
            try { client = ctx.gameData->clientRecipes(); } catch (const std::exception&) {}
            size_t differ = 0;
            const DbTable& t = server.recipes_.recipes_.edited;
            for (const auto& row : t.rows)
                if (!server.recipes_.differences(row[size_t(t.column("id"))].asInt(), client).empty()) ++differ;
            std::printf("      recipes: %zu here, %zu in the client, %zu not as the client\n", t.rows.size(), client.size(), differ);
            check(!client.empty(), "the client's recipes read (generated.shared.recipe)");
            if (!t.rows.empty() && !client.empty()) {
                const int64_t id = t.rows.front()[size_t(t.column("id"))].asInt();
                const tr::ClientRecipe* c = nullptr;
                for (const auto& r : client)
                    if (r.templateId == id) c = &r;
                auto& row = server.recipes_.recipes_.edited.rows.front();
                const int column = t.column("min_level");
                row[size_t(column)] = DbValue::integer(row[size_t(column)].asInt() + 3);
                check(!server.recipes_.differences(id, client).empty(), "a recipe edit shows as not the client's");
                std::string message;
                check(server.recipes_.save(*server.database_, message), "recipe edit saves");
                const DbTable reread = server.database_->loadTable("recipe");
                bool found = false;
                for (const auto& r : reread.rows)
                    if (r[size_t(reread.column("id"))].asInt() == id) found = r[size_t(reread.column("min_level"))] == server.recipes_.recipes_.edited.rows.front()[size_t(column)];
                check(found, "recipe edit reads back");
                if (c) {
                    server.recipes_.resetToClient(id, *c);
                    check(server.recipes_.differences(id, client).empty() && server.recipes_.save(*server.database_, message), "recipe reset to the client's");
                }
            }
        }

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
                // A value that differs from the stored one (an earlier run on the same copy leaves its edit saved).
                DbValue& comment = objectives->rows.front()[size_t(objectives->column("comment"))];
                const std::string written = comment == DbValue::text("smoke test objective") ? "smoke test objective 2" : "smoke test objective";
                comment = DbValue::text(written);
                frames(editor, device, 2);
                check(server.missions_.changed() && server.hasUnsavedChanges(), "mission edit marks unsaved changes");
                std::string message;
                check(server.missions_.save(*server.database_, message) && !server.missions_.changed(), "mission save succeeds");
                const DbTable reloaded = server.database_->loadTable("mission_objective_definition");
                bool found = false;
                for (const auto& row : reloaded.rows) found = found || row[size_t(reloaded.column("comment"))].t == written;
                check(found, "saved mission row reads back");
            }
            // The server's mission checks (MissionValidation): the shipped content passes; a required mission's objective without its
            // name text is reported and its save refused (the server would not report ready); an unreadable item intent too (the
            // server would not start); both put back.
            check(!server.missions_.validation().startupFails && server.missions_.validation().inactiveMissions.empty(), "mission checks pass on the database");
            DbTable* definitions = nullptr;
            DbTable* missionActions = nullptr;
            for (auto& t : server.missions_.tables_) {
                if (t.edited.name == "mission_content_definition") definitions = &t.edited;
                if (t.edited.name == "mission_action") missionActions = &t.edited;
            }
            if (objectives && definitions && missionActions) {
                // An objective of an enabled, required mission.
                std::vector<DbValue>* target = nullptr;
                for (auto& o : objectives->rows) {
                    for (const auto& d : definitions->rows)
                        if (d[size_t(definitions->column("mission_id"))] == o[size_t(objectives->column("mission_id"))] &&
                            d[size_t(definitions->column("content_revision"))] == o[size_t(objectives->column("content_revision"))] &&
                            d[size_t(definitions->column("enabled"))].asInt() == 1 && d[size_t(definitions->column("requirement"))].asInt() == 1)
                            target = &o;
                    if (target) break;
                }
                if (target) {
                    const DbValue before = (*target)[size_t(objectives->column("client_name_text_id"))];
                    (*target)[size_t(objectives->column("client_name_text_id"))] = DbValue::integer(0);
                    server.missions_.validationDirty_ = true;
                    frames(editor, device, 2);
                    bool reported = false;
                    for (const MissionDiagnostic& d : server.missions_.validation().diagnostics)
                        reported = reported || (d.code == "missing-client-text" && d.missionId == (*target)[size_t(objectives->column("mission_id"))].asInt());
                    std::string message;
                    const bool refused = !server.missions_.save(*server.database_, message);
                    check(reported && refused && message.find("reporting ready") != std::string::npos, "missing objective text reported, save refused");
                    (*target)[size_t(objectives->column("client_name_text_id"))] = before;
                    server.missions_.validationDirty_ = true;
                }
                const int intent = missionActions->column("item_intent");
                std::vector<DbValue>* action = nullptr;
                for (auto& a : missionActions->rows)
                    if (!a[size_t(intent)].isNull() && !action) action = &a;
                if (action) {
                    const DbValue before = (*action)[size_t(intent)];
                    (*action)[size_t(intent)] = DbValue::text("{\"$kind\":\"teleport\"}");
                    server.missions_.validationDirty_ = true;
                    std::string message;
                    const bool refused = !server.missions_.save(*server.database_, message);
                    check(server.missions_.validation().startupFails && refused && message.find("would not start") != std::string::npos,
                          "unreadable item intent: save refused (the server would not start)");
                    (*action)[size_t(intent)] = before;
                    server.missions_.validationDirty_ = true;
                }
                check(!server.missions_.changed() && server.missions_.validation().diagnostics.empty(), "mission content back as saved");
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

        // UI workspace: the export loads, a branch draws, a canvas pick finds a widget inside it.
        {
            editor.setActive(editor.ui_.get());
            frames(editor, device, 2);
            UiWorkspace& ui = *editor.ui_;
            check(dockspaceExists("UIDockSpace"), "UI layout built");
            check(ui.uiExport() && ui.uiExport()->widgets.children.size() == 8 && ui.uiExport()->textures.size() == 1512, "UI export loaded");
            check(ui.showBranch("FrontEnd"), "UI branch FrontEnd found");
            frames(editor, device, 2);
            size_t visual = 0;
            for (const auto& d : ui.drawnWidgets()) visual += d.visual ? 1 : 0;
            check(ui.texture_ && visual > 0 && ui.canvas_.missingTextures().empty() && ui.canvas_.missingFonts().empty(), "UI branch drawn with its textures and fonts");
            const std::vector<int32_t> under = ui.canvas_.pick(640, 480);
            check(!under.empty() && under.front() == ui.branch_.front(), "UI canvas pick finds a widget of the branch");
            // Interactive mode: hovering an enabled button draws its highlighted texture; edit mode draws the exported state.
            check(ui.showBranch("FrontEnd/LogOutWindow"), "UI branch FrontEnd/LogOutWindow found");
            frames(editor, device, 2);
            const tr::XmlNode* button = nullptr;
            UiCanvas::Rect buttonRect;
            for (const auto& d : ui.drawnWidgets()) {
                const tr::XmlNode* n = ui.canvas_.node(d.path);
                if (n && UiCanvas::isButton(*n) && n->tag == "PushButton" && n->attribute("enabled") == "true" &&
                    n->attribute("HighlightedTextureDef") != n->attribute("UpTextureDef")) {
                    button = n;
                    buttonRect = d.rect;
                    break;
                }
            }
            bool hoverChanges = false, buttonFound = false;
            if (button) {
                const int32_t cx = (buttonRect.x0 + buttonRect.x1) / 2, cy = (buttonRect.y0 + buttonRect.y1) / 2;
                buttonFound = ui.canvas_.buttonAt(cx, cy) == button;
                UiCanvas::Options edit;
                std::vector<uint8_t> a, b;
                ui.canvas_.render(ui.branch_, edit, a);
                UiCanvas::Options interactive;
                interactive.interactiveButtons = true;
                interactive.hoveredButton = button;
                ui.canvas_.render(ui.branch_, interactive, b);
                hoverChanges = a != b;
            }
            check(buttonFound, "UI interactive mode finds the button under the mouse");
            check(hoverChanges, "UI interactive mode highlights the hovered button");

            // Editing a COPY of the layout file: valid edits are written, invalid values refused, saving refused while the file has a
            // new error, and what is saved reads back.
            {
                const std::filesystem::path copy = outputPath("ui_export_edit_copy.xml");
                std::filesystem::copy_file(std::filesystem::path(ctx.gameRoot) / "data" / "xml" / "Tabula_Rasa_UI_EXPORT.xml", copy,
                                           std::filesystem::copy_options::overwrite_existing);
                ui.setFilePathForTests(copy.string());
                check(ui.document() != nullptr, "UI document loaded from the copy");
                check(ui.showBranch("FrontEnd/LogOutWindow"), "UI branch selected for editing");
                frames(editor, device, 2);
                const std::vector<int32_t> window = ui.selected_;
                ui.commitAttribute("Window", "anchorOffsetX", "5");
                check(ui.document()->node(false, window)->attribute("anchorOffsetX") == "5", "UI edit: anchorOffsetX written");
                ui.commitAttribute("Window", "childAnchor", "12");
                check(ui.document()->node(false, window)->attribute("childAnchor") == "8", "UI edit: an invalid anchor is refused");
                ui.commitAttribute("Window", "visible", "true");
                check(!ui.document()->node(false, window)->hasAttribute("visible"), "UI edit: a value at its default is left out");
                const size_t before = ui.document()->node(false, window)->children.size();
                ui.addChild("Bitmap");
                check(ui.document()->node(false, window)->children.size() == before + 1, "UI edit: a Bitmap added");
                const std::vector<int32_t> added = ui.selected_;
                ui.document()->setAttribute(false, added, "textureDef", "NoSuchTextureDef");      // bypassing the editor's check
                ui.syncWithDocument();
                check(!ui.saveChanges(), "UI save refused with a new error");
                ui.commitAttribute("Bitmap", "textureDef", "DEFAULT");
                check(ui.saveChanges(), "UI save after the error is fixed");
                tr::UiDocument reread(tr::readFile(copy.string()));
                check(reread.node(false, window) && reread.node(false, window)->attribute("anchorOffsetX") == "5" &&
                          reread.node(false, window)->children.size() == before + 1,
                      "UI saved file reads back with the edits");
                check(!ui.hasUnsavedChanges(), "UI no unsaved changes after saving");

                // Form designer: the anchor offset inverse, toolbox drops, clipboard, reparenting (one undo step).
                {
                    int32_t ox = 0, oy = 0;
                    ui.canvas_.offsetsFor(window, ui.canvas_.layout(window), ox, oy);
                    const tr::XmlNode* w = ui.document()->node(false, window);
                    check(std::to_string(ox) == w->attribute("anchorOffsetX") && std::to_string(oy) == w->attribute("anchorOffsetY"),
                          "UI designer: offsetsFor inverts the layout (centre anchors)");
                    frames(editor, device, 2);
                    const UiCanvas::Rect wr = ui.canvas_.layout(window);
                    const size_t count = w->children.size();
                    const bool dropped = ui.dropNewElement("Bitmap", wr.x0 + 20, wr.y0 + 30);
                    const std::vector<int32_t> bitmap = ui.selected_;
                    const UiCanvas::Rect br = ui.canvas_.layout(bitmap);
                    check(dropped && bitmap.size() == window.size() + 1 && br.x0 == wr.x0 + 20 && br.y0 == wr.y0 + 30,
                          "UI designer: a toolbox Bitmap lands at the drop point");
                    frames(editor, device, 2);
                    const bool button = ui.dropNewElement("PushButton", wr.x0 + 60, wr.y0 + 60);
                    const tr::XmlNode* b = ui.document()->node(false, ui.selected_);
                    check(button && b && b->tag == "PushButton" && b->child("Bitmap") && b->child("Text"), "UI designer: a toolbox PushButton comes with its parts");
                    // Multiple selection: the Bitmap is the primary; the button follows its left edge, then its size; each one undo step.
                    {
                        const std::vector<int32_t> buttonPath = ui.selected_;
                        ui.select(bitmap, false, false);
                        ui.toggleSelect(buttonPath);
                        check(ui.selection().size() == 2 && ui.selected_ == bitmap, "UI designer: Ctrl+click adds to the selection, the first stays primary");
                        const std::string beforeArrange = ui.document()->text();
                        int steps = 0;
                        const bool aligned = ui.arrangeSelection(UiWorkspace::Arrange::Lefts);
                        steps += aligned ? 1 : 0;
                        frames(editor, device, 1);
                        UiCanvas::Rect pr = ui.canvas_.layout(bitmap), qr = ui.canvas_.layout(buttonPath);
                        check(aligned && qr.x0 == pr.x0 && qr.y0 == wr.y0 + 60 && pr.x0 == wr.x0 + 20, "UI designer: align lefts follows the primary");
                        const bool sized = ui.arrangeSelection(UiWorkspace::Arrange::SameSize);
                        steps += sized ? 1 : 0;
                        frames(editor, device, 1);
                        pr = ui.canvas_.layout(bitmap);
                        qr = ui.canvas_.layout(buttonPath);
                        check(qr.width() == pr.width() && qr.height() == pr.height() && qr.x0 == pr.x0 && qr.y0 == wr.y0 + 60,
                              "UI designer: make same size keeps the top-left corner");
                        for (int k = 0; k < steps; ++k) ui.runEdit([&] { ui.document()->undo(); });
                        check(ui.document()->text() == beforeArrange && ui.selection().size() == 2, "UI designer: each arrange undoes in one step");
                    }
                    ui.select(bitmap, false, false);
                    ui.copySelection();
                    const bool pasted = ui.pasteClipboard();
                    check(pasted && ui.document()->node(false, window)->children.size() == count + 3, "UI designer: copy and paste");
                    // Move the pasted Bitmap into the window's first WidgetComposite, then undo: back in one step.
                    std::vector<int32_t> composite;
                    const tr::XmlNode* wn = ui.document()->node(false, window);
                    for (size_t k = 0; k < wn->children.size() && composite.empty(); ++k)
                        if (wn->children[k].tag == "WidgetComposite") composite = window, composite.push_back(int32_t(k));
                    const std::string before = ui.document()->text();
                    const size_t inside = composite.empty() ? 0 : ui.document()->node(false, composite)->children.size();
                    const std::vector<int32_t> pastedPath = ui.selected_;
                    const UiCanvas::Rect pastedRect = ui.canvas_.layout(pastedPath);
                    const bool moved = !composite.empty() && ui.moveSelectionInto(composite, inside, false);
                    check(moved && ui.document()->node(false, composite)->children.size() == inside + 1, "UI designer: element moved into another container");
                    ui.runEdit([&] { ui.document()->undo(); });
                    check(ui.document()->text() == before, "UI designer: the move undoes in one step");
                    // Again keeping its place on the screen: same rectangle in the new parent, still one undo step.
                    ui.select(pastedPath, false, false);
                    const bool kept = !composite.empty() && ui.moveSelectionInto(composite, inside, true);
                    const UiCanvas::Rect keptRect = ui.canvas_.layout(ui.selected_);
                    check(kept && ui.selected_.size() == composite.size() + 1 && keptRect.x0 == pastedRect.x0 && keptRect.y0 == pastedRect.y0 &&
                              keptRect.width() == pastedRect.width() && keptRect.height() == pastedRect.height(),
                          "UI designer: a move keeping its place stays where it was drawn");
                    ui.runEdit([&] { ui.document()->undo(); });
                    check(ui.document()->text() == before, "UI designer: the place-keeping move undoes in one step");
                    ui.revertChanges();
                    check(!ui.hasUnsavedChanges(), "UI designer: changes reverted");
                }

                // Definitions: value checks, rename carrying the references (one undo step), deletion refused while used, unique ids,
                // new entries with their mandatory fields, animType switching the tracks.
                {
                    using S = tr::UiSection;
                    const std::string start = ui.document()->text();
                    const tr::XmlNode* textures = ui.document()->node(S::Textures, {});
                    int32_t used = -1;
                    std::vector<UiWorkspace::Reference> refs;
                    for (size_t k = 0; textures && k < textures->children.size() && used < 0; ++k) {
                        refs = ui.findReferences(S::Textures, ui.definitionId(S::Textures, k));
                        if (refs.size() >= 2) used = int32_t(k);
                    }
                    check(used >= 0, "UI definitions: a textureDef used by widgets");
                    if (used >= 0) {
                        check(!ui.setDefinitionAttribute(S::Textures, {used}, "regionR", "wide").empty() && ui.document()->text() == start,
                              "UI definitions: a region that is not a number is refused");
                        check(!ui.setDefinitionAttribute(S::Textures, {used}, "name", "").empty(), "UI definitions: a textureDef keeps its file");
                        const std::string old = ui.definitionId(S::Textures, size_t(used));
                        const std::string renamed = ui.renameDefinition(S::Textures, size_t(used), old + "_Renamed", true);
                        check(renamed.empty() && ui.findReferences(S::Textures, old + "_Renamed").size() == refs.size() &&
                                  ui.findReferences(S::Textures, old).empty(),
                              "UI definitions: renaming carries the references");
                        ui.runEdit([&] { ui.document()->undo(); });
                        check(ui.document()->text() == start, "UI definitions: the rename undoes in one step");
                        check(!ui.removeDefinition(S::Textures, size_t(used)).empty() && ui.document()->text() == start,
                              "UI definitions: a used textureDef cannot be deleted");
                        const std::string other = ui.definitionId(S::Textures, used == 0 ? 1 : 0);
                        check(!ui.renameDefinition(S::Textures, size_t(used), other, false).empty(), "UI definitions: a duplicate id is refused");
                        // An edit that would add an error (an unused region change is fine; a file the archives lack is not).
                        check(!ui.setDefinitionAttribute(S::Textures, {used}, "name", "no_such_file_rasaedit.dds").empty(),
                              "UI definitions: a texture file the archives lack is refused");
                    }
                    const int32_t colour = ui.addDefinition(S::Colours, false);
                    check(colour >= 0 && ui.definitionId(S::Colours, size_t(colour)).rfind("NewColor", 0) == 0, "UI definitions: a new colour");
                    check(colour >= 0 && !ui.setDefinitionAttribute(S::Colours, {colour}, "R", "300").empty(), "UI definitions: a channel over 255 is refused");
                    check(colour >= 0 && ui.setDefinitionAttribute(S::Colours, {colour}, "R", "12").empty() &&
                              ui.uiExport()->colours.at(ui.definitionId(S::Colours, size_t(colour))).r == 12,
                          "UI definitions: a colour channel edit is read back");
                    int32_t font = -1;
                    const tr::XmlNode* fonts = ui.document()->node(S::Fonts, {});
                    for (size_t k = 0; fonts && k < fonts->children.size() && font < 0; ++k)
                        if (fonts->children[k].tag == "MultiFont") font = int32_t(k);
                    check(font >= 0 && !ui.renameDefinition(S::Fonts, size_t(font), "NoPrefix", false).empty(), "UI definitions: a font id needs \"Fonts.\"");
                    const int32_t anim = ui.addDefinition(S::Animations, false);
                    bool tracks = false, frame = false;
                    if (anim >= 0 && ui.setAnimationField(size_t(anim), "animType", "Bitmap").empty()) {
                        const tr::XmlNode* a = ui.document()->node(S::Animations, {anim});
                        tracks = a && a->child("uvTrack") && a->child("rotTrack") && a->child("textureTrack") && !a->child("scaleTrack") && !a->child("colorTrack");
                        int32_t uv = -1;
                        for (size_t k = 0; a && k < a->children.size(); ++k)
                            if (a->children[k].tag == "uvTrack") uv = int32_t(k);
                        frame = uv >= 0 && ui.addDefinitionChild(S::Animations, {anim, uv}).empty() &&
                                ui.document()->node(S::Animations, {anim, uv})->children.size() == 1;
                    }
                    check(tracks, "UI definitions: animType Bitmap brings the bitmap tracks");
                    check(frame, "UI definitions: a frame added to a track");
                    int32_t usedAnim = -1;
                    const tr::XmlNode* anims = ui.document()->node(S::Animations, {});
                    for (size_t k = 0; anims && k < anims->children.size() && usedAnim < 0; ++k) {
                        const tr::XmlNode* type = anims->children[k].child("animType");
                        if (type && type->text == "Widget" && !ui.findReferences(S::Animations, ui.definitionId(S::Animations, k)).empty()) usedAnim = int32_t(k);
                    }
                    check(usedAnim >= 0 && !ui.setAnimationField(size_t(usedAnim), "animType", "Bitmap").empty(),
                          "UI definitions: a Widget animation in use cannot become Bitmap");
                    ui.revertChanges();
                    check(ui.document()->text() == start, "UI definitions: changes reverted");
                }
            }
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
