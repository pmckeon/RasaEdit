#pragma once

#include "Avatar.h"
#include "Beams.h"
#include "ParticleSim.h"
#include "Renderer.h"
#include "assetlibrary.h"
#include "sky.h"
#include "WorldDatabase.h"

#include <algorithm>
#include <algorithm>
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
    // Axes given directly instead of yaw / pitch (the PVS builder's cube faces look straight up and down).
    bool explicitAxes = false;
    tr::Vec3 axisRight{1, 0, 0}, axisUp{0, 1, 0}, axisForward{0, 0, 1};

    tr::Vec3 forward() const;
    tr::Vec3 right() const;        // cross(world up, forward)
    tr::Vec3 up() const;           // cross(forward, right)
    Mat4 viewMatrix() const;
    Mat4 projectionMatrix(float aspect) const;
};

// What is selected: a map entity (.map file) or a server object (World database).
enum class SelectionKind : uint8_t { None, MapEntity, SpawnPool, Teleporter, MapLink, Region, MissionArea, Object, Arrival, PatrolStep, WormholeLock, MapLight, AudioEmitter };

struct Selection {
    SelectionKind kind = SelectionKind::None;
    size_t index = 0;                   // into MapFile::entities, SceneView::mapLights() or the matching WorldContent vector
};
inline bool isSelected(const Selection& s) { return s.kind != SelectionKind::None; }
inline bool sameSelection(const Selection& a, const Selection& b) { return a.kind == b.kind && (a.kind == SelectionKind::None || a.index == b.index); }

