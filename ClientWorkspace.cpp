#include "ClientWorkspace.h"

#include "Lookups.h"

#include "imgui_internal.h"   // DockBuilder

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

void ClientWorkspace::buildDefaultLayout(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID left = 0, centre = 0, right = 0, bottom = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.20f, &left, &centre);
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.25f, &right, &centre);
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.35f, &bottom, &centre);
    ImGui::DockBuilderDockWindow("Game###Client.Game", left);
    ImGui::DockBuilderDockWindow("Maps###Client.Maps", left);
    ImGui::DockBuilderDockWindow("Viewport###Client.Viewport", centre);      // first docked = the visible tab
    ImGui::DockBuilderDockWindow("Overview###Client.Overview", centre);
    ImGui::DockBuilderDockWindow("Entities###Client.Entities", bottom);
    ImGui::DockBuilderDockWindow("Lights###Client.Lights", bottom);
    ImGui::DockBuilderDockWindow("Map###Client.Map", right);
    ImGui::DockBuilderDockWindow("Entity###Client.Entity", right);
    ImGui::DockBuilderFinish(dockspace);
}

void ClientWorkspace::onGameOpened() {
    selection_ = Selection{};
    visibleEntities_.clear();
}

void ClientWorkspace::onMapLoaded() {
    selection_ = Selection{};
    rebuildEntityFilter();
    overview_.onMapLoaded(ctx_);
    undoStack_.clear();
    redoStack_.clear();
    cleanUndoDepth_ = 0;
    cleanReachable_ = true;
    fitBounds_.clear();
    saved_ = snapshot();
}

// ------------------------------------------------------------------------------------------
// Map editing
// ------------------------------------------------------------------------------------------
ClientWorkspace::MapEdit ClientWorkspace::snapshot() const {
    if (!ctx_.map) return MapEdit{};
    return MapEdit{ctx_.map->map.entities, ctx_.map->map.generatedAbvs, ctx_.map->lightsXml};
}

void ClientWorkspace::restore(const MapEdit& edit) {
    if (!ctx_.map) return;
    ctx_.map->map.entities = edit.entities;
    ctx_.map->map.generatedAbvs = edit.abvs;
    const bool lightsDiffer = ctx_.map->lightsXml != edit.lights;
    ctx_.map->lightsXml = edit.lights;
    if (lightsDiffer) lightsChanged();
    entitiesChanged();
}

void ClientWorkspace::beginEdit() {
    if (!ctx_.map) return;
    if (undoStack_.size() < cleanUndoDepth_) cleanReachable_ = false;
    undoStack_.push_back(snapshot());
    redoStack_.clear();
    if (undoStack_.size() > 200) {                   // whole-map snapshots: keep memory bounded
        undoStack_.erase(undoStack_.begin());
        if (cleanUndoDepth_ > 0) --cleanUndoDepth_;
        else cleanReachable_ = false;
    }
    // The map's .vis visibility data (leaf -> potentially visible leaves) was computed from the shipped geometry and is not
    // rebuilt by the editor: moving or deleting a large occluder (a cave wall, a building) can leave things behind it culled in
    // game. Warn for entities over 80 m across (the editor's own threshold, not the game's: 87 of Bootcamp's 1,435 entities, 212 of
    // Concordia's 5,758 by their bounding spheres; at 30 m it was a third of all entities).
    const int k = selectedEntity();
    tr::Vec3 centre;
    float radius = 0;
    if (k >= 0 && ctx_.scene && ctx_.scene->entityBounds(size_t(k), centre, radius) && radius >= 40.0f)
        ctx_.setStatus("Editing a large entity (" + std::to_string(int(radius * 2.0f + 0.5f)) +
                           " m across): the map's .vis visibility data is not rebuilt, so moving or deleting it can leave what was behind "
                           "it hidden in game.",
                       true);
}

void ClientWorkspace::noteEdit() {
    const ImGuiID id = ImGui::GetItemID();
    if (id != editItem_) {
        beginEdit();
        editItem_ = id;
    }
}

void ClientWorkspace::undo() {
    if (undoStack_.empty()) return;
    redoStack_.push_back(snapshot());
    const MapEdit edit = undoStack_.back();
    undoStack_.pop_back();
    restore(edit);
}

void ClientWorkspace::redo() {
    if (redoStack_.empty()) return;
    undoStack_.push_back(snapshot());
    const MapEdit edit = redoStack_.back();
    redoStack_.pop_back();
    restore(edit);
}

void ClientWorkspace::entityChanged(size_t index) {
    if (ctx_.scene) ctx_.scene->refreshEntity(index);
}

void ClientWorkspace::entitiesChanged() {
    if (ctx_.scene) ctx_.scene->rebuildEntities();
    if (selection_.kind == SelectionKind::MapEntity && (!ctx_.map || selection_.index >= ctx_.map->map.entities.size())) selection_ = Selection{};
    rebuildEntityFilter();
}

