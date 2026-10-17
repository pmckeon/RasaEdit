#pragma once

#include "Renderer.h"
#include "SceneView.h"

#include <cstdint>
#include <memory>
#include <vector>

// Rebuilds rows of a map's .vis PVS the way the game's own (unreachable) builder does - TRasa::BodyContainerAABSP::BuildPVS,
// tabula_rasa.exe FUN_0085a1d0 (decoded 2026-10-17; see CLAUDE.md ".vis PVS rebuild"):
//  - camera points per leaf (the scout, FUN_00859000): a grid over the leaf's box with the default profile "Exterior - Generic"
//    (FUN_0085e060: type 2 = about 5 m apart, at least 5 and at most 100 per axis; a leaf no object overlaps is capped at
//    4 x 3 x 4), 0.025 m in from the box; a point above solid terrain is kept when the terrain under four points 0.2 m around
//    it is more than 0.2 m below; a point under or off the terrain only inside an object's bounding sphere and when probe rays
//    in every direction hit something within 15 m (not closer than 0.0375 m); more than 1,000 candidates are shuffled (Park-Miller,
//    seeded with the leaf index) and cut to 1,000; at most 50 cameras; no camera at type 2 tries type 3 (1 m) and 4 (0.5 m);
//  - from each camera 6 views (a cube: 90 degrees, 512 x 512, near 0.025 m, far 350 m) of the bodies the game's distance rule
//    keeps; a leaf is visible when more than 26 pixels of its box pass the depth test (the occlusion queries, FUN_0085b7b0);
//  - the row: those leaves plus the leaf itself, stored with the time (TRasa::PVS::SetPVS).
// UNVERIFIED stand-ins: the probe directions (the engine's sphere builder; here an icosphere of 162 points), the collision world
// the probes hit (here the models' triangles), the occluders (every drawn opaque piece and the terrain; the builder's
// CheckAffectPVSFlag excludes bodies with a render flag 0x200 that nothing in the exe sets).
class PvsBuilder {
public:
    // The scene supplies the models, terrain and draws (its models must be loaded); vis is the tree the rows belong to.
    PvsBuilder(SDL_GPUDevice* device, SceneView& scene, const tr::MapBundle& map, const tr::VisFile& vis);
    ~PvsBuilder();

    uint32_t leafCount() const { return leafCount_; }
    int32_t leafNode(uint32_t leaf) const { return leaf < leafNodes_.size() ? leafNodes_[leaf] : -1; }
    // The body list again (after map edits) - the bodies in each leaf and near it.
    void refreshBodies();
    // The camera points the game would use for a leaf; empty when it finds none (the game then leaves the row without data).
    std::vector<tr::Vec3> cameras(uint32_t leaf) const;
    // The row for a leaf: leafCount bits; empty when the leaf has no camera point.
    std::vector<uint32_t> computeRow(uint32_t leaf);
    // The same in steps (the editor works a few views per frame): beginRow scouts (false: no camera point - no row), stepRow
    // renders up to maxViews views (true when all are done), finishRow waits for the GPU and returns the row.
    bool beginRow(uint32_t leaf);
    bool stepRow(size_t maxViews);
    std::vector<uint32_t> finishRow();
    size_t viewsLeft() const;
    // Leaves a box overlaps (closed boxes, as the game assigns bodies).
    std::vector<uint32_t> leavesOverlapping(const tr::Bounds& box) const;
    // A map entity's world box: its .map bounds (or a 1 m stand-in) placed by its transform.
    static tr::Bounds entityWorldBox(const tr::MapEntity& e);

    // Timing of the last computeRow (seconds): scouting, views.
    double lastScoutSeconds = 0, lastViewSeconds = 0;
    uint32_t lastViews = 0;
    uint32_t pixelThreshold = 26;     // a leaf is seen with more pixels than this (the game: 0x1a)

private:
    struct Body {
        uint32_t entity = 0;
        tr::Bounds box;
        tr::Vec3 centre;
        float radius = 0;
    };
    struct Candidate {
        tr::Vec3 p;
        bool aboveTerrain = false;
        float height = 0;               // above the terrain cell (aboveTerrain)
    };
    bool terrainCell(float x, float z, float& height, bool& solid) const;
    bool terrainInterpolated(float x, float z, float& height) const;
    bool insideObjectSphere(uint32_t leaf, const tr::Vec3& p) const;
    bool probesEnclosed(uint32_t leaf, const tr::Vec3& p) const;
    std::vector<tr::Vec3> scout(uint32_t leaf, int type) const;
    void collectLeaves(int32_t node, const tr::Bounds& box, std::vector<uint32_t>& out) const;

    struct RowJob {
        bool active = false;
        uint32_t leaf = 0;
        std::vector<tr::Vec3> points;
        size_t nextView = 0;
    };
    RowJob job_;
    SceneView& scene_;
    const tr::MapBundle& map_;
    const tr::VisFile& vis_;
    std::unique_ptr<Renderer> renderer_;
    uint32_t leafCount_ = 0;
    std::vector<int32_t> leafNodes_;                       // leaf index -> node
    std::vector<Body> bodies_;
    std::vector<std::vector<uint32_t>> leafBodies_;        // bodies whose box overlaps the leaf (indices into bodies_)
    // The probes' stand-in collision world: every body's model triangles in world space under a bounding volume hierarchy.
    struct Triangle {
        tr::Vec3 a, edge1, edge2;
    };
    struct BvhNode {
        tr::Bounds box;
        uint32_t first = 0;             // leaf: first triangle; inner: the right child (the left child is first - 1)
        uint32_t count = 0;             // triangles in a leaf, 0 for an inner node
    };
    std::vector<Triangle> triangles_;
    std::vector<BvhNode> bvh_;
    void buildBvh();
    bool nearestHit(const tr::Vec3& origin, const tr::Vec3& direction, float maxT, float& t) const;
    std::vector<tr::Vec3> probeDirections_;
    // Terrain grid as the game's terrain object has it (FUN_0090bc70): W x H cells from the header bounds.
    int32_t terrainW_ = 0, terrainH_ = 0;
};
