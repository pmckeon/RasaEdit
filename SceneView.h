#pragma once

#include "Avatar.h"
#include "ParticleSim.h"
#include "Renderer.h"
#include "assetlibrary.h"
#include "sky.h"
#include "WorldDatabase.h"

#include <deque>
#include <map>
#include <functional>
#include <memory>
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
    // classKnown says whether the class exists in the game data; cullingLayerForClass gives the class's vizsetting
    // culling layer (nullptr: 3). An entity's layer is the map's cullLayer when non-zero, else its class's; layer 6 is
    // never drawn by the game (World::AddToWorld skips the graphics scene), so such entities get only their box.
    SceneView(Renderer& renderer, const tr::AssetLibrary& assets, const tr::MapBundle& map,
              std::function<std::string(int32_t)> meshForClass, std::function<bool(int32_t)> classKnown,
              std::function<int32_t(int32_t)> cullingLayerForClass = nullptr, uint32_t terrainStep = 0);
    // Terrain detail by distance, like the game's TerrainLoaderClient quadtree (QuadNode): 64 m chunks drawn with every
    // sample (1 m) near the camera and every 2nd / 4th / 8th sample further out; skirts hide the cracks where neighbours
    // differ. Full detail matters: at 2 m the Caves of Donn stair top (y 5.0) poked through a plateau edge whose samples
    // go 5.00 -> 0.75 within 1 m, which the game does not show. terrainStep (constructor) != 0 fixes the step instead
    // (1, 2, 4 or 8; tests). The distances are the editor's own (the game's are not decoded).
    float terrainDetailDistance = 160.0f;        // full detail within this distance of the camera; levels 1/2 at x2.5 / x6
    size_t terrainBuildsPerFrame = 8;            // finer chunk meshes built per frame (tests: all at once)
    size_t terrainChunkCount() const { return terrainChunks_.size(); }
    size_t terrainChunksAtFullDetail() const { return terrainFullDetail_; }
    // The culling layer an entity ends up on (see above).
    int32_t entityCullingLayer(size_t entityIndex) const { return entityIndex < placements_.size() ? placements_[entityIndex].cullingLayer : 0; }
    ~SceneView();
    SceneView(const SceneView&) = delete;
    SceneView& operator=(const SceneView&) = delete;

    // Loads queued models for up to budgetSeconds (call between frames).
    void loadPending(double budgetSeconds);
    // Runs the particle systems of map entities within particleDistance of the camera for dt seconds (call once per
    // frame). New ones are warmed up first so steady effects do not start empty.
    void advanceParticles(float dt);
    size_t liveParticleSystems() const { return liveParticles_.size(); }
    size_t liveParticles() const;
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
    // What focusOn looks at and from how far; false if the selection is not in the scene.
    bool focusTarget(const Selection& selection, tr::Vec3& target, float& distance) const;

    ViewCamera& camera() { return camera_; }
    float drawDistance = 1200.0f;
    bool showTextures = true;
    bool showTerrain = true;
    bool showEntities = true;
    bool showPlaceholders = true;
    bool showEffects = true;
    bool showParticles = true;          // particle systems attached to models (.pfx on "psys" contact points)
    float particleDistance = 200.0f;
    bool showServer = true;             // spawn pools, teleporters, links, objects
    bool showRegions = false;           // map regions (large outlines)
    bool showMissionAreas = true;
    // Sky lighting (.sky of the map's regions). setRegionSkies gives (region id, .sky file name) pairs from the map's
    // RegionSettings (MapRegion.sky resolved through the string table); each distinct sky is loaded once.
    struct SkyChoice {
        std::string name;                   // .sky file
        std::vector<int32_t> regions;       // map regions that use it
        bool loaded = false;
        std::string error;                  // why it could not be used
        tr::Sky sky;
    };
    void setRegionSkies(const std::vector<std::pair<int32_t, std::string>>& regionSkies);
    // Map entity hues as the client sets them (gamemap.py _LoadEntities3: generated.client.physicalentityclasshue by class,
    // else the .map's own hue1 / hue2; StaticBodySetHue). The static body's post-load (FUN_00511370) applies hue1 only when
    // it differs from (0, 0, 0, 255) (DAT_00d08b38), as SetHue (Hue and Hue2 of every piece, bytes / 255, FUN_00408ae0).
    // Otherwise each piece keeps its material's stored Hue. classHue gives the table row (false: none).
    void setClassHues(const std::function<bool(int32_t, tr::HueColour&, tr::HueColour&)>& classHue);
    const std::vector<SkyChoice>& skies() const { return skies_; }
    size_t skyIndex = 0;                    // into skies(); the sky of region 0 by default (used when not by region)
    // Sky by region, as the client does (regionmanager.py OnUpdateRegions: the regions the player is in plus region 0,
    // highest map priority first; RegionManagerClient::DoTransitionSky takes the first with a sky). Region volumes come
    // from the server's map_region rows; without server content the chosen skyIndex is used.
    bool skyByRegion = true;
    size_t activeSkyIndex() const;          // the sky lighting uses (by region at the camera, else skyIndex)
    bool skyFromRegion() const;             // activeSkyIndex() came from a server region volume
    // Environment cube by region (RegionManagerClient, decompiled 2026-10-11): of the same priority-sorted regions, the first
    // whose enabledFlags has bit 1 names gGlobalEnvTexture (MapRegion.environmentMap); none, an empty name or a failed load
    // gives "Env1.dds". setRegionEnvironmentMaps gives (region id, .dds name) pairs and loads each distinct cube once.
    void setRegionEnvironmentMaps(const std::vector<std::pair<int32_t, std::string>>& regionEnvironmentMaps);
    std::string activeEnvironmentMap() const;   // lower-case file name of the cube in use ("" = none loaded)
    const std::map<std::string, std::string>& environmentMapErrors() const { return environmentMapErrors_; }
    float timeOfDay = 0.5f;                 // fraction of the day; the map's starting sky time by default
    float startTimeOfDay() const { return startTimeOfDay_; }
    bool skyLighting = true;                // false: plain editor lighting
    bool showFog = true;                    // distance fog in the sky colour (the .sky's own "Used" flag is false everywhere)
    bool showMapLights = true;              // the map's lights.xml (up to 16 nearest the camera)
    bool showLightBuffer = false;
    int debugView = 0;                      // 2: normals, 3: specular intensity, 4: albedo (the game's /dl_shownormals,
                                            // /dl_showspecintensity, /dl_showdiffuse)
    bool materialMaps = true;               // normal and gloss maps (Pal* materials); off for comparison
    bool environmentMaps = true;            // environment cube reflections (Pal*Env*); off for comparison
    std::unordered_map<std::string, int> globeIndex_;    // lower-case GlobeMapFile -> cube index (1-based; 0 = none)
    std::string globeErrors_;
    void loadGlobeMaps();
    float lightColourGamma = 1;             // experiment: placed light colours raised to this power (2.2 = sRGB -> linear)
    float ambientScale = 1;                 // debug/experiment: multiplies the sky's hemisphere light           // debug: draw 0.2 x light like the game's /dl_showlight
    bool showSun = true;                    // the sky's sun/moon
    bool showShadows = true;                // sun shadows
    const std::vector<tr::PlacedLight>& mapLights() const { return mapLights_; }
    const std::string& mapLightsError() const { return mapLightsError_; }
    // The frame lighting for the current sky, time and toggles (also used by queueDraws).
    SceneLighting lighting() const;

    // Why an entity draws the way it does: "model", "water", "effect (no geometry)", "failed: ...", etc.
    std::string modelStatus(size_t entityIndex) const;
    size_t drawnEntities() const { return drawnEntities_; }

