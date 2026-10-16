#include "ServerWorkspace.h"

#include "imgui_internal.h"   // DockBuilder

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <cstdio>

ServerWorkspace::ServerWorkspace(EditorContext& ctx) : Workspace(ctx) {
    viewport_.options.showServer = true;             // the server layers are what this workspace is about
    viewport_.options.showMissionAreas = true;
}

void ServerWorkspace::buildDefaultLayout(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID left = 0, centre = 0, right = 0, rightBottom = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.22f, &left, &centre);
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.30f, &right, &centre);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.25f, &rightBottom, &right);
    ImGui::DockBuilderDockWindow("Maps###Server.Maps", left);
    ImGui::DockBuilderDockWindow("Server objects###Server.Objects", left);
    ImGui::DockBuilderDockWindow("Viewport###Server.Viewport", centre);      // first docked = the visible tab
    ImGui::DockBuilderDockWindow("Overview###Server.Overview", centre);
    ImGui::DockBuilderDockWindow("Inspector###Server.Inspector", right);
    ImGui::DockBuilderDockWindow("Creatures###Server.Creatures", right);
    ImGui::DockBuilderDockWindow("Missions###Server.Missions", right);
    ImGui::DockBuilderDockWindow("Loot###Server.Loot", right);
    ImGui::DockBuilderDockWindow("Crafting###Server.Crafting", right);
    ImGui::DockBuilderDockWindow("Map###Server.Map", rightBottom);
    ImGui::DockBuilderFinish(dockspace);
}

void ServerWorkspace::beginFrame() {
    if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) editItem_ = 0;     // interaction finished
    // RASAEDIT_SERVER_SELECT=<pool|teleporter|object|creature|module|recipe|mission>:<database id>: start with that server object (or creature, in the
    // Creatures panel) selected (previews), once the
    // map's content is loaded.
    static bool preselected = false;
    if (!preselected && serverContent_.mapContextId >= 0) {
        preselected = true;
        if (const char* spec = std::getenv("RASAEDIT_SERVER_SELECT"); spec && *spec) {
            const std::string s = spec;
            const size_t colon = s.find(':');
            const std::string kind = s.substr(0, colon);
            const int64_t id = colon == std::string::npos ? 0 : std::strtoll(s.c_str() + colon + 1, nullptr, 10);
            if (kind == "pool")
                for (size_t k = 0; k < serverContent_.spawnPools.size(); ++k)
                    if (serverContent_.spawnPools[k].id == id) selection_ = Selection{SelectionKind::SpawnPool, k};
            if (kind == "teleporter")
                for (size_t k = 0; k < serverContent_.teleporters.size(); ++k)
                    if (serverContent_.teleporters[k].id == id) selection_ = Selection{SelectionKind::Teleporter, k};
            if (kind == "module") {
                modules_.selected_ = id;
                craftingTab_ = 0;
                focusModules_ = 10;
            }
            if (kind == "recipe") {
                recipes_.selected_ = id;
                craftingTab_ = 2;
                focusModules_ = 10;
            }
            if (kind == "mission") {
                missions_.selectMission(id);
                focusMissions_ = 10;
            }
            if (kind == "creature") {
                creatureSelected_ = int32_t(id);
                focusCreatures_ = 10;                 // a few frames: the docked window may not exist on the first
            }
            if (kind == "object")
                for (size_t k = 0; k < serverContent_.objects.size(); ++k)
                    if (serverContent_.objects[k].id == id && serverContent_.objects[k].kind == ObjectKind::ControlPoint) selection_ = Selection{SelectionKind::Object, k};
            // A placed object is also shown in the viewport ("Show in viewport").
            if (selection_.kind == SelectionKind::SpawnPool || selection_.kind == SelectionKind::Teleporter || selection_.kind == SelectionKind::Object)
                viewport_.focusOn(ctx_, selection_);
            // Options after the id, ";key=value": zoom=<factor> frames it from that many times the usual distance, placeholders=0
            // turns the viewport's placeholder boxes off.
            for (size_t p = s.find(';'); p != std::string::npos; p = s.find(';', p + 1)) {
                const std::string option = s.substr(p + 1, s.find(';', p + 1) - p - 1);
                const size_t eq = option.find('=');
                const std::string key = option.substr(0, eq), value = eq == std::string::npos ? "" : option.substr(eq + 1);
                if (key == "placeholders") ctx_.view.showPlaceholders = value != "0";
                tr::Vec3 target;
                float distance = 0;
                if (key == "zoom" && ctx_.scene && ctx_.scene->focusTarget(selection_, target, distance))
                    ctx_.scene->focusOnPoint(target, distance * std::max(0.1f, float(std::atof(value.c_str()))));
            }
        }
    }
}

void ServerWorkspace::onMapLoaded() {
    loadServerContent();
    overview_.onMapLoaded(ctx_);
}

void ServerWorkspace::showMySqlDialog(const std::string& spec) {
    MySqlSpec m;
    const std::string current = spec.empty() ? ctx_.setting("database") : spec;
    if (parseMySqlSpec(current.find('@') != std::string::npos ? current : std::string(), m)) {
        std::snprintf(mySqlHost_, sizeof mySqlHost_, "%s", m.host.c_str());
        mySqlPort_ = m.port;
        std::snprintf(mySqlUser_, sizeof mySqlUser_, "%s", m.user.c_str());
        std::snprintf(mySqlDatabase_, sizeof mySqlDatabase_, "%s", m.database.c_str());
        std::snprintf(mySqlPassword_, sizeof mySqlPassword_, "%s", m.password.c_str());
    }
    mySqlRemember_ = !ctx_.setting("databasePassword").empty();
    mySqlDialogOpen_ = true;
}

void ServerWorkspace::drawMySqlDialog() {
    if (mySqlDialogOpen_) {
        ImGui::OpenPopup("Connect to a MySQL World database");
        mySqlDialogOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Connect to a MySQL World database", nullptr, ImGuiWindowFlags_NoResize)) return;
    ImGui::TextWrapped("The Rasa.NET World database on a MySQL 8 or MariaDB server (Provider \"MySql\" in Rasa.DBL's databasesettings). "
                       "Apply the server's pending migrations first - the editor reads the schema as it is.");
    ImGui::Separator();
    ImGui::InputText("Host", mySqlHost_, sizeof mySqlHost_);
    ImGui::InputInt("Port", &mySqlPort_, 0);
    mySqlPort_ = std::clamp(mySqlPort_, 1, 65535);
    ImGui::InputText("User", mySqlUser_, sizeof mySqlUser_);
    ImGui::InputText("Password", mySqlPassword_, sizeof mySqlPassword_, ImGuiInputTextFlags_Password);
    ImGui::InputText("Database", mySqlDatabase_, sizeof mySqlDatabase_);
    ImGui::Checkbox("Remember the password", &mySqlRemember_);
    ImGui::SetItemTooltip("Stored in the editor's settings file in plain text. Off: asked for each time the editor reconnects.");
    const bool ready = mySqlHost_[0] && mySqlUser_[0] && mySqlDatabase_[0];
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Connect", ImVec2(120, 0))) {
        // The spec keeps the password in memory only until the database is open (openDatabase stores it without).
        const std::string spec = "mysql://" + std::string(mySqlUser_) + (mySqlPassword_[0] ? ":" + std::string(mySqlPassword_) : std::string()) + "@" +
                                 mySqlHost_ + ":" + std::to_string(mySqlPort_) + "/" + mySqlDatabase_;
        rememberPasswordOnOpen_ = mySqlRemember_;
        if (!mySqlRemember_) ctx_.setSetting("databasePassword", "");
        ctx_.queueDatabase(spec);
        std::memset(mySqlPassword_, 0, sizeof mySqlPassword_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        std::memset(mySqlPassword_, 0, sizeof mySqlPassword_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ServerWorkspace::showDatabaseDialog() {
    static const SDL_DialogFileFilter filters[] = {{"SQLite database", "db;sqlite;sqlite3"}, {"All files", "*"}};
    const std::string current = ctx_.setting("database");
    SDL_ShowOpenFileDialog(databaseDialogCallback, this, ctx_.window, filters, 2, current.empty() ? nullptr : current.c_str(), false);
}

void ServerWorkspace::drawPanels() {
    maps_.draw(ctx_, "Maps###Server.Maps");
    drawMySqlDialog();
    drawServerPanel();
    drawInspectorPanel();
    drawCreaturePanel();
    drawMissionPanel();
    drawLootPanel();
    drawCraftingPanel();
    drawMapInfoPanel(ctx_, "Map###Server.Map");
    OverviewLayers layers;
    layers.selection = &selection_;
    layers.selectEntities = false;
    layers.dimEntities = true;
    layers.server = database_ ? &serverContent_ : nullptr;
    layers.database = database_.get();
    layers.selectServer = true;
    layers.showServer = viewport_.options.showServer;
    layers.showMissionAreas = viewport_.options.showMissionAreas;
    layers.showRegions = viewport_.options.showRegions;
    layers.clientMarkers = &clientMarkers_;
    layers.label = [this](const Selection& s) { return serverObjectLabel(s); };
    layers.position = [this](const Selection& s, tr::Vec3& p) { return selectionPosition(s, p); };
    layers.showInViewport = [this](const Selection& s) { viewport_.focusOn(ctx_, s); };
    overview_.draw(ctx_, "Overview###Server.Overview", layers);
    // Drawn last: on first creation the newest window becomes the selected tab of a shared dock node.
    viewport_.draw(ctx_, "Viewport###Server.Viewport", selection_, false, true, this);
}

void ServerWorkspace::drawInspectorPanel() {
    if (!ImGui::Begin("Inspector###Server.Inspector")) { ImGui::End(); return; }
    if (!ctx_.map || !isSelected(selection_)) ImGui::TextDisabled("Nothing selected.");
    else drawInspectorContent();
    ImGui::End();
}

void ServerWorkspace::drawEditMenu() {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undoStack_.empty())) undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redoStack_.empty())) redo();
    ImGui::Separator();
    const bool selected = canEditServer() && isSelected(selection_);
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selected)) duplicateSelected();
    if (ImGui::MenuItem("Delete", "Del", false, selected)) deleteSelected();
    ImGui::Separator();
    if (ImGui::MenuItem("Save Server Changes", "Ctrl+S", false, canEditServer() && hasUnsavedChanges())) saveServer();
    if (ImGui::MenuItem("Revert Server Changes", nullptr, false, hasUnsavedChanges())) revertServer();
}

void SDLCALL ServerWorkspace::databaseDialogCallback(void* userdata, const char* const* filelist, int) {
    auto* workspace = static_cast<ServerWorkspace*>(userdata);
    if (filelist == nullptr || filelist[0] == nullptr) return;
    workspace->ctx_.queueDatabase(filelist[0]);
}

void ServerWorkspace::openDatabase(const std::string& path) {
    try {
        const char* base = SDL_GetBasePath();
        auto database = std::make_unique<WorldDatabase>(path, defaultMySqlPluginDir(base ? base : ""));
        if (ctx_.scene) ctx_.scene->setServerContent(nullptr, nullptr, nullptr);
        serverContent_ = WorldContent{};
        if (selection_.kind != SelectionKind::MapEntity) selection_ = Selection{};
        database_ = std::move(database);
        reloadCreatureDefinitions();
        try {
            missions_.load(*database_);
        } catch (const std::exception& e) {
            missions_.clear();
            ctx_.setStatus(std::string("Mission tables not loaded: ") + e.what(), true);
        }
        try {
            loot_.load(*database_);
        } catch (const std::exception& e) {
            loot_.clear();
            ctx_.setStatus(std::string("Loot tables not loaded: ") + e.what(), true);
        }
        try {
            modules_.load(*database_);
        } catch (const std::exception& e) {
            modules_.clear();
            ctx_.setStatus(std::string("Item module tables not loaded: ") + e.what(), true);
        }
        try {
            recipes_.load(*database_);
        } catch (const std::exception& e) {
            recipes_.clear();
            ctx_.setStatus(std::string("Recipe tables not loaded: ") + e.what(), true);
        }
        // Remembered without the password (WorldDatabase::path); a MySQL password only when the user asked for it.
        ctx_.setSetting("database", database_->path());
        if (database_->isMySql() && rememberPasswordOnOpen_) {
            MySqlSpec m;
            if (parseMySqlSpec(path, m)) ctx_.setSetting("databasePassword", m.password);
        }
        rememberPasswordOnOpen_ = false;
        loadServerContent();
        ctx_.setStatus("Opened server database " + database_->path() + (database_->warnings().empty() ? "" : "  (with schema warnings, see the Server panel)"),
                       !database_->warnings().empty());
    } catch (const std::exception& e) {
        ctx_.setStatus(std::string("Could not open server database: ") + e.what(), true);
        rememberPasswordOnOpen_ = false;
        if (isMySqlSpec(path)) showMySqlDialog(path);  // let the user correct the connection (the password field starts empty)
    }
}

void ServerWorkspace::loadServerContent() {
    if (ctx_.scene) ctx_.scene->setServerContent(nullptr, nullptr, nullptr);
    serverContent_ = WorldContent{};
    savedContent_ = WorldContent{};
    undoStack_.clear();
    redoStack_.clear();
    cleanUndoDepth_ = 0;
    cleanReachable_ = true;
    if (selection_.kind != SelectionKind::MapEntity && selection_.kind != SelectionKind::None) selection_ = Selection{};
    if (!database_ || !ctx_.map) return;
    int32_t context = database_->contextIdForMap(ctx_.map->name);
    if (context < 0 && ctx_.gameData && ctx_.mapTemplateId >= 0) {
        const std::vector<int32_t> contexts = ctx_.gameData->contextIdsForMapTemplate(ctx_.mapTemplateId);
        if (!contexts.empty()) context = contexts.front();
    }
    if (context < 0) return;
    clientMarkers_.clear();
    if (ctx_.gameData && ctx_.mapTemplateId >= 0) {
        try { clientMarkers_ = ctx_.gameData->factionedMapMarkers(ctx_.mapTemplateId); } catch (const std::exception&) {}
    }
    if (ctx_.scene) {
        std::vector<SceneView::ClientMapMarker> drawn;
        for (const tr::MapScreenMarker& m : clientMarkers_) drawn.push_back(SceneView::ClientMapMarker{m.entityId, m.type, tr::Vec3{m.x, m.y, m.z}});
        ctx_.scene->setClientMapMarkers(std::move(drawn));
    }
    try {
        serverContent_ = database_->loadMap(context);
        savedContent_ = serverContent_;
    } catch (const std::exception& e) {
        ctx_.setStatus(std::string("Could not read server content: ") + e.what(), true);
        return;
    }
    if (ctx_.scene)
        ctx_.scene->setServerContent(&serverContent_, database_.get(), [this](int32_t id) { return ctx_.classInfo(id).mesh; },
                                     [this](const Creature& creature) {
                                         if (!ctx_.gameData || !database_) return AvatarLook{};
                                         try {
                                             return resolveAvatar(*ctx_.gameData, *database_, creature);
                                         } catch (const std::exception&) {
                                             return AvatarLook{};     // incomplete game data: draw the bare class mesh
                                         }
                                     });
}

bool ServerWorkspace::selectionPosition(const Selection& s, tr::Vec3& position) const {
    const size_t i = s.index;
    switch (s.kind) {
        case SelectionKind::MapEntity: if (ctx_.map && i < ctx_.map->map.entities.size()) { position = ctx_.map->map.entities[i].location; return true; } break;
        case SelectionKind::SpawnPool: if (i < serverContent_.spawnPools.size()) { position = serverContent_.spawnPools[i].position; return true; } break;
        case SelectionKind::Teleporter: if (i < serverContent_.teleporters.size()) { position = serverContent_.teleporters[i].position; return true; } break;
        case SelectionKind::MapLink: if (i < serverContent_.mapLinks.size()) { position = serverContent_.mapLinks[i].position; return true; } break;
        case SelectionKind::Region: if (i < serverContent_.regions.size()) { position = serverContent_.regions[i].area.position; return true; } break;
        case SelectionKind::MissionArea: if (i < serverContent_.missionAreas.size()) { position = serverContent_.missionAreas[i].area.position; return true; } break;
        case SelectionKind::Object: if (i < serverContent_.objects.size()) { position = serverContent_.objects[i].position; return true; } break;
        case SelectionKind::Arrival: if (i < serverContent_.arrivals.size()) { position = serverContent_.arrivals[i].position; return true; } break;
        case SelectionKind::PatrolStep: if (i < serverContent_.patrolSteps.size()) { position = serverContent_.patrolSteps[i].position; return true; } break;
        case SelectionKind::WormholeLock:
            if (i < serverContent_.wormholeLocks.size())
                for (const Teleporter& t : serverContent_.teleporters)
                    if (t.id == serverContent_.wormholeLocks[i].fromTeleporterId) { position = t.position; return true; }
            break;
        default: break;
    }
    return false;
}

