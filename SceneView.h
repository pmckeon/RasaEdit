#pragma once

#include "Avatar.h"
#include "Renderer.h"
#include "assetlibrary.h"
#include "WorldDatabase.h"

#include <deque>
#include <map>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Fly camera in the game's left-handed space. yaw 0 looks along +z, positive yaw turns towards +x;
// positive pitch looks up.
struct ViewCamera {
    tr::Vec3 position;
    float yaw = 0;                 // radians
    float pitch = -0.5f;           // radians
    float fovY = 1.0f;             // radians (about 57 degrees)
    float nearZ = 0.5f;
    float farZ = 4000.0f;

    tr::Vec3 forward() const;
    tr::Vec3 right() const;        // cross(world up, forward)
    tr::Vec3 up() const;           // cross(forward, right)
    Mat4 viewMatrix() const;
    Mat4 projectionMatrix(float aspect) const;
};

// What is selected: a map entity (.map file) or a server object (World database).
enum class SelectionKind : uint8_t { None, MapEntity, SpawnPool, Teleporter, MapLink, Region, MissionArea, Object };

struct Selection {
    SelectionKind kind = SelectionKind::None;
    size_t index = 0;                   // into MapFile::entities or the matching WorldContent vector
};
inline bool isSelected(const Selection& s) { return s.kind != SelectionKind::None; }
inline bool sameSelection(const Selection& a, const Selection& b) { return a.kind == b.kind && (a.kind == SelectionKind::None || a.index == b.index); }

// Everything the viewport draws for one map.
class SceneView {
public:
    // meshForClass returns the .geo name for an entity class ("" if none / unknown);
    // classKnown says whether the class exists in the game data.
    SceneView(Renderer& renderer, const tr::AssetLibrary& assets, const tr::MapBundle& map,
              std::function<std::string(int32_t)> meshForClass, std::function<bool(int32_t)> classKnown, uint32_t terrainStep = 2);
    ~SceneView();
    SceneView(const SceneView&) = delete;
    SceneView& operator=(const SceneView&) = delete;

    // Loads queued models for up to budgetSeconds (call between frames).
    void loadPending(double budgetSeconds);
    size_t modelsLoaded() const { return modelsLoaded_; }
    size_t texturesLoaded() const;
    // Textures that could not be used, with the reason (missing from the archives, unsupported format...).
    const std::map<std::string, std::string>& textureErrors() const { return textureErrors_; }
    size_t modelsFailed() const { return modelsFailed_; }
    size_t modelsTotal() const { return models_.size(); }

    // Server content (World database) for this map; nullptr removes it. The content and database
    // must outlive the scene or be replaced first. avatarFor (optional) assembles swap-set creatures
    // from their creature_appearance items; without it creatures draw as their class's bare mesh.
    using AvatarResolver = std::function<AvatarLook(const Creature&)>;
    void setServerContent(const WorldContent* content, const WorldDatabase* database, std::function<std::string(int32_t)> meshForClass,
                          AvatarResolver avatarFor = nullptr);
    // Rebuilds the server placements after the content was edited (same content object).
    void refreshServerContent();

    // World-space ray through a point of the viewport (normalised device coordinates, +y up).
    void viewRay(float ndcX, float ndcY, tr::Vec3& origin, tr::Vec3& direction) const;
    // Where a ray meets the ground: the terrain if there is one, else the horizontal plane y = fallbackY.
    bool groundPoint(const tr::Vec3& origin, const tr::Vec3& direction, float fallbackY, tr::Vec3& point) const;
    // Terrain height at (x, z), or fallback where there is no terrain.
    float groundHeight(float x, float z, float fallback) const;

    // Queues this frame's draws on the renderer (aspect from the renderer's target size).
    void queueDraws(const Selection& selection);
    // What is under a point of the viewport, in normalised device coordinates (-1..1, +y up).
    // pickEntities / pickServer choose which kinds of object can be hit.
    Selection pick(float ndcX, float ndcY, bool pickEntities = true, bool pickServer = true) const;
    void focusOn(const Selection& selection);

