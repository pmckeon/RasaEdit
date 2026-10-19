// The Client workspace's collision checks: the entity panel's verdict and the Collision panel's map-wide problem list (Collision.h).

#include "ClientWorkspace.h"
#include "Collision.h"
#include "MarkerColours.h"

#include "imgui.h"

const ModelCollision& ClientWorkspace::collisionOf(const std::string& mesh) {
    auto it = collisionCache_.find(mesh);
    if (it != collisionCache_.end()) return it->second;
    ModelCollision c;
    if (ctx_.assets && !mesh.empty() && mesh != "default.geo") {
        try {
            c = modelCollision(tr::readGeoFile(ctx_.assets->read(mesh)));
        } catch (const std::exception&) {
        }
    }
    return collisionCache_.emplace(mesh, c).first->second;
}

void ClientWorkspace::drawEntityCollision(size_t index) {
    const tr::MapEntity& e = ctx_.map->map.entities[index];
    const ClassInfo& info = ctx_.classInfo(e.classId);
    ImGui::SeparatorText("Collision");
    if (info.mesh.empty() || info.mesh == "default.geo") {
        ImGui::TextDisabled("No model: the map's generated collision (if any) stands in.");
        return;
    }
    const ModelCollision& c = collisionOf(info.mesh);
    const std::string problem = collisionProblem(c, e.flags);
    const std::string verdict = collisionVerdict(c, e.flags);
    if (!problem.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.35f, 1.0f));
    ImGui::TextWrapped("%s", verdict.c_str());
    if (!problem.empty()) ImGui::PopStyleColor();
    if (c.boxes + c.spheres && problem.empty())
        ImGui::TextDisabled("Also %zu box / sphere volume(s), which map objects do not collide with.", c.boxes + c.spheres);
    ImGui::TextDisabled("Layers > Collision shows it in the viewport.");
}

void ClientWorkspace::checkCollision() {
    collisionProblems_.clear();
    collisionChecked_ = true;
    collisionEntitiesWith_ = 0;
    if (!ctx_.map) return;
    for (size_t k = 0; k < ctx_.map->map.entities.size(); ++k) {
        const tr::MapEntity& e = ctx_.map->map.entities[k];
        const ClassInfo& info = ctx_.classInfo(e.classId);
        if (info.mesh.empty() || info.mesh == "default.geo") continue;
        const ModelCollision& c = collisionOf(info.mesh);
        if (c.usedTriangles + c.droppedTriangles + c.boxes + c.spheres) ++collisionEntitiesWith_;
        const std::string problem = collisionProblem(c, e.flags);
        if (!problem.empty()) collisionProblems_.push_back(CollisionProblem{k, info.mesh, problem});
    }
}

void ClientWorkspace::drawCollisionPanel() {
    if (!ImGui::Begin("Collision###Client.Collision")) {
        ImGui::End();
        return;
    }
    if (!ctx_.map) {
        ImGui::TextDisabled("No map open.");
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Map entities whose collision the game drops or ignores: a walkable surface on a bone without a WSName, "
                       "surfaces turned off by the entity's map flags, or only box / sphere volumes (which the player does not "
                       "collide with on map objects).");
    if (ImGui::Button(collisionChecked_ ? "Check again" : "Check the map's entities")) {
        collisionCache_.clear();                      // models may have changed (imports, the patch archive)
        checkCollision();
    }
    if (collisionChecked_) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu entities with collision, %zu problem(s)", collisionEntitiesWith_, collisionProblems_.size());
        if (collisionProblems_.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "No problems: every entity's collision is used as it is.");
        } else if (ImGui::BeginTable("##collisionProblems", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Entity", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 16);
            ImGui::TableSetupColumn("Problem");
            ImGui::TableHeadersRow();
            for (const CollisionProblem& p : collisionProblems_) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = selection_.kind == SelectionKind::MapEntity && selection_.index == p.entity;
                ImGui::PushID(int(p.entity));
                if (ImGui::Selectable(p.mesh.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    selection_ = Selection{SelectionKind::MapEntity, p.entity};
                    viewport_.focusOn(ctx_, selection_);
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", p.problem.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}