namespace {

std::vector<float> parseNumbers(const std::string& s) {
    std::vector<float> out;
    const char* p = s.c_str();
    while (*p) {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) { ++p; continue; }
        out.push_back(v);
        p = end;
    }
    return out;
}

}  // namespace

void ClientWorkspace::lightsChanged() {
    if (ctx_.scene) ctx_.scene->reloadMapLights();
    if (selection_.kind == SelectionKind::MapLight && (!ctx_.scene || selection_.index >= ctx_.scene->mapLights().size())) selection_ = Selection{};
}

void ClientWorkspace::setLightProperty(size_t light, const std::string& name, const std::string& value, const std::string& type) {
    if (!ctx_.map) return;
    tr::LightsDocument doc(ctx_.map->lightsXml);
    if (doc.property(light, name) == value) return;
    if (!doc.setProperty(light, name, value, type)) return;
    ctx_.map->lightsXml = doc.bytes();
    lightsChanged();
}

tr::Vec3* ClientWorkspace::selectedPosition() {
    if (selectedLight() >= 0 && ctx_.map) {
        const std::vector<float> p = parseNumbers(tr::LightsDocument(ctx_.map->lightsXml).property(size_t(selectedLight()), "Position"));
        if (p.size() < 3) return nullptr;
        lightPosition_ = tr::Vec3{p[0], p[1], p[2]};
        return &lightPosition_;
    }
    const int k = selectedEntity();
    return ctx_.map && k >= 0 && size_t(k) < ctx_.map->map.entities.size() ? &ctx_.map->map.entities[size_t(k)].location : nullptr;
}

float* ClientWorkspace::selectedRotation() {
    if (!selectedPosition()) return nullptr;
    yawHandle_ = 0;                                  // the viewport adds its step; contentChanged applies it
    return &yawHandle_;
}

// Rotation about the world's vertical axis by the yaw handle - the server's yaw convention (CreateFromYawPitchRoll(r, 0, 0):
// q = (0, sin r/2, 0, cos r/2)), applied before the entity's own rotation so any tilt it has is kept.
namespace {

tr::Quat yawRotated(const tr::Quat& q, float yaw) {
    const float h = yaw * 0.5f;
    const tr::Quat r{0, std::sin(h), 0, std::cos(h)};
    return tr::Quat{r.w * q.x + r.x * q.w + r.y * q.z - r.z * q.y, r.w * q.y - r.x * q.z + r.y * q.w + r.z * q.x,
                    r.w * q.z + r.x * q.y - r.y * q.x + r.z * q.w, r.w * q.w - r.x * q.x - r.y * q.y - r.z * q.z};
}

}  // namespace

void ClientWorkspace::contentChanged() {
    if (selectedLight() >= 0 && ctx_.map) {
        // Lights: Position from the dragged proxy; yaw into WorldOrientation (spot / plane) or LocalOrientation (point), x y z w.
        const size_t light = size_t(selectedLight());
        const float p[3] = {lightPosition_.x, lightPosition_.y, lightPosition_.z};
        tr::LightsDocument doc(ctx_.map->lightsXml);
        const std::vector<float> current = parseNumbers(doc.property(light, "Position"));
        if (current.size() < 3 || current[0] != p[0] || current[1] != p[1] || current[2] != p[2])
            doc.setProperty(light, "Position", tr::LightsDocument::formatVector(p, 3));
        if (yawHandle_ != 0.0f) {
            const std::string key = !doc.property(light, "WorldOrientation").empty() ? "WorldOrientation" : "LocalOrientation";
            const std::vector<float> q = parseNumbers(doc.property(light, key));
            const tr::Quat rotated = yawRotated(q.size() >= 4 ? tr::Quat{q[0], q[1], q[2], q[3]} : tr::Quat{0, 0, 0, 1}, yawHandle_);
            const float v[4] = {rotated.x, rotated.y, rotated.z, rotated.w};
            doc.setProperty(light, key, tr::LightsDocument::formatVector(v, 4));
            yawHandle_ = 0;
        }
        if (doc.bytes() != ctx_.map->lightsXml) {
            ctx_.map->lightsXml = doc.bytes();
            lightsChanged();
        }
        return;
    }
    const int k = selectedEntity();
    if (!ctx_.map || k < 0 || size_t(k) >= ctx_.map->map.entities.size()) return;
    tr::MapEntity& e = ctx_.map->map.entities[size_t(k)];
    if (yawHandle_ != 0.0f) {
        e.orientation = yawRotated(e.orientation, yawHandle_);
        yawHandle_ = 0;
    }
    entityChanged(size_t(k));
}