    ViewCamera& camera() { return camera_; }
    float drawDistance = 1200.0f;
    bool showTextures = true;
    bool showTerrain = true;
    bool showEntities = true;
    bool showPlaceholders = true;
    bool showEffects = true;
    bool showServer = true;             // spawn pools, teleporters, links, objects
    bool showRegions = false;           // map regions (large outlines)
    bool showMissionAreas = true;
    bool dimMapEntities = false;        // draw the static map greyed out (context only)
    // Why an entity draws the way it does: "model", "water", "effect (no geometry)", "failed: ...", etc.
    std::string modelStatus(size_t entityIndex) const;
    size_t drawnEntities() const { return drawnEntities_; }

private:
    struct ModelPiece {
        GpuMesh mesh;
        SDL_GPUTexture* texture = nullptr;     // owned by textures_; nullptr = untextured
        bool alphaTest = false;
        bool water = false;                    // TRWater* effect: drawn as a translucent surface
        bool hued = false;                     // "Hue" effect: a colour is added, weighted by the diffuse alpha
        bool flesh = false;                    // swap group "flesh": hued with the skin colour
        uint32_t swapId = 0;                   // avatar swap set (0: not swappable)
    };
    struct Model {
        std::vector<ModelPiece> pieces;
        std::vector<tr::Vec3> positions;        // CPU copy of all pieces (model space) for picking
        std::vector<uint32_t> triangles;
        tr::Bounds bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 2, 0.5f}};   // from the geometry once loaded
        bool loaded = false;
        bool failed = false;
        bool effectOnly = false;               // every piece is an additive effect quad (particles drawn by the game)
        bool hasWater = false;
        std::string failure;
    };
    struct Placement {
        Mat4 world;                 // entity placement
        Mat4 boxWorld;              // placement * local bounds (for placeholders and selection)
        tr::Vec3 centre;            // world-space bounding sphere
        float radius = 1;
        Model* model = nullptr;     // nullptr: no mesh
        bool classKnown = true;
        bool hasBounds = false;
    };

    struct AvatarDraw {                 // one suit's pieces of one swap set
        Model* model = nullptr;
        uint32_t swapsetId = 0;
        Vec4f hue;                      // w: 1 = hue the non-flesh pieces
    };
    struct ServerPlacement {
        Selection selection;
        tr::Vec3 position;
        Mat4 world;                     // position + yaw (unit scale)
        Model* model = nullptr;         // the base suit for avatars
        std::vector<AvatarDraw> avatarParts;    // each replaces the base model's pieces of its swap set
        Vec4f skinHue;                  // w: 1 = hue flesh pieces
        Vec4f colour;
        float ringRadius = 0;           // spawn radius / trigger radius (0: none)
        bool facing = false;            // draw a facing arrow
        bool area = false;              // region or mission area outline
        AreaShape shape;
    };
    Model* modelFor(const std::string& mesh);
    bool rayHitsModel(const Model& model, const tr::Vec3& origin, const tr::Vec3& dir, float& t) const;
    void buildTerrain(uint32_t step);
    void buildTerrainMaterial();
    void loadModel(const std::string& name, Model& model);
    SDL_GPUTexture* texture(const std::string& name);       // cached; nullptr if missing or unreadable

    Renderer& renderer_;
    const tr::AssetLibrary& assets_;
    const tr::MapBundle& map_;
    ViewCamera camera_;
    GpuMesh terrain_;
    TerrainMaterial terrainMaterial_;
    std::unordered_map<std::string, SDL_GPUTexture*> textures_;   // lower-case file name -> texture (or nullptr)
    std::map<std::string, std::string> textureErrors_;
    GpuMesh unitBox_;
    GpuMesh unitBoxLines_;
    GpuMesh unitCircle_;                // radius 1 in the XZ plane (lines)
    GpuMesh unitSquare_;                // -1..1 in the XZ plane (lines)
    GpuMesh arrow_;                     // along -z (the models' front), length 1 (lines)
    GpuMesh pole_;                      // 0..1 along +y (lines)
    std::vector<ServerPlacement> serverPlacements_;
    const WorldContent* serverContent_ = nullptr;
    const WorldDatabase* serverDatabase_ = nullptr;
    std::function<std::string(int32_t)> serverMeshForClass_;
    AvatarResolver serverAvatarFor_;
    std::unordered_map<std::string, Model> models_;
    std::deque<std::string> pending_;
    std::vector<Placement> placements_;
    size_t modelsLoaded_ = 0;
    size_t modelsFailed_ = 0;
    size_t drawnEntities_ = 0;
};