std::string ServerWorkspace::serverObjectLabel(const Selection& s) const {
    char text[256];
    const size_t i = s.index;
    switch (s.kind) {
        case SelectionKind::SpawnPool: {
            if (i >= serverContent_.spawnPools.size()) break;
            const SpawnPool& p = serverContent_.spawnPools[i];
            const Creature* c = database_ ? database_->creature(p.slot1.creatureId) : nullptr;
            int slots = 0;
            for (int k = 0; k < 6; ++k) if (spawnSlot(p, k).creatureId > 0) ++slots;
            const bool npc = c && database_->classIsNpc(c->classId), vendor = c && database_->classIsVendor(c->classId);
            std::snprintf(text, sizeof text, "Spawn %lld: %s%s%s", static_cast<long long>(p.id), c ? c->comment.c_str() : "(no creature)",
                          vendor ? " [vendor]" : npc ? " [NPC]" : "", slots > 1 ? (" +" + std::to_string(slots - 1) + " more").c_str() : "");
            return text;
        }
        case SelectionKind::Teleporter:
            if (i >= serverContent_.teleporters.size()) break;
            std::snprintf(text, sizeof text, "Teleporter %lld (type %d): %s", static_cast<long long>(serverContent_.teleporters[i].id),
                          serverContent_.teleporters[i].type, serverContent_.teleporters[i].description.c_str());
            return text;
        case SelectionKind::MapLink: {
            if (i >= serverContent_.mapLinks.size()) break;
            const MapLink& l = serverContent_.mapLinks[i];
            std::string destination = std::to_string(l.destinationMapContextId);
            if (database_) for (const MapInfo& m : database_->maps()) if (m.mapContextId == l.destinationMapContextId) destination = m.name;
            std::snprintf(text, sizeof text, "Map link %lld -> %s%s", static_cast<long long>(l.id), destination.c_str(), l.enabled ? "" : " (disabled)");
            return text;
        }
        case SelectionKind::Region:
            if (i >= serverContent_.regions.size()) break;
            std::snprintf(text, sizeof text, "Region %d: %s", serverContent_.regions[i].regionId, serverContent_.regions[i].comment.c_str());
            return text;
        case SelectionKind::MissionArea:
            if (i >= serverContent_.missionAreas.size()) break;
            std::snprintf(text, sizeof text, "Mission %d area %d: %s", serverContent_.missionAreas[i].missionId, serverContent_.missionAreas[i].areaId,
                          serverContent_.missionAreas[i].comment.c_str());
            return text;
        case SelectionKind::Object:
            if (i >= serverContent_.objects.size()) break;
            std::snprintf(text, sizeof text, "%s %lld: %s", objectKindName(serverContent_.objects[i].kind), static_cast<long long>(serverContent_.objects[i].id),
                          serverContent_.objects[i].name.c_str());
            return text;
        case SelectionKind::Arrival: {
            if (i >= serverContent_.arrivals.size()) break;
            const SpawnArrival& a = serverContent_.arrivals[i];
            std::snprintf(text, sizeof text, "Arrival %lld (%s) for spawn %lld: %s", static_cast<long long>(a.id), arrivalKindName(a.kind),
                          static_cast<long long>(a.poolId), a.comment.c_str());
            return text;
        }
        case SelectionKind::PatrolStep: {
            if (i >= serverContent_.patrolSteps.size()) break;
            const PatrolStep& p = serverContent_.patrolSteps[i];
            std::snprintf(text, sizeof text, "Patrol of spawn %lld, step %d%s%s", static_cast<long long>(p.poolId), p.step,
                          p.hasFacing ? ", turns" : "", p.pauseMs > 0 ? ", pauses" : "");
            return text;
        }
        case SelectionKind::WormholeLock: {
            if (i >= serverContent_.wormholeLocks.size()) break;
            const WormholeLock& w = serverContent_.wormholeLocks[i];
            auto end = [](int64_t id) { return id == 0 ? std::string("any") : std::to_string(id); };
            std::snprintf(text, sizeof text, "Wormhole lock %lld: %s -> %s, %s %lld%s%s", static_cast<long long>(w.id), end(w.fromTeleporterId).c_str(),
                          end(w.toTeleporterId).c_str(), w.kind == 1 ? "mission" : w.kind == 2 ? "level" : "kind?", static_cast<long long>(w.value),
                          w.hasComment && !w.comment.empty() ? ": " : "", w.hasComment ? w.comment.c_str() : "");
            return text;
        }
        default: break;
    }
    return "";
}

