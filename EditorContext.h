#pragma once

#include "assetlibrary.h"
#include "gamedata.h"
#include "RadarMap.h"
#include "Renderer.h"
#include "SceneView.h"
#include "TerrainOverview.h"

#include <SDL3/SDL.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// What the editor knows about an entity class (from generated.client.entityclass).
struct ClassInfo {
    std::string name;
    std::string mesh;            // .geo file name; empty if the class has no mesh
    bool known = false;          // false: the class id is not in the shipped tables
    int32_t cullingLayer = 3;    // vizsetting culling layer type (6 = never drawn)
};

// How the 3D viewport shows the map, shared by both workspaces so switching keeps the same view
// (the camera itself lives in the shared SceneView). The server layers are per workspace (ViewportOptions).
struct ViewSettings {
    bool showTextures = true;
    bool showTerrain = true;
    bool showEntities = true;
    bool showEffects = true;
    bool showParticles = true;   // particle systems (.pfx) on models' contact points
    bool showClutter = true;     // terrain clutter (tribbles: grass, ferns, flowers, debris)
    bool showPlaceholders = true;
    float drawDistance = 1200.0f;
    float flySpeed = 20.0f;      // metres per second
    bool skyLighting = true;     // light from the map region's .sky (the time of day and sky choice are per map, in SceneView)
    bool fog = true;
    bool sun = true;             // the sky's sun/moon
    bool shadows = true;         // sun shadows (shadow map around the camera)
    bool mapLights = true;       // the map's lights.xml
};

// State shared by both workspaces: settings, the game install and its data tables, the open map with
// its renderer and scene, and work queued for between frames. It knows nothing about workspaces;
// the Editor tells them when the game or map changes.
class EditorContext {
public:
    EditorContext(SDL_Window* window, SDL_GPUDevice* device);
    EditorContext(const EditorContext&) = delete;
    EditorContext& operator=(const EditorContext&) = delete;

    // Settings: key=value lines in the user's preference folder, saved on every change.
    std::string setting(const std::string& key, const std::string& fallback = "") const;
    void setSetting(const std::string& key, const std::string& value);

    void setStatus(const std::string& text, bool isError);
    const ClassInfo& classInfo(int32_t classId);

    // Loading. Call between frames (they replace GPU textures and the scene). false on failure (status set).
    bool openGame(const std::string& root);
    bool openMap(const std::string& name);
    void releaseGpuResources();

    // Work queued from dialogs (any thread) or the UI, taken by the Editor between frames.
    struct Pending {
        std::string gameRoot;
        std::string map;
        std::string database;
    };
    void queueGameRoot(const std::string& root);
    void queueMap(const std::string& name);
    void queueDatabase(const std::string& path);
    Pending takePending();
    void showGameFolderDialog();

    SDL_Window* const window;
    SDL_GPUDevice* const device;
    std::string imguiIniPath;

    // Game
    std::string gameRoot;
    std::unique_ptr<tr::AssetLibrary> assets;
    std::unique_ptr<tr::GameData> gameData;
    std::vector<std::string> mapNames;

    // Open map
    std::string currentMapName;
    std::unique_ptr<tr::MapBundle> map;
    int32_t mapTemplateId = -1;
    std::string mapTemplateName;
    int32_t radarContextId = -1;
    std::unique_ptr<TerrainOverview> overview;          // hill-shaded terrain texture
    std::unique_ptr<RadarMap> radar;                    // the game's own map image
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<SceneView> scene;
    ViewSettings view;

    std::string statusText;
    bool statusIsError = false;

private:
    static void SDLCALL folderDialogCallback(void* userdata, const char* const* filelist, int filter);
    int32_t findMapTemplateId(const std::string& mapName);
    void loadSettings();
    void saveSettings() const;

    std::string settingsPath_;
    std::unordered_map<std::string, std::string> settings_;
    std::unordered_map<int32_t, ClassInfo> classInfoCache_;
    std::mutex pendingMutex_;
    Pending pending_;
};