// A number from the game's client.cfg (lines "Client.Graphics.Name = value"); fallback when the file or the key is missing.
float clientConfigValue(const std::string& gameRoot, const std::string& key, float fallback);

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
    // Each running system: map entity (or server placement) index, its file and particle count (viewport_test RASA_LISTLIVEPFX).
    std::vector<std::string> liveParticleList() const;
    // Every map usable / server object carrying persistent accessory beams (ACBM, durationMs 0): where, its texture, whether its ends
    // resolve, and the live beam's segment count once made (viewport_test RASA_LISTBEAMS).
    std::vector<std::string> stateBeamList() const;
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
    // The client's map-screen markers of this map (uimapmarker.factionedmarkers), drawn with the Server layer: a pole and ring at
    // each, a line to the teleporter / Kraftwerk (map_marker) or control point (marker_entity_id) bound to it, dim when none is.
    struct ClientMapMarker {
        int64_t entityId = 0;
        int32_t type = 0;
        tr::Vec3 position;
    };
    void setClientMapMarkers(std::vector<ClientMapMarker> markers) { clientMapMarkers_ = std::move(markers); }
    // Where the object bound to a client marker is (false: none bound, or not on this map).
    bool clientMarkerTarget(int64_t entityId, tr::Vec3& position) const;

    // World-space ray through a point of the viewport (normalised device coordinates, +y up).
    void viewRay(float ndcX, float ndcY, tr::Vec3& origin, tr::Vec3& direction) const;
    // Where a ray meets the ground: the terrain if there is one (its holes let the ray through), else the plane y = fallbackY.
    bool groundPoint(const tr::Vec3& origin, const tr::Vec3& direction, float fallbackY, tr::Vec3& point) const;
    // Where a ray first meets a surface: the nearer of the ground (groundPoint) and the drawn map entities' model triangles -
    // where new objects are put (a floor, platform or roof rather than the terrain under or beyond it). excludeEntity: a map
    // entity not to count (the one being dragged).
    bool surfacePoint(const tr::Vec3& origin, const tr::Vec3& direction, float fallbackY, tr::Vec3& point, int excludeEntity = -1) const;
    // The nearest model triangle of a drawn map entity the ray hits within maxT (t along the normalised direction).
    bool modelSurfaceHit(const tr::Vec3& origin, const tr::Vec3& direction, float maxT, float& t, int excludeEntity = -1) const;
    // PVS builder (PvsBuilder.h): the nearest hit of a ray with the given map entities' model triangles (no distance rule; layer-6
    // entities skipped) - the stand-in for the game's collision world in the camera probes.
    bool probeHit(const tr::Vec3& origin, const tr::Vec3& direction, float maxT, float& t, const std::vector<uint32_t>& entities) const;
    // A map entity's model triangles in world space (3 points each), appended; false without a loaded model.
    bool entityWorldTriangles(size_t entityIndex, std::vector<tr::Vec3>& out) const;
    // Whether a map entity is in the game's graphics scene: it has a model and is not on culling layer 6.
    bool entityHasModel(size_t entityIndex) const { return entityIndex < placements_.size() && placements_[entityIndex].model && !placements_[entityIndex].neverDrawn; }
    // Where queueDraws sends its draws: another renderer (a PVS view's), or nullptr for the scene's own.
    void setDrawTarget(Renderer* target) { renderer_ = target ? target : ownRenderer_; }
    // Snap to ground: the first surface straight down from (x, fromY, z) - architecture or terrain, holes excluded; fallback
    // when there is none (e.g. under the terrain with no cave floor loaded).
    float surfaceHeightBelow(float x, float z, float fromY, float fallback, int excludeEntity = -1) const;
    // The terrain height where the terrain is drawn (not in a hole: flag 0 interpolated >= 0.5, as terrain.hlsl discards).
    bool solidTerrainHeight(float x, float z, float& height) const;
    // Terrain height at (x, z), or fallback where there is no terrain.
    float groundHeight(float x, float z, float fallback) const;

    // Queues this frame's draws on the renderer (aspect from the renderer's target size).
    void queueDraws(const Selection& selection);
    // What is under a point of the viewport, in normalised device coordinates (-1..1, +y up).
    // pickEntities / pickServer choose which kinds of object can be hit.
    Selection pick(float ndcX, float ndcY, bool pickEntities = true, bool pickServer = true) const;
    void focusOn(const Selection& selection);
    // Moves the camera to look at a point from about `distance` away (as focusOn does for a selection).
    void focusOnPoint(const tr::Vec3& target, float distance) { frameTarget(target, distance); }
    std::vector<int32_t> regionsAtCamera() const;       // the client's region list at the camera, highest priority first
    // Map entity bounds from its loaded model (placements start from the .map's stored bounds or a 1 m box; the asset preview's
    // one-entity map has none). false while the model is not loaded.
    bool fitEntityToModel(size_t entityIndex);
    // World-space bounding sphere of a map entity's placement.
    bool entityBounds(size_t entityIndex, tr::Vec3& centre, float& radius) const;
    // Map editing (Client workspace): after an entity of the map's MapFile changed (moved, rotated, scaled, new class or hue),
    // refreshEntity rebuilds its placement; after entities were added or removed, rebuildEntities rebuilds them all. Both drop
    // the affected entities' particle systems, which advanceParticles restarts at the new place.
    void refreshEntity(size_t entityIndex);
    // Plays this animation on a map entity's model instead of its own ("" = its own again), from the start (the light clock is
    // reset: used by the Assets preview, whose scene holds that one entity).
    void setEntityAnimation(size_t entityIndex, const std::string& animation);
    // Plays a special-FX package's events on a map entity (the Assets preview of a .pkg): its accessories, particle attaches, lights
    // and beams, as a map usable's state package; "" removes them. Restarts the package clock (and with previewTimedEffects its timed
    // lights and beams, which otherwise only persistent ones are shown).
    void setEntityPackage(size_t entityIndex, const std::string& package);
    // Every object's state package plays as the game plays it (PackagePlayer, see tr::SfxPackage): its events wait for their markers,
    // starting it fires 0, its TIME events fire Timer markers, the object's animation fires its .mkr markers. With previewTimedEffects
    // (the Assets preview) the package starts without the warm-up the world's long-running states get, timed lights / beams play too,
    // firePackageMarker fires any marker by hand and stopPackage ends what runs and fires 24 (Stop).
    bool previewTimedEffects = false;
    float packageTime() const { return lightTime - packageStart_; }
    void firePackageMarker(uint32_t marker);
    void stopPackage();
    std::vector<uint32_t> packageMarkers() const;                     // the markers the package's events wait for, ascending
    std::vector<std::pair<float, uint32_t>> packageFired() const;     // (package time, marker), the last 64
    size_t packageTimersRunning() const;
    // Every object with a state package: where it is, its events, and the markers fired so far (other than the start), for checks.
    std::vector<std::string> debugPackagePlayers() const;
    // A particle system on its own at the origin (the Assets preview of a .pfx): started afresh by each call ("" = none), run by
    // advanceParticles and drawn with the others while Particles is on.
    void setStandaloneParticles(const std::string& file);
    bool standaloneParticleBounds(tr::Vec3& centre, float& radius) const;
    float standaloneParticleTime() const { return standaloneTime_; }
    void rebuildEntities();
    // After the map's lights.xml text changed (Client workspace light editing): the lights, their modifiers and globe maps again.
    void reloadMapLights();
    // The local bounds of a map entity's loaded model (false while loading or without a model) - a new class's bounds.
    bool entityModelBounds(size_t entityIndex, tr::Bounds& bounds) const;
    // Diagnostic: world skin matrix (placement x skin(time)) of every skeleton bone of an animated map entity's model, by bone
    // name - comparable with the game's gWorldMatrixArray palette (e.g. from an apitrace capture). Empty if not animated.
    std::vector<std::pair<std::string, Mat4>> debugWorldSkinMatrices(size_t entityIndex, float time) const;
    // Diagnostic: server placements whose bodies (serverBodies) carry model lights or particle systems: position, lights, systems.
    std::vector<std::string> debugServerBodyEffects() const;
    // The accessories on server placements (e.g. a teleporter's column): animation, material tracks, per piece UV tracks.
    std::vector<std::string> debugServerAccessories() const;
    // Diagnostic: the figures drawn for a server selection (a spawn pool draws one per creature it can spawn): position, model.
    std::vector<std::string> debugServerFigures(const Selection& selection) const;
    // Diagnostic: the idle pose of the server placement nearest (x, z) at lightTime - its base skeleton's bones, posed world
    // positions (placement x posed bone), and which animation of its sequence plays.
    std::vector<std::string> debugServerPose(float x, float z);
    // What focusOn looks at and from how far; false if the selection is not in the scene.
    bool focusTarget(const Selection& selection, tr::Vec3& target, float& distance) const;

    ViewCamera& camera() { return camera_; }
    float drawDistance = 1200.0f;
    bool showTextures = true;
    bool showTerrain = true;
    bool showEntities = true;
    bool showPlaceholders = true;
    // The collision the client uses for map entities (CLAUDE.md "What the client collides with"): each model's walkable surfaces
    // (BVWS on a bone whose property group names a WSName) placed as the game places them - cyan fill and edges; surfaces the game
    // drops (no WSName on the bone, or the entity's .map flag bit 1) red; box / sphere volumes, which map objects do not collide with,
    // grey outlines. Depth-tested; off by default.
    bool showCollision = false;
    size_t collisionBuildsPerFrame = 4;     // models whose collision meshes are built a frame (each reads its .geo)
    bool showSoundMarkers = false;          // the map's audioemitters.xml emitters as small boxes (selectable); the selected one shows its distances
    bool showLightMarkers = false;          // the map's lights.xml lights as small boxes (selectable); the selected one shows its reach
    bool showEffects = true;
    bool showParticles = true;          // particle systems attached to models (.pfx on "psys" contact points)
    float particleDistance = 200.0f;
    // Terrain clutter (tribbles: grass, ferns, flowers, debris), placed as the client does (tr::placeTribbleCell) in
    // the cells within tribbleCellRange of the camera's cell.
    bool showClutter = true;
    // The game's distance culling of map entities by culling layer (tabula_rasa.exe FUN_00856c00; see entityDistanceAlpha).
    // viewDistanceMultiplier: the view object's scalar (+0xEC, at least 1; 1 fits the fountain trace with the user's
    // Client.Graphics.ViewDistance 100); farCullLayerScalar: RQS "Far CullLayer Scalar" (TRRQS + 0x5C, 1 = full quality).
    bool gameDistances = true;
    float viewDistanceMultiplier = 1.0f;
    float farCullLayerScalar = 1.0f;
    // The game draws trunc(N x factor) of each clutter container's shuffled instances, factor = FloraDensity x the RQS
    // flora scalar ("Flora Density I / II" lower it as the frame rate drops); 1 = full quality.
    float clutterDensity = 1.0f;
    bool showServer = true;             // spawn pools, teleporters, links, objects
    bool showRegions = false;           // map regions (large outlines)
    bool showMissionAreas = true;
    // Control point garrisons: which side's linked pools are drawn (Rasa.NET ControlPoints: only the holder's garrison spawns, the
    // other side's pools are suspended), per control point id: 0 (or absent) = the side that holds it at start (default_owner),
    // 1 = the Bane's (link kind 1), 2 = the AFS's (kind 2). The other side's pools are not drawn or picked (selected: in full).
    std::unordered_map<int64_t, int> garrisonViews;
    int garrisonViewOf(int64_t pointId) const {
        const auto v = garrisonViews.find(pointId);
        return v == garrisonViews.end() ? 0 : v->second;
    }
    bool garrisonHidden(int64_t pointId, int32_t kind) const;  // a garrison of this link kind (1 / 2) at that point is not shown
    // A spawn pool (index into the server content) of the side its point's garrison view leaves out (the Overview's rule too).
    bool spawnPoolHidden(size_t poolIndex) const {
        return poolIndex < poolGarrisons_.size() && garrisonHidden(poolGarrisons_[poolIndex].point, poolGarrisons_[poolIndex].kind);
    }
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
    // Special-FX packages of usable classes per use state (GameData::usableStatePackage); call before setServerContent.
    // Server teleporters show their USE_TS_STATE_1 (56) package - Rasa.NET puts world teleporters in that state
    // (DynamicObjectManager) - as accessory models (`ACCY` events, tr::readSfxPackage), e.g. the waypoint's beam.
    void setUsableStatePackages(std::function<std::string(int32_t classId, int32_t stateId)> packageFor) { usablePackageFor_ = std::move(packageFor); }
    // The use states the server gives the map's own usables (entity id -> state; rasanet::mapUsableStates); rebuilds the entities.
    void setMapUsableStates(std::map<uint64_t, int32_t> states);
    int32_t entityUseState(size_t entityIndex) const { return entityIndex < placements_.size() ? placements_[entityIndex].useState : -1; }
    // The client's string table (an FX emitter names its package by string id); call before setServerContent.
    void setStringLookup(std::function<std::string(int32_t stringId)> lookup) { stringFor_ = std::move(lookup); }
    // A usable class's animation in a state (GameData::usableStateAnimation); ambient figures play theirs (state 44).
    void setUsableStateAnimations(std::function<tr::GameData::UsableAnimation(int32_t classId, int32_t stateId)> animationFor) {
        usableAnimationFor_ = std::move(animationFor);
    }
    const std::vector<SkyChoice>& skies() const { return skies_; }
    size_t skyIndex = 0;                    // into skies(); the sky of region 0 by default (used when not by region)
    // Sky by region, as the client does (regionmanager.py OnUpdateRegions: the regions the player is in plus region 0,
    // highest map priority first; RegionManagerClient::DoTransitionSky takes the first with a sky). Region volumes come
    // from the server's map_region rows; without server content the chosen skyIndex is used.
    bool skyByRegion = true;
    size_t activeSkyIndex() const;          // the sky lighting uses (by region at the camera, else skyIndex)
    // Sky changes cross-fade (tabula_rasa.exe, decompiled 2026-10-16): when the region's sky changes, FUN_0052c5d0 fades the old
    // sky's layers and lights out (FUN_00822ac0 / FUN_008238d0 -> FUN_008857c0 / FUN_008410b0, state 2) and the new one's in
    // (FUN_00822cb0 / FUN_008239c0, state 1) over a window of sky time [now, now + 4000 / day length in ms] (constant 4000.0 at
    // 0x00c2bd78; gamemap.py SetLengthOfDay(seconds x 1000)) - 4 seconds, linear (FUN_008859a0 / FUN_00841270 return the fraction
    // through the window, or 1 - it). A sky already fading out is dropped at the next change; one fading in fades out from where
    // it is. Matches the 4 s near-linear brightness ramp measured from video at Ranja Cavern. Lights are scaled by their weight
    // (CombinedHemiLight sums them), dome layers' alpha too (UNVERIFIED: read from FUN_00885af0, where the decompiled variables
    // are tangled). Fog switches at once (the transition code does not touch it - NOT traced further). The fade runs on real time
    // whether or not `animate` is on. skyTransitions off: switch at once.
    bool skyTransitions = true;
    static constexpr float skyFadeSeconds = 4.0f;
    struct SkyWeight {
        size_t index;
        float weight;
    };
    std::vector<SkyWeight> skyWeights() const;   // the skies drawn this frame (fading out first) with their weights
    void advanceSky(float dt);
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
    bool showMapLights = true;              // the map's lights.xml (up to 32 in view, nearest first)
    bool blackBackground = false;           // clear to black instead of the sky colour (the front-end SceneView's SetClearColor)
    const std::string& globeErrors() const { return globeErrors_; }   // globe maps that could not be loaded, one per line
    bool localShadows = true;               // shadows of spot lights with CastsShadows (the game's selection, see lighting())
    bool localShadowTerrain = true;         // terrain chunks among the placed lights' shadow casters
    bool spotCookies = true;                // spot lights' 2D globe maps (diagnostic switch)
    // The game's limits on placed-light shadows (client.cfg, each / 100 clamped to 0..1, tabula_rasa.exe FUN_007bfcb0 / FUN_007bfc50):
    // Client.Graphics.ShadowMaxSimultaneous (render settings + 0x34) and Client.Graphics.ShadowDistance (+ 0x30).
    float shadowMaxSimultaneous = 1.0f;
    float shadowDistance = 1.0f;
    // The real-time quality scaling option "Shadow Distance" (TRRQS + 0x84, on at full quality; tabula_rasa.exe FUN_00846c..: the RQS
    // table, FUN_008475b0 reads it): off - as when the frame rate falls below Client.Graphics.RealTimeQualityScaling - the shadow
    // distance limit drops from 25 + 50 x ShadowDistance to 25 m. The editor models full quality (viewport_test RASA_RQSSHADOWDISTANCE=0).
    bool rqsShadowDistance = true;
    // Static lights' shadow maps are drawn once and kept (the game renders a static light's map when it is created, never again);
    // anything that changes the casters calls this (model loads, entity / server content edits, caster toggles) so they are redrawn.
    void invalidateShadowMaps() { ++shadowCasterVersion_; }
    bool cacheShadowMaps = true;            // off: every placed-light shadow map is redrawn every frame (viewport_test RASA_NOSHADOWCACHE=1)
    // The last queueDraws: shadow lights whose maps were drawn (diagnostic, viewport_test RASA_LISTLIGHTS).
    int lastShadowMapsDrawn = 0;
    // The last lighting()'s shadow candidates (diagnostic, viewport_test RASA_LISTLIGHTS): value, position, range, kept.
    struct ShadowCandidateInfo {
        float value;
        tr::Vec3 position;
        float range;
        bool kept;
    };
    mutable std::vector<ShadowCandidateInfo> lastShadowCandidates;
    int lastActorShadowCasters = 0;          // characters drawn into non-static lights' shadow maps this frame (queueDraws)
    int onlyLight = -1;                     // diagnostic: >= 0 lights the frame with that placed light only
    bool showLightBuffer = false;
    int debugView = 0;                      // 2: normals, 3: specular intensity, 4: albedo, 5: specular power (the game's
                                            // /dl_shownormals, /dl_showspecintensity, /dl_showdiffuse, /dl_showspecpower)
    bool materialMaps = true;               // normal and gloss maps (Pal* materials); off for comparison
    bool environmentMaps = true;
    bool showStateEffects = true;           // server objects' use-state particles and lights (special-FX packages)
    bool showBeams = true;                  // their accessory beams (ACBM; viewport_test RASA_NOBEAMS)
    std::string hiddenGroup;                // debug: skip model pieces of this GPCE group; "!<group>" draws only it
    int onlyEntity = -1;                    // debug: draw only this map entity (-1 = all)            // environment cube reflections (Pal*Env*); off for comparison
    int entityFirst = -1, entityLast = -1;  // debug: draw only map entities in [first, last) (-1 = all)
    std::unordered_map<std::string, int> globeIndex_;    // lower-case GlobeMapFile -> cube index (1-based; 0 = none)
    std::unordered_map<std::string, int> cookieIndex_;   // lower-case GlobeMapFile that is a 2D texture -> layer (1-based)
    std::string globeErrors_;
    void loadGlobeMaps();
    float lightColourGamma = 1;             // experiment: placed light colours raised to this power (2.2 = sRGB -> linear)
    float ambientScale = 1;                 // debug/experiment: multiplies the sky's hemisphere light           // debug: draw 0.2 x light like the game's /dl_showlight
    bool showSun = true;                    // the sky's sun/moon
    bool showSkyDome = true;                // the sky's layer models (dome, sun / moon discs, stars, clouds, horizon)
    bool showShadows = true;                // sun shadows
    const std::vector<tr::PlacedLight>& mapLights() const { return mapLights_; }
    const std::vector<tr::AudioEmitter>& audioEmitters() const { return audioEmitters_; }
    const std::string& audioEmittersError() const { return audioEmittersError_; }
    void reloadAudioEmitters();                      // after the map's audioemitters.xml text changed
    // A map light as drawn this frame: with its modifiers (lights.xml <Mods>) applied when animateLights is on.
    tr::PlacedLight displayedMapLight(size_t index) const;
    // The lights.xml <Mods> tracks attached to a light (by its ID; none for ID 0).
    size_t mapLightModifierTracks(const tr::PlacedLight& light) const;
    // World animation clock in seconds (advanceLights): light modifiers, object and material animations, creature idles. With
    // `animate` off the clock and the particle systems stand still (everything keeps its current pose). animateLights off
    // (diagnostic) draws placed lights without their modifiers.
    float lightTime = 0;
    bool animate = true;
    bool animateLights = true;
    void advanceLights(float dt) {
        advanceSky(dt);
        if (animate) {
            lightTime += std::clamp(dt, 0.0f, 0.1f);
            advancePackagePlayers(std::clamp(dt, 0.0f, 0.1f));
        }
    }
    const std::string& mapLightsError() const { return mapLightsError_; }
    // The frame lighting for the current sky, time and toggles (also used by queueDraws).
    SceneLighting lighting() const;

    // Why an entity draws the way it does: "model", "water", "effect (no geometry)", "failed: ...", etc.
    std::string modelStatus(size_t entityIndex) const;
    size_t drawnEntities() const { return drawnEntities_; }
    size_t fadingForwardPieces() const { return fadingForwardPieces_; }
    // Diagnostic: milliseconds the last queueDraws spent per section - 0 setup + terrain, 1 map entities, 2 clutter, 3 particles,
    // 4 sky, 5 server objects.
    double queueSectionMs(int section) const { return section >= 0 && section < 6 ? queueSectionMs_[section] : section == 6 ? queueTerrainMs_ : 0.0; }   // 6: terrain alone (part of 0)   // pieces drawn in the fade band's forward mode this frame
    size_t drawnClutter() const { return drawnClutter_; }
    size_t clutterInstances() const;             // in the cells currently built