void ClientWorkspace::deleteSelected() {
    if (selectedLight() >= 0 && ctx_.map) {
        beginEdit();
        tr::LightsDocument doc(ctx_.map->lightsXml);
        doc.remove(size_t(selectedLight()));               // with its <Mod> blocks
        ctx_.map->lightsXml = doc.bytes();
        selection_ = Selection{};
        lightsChanged();
        return;
    }
    const int k = selectedEntity();
    if (!ctx_.map || k < 0 || size_t(k) >= ctx_.map->map.entities.size()) return;
    beginEdit();
    tr::MapFile& m = ctx_.map->map;
    const uint64_t id = m.entities[size_t(k)].entityId;
    m.entities.erase(m.entities.begin() + k);
    m.generatedAbvs.erase(std::remove_if(m.generatedAbvs.begin(), m.generatedAbvs.end(), [&](const tr::MapGeneratedAbv& a) { return a.entityId == id; }),
                          m.generatedAbvs.end());
    selection_ = Selection{};
    entitiesChanged();
}

// A new entity id: one more than the largest in the map (ids are unique within a map; the server's control points and Bane
// teleporter arrivals refer to map entities by id, so existing ids are never reused).
uint64_t ClientWorkspace::newEntityId() const {
    uint64_t largest = 0;
    for (const tr::MapEntity& e : ctx_.map->map.entities) largest = std::max(largest, e.entityId);
    return largest + 1;
}

void ClientWorkspace::duplicateSelected() {
    if (selectedLight() >= 0 && ctx_.map && ctx_.scene) {
        // New ID: one more than the largest (light modifiers attach by ID); the copy 2 m east, with the original's modifiers.
        uint64_t largest = 0;
        for (const tr::PlacedLight& l : ctx_.scene->mapLights()) largest = std::max(largest, l.id);
        beginEdit();
        tr::LightsDocument doc(ctx_.map->lightsXml);
        const size_t source = size_t(selectedLight());
        const size_t copy = doc.duplicate(source, largest + 1, doc.property(source, "Name") + " copy");
        std::vector<float> p = parseNumbers(doc.property(copy, "Position"));
        if (p.size() >= 3) {
            p[0] += 2.0f;
            doc.setProperty(copy, "Position", tr::LightsDocument::formatVector(p.data(), 3));
        }
        ctx_.map->lightsXml = doc.bytes();
        lightsChanged();
        selection_ = Selection{SelectionKind::MapLight, copy};
        return;
    }
    const int k = selectedEntity();
    if (!ctx_.map || k < 0 || size_t(k) >= ctx_.map->map.entities.size()) return;
    beginEdit();
    tr::MapFile& m = ctx_.map->map;
    tr::MapEntity copy = m.entities[size_t(k)];
    const uint64_t oldId = copy.entityId;
    copy.entityId = newEntityId();
    copy.location.x += 2.0f;                          // beside the original
    std::vector<tr::MapGeneratedAbv> abvs;
    for (const tr::MapGeneratedAbv& a : m.generatedAbvs)
        if (a.entityId == oldId) {
            tr::MapGeneratedAbv b = a;
            b.entityId = copy.entityId;
            abvs.push_back(b);
        }
    m.generatedAbvs.insert(m.generatedAbvs.end(), abvs.begin(), abvs.end());
    m.entities.push_back(copy);
    selection_ = Selection{SelectionKind::MapEntity, m.entities.size() - 1};
    entitiesChanged();
}

// A new entity of a class at the ground point under the viewport's centre: upright, scale 1, no hue (0,0,0,255 = "not hued"),
// cull layer 0 (the class's), bounds from its model once loaded.
void ClientWorkspace::addEntity(int32_t classId) {
    if (!ctx_.map || !ctx_.scene) return;
    tr::Vec3 origin, direction, point;
    ctx_.scene->viewRay(0, 0, origin, direction);
    if (!ctx_.scene->surfacePoint(origin, direction, origin.y - 10.0f, point)) point = add(origin, scaled(direction, 20.0f));
    beginEdit();
    tr::MapEntity e;
    e.classId = classId;
    e.entityId = newEntityId();
    e.location = point;
    e.orientation = tr::Quat{0, 0, 0, 1};
    e.hue1 = tr::HueColour{0, 0, 0, 255};
    e.hue2 = tr::HueColour{0, 0, 0, 255};
    e.scale = 1;
    e.hasBounds = ctx_.map->map.minorVersion >= 45;
    e.bounds = tr::Bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 1, 0.5f}};
    ctx_.map->map.entities.push_back(e);
    selection_ = Selection{SelectionKind::MapEntity, ctx_.map->map.entities.size() - 1};
    fitBounds_.push_back(ctx_.map->map.entities.size() - 1);
    entitiesChanged();
}

namespace {

std::filesystem::path mapFilePath(const tr::MapBundle& map) {
    std::error_code ec;
    const std::string wanted = tr::toLowerAscii(map.name + ".map");
    for (const auto& f : std::filesystem::directory_iterator(map.folder, ec))
        if (tr::toLowerAscii(f.path().filename().string()) == wanted) return f.path();
    return std::filesystem::path(map.folder) / (map.name + ".map");
}

}  // namespace

