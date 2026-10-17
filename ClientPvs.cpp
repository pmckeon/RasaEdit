// The Client workspace's PVS panel: keeping the map's .vis rows right after map edits (see PvsBuilder.h, CLAUDE.md ".vis PVS
// rebuild"). The game culls an object when none of the leaves its box overlaps is in the camera leaf's row, and a row without data
// means every leaf is visible (TRasa::PVS::LoadPVS fills it with 0xFF). Moving or adding ordinary objects needs nothing; what goes
// stale is what the rows learned about occluders - an occluder that moved, shrank or went away leaves rows that still hide what it
// used to hide. Those rows are the ones that see a leaf its old box overlaps (a leaf that never saw the occluder cannot see past it).
#include "ClientWorkspace.h"

#include <imgui.h>

#include <algorithm>
#include <ctime>
#include <set>

namespace {

bool sameEntity(const tr::MapEntity& a, const tr::MapEntity& b) {
    return a.classId == b.classId && a.location.x == b.location.x && a.location.y == b.location.y && a.location.z == b.location.z &&
           a.orientation.x == b.orientation.x && a.orientation.y == b.orientation.y && a.orientation.z == b.orientation.z &&
           a.orientation.w == b.orientation.w && a.scale == b.scale && a.cullLayer == b.cullLayer && a.hasBounds == b.hasBounds &&
           a.bounds.min.x == b.bounds.min.x && a.bounds.min.y == b.bounds.min.y && a.bounds.min.z == b.bounds.min.z &&
           a.bounds.max.x == b.bounds.max.x && a.bounds.max.y == b.bounds.max.y && a.bounds.max.z == b.bounds.max.z;
}

bool rowSeesAny(const tr::VisPvsRow& row, const std::vector<uint32_t>& leaves) {
    for (uint32_t leaf : leaves)
        if ((leaf >> 5) < row.words.size() && tr::pvsLeafVisible(row, leaf)) return true;
    return false;
}

}  // namespace

std::vector<uint32_t> ClientWorkspace::pvsAffectedRows() const {
    if (!ctx_.map || !ctx_.map->vis.hasPvs || !pvsBuilder_) return {};
    const tr::MapBundle& map = *ctx_.map;
    // Changed, removed and added entities by id.
    std::vector<tr::Bounds> boxes;
    std::set<uint64_t> current;
    for (const tr::MapEntity& e : map.map.entities) current.insert(e.entityId);
    for (const tr::MapEntity& old : pvsBaseline_) {
        const auto now = std::find_if(map.map.entities.begin(), map.map.entities.end(), [&](const tr::MapEntity& e) { return e.entityId == old.entityId; });
        if (now != map.map.entities.end() && sameEntity(old, *now)) continue;
        boxes.push_back(PvsBuilder::entityWorldBox(old));                                     // what it may have hidden
        if (pvsIncludeNewBoxes_ && now != map.map.entities.end()) boxes.push_back(PvsBuilder::entityWorldBox(*now));
    }
    if (pvsIncludeNewBoxes_) {
        std::set<uint64_t> before;
        for (const tr::MapEntity& e : pvsBaseline_) before.insert(e.entityId);
        for (const tr::MapEntity& e : map.map.entities)
            if (!before.count(e.entityId)) boxes.push_back(PvsBuilder::entityWorldBox(e));
    }
    std::vector<uint32_t> leaves;
    for (const tr::Bounds& b : boxes) {
        const std::vector<uint32_t> l = pvsBuilder_->leavesOverlapping(b);
        leaves.insert(leaves.end(), l.begin(), l.end());
    }
    std::sort(leaves.begin(), leaves.end());
    leaves.erase(std::unique(leaves.begin(), leaves.end()), leaves.end());
    std::vector<uint32_t> rows;
    if (leaves.empty()) return rows;
    for (uint32_t r = 0; r < map.vis.rows.size(); ++r)
        if (tr::pvsHasData(map.vis.rows[r]) && rowSeesAny(map.vis.rows[r], leaves)) rows.push_back(r);
    return rows;
}

void ClientWorkspace::pvsClearRows(const std::vector<uint32_t>& rows) {
    if (!ctx_.map) return;
    for (uint32_t r : rows) tr::clearPvsRow(ctx_.map->vis, r);
    if (!rows.empty()) pvsChanged_ = true;
    pvsBaseline_ = ctx_.map->map.entities;
    pvsResult_ = std::to_string(rows.size()) + " row(s) cleared: the game draws everything from those leaves (correct, slower there).";
}

void ClientWorkspace::pvsStartRebuild(const std::vector<uint32_t>& rows) {
    if (!pvsBuilder_ || !ctx_.map) return;
    pvsBuilder_->refreshBodies();                 // the current entities are the occluders
    pvsQueue_.assign(rows.rbegin(), rows.rend());  // popped from the back: lowest first
    pvsQueueTotal_ = rows.size();
    pvsRowStarted_ = false;
    pvsRebuildBaseline_ = ctx_.map->map.entities;
    pvsResult_.clear();
}

