#include "ModelAnimations.h"

#include "ModelExchange.h"
#include "assetlibrary.h"
#include "gamedata.h"
#include "particles.h"

#include "imgui.h"

#include <algorithm>

namespace {
std::string lower(const std::string& s) { return tr::toLowerAscii(s); }
}  // namespace

ModelAnimationLinks AnimationLinkFinder::find(tr::AssetLibrary& assets, tr::GameData* data, const std::string& model) {
    ModelAnimationLinks links;
    links.model = model;
    const std::vector<std::string> own = modelOwnAnimations(assets, model);
    if (!own.empty()) {
        AnimationGroup g{"Its own animation", "the model's anim property (the engine plays it) or the .anm of the same name", {}, -1};
        for (const std::string& a : own) g.animations.push_back(AnimationChoice{a, 0});
        links.groups.push_back(std::move(g));
        links.kinds.push_back("Animated object: plays its own animation");
    }
    // Special-FX packages (indexed once: every .pkg's accessory events).
    if (!packagesIndexed_) {
        packagesIndexed_ = true;
        for (const std::string& name : assets.namesWithExtension(".pkg")) {
            try {
                for (const tr::SfxAccessory& a : tr::readSfxPackage(assets.read(name)).accessories)
                    if (!a.mesh.empty()) packageAccessories_[lower(a.mesh)].push_back({name, a.animation});
            } catch (const std::exception&) {
            }
        }
    }
    if (const auto it = packageAccessories_.find(lower(model)); it != packageAccessories_.end()) {
        AnimationGroup g{"Attached by special-FX packages", "", {}, -1};
        std::string packages;
        for (const auto& [package, animation] : it->second) {
            packages += (packages.empty() ? "" : ", ") + package;
            if (!animation.empty() && assets.exists(animation) &&
                std::none_of(g.animations.begin(), g.animations.end(), [&](const AnimationChoice& c) { return lower(c.name) == lower(animation); }))
                g.animations.push_back(AnimationChoice{animation, 0});
        }
        g.note = "accessory of " + packages;
        links.groups.push_back(std::move(g));
        links.kinds.push_back("Special-FX accessory (" + std::to_string(it->second.size()) + " package event(s))");
    }
    if (data) {
        std::vector<int32_t> skeletons;
        auto addSkeleton = [&](int32_t skeleton) {
            if (skeleton < 0 || std::find(skeletons.begin(), skeletons.end(), skeleton) != skeletons.end()) return;
            skeletons.push_back(skeleton);
            for (const auto& [family, name] : data->skeletonFamilies(skeleton)) {
                AnimationGroup g{name.empty() ? "family " + std::to_string(family) : name, "animation family " + std::to_string(family), {}, skeleton};
                for (const auto& a : data->familyAnimations(family, skeleton))
                    if (assets.exists(a.name)) g.animations.push_back(AnimationChoice{a.name, a.frequency});
                if (!g.animations.empty()) links.groups.push_back(std::move(g));
            }
        };
        try {
            for (int32_t meshId : data->meshStringIds(model)) {
                const int32_t skeleton = data->meshSkeletonId(meshId);
                if (skeleton >= 0) {
                    links.kinds.push_back("Creature / avatar body on skeleton " + std::to_string(skeleton) + " (" +
                                          std::to_string(data->skeletonMeshCount(skeleton)) + " models share it)");
                    addSkeleton(skeleton);
                }
                for (int32_t base : data->suitBaseMeshes(meshId)) {
                    if (base == meshId) continue;            // a body is also its own default suit
                    const int32_t baseSkeleton = data->meshSkeletonId(base);
                    links.kinds.push_back("Avatar part worn on " + data->stringById(base) +
                                          (baseSkeleton >= 0 ? " (skeleton " + std::to_string(baseSkeleton) + ")" : ""));
                    addSkeleton(baseSkeleton);
                }
                for (const auto& [family, name] : data->objectFamilies(meshId)) {
                    AnimationGroup g{(name.empty() ? "family " + std::to_string(family) : name),
                                     "object animation family " + std::to_string(family) + " of this mesh (usable objects' state animations)", {}, -1};
                    for (const auto& a : data->familyObjectAnimations(family, meshId))
                        if (assets.exists(a.name)) g.animations.push_back(AnimationChoice{a.name, a.frequency});
                    if (!g.animations.empty()) {
                        if (links.kinds.empty() || links.kinds.back().rfind("Usable object", 0) != 0) links.kinds.push_back("Usable object with state animations");
                        links.groups.push_back(std::move(g));
                    }
                }
            }
        } catch (const std::exception&) {
        }
    }
    if (links.kinds.empty()) links.kinds.push_back("Static model: the game's files tie no animation to it");
    return links;
}

void drawAnimationGroups(const ModelAnimationLinks& links, const std::function<void(const AnimationChoice&)>& row,
                         const std::function<void(const AnimationGroup&)>& groupExtra) {
    auto drawGroup = [&](const AnimationGroup& g, size_t index) {
        ImGui::PushID(int(index));
        const bool open = ImGui::TreeNodeEx("##group", g.skeleton < 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s  (%zu)", g.title.c_str(), g.animations.size());
        if (!g.note.empty()) ImGui::SetItemTooltip("%s", g.note.c_str());
        if (groupExtra) {
            ImGui::SameLine();
            groupExtra(g);
        }
        if (open) {
            for (size_t k = 0; k < g.animations.size(); ++k) {
                ImGui::PushID(int(k));
                row(g.animations[k]);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    };
    for (size_t k = 0; k < links.groups.size(); ++k)
        if (links.groups[k].skeleton < 0) drawGroup(links.groups[k], k);
    std::vector<int32_t> skeletons;
    for (const AnimationGroup& g : links.groups)
        if (g.skeleton >= 0 && std::find(skeletons.begin(), skeletons.end(), g.skeleton) == skeletons.end()) skeletons.push_back(g.skeleton);
    for (int32_t s : skeletons) {
        size_t families = 0;
        for (const AnimationGroup& g : links.groups) families += g.skeleton == s ? 1 : 0;
        ImGui::PushID(1000000 + s);
        const bool open = ImGui::TreeNode("##skeleton", "Skeleton %d animations: %zu families", s, families);
        ImGui::SetItemTooltip("animationdata.animationFamilyData: what a creature on this skeleton plays, by situation (idle, combat, ...);\n"
                              "the client picks one of a family's animations at random by weight.");
        if (open) {
            for (size_t k = 0; k < links.groups.size(); ++k)
                if (links.groups[k].skeleton == s) drawGroup(links.groups[k], k);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}