void ServerWorkspace::drawServerPanel() {
    if (!ImGui::Begin("Server objects###Server.Objects")) { ImGui::End(); return; }
    if (!database_) {
        ImGui::TextWrapped("No server database open. Use File > Open Server Database... and choose the Rasa.NET World database "
                           "(rasaworld.db with the SQLite provider), or File > Connect to MySQL Database... for the MySQL provider.");
        if (ImGui::Button("Connect to MySQL...")) showMySqlDialog();
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", database_->path().c_str());
    ImGui::Text("%zu maps, %zu creatures (read-only)", database_->maps().size(), database_->creatureCount());
    if (!database_->warnings().empty() && ImGui::CollapsingHeader("Schema warnings"))
        for (const std::string& w : database_->warnings()) ImGui::TextWrapped("%s", w.c_str());
    if (!ctx_.map) { ImGui::TextDisabled("Select a map."); ImGui::End(); return; }
    if (serverContent_.mapContextId < 0) {
        ImGui::TextDisabled("This map has no entry in map_info.");
        ImGui::End();
        return;
    }
    ImGui::Text("Map context %d", serverContent_.mapContextId);
    if (database_->readOnly()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Read-only: the database file is not writable.");
    } else {
        if (ImGui::Button("Add...")) ImGui::OpenPopup("addserverobject");
        if (ImGui::BeginPopup("addserverobject")) {
            ImGui::TextDisabled("Placed on the ground at the centre of the viewport");
            if (ImGui::MenuItem("Spawn pool")) addServerObject(SelectionKind::SpawnPool, ObjectKind::Footlocker);
            if (ImGui::MenuItem("Teleporter")) addServerObject(SelectionKind::Teleporter, ObjectKind::Footlocker);
            if (ImGui::MenuItem("Map link")) addServerObject(SelectionKind::MapLink, ObjectKind::Footlocker);
            {
                const bool poolSaved = selection_.kind == SelectionKind::SpawnPool && selection_.index < serverContent_.spawnPools.size() &&
                                       serverContent_.spawnPools[selection_.index].id > 0;
                if (ImGui::MenuItem("Spawn arrival (for the selected spawn pool)", nullptr, false, poolSaved))
                    addServerObject(SelectionKind::Arrival, ObjectKind::Footlocker);
                const bool patrolStep = selection_.kind == SelectionKind::PatrolStep && selection_.index < serverContent_.patrolSteps.size();
                if (database_ && database_->hasTable("spawnpool_patrol") &&
                    ImGui::MenuItem("Patrol step (for the selected spawn pool or after the selected step)", nullptr, false, poolSaved || patrolStep))
                    addServerObject(SelectionKind::PatrolStep, ObjectKind::Footlocker);
            }
            if (ImGui::MenuItem("Footlocker")) addServerObject(SelectionKind::Object, ObjectKind::Footlocker);
            if (database_ && database_->hasTable("ambient_npc") && ImGui::MenuItem("Ambient NPC"))
                addServerObject(SelectionKind::Object, ObjectKind::AmbientNpc);
            if (database_ && database_->hasTable("world_usable") && ImGui::MenuItem("World usable"))
                addServerObject(SelectionKind::Object, ObjectKind::WorldUsable);
            if (database_ && database_->hasTable("world_destructible") && ImGui::MenuItem("World destructible"))
                addServerObject(SelectionKind::Object, ObjectKind::WorldDestructible);
            if (database_ && database_->hasTable("map_emitter") && ImGui::MenuItem("FX emitter"))
                addServerObject(SelectionKind::Object, ObjectKind::Emitter);
            if (database_ && database_->hasTable("wormhole_lock") && ImGui::MenuItem("Wormhole lock"))
                addServerObject(SelectionKind::WormholeLock, ObjectKind::Footlocker);
            if (ImGui::MenuItem("Kraftwerk")) addServerObject(SelectionKind::Object, ObjectKind::Kraftwerk);
            if (ImGui::MenuItem("Logos stone")) addServerObject(SelectionKind::Object, ObjectKind::Logos);
            if (controlPointsEditable() && ImGui::MenuItem("Control point")) addServerObject(SelectionKind::Object, ObjectKind::ControlPoint);
            ImGui::EndPopup();
        }
        const bool serverSelected = isSelected(selection_) && selection_.kind != SelectionKind::MapEntity;
        ImGui::SameLine();
        if (!serverSelected) ImGui::BeginDisabled();
        if (ImGui::Button("Duplicate")) duplicateSelected();
        ImGui::SameLine();
        if (ImGui::Button("Delete")) deleteSelected();
        if (!serverSelected) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!hasUnsavedChanges()) ImGui::BeginDisabled();
        if (ImGui::Button("Save")) saveServer();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) revertServer();
        if (!hasUnsavedChanges()) ImGui::EndDisabled();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##serverfilter", "filter", serverFilter_, sizeof serverFilter_);
    const std::string needle = tr::toLowerAscii(serverFilter_);

    auto list = [&](const char* title, SelectionKind kind, size_t count) {
        char header[96];
        std::snprintf(header, sizeof header, "%s (%zu)###%s", title, count, title);
        if (!ImGui::TreeNodeEx(header, count ? 0 : ImGuiTreeNodeFlags_Leaf)) return;
        for (size_t k = 0; k < count; ++k) {
            const Selection s{kind, k};
            const std::string label = serverObjectLabel(s);
            if (!needle.empty() && tr::toLowerAscii(label).find(needle) == std::string::npos) continue;
            ImGui::PushID(int(k));
            if (ImGui::Selectable(label.c_str(), sameSelection(selection_, s), ImGuiSelectableFlags_AllowDoubleClick)) {
                selection_ = s;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) viewport_.focusOn(ctx_, s);
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    };
    if (ImGui::BeginChild("serverlist")) {
        list("Spawn pools", SelectionKind::SpawnPool, serverContent_.spawnPools.size());
        list("Spawn arrivals", SelectionKind::Arrival, serverContent_.arrivals.size());
        list("Patrol steps", SelectionKind::PatrolStep, serverContent_.patrolSteps.size());
        list("Wormhole locks (all maps)", SelectionKind::WormholeLock, serverContent_.wormholeLocks.size());
        list("Teleporters", SelectionKind::Teleporter, serverContent_.teleporters.size());
        list("Objects", SelectionKind::Object, serverContent_.objects.size());
        list("Map links", SelectionKind::MapLink, serverContent_.mapLinks.size());
        list("Mission areas", SelectionKind::MissionArea, serverContent_.missionAreas.size());
        list("Regions", SelectionKind::Region, serverContent_.regions.size());
        drawMarkerList();
    }
    ImGui::EndChild();
    ImGui::End();
}

void ServerWorkspace::drawInspectorContent() {
    if (canEditServer()) {
        drawServerObjectEditor();
        return;
    }
    const size_t i = selection_.index;
    auto placement = [](const tr::Vec3& p, float rotation) {
        ImGui::Text("Position  %.2f, %.2f, %.2f", p.x, p.y, p.z);
        ImGui::Text("Facing    %.1f deg (%.3f rad)", rotation * 57.29578f, rotation);
    };
    auto creatureLine = [&](const char* label, const SpawnSlot& slot) {
        if (slot.creatureId <= 0) return;
        const Creature* c = database_ ? database_->creature(slot.creatureId) : nullptr;
        if (!c) { ImGui::Text("%s creature %d (not in the creature table)  x%d-%d", label, slot.creatureId, slot.minCount, slot.maxCount); return; }
        const ClassInfo& info = ctx_.classInfo(c->classId);
        ImGui::Text("%s creature %d  %s  x%d-%d", label, c->id, c->comment.c_str(), slot.minCount, slot.maxCount);
        ImGui::TextDisabled("      class %d %s, level %d, faction %d, hp %d%s%s", c->classId, info.name.c_str(), c->level, c->faction, c->maxHp,
                            database_->classIsNpc(c->classId) ? ", NPC" : "", database_->classIsVendor(c->classId) ? ", vendor" : "");
    };
    ImGui::TextWrapped("%s", serverObjectLabel(selection_).c_str());
    ImGui::Separator();
    switch (selection_.kind) {
        case SelectionKind::SpawnPool: {
            if (i >= serverContent_.spawnPools.size()) break;
            const SpawnPool& p = serverContent_.spawnPools[i];
            placement(p.position, p.rotation);
            ImGui::Text("Radius %.1f m, respawn %d s", p.radius, p.respawnTime);
            ImGui::Text("Mode %s, arrival %s", spawnModeName(p.mode), spawnAnimationName(p.animType));
            ImGui::SeparatorText("Creatures");
            const char* labels[6] = {"1", "2", "3", "4", "5", "6"};
            for (int k = 0; k < 6; ++k) creatureLine(labels[k], spawnSlot(p, k));
            int arrivals = 0;
            for (const SpawnArrival& a : serverContent_.arrivals) if (a.poolId == p.id) ++arrivals;
            if (arrivals) ImGui::Text("%d arrival point(s)", arrivals);
            int steps = 0;
            for (const PatrolStep& s : serverContent_.patrolSteps) if (s.poolId == p.id) ++steps;
            if (steps) ImGui::Text("Patrol of %d step(s)", steps);
            for (const PoolPose& pose : serverContent_.poses)
                if (pose.poolId == p.id) ImGui::Text("Pose %d (%s)", pose.pose, npcPoseName(pose.pose));
            break;
        }
        case SelectionKind::PatrolStep: {
            if (i >= serverContent_.patrolSteps.size()) break;
            const PatrolStep& s = serverContent_.patrolSteps[i];
            placement(s.position, s.hasFacing ? s.facing : 0.0f);
            ImGui::Text("Spawn pool %lld, step %d", static_cast<long long>(s.poolId), s.step);
            ImGui::Text("%s, pause %d ms", s.hasFacing ? "turns to the facing above" : "keeps facing the way it came", s.pauseMs);
            break;
        }
        case SelectionKind::Arrival: {
            if (i >= serverContent_.arrivals.size()) break;
            const SpawnArrival& a = serverContent_.arrivals[i];
            placement(a.position, a.rotation);
            ImGui::Text("Kind %d (%s), spawn pool %lld", a.kind, arrivalKindName(a.kind), static_cast<long long>(a.poolId));
            if (a.kind == 2) ImGui::Text("Teleporter entity %lld", static_cast<long long>(a.entityId));
            break;
        }
        case SelectionKind::Teleporter: {
            if (i >= serverContent_.teleporters.size()) break;
            const Teleporter& t = serverContent_.teleporters[i];
            placement(t.position, t.rotation);
            ImGui::Text("Class %d %s", t.classId, ctx_.classInfo(t.classId).name.c_str());
            break;
        }
        case SelectionKind::Object: {
            if (i >= serverContent_.objects.size()) break;
            const PlacedObject& o = serverContent_.objects[i];
            placement(o.position, o.rotation);
            ImGui::Text("Class %d %s", o.classId, ctx_.classInfo(o.classId).name.c_str());
            break;
        }
        case SelectionKind::WormholeLock:
            break;                                     // the label says it all
        case SelectionKind::MapLink: {
            if (i >= serverContent_.mapLinks.size()) break;
            const MapLink& l = serverContent_.mapLinks[i];
            ImGui::Text("Position  %.2f, %.2f, %.2f   radius %.1f", l.position.x, l.position.y, l.position.z, l.radius);
            ImGui::Text("Kind %d, %s", l.kind, l.enabled ? "enabled" : "disabled");
            ImGui::Text("Destination context %d at %.1f, %.1f, %.1f facing %.1f deg", l.destinationMapContextId, l.destinationPosition.x,
                        l.destinationPosition.y, l.destinationPosition.z, l.destinationRotation * 57.29578f);
            break;
        }
        case SelectionKind::Region:
        case SelectionKind::MissionArea: {
            const AreaShape* a = nullptr;
            if (selection_.kind == SelectionKind::Region && i < serverContent_.regions.size()) a = &serverContent_.regions[i].area;
            if (selection_.kind == SelectionKind::MissionArea && i < serverContent_.missionAreas.size()) a = &serverContent_.missionAreas[i].area;
            if (!a) break;
            ImGui::Text("Centre %.1f, %.1f, %.1f", a->position.x, a->position.y, a->position.z);
            if (a->shape == 2) ImGui::Text("Box %.1f x %.1f m", a->halfX * 2, a->halfZ * 2);
            else ImGui::Text("Circle, radius %.1f m", a->radius);
            ImGui::Text("Height %.0f .. %.0f", a->minY, a->maxY);
            if (selection_.kind == SelectionKind::Region)
                ImGui::Text("%s, %s", serverContent_.regions[i].underground == 1 ? "underground only" : serverContent_.regions[i].underground == 2 ? "surface only" : "anywhere",
                            serverContent_.regions[i].enabled ? "enabled" : "disabled");
            break;
        }
        default: break;
    }
    ImGui::Spacing();
    if (ImGui::Button("Show in overview")) overview_.requestFocus();
    ImGui::SameLine();
    if (ImGui::Button("Show in viewport")) viewport_.focusOn(ctx_, selection_);
}

void ServerWorkspace::beginEdit() {
    if (undoStack_.size() < cleanUndoDepth_) cleanReachable_ = false;       // the saved state is no longer reachable by redo
    undoStack_.push_back(serverContent_);
    redoStack_.clear();
    if (undoStack_.size() > 200) {                                           // keep memory bounded
        undoStack_.erase(undoStack_.begin());
        if (cleanUndoDepth_ > 0) --cleanUndoDepth_;
        else cleanReachable_ = false;
    }
}

void ServerWorkspace::noteEdit() {
    const ImGuiID id = ImGui::GetItemID();
    if (id != editItem_) {
        beginEdit();
        editItem_ = id;
    }
}

void ServerWorkspace::contentChanged() {
    validateSelection();
    if (ctx_.scene) ctx_.scene->refreshServerContent();
}

void ServerWorkspace::validateSelection() {
    size_t count = 0;
    switch (selection_.kind) {
        case SelectionKind::SpawnPool: count = serverContent_.spawnPools.size(); break;
        case SelectionKind::Teleporter: count = serverContent_.teleporters.size(); break;
        case SelectionKind::MapLink: count = serverContent_.mapLinks.size(); break;
        case SelectionKind::Region: count = serverContent_.regions.size(); break;
        case SelectionKind::MissionArea: count = serverContent_.missionAreas.size(); break;
        case SelectionKind::Object: count = serverContent_.objects.size(); break;
        case SelectionKind::Arrival: count = serverContent_.arrivals.size(); break;
        case SelectionKind::PatrolStep: count = serverContent_.patrolSteps.size(); break;
        case SelectionKind::WormholeLock: count = serverContent_.wormholeLocks.size(); break;
        default: return;
    }
    if (selection_.index >= count) selection_ = Selection{};
}

void ServerWorkspace::undo() {
    if (undoStack_.empty()) return;
    redoStack_.push_back(serverContent_);
    serverContent_ = undoStack_.back();
    undoStack_.pop_back();
    contentChanged();
}

void ServerWorkspace::redo() {
    if (redoStack_.empty()) return;
    undoStack_.push_back(serverContent_);
    serverContent_ = redoStack_.back();
    redoStack_.pop_back();
    contentChanged();
}

bool ServerWorkspace::saveServer() {
    if (!canEditServer()) return false;
    WorldDatabase::SaveSummary summary;
    std::string error;
    if (!database_->save(savedContent_, serverContent_, summary, error)) {
        ctx_.setStatus("Save failed, nothing was written: " + error, true);
        return false;
    }
    const Selection keep = selection_;
    try {
        serverContent_ = database_->loadMap(serverContent_.mapContextId);     // new rows get their database ids
    } catch (const std::exception& e) {
        ctx_.setStatus(std::string("Saved, but reloading failed: ") + e.what(), true);
    }
    savedContent_ = serverContent_;
    undoStack_.clear();
    redoStack_.clear();
    cleanUndoDepth_ = 0;
    cleanReachable_ = true;
    selection_ = keep;                                                       // rows stay in id order; new rows come last
    contentChanged();
    ctx_.setStatus("Saved: " + std::to_string(summary.inserted) + " added, " + std::to_string(summary.updated) + " changed, " +
                  std::to_string(summary.deleted) + " deleted. Restart the server to apply.", false);
    return true;
}

void ServerWorkspace::revertServer() {
    if (!hasUnsavedChanges()) return;
    beginEdit();                                                             // revert itself can be undone
    serverContent_ = savedContent_;
    contentChanged();
    ctx_.setStatus("Reverted to the saved server content (Ctrl+Z undoes the revert).", false);
}

tr::Vec3* ServerWorkspace::selectedPosition() {
    const size_t i = selection_.index;
    switch (selection_.kind) {
        case SelectionKind::SpawnPool: return i < serverContent_.spawnPools.size() ? &serverContent_.spawnPools[i].position : nullptr;
        case SelectionKind::Teleporter: return i < serverContent_.teleporters.size() ? &serverContent_.teleporters[i].position : nullptr;
        case SelectionKind::MapLink: return i < serverContent_.mapLinks.size() ? &serverContent_.mapLinks[i].position : nullptr;
        case SelectionKind::Region: return i < serverContent_.regions.size() ? &serverContent_.regions[i].area.position : nullptr;
        case SelectionKind::MissionArea: return i < serverContent_.missionAreas.size() ? &serverContent_.missionAreas[i].area.position : nullptr;
        case SelectionKind::Object: return i < serverContent_.objects.size() ? &serverContent_.objects[i].position : nullptr;
        case SelectionKind::Arrival: return i < serverContent_.arrivals.size() ? &serverContent_.arrivals[i].position : nullptr;
        case SelectionKind::PatrolStep: return i < serverContent_.patrolSteps.size() ? &serverContent_.patrolSteps[i].position : nullptr;
        default: return nullptr;
    }
}

float* ServerWorkspace::selectedRotation() {
    const size_t i = selection_.index;
    switch (selection_.kind) {
        case SelectionKind::SpawnPool: return i < serverContent_.spawnPools.size() ? &serverContent_.spawnPools[i].rotation : nullptr;
        case SelectionKind::Teleporter: return i < serverContent_.teleporters.size() ? &serverContent_.teleporters[i].rotation : nullptr;
        case SelectionKind::Object:
            return i < serverContent_.objects.size() && serverContent_.objects[i].kind != ObjectKind::Logos ? &serverContent_.objects[i].rotation : nullptr;
        case SelectionKind::Arrival: return i < serverContent_.arrivals.size() ? &serverContent_.arrivals[i].rotation : nullptr;
        case SelectionKind::PatrolStep:            // only a step that turns the creature has a facing
            return i < serverContent_.patrolSteps.size() && serverContent_.patrolSteps[i].hasFacing ? &serverContent_.patrolSteps[i].facing : nullptr;
        default: return nullptr;
    }
}

void ServerWorkspace::deleteSelected() {
    if (!canEditServer()) return;
    const size_t i = selection_.index;
    std::string what;
    switch (selection_.kind) {
        case SelectionKind::SpawnPool: {
            if (i >= serverContent_.spawnPools.size()) return;
            int arrivals = 0;
            for (const SpawnArrival& a : serverContent_.arrivals) if (a.poolId == serverContent_.spawnPools[i].id) ++arrivals;
            beginEdit();
            const int64_t poolId = serverContent_.spawnPools[i].id;
            serverContent_.spawnPools.erase(serverContent_.spawnPools.begin() + std::ptrdiff_t(i));
            if (poolId > 0) {                                        // the database deletes them with the pool
                serverContent_.arrivals.erase(std::remove_if(serverContent_.arrivals.begin(), serverContent_.arrivals.end(),
                                                             [&](const SpawnArrival& a) { return a.poolId == poolId; }),
                                              serverContent_.arrivals.end());
                serverContent_.patrolSteps.erase(std::remove_if(serverContent_.patrolSteps.begin(), serverContent_.patrolSteps.end(),
                                                                [&](const PatrolStep& s) { return s.poolId == poolId; }),
                                                 serverContent_.patrolSteps.end());
                serverContent_.poses.erase(std::remove_if(serverContent_.poses.begin(), serverContent_.poses.end(),
                                                          [&](const PoolPose& p) { return p.poolId == poolId; }),
                                           serverContent_.poses.end());
                removeControlPointLinks({1, 2}, poolId);
            }
            what = "spawn pool" + (arrivals ? " and its " + std::to_string(arrivals) + " arrival point(s)" : std::string());
            break;
        }
        case SelectionKind::Arrival:
            if (i >= serverContent_.arrivals.size()) return;
            beginEdit();
            serverContent_.arrivals.erase(serverContent_.arrivals.begin() + std::ptrdiff_t(i));
            what = "arrival point";
            break;
        case SelectionKind::PatrolStep:
            if (i >= serverContent_.patrolSteps.size()) return;
            beginEdit();
            serverContent_.patrolSteps.erase(serverContent_.patrolSteps.begin() + std::ptrdiff_t(i));
            what = "patrol step";
            break;
        case SelectionKind::WormholeLock:
            if (i >= serverContent_.wormholeLocks.size()) return;
            beginEdit();
            serverContent_.wormholeLocks.erase(serverContent_.wormholeLocks.begin() + std::ptrdiff_t(i));
            what = "wormhole lock";
            break;
        case SelectionKind::Teleporter:
            if (i >= serverContent_.teleporters.size()) return;
            beginEdit();
            if (serverContent_.teleporters[i].id > 0) {
                removeControlPointLinks({3, 4}, serverContent_.teleporters[i].id);
                removeMarkerBindings(1, serverContent_.teleporters[i].id);
            }
            serverContent_.teleporters.erase(serverContent_.teleporters.begin() + std::ptrdiff_t(i));
            what = "teleporter";
            break;
        case SelectionKind::MapLink:
            if (i >= serverContent_.mapLinks.size()) return;
            beginEdit();
            serverContent_.mapLinks.erase(serverContent_.mapLinks.begin() + std::ptrdiff_t(i));
            what = "map link";
            break;
        case SelectionKind::Object:
            if (i >= serverContent_.objects.size()) return;
            if (serverContent_.objects[i].kind == ObjectKind::ControlPoint && !controlPointsEditable()) {
                ctx_.setStatus("Control points can be edited but not deleted (this world's control_point table has no marker / owner columns).", true);
                return;
            }
            beginEdit();
            what = objectKindName(serverContent_.objects[i].kind);
            if (serverContent_.objects[i].kind == ObjectKind::ControlPoint) {
                // Its links go with it (the save deletes them too).
                const int64_t id = serverContent_.objects[i].id;
                auto& links = serverContent_.controlPointLinks;
                links.erase(std::remove_if(links.begin(), links.end(), [&](const ControlPointLink& l) { return l.controlPointId == id; }), links.end());
            }
            if (serverContent_.objects[i].kind == ObjectKind::Footlocker && serverContent_.objects[i].id > 0)
                removeControlPointLinks({6}, serverContent_.objects[i].id);
            if (serverContent_.objects[i].kind == ObjectKind::Kraftwerk && serverContent_.objects[i].id > 0) removeMarkerBindings(2, serverContent_.objects[i].id);
            serverContent_.objects.erase(serverContent_.objects.begin() + std::ptrdiff_t(i));
            break;
        default:
            ctx_.setStatus("Regions and mission areas can be edited but not deleted.", true);
            return;
    }
    selection_ = Selection{};
    contentChanged();
    ctx_.setStatus("Deleted " + what + " (Ctrl+Z to undo; not saved yet).", false);
}

void ServerWorkspace::duplicateSelected() {
    if (!canEditServer() || !ctx_.scene) return;
    const size_t i = selection_.index;
    const tr::Vec3 offset = scaled(ctx_.scene->camera().right(), 2.0f);
    auto shifted = [&](tr::Vec3 p) {
        p = add(p, tr::Vec3{offset.x, 0, offset.z});
        if (snapToGround_) p.y = ctx_.scene->groundHeight(p.x, p.z, p.y);
        return p;
    };
    switch (selection_.kind) {
        case SelectionKind::SpawnPool: {
            if (i >= serverContent_.spawnPools.size()) return;
            SpawnPool copy = serverContent_.spawnPools[i];
            copy.id = 0;
            copy.position = shifted(copy.position);
            beginEdit();
            serverContent_.spawnPools.push_back(copy);
            selection_ = Selection{SelectionKind::SpawnPool, serverContent_.spawnPools.size() - 1};
            break;
        }
        case SelectionKind::Arrival: {
            if (i >= serverContent_.arrivals.size()) return;
            SpawnArrival copy = serverContent_.arrivals[i];
            copy.id = 0;
            copy.position = shifted(copy.position);
            beginEdit();
            serverContent_.arrivals.push_back(copy);
            selection_ = Selection{SelectionKind::Arrival, serverContent_.arrivals.size() - 1};
            break;
        }
        case SelectionKind::WormholeLock: {
            if (i >= serverContent_.wormholeLocks.size()) return;
            WormholeLock copy = serverContent_.wormholeLocks[i];
            copy.id = 0;
            beginEdit();
            serverContent_.wormholeLocks.push_back(copy);
            selection_ = Selection{SelectionKind::WormholeLock, serverContent_.wormholeLocks.size() - 1};
            break;
        }
        case SelectionKind::PatrolStep: {
            if (i >= serverContent_.patrolSteps.size()) return;
            const PatrolStep source = serverContent_.patrolSteps[i];
            beginEdit();
            insertPatrolStep(source.poolId, source.step, shifted(source.position));
            break;
        }
        case SelectionKind::Teleporter: {
            if (i >= serverContent_.teleporters.size()) return;
            Teleporter copy = serverContent_.teleporters[i];
            copy.id = 0;
            copy.position = shifted(copy.position);
            beginEdit();
            serverContent_.teleporters.push_back(copy);
            selection_ = Selection{SelectionKind::Teleporter, serverContent_.teleporters.size() - 1};
            break;
        }
        case SelectionKind::MapLink: {
            if (i >= serverContent_.mapLinks.size()) return;
            MapLink copy = serverContent_.mapLinks[i];
            copy.id = 0;
            copy.position = shifted(copy.position);
            beginEdit();
            serverContent_.mapLinks.push_back(copy);
            selection_ = Selection{SelectionKind::MapLink, serverContent_.mapLinks.size() - 1};
            break;
        }
        case SelectionKind::Object: {
            if (i >= serverContent_.objects.size() || (serverContent_.objects[i].kind == ObjectKind::ControlPoint && !controlPointsEditable())) {
                ctx_.setStatus("Control points cannot be duplicated (this world's control_point table has no marker / owner columns).", true);
                return;
            }
            PlacedObject copy = serverContent_.objects[i];
            copy.id = 0;
            copy.controlPoint.markerEntityId = 0;               // a marker belongs to one point
            copy.position = shifted(copy.position);
            beginEdit();
            serverContent_.objects.push_back(copy);
            selection_ = Selection{SelectionKind::Object, serverContent_.objects.size() - 1};
            break;
        }
        default:
            ctx_.setStatus("Regions and mission areas cannot be duplicated.", true);
            return;
    }
    contentChanged();
    ctx_.setStatus("Duplicated (not saved yet).", false);
}

void ServerWorkspace::insertPatrolStep(int64_t poolId, int32_t afterStep, const tr::Vec3& position) {
    int32_t last = 0;
    for (const PatrolStep& s : serverContent_.patrolSteps)
        if (s.poolId == poolId) last = std::max(last, s.step);
    PatrolStep step;
    step.poolId = poolId;
    step.position = position;
    if (afterStep < 0) {
        step.step = last + 1;
    } else {
        for (PatrolStep& s : serverContent_.patrolSteps)
            if (s.poolId == poolId && s.step > afterStep) ++s.step;
        step.step = afterStep + 1;
    }
    serverContent_.patrolSteps.push_back(step);
    sortPatrolSteps(poolId, step.step);
}

void ServerWorkspace::sortPatrolSteps(int64_t selectPool, int32_t selectStep) {
    auto& steps = serverContent_.patrolSteps;
    std::stable_sort(steps.begin(), steps.end(),
                     [](const PatrolStep& a, const PatrolStep& b) { return a.poolId != b.poolId ? a.poolId < b.poolId : a.step < b.step; });
    for (size_t k = 0; k < steps.size(); ++k)
        if (steps[k].poolId == selectPool && steps[k].step == selectStep) selection_ = Selection{SelectionKind::PatrolStep, k};
}

void ServerWorkspace::addServerObject(SelectionKind kind, ObjectKind objectKind) {
    if (!canEditServer() || !ctx_.scene) return;
    tr::Vec3 rayOrigin, rayDirection, point;
    ctx_.scene->viewRay(0.0f, 0.0f, rayOrigin, rayDirection);
    if (!ctx_.scene->groundPoint(rayOrigin, rayDirection, rayOrigin.y - 10.0f, point)) point = add(rayOrigin, scaled(rayDirection, 20.0f));
    if (kind == SelectionKind::WormholeLock) {
        // From the selected teleporter when it is one, else from any wormhole; to any; mission kind.
        WormholeLock w;
        if (selection_.kind == SelectionKind::Teleporter && selection_.index < serverContent_.teleporters.size())
            w.fromTeleporterId = serverContent_.teleporters[selection_.index].id;
        w.hasComment = true;
        w.comment = "New wormhole lock";
        beginEdit();
        serverContent_.wormholeLocks.push_back(w);
        selection_ = Selection{SelectionKind::WormholeLock, serverContent_.wormholeLocks.size() - 1};
        contentChanged();
        ctx_.setStatus("Added a wormhole lock (not saved yet).", false);
        return;
    }
    if (kind == SelectionKind::PatrolStep) {
        // After the selected step, or at the end of the selected (saved) pool's patrol.
        int64_t poolId = 0;
        int32_t after = -1;
        if (selection_.kind == SelectionKind::PatrolStep && selection_.index < serverContent_.patrolSteps.size()) {
            poolId = serverContent_.patrolSteps[selection_.index].poolId;
            after = serverContent_.patrolSteps[selection_.index].step;
        } else if (selection_.kind == SelectionKind::SpawnPool && selection_.index < serverContent_.spawnPools.size()) {
            poolId = serverContent_.spawnPools[selection_.index].id;
        }
        if (poolId <= 0) {
            ctx_.setStatus("Select a saved spawn pool or one of its patrol steps first: a patrol step needs its pool's database id.", true);
            return;
        }
        if (snapToGround_) point.y = ctx_.scene->groundHeight(point.x, point.z, point.y);
        beginEdit();
        insertPatrolStep(poolId, after, point);
        contentChanged();
        ctx_.setStatus("Added a patrol step at the centre of the view (not saved yet).", false);
        return;
    }
    if (kind == SelectionKind::Arrival) {
        // For the selected spawn pool, which must already be saved (the row needs its id).
        if (selection_.kind != SelectionKind::SpawnPool || selection_.index >= serverContent_.spawnPools.size() ||
            serverContent_.spawnPools[selection_.index].id <= 0) {
            ctx_.setStatus("Select a saved spawn pool first: an arrival point needs its pool's database id.", true);
            return;
        }
        const SpawnPool& pool = serverContent_.spawnPools[selection_.index];
        beginEdit();
        SpawnArrival a;
        a.poolId = pool.id;
        a.kind = 1;
        a.position = point;
        a.comment = "New arrival point";
        serverContent_.arrivals.push_back(a);
        selection_ = Selection{SelectionKind::Arrival, serverContent_.arrivals.size() - 1};
        contentChanged();
        ctx_.setStatus("Added an arrival point at the centre of the view (not saved yet).", false);
        return;
    }
    beginEdit();
    switch (kind) {
        case SelectionKind::SpawnPool: {
            SpawnPool p;
            if (selection_.kind == SelectionKind::SpawnPool && selection_.index < serverContent_.spawnPools.size())
                p = serverContent_.spawnPools[selection_.index];                    // start from the selected pool's settings
            else {
                p.respawnTime = 60;
                p.slot1 = SpawnSlot{database_->creatureList().empty() ? 0 : database_->creatureList().front().id, 1, 1};
            }
            p.id = 0;
            p.position = point;
            p.mapContextId = serverContent_.mapContextId;
            serverContent_.spawnPools.push_back(p);
            selection_ = Selection{SelectionKind::SpawnPool, serverContent_.spawnPools.size() - 1};
            break;
        }
        case SelectionKind::Teleporter: {
            Teleporter t;
            if (!serverContent_.teleporters.empty()) {
                t.classId = serverContent_.teleporters.front().classId;
                t.type = serverContent_.teleporters.front().type;
            }
            t.description = "New teleporter";
            t.position = point;
            t.mapContextId = serverContent_.mapContextId;
            serverContent_.teleporters.push_back(t);
            selection_ = Selection{SelectionKind::Teleporter, serverContent_.teleporters.size() - 1};
            break;
        }
        case SelectionKind::MapLink: {
            MapLink l;
            l.mapContextId = serverContent_.mapContextId;
            l.position = point;
            l.radius = 5.0f;
            l.destinationMapContextId = serverContent_.mapContextId;
            l.destinationPosition = point;
            l.comment = "New map link";
            serverContent_.mapLinks.push_back(l);
            selection_ = Selection{SelectionKind::MapLink, serverContent_.mapLinks.size() - 1};
            break;
        }
        default: {
            PlacedObject o;
            o.kind = objectKind;
            for (const PlacedObject& existing : serverContent_.objects)
                if (existing.kind == objectKind) { o.classId = existing.classId; break; }       // a sensible default class
            o.mapContextId = serverContent_.mapContextId;
            o.position = point;
            o.name = std::string("New ") + objectKindName(objectKind);
            if (objectKind == ObjectKind::ControlPoint) {
                // The Eloh control point unless the map's points use another class; the first client control point marker no point
                // has yet, else none.
                if (o.classId == 0) o.classId = 3814;
                for (const tr::MapScreenMarker& m : clientMarkers_) {
                    if (m.type != 1) continue;
                    bool used = false;
                    for (const PlacedObject& existing : serverContent_.objects)
                        if (existing.kind == ObjectKind::ControlPoint && existing.controlPoint.markerEntityId == m.entityId) used = true;
                    if (!used) {
                        o.controlPoint.markerEntityId = m.entityId;
                        if (!m.name.empty()) o.name = m.name;
                        break;
                    }
                }
            }
            if (objectKind == ObjectKind::Emitter) {
                o.classId = kEmitterClassId;
                // The package of the emitter last chosen on this map, else none yet (the inspector asks for one).
                for (const PlacedObject& existing : serverContent_.objects)
                    if (existing.kind == ObjectKind::Emitter) o.emitter.packageId = existing.emitter.packageId;
            }
            serverContent_.objects.push_back(o);
            selection_ = Selection{SelectionKind::Object, serverContent_.objects.size() - 1};
            break;
        }
    }
    contentChanged();
    ctx_.setStatus("Added at the centre of the view; set its fields in the Entity panel (not saved yet).", false);
}

// ------------------------------------------------------------------------------------------
// Map-screen markers (map_marker)
// ------------------------------------------------------------------------------------------
void ServerWorkspace::removeMarkerBindings(int32_t objectKind, int64_t objectId) {
    auto& b = serverContent_.mapMarkers;
    b.erase(std::remove_if(b.begin(), b.end(), [&](const MapMarkerBinding& m) { return m.objectKind == objectKind && m.objectId == objectId; }), b.end());
}

const tr::MapScreenMarker* ServerWorkspace::clientMarker(int64_t entityId) const {
    for (const tr::MapScreenMarker& m : clientMarkers_)
        if (m.entityId == entityId) return &m;
    return nullptr;
}

void ServerWorkspace::drawMarkerBinding(int32_t objectKind, int64_t objectId, const tr::Vec3& position, std::initializer_list<int32_t> types) {
    if (!database_ || !database_->hasTable("map_marker")) return;
    ImGui::SeparatorText("Map screen marker (map_marker)");
    if (objectId <= 0) {
        ImGui::TextDisabled("Save the object first.");
        return;
    }
    auto& bindings = serverContent_.mapMarkers;
    const auto describe = [&](int64_t entityId) {
        const tr::MapScreenMarker* m = clientMarker(entityId);
        if (!m) return "marker " + std::to_string(entityId) + " (not one of this map's client markers)";
        return (m->name.empty() ? std::string("(no name)") : m->name) + "  [" + tr::mapScreenMarkerTypeName(m->type) + "]";
    };
    size_t bound = 0;
    for (size_t k = 0; k < bindings.size(); ++k) {
        const MapMarkerBinding& b = bindings[k];
        if (b.objectKind != objectKind || b.objectId != objectId) continue;
        ++bound;
        ImGui::PushID(int(k));
        if (ImGui::SmallButton("x")) {
            beginEdit();
            bindings.erase(bindings.begin() + std::ptrdiff_t(k));
            contentChanged();
            ImGui::PopID();
            break;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unbind: the marker is then sent no state");
        ImGui::SameLine();
        ImGui::Text("%s, %.1f m", describe(b.markerEntityId).c_str(), b.matchDistance);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("client entity id %lld", static_cast<long long>(b.markerEntityId));
        ImGui::PopID();
    }
    if (bound == 0) ImGui::TextDisabled("No map-screen marker shows this object's state.");
    // Bind: this map's client markers of a type such an object shows, nearest first.
    std::vector<std::pair<float, const tr::MapScreenMarker*>> candidates;
    for (const tr::MapScreenMarker& m : clientMarkers_) {
        if (std::find(types.begin(), types.end(), m.type) == types.end() || m.nameId < 0) continue;
        const float dx = m.x - position.x, dy = m.y - position.y, dz = m.z - position.z;
        candidates.push_back({std::sqrt(dx * dx + dy * dy + dz * dz), &m});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    ImGui::SetNextItemWidth(320);
    if (ImGui::BeginCombo("##bindmarker", "bind a marker...", ImGuiComboFlags_HeightLarge)) {
        for (const auto& [distance, m] : candidates) {
            const MapMarkerBinding* existing = nullptr;
            for (const MapMarkerBinding& b : bindings)
                if (b.markerEntityId == m->entityId) existing = &b;
            if (existing && existing->objectKind == objectKind && existing->objectId == objectId) continue;
            char label[256];
            std::snprintf(label, sizeof label, "%s  [%s] - %.1f m%s", m->name.c_str(), tr::mapScreenMarkerTypeName(m->type), distance,
                          existing ? "  (bound elsewhere: moves here)" : "");
            ImGui::PushID(&m->entityId);
            if (ImGui::Selectable(label)) {
                beginEdit();
                bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [&](const MapMarkerBinding& b) { return b.markerEntityId == m->entityId; }),
                               bindings.end());
                MapMarkerBinding b;
                b.markerEntityId = m->entityId;
                b.markerType = m->type;
                b.objectKind = objectKind;
                b.objectId = objectId;
                b.matchDistance = std::round(double(distance) * 100.0) / 100.0;
                b.hasComment = true;
                b.comment = m->name.substr(0, 64);           // as the seeded rows: the client's marker name (varchar(64))
                bindings.push_back(b);
                contentChanged();
            }
            ImGui::PopID();
        }
        if (candidates.empty()) ImGui::TextDisabled("(no client marker of a matching type on this map)");
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The client draws its own markers (uimapmarker); binding one to this object makes the server send it this object's state.\n"
                          "The shipped bindings were matched by position (within 25 m).");
}

void ServerWorkspace::drawMarkerList() {
    if (!database_ || !database_->hasTable("map_marker")) return;
    char header[96];
    std::snprintf(header, sizeof header, "Map screen markers (client, %zu)###markers", clientMarkers_.size());
    if (!ImGui::TreeNodeEx(header, clientMarkers_.empty() ? ImGuiTreeNodeFlags_Leaf : 0)) return;
    const std::string needle = tr::toLowerAscii(serverFilter_);
    for (size_t k = 0; k < clientMarkers_.size(); ++k) {
        const tr::MapScreenMarker& m = clientMarkers_[k];
        std::string label = (m.name.empty() ? std::string("(no name: the client skips it)") : m.name) + "  [" + tr::mapScreenMarkerTypeName(m.type) + "]";
        Selection target{};
        std::string boundTo = "unbound";
        for (const MapMarkerBinding& b : serverContent_.mapMarkers) {
            if (b.markerEntityId != m.entityId) continue;
            boundTo = (b.objectKind == 1 ? "teleporter " : "kraftwerks ") + std::to_string(b.objectId);
            if (b.objectKind == 1) {
                for (size_t t = 0; t < serverContent_.teleporters.size(); ++t)
                    if (serverContent_.teleporters[t].id == b.objectId) target = Selection{SelectionKind::Teleporter, t};
            } else {
                for (size_t o = 0; o < serverContent_.objects.size(); ++o)
                    if (serverContent_.objects[o].kind == ObjectKind::Kraftwerk && serverContent_.objects[o].id == b.objectId)
                        target = Selection{SelectionKind::Object, o};
            }
            if (target.kind == SelectionKind::None) boundTo += " (not on this map)";
        }
        if (m.type == 1) boundTo = "control point: control_point.marker_entity_id";
        label += "  -> " + boundTo;
        if (!needle.empty() && tr::toLowerAscii(label).find(needle) == std::string::npos) continue;
        ImGui::PushID(int(k));
        if (ImGui::Selectable(label.c_str(), target.kind != SelectionKind::None && sameSelection(selection_, target), ImGuiSelectableFlags_AllowDoubleClick) &&
            target.kind != SelectionKind::None) {
            selection_ = target;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) viewport_.focusOn(ctx_, target);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("client entity id %lld at (%.1f, %.1f, %.1f)%s", static_cast<long long>(m.entityId), m.x, m.y, m.z,
                              target.kind == SelectionKind::None && m.type != 1 ? "\nBind it from a teleporter's or Kraftwerk's inspector." : "");
        ImGui::PopID();
    }
    ImGui::TreePop();
}

// ------------------------------------------------------------------------------------------
// Control point links (control_point_link)
// ------------------------------------------------------------------------------------------
std::string ServerWorkspace::linkedObjectLabel(int32_t kind, int64_t objectId) const {
    const std::string id = std::to_string(objectId);
    switch (kind) {
        case 1:
        case 2:
            for (size_t k = 0; k < serverContent_.spawnPools.size(); ++k)
                if (serverContent_.spawnPools[k].id == objectId) return serverObjectLabel(Selection{SelectionKind::SpawnPool, k});
            return "spawn pool " + id + " (not on this map)";
        case 3:
        case 4:
            for (size_t k = 0; k < serverContent_.teleporters.size(); ++k)
                if (serverContent_.teleporters[k].id == objectId) return serverObjectLabel(Selection{SelectionKind::Teleporter, k});
            return "teleporter " + id + " (not on this map)";
        case 5: {
            const Creature* c = database_ ? database_->creature(int32_t(objectId)) : nullptr;
            return "creature " + id + (c ? "  " + c->comment : std::string(" (not in the creature table)"));
        }
        case 6:
            for (size_t k = 0; k < serverContent_.objects.size(); ++k)
                if (serverContent_.objects[k].kind == ObjectKind::Footlocker && serverContent_.objects[k].id == objectId)
                    return serverObjectLabel(Selection{SelectionKind::Object, k});
            return "footlocker " + id + " (not on this map: the server ignores the link)";
        default: return id;
    }
}

void ServerWorkspace::removeControlPointLinks(std::initializer_list<int32_t> kinds, int64_t objectId) {
    auto& links = serverContent_.controlPointLinks;
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const ControlPointLink& l) {
                                   return l.objectId == objectId && std::find(kinds.begin(), kinds.end(), l.kind) != kinds.end();
                               }),
                links.end());
}