// Writes the map in place (the client reads loose .map files): the first save copies the original to <name>.map.rasaedit-original;
// the new file is written beside it and moved over the old one, then read back and compared.
// Writes a file in place: the first write keeps the original as <file>.rasaedit-original; the data goes to a temporary file
// beside it, which then replaces the old one.
bool ClientWorkspace::writeFileWithBackup(const std::string& pathString, const tr::Bytes& data, std::string& error) {
    namespace fs = std::filesystem;
    const fs::path path(pathString);
    std::error_code ec;
    fs::path backup = path;
    backup += ".rasaedit-original";
    if (fs::exists(path, ec) && !fs::exists(backup, ec)) fs::copy_file(path, backup, ec);
    if (ec) { error = "cannot keep the original as " + backup.string() + ": " + ec.message(); return false; }
    fs::path temp = path;
    temp += ".rasaedit-new";
    std::FILE* f = std::fopen(temp.string().c_str(), "wb");
    if (!f) { error = "cannot write " + temp.string(); return false; }
    const size_t written = std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (written != data.size()) { error = "short write to " + temp.string(); return false; }
    fs::rename(temp, path, ec);
    if (ec) { error = "cannot replace " + path.string() + ": " + ec.message(); return false; }
    return true;
}

bool ClientWorkspace::saveMap() {
    if (!ctx_.map) return false;
    namespace fs = std::filesystem;
    const fs::path path = mapFilePath(*ctx_.map);
    const bool mapChanged = !(ctx_.map->map.entities.size() == saved_.entities.size() && ctx_.map->map.generatedAbvs.size() == saved_.abvs.size() &&
                              tr::writeMap(ctx_.map->map) == [&] { tr::MapFile m = ctx_.map->map; m.entities = saved_.entities; m.generatedAbvs = saved_.abvs; return tr::writeMap(m); }());
    const bool lightsChanged = ctx_.map->lightsXml != saved_.lights;
    std::string error;
    try {
        if (mapChanged) {
            const tr::Bytes data = tr::writeMap(ctx_.map->map);
            const tr::MapFile check = tr::readMap(data);
            if (check.entities.size() != ctx_.map->map.entities.size()) throw std::runtime_error("the written map does not read back");
            if (!writeFileWithBackup(path.string(), data, error)) throw std::runtime_error(error);
        }
        if (lightsChanged) {
            tr::readLights(ctx_.map->lightsXml);             // must still parse
            if (!writeFileWithBackup((path.parent_path() / "lights.xml").string(), ctx_.map->lightsXml, error)) throw std::runtime_error(error);
        }
    } catch (const std::exception& e) {
        ctx_.setStatus(std::string("Saving failed: ") + e.what(), true);
        return false;
    }
    undoStack_.clear();
    redoStack_.clear();
    cleanUndoDepth_ = 0;
    cleanReachable_ = true;
    saved_ = snapshot();
    ctx_.setStatus(std::string("Saved") + (mapChanged ? " " + path.filename().string() : "") + (lightsChanged ? " lights.xml" : "") + " in " +
                       path.parent_path().string() + ". The game reads them on the next map load.",
                   false);
    return true;
}

void ClientWorkspace::revertMap() {
    if (!hasUnsavedChanges()) return;
    restore(saved_);
    undoStack_.clear();
    redoStack_.clear();
    cleanUndoDepth_ = 0;
    cleanReachable_ = true;
    ctx_.setStatus("Map changes reverted.", false);
}

void ClientWorkspace::drawEditMenu() {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undoStack_.empty())) undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redoStack_.empty())) redo();
    ImGui::Separator();
    const bool selected = ctx_.map && (selectedEntity() >= 0 || selectedLight() >= 0);
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selected)) duplicateSelected();
    if (ImGui::MenuItem("Delete", "Del", false, selected)) deleteSelected();
    ImGui::Separator();
    if (ImGui::MenuItem("Save Map", "Ctrl+S", false, ctx_.map && hasUnsavedChanges())) saveMap();
    if (ImGui::MenuItem("Revert Map Changes", nullptr, false, hasUnsavedChanges())) revertMap();
}

void ClientWorkspace::handleShortcuts() {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
        redo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal) && hasUnsavedChanges()) saveMap();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, ImGuiInputFlags_RouteGlobal) && (selectedEntity() >= 0 || selectedLight() >= 0)) duplicateSelected();
}

