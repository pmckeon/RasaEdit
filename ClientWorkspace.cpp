#include "ClientWorkspace.h"

#include "imgui_internal.h"   // DockBuilder

#include <cstdio>

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
    ImGui::DockBuilderDockWindow("Assets###Client.Assets", bottom);
    ImGui::DockBuilderDockWindow("Entities###Client.Entities", bottom);
    ImGui::DockBuilderDockWindow("Lights###Client.Lights", bottom);
    ImGui::DockBuilderDockWindow("Map###Client.Map", right);
    ImGui::DockBuilderDockWindow("Entity###Client.Entity", right);
    ImGui::DockBuilderFinish(dockspace);
}

void ClientWorkspace::onGameOpened() {
    selection_ = Selection{};
    visibleEntities_.clear();
    assets_.reset(ctx_);
}

void ClientWorkspace::onMapLoaded() {
    selection_ = Selection{};
    rebuildEntityFilter();
    overview_.onMapLoaded(ctx_);
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
    assets_.draw(ctx_, "Assets###Client.Assets");
    // Drawn last: on first creation the newest window becomes the selected tab of a shared dock node.
    viewport_.draw(ctx_, "Viewport###Client.Viewport", selection_, true, false, nullptr);
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
    const tr::PlacedLight& light = ctx_.scene->mapLights()[index];
    ImGui::Text("Light  %s", light.name.empty() ? "(unnamed)" : light.name.c_str());
    ImGui::Text("Kind   %s%s%s", lightKindName(light.kind), light.enabled ? "" : ", disabled", light.castsShadows ? ", casts shadows" : "");
    ImGui::Text("ID     %llu", static_cast<unsigned long long>(light.id));
    ImGui::SeparatorText("Placement");
    ImGui::Text("Position     %.2f, %.2f, %.2f", light.position.x, light.position.y, light.position.z);
    const tr::Quat& q = light.worldOrientation;
    ImGui::Text("Orientation  x %.3f  y %.3f  z %.3f  w %.3f", q.x, q.y, q.z, q.w);
    if (light.kind == tr::PlacedLight::Kind::Spot) {
        const tr::Vec3 d = normalized(light.direction());
        ImGui::Text("Axis         %.3f, %.3f, %.3f", d.x, d.y, d.z);
    }
    ImGui::SeparatorText("Light");
    ImGui::ColorButton("##colour", ImVec4(light.colour.x, light.colour.y, light.colour.z, 1), ImGuiColorEditFlags_NoTooltip);
    ImGui::SameLine();
    ImGui::Text("Colour  %.3f, %.3f, %.3f   intensity %.2f", light.colour.x, light.colour.y, light.colour.z, light.intensity);
    ImGui::Text("Range %.2f m, falloff %.0f %%", light.range, light.falloffPercent * 100.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Full strength to (1 - FalloffPercent) x Range, then a linear fade to the range\n"
                          "(LightFalloff = (1 - FalloffPercent) x LightRange, tabula_rasa.exe FUN_00875360 / FUN_00879150).");
    if (light.kind == tr::PlacedLight::Kind::Spot) ImGui::Text("Cone %.1f deg, penumbra %.1f deg (full angles)", light.coneAngle, light.penumbraAngle);
    if (light.kind == tr::PlacedLight::Kind::Plane)
        ImGui::Text("Width %.2f, length %.2f; falloff %.2f %.2f %.2f", light.width, light.length, light.falloffPercentVec.x,
                    light.falloffPercentVec.y, light.falloffPercentVec.z);
    ImGui::Text("Globe map  %s", light.globeMap.empty() ? "(none)" : light.globeMap.c_str());
    const size_t tracks = ctx_.scene->mapLightModifierTracks(light);
    if (tracks > 0) {
        const tr::PlacedLight now = ctx_.scene->displayedMapLight(index);
        ImGui::Text("Modifiers  %zu track(s); now: colour %.2f %.2f %.2f, intensity %.2f", tracks, now.colour.x, now.colour.y,
                    now.colour.z, now.intensity);
    } else {
        ImGui::TextDisabled("Modifiers  none");
    }
    ImGui::Spacing();
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
        ImGui::End();
        return;
    }
    const tr::MapEntity& e = ctx_.map->map.entities[size_t(selectedEntity())];
    const ClassInfo& info = ctx_.classInfo(e.classId);
    ImGui::Text("Class %d  %s", e.classId, info.known ? info.name.c_str() : "(unknown class)");
    ImGui::Text("Mesh  %s", info.mesh.empty() ? "(none)" : info.mesh.c_str());
    if (!info.mesh.empty() && ctx_.assets && !ctx_.assets->exists(info.mesh))
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "mesh file not found in the archives");
    if (ctx_.scene) ImGui::TextWrapped("Viewport: %s", ctx_.scene->modelStatus(size_t(selectedEntity())).c_str());
    ImGui::Text("Entity id %llu (high %u, low %u)", static_cast<unsigned long long>(e.entityId), tr::entityIdHigh(e.entityId),
                tr::entityIdLow(e.entityId));
    ImGui::SeparatorText("Placement");
    ImGui::Text("Location  %.2f, %.2f, %.2f", e.location.x, e.location.y, e.location.z);
    ImGui::Text("Rotation  w %.3f  x %.3f  y %.3f  z %.3f", e.orientation.w, e.orientation.x, e.orientation.y, e.orientation.z);
    ImGui::Text("Yaw       %.1f deg (if upright)", yawDegrees(e.orientation));
    ImGui::Text("Scale     %.3f", e.scale);
    if (ctx_.map->hasTerrain) {
        float ground = 0;
        if (ctx_.map->terrain.heightAtWorld(e.location.x, e.location.z, ground))
            ImGui::Text("Terrain   %.2f (entity %+.2f)", ground, e.location.y - ground);
    }
    ImGui::SeparatorText("Appearance");
    ImGui::ColorButton("##hue1", ImGui::ColorConvertU32ToFloat4(hueColour(e.hue1)), ImGuiColorEditFlags_NoTooltip);
    ImGui::SameLine(); ImGui::Text("Hue 1  %02X%02X%02X%02X", e.hue1.red, e.hue1.green, e.hue1.blue, e.hue1.alpha);
    ImGui::ColorButton("##hue2", ImGui::ColorConvertU32ToFloat4(hueColour(e.hue2)), ImGuiColorEditFlags_NoTooltip);
    ImGui::SameLine(); ImGui::Text("Hue 2  %02X%02X%02X%02X", e.hue2.red, e.hue2.green, e.hue2.blue, e.hue2.alpha);
    ImGui::Text("Flags %d, cull layer %d (class %d)", e.flags, e.cullLayer, ctx_.classInfo(e.classId).cullingLayer);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The map's cull layer (0 = use the class's vizsetting layer). Layer 6 is never drawn in game:\n"
                          "World::AddToWorld does not add such bodies to the graphics scene.");
    if (e.hasBounds)
        ImGui::Text("Local bounds (%.1f %.1f %.1f) - (%.1f %.1f %.1f)", e.bounds.min.x, e.bounds.min.y, e.bounds.min.z,
                    e.bounds.max.x, e.bounds.max.y, e.bounds.max.z);
    if (!e.particles.empty()) {
        ImGui::SeparatorText("Particles");
        for (const tr::MapParticle& p : e.particles)
            ImGui::Text("asset %d, connection point %d, type %u", p.assetId, p.connectionPoint, p.type);
    }
    ImGui::Spacing();
    if (ImGui::Button("Show in overview")) overview_.requestFocus();
    ImGui::SameLine();
    if (ImGui::Button("Show in viewport")) viewport_.focusOn(ctx_, selection_);
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