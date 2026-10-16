#include "MarkerColours.h"
#include "Panels.h"

#include "imgui_internal.h"   // IsMouseDragPastThreshold

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

bool containsIgnoreCase(const std::string& text, const std::string& needleLower) {
    if (needleLower.empty()) return true;
    return tr::toLowerAscii(text).find(needleLower) != std::string::npos;
}

float yawDegrees(const tr::Quat& q) { return 2.0f * std::atan2(q.y, q.w) * 57.29578f; }

ImU32 hueColour(const tr::HueColour& c) { return IM_COL32(c.red, c.green, c.blue, 255); }

ImU32 toU32(const Vec4f& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w)); }

// A "Legend" label; hovering it lists what the marker colours mean (viewport and overview share them).
void drawMarkerLegend() {
    ImGui::TextDisabled("Legend");
    if (!ImGui::BeginItemTooltip()) return;
    auto row = [](const Vec4f& c, const char* text) {
        ImGui::ColorButton(text, ImVec4(c.x, c.y, c.z, 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(14, 14));
        ImGui::SameLine();
        ImGui::TextUnformatted(text);
    };
    ImGui::SeparatorText("Server (boxes, rings, overview dots)");
    row(markerColour::selected, "Selected (any kind)");
    row(markerColour::hostileCreature, "Spawn pool: creatures");
    row(markerColour::npc, "Spawn pool: NPC");
    row(markerColour::vendor, "Spawn pool: vendor");
    row(markerColour::teleporter, "Teleporter / waypoint / hospital");
    row(markerColour::object, "Object (logos, shrine, ...)");
    row(markerColour::ambientNpc, "Ambient NPC figure");
    row(markerColour::worldUsable, "World usable (switch, door, dispenser, blast)");
    row(markerColour::worldDestructible, "World destructible (barrel, pod, hive, force field)");
    row(markerColour::emitter, "FX emitter (a package played at a spot)");
    row(markerColour::arrival, "Spawn arrival (dropship pad / Bane teleporter), line to its pool");
    row(markerColour::patrol, "Patrol step, lines along the pool's circuit");
    row(markerColour::mapLink, "Map link");
    row(markerColour::mapLinkDisabled, "Map link (disabled)");
    row(markerColour::mapScreenMarker, "Client map-screen marker, line to its bound object");
    row(markerColour::mapScreenMarkerUnbound, "Client map-screen marker, nothing bound");
    row(markerColour::missionArea, "Mission area");
    row(markerColour::region, "Region");
    row(markerColour::regionUnderground, "Region (underground)");
    ImGui::SeparatorText("Map entities");
    row(markerColour::placeholder, "Box: no model, or an effect (Placeholders)");
    row(markerColour::light, "Small box: a light of the map's lights.xml (Light markers)");
    row(markerColour::entityModel, "Overview dot: entity with a model");
    row(markerColour::entityNoMesh, "Overview dot: class without a mesh");
    row(markerColour::entityUnknown, "Overview dot: unknown class");
    ImGui::TextDisabled("Rings mark a marker's position / radius;\nclick a model, a box edge or a ring to select.");
    ImGui::EndTooltip();
}

// ------------------------------------------------------------------------------------------
// Game, Maps, Map info, Assets
// ------------------------------------------------------------------------------------------
void drawGamePanel(EditorContext& ctx, const char* title) {
    if (!ImGui::Begin(title)) { ImGui::End(); return; }
    if (!ctx.assets) {
        ImGui::TextWrapped("No game folder open. Use File > Open Game Folder... and choose the Tabula Rasa install "
                           "(the folder containing tabula_rasa.cfg).");
        if (ImGui::Button("Open Game Folder...")) ctx.showGameFolderDialog();
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", ctx.gameRoot.c_str());
    size_t entries = 0;
    for (const auto& a : ctx.assets->archives()) entries += a.archive->entries().size();
    ImGui::Text("%zu archives, %zu entries; %zu names indexed", ctx.assets->archives().size(), entries, ctx.assets->indexedCount());
    ImGui::Text("%zu maps", ctx.mapNames.size());
    ImGui::Text("Game data: %s", ctx.gameData ? "loaded" : "not found");
    if (!ctx.assets->warnings().empty() && ImGui::CollapsingHeader("Warnings")) {
        for (const auto& w : ctx.assets->warnings()) ImGui::TextWrapped("%s", w.c_str());
    }
    if (ImGui::CollapsingHeader("Archives")) {
        for (const auto& a : ctx.assets->archives())
            ImGui::Text("%-14s %6zu  %s", a.configKey.c_str(), a.archive->entries().size(),
                        fs::path(a.archive->path()).filename().string().c_str());
    }
    ImGui::End();
}
void MapsPanel::draw(EditorContext& ctx, const char* title) {
    if (!ImGui::Begin(title)) { ImGui::End(); return; }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##mapfilter", "filter maps", filter_, sizeof filter_);
    const std::string needle = tr::toLowerAscii(filter_);
    if (ImGui::BeginChild("maplist")) {
        for (const std::string& name : ctx.mapNames) {
            if (!containsIgnoreCase(name, needle)) continue;
            if (ImGui::Selectable(name.c_str(), name == ctx.currentMapName)) ctx.queueMap(name);                     // loaded before the next frame
        }
    }
    ImGui::EndChild();
    ImGui::End();
}
void drawMapInfoPanel(EditorContext& ctx, const char* title) {
    if (!ImGui::Begin(title)) { ImGui::End(); return; }
    if (!ctx.map) { ImGui::TextDisabled("No map loaded."); ImGui::End(); return; }
    const tr::MapFile& m = ctx.map->map;
    ImGui::Text("%s", ctx.map->name.c_str());
    if (ctx.mapTemplateId >= 0) ImGui::Text("Template %d (0x%X) %s", ctx.mapTemplateId, ctx.mapTemplateId, ctx.mapTemplateName.c_str());
    ImGui::Text("Format %u.%u, template version %d, status %d", m.majorVersion, m.minorVersion, m.templateVersion, m.status);
    ImGui::Text("%zu entities, %zu regions, %zu collision volumes", m.entities.size(), m.regions.size(), m.generatedAbvs.size());
    ImGui::Text("Day length %d s, start sky time %d", m.dayLengthSeconds, m.startingSkyTime);
    if (ctx.map->hasTerrain) {
        const tr::TerrainHeader& h = ctx.map->terrain.header;
        ImGui::SeparatorText("Terrain");
        ImGui::Text("Bounds x %.0f..%.0f, z %.0f..%.0f, height +/-%.0f", h.bounds.min.x, h.bounds.max.x, h.bounds.min.z, h.bounds.max.z, h.bounds.max.y);
        ImGui::Text("%ux%u patches, %zu texture layers, %zu clutter sets", h.patchesX, h.patchesZ, ctx.map->textureLayers.size(), ctx.map->tribbleSets.size());
    } else {
        ImGui::TextDisabled("No terrain (interior-only map)");
    }
    ImGui::SeparatorText("Visibility");
    ImGui::Text("%zu BSP nodes, PVS for %u leaves", ctx.map->vis.nodes.size(), ctx.map->vis.leafCount);
    ImGui::SeparatorText("Files");
    ImGui::Text("lights.xml %zu bytes, audioemitters.xml %zu bytes", ctx.map->lightsXml.size(), ctx.map->audioEmittersXml.size());
    ImGui::TextWrapped("Radar: %s", ctx.map->radarArchivePath.empty() ? "(none)" : ctx.map->radarArchivePath.c_str());
    if (!ctx.radarRegions.empty()) {
        std::string regions;
        for (int32_t id : ctx.radarRegions) regions += (regions.empty() ? "" : "; ") + ctx.regionLabel(id);
        ImGui::TextWrapped("Radar maps (context %d): %s", ctx.radarContextId, regions.c_str());
    }
    ImGui::End();
}

void AssetsPanel::rebuild(EditorContext& ctx) {
    visible_.clear();
    const std::string needle = tr::toLowerAscii(filter_);
    for (size_t k = 0; k < names_.size(); ++k)
        if (containsIgnoreCase(names_[k], needle)) visible_.push_back(k);
    selected_ = -1;
    preview_.select(ctx, "");
}
void AssetsPanel::draw(EditorContext& ctx, const char* title) {
    if (!ImGui::Begin(title)) { ImGui::End(); return; }
    if (!ctx.assets) { ImGui::TextDisabled("No game folder open."); ImGui::End(); return; }
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputTextWithHint("##assetfilter", "search asset names", filter_, sizeof filter_)) rebuild(ctx);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu", visible_.size(), names_.size());
    const float detailsWidth = ImGui::GetContentRegionAvail().x * 0.55f;
    if (ImGui::BeginChild("assetlist", ImVec2(ImGui::GetContentRegionAvail().x - detailsWidth, 0), ImGuiChildFlags_Borders)) {
        ImGuiListClipper clipper;
        clipper.Begin(int(visible_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const size_t index = visible_[size_t(row)];
                if (ImGui::Selectable(names_[index].c_str(), selected_ == int(index)) && selected_ != int(index)) {
                    selected_ = int(index);
                    preview_.select(ctx, names_[index]);
                }
            }
        }
        // Up / down keys step through the list while it has focus.
        if (ImGui::IsWindowFocused() && !visible_.empty()) {
            int step = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) step = -1;
            if (step != 0) {
                const auto at = std::find(visible_.begin(), visible_.end(), size_t(selected_));
                const int row = at == visible_.end() ? (step > 0 ? 0 : int(visible_.size()) - 1)
                                                     : std::clamp(int(at - visible_.begin()) + step, 0, int(visible_.size()) - 1);
                selected_ = int(visible_[size_t(row)]);
                preview_.select(ctx, names_[size_t(selected_)]);
                ImGui::SetScrollY(std::max(0.0f, row * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetWindowHeight() * 0.5f));
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("assetdetails", ImVec2(0, 0), ImGuiChildFlags_Borders)) preview_.draw(ctx);
    ImGui::EndChild();
    ImGui::End();
}
void AssetsPanel::showAsset(EditorContext& ctx, const std::string& name) {
    names_ = ctx.assets ? ctx.assets->namesWithExtension("") : std::vector<std::string>();
    filter_[0] = 0;
    rebuild(ctx);
    for (size_t k = 0; k < names_.size(); ++k)
        if (tr::toLowerAscii(names_[k]) == tr::toLowerAscii(name)) {
            selected_ = int(k);
            preview_.release();                         // a cached model of that name would be the old one
            preview_.select(ctx, names_[k]);
        }
}

void AssetsPanel::reset(EditorContext& ctx) {
    names_ = ctx.assets ? ctx.assets->namesWithExtension("") : std::vector<std::string>();
    preview_.release();                             // its scene refers to the previous asset library
    rebuild(ctx);
}

// ------------------------------------------------------------------------------------------
// Overview
// ------------------------------------------------------------------------------------------
void OverviewPanel::onMapLoaded(const EditorContext& ctx) {
    fitPending_ = true;
    radarChoice_ = -1;
    backgroundMode_ = ctx.radarRegions.empty() ? 1 : 0;
}

void OverviewPanel::draw(EditorContext& ctx, const char* title, const OverviewLayers& layers) {
    if (!ImGui::Begin(title)) { ImGui::End(); return; }
    if (!ctx.map) { ImGui::TextDisabled("Select a map."); ImGui::End(); return; }
    const auto& entities = ctx.map->map.entities;

    // Background choice. The radar region follows the camera as the game's radar does, or is chosen by hand (sub-regions -
    // caves, interiors - have their own radar maps, often over the same ground as the main map).
    const bool hasRadar = !ctx.radarRegions.empty();
    const int32_t autoRegion = ctx.radarRegionAtCamera();
    if (radarChoice_ >= 0 && std::find(ctx.radarRegions.begin(), ctx.radarRegions.end(), radarChoice_) == ctx.radarRegions.end()) radarChoice_ = -1;
    const int32_t radarRegion = radarChoice_ >= 0 ? radarChoice_ : autoRegion;
    RadarMap* radar = backgroundMode_ == 0 && radarRegion >= 0 ? ctx.radarFor(radarRegion) : nullptr;
    if (hasRadar || ctx.overview) {
        if (!hasRadar) ImGui::BeginDisabled();
        if (ImGui::RadioButton("Radar map", backgroundMode_ == 0)) backgroundMode_ = 0;
        if (hasRadar && ctx.radarRegions.size() > 1) {
            ImGui::SameLine();
            const std::string preview = radarChoice_ < 0 ? "Auto: " + ctx.regionLabel(autoRegion) : ctx.regionLabel(radarChoice_);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            if (ImGui::BeginCombo("##radarregion", preview.c_str())) {
                if (ImGui::Selectable("Auto: follow the camera", radarChoice_ < 0)) radarChoice_ = -1;
                for (int32_t id : ctx.radarRegions) {
                    const std::string label = ctx.regionLabel(id) + (id == 0 ? " (main map)" : "");
                    if (ImGui::Selectable(label.c_str(), radarChoice_ == id)) {
                        radarChoice_ = id;
                        backgroundMode_ = 0;
                        fitPending_ = true;                  // show the whole chosen map
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Which region's radar map to show. Auto follows the 3D camera like the game's radar: of the regions\n"
                                  "the camera is in (the server's map_region volumes) and region 0, the highest region id with radar\n"
                                  "tiles. Pick a region to see a cave or interior from anywhere; double-click a marker there to jump\n"
                                  "the 3D view to it.");
        }
        if (!hasRadar) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!ctx.overview) ImGui::BeginDisabled();
        if (ImGui::RadioButton("Terrain relief", backgroundMode_ == 1)) backgroundMode_ = 1;
        if (!ctx.overview) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit")) fitPending_ = true;
        ImGui::SameLine();
        drawMarkerLegend();
    }
    const bool showRadar = backgroundMode_ == 0 && radar;
    const bool showRelief = backgroundMode_ == 1 && ctx.overview;

    // World rectangle to fit: the background image, or the entities' extent.
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    if (showRadar) {
        minX = radar->leftX(); maxX = radar->rightX(); minZ = radar->bottomZ(); maxZ = radar->topZ();
    } else if (ctx.overview) {
        minX = ctx.overview->minX(); maxX = ctx.overview->maxX(); minZ = ctx.overview->minZ(); maxZ = ctx.overview->maxZ();
    } else {
        for (const auto& e : entities) {
            minX = std::min(minX, e.location.x); maxX = std::max(maxX, e.location.x);
            minZ = std::min(minZ, e.location.z); maxZ = std::max(maxZ, e.location.z);
        }
        if (minX > maxX) { minX = -10; maxX = 10; minZ = -10; maxZ = 10; }
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 50.0f);
    size.y = std::max(size.y, 50.0f);
    ImGui::InvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 centre(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);

    if (fitPending_) {
        fitPending_ = false;
        centreX_ = (minX + maxX) * 0.5f;
        centreZ_ = (minZ + maxZ) * 0.5f;
        zoom_ = 0.95f * std::min(size.x / std::max(maxX - minX, 1.0f), size.y / std::max(maxZ - minZ, 1.0f));
    }
    tr::Vec3 selectedPosition;
    const bool hasSelectedPosition = layers.selection && layers.position && isSelected(*layers.selection) &&
                                     layers.position(*layers.selection, selectedPosition);
    if (focusPending_) {
        focusPending_ = false;
        if (hasSelectedPosition) {
            centreX_ = selectedPosition.x;
            centreZ_ = selectedPosition.z;
            zoom_ = std::max(zoom_, 4.0f);
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    auto toScreen = [&](float x, float z) { return ImVec2(centre.x + (x - centreX_) * zoom_, centre.y - (z - centreZ_) * zoom_); };
    const float mouseX = centreX_ + (io.MousePos.x - centre.x) / zoom_;
    const float mouseZ = centreZ_ - (io.MousePos.y - centre.y) / zoom_;

    // Pan with the right or middle button, zoom about the cursor with the wheel.
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        centreX_ -= io.MouseDelta.x / zoom_;
        centreZ_ += io.MouseDelta.y / zoom_;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        zoom_ = std::clamp(zoom_ * std::pow(1.2f, io.MouseWheel), 0.01f, 200.0f);
        centreX_ = mouseX - (io.MousePos.x - centre.x) / zoom_;
        centreZ_ = mouseZ + (io.MousePos.y - centre.y) / zoom_;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 corner(origin.x + size.x, origin.y + size.y);
    draw->PushClipRect(origin, corner, true);
    draw->AddRectFilled(origin, corner, IM_COL32(24, 26, 30, 255));
    if (showRadar) {
        // Radar image row 0 is the top (largest z), so it maps straight onto the screen.
        draw->AddImage(ImTextureRef((ImTextureID)(intptr_t)radar->texture()), toScreen(radar->leftX(), radar->topZ()),
                       toScreen(radar->rightX(), radar->bottomZ()));
    }
    if (showRelief) {
        // Image row 0 is minimum z, so the top of the drawn rectangle (maximum z) uses v = 1.
        draw->AddImage(ImTextureRef((ImTextureID)(intptr_t)ctx.overview->texture()), toScreen(ctx.overview->minX(), ctx.overview->maxZ()),
                       toScreen(ctx.overview->maxX(), ctx.overview->minZ()), ImVec2(0, 1), ImVec2(1, 0));
    }

    // Map entities; find the one nearest the cursor for hover and click.
    int nearest = -1;
    float nearestDistance = 8.0f * 8.0f;
    const float radius = std::clamp(zoom_ * 0.6f, 1.5f, 5.0f);
    for (size_t k = 0; k < entities.size(); ++k) {
        const ImVec2 p = toScreen(entities[k].location.x, entities[k].location.z);
        if (p.x < origin.x - 5 || p.y < origin.y - 5 || p.x > corner.x + 5 || p.y > corner.y + 5) continue;
        const ClassInfo& info = ctx.classInfo(entities[k].classId);
        ImU32 colour = toU32(!info.known ? markerColour::entityUnknown : info.mesh.empty() ? markerColour::entityNoMesh : markerColour::entityModel);
        if (layers.dimEntities) colour = IM_COL32(170, 170, 170, 90);
        draw->AddCircleFilled(p, layers.dimEntities ? radius * 0.7f : radius, colour, 8);
        if (hovered && layers.selectEntities) {
            const float dx = p.x - io.MousePos.x, dy = p.y - io.MousePos.y;
            if (dx * dx + dy * dy < nearestDistance) { nearestDistance = dx * dx + dy * dy; nearest = int(k); }
        }
    }

    // Server objects: spawn pools (green NPC, gold vendor, red creature), teleporters (cyan),
    // objects (yellow), map links (magenta), mission areas (orange), regions (green).
    Selection nearestServer;
    float nearestServerDistance = 8.0f * 8.0f;
    const tr::MapScreenMarker* hoveredMarker = nullptr;      // a client map-screen marker under the cursor (tooltip only)
    auto consider = [&](const ImVec2& p, const Selection& s) {
        if (!hovered || !layers.selectServer) return;
        const float dx = p.x - io.MousePos.x, dy = p.y - io.MousePos.y;
        if (dx * dx + dy * dy < nearestServerDistance) { nearestServerDistance = dx * dx + dy * dy; nearestServer = s; }
    };
    if (layers.server) {
        const WorldContent& server = *layers.server;
        const WorldDatabase* database = layers.database;
        if (layers.showServer) {
            for (size_t k = 0; k < server.spawnPools.size(); ++k) {
                const SpawnPool& pool = server.spawnPools[k];
                // A control point garrison of the side its point does not show (the inspector's "Garrison shown"), as in 3D.
                if (ctx.scene && ctx.scene->spawnPoolHidden(k) &&
                    !(layers.selection && sameSelection(*layers.selection, Selection{SelectionKind::SpawnPool, k})))
                    continue;
                const Creature* creature = database ? database->creature(pool.slot1.creatureId) : nullptr;
                const bool npc = creature && database->classIsNpc(creature->classId), vendor = creature && database->classIsVendor(creature->classId);
                const ImU32 colour = toU32(vendor ? markerColour::vendor : npc ? markerColour::npc : markerColour::hostileCreature);
                const ImVec2 p = toScreen(pool.position.x, pool.position.z);
                draw->AddCircleFilled(p, radius + 1.0f, colour, 10);
                if (pool.radius * zoom_ > 6.0f) draw->AddCircle(p, pool.radius * zoom_, colour & 0x80FFFFFF, 32);
                consider(p, Selection{SelectionKind::SpawnPool, k});
            }
            for (size_t k = 0; k < server.teleporters.size(); ++k) {
                const ImVec2 p = toScreen(server.teleporters[k].position.x, server.teleporters[k].position.z);
                draw->AddRectFilled(ImVec2(p.x - 4, p.y - 4), ImVec2(p.x + 4, p.y + 4), toU32(markerColour::teleporter));
                consider(p, Selection{SelectionKind::Teleporter, k});
            }
            for (size_t k = 0; k < server.objects.size(); ++k) {
                const ImVec2 p = toScreen(server.objects[k].position.x, server.objects[k].position.z);
                draw->AddQuadFilled(ImVec2(p.x, p.y - 5), ImVec2(p.x + 5, p.y), ImVec2(p.x, p.y + 5), ImVec2(p.x - 5, p.y),
                                    toU32(markerColour::ofObject(server.objects[k].kind)));
                consider(p, Selection{SelectionKind::Object, k});
            }
            for (size_t k = 0; k < server.arrivals.size(); ++k) {
                const SpawnArrival& a = server.arrivals[k];
                const ImVec2 p = toScreen(a.position.x, a.position.z);
                for (const SpawnPool& pool : server.spawnPools)
                    if (pool.id == a.poolId) draw->AddLine(p, toScreen(pool.position.x, pool.position.z), toU32(markerColour::arrival) & 0xA0FFFFFF, 1.0f);
                draw->AddTriangleFilled(ImVec2(p.x, p.y - 5), ImVec2(p.x + 5, p.y + 4), ImVec2(p.x - 5, p.y + 4), toU32(markerColour::arrival));
                consider(p, Selection{SelectionKind::Arrival, k});
            }
            for (size_t k = 0; k < server.patrolSteps.size(); ++k) {
                const PatrolStep& s = server.patrolSteps[k];
                const ImVec2 p = toScreen(s.position.x, s.position.z);
                size_t next = k + 1;
                if (next >= server.patrolSteps.size() || server.patrolSteps[next].poolId != s.poolId) {
                    next = k;
                    while (next > 0 && server.patrolSteps[next - 1].poolId == s.poolId) --next;
                }
                if (next != k)
                    draw->AddLine(p, toScreen(server.patrolSteps[next].position.x, server.patrolSteps[next].position.z),
                                  toU32(markerColour::patrol) & 0xC0FFFFFF, 1.5f);
                draw->AddCircleFilled(p, 3.5f, toU32(markerColour::patrol), 8);
                consider(p, Selection{SelectionKind::PatrolStep, k});
            }
            // The client's map-screen markers: a ring, a line to the object bound to it (map_marker, or a control point's
            // marker_entity_id); the tooltip names the marker and its binding.
            if (layers.clientMarkers && ctx.scene) {
                float hoveredDistance = 7.0f * 7.0f;
                for (const tr::MapScreenMarker& m : *layers.clientMarkers) {
                    const ImVec2 p = toScreen(m.x, m.z);
                    tr::Vec3 target;
                    const bool bound = ctx.scene->clientMarkerTarget(m.entityId, target);
                    const ImU32 colour = toU32(bound ? markerColour::mapScreenMarker : markerColour::mapScreenMarkerUnbound);
                    if (bound) draw->AddLine(p, toScreen(target.x, target.z), colour & 0xA0FFFFFF, 1.0f);
                    draw->AddCircle(p, 6.0f, colour, 16, 2.0f);
                    if (hovered) {
                        const float dx = p.x - io.MousePos.x, dy = p.y - io.MousePos.y;
                        if (dx * dx + dy * dy < hoveredDistance) {
                            hoveredDistance = dx * dx + dy * dy;
                            hoveredMarker = &m;
                        }
                    }
                }
            }
            for (size_t k = 0; k < server.mapLinks.size(); ++k) {
                const MapLink& l = server.mapLinks[k];
                const ImVec2 p = toScreen(l.position.x, l.position.z);
                draw->AddCircle(p, std::max(5.0f, l.radius * zoom_), toU32(l.enabled ? markerColour::mapLink : markerColour::mapLinkDisabled), 24, 2.0f);
                consider(p, Selection{SelectionKind::MapLink, k});
            }
        }
        auto drawArea = [&](const AreaShape& a, ImU32 colour, const Selection& s) {
            const ImVec2 c = toScreen(a.position.x, a.position.z);
            if (a.shape == 2) draw->AddRect(toScreen(a.position.x - a.halfX, a.position.z + a.halfZ), toScreen(a.position.x + a.halfX, a.position.z - a.halfZ), colour, 0, 0, 1.5f);
            else draw->AddCircle(c, a.radius * zoom_, colour, 48, 1.5f);
            draw->AddCircleFilled(c, 3.0f, colour);
            consider(c, s);
        };
        if (layers.showMissionAreas)
            for (size_t k = 0; k < server.missionAreas.size(); ++k)
                drawArea(server.missionAreas[k].area, toU32(markerColour::missionArea), Selection{SelectionKind::MissionArea, k});
        if (layers.showRegions)
            for (size_t k = 0; k < server.regions.size(); ++k)
                drawArea(server.regions[k].area, toU32(server.regions[k].underground == 1 ? markerColour::regionUnderground : markerColour::region), Selection{SelectionKind::Region, k});
    }

    if (hasSelectedPosition) {
        const ImVec2 p = toScreen(selectedPosition.x, selectedPosition.z);
        draw->AddCircle(p, radius + 4.0f, toU32(markerColour::selected), 16, 2.0f);
    }
    // The 3D camera: a white arrowhead pointing where it looks.
    if (ctx.scene) {
        const ViewCamera& camera = ctx.scene->camera();
        const tr::Vec3 f = camera.forward();
        const float fl = std::sqrt(f.x * f.x + f.z * f.z);
        const float dx = fl > 1e-4f ? f.x / fl : 0.0f, dz = fl > 1e-4f ? f.z / fl : 1.0f;
        const ImVec2 p = toScreen(camera.position.x, camera.position.z);
        const ImVec2 tip(p.x + dx * 11.0f, p.y - dz * 11.0f);
        const ImVec2 left(p.x - dz * 5.0f - dx * 4.0f, p.y - dx * 5.0f + dz * 4.0f), right(p.x + dz * 5.0f - dx * 4.0f, p.y + dx * 5.0f + dz * 4.0f);
        draw->AddTriangleFilled(tip, left, right, IM_COL32(255, 255, 255, 230));
        draw->AddTriangle(tip, left, right, IM_COL32(0, 0, 0, 200), 1.0f);
    }
    // Cursor read-out.
    char text[160];
    float ground = 0;
    if (ctx.map->hasTerrain && ctx.map->terrain.heightAtWorld(mouseX, mouseZ, ground))
        std::snprintf(text, sizeof text, "x %.1f  z %.1f  ground %.1f", mouseX, mouseZ, ground);
    else
        std::snprintf(text, sizeof text, "x %.1f  z %.1f", mouseX, mouseZ);
    draw->AddText(ImVec2(origin.x + 6, origin.y + 4), IM_COL32(255, 255, 255, 220), text);
    draw->PopClipRect();

    // Hover and click: server markers take precedence over map entities under the cursor. A double-click selects and jumps the
    // 3D view to the marker; on empty map space it moves the camera there (looking at the ground, or the map's height without
    // terrain).
    const bool clicked = hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left);
    const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (isSelected(nearestServer)) {
        nearest = -1;
        if (layers.label) ImGui::SetTooltip("%s", layers.label(nearestServer).c_str());
        if (clicked && layers.selection) *layers.selection = nearestServer;
        if (doubleClicked && layers.selection) {
            *layers.selection = nearestServer;
            if (layers.showInViewport) layers.showInViewport(nearestServer);
        }
    } else if (hoveredMarker && ctx.scene) {
        tr::Vec3 target;
        const bool bound = ctx.scene->clientMarkerTarget(hoveredMarker->entityId, target);
        ImGui::SetTooltip("Map-screen marker: %s [%s]\nentity %lld at (%.1f, %.1f, %.1f)\n%s", hoveredMarker->name.c_str(),
                          tr::mapScreenMarkerTypeName(hoveredMarker->type), static_cast<long long>(hoveredMarker->entityId), hoveredMarker->x,
                          hoveredMarker->y, hoveredMarker->z, bound ? "bound to a server object (line)" : "nothing bound: the server sends no state for it");
    } else if (nearest >= 0) {
        const ClassInfo& info = ctx.classInfo(entities[size_t(nearest)].classId);
        ImGui::SetTooltip("%s\n%s\n%.1f, %.1f, %.1f", info.known ? info.name.c_str() : "(unknown class)", info.mesh.c_str(),
                          entities[size_t(nearest)].location.x, entities[size_t(nearest)].location.y, entities[size_t(nearest)].location.z);
        const Selection entity{SelectionKind::MapEntity, size_t(nearest)};
        if (clicked && layers.selection) *layers.selection = entity;
        if (doubleClicked && layers.selection) {
            *layers.selection = entity;
            if (layers.showInViewport) layers.showInViewport(entity);
        }
    } else if (doubleClicked && ctx.scene) {
        const float fallback = ctx.scene->camera().position.y;
        float y = fallback;
        if (!(ctx.map->hasTerrain && ctx.map->terrain.heightAtWorld(mouseX, mouseZ, y))) y = fallback;
        ctx.scene->focusOnPoint(tr::Vec3{mouseX, y, mouseZ}, 25.0f);
    } else if (clicked && layers.selection) {
        *layers.selection = Selection{};                   // click on empty map space deselects
    }
    ImGui::End();
}

// ------------------------------------------------------------------------------------------
// Viewport
// ------------------------------------------------------------------------------------------
void ViewportPanel::applyOptions(const ViewSettings& view, SceneView& scene) const {
    scene.showTextures = view.showTextures;
    scene.showTerrain = view.showTerrain;
    scene.showEntities = view.showEntities;
    scene.showEffects = view.showEffects;
    scene.showParticles = view.showParticles;
    scene.showClutter = view.showClutter;
    scene.gameDistances = view.gameDistances;
    scene.animate = view.animate;
    scene.showPlaceholders = view.showPlaceholders;
    scene.showLightMarkers = view.lightMarkers && !serverControls_;      // selectable only where map entities are
    scene.showServer = options.showServer;
    scene.showMissionAreas = options.showMissionAreas;
    scene.showRegions = options.showRegions;
    scene.drawDistance = view.drawDistance;
    scene.skyLighting = view.skyLighting;
    scene.showSkyDome = view.skyDome;
    scene.showFog = view.fog;
    scene.showMapLights = view.mapLights;
    scene.showSun = view.sun;
    scene.showShadows = view.shadows;
}

void ViewportPanel::focusOn(EditorContext& ctx, const Selection& selection) {
    if (ctx.scene && isSelected(selection)) ctx.scene->focusOn(selection);
    raiseWindow_ = true;
}

void ViewportPanel::draw(EditorContext& ctx, const char* title, Selection& selection, bool pickEntities, bool pickServer, ViewportEditHooks* hooks) {
    visible_ = false;
    if (raiseWindow_) {
        ImGui::SetNextWindowFocus();
        raiseWindow_ = false;
    }
    if (!ImGui::Begin(title)) { ImGui::End(); return; }
    if (!ctx.renderer) { ImGui::TextDisabled("3D rendering is unavailable (see the status bar)."); ImGui::End(); return; }
    if (!ctx.scene) { ImGui::TextDisabled("Select a map."); ImGui::End(); return; }
    SceneView& scene = *ctx.scene;
    const bool editing = hooks && hooks->editable();

    // Toolbar.
    ImGui::Checkbox("Textures", &ctx.view.showTextures);
    ImGui::SameLine();
    ImGui::Checkbox("Terrain", &ctx.view.showTerrain);
    ImGui::SameLine();
    ImGui::Checkbox(serverControls_ ? "Map entities" : "Entities", &ctx.view.showEntities);
    ImGui::SameLine();
    ImGui::Checkbox("Effects", &ctx.view.showEffects);
    ImGui::SameLine();
    ImGui::Checkbox("Particles", &ctx.view.showParticles);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Particle systems (.pfx) on models' contact points, within %.0f m: %zu running, %zu particles", scene.particleDistance,
                          scene.liveParticleSystems(), scene.liveParticles());
    ImGui::SameLine();
    ImGui::Checkbox("Animation", &ctx.view.animate);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Play the world's animation: moving objects (holograms, rotating lights, water), creatures' idles, light\n"
                          "flicker and particles. Off stops everything where it is.");
    ImGui::SameLine();
    ImGui::Checkbox("Clutter", &ctx.view.showClutter);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Terrain clutter (grass, ferns, flowers, debris) from the terrain's tribble sets, placed as the game does,\n"
                          "in the 7 x 7 cells of 32 m around the camera: %zu instances, %zu drawn", scene.clutterInstances(), scene.drawnClutter());
    ImGui::SameLine();
    ImGui::Checkbox("Game distances", &ctx.view.gameDistances);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hide map entities beyond the game's draw distance for their culling layer (layer 3: 100 m, layer 2: 300 m,\n"
                          "at most 250 m), as tabula_rasa.exe does at View Distance 100 and full quality. Off: everything within the\n"
                          "editor's draw distance.");
    ImGui::SameLine();
    ImGui::Checkbox("Placeholders", &ctx.view.showPlaceholders);
    if (!serverControls_) {
        ImGui::SameLine();
        ImGui::Checkbox("Light markers", &ctx.view.lightMarkers);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The map's lights (lights.xml) as small white boxes: click one to select it; the selected light shows\n"
                              "its reach (point: range circles, spot: its axis, plane: the box it lights). Lights on models and\n"
                              "special-FX packages are not listed.");
    }
    if (serverControls_) {
        ImGui::SameLine();
        ImGui::Checkbox("Server", &options.showServer);
        ImGui::SameLine();
        ImGui::Checkbox("Mission areas", &options.showMissionAreas);
        ImGui::SameLine();
        ImGui::Checkbox("Regions", &options.showRegions);
        if (hooks) {
            ImGui::SameLine();
            ImGui::Checkbox("Snap to ground", &hooks->snapToGround());
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    ImGui::SliderFloat("##distance", &ctx.view.drawDistance, 100.0f, 4000.0f, "draw %.0f m");
    ImGui::SameLine();
    const bool canFocus = isSelected(selection);
    if (!canFocus) ImGui::BeginDisabled();
    if (ImGui::Button("Focus (F)")) scene.focusOn(selection);
    if (!canFocus) ImGui::EndDisabled();

    // Second row: sky lighting (the region's .sky), its time of day, fog and the map's placed lights.
    ImGui::Checkbox("Sky", &ctx.view.skyLighting);
    ImGui::SameLine();
    ImGui::Checkbox("Dome", &ctx.view.skyDome);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The sky's layer models, as the game draws them behind everything: dome, sun and moon, stars, clouds,\n"
                          "distant mountains. Rotated and coloured by the time of day.");
    ImGui::SameLine();
    if (!ctx.view.skyLighting || scene.skies().empty()) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    const auto skyLabel = [&](size_t k) {
        const SceneView::SkyChoice& c = scene.skies()[k];
        std::string regions;
        for (int32_t r : c.regions) regions += (regions.empty() ? "" : ", ") + std::to_string(r);
        return c.name + "  (region " + regions + ")" + (c.loaded ? "" : "  - not usable");
    };
    const std::string comboLabel = scene.skies().empty() ? std::string("no region skies")
                                 : scene.skyFromRegion() ? "by region: " + skyLabel(std::min(scene.activeSkyIndex(), scene.skies().size() - 1))
                                                         : skyLabel(std::min(scene.skyIndex, scene.skies().size() - 1));
    if (ImGui::BeginCombo("##sky", comboLabel.c_str())) {
        if (ImGui::Selectable("By region (server regions at the camera)", scene.skyByRegion)) scene.skyByRegion = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("As the game: the highest-priority region the camera is in that has a sky.\nNeeds the server's map_region volumes (a loaded World database).");
        for (size_t k = 0; k < scene.skies().size(); ++k) {
            if (ImGui::Selectable(skyLabel(k).c_str(), !scene.skyByRegion && k == scene.skyIndex)) {
                scene.skyIndex = k;
                scene.skyByRegion = false;
            }
            if (!scene.skies()[k].loaded && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", scene.skies()[k].error.c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    float hours = scene.timeOfDay * 24.0f;
    const int minutes = int(hours * 60.0f + 0.5f) % (24 * 60);
    char timeText[16];
    std::snprintf(timeText, sizeof timeText, "%02d:%02d", minutes / 60, minutes % 60);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::SliderFloat("##time", &hours, 0.0f, 24.0f, timeText)) scene.timeOfDay = std::clamp(hours / 24.0f, 0.0f, 1.0f);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) scene.timeOfDay = scene.startTimeOfDay();
    if (ImGui::IsItemHovered()) {
        const int start = int(scene.startTimeOfDay() * 24.0f * 60.0f + 0.5f) % (24 * 60);
        ImGui::SetTooltip("Time of day; the map starts at %02d:%02d (right-click to reset)", start / 60, start % 60);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Fog", &ctx.view.fog);
    ImGui::SameLine();
    ImGui::Checkbox("Sun", &ctx.view.sun);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The sky's sun or moon (caves get their own sky by region).");
    ImGui::SameLine();
    ImGui::Checkbox("Shadows", &ctx.view.shadows);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sun shadows within 120 m of the camera; like the game they block 64%% of the sun.");
    if (!ctx.view.skyLighting || scene.skies().empty()) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Lights", &ctx.view.mapLights);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The map's placed lights (lights.xml): %zu, up to 16 nearest the camera are used%s%s", scene.mapLights().size(),
                          scene.mapLightsError().empty() ? "" : "\n", scene.mapLightsError().c_str());
    ImGui::SameLine();
    drawMarkerLegend();
    applyOptions(ctx.view, scene);

    // Canvas: an invisible button for input, the render target drawn underneath it.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 32.0f);
    size.y = std::max(size.y, 32.0f);
    ImGuiIO& io = ImGui::GetIO();
    const float scale = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
    ctx.renderer->resize(uint32_t(size.x * scale), uint32_t(size.y * scale));   // before the image is referenced
    ImGui::InvisibleButton("viewport", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddImage(ImTextureRef((ImTextureID)(intptr_t)ctx.renderer->colourTarget()), origin, ImVec2(origin.x + size.x, origin.y + size.y));
    visible_ = true;

    // Camera. W/A/S/D move and Q/E lower/raise whenever the viewport has keyboard focus (click into it);
    // Shift is x4. Hold the right button to look around (the wheel then sets the speed). Middle-drag
    // pans; the wheel alone moves forward and back. Escape clears the selection.
    ViewCamera& cam = scene.camera();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput;
    const bool looking = active && ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (looking) {
        cam.yaw += io.MouseDelta.x * 0.005f;
        cam.pitch = std::clamp(cam.pitch - io.MouseDelta.y * 0.005f, -1.55f, 1.55f);
        if (io.MouseWheel != 0.0f) ctx.view.flySpeed = std::clamp(ctx.view.flySpeed * std::pow(1.25f, io.MouseWheel), 2.0f, 2000.0f);
    } else if (hovered && io.MouseWheel != 0.0f && !io.KeyCtrl) {
        cam.position = add(cam.position, scaled(cam.forward(), io.MouseWheel * ctx.view.flySpeed * 0.1f));   // 2 m a notch at 20 m/s
    }
    if ((focused || looking) && !io.KeyCtrl) {
        const float step = ctx.view.flySpeed * io.DeltaTime * (io.KeyShift ? 4.0f : 1.0f);
        tr::Vec3 move{0, 0, 0};
        if (ImGui::IsKeyDown(ImGuiKey_W)) move = add(move, cam.forward());
        if (ImGui::IsKeyDown(ImGuiKey_S)) move = subtract(move, cam.forward());
        if (ImGui::IsKeyDown(ImGuiKey_D)) move = add(move, cam.right());
        if (ImGui::IsKeyDown(ImGuiKey_A)) move = subtract(move, cam.right());
        if (ImGui::IsKeyDown(ImGuiKey_E)) move = add(move, tr::Vec3{0, 1, 0});
        if (ImGui::IsKeyDown(ImGuiKey_Q)) move = subtract(move, tr::Vec3{0, 1, 0});
        if (length(move) > 0) cam.position = add(cam.position, scaled(normalized(move), step));
        if (ImGui::IsKeyPressed(ImGuiKey_F) && isSelected(selection)) scene.focusOn(selection);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) selection = Selection{};
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const float panScale = ctx.view.flySpeed * 0.01f;
        cam.position = add(cam.position, add(scaled(cam.right(), -io.MouseDelta.x * panScale), scaled(cam.up(), io.MouseDelta.y * panScale)));
    }

    // Editing (workspaces that provide hooks): drag the selection along the ground; Ctrl+wheel rotates
    // it (15 degrees a notch, 1 with Shift); Delete removes it.
    const float mouseNdcX = (io.MousePos.x - origin.x) / size.x * 2.0f - 1.0f;
    const float mouseNdcY = 1.0f - (io.MousePos.y - origin.y) / size.y * 2.0f;
    if (editing) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            dragCandidate_ = isSelected(selection) && sameSelection(scene.pick(mouseNdcX, mouseNdcY, pickEntities, pickServer), selection) &&
                             hooks->selectedPosition() != nullptr;
        if (dragCandidate_ && active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            tr::Vec3 rayOrigin, rayDirection, hit;
            scene.viewRay(mouseNdcX, mouseNdcY, rayOrigin, rayDirection);
            tr::Vec3* position = hooks->selectedPosition();
            const bool snap = hooks->snapToGround();
            // Snapping follows the first surface under the cursor - architecture or terrain - not counting a dragged map entity.
            const int dragged = selection.kind == SelectionKind::MapEntity ? int(selection.index) : -1;
            auto groundHit = [&](float planeY, tr::Vec3& point) {
                if (snap) return scene.surfacePoint(rayOrigin, rayDirection, planeY, point, dragged);
                if (std::fabs(rayDirection.y) < 1e-6f) return false;               // keep the height: horizontal plane
                const float t = (planeY - rayOrigin.y) / rayDirection.y;
                if (t <= 0) return false;
                point = add(rayOrigin, scaled(rayDirection, t));
                return true;
            };
            if (position && !dragging_) {
                if (groundHit(position->y, hit)) {
                    hooks->beginEdit();
                    dragging_ = true;
                    dragOffset_ = tr::Vec3{position->x - hit.x, 0, position->z - hit.z};
                    dragY_ = position->y;
                }
            } else if (position && groundHit(dragY_, hit)) {
                position->x = hit.x + dragOffset_.x;
                position->z = hit.z + dragOffset_.z;
                position->y = snap ? scene.surfaceHeightBelow(position->x, position->z, hit.y + 2.0f, dragY_, dragged) : dragY_;
                hooks->contentChanged();
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            dragging_ = false;
            dragCandidate_ = false;
        }
        if (hovered && io.KeyCtrl && io.MouseWheel != 0.0f) {
            if (float* rotation = hooks->selectedRotation()) {
                hooks->beginEdit();
                *rotation += io.MouseWheel * (io.KeyShift ? 1.0f : 15.0f) / 57.29578f;
                hooks->contentChanged();
            }
        }
        if (focused && ImGui::IsKeyPressed(ImGuiKey_Delete)) hooks->deleteSelected();
    }

    // Left click (without dragging) picks; empty space deselects.
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left))
        selection = scene.pick(mouseNdcX, mouseNdcY, pickEntities, pickServer);

    // Status overlay.
    char text[256];
    std::snprintf(text, sizeof text, "%.1f, %.1f, %.1f   speed %.0f m/s   models %zu/%zu (%zu failed)   textures %zu (%zu failed)   entities drawn %zu",
                  cam.position.x, cam.position.y, cam.position.z, ctx.view.flySpeed, scene.modelsLoaded(), scene.modelsTotal(), scene.modelsFailed(),
                  scene.texturesLoaded(), scene.textureErrors().size(), scene.drawnEntities());
    const ImVec2 statusEnd(origin.x + ImGui::CalcTextSize(text).x + 12, origin.y + ImGui::GetFontSize() + 6);
    draw->AddRectFilled(ImVec2(origin.x, origin.y), statusEnd, IM_COL32(0, 0, 0, 140));
    draw->AddText(ImVec2(origin.x + 6, origin.y + 3), scene.textureErrors().empty() ? IM_COL32(255, 255, 255, 230) : IM_COL32(255, 210, 120, 255), text);
    // Hovering the status line lists the textures that failed and why.
    if (!scene.textureErrors().empty() && ImGui::IsMouseHoveringRect(origin, statusEnd) && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("Textures that could not be used (drawn untextured):");
        size_t shown = 0;
        for (const auto& e : scene.textureErrors()) {
            if (++shown > 30) { ImGui::TextDisabled("... and %zu more", scene.textureErrors().size() - 30); break; }
            ImGui::Text("%s: %s", e.first.c_str(), e.second.c_str());
        }
        ImGui::EndTooltip();
    }
    const char* hint = !focused ? "Click in the viewport to use the keyboard (WASD, Q/E)"
                       : editing ? "WASD/QE move  RMB look  MMB pan  F focus  Esc deselect  |  drag: move  Ctrl+wheel: rotate  Del: delete  Ctrl+D: duplicate"
                                 : "WASD move  Q/E down/up  Shift fast  RMB look  wheel move (RMB+wheel: speed)  MMB pan  F focus  Esc deselect";
    const float hintY = origin.y + size.y - ImGui::GetFontSize() - 6;
    draw->AddRectFilled(ImVec2(origin.x, hintY - 3), ImVec2(origin.x + ImGui::CalcTextSize(hint).x + 12, origin.y + size.y), IM_COL32(0, 0, 0, 140));
    draw->AddText(ImVec2(origin.x + 6, hintY), IM_COL32(255, 255, 255, 200), hint);
    ImGui::End();
}

void ViewportPanel::render(EditorContext& ctx, SDL_GPUCommandBuffer* commands, const Selection& selection) {
    if (!visible_ || !ctx.scene || !ctx.renderer) return;
    applyOptions(ctx.view, *ctx.scene);
    ctx.scene->advanceParticles(ImGui::GetIO().DeltaTime);
    ctx.scene->advanceLights(ImGui::GetIO().DeltaTime);          // light modifiers (flicker, rotating globes)
    ctx.scene->queueDraws(selection);
    ctx.renderer->record(commands);
}