void ClientWorkspace::drawPanels() {
    drawGamePanel(ctx_, "Game###Client.Game");
    maps_.draw(ctx_, "Maps###Client.Maps");
    drawMapInfoPanel(ctx_, "Map###Client.Map");
    drawEntitiesPanel();
    drawLightsPanel();
    drawEntityPanel();
    OverviewLayers layers;
    layers.selection = &selection_;
    layers.selectEntities = true;
    layers.position = [this](const Selection& s, tr::Vec3& p) {
        if (s.kind == SelectionKind::MapLight && ctx_.scene && s.index < ctx_.scene->mapLights().size()) {
            p = ctx_.scene->mapLights()[s.index].position;
            return true;
        }
        if (s.kind != SelectionKind::MapEntity || !ctx_.map || s.index >= ctx_.map->map.entities.size()) return false;
        p = ctx_.map->map.entities[s.index].location;
        return true;
    };
    layers.showInViewport = [this](const Selection& s) { viewport_.focusOn(ctx_, s); };
    overview_.draw(ctx_, "Overview###Client.Overview", layers);
    {
        // Preview hook: RASAEDIT_CLIENT_FOCUS=<panel title> brings that panel to the front once (e.g. Lights###Client.Lights).
        static int frame = 0;
        if (const char* focus = std::getenv("RASAEDIT_CLIENT_FOCUS"); focus && *focus && ++frame == 10) ImGui::SetWindowFocus(focus);   // once the layout is built
    }
    // Drawn last: on first creation the newest window becomes the selected tab of a shared dock node.
    // New class's bounds once its model is loaded (2.45 maps store per-entity bounds the game culls and picks with).
    if (ctx_.map && ctx_.scene)
        for (auto it = fitBounds_.begin(); it != fitBounds_.end();) {
            tr::Bounds b;
            if (*it >= ctx_.map->map.entities.size()) it = fitBounds_.erase(it);
            else if (ctx_.scene->entityModelBounds(*it, b)) {
                ctx_.map->map.entities[*it].bounds = b;
                ctx_.map->map.entities[*it].hasBounds = ctx_.map->map.minorVersion >= 45;
                entityChanged(*it);
                it = fitBounds_.erase(it);
            } else ++it;
        }
    viewport_.draw(ctx_, "Viewport###Client.Viewport", selection_, true, false, this);
}

void ClientWorkspace::drawEntitiesPanel() {
    if (!ImGui::Begin("Entities###Client.Entities")) { ImGui::End(); return; }
    if (!ctx_.map) { ImGui::TextDisabled("Select a map."); ImGui::End(); return; }
    const tr::MapFile& m = ctx_.map->map;
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputTextWithHint("##entityfilter", "filter by class name, mesh or id", entityFilter_, sizeof entityFilter_))
        rebuildEntityFilter();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu", visibleEntities_.size(), m.entities.size());
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("entities", 7, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Mesh", ImGuiTableColumnFlags_WidthStretch);
        const float numberWidth = ImGui::CalcTextSize("-00000.0").x;
        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, numberWidth);
        ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, numberWidth);
        ImGui::TableSetupColumn("Z", ImGuiTableColumnFlags_WidthFixed, numberWidth);
        ImGui::TableSetupColumn("Scale", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00.00").x);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(int(visibleEntities_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const size_t index = visibleEntities_[size_t(row)];
                const tr::MapEntity& e = m.entities[index];
                const ClassInfo& info = ctx_.classInfo(e.classId);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char label[32];
                std::snprintf(label, sizeof label, "%d##%zu", e.classId, index);
                if (ImGui::Selectable(label, selectedEntity() == int(index), ImGuiSelectableFlags_SpanAllColumns))
                    selection_ = Selection{SelectionKind::MapEntity, index};
                ImGui::TableNextColumn();
                if (info.known) ImGui::TextUnformatted(info.name.c_str());
                else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "(unknown class)");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(info.mesh.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", e.location.x);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", e.location.y);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", e.location.z);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", e.scale);
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

namespace {

const char* lightKindName(tr::PlacedLight::Kind kind) {
    switch (kind) {
        case tr::PlacedLight::Kind::Spot: return "spot";
        case tr::PlacedLight::Kind::Plane: return "plane";
        default: return "point";
    }
}

}  // namespace

