#include "ServerWorkspace.h"

#include "imgui_internal.h"   // DockBuilder

#include <algorithm>
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
    ImGui::DockBuilderDockWindow("Map###Server.Map", rightBottom);
    ImGui::DockBuilderFinish(dockspace);
}

void ServerWorkspace::beginFrame() {
    if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) editItem_ = 0;     // interaction finished
}

void ServerWorkspace::onMapLoaded() {
    loadServerContent();
    overview_.onMapLoaded(ctx_);
}

void ServerWorkspace::showDatabaseDialog() {
    static const SDL_DialogFileFilter filters[] = {{"SQLite database", "db;sqlite;sqlite3"}, {"All files", "*"}};
    const std::string current = ctx_.setting("database");
    SDL_ShowOpenFileDialog(databaseDialogCallback, this, ctx_.window, filters, 2, current.empty() ? nullptr : current.c_str(), false);
}

void ServerWorkspace::drawPanels() {
    maps_.draw(ctx_, "Maps###Server.Maps");
    drawServerPanel();
    drawInspectorPanel();
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
    layers.label = [this](const Selection& s) { return serverObjectLabel(s); };
    layers.position = [this](const Selection& s, tr::Vec3& p) { return selectionPosition(s, p); };
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
        auto database = std::make_unique<WorldDatabase>(path);
        if (ctx_.scene) ctx_.scene->setServerContent(nullptr, nullptr, nullptr);
        serverContent_ = WorldContent{};
        if (selection_.kind != SelectionKind::MapEntity) selection_ = Selection{};
        database_ = std::move(database);
        ctx_.setSetting("database", path);
        loadServerContent();
        ctx_.setStatus("Opened server database " + path + (database_->warnings().empty() ? "" : "  (with schema warnings, see the Server panel)"),
                  !database_->warnings().empty());
    } catch (const std::exception& e) {
        ctx_.setStatus(std::string("Could not open server database: ") + e.what(), true);
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
        default: break;
    }
    return "";
}

void ServerWorkspace::drawServerPanel() {
    if (!ImGui::Begin("Server objects###Server.Objects")) { ImGui::End(); return; }
    if (!database_) {
        ImGui::TextWrapped("No server database open. Use File > Open Server Database... and choose the Rasa.NET World database "
                           "(rasaworld.db with the SQLite provider). It is opened read-only.");
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
            if (ImGui::MenuItem("Footlocker")) addServerObject(SelectionKind::Object, ObjectKind::Footlocker);
            if (ImGui::MenuItem("Kraftwerk")) addServerObject(SelectionKind::Object, ObjectKind::Kraftwerk);
            if (ImGui::MenuItem("Logos stone")) addServerObject(SelectionKind::Object, ObjectKind::Logos);
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
        list("Teleporters", SelectionKind::Teleporter, serverContent_.teleporters.size());
        list("Objects", SelectionKind::Object, serverContent_.objects.size());
        list("Map links", SelectionKind::MapLink, serverContent_.mapLinks.size());
        list("Mission areas", SelectionKind::MissionArea, serverContent_.missionAreas.size());
        list("Regions", SelectionKind::Region, serverContent_.regions.size());
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
            if (arrivals) ImGui::Text("%d dropship arrival point(s)", arrivals);
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
                ImGui::Text("%s, %s", serverContent_.regions[i].underground ? "underground" : "surface", serverContent_.regions[i].enabled ? "enabled" : "disabled");
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
            serverContent_.spawnPools.erase(serverContent_.spawnPools.begin() + std::ptrdiff_t(i));
            what = "spawn pool" + (arrivals ? " (and its " + std::to_string(arrivals) + " arrival point(s) when saved)" : std::string());
            break;
        }
        case SelectionKind::Teleporter:
            if (i >= serverContent_.teleporters.size()) return;
            beginEdit();
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
            if (serverContent_.objects[i].kind == ObjectKind::ControlPoint) {
                ctx_.setStatus("Control points can be edited but not deleted.", true);
                return;
            }
            beginEdit();
            what = objectKindName(serverContent_.objects[i].kind);
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
            if (i >= serverContent_.objects.size() || serverContent_.objects[i].kind == ObjectKind::ControlPoint) {
                ctx_.setStatus("Control points cannot be duplicated.", true);
                return;
            }
            PlacedObject copy = serverContent_.objects[i];
            copy.id = 0;
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

void ServerWorkspace::addServerObject(SelectionKind kind, ObjectKind objectKind) {
    if (!canEditServer() || !ctx_.scene) return;
    tr::Vec3 rayOrigin, rayDirection, point;
    ctx_.scene->viewRay(0.0f, 0.0f, rayOrigin, rayDirection);
    if (!ctx_.scene->groundPoint(rayOrigin, rayDirection, rayOrigin.y - 10.0f, point)) point = add(rayOrigin, scaled(rayDirection, 20.0f));
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
            serverContent_.objects.push_back(o);
            selection_ = Selection{SelectionKind::Object, serverContent_.objects.size() - 1};
            break;
        }
    }
    contentChanged();
    ctx_.setStatus("Added at the centre of the view; set its fields in the Entity panel (not saved yet).", false);
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
            break;
        }
        case SelectionKind::Teleporter: {
            if (i >= serverContent_.teleporters.size()) break;
            Teleporter& t = serverContent_.teleporters[i];
            ImGui::TextDisabled("id %lld", static_cast<long long>(t.id));
            position("Position", t.position);
            angle("Facing", t.rotation);
            classField("Class", t.classId);
            integer("Type", t.type);
            text("Description", t.description);
            break;
        }
        case SelectionKind::Object: {
            if (i >= serverContent_.objects.size()) break;
            PlacedObject& o = serverContent_.objects[i];
            ImGui::TextDisabled("%s, id %lld", objectKindName(o.kind), static_cast<long long>(o.id));
            position("Position", o.position);
            if (o.kind != ObjectKind::Logos) angle("Facing", o.rotation);
            classField("Class", o.classId);
            text(o.kind == ObjectKind::Logos || o.kind == ObjectKind::ControlPoint ? "Name" : "Comment", o.name);
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
            flag("Underground", r.underground);
            ImGui::SameLine();
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