void ServerWorkspace::drawControlPointMembership(std::initializer_list<int32_t> kinds, int64_t objectId) {
    if (objectId <= 0) return;
    for (const ControlPointLink& l : serverContent_.controlPointLinks) {
        if (l.objectId != objectId || std::find(kinds.begin(), kinds.end(), l.kind) == kinds.end()) continue;
        std::string point = "control point " + std::to_string(l.controlPointId);
        for (size_t k = 0; k < serverContent_.objects.size(); ++k) {
            const PlacedObject& o = serverContent_.objects[k];
            if (o.kind != ObjectKind::ControlPoint || o.id != l.controlPointId) continue;
            point = o.name.empty() ? point : o.name;
            ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s of control point %s", controlPointLinkKindName(l.kind), point.c_str());
            ImGui::SameLine();
            ImGui::PushID(int(k));
            if (ImGui::SmallButton("select")) selection_ = Selection{SelectionKind::Object, k};
            ImGui::PopID();
        }
    }
}

bool ServerWorkspace::controlPointsEditable() const {
    return database_ && database_->hasColumn("control_point", "marker_entity_id") && database_->hasColumn("control_point", "default_owner");
}

void ServerWorkspace::drawControlPointFields(PlacedObject& point) {
    // Rasa.NET ControlPointEntry: the client's map marker the point's owner is sent under (MapMarkerManager: UpdateMapMarker with
    // the marker's entity id; 0 = none sent), and the side that holds it until it first changes hands.
    ImGui::SeparatorText("Control point");
    const ImVec4 warn(1.0f, 0.8f, 0.3f, 1.0f);
    const float fieldWidth = ImGui::GetFontSize() * 14;
    ControlPointSettings& c = point.controlPoint;
    const tr::MapScreenMarker* marker = c.markerEntityId ? clientMarker(c.markerEntityId) : nullptr;
    const auto markerLabel = [&](const tr::MapScreenMarker& m) {
        const float dx = m.x - point.position.x, dy = m.y - point.position.y, dz = m.z - point.position.z;
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s  (%lld, %.0f m)", m.name.empty() ? tr::mapScreenMarkerTypeName(m.type) : m.name.c_str(),
                      static_cast<long long>(m.entityId), std::sqrt(dx * dx + dy * dy + dz * dz));
        return std::string(buf);
    };
    const std::string current = c.markerEntityId == 0 ? std::string("(none)")
                                : marker              ? markerLabel(*marker)
                                                      : std::to_string(c.markerEntityId) + "  (not a marker of this map)";
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::BeginCombo("Map marker", current.c_str())) {
        if (ImGui::Selectable("(none)", c.markerEntityId == 0)) {
            noteEdit();
            c.markerEntityId = 0;
            contentChanged();
        }
        // The client's control point markers of this map (uimapmarker type 1, CONTROL_POINT).
        for (const tr::MapScreenMarker& m : clientMarkers_) {
            if (m.type != 1) continue;
            ImGui::PushID(static_cast<int>(m.entityId & 0x7FFFFFFF));
            if (ImGui::Selectable(markerLabel(m).c_str(), m.entityId == c.markerEntityId)) {
                noteEdit();
                c.markerEntityId = m.entityId;
                contentChanged();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The client's control point marker on the map screen (uimapmarker); the server sends the point's owner under it.");
    if (marker) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Move here")) {
            // The seeds put a point at its marker (Rasa.NET ControlPointSeed); the server sets it on the ground beneath.
            noteEdit();
            point.position = tr::Vec3{marker->x, marker->y, marker->z};
            contentChanged();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put the point at its marker's position (the server sets it on the ground beneath).");
    }
    if (c.markerEntityId != 0 && !marker) ImGui::TextColored(warn, "The client has no marker with this id on this map: no owner is shown on the map screen.");
    if (marker && marker->type != 1) ImGui::TextColored(warn, "This marker is a %s, not a control point.", tr::mapScreenMarkerTypeName(marker->type));
    for (const PlacedObject& other : serverContent_.objects)
        if (&other != &point && other.kind == ObjectKind::ControlPoint && c.markerEntityId != 0 && other.controlPoint.markerEntityId == c.markerEntityId)
            ImGui::TextColored(warn, "Control point %lld (%s) uses this marker too.", static_cast<long long>(other.id), other.name.c_str());
    int owner = c.defaultOwner == 1 ? 1 : 0;
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::Combo("Starts held by", &owner, "Bane\0AFS\0")) {
        noteEdit();
        c.defaultOwner = owner;
        contentChanged();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("default_owner: who holds the point until it first changes hands. The server keeps the current owner apart\n"
                          "(control_point_state), so a change applies to a point that has never changed hands.");
    if (c.defaultOwner != 0 && c.defaultOwner != 1)
        ImGui::TextColored(warn, "default_owner %lld: the server reads it as Bane.", static_cast<long long>(c.defaultOwner));
    if (point.id <= 0) ImGui::TextDisabled("New: saved with the table's largest id + 1.");
}

void ServerWorkspace::drawControlPointLinks(const PlacedObject& point) {
    ImGui::SeparatorText("Garrison and holdings (control_point_link)");
    if (point.id <= 0) {
        ImGui::TextDisabled("Save the control point first.");
        return;
    }
    auto& links = serverContent_.controlPointLinks;
    const ImVec4 warn(1.0f, 0.8f, 0.3f, 1.0f);
    for (int32_t kind = 1; kind <= 6; ++kind) {
        ImGui::PushID(kind);
        ImGui::TextUnformatted(controlPointLinkKindName(kind));
        static const char* help[] = {"", "spawn pools that stand here while the Bane hold the point", "spawn pools that stand here while the AFS hold it",
                                     "the teleporter of its hospital (AFS-held only)", "the teleporter of its waypoint (AFS-held only)",
                                     "creature rows that count as bosses of its garrison", "the footlocker of its clan lockbox (one a point; clan-held only)"};
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help[kind]);
        size_t count = 0;
        for (size_t k = 0; k < links.size(); ++k) {
            const ControlPointLink l = links[k];
            if (l.controlPointId != point.id || l.kind != kind) continue;
            ++count;
            ImGui::PushID(int(k));
            // Remove first, so it stays in view whatever the label's length.
            if (ImGui::SmallButton("x")) {
                beginEdit();
                links.erase(links.begin() + std::ptrdiff_t(k));
                contentChanged();
                ImGui::PopID();
                break;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this link");
            ImGui::SameLine();
            ImGui::TextUnformatted(linkedObjectLabel(kind, l.objectId).c_str());
            // Checks the server's loader makes or the shipped data follows.
            if (kind == 3 || kind == 4)
                for (const Teleporter& t : serverContent_.teleporters)
                    if (t.id == l.objectId && t.type != (kind == 3 ? 5 : 2)) {
                        ImGui::SameLine();
                        ImGui::TextColored(warn, "(type %d; shipped %s are type %d)", t.type, kind == 3 ? "hospitals" : "waypoints", kind == 3 ? 5 : 2);
                    }
            int owners = 0;
            for (const ControlPointLink& o : links)
                if (o.objectId == l.objectId && kind <= 4 && o.kind <= 4 && ((kind <= 2) == (o.kind <= 2))) ++owners;
            if (owners > 1) {
                ImGui::SameLine();
                ImGui::TextColored(warn, "(linked %d times: the server keeps the last)", owners);
            }
            ImGui::PopID();
        }
        // Add: saved objects of the right kind on this map (a link names a row id).
        const auto add = [&](int64_t objectId) {
            beginEdit();
            links.push_back(ControlPointLink{point.id, kind, objectId});
            contentChanged();
        };
        if (kind == 5) {
            int32_t creature = 0;
            ImGui::SetNextItemWidth(320);
            if (creatureCombo("##boss", creature, false) && creature > 0) add(creature);
        } else if (kind == 6 && count > 0) {
            ImGui::TextDisabled("  (one lockbox a point)");
        } else {
            ImGui::SetNextItemWidth(320);
            if (ImGui::BeginCombo("##add", "add...", ImGuiComboFlags_HeightLarge)) {
                const auto linked = [&](int64_t id) {
                    for (const ControlPointLink& l : links)
                        if (l.controlPointId == point.id && l.kind == kind && l.objectId == id) return true;
                    return false;
                };
                if (kind <= 2)
                    for (size_t k = 0; k < serverContent_.spawnPools.size(); ++k) {
                        const int64_t id = serverContent_.spawnPools[k].id;
                        if (id <= 0 || linked(id)) continue;
                        ImGui::PushID(int(k));
                        if (ImGui::Selectable(serverObjectLabel(Selection{SelectionKind::SpawnPool, k}).c_str())) add(id);
                        ImGui::PopID();
                    }
                if (kind == 3 || kind == 4)
                    for (size_t k = 0; k < serverContent_.teleporters.size(); ++k) {
                        const Teleporter& t = serverContent_.teleporters[k];
                        if (t.id <= 0 || linked(t.id)) continue;
                        ImGui::PushID(int(k));
                        if (ImGui::Selectable((serverObjectLabel(Selection{SelectionKind::Teleporter, k}) + "  (type " + std::to_string(t.type) + ")").c_str()))
                            add(t.id);
                        ImGui::PopID();
                    }
                if (kind == 6)
                    for (size_t k = 0; k < serverContent_.objects.size(); ++k) {
                        const PlacedObject& o = serverContent_.objects[k];
                        if (o.kind != ObjectKind::Footlocker || o.id <= 0) continue;
                        ImGui::PushID(int(k));
                        if (ImGui::Selectable(serverObjectLabel(Selection{SelectionKind::Object, k}).c_str())) add(o.id);
                        ImGui::PopID();
                    }
                ImGui::TextDisabled("(saved objects of this map only)");
                ImGui::EndCombo();
            }
        }
        ImGui::PopID();
    }
}

bool ServerWorkspace::fxPackageCombo(const char* id, int64_t& packageId) {
    if (!fxPackagesRead_ && ctx_.gameData) {
        fxPackagesRead_ = true;
        try { fxPackages_ = ctx_.gameData->stringsWithExtension(".pkg"); } catch (const std::exception&) {}
    }
    std::string current = "(none)";
    for (const auto& [pid, name] : fxPackages_)
        if (pid == packageId) current = name;
    if (packageId != 0 && current == "(none)") current = "#" + std::to_string(packageId) + " (not a package)";
    bool changed = false;
    if (ImGui::BeginCombo(id, current.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            fxPackageFilter_[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##filter", "filter (words)", fxPackageFilter_, sizeof fxPackageFilter_);
        // Every word must appear (as Rasa.NET's .emitter search does).
        std::vector<std::string> words;
        {
            std::string w;
            for (const char* c = fxPackageFilter_;; ++c) {
                if (*c == 0 || *c == ' ') {
                    if (!w.empty()) words.push_back(w);
                    w.clear();
                    if (*c == 0) break;
                } else w += char(std::tolower(static_cast<unsigned char>(*c)));
            }
        }
        size_t shown = 0;
        for (const auto& [pid, name] : fxPackages_) {
            std::string lower = name;
            for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            bool match = true;
            for (const std::string& w : words) match = match && lower.find(w) != std::string::npos;
            if (!match) continue;
            if (++shown > 500) {
                ImGui::TextDisabled("(more: filter)");
                break;
            }
            ImGui::PushID(pid);
            if (ImGui::Selectable(name.c_str(), pid == packageId)) {
                noteEdit();
                packageId = pid;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool ServerWorkspace::creatureCombo(const char* id, int32_t& creatureId, bool allowNone) {
    const Creature* current = database_->creature(creatureId);
    char preview[160];
    if (creatureId <= 0) std::snprintf(preview, sizeof preview, "(none)");
    else std::snprintf(preview, sizeof preview, "%d  %s", creatureId, current ? current->comment.c_str() : "(not in the creature table)");
    bool changed = false;
    if (ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            creatureFilter_[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##creaturefilter", "search by name, class or id", creatureFilter_, sizeof creatureFilter_);
        const std::string needle = tr::toLowerAscii(creatureFilter_);
        if (allowNone && ImGui::Selectable("(none)", creatureId <= 0)) {
            creatureId = 0;
            changed = true;
        }
        for (const Creature& c : database_->creatureList()) {
            const ClassInfo& info = ctx_.classInfo(c.classId);
            char label[256];
            std::snprintf(label, sizeof label, "%d  %s  (lvl %d, %s)%s", c.id, c.comment.c_str(), c.level, info.name.c_str(),
                          database_->classIsVendor(c.classId) ? " vendor" : database_->classIsNpc(c.classId) ? " NPC" : "");
            if (!needle.empty() && tr::toLowerAscii(label).find(needle) == std::string::npos) continue;
            if (ImGui::Selectable(label, c.id == creatureId)) {
                creatureId = c.id;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void ServerWorkspace::drawServerObjectEditor() {
    const size_t i = selection_.index;
    ImGui::TextWrapped("%s", serverObjectLabel(selection_).c_str());
    ImGui::TextDisabled("Drag in the viewport to move, Ctrl+wheel to rotate, Del to delete, Ctrl+D to duplicate.");
    ImGui::Separator();
    const float fieldWidth = ImGui::GetFontSize() * 14;
    // Each widget edits a copy; the first change of an interaction takes the undo snapshot.
    auto position = [&](const char* label, tr::Vec3& target) {
        float v[3] = {target.x, target.y, target.z};
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::DragFloat3(label, v, 0.1f, 0, 0, "%.2f")) {
            noteEdit();
            target = tr::Vec3{v[0], v[1], v[2]};
            contentChanged();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Snap to ground") && ctx_.scene) {
            beginEdit();
            target.y = ctx_.scene->groundHeight(target.x, target.z, target.y);
            contentChanged();
        }
    };
    auto angle = [&](const char* label, float& radians) {
        float degrees = radians * 57.29578f;
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::DragFloat(label, &degrees, 1.0f, -360.0f, 360.0f, "%.1f deg")) {
            noteEdit();
            radians = degrees / 57.29578f;
            contentChanged();
        }
    };
    auto number = [&](const char* label, float& value, float speed, float lo, float hi) {
        float v = value;
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::DragFloat(label, &v, speed, lo, hi, "%.2f")) {
            noteEdit();
            value = v;
            contentChanged();
        }
    };
    auto integer = [&](const char* label, int32_t& value) {
        int v = value;
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::InputInt(label, &v)) {
            noteEdit();
            value = v;
            contentChanged();
        }
    };
    auto text = [&](const char* label, std::string& value) {
        char buffer[256];
        std::snprintf(buffer, sizeof buffer, "%s", value.c_str());
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::InputText(label, buffer, sizeof buffer)) {
            noteEdit();
            value = buffer;
            contentChanged();
        }
    };
    auto flag = [&](const char* label, bool& value) {
        bool v = value;
        if (ImGui::Checkbox(label, &v)) {
            noteEdit();
            value = v;
            contentChanged();
        }
    };
    auto choice = [&](const char* label, int32_t& value, std::initializer_list<const char*> names) {
        int v = value;
        std::string items;
        for (const char* n : names) items += std::string(n) + '\0';
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::Combo(label, &v, items.c_str())) {
            noteEdit();
            value = v;
            contentChanged();
        }
    };
    auto classField = [&](const char* label, int32_t& classId) {
        integer(label, classId);
        ImGui::SameLine();
        const ClassInfo& info = ctx_.classInfo(classId);
        ImGui::TextDisabled("%s", info.known ? info.name.c_str() : "(unknown class)");
    };
    auto area = [&](AreaShape& a, bool heightRange) {
        position("Centre", a.position);
        int32_t shape = a.shape == 2 ? 1 : 0;
        choice("Shape", shape, {"circle", "box"});
        a.shape = shape == 1 ? 2 : 1;
        if (a.shape == 2) {
            number("Half width (x)", a.halfX, 0.1f, 0.0f, 5000.0f);
            number("Half depth (z)", a.halfZ, 0.1f, 0.0f, 5000.0f);
        } else {
            number("Radius", a.radius, 0.1f, 0.0f, 5000.0f);
        }
        if (heightRange) {
            number("Lowest y", a.minY, 1.0f, -5000.0f, 5000.0f);
            number("Highest y", a.maxY, 1.0f, -5000.0f, 5000.0f);
        }
    };

    switch (selection_.kind) {
        case SelectionKind::SpawnPool: {
            if (i >= serverContent_.spawnPools.size()) break;
            SpawnPool& p = serverContent_.spawnPools[i];
            ImGui::TextDisabled("id %lld", static_cast<long long>(p.id));
            drawControlPointMembership({1, 2}, p.id);
            position("Position", p.position);
            angle("Facing", p.rotation);
            number("Radius (m)", p.radius, 0.1f, 0.0f, 500.0f);
            integer("Respawn (s)", p.respawnTime);
            choice("Mode", p.mode, {"automatic", "control point", "scripted"});
            choice("Arrival", p.animType, {"none", "hostile dropship", "friendly dropship"});
            ImGui::SeparatorText("Creatures (min / max count)");
            for (int k = 0; k < 6; ++k) {
                SpawnSlot& slot = spawnSlot(p, k);
                ImGui::PushID(k);
                int32_t creature = slot.creatureId;
                ImGui::SetNextItemWidth(fieldWidth * 1.4f);
                char label[16];
                std::snprintf(label, sizeof label, "##slot%d", k + 1);
                if (creatureCombo(label, creature, k > 0)) {
                    noteEdit();
                    slot.creatureId = creature;
                    contentChanged();
                }
                ImGui::SameLine();
                int counts[2] = {slot.minCount, slot.maxCount};
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
                if (ImGui::InputInt2("##counts", counts)) {
                    noteEdit();
                    slot.minCount = counts[0];
                    slot.maxCount = counts[1];
                    contentChanged();
                }
                ImGui::PopID();
            }
            ImGui::SeparatorText("Arrival points");
            int arrivals = 0;
            for (size_t k = 0; k < serverContent_.arrivals.size(); ++k) {
                if (serverContent_.arrivals[k].poolId != p.id || p.id <= 0) continue;
                ++arrivals;
                ImGui::PushID(int(1000 + k));
                const std::string label = serverObjectLabel(Selection{SelectionKind::Arrival, k});
                if (ImGui::Selectable(label.c_str(), false)) selection_ = Selection{SelectionKind::Arrival, k};
                ImGui::PopID();
            }
            if (!arrivals) ImGui::TextDisabled("none: the creatures appear in the pool (or by its dropship setting)");
            if (p.id <= 0) ImGui::TextDisabled("Save the pool before adding arrival points.");
            else if (ImGui::SmallButton("Add arrival point")) addServerObject(SelectionKind::Arrival, ObjectKind::Footlocker);
            // Patrol and pose (Rasa.NET upstream spawnpool_patrol / spawnpool_pose; absent on older worlds).
            const bool patrolTable = database_ && database_->hasTable("spawnpool_patrol");
            const bool poseTable = database_ && database_->hasTable("spawnpool_pose");
            int steps = 0;
            if (patrolTable) {
                ImGui::SeparatorText("Patrol");
                for (size_t k = 0; k < serverContent_.patrolSteps.size(); ++k) {
                    if (serverContent_.patrolSteps[k].poolId != p.id || p.id <= 0) continue;
                    ++steps;
                    ImGui::PushID(int(2000 + k));
                    const std::string label = serverObjectLabel(Selection{SelectionKind::PatrolStep, k});
                    if (ImGui::Selectable(label.c_str(), false)) selection_ = Selection{SelectionKind::PatrolStep, k};
                    ImGui::PopID();
                }
                if (!steps) ImGui::TextDisabled("none: the creatures keep to the pool");
                else ImGui::TextDisabled("Walked in step order, then back to the first; one creature to a beat, and it needs a walk speed.");
                if (p.id <= 0) ImGui::TextDisabled("Save the pool before adding patrol steps.");
                else if (ImGui::SmallButton("Add patrol step")) addServerObject(SelectionKind::PatrolStep, ObjectKind::Footlocker);
            }
            if (poseTable) {
                ImGui::SeparatorText("Pose");
                PoolPose* pose = nullptr;
                for (PoolPose& candidate : serverContent_.poses)
                    if (candidate.poolId == p.id) pose = &candidate;
                if (p.id <= 0) {
                    ImGui::TextDisabled("Save the pool before giving it a pose.");
                } else {
                    // Index 0 = no row; 1..10 = pose 0..9 (0 "none" is a valid stored value, kept if present).
                    int current = pose ? pose->pose + 1 : 0;
                    std::string items = "(no row)";
                    items += '\0';
                    for (int v = 0; v <= 9; ++v) items += std::to_string(v) + " " + npcPoseName(v) + '\0';
                    ImGui::SetNextItemWidth(fieldWidth);
                    if (ImGui::Combo("Pose", &current, items.c_str())) {
                        noteEdit();
                        if (current == 0) {
                            serverContent_.poses.erase(std::remove_if(serverContent_.poses.begin(), serverContent_.poses.end(),
                                                                      [&](const PoolPose& c) { return c.poolId == p.id; }),
                                                       serverContent_.poses.end());
                        } else if (pose) {
                            pose->pose = current - 1;
                        } else {
                            serverContent_.poses.push_back(PoolPose{p.id, current - 1});
                        }
                        contentChanged();
                    }
                    ImGui::TextDisabled("5-9 are the client's ambient animations, humans only (8 and 9: female skeleton only).");
                    if (steps && current > 0) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "A pool with a patrol has no pose (Rasa.NET NpcPoses).");
                }
            }
            break;
        }
        case SelectionKind::PatrolStep: {
            if (i >= serverContent_.patrolSteps.size()) break;
            PatrolStep& s = serverContent_.patrolSteps[i];
            ImGui::TextDisabled("spawn pool %lld, step %d", static_cast<long long>(s.poolId), s.step);
            position("Position", s.position);
            ImGui::TextDisabled("A floor height: the creature walks straight between steps, heights and all (not on the navmesh).");
            flag("Turns to a facing", s.hasFacing);
            if (s.hasFacing) angle("Facing", s.facing);
            integer("Pause (ms)", s.pauseMs);
            if (s.pauseMs < 0) s.pauseMs = 0;
            // Order: swap step numbers with the neighbour in the same pool.
            const int64_t pool = s.poolId;
            const int32_t number = s.step;
            const bool hasEarlier = i > 0 && serverContent_.patrolSteps[i - 1].poolId == pool;
            const bool hasLater = i + 1 < serverContent_.patrolSteps.size() && serverContent_.patrolSteps[i + 1].poolId == pool;
            ImGui::BeginDisabled(!hasEarlier);
            const bool earlier = ImGui::SmallButton("Earlier");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!hasLater);
            const bool later = ImGui::SmallButton("Later");
            ImGui::EndDisabled();
            if (earlier || later) {
                beginEdit();
                PatrolStep& other = serverContent_.patrolSteps[earlier ? i - 1 : i + 1];
                const int32_t otherNumber = other.step;
                other.step = number;
                serverContent_.patrolSteps[i].step = otherNumber;
                sortPatrolSteps(pool, otherNumber);
                contentChanged();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Add step after")) addServerObject(SelectionKind::PatrolStep, ObjectKind::Footlocker);
            ImGui::SameLine();
            if (ImGui::SmallButton("Spawn pool")) {
                for (size_t k = 0; k < serverContent_.spawnPools.size(); ++k)
                    if (serverContent_.spawnPools[k].id == pool) selection_ = Selection{SelectionKind::SpawnPool, k};
            }
            break;
        }
        case SelectionKind::Arrival: {
            if (i >= serverContent_.arrivals.size()) break;
            SpawnArrival& a = serverContent_.arrivals[i];
            ImGui::TextDisabled("id %lld", static_cast<long long>(a.id));
            position("Position", a.position);
            angle("Facing", a.rotation);
            int32_t kindIndex = a.kind == 2 ? 1 : 0;
            choice("Kind", kindIndex, {"dropship pad (1)", "Bane teleporter (2)"});
            a.kind = kindIndex == 1 ? 2 : 1;
            // The pool: any saved pool on this map.
            char poolPreview[64];
            std::snprintf(poolPreview, sizeof poolPreview, "spawn %lld", static_cast<long long>(a.poolId));
            ImGui::SetNextItemWidth(fieldWidth);
            if (ImGui::BeginCombo("Spawn pool", poolPreview, ImGuiComboFlags_HeightLarge)) {
                for (size_t k = 0; k < serverContent_.spawnPools.size(); ++k) {
                    const SpawnPool& pool = serverContent_.spawnPools[k];
                    if (pool.id <= 0) continue;
                    const std::string label = serverObjectLabel(Selection{SelectionKind::SpawnPool, k});
                    if (ImGui::Selectable(label.c_str(), pool.id == a.poolId)) {
                        noteEdit();
                        a.poolId = pool.id;
                        contentChanged();
                    }
                }
                ImGui::EndCombo();
            }
            if (a.kind == 2) {
                long long entity = a.entityId;
                ImGui::SetNextItemWidth(fieldWidth);
                if (ImGui::InputScalar("Teleporter entity id", ImGuiDataType_S64, &entity)) {
                    noteEdit();
                    a.entityId = entity;
                    contentChanged();
                }
                ImGui::TextDisabled("The map's own entity id of the Bane teleporter switched on for the arrival.");
            }
            text("Comment", a.comment);
            break;
        }
        case SelectionKind::Teleporter: {
            if (i >= serverContent_.teleporters.size()) break;
            Teleporter& t = serverContent_.teleporters[i];
            ImGui::TextDisabled("id %lld", static_cast<long long>(t.id));
            drawControlPointMembership({3, 4}, t.id);
            position("Position", t.position);
            angle("Facing", t.rotation);
            classField("Class", t.classId);
            integer("Type", t.type);
            text("Description", t.description);
            drawMarkerBinding(1, t.id, t.position, {2, 3, 19});
            break;
        }
        case SelectionKind::Object: {
            if (i >= serverContent_.objects.size()) break;
            PlacedObject& o = serverContent_.objects[i];
            ImGui::TextDisabled("%s, id %lld", objectKindName(o.kind), static_cast<long long>(o.id));
            if (o.kind == ObjectKind::Footlocker) drawControlPointMembership({6}, o.id);
            if (o.kind == ObjectKind::Kraftwerk) drawMarkerBinding(2, o.id, o.position, {4});
            position("Position", o.position);
            if (o.kind != ObjectKind::Logos) angle("Facing", o.rotation);
            if (o.kind == ObjectKind::Emitter) ImGui::TextDisabled("Class %d FXPackageEmitter (fixed: an invisible proxy that plays the package)", kEmitterClassId);
            else classField("Class", o.classId);
            text(o.kind == ObjectKind::Logos || o.kind == ObjectKind::ControlPoint ? "Name" : "Comment", o.name);
            // Number columns of the Rasa.NET upstream tables (0 = the kind's / class's default).
            auto whole = [&](const char* label, int64_t& value) {
                long long v = value;
                ImGui::SetNextItemWidth(fieldWidth);
                if (ImGui::InputScalar(label, ImGuiDataType_S64, &v)) {
                    noteEdit();
                    value = std::max<long long>(v, 0);
                    contentChanged();
                }
            };
            if (o.kind == ObjectKind::AmbientNpc)
                ImGui::TextDisabled("A class with the STATELESSSWITCH augmentation (UsableStatelessNPC*): its own looping animations, nothing to use.");
            if (o.kind == ObjectKind::ControlPoint && controlPointsEditable()) drawControlPointFields(o);
            if (o.kind == ObjectKind::ControlPoint && database_ && database_->hasTable("control_point_link")) drawControlPointLinks(o);
            if (o.kind == ObjectKind::Emitter) {
                // Rasa.NET EmitterManager: the client attaches the package while the emitter is on (fxpackageemitter.py TurnOn:
                // body.AttachFXPackage); a package the client's string table lacks is left out at start-up.
                ImGui::SeparatorText("FX emitter");
                if (fxPackageCombo("Package", o.emitter.packageId)) contentChanged();
                bool on = o.emitter.isOn;
                if (ImGui::Checkbox("Plays from map load", &on)) {
                    noteEdit();
                    o.emitter.isOn = on;
                    contentChanged();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Off: placed but silent until something turns it on (.emitter commands, scripts).");
                if (o.name.size() > 96) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "The comment is longer than its column (varchar 96).");
                // What the package holds (special-FX events the editor draws: accessories, particle systems, lights).
                std::string package;
                if (ctx_.gameData) {
                    try { package = ctx_.gameData->stringById(int32_t(o.emitter.packageId)); } catch (const std::exception&) {}
                }
                if (o.emitter.packageId == 0) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Choose a package (the server leaves out an emitter without one).");
                else if (package.empty() || package.size() < 4 || package.compare(package.size() - 4, 4, ".pkg") != 0)
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "String %lld is not an FX package: the server leaves this emitter out.",
                                       static_cast<long long>(o.emitter.packageId));
                else if (ctx_.assets) {
                    try {
                        const tr::SfxPackage p = tr::readSfxPackage(ctx_.assets->read(package));
                        ImGui::TextDisabled("%s: %zu accessories, %zu particle systems, %zu lights", package.c_str(), p.accessories.size(), p.particles.size(),
                                            p.lights.size());
                    } catch (const std::exception& e) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s: %s", package.c_str(), e.what());
                    }
                }
            }
            if (o.kind == ObjectKind::WorldUsable) {
                ImGui::SeparatorText("Usable (0 = the kind's default)");
                UsableSettings& u = o.usable;
                whole("Start state (UseObjectState)", u.state);
                bool usable = u.usable != 0;
                if (ImGui::Checkbox("Usable by players", &usable)) {
                    noteEdit();
                    u.usable = usable ? 1 : 0;
                    contentChanged();
                }
                whole("Dispensed item (itemtemplate.id)", u.itemTemplateId);
                whole("Accepted / bomb item class", u.itemClassId);
                whole("Item count", u.itemCount);
                whole("Reset (s)", u.resetSeconds);
                whole("Interval min (s) / bomb fuse", u.intervalMinSeconds);
                whole("Interval max (s)", u.intervalMaxSeconds);
                whole("Damage min", u.damageMin);
                whole("Damage max", u.damageMax);
                number("Blast radius (m)", u.radius, 0.1f, 0.0f, 500.0f);
                int arg = int(u.actionArg);
                ImGui::SetNextItemWidth(fieldWidth);
                if (ImGui::Combo("Blast (OBJ_DAMAGE arg)", &arg, "0 default\0" "1 small explosion\0" "2 large explosion\0" "3 lightning\0" "4 mine\0"
                                                                   "5 bomb\0" "6 geyser\0" "7 hornito\0")) {
                    noteEdit();
                    u.actionArg = arg;
                    contentChanged();
                }
            }
            if (o.kind == ObjectKind::WorldDestructible) {
                ImGui::SeparatorText("Destructible (0 = the class's default)");
                DestructibleSettings& d = o.destructible;
                whole("Hit points", d.hitPoints);
                whole("Spawned creature (creature.id)", d.creatureId);
                whole("Creatures at a time", d.creatureCount);
                int side = int(d.side);
                ImGui::SetNextItemWidth(fieldWidth);
                if (ImGui::Combo("Force field side", &side, "0 class default (Bane's)\0" "1 AFS: lets players through\0" "2 Bane: stops players\0")) {
                    noteEdit();
                    d.side = side;
                    contentChanged();
                }
            }
            break;
        }
        case SelectionKind::WormholeLock: {
            if (i >= serverContent_.wormholeLocks.size()) break;
            WormholeLock& w = serverContent_.wormholeLocks[i];
            ImGui::TextDisabled("id %lld - every lock that matches a trip must be met (Rasa.NET Wormholes)", static_cast<long long>(w.id));
            auto teleporter = [&](const char* label, int64_t& id) {
                long long v = id;
                ImGui::SetNextItemWidth(fieldWidth);
                if (ImGui::InputScalar(label, ImGuiDataType_S64, &v)) {
                    noteEdit();
                    id = std::max<long long>(v, 0);
                    contentChanged();
                }
                ImGui::SameLine();
                std::string where = id == 0 ? "any wormhole" : "a teleporter on another map";
                for (const Teleporter& t : serverContent_.teleporters)
                    if (t.id == id) where = t.description + (t.type == 3 ? "" : " (not a wormhole: type " + std::to_string(t.type) + ")");
                ImGui::TextDisabled("%s", where.c_str());
            };
            teleporter("From teleporter id", w.fromTeleporterId);
            teleporter("To teleporter id", w.toTeleporterId);
            int kindIndex = w.kind == 2 ? 1 : 0;
            ImGui::SetNextItemWidth(fieldWidth);
            if (ImGui::Combo("Kind", &kindIndex, "1 mission accepted\0" "2 required level (not enforced yet)\0")) {
                noteEdit();
                w.kind = kindIndex == 1 ? 2 : 1;
                contentChanged();
            }
            {
                long long v = w.value;
                ImGui::SetNextItemWidth(fieldWidth);
                if (ImGui::InputScalar(w.kind == 2 ? "Level" : "Mission id", ImGuiDataType_S64, &v)) {
                    noteEdit();
                    w.value = std::max<long long>(v, 0);
                    contentChanged();
                }
            }
            flag("Has a comment", w.hasComment);
            if (w.hasComment) text("Comment", w.comment);
            break;
        }
        case SelectionKind::MapLink: {
            if (i >= serverContent_.mapLinks.size()) break;
            MapLink& l = serverContent_.mapLinks[i];
            ImGui::TextDisabled("id %lld", static_cast<long long>(l.id));
            position("Position", l.position);
            number("Radius (m)", l.radius, 0.1f, 0.0f, 500.0f);
            flag("Enabled", l.enabled);
            integer("Kind", l.kind);
            ImGui::SeparatorText("Destination");
            std::string destination = std::to_string(l.destinationMapContextId);
            for (const MapInfo& m : database_->maps()) if (m.mapContextId == l.destinationMapContextId) destination = m.name;
            ImGui::SetNextItemWidth(fieldWidth);
            if (ImGui::BeginCombo("Map", destination.c_str(), ImGuiComboFlags_HeightLarge)) {
                for (const MapInfo& m : database_->maps()) {
                    if (ImGui::Selectable(m.name.c_str(), m.mapContextId == l.destinationMapContextId)) {
                        noteEdit();
                        l.destinationMapContextId = m.mapContextId;
                        contentChanged();
                    }
                }
                ImGui::EndCombo();
            }
            {
                float v[3] = {l.destinationPosition.x, l.destinationPosition.y, l.destinationPosition.z};
                ImGui::SetNextItemWidth(fieldWidth);
                if (ImGui::DragFloat3("Arrival position", v, 0.1f, 0, 0, "%.2f")) {
                    noteEdit();
                    l.destinationPosition = tr::Vec3{v[0], v[1], v[2]};
                    contentChanged();
                }
            }
            angle("Arrival facing", l.destinationRotation);
            text("Comment", l.comment);
            break;
        }
        case SelectionKind::Region: {
            if (i >= serverContent_.regions.size()) break;
            MapRegion& r = serverContent_.regions[i];
            ImGui::TextDisabled("id %lld", static_cast<long long>(r.id));
            integer("Region id", r.regionId);
            area(r.area, true);
            choice("Where", r.underground, {"anywhere", "underground only", "surface only"});
            ImGui::SetItemTooltip("map_region.underground: Rasa.NET counts the volume only for a player underground (on navmesh more than 2 m\n"
                                  "below the terrain - a cave or tunnel) or only on the surface. The editor tests the camera against the terrain.");
            flag("Enabled", r.enabled);
            text("Comment", r.comment);
            break;
        }
        case SelectionKind::MissionArea: {
            if (i >= serverContent_.missionAreas.size()) break;
            MissionArea& a = serverContent_.missionAreas[i];
            ImGui::TextDisabled("mission %d, revision %s, area %d", a.missionId, a.contentRevision.c_str(), a.areaId);
            area(a.area, false);
            text("Comment", a.comment);
            break;
        }
        default: break;
    }
    ImGui::Spacing();
    if (ImGui::Button("Show in overview")) overview_.requestFocus();
    ImGui::SameLine();
    if (ImGui::Button("Show in viewport")) viewport_.focusOn(ctx_, selection_);
}

// ------------------------------------------------------------------------------------------
// Missions
// ------------------------------------------------------------------------------------------
void ServerWorkspace::drawMissionPanel() {
    if (focusMissions_ > 0) {
        ImGui::SetNextWindowFocus();
        --focusMissions_;
    }
    if (!ImGui::Begin("Missions###Server.Missions")) { ImGui::End(); return; }
    if (!database_) ImGui::TextDisabled("Open a server database first.");
    else if (!missions_.loaded()) ImGui::TextDisabled("This database has no mission tables.");
    else {
        std::vector<int32_t> creatures;
        for (const SpawnPool& p : serverContent_.spawnPools)
            for (int k = 0; k < 6; ++k)
                if (spawnSlot(p, k).creatureId > 0) creatures.push_back(spawnSlot(p, k).creatureId);
        missions_.draw(ctx_, *database_, serverContent_.mapContextId, creatures, !database_->readOnly());
    }
    ImGui::End();
}

// ------------------------------------------------------------------------------------------
// Loot
// ------------------------------------------------------------------------------------------
// Crafting: item modules (module_class / module_effect / module_item), the classes that take them (modifiable_class) and the
// fabrication recipes (recipe / recipe_input). Database-wide, each with its own Save / Revert.
void ServerWorkspace::drawCraftingPanel() {
    // Docked with the Loot panel on its first appearance (layouts saved before this panel existed).
    if (const ImGuiWindow* loot = ImGui::FindWindowByName("Loot###Server.Loot"); loot && loot->DockId)
        ImGui::SetNextWindowDockID(loot->DockId, ImGuiCond_FirstUseEver);
    const bool focus = focusModules_ > 0;
    if (focus) {
        ImGui::SetNextWindowFocus();
        --focusModules_;
    }
    if (!ImGui::Begin("Crafting###Server.Crafting")) { ImGui::End(); return; }
    if (!database_) ImGui::TextDisabled("Open a server database first.");
    else if (ImGui::BeginTabBar("crafting")) {
        static const char* tabs[] = {"Modules", "Modifiable classes", "Recipes"};
        for (int k = 0; k < 3; ++k) {
            if (!ImGui::BeginTabItem(tabs[k], nullptr, focus && k == craftingTab_ ? ImGuiTabItemFlags_SetSelected : 0)) continue;
            if (k == 2) recipes_.draw(ctx_, *database_, !database_->readOnly());
            else modules_.draw(ctx_, *database_, !database_->readOnly(), k);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void ServerWorkspace::drawLootPanel() {
    if (!ImGui::Begin("Loot###Server.Loot")) { ImGui::End(); return; }
    if (!database_) ImGui::TextDisabled("Open a server database first.");
    else {
        std::set<int32_t> creatures;
        for (const SpawnPool& p : serverContent_.spawnPools)
            for (int k = 0; k < 6; ++k)
                if (spawnSlot(p, k).creatureId > 0 && spawnSlot(p, k).maxCount > 0) creatures.insert(spawnSlot(p, k).creatureId);
        loot_.draw(ctx_, *database_, serverContent_.mapContextId, std::vector<int32_t>(creatures.begin(), creatures.end()), !database_->readOnly(),
                   [this](const char* id, int32_t& creature) { return creatureCombo(id, creature, false); });
    }
    ImGui::End();
}

// ------------------------------------------------------------------------------------------
// Creatures
// ------------------------------------------------------------------------------------------
void ServerWorkspace::reloadCreatureDefinitions() {
    creatureDefs_ = database_ ? database_->creatureDefinitions() : std::vector<CreatureDefinition>{};
    creatureDefsSaved_ = creatureDefs_;
    classCries_ = database_ ? database_->classBattlecries() : std::map<int32_t, int32_t>{};
    classCriesSaved_ = classCries_;
    classFlags_ = database_ ? database_->classFlags() : std::map<int32_t, std::set<int32_t>>{};
    classFlagsSaved_ = classFlags_;
}

// creature_class_flag: the selected creature's class flags (shared by every creature of the class).
void ServerWorkspace::drawClassFlags(const CreatureDefinition& d) {
    ImGui::SeparatorText("Class flags (creature_class_flag)");
    if (creatureFlagNames_.empty() && ctx_.gameData) {
        try { creatureFlagNames_ = ctx_.gameData->moduleConstants("generated.client.constant.creatureflag"); } catch (const std::exception&) {}
    }
    const auto nameOf = [&](int32_t flag) {
        for (const auto& [value, name] : creatureFlagNames_)
            if (value == flag) return name;
        return std::string("flag ") + std::to_string(flag) + " (not in the client's creatureflag)";
    };
    const int32_t classId = d.creature.classId;
    int sharing = 0;
    for (const CreatureDefinition& other : creatureDefs_) sharing += other.creature.classId == classId ? 1 : 0;
    ImGui::TextDisabled("Class %d %s - shared by %d creature(s); sent to the client in CreatureInfo.", classId, ctx_.classInfo(classId).name.c_str(), sharing);
    std::set<int32_t>& flags = classFlags_[classId];
    int32_t removed = -1;
    for (int32_t flag : flags) {
        ImGui::PushID(flag);
        if (ImGui::SmallButton("x")) removed = flag;
        ImGui::SameLine();
        ImGui::Text("%d %s", flag, nameOf(flag).c_str());
        ImGui::PopID();
    }
    if (removed >= 0) flags.erase(removed);
    if (flags.empty()) ImGui::TextDisabled("No flags.");
    // What the client reads (Rasa.NET Data/CreatureFlag.cs: harvest.py, healdisc.py).
    if (flags.count(5) && flags.count(11)) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "BIOLOGICAL and MECHANICAL: neither Salvage nor Tissue Extraction works on it.");
    ImGui::SetNextItemWidth(320);
    if (ImGui::BeginCombo("##addflag", "add a flag...", ImGuiComboFlags_HeightLarge)) {
        for (const auto& [value, name] : creatureFlagNames_) {
            if (flags.count(value)) continue;
            const std::string label = std::to_string(value) + " " + name;
            if (ImGui::Selectable(label.c_str())) flags.insert(value);
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("BIOLOGICAL refuses Salvage, MECHANICAL refuses Tissue Extraction (harvest.py); healing tools need BIOLOGICAL or\n"
                          "MACHINA (healdisc.py); SPECIES_* decide the harvest yields. The other flags are the client's own, not read by the server yet.");
    if (flags.empty()) classFlags_.erase(classId);
}

// Battle cries (creature_battlecry; Rasa.NET Battlecries): the creature's own package (scope 2) comes before its class's (scope
// 1, shared by every creature of the class). Packages and their cries are the client's (battlecrypackages -> audio sets).
void ServerWorkspace::drawBattlecry(CreatureDefinition& d) {
    ImGui::SeparatorText("Battle cry (creature_battlecry)");
    std::vector<int32_t> packages;
    if (ctx_.gameData) {
        try { packages = ctx_.gameData->battlecryPackages(); } catch (const std::exception&) {}
    }
    // A package's name: the words its audio sets' names share (e.g. "Battlecry English Male Rugd"), else the first set's name.
    auto packageName = [&](int32_t package) -> std::string {
        if (!ctx_.gameData) return "";
        std::vector<tr::GameData::BattlecryCry> cries;
        try { cries = ctx_.gameData->battlecryCries(package); } catch (const std::exception&) {}
        if (cries.empty()) return "(not in the client's battlecrypackages)";
        std::string common = cries[0].audioSetName;
        for (const auto& cry : cries) {
            size_t n = 0;
            while (n < common.size() && n < cry.audioSetName.size() && common[n] == cry.audioSetName[n]) ++n;
            common.resize(n);
        }
        while (!common.empty() && common.back() != ' ' && common.size() != cries[0].audioSetName.size()) common.pop_back();   // whole words
        while (!common.empty() && common.back() == ' ') common.pop_back();
        return common.empty() || cries.size() == 1 ? cries[0].audioSetName : common;
    };
    auto packageCombo = [&](const char* label, int32_t& package) {
        const std::string preview = std::to_string(package) + "  " + packageName(package);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        if (ImGui::BeginCombo(label, preview.c_str())) {
            for (int32_t p : packages) {
                const std::string text = std::to_string(p) + "  " + packageName(p);
                if (ImGui::Selectable(text.c_str(), p == package)) package = p;
            }
            ImGui::EndCombo();
        }
    };
    ImGui::Checkbox("Own package (scope 2, this creature)", &d.hasBattlecry);
    if (d.hasBattlecry) {
        if (d.battlecryPackage == 0 && !packages.empty()) d.battlecryPackage = packages.front();
        packageCombo("##ownpackage", d.battlecryPackage);
    }
    const int32_t classId = d.creature.classId;
    int sharing = 0;
    for (const CreatureDefinition& other : creatureDefs_) sharing += other.creature.classId == classId ? 1 : 0;
    const auto classRow = classCries_.find(classId);
    bool hasClass = classRow != classCries_.end();
    char classLabel[160];
    std::snprintf(classLabel, sizeof classLabel, "Class package (scope 1, class %d %s - %d creature(s))", classId,
                  ctx_.classInfo(classId).name.c_str(), sharing);
    if (ImGui::Checkbox(classLabel, &hasClass)) {
        if (hasClass) classCries_[classId] = packages.empty() ? 1 : packages.front();
        else classCries_.erase(classId);
    }
    ImGui::SetItemTooltip("Shared by every creature of this class that has no package of its own.");
    if (hasClass) packageCombo("##classpackage", classCries_[classId]);
    const auto now = classCries_.find(classId);
    const int32_t effective = d.hasBattlecry ? d.battlecryPackage : now != classCries_.end() ? now->second : 0;
    if (effective == 0) {
        ImGui::TextWrapped("No package: silent. (Rasa.NET gives an armed human or a mission escort with no package one of its sex's three "
                           "English human voices at random - Battlecries.HumanVoice; an escort's is then kept as its own row.)");
        return;
    }
    ImGui::Text("Cries with package %d (%s):", effective, d.hasBattlecry ? "its own" : "its class's");
    static const char* typeNames[] = {"", "Aggro", "Killed target", "Received damage", "Received critical damage", "Help", "Close to death",
                                      "Enter combat", "Exit combat", "Start patrol", "Stop patrol", "Resume patrol"};
    std::vector<tr::GameData::BattlecryCry> cries;
    if (ctx_.gameData) {
        try { cries = ctx_.gameData->battlecryCries(effective); } catch (const std::exception&) {}
    }
    // Each cry with its audio set's sounds; Play previews one (the game picks among them in the engine, seeded by the server).
    static const char* selectionNames[] = {"", "random", "sequential", "simultaneous"};
    for (const auto& cry : cries) {
        ImGui::PushID(cry.type);
        std::vector<tr::GameData::AudioSetSound> sounds;
        int32_t selection = 0;
        try {
            sounds = ctx_.gameData->audioSetSounds(cry.audioSet);
            selection = ctx_.gameData->audioSetSelectionType(cry.audioSet);
        } catch (const std::exception&) {}
        const std::string header = std::string(cry.type >= 1 && cry.type <= 11 ? typeNames[cry.type] : "?") + ": " + cry.audioSetName + "  (" +
                                   std::to_string(sounds.size()) + " sound" + (sounds.size() == 1 ? "" : "s") +
                                   (selection >= 1 && selection <= 3 ? std::string(", ") + selectionNames[selection] : "") + ")";
        if (ImGui::TreeNode(header.c_str())) {
            for (size_t k = 0; k < sounds.size(); ++k) {
                const tr::GameData::AudioSetSound& s = sounds[k];
                ImGui::PushID(int(k));
                const bool exists = ctx_.assets && ctx_.assets->exists(s.file);
                if (!exists) ImGui::BeginDisabled();
                if (cryPlayer_.playing(s.file)) {
                    if (ImGui::SmallButton("Stop")) cryPlayer_.stop();
                } else if (ImGui::SmallButton("Play")) {
                    try {
                        if (!cryPlayer_.play(s.file, ctx_.assets->read(s.file))) ctx_.setStatus("Cannot play " + cryPlayer_.error(), true);
                    } catch (const std::exception& e) {
                        ctx_.setStatus("Cannot play " + s.file + ": " + e.what(), true);
                    }
                }
                if (!exists) ImGui::EndDisabled();
                ImGui::SameLine();
                if (exists) ImGui::TextUnformatted(s.file.c_str());
                else ImGui::TextDisabled("%s (not in the archives)", s.file.c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (cries.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "The client has no cries for this package: it plays nothing.");
}

bool ServerWorkspace::creaturesChanged() const {
    if (classCries_ != classCriesSaved_ || classFlags_ != classFlagsSaved_) return true;
    if (creatureDefs_.size() != creatureDefsSaved_.size()) return true;
    for (size_t k = 0; k < creatureDefs_.size(); ++k)
        if (!sameCreatureDefinition(creatureDefs_[k], creatureDefsSaved_[k])) return true;
    return false;
}

bool ServerWorkspace::saveCreatures() {
    if (!database_ || database_->readOnly()) return false;
    WorldDatabase::SaveSummary summary;
    std::string error;
    if (!database_->saveCreatures(creatureDefsSaved_, creatureDefs_, summary, error, &classCriesSaved_, &classCries_, &classFlagsSaved_, &classFlags_)) {
        ctx_.setStatus("Saving creatures failed, nothing was written: " + error, true);
        return false;
    }
    reloadCreatureDefinitions();
    if (ctx_.scene) ctx_.scene->refreshServerContent();               // avatars follow the new appearance
    ctx_.setStatus("Creatures saved: " + std::to_string(summary.inserted) + " added, " + std::to_string(summary.updated) + " changed, " +
                       std::to_string(summary.deleted) + " deleted. Restart the server to apply.",
                   false);
    return true;
}

// The name the client shows (creature.py Creature.GetName): with a name id, the creaturenamelanguage text, with the actor name
// (else the entity class name) substituted when the text has exactly one "%s"; without one (the server sends none for 0),
// the actor name (creature_actor_name), else the entity class name.
std::string ServerWorkspace::creatureDisplayName(const CreatureDefinition& d) const {
    const Creature& c = d.creature;
    std::string fallback = d.hasActorName ? d.actorName : "";
    if (fallback.empty()) fallback = ctx_.classInfo(c.classId).name;
    if (c.nameId <= 0 || !ctx_.gameData) return fallback;
    std::string name;
    try { name = ctx_.gameData->creatureName(c.nameId); } catch (const std::exception&) {}
    const size_t at = name.find("%s");
    if (at != std::string::npos && name.find("%s", at + 2) == std::string::npos && !fallback.empty()) name.replace(at, 2, fallback);
    return name;
}

std::string ServerWorkspace::creatureLabel(const CreatureDefinition& d) const {
    const Creature& c = d.creature;
    const std::string name = creatureDisplayName(d);
    char text[256];
    std::snprintf(text, sizeof text, "%d  %s%s%s  (lvl %d, %s)", c.id, c.comment.c_str(), name.empty() || name == c.comment ? "" : "  \"",
                  name.empty() || name == c.comment ? "" : (name + "\"").c_str(), c.level, creatureFactionName(c.faction));
    return text;
}

void ServerWorkspace::drawCreaturePanel() {
    if (focusCreatures_ > 0) {
        ImGui::SetNextWindowFocus();
        --focusCreatures_;
    }
    if (!ImGui::Begin("Creatures###Server.Creatures")) { ImGui::End(); return; }
    if (!database_) {
        ImGui::TextDisabled("Open a server database first.");
        ImGui::End();
        return;
    }
    const bool writable = !database_->readOnly();
    const bool changed = creaturesChanged();
    if (writable) {
        if (ImGui::Button("New")) {
            CreatureDefinition d;
            for (const CreatureDefinition& existing : creatureDefs_)
                if (existing.creature.id == creatureSelected_) d = existing;          // start from the selected creature
            int32_t next = 1;
            for (const CreatureDefinition& existing : creatureDefs_) next = std::max(next, existing.creature.id + 1);
            d.creature.id = next;
            d.creature.comment = d.creature.comment.empty() ? "New creature" : d.creature.comment + " (copy)";
            creatureDefs_.push_back(d);
            creatureSelected_ = next;
        }
        ImGui::SetItemTooltip("Adds a creature (a copy of the selected one) with the next free id; set its id before saving if needed.");
        ImGui::SameLine();
        if (!changed) ImGui::BeginDisabled();
        if (ImGui::Button("Save creatures")) saveCreatures();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            creatureDefs_ = creatureDefsSaved_;
            classCries_ = classCriesSaved_;
            classFlags_ = classFlagsSaved_;
            ctx_.setStatus("Creature edits reverted.", false);
        }
        if (!changed) ImGui::EndDisabled();
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Read-only database.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("On this map", &creatureMapOnly_);
    ImGui::SetItemTooltip("Only creatures used by this map's spawn pools.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##creaturelistfilter", "filter by id, comment or name", creatureListFilter_, sizeof creatureListFilter_);
    const std::string needle = tr::toLowerAscii(creatureListFilter_);
    std::set<int32_t> onMap;
    for (const SpawnPool& p : serverContent_.spawnPools)
        for (int k = 0; k < 6; ++k)
            if (spawnSlot(p, k).creatureId > 0) onMap.insert(spawnSlot(p, k).creatureId);
    if (ImGui::BeginChild("creaturelist", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.35f), ImGuiChildFlags_Borders)) {
        ImGuiListClipper clipper;
        std::vector<size_t> shown;
        for (size_t k = 0; k < creatureDefs_.size(); ++k) {
            if (creatureMapOnly_ && !onMap.count(creatureDefs_[k].creature.id)) continue;
            if (!needle.empty() && tr::toLowerAscii(creatureLabel(creatureDefs_[k])).find(needle) == std::string::npos) continue;
            shown.push_back(k);
        }
        clipper.Begin(int(shown.size()));
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const CreatureDefinition& d = creatureDefs_[shown[size_t(row)]];
                ImGui::PushID(row);
                bool modified = true;
                for (const CreatureDefinition& s : creatureDefsSaved_)
                    if (s.creature.id == d.creature.id) modified = !sameCreatureDefinition(s, d);
                const std::string label = (modified ? "* " : "") + creatureLabel(d);
                if (ImGui::Selectable(label.c_str(), d.creature.id == creatureSelected_)) creatureSelected_ = d.creature.id;
                ImGui::PopID();
            }
    }
    ImGui::EndChild();
    CreatureDefinition* selected = nullptr;
    for (CreatureDefinition& d : creatureDefs_)
        if (d.creature.id == creatureSelected_) selected = &d;
    if (!selected) {
        ImGui::TextDisabled("Select a creature.");
    } else {
        bool isNew = true;
        for (const CreatureDefinition& s : creatureDefsSaved_)
            if (s.creature.id == selected->creature.id) isNew = false;
        if (ImGui::BeginChild("creatureeditor")) {
            if (!writable) ImGui::BeginDisabled();
            drawCreatureEditor(*selected, isNew);
            if (!writable) ImGui::EndDisabled();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void ServerWorkspace::drawCreatureEditor(CreatureDefinition& d, bool isNew) {
    Creature& c = d.creature;
    const float fieldWidth = ImGui::GetFontSize() * 12;
    auto integer = [&](const char* label, int32_t& value) {
        int v = value;
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::InputInt(label, &v)) value = v;
    };
    if (isNew) {
        int32_t id = c.id;
        integer("Id (new)", id);
        if (id != c.id && id > 0) {
            bool taken = false;
            for (const CreatureDefinition& other : creatureDefs_) taken = taken || (&other != &d && other.creature.id == id);
            if (!taken) {
                c.id = id;
                creatureSelected_ = id;
            }
        }
    } else {
        ImGui::TextDisabled("id %d", c.id);
    }
    {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%s", c.comment.c_str());
        ImGui::SetNextItemWidth(fieldWidth * 1.5f);
        if (ImGui::InputText("Comment", buffer, 51)) c.comment = buffer;          // varchar(50)
    }
    integer("Name id", c.nameId);
    ImGui::SameLine();
    {
        std::string text;
        if (ctx_.gameData && c.nameId > 0) {
            try { text = ctx_.gameData->creatureName(c.nameId); } catch (const std::exception&) {}
        }
        if (c.nameId > 0 && text.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "(not in creaturenamelanguage)");
        else ImGui::TextDisabled("in game: \"%s\"%s", creatureDisplayName(d).c_str(),
                                 c.nameId > 0 ? "" : d.hasActorName ? " (actor name)" : " (class name: no name id or actor name)");
    }
    integer("Class", c.classId);
    ImGui::SameLine();
    {
        const ClassInfo& info = ctx_.classInfo(c.classId);
        ImGui::TextDisabled("%s", info.known ? info.name.c_str() : "(unknown class)");
        if (info.known && !info.mesh.empty()) ImGui::SetItemTooltip("%s", info.mesh.c_str());
    }
    {
        int faction = std::clamp(c.faction, 0, 6);
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::Combo("Faction", &faction, "hostile\0friendly\0object\0neutral\0decoration\0decoration proxy\0ignore\0")) c.faction = faction;
    }
    integer("Level", c.level);
    integer("Max HP", c.maxHp);
    integer("Run speed", c.runSpeed);
    integer("Walk speed", c.walkSpeed);
    if (database_->hasTable("creature_class_flag")) drawClassFlags(d);
    if (database_->hasTable("creature_battlecry")) drawBattlecry(d);
    ImGui::SeparatorText("Actions (creature_action)");
    const std::vector<CreatureAction>& actions = database_->creatureActions();
    for (int k = 0; k < 8; ++k) {
        int32_t& action = creatureAction(c, k);
        char preview[160] = "(none)";
        for (const CreatureAction& a : actions)
            if (a.id == action) std::snprintf(preview, sizeof preview, "%d  %s", a.id, a.description.c_str());
        if (action != 0 && std::strcmp(preview, "(none)") == 0) std::snprintf(preview, sizeof preview, "%d (not in creature_action)", action);
        char label[16];
        std::snprintf(label, sizeof label, "Action %d", k + 1);
        ImGui::SetNextItemWidth(fieldWidth * 1.6f);
        if (ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("(none)", action == 0)) action = 0;
            for (const CreatureAction& a : actions) {
                char item[200];
                std::snprintf(item, sizeof item, "%d  %s  (damage %d-%d, range %.1f-%.1f m, cooldown %d ms)", a.id, a.description.c_str(), a.minDamage,
                              a.maxDamage, a.rangeMin, a.rangeMax, a.cooldown);
                if (ImGui::Selectable(item, a.id == action)) action = a.id;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SeparatorText("Attributes (creature_stat)");
    ImGui::Checkbox("Has a creature_stat row", &d.hasStat);
    if (d.hasStat) {
        integer("Body", d.stat.body);
        integer("Mind", d.stat.mind);
        integer("Spirit", d.stat.spirit);
        integer("Health", d.stat.health);
        integer("Armor", d.stat.armor);
    } else {
        ImGui::TextDisabled("The server uses its defaults.");
    }
    ImGui::SeparatorText("Appearance (creature_appearance)");
    int remove = -1;
    for (size_t k = 0; k < d.appearance.size(); ++k) {
        AppearanceSlot& slot = d.appearance[k];
        ImGui::PushID(int(k));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        int slotId = slot.slotId;
        if (ImGui::InputInt("##slot", &slotId, 0)) slot.slotId = slotId;
        ImGui::SameLine();
        ImGui::TextDisabled("%-8s", equipmentSlotName(slot.slotId));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        int classId = slot.classId;
        if (ImGui::InputInt("##class", &classId, 0)) slot.classId = classId;
        ImGui::SetItemTooltip("%s", ctx_.classInfo(slot.classId).name.c_str());
        ImGui::SameLine();
        // ABGR, red in the low byte.
        float colour[4] = {float(slot.colour & 0xFF) / 255.0f, float((slot.colour >> 8) & 0xFF) / 255.0f, float((slot.colour >> 16) & 0xFF) / 255.0f,
                           float((slot.colour >> 24) & 0xFF) / 255.0f};
        if (ImGui::ColorEdit4("##colour", colour, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
            auto byte = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            slot.colour = byte(colour[0]) | (byte(colour[1]) << 8) | (byte(colour[2]) << 16) | (byte(colour[3]) << 24);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%08x", slot.colour);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove = int(k);
        ImGui::PopID();
    }
    if (remove >= 0) d.appearance.erase(d.appearance.begin() + remove);
    if (ImGui::SmallButton("Add slot")) d.appearance.push_back(AppearanceSlot{15, 0, 0});
    std::sort(d.appearance.begin(), d.appearance.end(), [](const AppearanceSlot& a, const AppearanceSlot& b) { return a.slotId < b.slotId; });
    ImGui::TextDisabled("The viewport shows saved appearances (avatars are built from the database).");
    // NPC and vendor rows (see CreatureDefinition).
    const bool npcClass = database_->classIsNpc(c.classId), vendorClass = database_->classIsVendor(c.classId);
    if (npcClass || d.hasNpcPackage || d.hasGreeting || d.hasActorName) {
        ImGui::SeparatorText("NPC");
        if (!npcClass) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Its class has no NPC augmentation: the server ignores these.");
        ImGui::Checkbox("Conversation package (npc_package)", &d.hasNpcPackage);
        if (d.hasNpcPackage) {
            integer("Package id", d.npcPackageId);
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%s", d.npcPackageComment.c_str());
            ImGui::SetNextItemWidth(fieldWidth * 1.5f);
            if (ImGui::InputText("Package comment", buffer, 51)) d.npcPackageComment = buffer;
        }
        ImGui::Checkbox("Greeting (npc_greeting)", &d.hasGreeting);
        if (d.hasGreeting) {
            integer("Greeting id", d.greetingId);
            std::string line;
            if (ctx_.gameData && d.greetingId > 0) {
                try { line = ctx_.gameData->npcGreeting(d.greetingId); } catch (const std::exception&) {}
            }
            ImGui::TextWrapped("%s", line.empty() ? "(no such line in npcgreetinglanguage)" : ("\"" + line + "\"").c_str());
            if (database_ && database_->hasColumn("npc_greeting", "important")) {
                ImGui::Checkbox("Important", &d.greetingImportant);
                ImGui::SetItemTooltip("npc_greeting.important: a speech bubble over the NPC until the character has read this line\n"
                                      "(Rasa.NET NpcGreetings.IsUnread).");
            }
        } else {
            ImGui::TextDisabled("Says the server's default, \"Greetings.\"");
        }
        ImGui::Checkbox("Server-given name (creature_actor_name)", &d.hasActorName);
        if (d.hasActorName) {
            char buffer[128];
            std::snprintf(buffer, sizeof buffer, "%s", d.actorName.c_str());
            ImGui::SetNextItemWidth(fieldWidth * 1.5f);
            if (ImGui::InputText("Actor name", buffer, sizeof buffer)) d.actorName = buffer;
        }
    }
    if (vendorClass || d.hasVendor || !d.vendorItems.empty()) {
        ImGui::SeparatorText("Vendor");
        if (!vendorClass) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Its class has no vendor augmentation: the server ignores these.");
        ImGui::Checkbox("Vendor row (vendor)", &d.hasVendor);
        if (d.hasVendor) integer("Vendor package id", d.vendorPackageId);
        ImGui::Checkbox("One price for everything (vendor_price)", &d.hasVendorPrice);
        if (d.hasVendorPrice) integer("Price (credits)", d.vendorPrice);
        ImGui::Text("Stock: %zu item template(s)", d.vendorItems.size());
        int removeItem = -1;
        if (ImGui::BeginTable("vendorstock", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * std::min<size_t>(10, d.vendorItems.size() + 1)))) {
            ImGui::TableSetupColumn("Template");
            ImGui::TableSetupColumn("Item class");
            ImGui::TableSetupColumn("Buy price");
            ImGui::TableSetupColumn("");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (size_t k = 0; k < d.vendorItems.size(); ++k) {
                ImGui::PushID(int(20000 + k));
                const ItemTemplateInfo* info = database_->itemTemplate(d.vendorItems[k]);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", d.vendorItems[k]);
                ImGui::TableNextColumn();
                if (info) ImGui::TextUnformatted(ctx_.classInfo(info->classId).name.c_str());
                else ImGui::TextDisabled("(not in itemtemplate)");
                ImGui::TableNextColumn();
                if (info) ImGui::Text("%d", d.hasVendorPrice ? d.vendorPrice : info->buyPrice);
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Remove")) removeItem = int(k);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (removeItem >= 0) d.vendorItems.erase(d.vendorItems.begin() + removeItem);
        static int newItem = 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        ImGui::InputInt("##newitem", &newItem, 0);
        ImGui::SameLine();
        const ItemTemplateInfo* candidate = database_->itemTemplate(newItem);
        if (!candidate) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Add item template")) d.vendorItems.push_back(newItem);
        if (!candidate) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", candidate ? ctx_.classInfo(candidate->classId).name.c_str() : "(enter an itemtemplate id)");
    }
    ImGui::SeparatorText("Used on this map");
    int uses = 0;
    for (size_t k = 0; k < serverContent_.spawnPools.size(); ++k) {
        const SpawnPool& p = serverContent_.spawnPools[k];
        bool uses_ = false;
        for (int s = 0; s < 6; ++s) uses_ = uses_ || spawnSlot(p, s).creatureId == c.id;
        if (!uses_) continue;
        ++uses;
        ImGui::PushID(int(10000 + k));
        if (ImGui::Selectable(serverObjectLabel(Selection{SelectionKind::SpawnPool, k}).c_str())) {
            selection_ = Selection{SelectionKind::SpawnPool, k};
            viewport_.focusOn(ctx_, selection_);
        }
        ImGui::PopID();
    }
    if (!uses) ImGui::TextDisabled("No spawn pool on this map uses it.");
}

void ServerWorkspace::handleShortcuts() {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
        redo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal) && canEditServer() && hasUnsavedChanges()) saveServer();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, ImGuiInputFlags_RouteGlobal) && canEditServer() && isSelected(selection_) &&
        selection_.kind != SelectionKind::MapEntity)
        duplicateSelected();
}