void ClientWorkspace::drawLightsPanel() {
    if (!ImGui::Begin("Lights###Client.Lights")) { ImGui::End(); return; }
    if (!ctx_.map || !ctx_.scene) { ImGui::TextDisabled("Select a map."); ImGui::End(); return; }
    const std::vector<tr::PlacedLight>& lights = ctx_.scene->mapLights();
    if (!ctx_.scene->mapLightsError().empty())
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "lights.xml: %s", ctx_.scene->mapLightsError().c_str());
    ImGui::TextDisabled("%zu lights in the map's lights.xml (turn on Light markers in the viewport to click them)", lights.size());
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("lights", 8, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed);
        const float numberWidth = ImGui::CalcTextSize("-00000.0").x;
        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, numberWidth);
        ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, numberWidth);
        ImGui::TableSetupColumn("Z", ImGuiTableColumnFlags_WidthFixed, numberWidth);
        ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthFixed, numberWidth);
        ImGui::TableSetupColumn("Colour", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Intensity", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(int(lights.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const size_t index = size_t(row);
                const tr::PlacedLight& light = lights[index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char label[160];
                std::snprintf(label, sizeof label, "%s##light%zu", light.name.empty() ? "(unnamed)" : light.name.c_str(), index);
                const bool selected = selection_.kind == SelectionKind::MapLight && selection_.index == index;
                if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns))
                    selection_ = Selection{SelectionKind::MapLight, index};
                ImGui::TableNextColumn(); ImGui::TextUnformatted(lightKindName(light.kind));
                ImGui::TableNextColumn(); ImGui::Text("%.1f", light.position.x);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", light.position.y);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", light.position.z);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", light.range);
                ImGui::TableNextColumn();
                ImGui::ColorButton("##c", ImVec4(light.colour.x, light.colour.y, light.colour.z, 1), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
                ImGui::TableNextColumn(); ImGui::Text("%.2f", light.intensity);
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void ClientWorkspace::drawLightInspector(size_t index) {
    const tr::PlacedLight light = ctx_.scene->mapLights()[index];          // a copy: edits reload the list
    const tr::LightsDocument doc(ctx_.map->lightsXml);
    const float fieldWidth = ImGui::GetFontSize() * 12;
    auto noteIfActivated = [&]() { if (ImGui::IsItemActivated()) noteEdit(); };
    auto setFloat = [&](const char* name, float v) { setLightProperty(index, name, tr::LightsDocument::formatNumber(v), "float"); };
    ImGui::Text("Kind   %s", lightKindName(light.kind));
    ImGui::Text("ID     %llu", static_cast<unsigned long long>(light.id));
    char name[128];
    std::snprintf(name, sizeof name, "%s", light.name.c_str());
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::InputText("Name", name, sizeof name)) setLightProperty(index, "Name", name, "string");
    noteIfActivated();
    bool enabled = light.enabled, shadows = light.castsShadows;
    if (ImGui::Checkbox("Enabled", &enabled)) { beginEdit(); setLightProperty(index, "Enabled", enabled ? "true" : "false", "bool"); }
    ImGui::SameLine();
    if (ImGui::Checkbox("Casts shadows", &shadows)) { beginEdit(); setLightProperty(index, "CastsShadows", shadows ? "true" : "false", "bool"); }
    ImGui::SetItemTooltip("Placed-light shadows are not drawn by the editor (see CLAUDE.md, Placed-light shadows).");
    ImGui::SeparatorText("Placement");
    float position[3] = {light.position.x, light.position.y, light.position.z};
    ImGui::SetNextItemWidth(fieldWidth * 1.4f);
    if (ImGui::DragFloat3("Position", position, 0.05f, 0, 0, "%.2f")) setLightProperty(index, "Position", tr::LightsDocument::formatVector(position, 3), "string");
    noteIfActivated();
    const tr::Quat& q = light.worldOrientation;
    ImGui::TextDisabled("Orientation  x %.3f  y %.3f  z %.3f  w %.3f (Ctrl+wheel in the viewport turns it about the vertical)", q.x, q.y, q.z, q.w);
    if (light.kind == tr::PlacedLight::Kind::Spot) {
        const tr::Vec3 d = normalized(light.direction());
        ImGui::TextDisabled("Axis         %.3f, %.3f, %.3f", d.x, d.y, d.z);
    }
    ImGui::SeparatorText("Light");
    const std::vector<float> colourValues = parseNumbers(doc.property(index, "Color"));
    float colour[3] = {light.colour.x, light.colour.y, light.colour.z};
    if (ImGui::ColorEdit3("Colour", colour, ImGuiColorEditFlags_Float)) {
        const float rgba[4] = {colour[0], colour[1], colour[2], colourValues.size() >= 4 ? colourValues[3] : 1.0f};
        setLightProperty(index, "Color", tr::LightsDocument::formatVector(rgba, 4), "string");
    }
    noteIfActivated();
    float intensity = light.intensity, range = light.range, falloff = light.falloffPercent;
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::DragFloat("Intensity", &intensity, 0.01f, 0.0f, 100.0f, "%.3f")) setFloat("Intensity", intensity);
    noteIfActivated();
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::DragFloat("Range (m)", &range, 0.1f, 0.0f, 1000.0f, "%.2f")) setFloat("Range", range);
    noteIfActivated();
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::SliderFloat("Falloff", &falloff, 0.0f, 1.0f, "%.3f")) setFloat("FalloffPercent", falloff);
    noteIfActivated();
    ImGui::SetItemTooltip("FalloffPercent: full strength to (1 - falloff) x Range, then a linear fade to the range\n"
                          "(LightFalloff = (1 - FalloffPercent) x LightRange, tabula_rasa.exe FUN_00875360 / FUN_00879150).");
    if (light.kind == tr::PlacedLight::Kind::Spot) {
        float cone = light.coneAngle, penumbra = light.penumbraAngle;
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::DragFloat("Cone (deg)", &cone, 0.2f, 0.0f, 179.0f, "%.1f")) setFloat("ConeAngle", cone);
        noteIfActivated();
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::DragFloat("Penumbra (deg)", &penumbra, 0.2f, 0.0f, 179.0f, "%.1f")) setFloat("PenumbraAngle", penumbra);
        noteIfActivated();
    }
    if (light.kind == tr::PlacedLight::Kind::Plane) {
        float width = light.width, length = light.length;
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::DragFloat("Width", &width, 0.1f, 0.0f, 1000.0f, "%.2f")) setFloat("Width", width);
        noteIfActivated();
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::DragFloat("Length", &length, 0.1f, 0.0f, 1000.0f, "%.2f")) setFloat("Length", length);
        noteIfActivated();
    }
    char globe[128];
    std::snprintf(globe, sizeof globe, "%s", light.globeMap.c_str());
    ImGui::SetNextItemWidth(fieldWidth * 1.4f);
    if (ImGui::InputText("Globe map", globe, sizeof globe, ImGuiInputTextFlags_EnterReturnsTrue)) {
        beginEdit();
        setLightProperty(index, "GlobeMapFile", globe, "string");
    }
    ImGui::SetItemTooltip("A cube map .dds the light projects around itself (e.g. globe_vfx_crystal.dds); empty = none. Enter applies.");
    const size_t tracks = ctx_.scene->mapLightModifierTracks(light);
    if (tracks > 0) {
        const tr::PlacedLight now = ctx_.scene->displayedMapLight(index);
        ImGui::TextDisabled("Modifiers  %zu track(s) (kept, edited in lights.xml only); now: colour %.2f %.2f %.2f, intensity %.2f", tracks,
                            now.colour.x, now.colour.y, now.colour.z, now.intensity);
    } else {
        ImGui::TextDisabled("Modifiers  none");
    }
    ImGui::Spacing();
    if (ImGui::Button("Duplicate")) duplicateSelected();
    ImGui::SameLine();
    if (ImGui::Button("Delete")) deleteSelected();
    ImGui::SameLine();
    if (ImGui::Button("Show in viewport")) viewport_.focusOn(ctx_, selection_);
}

