#include "EditorContext.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

EditorContext::EditorContext(SDL_Window* window_, SDL_GPUDevice* device_) : window(window_), device(device_) {
    if (char* pref = SDL_GetPrefPath("RasaEdit", "RasaEdit")) {
        settingsPath_ = std::string(pref) + "settings.ini";
        imguiIniPath = std::string(pref) + "imgui.ini";
        SDL_free(pref);
    } else {
        settingsPath_ = "rasaedit_settings.ini";
        imguiIniPath = "imgui.ini";
    }
    try {
        renderer = std::make_unique<Renderer>(device);
    } catch (const std::exception& e) {
        setStatus(std::string("3D viewport unavailable: ") + e.what(), true);
    }
    loadSettings();
}

// ------------------------------------------------------------------------------------------
// Settings, status
// ------------------------------------------------------------------------------------------
void EditorContext::loadSettings() {
    std::ifstream file(settingsPath_);
    std::string line;
    while (std::getline(file, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) settings_[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void EditorContext::saveSettings() const {
    std::ofstream file(settingsPath_);
    for (const auto& s : settings_) file << s.first << "=" << s.second << "\n";
}

std::string EditorContext::setting(const std::string& key, const std::string& fallback) const {
    const auto it = settings_.find(key);
    return it == settings_.end() ? fallback : it->second;
}

void EditorContext::setSetting(const std::string& key, const std::string& value) {
    settings_[key] = value;
    saveSettings();
}

void EditorContext::setStatus(const std::string& text, bool isError) {
    statusText = text;
    statusIsError = isError;
}

// ------------------------------------------------------------------------------------------
// Queued work
// ------------------------------------------------------------------------------------------
void EditorContext::queueGameRoot(const std::string& root) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.gameRoot = root;
}

void EditorContext::queueMap(const std::string& name) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.map = name;
}

void EditorContext::queueDatabase(const std::string& path) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.database = path;
}

EditorContext::Pending EditorContext::takePending() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    Pending taken = pending_;
    pending_ = Pending{};
    return taken;
}

void SDLCALL EditorContext::folderDialogCallback(void* userdata, const char* const* filelist, int) {
    if (filelist == nullptr || filelist[0] == nullptr) return;   // error or cancelled
    static_cast<EditorContext*>(userdata)->queueGameRoot(filelist[0]);
}

void EditorContext::showGameFolderDialog() {
    SDL_ShowOpenFolderDialog(folderDialogCallback, this, window, gameRoot.empty() ? nullptr : gameRoot.c_str(), false);
}

// ------------------------------------------------------------------------------------------
// Game and map
// ------------------------------------------------------------------------------------------
bool EditorContext::openGame(const std::string& root) {
    try {
        auto newAssets = std::make_unique<tr::AssetLibrary>(root);
        std::unique_ptr<tr::GameData> newGameData;
        const std::string gameDataPath = newAssets->gameDataPath();
        std::error_code ec;
        if (!gameDataPath.empty() && fs::exists(gameDataPath, ec)) newGameData = std::make_unique<tr::GameData>(gameDataPath);

        scene.reset();
        overview.reset();
        radar.reset();
        map.reset();
        currentMapName.clear();
        classInfoCache_.clear();
        assets = std::move(newAssets);
        gameData = std::move(newGameData);
        gameRoot = root;
        mapNames = assets->mapNames();
        setSetting("gameRoot", root);
        setStatus("Opened " + root + (gameData ? "" : "  (no game data zip: class names unavailable)"), !gameData);
        return true;
    } catch (const std::exception& e) {
        setStatus(std::string("Could not open game folder: ") + e.what(), true);
        return false;
    }
}

int32_t EditorContext::findMapTemplateId(const std::string& mapName) {
    if (!gameData) return -1;
    const tr::py::Object* lookup = gameData->global("generated.client.maptemplate", "lookup");
    if (lookup == nullptr || lookup->kind != tr::py::Kind::Dict) return -1;
    const std::string wanted = tr::toLowerAscii(mapName);
    for (const auto& entry : lookup->entries)
        if (tr::toLowerAscii(tr::py::asString(entry.second.get())) == wanted) return int32_t(tr::py::asInt(entry.first.get()));
    return -1;
}

bool EditorContext::openMap(const std::string& name) {
    if (!assets) return false;
    try {
        auto bundle = std::make_unique<tr::MapBundle>(tr::loadMap(*assets, name));
        std::unique_ptr<TerrainOverview> newOverview;
        if (bundle->hasTerrain) newOverview = std::make_unique<TerrainOverview>(device, bundle->terrain);

        // Map template id: from the map-template table by name (works for interior maps too),
        // else from the terrain archive name t<id hex>_terrain.glm.
        int32_t templateId = -1;
        try { templateId = findMapTemplateId(bundle->name); } catch (const std::exception&) {}
        if (templateId < 0 && bundle->terrainArchive) {
            const std::string file = fs::path(bundle->terrainArchive->path()).filename().string();
            templateId = int32_t(std::stoul(file.substr(1, 8), nullptr, 16));
        }

        // The game's radar map: first game context for this template that has radar data for region 0.
        std::unique_ptr<RadarMap> newRadar;
        int32_t contextId = -1;
        std::string radarProblem;
        if (gameData && templateId >= 0 && !bundle->radarArchivePath.empty()) {
            try {
                for (int32_t context : gameData->contextIdsForMapTemplate(templateId)) {
                    tr::RadarInfo info;
                    if (!gameData->radarInfo(context, 0, info)) continue;
                    newRadar = std::make_unique<RadarMap>(device, bundle->radarArchivePath, templateId, 0, info);
                    contextId = context;
                    break;
                }
            } catch (const std::exception& e) {
                radarProblem = std::string("  (radar map: ") + e.what() + ")";
            }
        }

        scene.reset();                                   // references the old bundle
        overview = std::move(newOverview);               // old textures released here (no draw list holds them)
        radar = std::move(newRadar);
        map = std::move(bundle);
        if (renderer) {
            scene = std::make_unique<SceneView>(*renderer, *assets, *map, [this](int32_t id) { return classInfo(id).mesh; },
                                                [this](int32_t id) { return !gameData || classInfo(id).known; });
        }
        currentMapName = map->name;
        mapTemplateId = templateId;
        radarContextId = contextId;
        mapTemplateName = (gameData && templateId >= 0) ? gameData->mapTemplateName(templateId) : "";
        setSetting("map", map->name);
        setStatus("Loaded map " + map->name + radarProblem, !radarProblem.empty());
        return true;
    } catch (const std::exception& e) {
        setStatus("Could not load map " + name + ": " + e.what(), true);
        return false;
    }
}

const ClassInfo& EditorContext::classInfo(int32_t classId) {
    const auto it = classInfoCache_.find(classId);
    if (it != classInfoCache_.end()) return it->second;
    ClassInfo info;
    if (gameData) {
        try {
            info.name = gameData->className(classId);
            info.mesh = gameData->classMeshName(classId);
            info.known = !info.name.empty();
        } catch (const std::exception&) {
            info.known = false;
        }
    }
    return classInfoCache_.emplace(classId, std::move(info)).first->second;
}

void EditorContext::releaseGpuResources() {
    scene.reset();
    renderer.reset();
    overview.reset();
    radar.reset();
}