void ClientWorkspace::pvsStep() {
    if (!pvsBuilder_ || !ctx_.map || pvsQueue_.empty()) return;
    tr::VisFile& vis = ctx_.map->vis;
    size_t budget = pvsViewsPerFrame_;
    while (budget > 0 && !pvsQueue_.empty()) {
        const uint32_t leaf = pvsQueue_.back();
        if (!pvsRowStarted_) {
            if (!pvsBuilder_->beginRow(leaf)) {
                // No camera point in this leaf: the game leaves such a row without data (every leaf visible) - kept as it is.
                pvsQueue_.pop_back();
                continue;
            }
            pvsRowStarted_ = true;
        }
        const size_t before = pvsBuilder_->viewsLeft();
        const bool done = pvsBuilder_->stepRow(budget);
        budget -= std::min(budget, before - pvsBuilder_->viewsLeft());
        if (!done) break;
        std::vector<uint32_t> row = pvsBuilder_->finishRow();
        pvsRowStarted_ = false;
        pvsQueue_.pop_back();
        if (row.empty()) continue;
        if (pvsMerge_ && leaf < vis.rows.size() && tr::pvsHasData(vis.rows[leaf]))
            for (size_t w = 0; w < row.size() && w < vis.rows[leaf].words.size(); ++w) row[w] |= vis.rows[leaf].words[w];
        tr::setPvsRow(vis, leaf, row, uint64_t(std::time(nullptr)));
        pvsChanged_ = true;
    }
    if (pvsQueue_.empty()) {
        pvsBaseline_ = pvsRebuildBaseline_;
        pvsResult_ = std::to_string(pvsQueueTotal_) + " row(s) rebuilt" + (pvsMerge_ ? " (merged into the existing rows)" : "") + ".";
        pvsQueueTotal_ = 0;
    }
}

void ClientWorkspace::drawPvsPanel() {
    if (!ImGui::Begin("PVS###Client.Pvs")) {
        ImGui::End();
        return;
    }
    if (!ctx_.map || !ctx_.scene) {
        ImGui::TextDisabled("No map open.");
        ImGui::End();
        return;
    }
    const tr::VisFile& vis = ctx_.map->vis;
    if (!vis.hasPvs || !vis.hasTree) {
        ImGui::TextDisabled("This map's .vis has no PVS.");
        ImGui::End();
        return;
    }
    size_t withData = 0;
    for (const tr::VisPvsRow& r : vis.rows) withData += tr::pvsHasData(r) ? 1 : 0;
    ImGui::Text("%u leaves, %zu rows with data (the rest: everything visible)", vis.leafCount, withData);
    const bool loaded = ctx_.scene->modelsLoaded() + ctx_.scene->modelsFailed() >= ctx_.scene->modelsTotal();
    if (!loaded) {
        ImGui::TextDisabled("Waiting for the map's models to load (%zu of %zu)...", ctx_.scene->modelsLoaded(), ctx_.scene->modelsTotal());
        ImGui::End();
        return;
    }
    if (!pvsBuilder_) pvsBuilder_ = std::make_unique<PvsBuilder>(ctx_.device, *ctx_.scene, *ctx_.map, ctx_.map->vis);

    ImGui::TextWrapped("Moving or adding ordinary objects needs nothing here. When something that blocks the view (a wall, a building, a "
                       "large rock) is moved, shrunk or removed, rows of the leaves that could see it may still hide what it used to hide.");
    ImGui::Checkbox("Also rows that see where entities are now", &pvsIncludeNewBoxes_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rows seeing a new or moved occluder could get tighter (fewer leaves drawn) - optional.");
    const std::vector<uint32_t> affected = pvsAffectedRows();
    ImGui::Text("Rows affected by edits: %zu", affected.size());
    const bool running = !pvsQueue_.empty();
    ImGui::BeginDisabled(running || affected.empty());
    if (ImGui::Button("Rebuild affected rows")) pvsStartRebuild(affected);
    ImGui::SameLine();
    if (ImGui::Button("Clear affected rows")) pvsClearRows(affected);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Instant and always safe: those leaves draw everything (the game's rule for a row without data).");
    ImGui::Checkbox("Merge into the existing rows", &pvsMerge_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The rebuilt row is OR-ed with the shipped one, so no leaf the game saw before is lost.\n"
                          "The builder approximates the game's (on sampled rows it finds 80 - 90 %% of the shipped bits).");
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Rebuild every row with data")) {
        std::vector<uint32_t> all;
        for (uint32_t r = 0; r < vis.rows.size(); ++r)
            if (tr::pvsHasData(vis.rows[r])) all.push_back(r);
        pvsStartRebuild(all);
    }
    ImGui::EndDisabled();
    if (running) {
        const float done = pvsQueueTotal_ ? float(pvsQueueTotal_ - pvsQueue_.size()) / float(pvsQueueTotal_) : 1.0f;
        ImGui::ProgressBar(done, ImVec2(-1, 0), (std::to_string(pvsQueueTotal_ - pvsQueue_.size()) + " / " + std::to_string(pvsQueueTotal_)).c_str());
        if (ImGui::Button("Stop")) {
            if (pvsRowStarted_) pvsBuilder_->finishRow();
            pvsRowStarted_ = false;
            pvsQueue_.clear();
            pvsResult_ = "Stopped; the rows done so far are kept.";
            pvsQueueTotal_ = 0;
        }
    }
    if (!pvsResult_.empty()) ImGui::TextWrapped("%s", pvsResult_.c_str());
    if (pvsChanged_) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "The .vis has unsaved changes (saved with the map, Ctrl+S).");
    ImGui::End();
}