void ClientWorkspace::drawEntityPanel() {
    if (!ImGui::Begin("Entity###Client.Entity")) { ImGui::End(); return; }
    if (selection_.kind == SelectionKind::MapLight && ctx_.scene && selection_.index < ctx_.scene->mapLights().size()) {
        drawLightInspector(selection_.index);
        ImGui::End();
        return;
    }
    if (!ctx_.map || selectedEntity() < 0 || size_t(selectedEntity()) >= ctx_.map->map.entities.size()) {
        ImGui::TextDisabled("Nothing selected.");
        if (ctx_.map) {
            ImGui::SeparatorText("Add an entity");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
            lookupCombo("Class", newClass_, lookup::entityClass(ctx_, "(pick an entity class)"));
            const ClassInfo& info = ctx_.classInfo(newClass_);
            if (newClass_ > 0 && info.known) ImGui::TextDisabled("%s", info.mesh.empty() ? "(no mesh)" : info.mesh.c_str());
            if (!(newClass_ > 0 && info.known)) ImGui::BeginDisabled();
            if (ImGui::Button("Add at the view's centre")) addEntity(newClass_);
            if (!(newClass_ > 0 && info.known)) ImGui::EndDisabled();
        }
        ImGui::End();
        return;
    }
    const size_t index = size_t(selectedEntity());
    tr::MapEntity& e = ctx_.map->map.entities[index];
    const ClassInfo& info = ctx_.classInfo(e.classId);
    const float fieldWidth = ImGui::GetFontSize() * 12;
    bool changed = false;
    auto edited = [&](bool c) {                      // one undo step per field being edited
        if (ImGui::IsItemActivated()) noteEdit();
        if (c) changed = true;
    };
    int32_t classId = e.classId;
    ImGui::SetNextItemWidth(fieldWidth * 1.6f);
    if (lookupCombo("Class", classId, lookup::entityClass(ctx_))) {
        noteEdit();
        changed = true;
    }
    if (classId != e.classId) {
        e.classId = classId;
        fitBounds_.push_back(index);                 // the new model's bounds once it is loaded
    }
    ImGui::Text("Mesh  %s", info.mesh.empty() ? "(none)" : info.mesh.c_str());
    if (!info.mesh.empty() && ctx_.assets && ctx_.assets->exists(info.mesh)) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Open in Assets")) ctx_.showAssetRequest = info.mesh;
        ImGui::SetItemTooltip("The model in the Assets workspace: preview, extract, export to Blender.");
    }
    if (!info.mesh.empty() && ctx_.assets && !ctx_.assets->exists(info.mesh))
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "mesh file not found in the archives");
    if (ctx_.scene) ImGui::TextWrapped("Viewport: %s", ctx_.scene->modelStatus(index).c_str());
    ImGui::Text("Entity id %llu (high %u, low %u)", static_cast<unsigned long long>(e.entityId), tr::entityIdHigh(e.entityId),
                tr::entityIdLow(e.entityId));
    ImGui::SetItemTooltip("Server content refers to map entities by id (control_point.marker_entity_id, spawnpool_arrival.entity_id).");
    ImGui::SeparatorText("Placement");
    ImGui::SetNextItemWidth(fieldWidth * 1.4f);
    edited(ImGui::DragFloat3("Location", &e.location.x, 0.05f, 0, 0, "%.2f"));
    float yaw = yawDegrees(e.orientation);
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::DragFloat("Yaw (deg)", &yaw, 0.5f, 0, 0, "%.1f")) {
        yawHandle_ = (yaw - yawDegrees(e.orientation)) / 57.29578f;      // applied about the vertical, keeping any tilt
        changed = true;
    }
    if (ImGui::IsItemActivated()) noteEdit();
    ImGui::TextDisabled("Rotation  w %.3f  x %.3f  y %.3f  z %.3f", e.orientation.w, e.orientation.x, e.orientation.y, e.orientation.z);
    ImGui::SetNextItemWidth(fieldWidth);
    edited(ImGui::DragFloat("Scale", &e.scale, 0.01f, 0.01f, 100.0f, "%.3f"));
    if (ctx_.scene) {
        // The first surface straight below (architecture or terrain, not this entity; holes excluded) - as the viewport's snap.
        const float ground = ctx_.scene->surfaceHeightBelow(e.location.x, e.location.z, e.location.y + 2.0f, NAN, int(index));
        if (!std::isnan(ground)) {
            ImGui::Text("Ground    %.2f (entity %+.2f)", ground, e.location.y - ground);
            ImGui::SameLine();
            if (ImGui::SmallButton("Snap to ground")) {
                beginEdit();
                e.location.y = ground;
                changed = true;
            }
        }
    }
    ImGui::SeparatorText("Appearance");
    auto hueEdit = [&](const char* label, tr::HueColour& h) {
        float c[4] = {h.red / 255.0f, h.green / 255.0f, h.blue / 255.0f, h.alpha / 255.0f};
        if (ImGui::ColorEdit4(label, c, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_AlphaBar)) {
            h = tr::HueColour{uint8_t(c[0] * 255 + 0.5f), uint8_t(c[1] * 255 + 0.5f), uint8_t(c[2] * 255 + 0.5f), uint8_t(c[3] * 255 + 0.5f)};
            changed = true;
        }
        if (ImGui::IsItemActivated()) noteEdit();
    };
    hueEdit("Hue 1", e.hue1);
    ImGui::SetItemTooltip("(0, 0, 0, 255) = not hued: the materials keep their own Hue (the static post-load calls SetHue only for\n"
                          "other values). A class with a physicalentityclasshue row uses that instead, as gamemap.py does.");
    if (ctx_.map->map.minorVersion >= 45) hueEdit("Hue 2", e.hue2);
    ImGui::SetNextItemWidth(fieldWidth);
    edited(ImGui::InputInt("Flags", &e.flags, 0));
    ImGui::SetNextItemWidth(fieldWidth);
    edited(ImGui::InputInt("Cull layer", &e.cullLayer, 1));
    ImGui::SetItemTooltip("0 = the class's vizsetting layer (class: %d). Layer 6 is never drawn in game (World::AddToWorld skips the\n"
                          "graphics scene); 2-5 and 7-11 limit the draw distance (see Game distances).", ctx_.classInfo(e.classId).cullingLayer);
    if (e.hasBounds)
        ImGui::TextDisabled("Local bounds (%.1f %.1f %.1f) - (%.1f %.1f %.1f)", e.bounds.min.x, e.bounds.min.y, e.bounds.min.z,
                            e.bounds.max.x, e.bounds.max.y, e.bounds.max.z);
    if (!e.particles.empty()) {
        ImGui::SeparatorText("Particles");
        for (const tr::MapParticle& p : e.particles)
            ImGui::Text("asset %d, connection point %d, type %u", p.assetId, p.connectionPoint, p.type);
    }
    if (changed) contentChanged();
    ImGui::Spacing();
    if (ImGui::Button("Duplicate")) duplicateSelected();
    ImGui::SameLine();
    if (ImGui::Button("Delete")) deleteSelected();
    ImGui::SameLine();
    if (ImGui::Button("Show in viewport")) viewport_.focusOn(ctx_, selection_);
    ImGui::SameLine();
    if (ImGui::Button("Show in overview")) overview_.requestFocus();
    ImGui::End();
}

void ClientWorkspace::rebuildEntityFilter() {
    visibleEntities_.clear();
    if (!ctx_.map) return;
    const std::string needle = tr::toLowerAscii(entityFilter_);
    for (size_t k = 0; k < ctx_.map->map.entities.size(); ++k) {
        const tr::MapEntity& e = ctx_.map->map.entities[k];
        const ClassInfo& info = ctx_.classInfo(e.classId);
        if (needle.empty() || containsIgnoreCase(info.name, needle) || containsIgnoreCase(info.mesh, needle) ||
            std::to_string(e.classId).find(needle) != std::string::npos)
            visibleEntities_.push_back(k);
    }
}