private:
    struct ModelPiece {
        GpuMesh mesh;
        SDL_GPUTexture* texture = nullptr;     // owned by textures_; nullptr = untextured
        bool alphaTest = false;
        float alphaReference = 0.5f;           // texture alpha below it is discarded (see loadModel)
        bool water = false;                    // TRWater* effect: drawn as a translucent surface
        bool hued = false;                     // "Hue" effect: a colour is added, weighted by the diffuse alpha
        float colourScale = 1;                 // 2 for "VertColor" effects (texture x vertex colour x 2)
        SDL_GPUTexture* glow = nullptr;        // GlowTexture (owned by textures_), added after lighting
        bool additive = false;                 // Additive phase: drawn additively with Effects on, not pickable
        bool translucent = false;              // Translucent phase: alpha-blended, lit
        // Pal* materials (PalDiffNormalGloss...): normal map and gloss map (r spec power, g spec intensity); see
        // CLAUDE.md "Material maps". Environment reflection (gloss b) not applied yet.
        SDL_GPUTexture* normalMap = nullptr;
        SDL_GPUTexture* glossMap = nullptr;
        bool environment = false;              // Pal*Env* material: reflects the region's environment cube by gloss.b
        bool hueUnweighted = false;            // alpha-tested Pal*Hue: Hue added without the diffuse-alpha weight
        // The material's stored Hue parameter (w = 1 when set): what a map entity's pieces keep when the entity is not hued.
        Vec4f materialHue = Vec4f{0, 0, 0, 0};
        TrMaterial trMaterial;                 // TR* deferred material (stage 3), trMaterial.active
        bool hairCutout = false;               // TRHair: cut out by the normal map's alpha
        // Phase pieces of non-TRAdditive effects are drawn with their forward technique (verified in game: absent from
        // /dl_showdiffuse): lit per vertex, detail layer, opacity x MatDiffuse alpha.
        bool forwardLit = false;
        SDL_GPUTexture* detail = nullptr;
        bool detailUv1 = false;
        float matDiffuseAlpha = 1.0f;
        bool flesh = false;                    // swap group "flesh": hued with the skin colour
        uint32_t swapId = 0;                   // avatar swap set (0: not swappable)
        bool aniso = false;                    // TRAdditiveAnisoSpec* effect (holograms): drawn with anisoMaterial
        bool twoSided = false;                 // the effect sets CULLMODE = NONE (PalDiffMap, TRDiffMapNoLight, TRLava, TRBeam)
        AnisoMaterial anisoMaterial;           // textures owned by textures_
    };
    struct Model {
        std::vector<ModelPiece> pieces;
        std::vector<tr::Vec3> positions;        // CPU copy of all pieces (model space) for picking
        std::vector<uint32_t> triangles;
        std::vector<uint8_t> triangleTwoSided;  // per triangle: hit from either side (two-sided effect)
        tr::Bounds bounds{tr::Vec3{-0.5f, 0, -0.5f}, tr::Vec3{0.5f, 2, 0.5f}};   // from the geometry once loaded
        bool loaded = false;
        bool failed = false;
        bool effectOnly = false;               // every piece is additive (glows, light cones, particle placeholders)
        bool hasSolid = false;                 // at least one non-additive piece
        bool hasWater = false;
        std::string failure;
        struct ParticleMount {                 // a .pfx on a contact point
            Mat4 local;                        // contact point frame in model space (bone bind pose x point)
            std::string file;
        };
        std::vector<ParticleMount> particles;
        // Lights on contact points ("lsys" .ltmp templates), in model space: position = contact point frame x Offset,
        // spot/globe orientation = frame rotation x the template's. Placed per entity in lighting().
        std::vector<tr::PlacedLight> lights;
    };
    struct Placement {
        Mat4 world;                 // entity placement
        Mat4 boxWorld;              // placement * local bounds (for placeholders and selection)
        tr::Vec3 centre;            // world-space bounding sphere
        float radius = 1;
        Model* model = nullptr;     // nullptr: no mesh
        bool classKnown = true;
        bool hasBounds = false;
        int32_t cullingLayer = 3;   // map cullLayer, else the class's vizsetting layer
        bool neverDrawn = false;    // culling layer 6
        Vec4f hue = Vec4f{0, 0, 0, 0};   // the entity's hue1 / 255 when it is set (w = 1), else the materials keep theirs
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
    // poseAnimation: skin the model into the first frame of this .anm ("" = as authored, bind pose).
    Model* modelFor(const std::string& mesh, const std::string& poseAnimation = "", const std::string& skeletonMesh = "");
    SceneLighting skyLightingAt() const;       // the sky part of lighting()
    // Puts the camera distance metres from target, looking at it (see focusOn).
    void frameTarget(const tr::Vec3& target, float distance);
    bool rayHitsModel(const Model& model, const tr::Vec3& origin, const tr::Vec3& dir, float& t) const;
    // Terrain chunk: samples rows row0..row1, columns col0..col1 (edges shared with the neighbours), one mesh per level
    // (step 1, 2, 4, 8); level 3 is built at load, finer ones on demand and released when the camera has moved away.
    struct TerrainChunk {
        uint32_t row0 = 0, row1 = 0, col0 = 0, col1 = 0;
        tr::Vec3 centre;
        float radius = 0;
        GpuMesh level0, level1, level2, level3;
    };
    static GpuMesh& chunkLevel(TerrainChunk& chunk, uint32_t level);
    void buildTerrainChunks();
    GpuMesh buildTerrainChunkMesh(const TerrainChunk& chunk, uint32_t level) const;
    void queueTerrain(const std::function<bool(const tr::Vec3&, float)>& visible);
    std::vector<TerrainChunk> terrainChunks_;
    float terrainLowest_ = 0, terrainRange_ = 1;
    int32_t terrainFixedLevel_ = -1;             // >= 0: every chunk at this level
    size_t terrainFullDetail_ = 0;
    void buildTerrainMaterial();
    void loadModel(const std::string& name, Model& model);
    SDL_GPUTexture* texture(const std::string& name);       // cached; nullptr if missing or unreadable

    Renderer& renderer_;
    const tr::AssetLibrary& assets_;
    const tr::MapBundle& map_;
    ViewCamera camera_;
    TerrainMaterial terrainMaterial_;
    std::unordered_map<std::string, SDL_GPUTexture*> textures_;   // lower-case file name -> texture (or nullptr)
    std::map<std::string, std::string> textureErrors_;
    GpuMesh unitBox_;
    GpuMesh unitBoxLines_;
    GpuMesh unitCircle_;                // radius 1 in the XZ plane (lines)
    GpuMesh unitSquare_;                // -1..1 in the XZ plane (lines)
    GpuMesh arrow_;                     // along -z (the models' front), length 1 (lines)
    GpuMesh pole_;                      // 0..1 along +y (lines)
    std::vector<SkyChoice> skies_;
    std::unordered_map<int32_t, size_t> regionSky_;       // map region id -> skies_ index (base sky for _tranz)
    std::unordered_map<int32_t, int32_t> regionPriority_; // map region id -> MapRegion.priority
    std::unordered_map<int32_t, int32_t> regionFlags_;    // map region id -> MapRegion.enabledFlags
    std::unordered_map<int32_t, std::string> regionEnvironmentMap_;   // map region id -> lower-case env cube name (may be empty)
    std::unordered_map<std::string, SDL_GPUTexture*> environmentCubes_;   // lower-case name -> cube (nullptr: failed)
    std::map<std::string, std::string> environmentMapErrors_;
    void readRegionSettings();
    std::vector<int32_t> regionsAtCamera() const;       // the client's region list at the camera, highest priority first
    float startTimeOfDay_ = 0.5f;
    std::vector<tr::PlacedLight> mapLights_;
    std::string mapLightsError_;
    std::vector<ServerPlacement> serverPlacements_;
    const WorldContent* serverContent_ = nullptr;
    const WorldDatabase* serverDatabase_ = nullptr;
    std::function<std::string(int32_t)> serverMeshForClass_;
    AvatarResolver serverAvatarFor_;
    std::unordered_map<std::string, Model> models_;
    struct ParticleFile {
        bool loaded = false;
        std::string error;
        tr::ParticleSystem system;
    };
    std::unordered_map<std::string, ParticleFile> particleFiles_;   // lower-case .pfx name
    const tr::ParticleSystem* particleSystem(const std::string& file);
    struct LiveParticles {
        size_t placement = 0;
        size_t mount = 0;
        std::unique_ptr<ParticleInstance> instance;
    };
    std::vector<LiveParticles> liveParticles_;
    std::vector<MeshVertex> particleScratch_;
    std::deque<std::string> pending_;
    std::vector<Placement> placements_;
    size_t modelsLoaded_ = 0;
    size_t modelsFailed_ = 0;
    size_t drawnEntities_ = 0;
};
