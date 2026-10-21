#pragma once

// How the game ties animations to a model - from the files' own links, never by matching names (CLAUDE.md "Context per model
// type"): the model's `anim` property (or X.anm), special-FX packages whose accessory event names the mesh with an animation,
// object animation families of the mesh (animationFamilyObject: usable objects' state animations), the skeleton the mesh is on
// (meshSkeleton -> animationFamilyData: creature and avatar animations by family) and, for a suit part (clothing, hair, face), the
// skeleton of each body it is worn on (equipmentdata). Used by the Assets workspace's Blender panel (what to export) and preview
// (what to play).

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tr {
class AssetLibrary;
class GameData;
}

struct AnimationChoice {
    std::string name;                            // .anm
    int32_t frequency = 0;                       // a family's random-pick weight (0: none)
};

struct AnimationGroup {
    std::string title;
    std::string note;                            // where the link comes from
    std::vector<AnimationChoice> animations;
    int32_t skeleton = -1;                       // a skeleton family (drawn under its skeleton's node)
};

struct ModelAnimationLinks {
    std::string model;
    std::vector<std::string> kinds;              // what the model is, e.g. "Creature / avatar body on skeleton 1 (45 models share it)"
    std::vector<AnimationGroup> groups;
};

// A model that plays an animation, and why (the same links, the other way round).
struct ModelUse {
    std::string model;                           // .geo
    std::string why;                             // e.g. "skeleton 1, Idle - Non Combat"
};

class AnimationLinkFinder {
public:
    ModelAnimationLinks find(tr::AssetLibrary& assets, tr::GameData* data, const std::string& model);
    // The models an animation plays on: models whose `anim` property names it (complete once modelIndexDone()), special-FX
    // packages' accessories, object families and the meshes of skeletons whose families hold it (only meshes in the archives).
    std::vector<ModelUse> modelsPlaying(tr::AssetLibrary& assets, tr::GameData* data, const std::string& animation);
    // Every model's `anim` property, read a few models at a time (call each frame until done; budget in milliseconds).
    void indexModels(tr::AssetLibrary& assets, double budgetMs);
    bool modelIndexDone() const { return modelsIndexed_; }
    size_t modelsRead() const { return modelCursor_; }
    size_t modelsToRead() const { return modelQueue_.size(); }
    void reset();                                // another game

private:
    void indexPackages(tr::AssetLibrary& assets);
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> packageAccessories_;   // mesh -> (package, animation)
    bool packagesIndexed_ = false;
    std::unordered_map<std::string, std::vector<std::string>> animProperty_;   // animation (lower case) -> models naming it
    std::vector<std::string> modelQueue_;
    size_t modelCursor_ = 0;
    bool modelsIndexed_ = false;
};

// The groups as a tree: groups of their own first (open), then one node per skeleton holding its families. row draws one
// animation (a checkbox, a selectable); groupExtra, when given, draws after a group's title (e.g. an "all" button).
void drawAnimationGroups(const ModelAnimationLinks& links, const std::function<void(const AnimationChoice&)>& row,
                         const std::function<void(const AnimationGroup&)>& groupExtra = {});