private:
    struct Model;
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
        SDL_GPUTexture* palDecal = nullptr;     // Pal*Dec* decal map (DecalTexture on UV set DecalTexCoord, in texU2)
        bool skyEmissive = false;               // TREmissive* piece: drawn as a sky layer by queueSky (unlit, MatDiffuse x 4)
        bool environment = false;              // Pal*Env* material: reflects the region's environment cube by gloss.b
        bool hueUnweighted = false;            // alpha-tested Pal*Hue: Hue added without the diffuse-alpha weight
        // TRDiffMapDiffMap* effect: in the game's distance fade band (SceneView::entityDistanceAlpha < 0.999) the piece is drawn with
        // the effect's one static forward technique (see "fade band" in queueDraws), which the fountain capture verifies.
        bool fadeForward = false;
        // How the effect's preferred forward technique (FUN_005c1d40 / FUN_005c34b0: per RenderMode / TransformMode /
        // TechniqueMode, highest ComplexityFactor first) writes alpha when the body fades (MatDiffuse.a = the fade): 1
        // TRDiffMapDiffMap* lerp(diffuse.a, modifier.a, vertex.a) (no MatDiffuse), 2 diffuse.a x vertex.a x MatDiffuse.a, 3 diffuse.a
        // x MatDiffuse.a, 4 vertex.a x MatDiffuse.a, 5 2 x diffuse.a x vertex.a x MatDiffuse.a; 0 = not decoded (drawn opaque).
        int fadeAlphaMode = 0;
        // TRDiffMapVertColorWave: the vertex sway's gSwayRate / gSwayAmplitude (the piece's, else the effect's defaults 25 / 1);
        // UV set 1 (the sway weights) is in texU1 / texV1.
        bool sway = false;
        bool palFading = false;     // fadeAlphaMode 6 = Pal* forward (alpha MatDiffuse.a x gBlendFade), drawn through draw()
        float swayRate = 25.0f, swayAmplitude = 1.0f;
        bool lavaForward = false;              // TRLava: forward-only effect (no deferred technique), drawn after the composite - not in the light buffer
        // The material's stored Hue parameter (w = 1 when set): what a map entity's pieces keep when the entity is not hued.
        Vec4f materialHue = Vec4f{0, 0, 0, 0};
        TrMaterial trMaterial;                 // TR* deferred material (stage 3), trMaterial.active
        bool hairCutout = false;               // TRHair: cut out by the normal map's alpha
        int animBone = -1;
        bool refract = false;                  // TRRefract: drawn by Renderer::drawRefract with refractMaterial
        RefractMaterial refractMaterial;                     // animated model: the one skeleton bone this piece is skinned to (-1: static)
        // Phase pieces of non-TRAdditive effects are drawn with their forward technique (verified in game: absent from
        // /dl_showdiffuse): lit per vertex, detail layer, opacity x MatDiffuse alpha.
        bool forwardLit = false;
        SDL_GPUTexture* detail = nullptr;
        int detailSet = 0;                     // DetailTexCoord: the detail map's UV set (0 / 1 / 2)
        // The forward technique's TexCoordTransforms (vsBasicStatic2TexVertColor, disassembled 2026-10-15: diffuse = UV set 0 x
        // TexCoordTransform0, detail = UV set n x TexCoordTransform<n>); only the transform fields are used.
        TrMaterial forwardUv;
        float matDiffuseAlpha = 1.0f;
        bool flesh = false;                    // swap group "flesh": hued with the skin colour
        uint32_t swapId = 0;                   // avatar swap set (0: not swappable)
        bool aniso = false;                    // TRAdditiveAnisoSpec* effect (holograms): drawn with anisoMaterial
        // Its facing term's normal (vs2x_AnisoDetDec, see scene.hlsl VertexUniforms): skinned pieces use palette entry 0's skin
        // matrix (this skeleton bone; -1 for static pieces); static pieces on a bone divide by that bone's scale (the baked normals
        // carry it).
        int anisoNormalBone = -1;
        float anisoStaticScale = 1.0f;
        bool twoSided = false;                 // the effect sets CULLMODE = NONE (PalDiffMap, TRDiffMapNoLight, TRLava, TRBeam)
        AnisoMaterial anisoMaterial;           // textures owned by textures_
        // Object animation (X.anm of X.geo): a skinned piece blended over several bones keeps its stored (unskinned) vertices
        // in its mesh and is skinned on the GPU each frame (SceneView::setPieceSkin); null for every other piece.
        SDL_GPUBuffer* skinBuffer = nullptr;   // SkinVertex per vertex: skeleton bones and weights
        bool skinNormals = true;               // false for TRAdditiveAnisoSpec*: the game skins only the position
        const Model* skinOwner = nullptr;         // the model whose skeleton and animation pose it
        // Model animation's material tracks (index into animation.materialTracks, -1: none) for TexCoordTransform0..2.
        int uvTrack0 = -1, uvTrack1 = -1, uvTrack2 = -1, uvTrack3 = -1;
        bool waterShader = false;                           // TRWaterReflectRefract: drawn by Renderer::drawWater
        WaterMaterial waterMaterial;
        std::string group;                     // GPCE group (GeoPiece::swapGroup)
    };
    struct UvTransforms {                      // TexCoordTransform0..2 as u / v rows (see TrMaterial)
        Vec4f u0, v0, u1, v1, u2, v2;
        Vec4f u3 = Vec4f{1, 0, 0, 0}, v3 = Vec4f{0, 1, 0, 0};
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
        // TRSphBillboard pieces (camera-facing sprites, e.g. the Logos symbols over arch_eloh_logos_*): drawn per frame by
        // queueBillboards, not as meshes.
        struct Billboard {
            std::vector<tr::GeoVertex> vertices;   // position = corner offset (world units), position1 = centre (piece space)
            std::vector<uint32_t> indices;
            Mat4 boneWorld = identityMatrix();     // the static piece's bone, frame 0 / bind (model space)
            int32_t animBone = -1;                 // follows this bone in object animations (-1: static)
            SDL_GPUTexture* texture = nullptr;
            Vec4f diffuse{0.5f, 0.5f, 0.5f, 1}, ambient{0.5f, 0.5f, 0.5f, 1}, emissive{0, 0, 0, 0};   // x gModulateScale 0.5 (rgb)
        };
        std::vector<Billboard> billboards;
        // Lights on contact points ("lsys" .ltmp templates), in model space: position = contact point frame x Offset,
        // spot/globe orientation = frame rotation x the template's. Placed per entity in lighting().
        std::vector<tr::PlacedLight> lights;
        std::vector<int32_t> lightBones;       // per light: its contact point's skeleton bone (-1: none)
        // Every contact point by its CPDF id (special-FX particles and lights attach by id): model-space frame, rotation.
        struct ContactFrame {
            uint32_t id = 0;                   // CPDF id
            tr::ConnectionPointName connection;    // what lookups match (see tr::ConnectionPointName)
            Mat4 frame;                        // model space; posed models: on the posed skeleton
            tr::Quat rotation;
            int32_t bone = -1;                 // skeleton bone the point hangs on (-1: none)
            Mat4 local = identityMatrix();     // the point's transform relative to that bone
        };
        std::vector<ContactFrame> contactFrames;
        // Animation playback (map objects with an X.anm named like their model): pieces skinned wholly to one bone are
        // built in the first frame and drawn with skin(t)[bone] x inverse(skin(0)[bone]); others stay in frame 0.
        tr::GeoModel skeleton;                 // bones only
        tr::Animation animation;
        std::string animationName;             // its file (the object animation of a "mesh|@anim" key) - for its .mkr
        std::vector<Mat4> restInverse;         // per bone: inverse of the frame-0 skin matrix
        std::vector<int> boneTracks;           // per bone: its track in animation (-1: none)
        bool animated = false;
        bool materialAnimated = false;         // some drawn piece has material (texture transform) tracks
        bool skinAnimated = false;             // some piece has a skinBuffer
        mutable uint64_t boneFrame = 0;        // frame whose bone palette was uploaded (setPieceSkin)
        mutable uint32_t boneBase = 0;
        std::vector<Mat4> restSkin;            // skin matrices the mesh was posed with at load (frame 0 / bind pose)
        bool avatarPose = false;               // a creature's idle pose ("%" key): posed per placement (ServerPlacement::idles)
        tr::GeoModel poseSkeleton;             // avatar parts: the base body's bones they are posed on (empty: own bones)
        mutable std::vector<Mat4> frameSkin;   // currentSkin's cache
        mutable float frameSkinTime = -1;      // lightTime of frameSkin (-1: none)
    };
    // Per-piece matrices of an animated model at time t (loops over the animation's duration); empty if not animated.
    std::vector<Mat4> animatedPieceMatrices(const Model& model, float time) const;
    // Before drawing a piece: GPU skinning on (its model's bone palette at lightTime, uploaded once per frame) or off.
    void setPieceSkin(const ModelPiece& piece);
    // The model's skin matrices this frame: animated objects at lightTime (held while the clock is stopped), else as loaded.
    const std::vector<Mat4>& currentSkin(const Model& model) const;
    // AnisoMaterial::normalMatrix for a piece drawn with this placement (entity world, without per-piece animation).
    Mat4 anisoNormalMatrix(const Model* model, const ModelPiece& piece, const Mat4& placementWorld, const Mat4& pieceWorld) const;
    uint64_t frameNumber_ = 1;
    // A piece's texture transforms at time t from its material tracks (slots without a track keep out's values); false if none.
    bool animatedUvTransforms(const Model& model, const ModelPiece& piece, float time, UvTransforms& out) const;
    // A special-FX event attached to a body: the placement's own model (body null) or one of its accessories (events of an
    // accessory's nested package).
    struct StateParticle {
        Model* body = nullptr;
        tr::SfxParticleAttach event;
        uint32_t marker = 0;            // the marker that fires it (a nested package's 0 = its accessory's)
        size_t eventIndex = 0;          // in the player's pending events
        float startTime = 0;            // lightTime when it fired
        bool warm = false;              // started with the world's 3 s warm-up (a long-running state seen at an arbitrary moment)
        bool spent = false;             // a copy fired again whose particles are gone (its slot is reused by the next firing)
    };
    struct StateLight {
        Model* body = nullptr;
        tr::SfxLight event;
        uint32_t marker = 0;
        size_t eventIndex = 0;
        float startTime = 0;            // lightTime when it fired
    };
    struct StateBeam {                  // ACBM (see Beams.h): both ends on this body's contact points
        Model* body = nullptr;
        tr::SfxBeam event;
        uint32_t marker = 0;
        size_t eventIndex = 0;
        float startTime = 0;            // lightTime when it fired (a live beam made before this is remade)
    };
    struct StateEffects {
        std::vector<Model*> accessories;
        std::vector<uint32_t> accessoryMarkers;
        std::vector<StateParticle> particles;
        std::vector<StateLight> lights;
        std::vector<StateBeam> beams;
        std::vector<tr::SfxTimer> timers;
        std::vector<uint32_t> stops;
    };
    struct LiveTimer {
        tr::SfxTimer timer;
        float countdown = 0, elapsed = 0;
    };
    // A state package playing on one object (map usable, server object, the preview's entity): every event of the package waits in
    // `pending` for its marker; firing one puts the events waiting for it into the object's state lists (stateParticles etc.).
    struct PackagePlayer {
        StateEffects pending;
        std::vector<LiveTimer> timers;
        std::vector<std::pair<float, uint32_t>> fired;   // (lightTime, marker), the last 64
        uint32_t random = 1;                // Park-Miller (the game's FUN_007880d0 stream is not reproduced)
        std::string markerAnimation;        // the animation whose markers it follows, and its time at the last update
        float markerLast = -1;
        bool active() const {
            return !pending.particles.empty() || !pending.lights.empty() || !pending.beams.empty() || !pending.accessories.empty() ||
                   !pending.timers.empty() || !pending.stops.empty();
        }
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
        // A usable the .map places, in the use state the server sends it (setMapUsableStates; -1 = none, as the client draws it
        // without one): the state's special-FX accessories, particle attaches and lights, as server objects carry theirs.
        int32_t useState = -1;
        std::vector<Model*> stateAccessories;
        std::vector<StateParticle> stateParticles;
        std::vector<StateLight> stateLights;
        std::vector<StateBeam> stateBeams;
        PackagePlayer player;
    };

    struct AvatarDraw {                 // one suit's pieces of one swap set
        Model* model = nullptr;
        uint32_t swapsetId = 0;
        Vec4f hue;                      // w: 1 = hue the non-flesh pieces
    };
    struct IdleSequence {               // a creature's idle playback (poseIdle)
        float start = 0;                // light-clock time the current animation started
        float last = -1;                // light-clock time of the last update (a clock running backwards restarts)
        int32_t current = -1;           // index in ServerPlacement::idles; -1 = not started
        uint32_t random = 1;            // Park-Miller state
        float delay = 0;                // seconds, drawn once per sequence (objectanimationmgr.py: one delay per PlayObjectAnimation)
        // The last pose's palettes (base skeleton, its posed bones, each part's), reused while the pose is unchanged.
        float cacheTime = -1;
        const tr::Animation* cacheAnimation = nullptr;
        size_t cacheParts = 0;
        std::vector<Mat4> baseSkin, posed;
        std::vector<std::pair<const Model*, std::vector<Mat4>>> partSkins;
    };
    struct ServerPlacement {
        Selection selection;
        int32_t cullingLayer = 0;       // the class's vizsetting culling layer (creature / teleporter / object class; 0 = no limit)
        tr::Vec3 position;
        Mat4 world;                     // position + yaw (unit scale)
        Model* model = nullptr;         // the base suit for avatars
        std::vector<AvatarDraw> avatarParts;    // each replaces the base model's pieces of its swap set
        std::vector<Model*> accessories;        // the use state's special-FX accessory models (drawn at the same placement)
        std::vector<StateParticle> stateParticles;   // the use state's particle attaches (PFXA)
        std::vector<StateLight> stateLights;         // the use state's lights (SLIT)
        std::vector<StateBeam> stateBeams;           // the use state's accessory beams (ACBM)
        PackagePlayer player;                        // plays the use state's package (fires the events into the lists above)
        struct BodyAccessory {                       // avatar accessories (weapons, holsters, headsets)
            Model* model = nullptr;
            uint32_t connectionPoint = 0;
            Vec4f hue;
        };
        std::vector<BodyAccessory> bodyAccessories;
        std::vector<tr::GameData::FamilyAnimation> idles;   // creatures: "Idle - Non Combat"; ambient figures: their state's family
        int32_t idleType = 3;           // animationspecificationtype (3 recurring one-shot = chained, as creatures' ambient idles)
        int32_t idleDelayLower = 0, idleDelayUpper = 0;     // milliseconds before each animation (the spec's delay range)
        mutable IdleSequence idle;
        Vec4f skinHue;                  // w: 1 = hue flesh pieces
        Vec4f colour;
        float ringRadius = 0;           // spawn radius / trigger radius (0: none)
        bool linked = false;            // draw a line to linkTarget (an arrival point's spawn pool, a patrol step's next step)
        tr::Vec3 linkTarget;
        bool pointLink = false;         // the line goes to a control point: drawn only while it or this object is selected
        Selection pointSelection;       // that control point
        int64_t garrisonPoint = 0;      // a garrison pool's control point id (control_point_link kind 1 / 2; 0 = none)
        int32_t garrisonKind = 0;       // 1 Bane garrison, 2 AFS garrison
        bool extraFigure = false;       // a spawn pool's second and later figures (the first carries ring, arrow and link)
        bool facing = false;            // draw a facing arrow
        bool area = false;              // region or mission area outline
        AreaShape shape;
    };
    // poseAnimation: skin the model into the first frame of this .anm ("" = as authored, bind pose).
    Model* modelFor(const std::string& mesh, const std::string& poseAnimation = "", const std::string& skeletonMesh = "");
    std::string objectAnimationFor(const std::string& mesh);
    float entityDistanceAlpha(const Placement& p) const;    // 1 opaque, (0.01, 1) fading, <= 0.01 culled
    float distanceAlpha(int32_t layer, const tr::Vec3& centre, float radius) const;   // the game's rule for any body
    // A server object's model beyond the game's draw distance for its class's culling layer (with gameDistances): not posed,
    // drawn or picked - the marker box stands in (the selection is always drawn).
    bool serverModelCulled(const ServerPlacement& s) const;
    // A garrison pool of the side its point's garrison view does not show: not drawn or picked unless selected.
    bool garrisonSuspended(const ServerPlacement& s) const;
    std::unordered_map<int64_t, int64_t> pointDefaultOwner_;   // control point id -> default_owner (0 Bane, 1 AFS)
    struct PoolGarrison {
        int64_t point = 0;
        int32_t kind = 0;                                       // 0: not a garrison pool
    };
    std::vector<PoolGarrison> poolGarrisons_;                   // per spawn pool of the server content
    std::function<int32_t(int32_t)> cullingLayerForClass_;
    std::function<std::string(int32_t)> meshForClass_;
    std::function<bool(int32_t)> classKnown_;
    Placement makePlacement(const tr::MapEntity& e);
    std::function<bool(int32_t, tr::HueColour&, tr::HueColour&)> classHue_;
    Model* objectModelFor(const std::string& mesh, const std::string& packageAnimation = "");
    std::unordered_map<std::string, std::string> objectAnimations_;   // mesh -> its object animation ("" = none)
    // Creature idles: animations by name (null = failed to read), bone -> track per (model, animation), and the palettes of the
    // creature being drawn (model -> bone base, set by poseIdle, cleared after the placement) with its base skeleton's posed bones.
    const tr::Animation* idleAnimation(const std::string& name);
    const std::vector<int>& idleTracks(const Model& model, const tr::Animation& animation);
    bool poseIdle(const ServerPlacement& s);
    void queueSky();                        // the active sky's layers (TRasa::SkyLayer bodies), see the comment at queueSky
    void queueSkyLayers(const tr::Sky& sky, float fade);
    void queueBillboards(const Model& model, const Mat4& placementWorld, const std::vector<Mat4>& animatedMatrices, const SceneLighting& light);
    SceneLighting frameLighting_;           // this frame's lighting (queueDraws)
    // The rigid bodies of a server placement that carry their own model lights and particle systems, as map entities' models do:
    // the object's model, its use-state accessories (both at the placement) and avatar accessories on their sockets (posedBones:
    // the creature's pose this frame; null = the models' stored first frame).
    struct ServerBody {
        const Model* model = nullptr;
        Mat4 world;
    };
    std::vector<ServerBody> serverBodies(const ServerPlacement& s, const std::vector<Mat4>* posedBones) const;
    bool bodyAccessoryWorld(const ServerPlacement& s, const ServerPlacement::BodyAccessory& a, const std::vector<Mat4>* posedBones,
                            Mat4& world) const;
    bool serverPlacementShown(const ServerPlacement& s) const;     // its layer is on (Server / Mission areas / Regions)
    std::unordered_map<std::string, std::unique_ptr<tr::Animation>> idleAnimations_;
    std::map<std::pair<const Model*, const tr::Animation*>, std::vector<int>> idleTracks_;
    const std::vector<int>& poseBoneIndices(const Model& part, const Model& base);
    std::map<std::pair<const Model*, const Model*>, std::vector<int>> poseBoneIndices_;
    std::unordered_map<const Model*, uint32_t> skinOverride_;
    std::vector<Mat4> idlePosed_;
    SceneLighting skyLightingAt() const;       // the sky part of lighting()
    // Puts the camera distance metres from target, looking at it (see focusOn).
    void frameTarget(const tr::Vec3& target, float distance);
    bool rayHitsModel(const Model& model, const tr::Vec3& origin, const tr::Vec3& dir, float& t) const;
    bool rayHitsEntityModel(size_t entityIndex, const tr::Vec3& origin, const tr::Vec3& direction, float& best) const;
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
    SDL_GPUTexture* volumeTexture(const std::string& name); // 3D (volume DDS); cached; nullptr if missing or unreadable

    Renderer* renderer_;                    // where draws go (the own renderer, or a PVS view's - setDrawTarget)
    // Placed-light shadow maps kept in a renderer's atlas (one cache per renderer): an entry per light that has had a shadow - its
    // atlas cells (128-texel units, one square per view) and the caster version its map was drawn at. A static light's map is redrawn
    // only when shadowCasterVersion_ moves on; a non-static light's every frame. Entries not used this frame are evicted, oldest
    // first, when a new light needs room.
    struct ShadowCacheEntry {
        bool alive = false;
        int kind = 0;
        tr::Vec3 position;
        float range = 0, width = 0, length = 0, penumbra = 0;
        tr::Quat orientation;
        uint32_t size = 0;
        int views = 0;
        bool isStatic = true;
        std::vector<std::pair<int, int>> cells;     // per view: the square's first cell (x, y)
        uint64_t drawnVersion = 0;                  // caster version of the map in the atlas (0 = none)
        uint64_t pendingVersion = 0;                // queued for drawing at this version, once the renderer's pass count passes pendingPass
        uint64_t pendingPass = 0;
        uint64_t lastUsed = 0;
    };
    struct ShadowCache {
        std::vector<ShadowCacheEntry> entries;
        uint64_t passesSeen = 0;
    };
    mutable std::map<const Renderer*, ShadowCache> shadowCache_;
    // The cache entry of a light (same kind, place, orientation, size, range, static flag), else a new one with atlas cells for its
    // views (evicting entries not used this frame, oldest first); -1 when the atlas cannot hold it.
    int shadowCacheEntry(ShadowCache& cache, const tr::PlacedLight& light, uint32_t size, int views) const;
    mutable std::vector<int> frameShadowEntries_;    // this frame's shadow lights (by mask bit) -> their cache entry
    mutable uint64_t shadowFrame_ = 0;
    uint64_t shadowCasterVersion_ = 1;
    uint64_t shadowCasterSignature_ = 0;
    Renderer* ownRenderer_;
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
    float startTimeOfDay_ = 0.5f;
    std::vector<tr::PlacedLight> mapLights_;
    std::vector<tr::LightModifier> lightModifiers_;                // lights.xml <Mods>
    tr::PlacedLight modifiedLight(const tr::PlacedLight& light) const;
    std::string mapLightsError_;
    std::vector<tr::AudioEmitter> audioEmitters_;
    std::string audioEmittersError_;
    std::vector<ServerPlacement> serverPlacements_;
    const WorldContent* serverContent_ = nullptr;
    size_t skyCurrent_ = size_t(-1);                 // the sky fading in / shown (npos: not started, use activeSkyIndex at full)
    size_t skyPrevious_ = size_t(-1);                // the sky fading out (npos: none)
    float skyPreviousStart_ = 0;                     // its weight when the change came
    float skyElapsed_ = 0;                           // seconds since the change
    std::vector<ClientMapMarker> clientMapMarkers_;
    const WorldDatabase* serverDatabase_ = nullptr;
    std::function<std::string(int32_t)> serverMeshForClass_;
    std::function<std::string(int32_t, int32_t)> usablePackageFor_;
    std::map<uint64_t, int32_t> mapUsableStates_;
    std::function<std::string(int32_t)> stringFor_;
    std::function<tr::GameData::UsableAnimation(int32_t, int32_t)> usableAnimationFor_;
    StateEffects stateEffects(int32_t classId, int32_t stateId);
    // nestedMarker: for an accessory's nested package, the accessory's marker - the nested package starts (its marker 0) when the
    // accessory fires (UNVERIFIED: its other markers are taken to share the outer package's).
    void addPackageEffects(const std::string& package, Model* body, int depth, StateEffects& effects, int64_t nestedMarker = -1);
    size_t packageEntity_ = 0;              // the preview's entity (setEntityPackage)
    // The lists a player fires into: a map placement's (server false) or a server placement's.
    struct EffectLists {
        PackagePlayer* player = nullptr;
        std::vector<Model*>* accessories = nullptr;
        std::vector<StateParticle>* particles = nullptr;
        std::vector<StateLight>* lights = nullptr;
        std::vector<StateBeam>* beams = nullptr;
    };
    EffectLists effectLists(Placement& p);
    EffectLists effectLists(ServerPlacement& s);
    // Starts a package on an object: its events wait in the player, marker 0 fires (warm: the world's long-running states).
    void startPackage(EffectLists lists, StateEffects fx, bool warm);
    void fireMarker(EffectLists lists, uint32_t marker, bool warm = false);
    void stopPlayer(EffectLists lists);
    float packageRandomDelay(PackagePlayer& player, float minDelay, float maxDelay);
    // Timers and the objects' animation markers (advanceLights).
    void advancePackagePlayers(float dt);
    void followAnimationMarkers(EffectLists lists, const std::string& animation, float time, float duration);
    std::unordered_map<std::string, std::vector<tr::AnimMarker>> markerFiles_;   // lower-case .anm name -> its .mkr markers
    const std::vector<tr::AnimMarker>& animationMarkers(const std::string& animation);
    bool marker24Fired_ = false;
    bool startingPackage_ = false;          // startPackage: no globe-map / shadow-map refresh per object (map loads do it once)
    // A contact point's frame on a body (cp 0 = the body itself); false when the body is not loaded or lacks the id.
    // Lookup by connection point (type 1, `usage`, id) as FUN_00924880; id 0 = the body itself (special-FX events).
    // posedBones: the body's bones posed this frame (a creature's idle); the point then follows its bone.
    static bool contactFrame(const Model* body, uint32_t id, uint32_t usage, Mat4& frame, tr::Quat& rotation,
                             const std::vector<Mat4>* posedBones = nullptr);
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
        size_t placement = 0;                   // map entity index, or server placement index when `server`
        size_t mount = 0;                       // model particle mount, or the server placement's stateParticles index
        bool server = false;
        bool mapState = false;                  // a map usable's state effect: mount = stateParticles index (body -1) or the mount of
                                                // stateAccessories[body]'s own particle systems
        int body = -1;                          // server: index in serverBodies (its model's mount), -1 = a state particle
        std::unique_ptr<ParticleInstance> instance;
    };
    std::vector<LiveParticles> liveParticles_;
    // Persistent accessory beams (durationMs 0, both targets 0) of server objects' and map usables' use states, made when they come
    // within particleDistance (BeamInstance: the trigger's branches, then the noise and ribbons each frame).
    struct LiveBeam {
        size_t placement = 0;                   // server placement index when `server`, else map entity index
        size_t index = 0;                       // stateBeams index
        bool server = false;
        std::unique_ptr<BeamInstance> beam;
    };
    std::vector<LiveBeam> liveBeams_;
    float packageStart_ = 0;                // lightTime when setEntityPackage started the package
    // A state beam's two ends in world space this frame; false when a contact point is missing (or the body not loaded).
    bool beamEnds(const StateBeam& beam, const Model* model, const Mat4& world, tr::Vec3& start, tr::Vec3& end) const;
    std::string standaloneFile_;
    std::unique_ptr<ParticleInstance> standalone_;
    // GEOG particle generators: each model's pieces' stored vertex positions, read once per model.
    std::map<std::string, std::vector<std::vector<tr::Vec3>>> geoPoints_;
    GeoPointSource geoPointSource();
    float standaloneTime_ = 0;
    std::vector<MeshVertex> particleScratch_;
    std::deque<std::string> pending_;
    std::vector<Placement> placements_;
    // Collision meshes per model (collisionFor), in model space: vertices pushed 3 cm out along the surface normal so the overlay
    // does not fight the model's own faces.
    struct CollisionMesh {
        bool built = false;
        GpuMesh used, usedEdges;          // walkable surfaces on a bone with WSName
        GpuMesh dropped, droppedEdges;    // walkable surfaces on a bone without one
        GpuMesh ignored;                  // box / sphere volumes (outlines)
    };
    std::unordered_map<const Model*, CollisionMesh> collisionMeshes_;
    const CollisionMesh* collisionFor(const Model* model, size_t& buildBudget);
    size_t modelsLoaded_ = 0;
    size_t modelsFailed_ = 0;
    size_t drawnEntities_ = 0;
    size_t fadingForwardPieces_ = 0;
    double queueSectionMs_[6] = {};
    double queueTerrainMs_ = 0;

    // Terrain clutter cells (32 x 32 samples). The client keeps the cells within 3 of the camera's cell
    // (FUN_0073b930), shows a model's instances while the camera is within the model's drawDistance of the cell
    // (FUN_0074c040; the cell point it measures from is taken as the instances' bounding sphere - UNVERIFIED), and
    // draws round(count x density factor) of each model's shuffled instances (FUN_00751050), the factor being
    // FloraDensity x the RQS flora scalar - 1 at full quality, which the editor assumes.
    struct ClutterInstance {
        Model* model = nullptr;
        Mat4 world;
        tr::Vec3 position;
        float scale = 1;
    };
    struct ClutterGroup {                        // one tribble model's instances in a cell
        Model* model = nullptr;
        float drawDistance = 0;
        std::vector<ClutterInstance> instances;
    };
    struct ClutterCell {
        uint32_t index = 0;
        tr::Vec3 centre;
        float radius = 0;
        std::vector<ClutterGroup> groups;
    };
    static constexpr int tribbleCellRange = 3;
public:
    int clutterCellsPerFrame = 4;                // cells built per frame (viewport_test builds them all at once)
    // Builds missing clutter cells around the camera (up to clutterCellsPerFrame) and drops far ones; queueDraws calls it.
    void updateClutterCells();
private:
    std::vector<ClutterCell> clutterCells_;
    size_t drawnClutter_ = 0;
};
