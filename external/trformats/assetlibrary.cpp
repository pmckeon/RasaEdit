// assetlibrary.cpp - see assetlibrary.h.
#include "assetlibrary.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace tr {

namespace {

std::string trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) { return toLowerAscii(a) == toLowerAscii(b); }

// Case-insensitive lookup of one directory entry (Windows file names are case-insensitive).
fs::path findChildIgnoreCase(const fs::path& directory, const std::string& name) {
    std::error_code ec;
    const fs::path direct = directory / name;
    if (fs::exists(direct, ec)) return direct;
    if (!fs::is_directory(directory, ec)) return {};
    const std::string wanted = toLowerAscii(name);
    for (const auto& entry : fs::directory_iterator(directory, ec))
        if (toLowerAscii(entry.path().filename().string()) == wanted) return entry.path();
    return {};
}

}  // namespace

// ------------------------------------------------------------------------------------------
// ConfigFile
// ------------------------------------------------------------------------------------------
ConfigFile::ConfigFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw FormatError("cannot open " + path);
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            sections_.emplace_back(trim(line.substr(1, line.size() - 2)), std::vector<Item>());
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || sections_.empty()) continue;
        Item item;
        item.key = trim(line.substr(0, eq));
        item.value = trim(line.substr(eq + 1));
        if (item.value.size() >= 2 && item.value.front() == '"' && item.value.back() == '"')
            item.value = item.value.substr(1, item.value.size() - 2);
        sections_.back().second.push_back(std::move(item));
    }
}

const std::vector<ConfigFile::Item>* ConfigFile::section(const std::string& name) const {
    for (const auto& s : sections_)
        if (equalsIgnoreCase(s.first, name)) return &s.second;
    return nullptr;
}

std::string ConfigFile::value(const std::string& sectionName, const std::string& key, const std::string& fallback) const {
    if (const auto* items = section(sectionName))
        for (const Item& item : *items)
            if (equalsIgnoreCase(item.key, key)) return item.value;
    return fallback;
}

uint32_t configKeyHash(const std::string& key) {
    uint32_t h = 5381;                                                    // FUN_007086b0
    for (char c : key) h = h * 33u + uint32_t(int32_t(static_cast<signed char>(std::tolower(static_cast<unsigned char>(c)))));
    return h;
}

std::vector<std::string> configKeyOrder(const std::vector<std::string>& keys) {
    // Unique keys (case-insensitive), then the table's final shape: FUN_00478cd0 grows before an insert when maxidx <= size / 4 -
    // the mask doubles (+1) once maxidx passes it, and the bucket vector (8 + 1 at first) is doubled when maxidx reaches its end.
    std::vector<std::string> unique;
    for (const std::string& k : keys) {
        bool seen = false;
        for (const std::string& u : unique) seen = seen || equalsIgnoreCase(u, k);
        if (!seen) unique.push_back(k);
    }
    uint32_t mask = 1, maxidx = 1, vecSize = 9;
    for (size_t n = 0; n < unique.size(); ++n) {
        if (maxidx <= n >> 2) {
            if (maxidx < vecSize - 1) {
                if (mask < maxidx) mask = mask * 2 + 1;
            } else {
                mask = vecSize * 2 - 3;
                vecSize = vecSize * 2 - 1;
            }
            ++maxidx;
        }
    }
    auto bucket = [&](const std::string& k) {
        uint32_t b = configKeyHash(k) & mask;
        if (b >= maxidx) b -= (mask >> 1) + 1;
        return b;
    };
    std::stable_sort(unique.begin(), unique.end(), [&](const std::string& a, const std::string& b) {
        const uint32_t ba = bucket(a), bb = bucket(b);
        if (ba != bb) return ba < bb;
        return toLowerAscii(a) < toLowerAscii(b);                         // _stricmp within a bucket
    });
    return unique;
}

// ------------------------------------------------------------------------------------------
// AssetLibrary
// ------------------------------------------------------------------------------------------
AssetLibrary::AssetLibrary(const std::string& gameRoot, const std::string& language) : root_(gameRoot) {
    const fs::path cfgPath = findChildIgnoreCase(fs::path(root_), "tabula_rasa.cfg");
    if (cfgPath.empty()) throw FormatError("no tabula_rasa.cfg in " + root_);
    config_ = std::make_unique<ConfigFile>(cfgPath.string());
    if (!equalsIgnoreCase(language, "English")) mountSection(*config_, "Paths." + language);
    mountSection(*config_, "Paths");
    for (uint32_t k = 0; k < loose_.size(); ++k)
        if (loose_[k].indexed) indexLooseDirectory(k);
}

std::string AssetLibrary::resolve(const std::string& relative) const {
    std::string r = relative;
    std::replace(r.begin(), r.end(), '\\', '/');
    return (fs::path(root_) / fs::path(r)).lexically_normal().string();
}

void AssetLibrary::mountSection(const ConfigFile& cfg, const std::string& name) {
    const auto* section = cfg.section(name);
    if (!section) return;
    // The game takes a section's entries in its hash-table order (configKeyOrder), the first key winning for a file several
    // archives hold. Shipped file names are unique across archives, so the order only matters for a patch archive. A repeated key
    // keeps its last value.
    std::vector<std::string> keys;
    for (const ConfigFile::Item& item : *section) keys.push_back(item.key);
    std::vector<ConfigFile::Item> items;
    for (const std::string& key : configKeyOrder(keys)) {
        ConfigFile::Item chosen;
        for (const ConfigFile::Item& item : *section)
            if (equalsIgnoreCase(item.key, key)) chosen = item;
        items.push_back(chosen);
    }
    for (const ConfigFile::Item& item : items) {
        const std::string& value = item.value;
        if (value.find(".glm") != std::string::npos) {                 // archive (client: rfind(".glm"))
            const std::string path = resolve(value);
            std::error_code ec;
            if (!fs::exists(path, ec)) {
                warnings_.push_back("[" + name + "] " + item.key + ": archive not found: " + path);
                continue;
            }
            try {
                MountedArchive mounted;
                mounted.configKey = item.key;
                mounted.archive = std::make_unique<GlmArchive>(path);
                const auto archiveIndex = uint32_t(archives_.size());
                for (const GlmEntry& entry : mounted.archive->entries()) {
                    AssetLocation location;
                    location.inArchive = true;
                    location.archiveIndex = archiveIndex;
                    location.entry = &entry;
                    index_.emplace(toLowerAscii(entry.name), location);  // first registration wins
                }
                archives_.push_back(std::move(mounted));
            } catch (const std::exception& e) {
                warnings_.push_back("[" + name + "] " + item.key + ": " + e.what());
            }
        } else if (value.find('|') != std::string::npos) {               // "dir|.ext|.ext"
            LooseDirectory dir;
            dir.configKey = item.key;
            const size_t bar = value.find('|');
            dir.path = resolve(value.substr(0, bar));
            size_t start = bar + 1;
            while (start <= value.size()) {
                const size_t next = value.find('|', start);
                const std::string ext = toLowerAscii(trim(value.substr(start, next - start)));
                if (!ext.empty()) dir.extensions.push_back(ext);
                if (next == std::string::npos) break;
                start = next + 1;
            }
            dir.indexed = std::find(dir.extensions.begin(), dir.extensions.end(), ".xxx") == dir.extensions.end();
            loose_.push_back(std::move(dir));
        }
    }
}

void AssetLibrary::indexLooseDirectory(uint32_t directoryIndex) {
    const LooseDirectory& dir = loose_[directoryIndex];
    std::error_code ec;
    if (!fs::is_directory(dir.path, ec)) {
        warnings_.push_back(dir.configKey + ": directory not found: " + dir.path);
        return;
    }
    for (auto it = fs::recursive_directory_iterator(dir.path, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = toLowerAscii(it->path().extension().string());
        if (std::find(dir.extensions.begin(), dir.extensions.end(), ext) == dir.extensions.end()) continue;
        AssetLocation location;
        location.filePath = it->path().string();
        index_.emplace(toLowerAscii(it->path().filename().string()), location);   // archives keep priority
    }
}

std::optional<AssetLocation> AssetLibrary::locateLocked(const std::string& name) const {
    const auto it = index_.find(toLowerAscii(name));
    if (it == index_.end()) return std::nullopt;
    return it->second;
}

std::optional<AssetLocation> AssetLibrary::locate(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mountMutex_);
    return locateLocked(name);
}

void AssetLibrary::rebuildIndex() {
    // As the constructor builds it: archives in mount order, first registration winning, then the indexed loose directories.
    index_.clear();
    for (uint32_t a = 0; a < archives_.size(); ++a) {
        if (!archives_[a].archive) continue;
        for (const GlmEntry& entry : archives_[a].archive->entries()) {
            AssetLocation location;
            location.inArchive = true;
            location.archiveIndex = a;
            location.entry = &entry;
            index_.emplace(toLowerAscii(entry.name), location);
        }
    }
    for (uint32_t k = 0; k < loose_.size(); ++k)
        if (loose_[k].indexed) indexLooseDirectory(k);
}

void AssetLibrary::rewriteArchive(const std::string& path, const std::function<void()>& write) {
    std::unique_lock<std::shared_mutex> lock(mountMutex_);
    const auto key = [](const std::string& p) { return toLowerAscii(fs::path(p).lexically_normal().generic_string()); };
    int32_t mounted = -1;
    for (size_t a = 0; a < archives_.size(); ++a)
        if (archives_[a].archive && key(archives_[a].archive->path()) == key(path)) mounted = int32_t(a);
    if (mounted < 0) {
        write();
        return;
    }
    archives_[size_t(mounted)].archive.reset();                       // closes the file
    std::exception_ptr failure;
    try {
        write();
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        archives_[size_t(mounted)].archive = std::make_unique<GlmArchive>(path);
    } catch (const std::exception& e) {
        warnings_.push_back(path + ": not mounted again: " + e.what());
    }
    rebuildIndex();
    if (failure) std::rethrow_exception(failure);
}

void AssetLibrary::setOverride(const std::string& name, Bytes data) {
    std::lock_guard<std::mutex> lock(overridesMutex_);
    overrides_[toLowerAscii(name)] = {name, std::move(data)};
}

void AssetLibrary::clearOverrides() {
    std::lock_guard<std::mutex> lock(overridesMutex_);
    overrides_.clear();
}

bool AssetLibrary::hasOverride(const std::string& name) const {
    std::lock_guard<std::mutex> lock(overridesMutex_);
    return overrides_.count(toLowerAscii(name)) != 0;
}

std::vector<std::string> AssetLibrary::overrideNames() const {
    std::lock_guard<std::mutex> lock(overridesMutex_);
    std::vector<std::string> names;
    for (const auto& o : overrides_) names.push_back(o.second.first);
    std::sort(names.begin(), names.end());
    return names;
}

Bytes AssetLibrary::read(const std::string& name) const {
    {
        std::lock_guard<std::mutex> lock(overridesMutex_);
        if (auto it = overrides_.find(toLowerAscii(name)); it != overrides_.end()) return it->second.second;
    }
    std::shared_lock<std::shared_mutex> lock(mountMutex_);
    const auto location = locateLocked(name);
    if (!location) throw FormatError("asset not found: " + name);
    if (location->inArchive) return archives_[location->archiveIndex].archive->read(*location->entry);
    return readFile(location->filePath);
}

bool AssetLibrary::tryRead(const std::string& name, Bytes& out) const {
    {
        std::lock_guard<std::mutex> lock(overridesMutex_);
        if (auto it = overrides_.find(toLowerAscii(name)); it != overrides_.end()) {
            out = it->second.second;
            return true;
        }
    }
    std::shared_lock<std::shared_mutex> lock(mountMutex_);
    const auto location = locateLocked(name);
    if (!location) return false;
    out = location->inArchive ? archives_[location->archiveIndex].archive->read(*location->entry) : readFile(location->filePath);
    return true;
}

std::vector<std::string> AssetLibrary::namesWithExtension(const std::string& extension) const {
    const std::string ext = toLowerAscii(extension);
    std::vector<std::string> names;
    std::shared_lock<std::shared_mutex> lock(mountMutex_);
    for (const auto& item : index_)
        if (item.first.size() >= ext.size() && item.first.compare(item.first.size() - ext.size(), ext.size(), ext) == 0)
            names.push_back(item.second.inArchive ? item.second.entry->name : fs::path(item.second.filePath).filename().string());
    {
        std::lock_guard<std::mutex> overridesLock(overridesMutex_);
        for (const auto& o : overrides_)
            if (o.first.size() >= ext.size() && o.first.compare(o.first.size() - ext.size(), ext.size(), ext) == 0 && !index_.count(o.first))
                names.push_back(o.second.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> AssetLibrary::mapNames() const {
    std::vector<std::string> names;
    for (const LooseDirectory& dir : loose_) {
        if (!equalsIgnoreCase(dir.configKey, "Maps")) continue;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir.path, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::string name = entry.path().filename().string();
            if (!findChildIgnoreCase(entry.path(), name + ".map").empty()) names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string AssetLibrary::mapFolder(const std::string& mapName) const {
    for (const LooseDirectory& dir : loose_) {
        if (!equalsIgnoreCase(dir.configKey, "Maps")) continue;
        const fs::path folder = findChildIgnoreCase(fs::path(dir.path), mapName);
        if (!folder.empty()) return folder.string();
    }
    return "";
}

std::string AssetLibrary::terrainArchivePath(const std::string& mapName) const {
    const std::string folder = mapFolder(mapName);
    if (folder.empty()) return "";
    static const std::regex pattern("t[0-9a-fA-F]{8}_terrain\\.glm", std::regex::icase);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec))
        if (std::regex_match(entry.path().filename().string(), pattern)) return entry.path().string();
    return "";
}

std::string AssetLibrary::radarArchivePath(const std::string& mapName) const {
    for (const LooseDirectory& dir : loose_) {
        if (dir.indexed) continue;                                       // the ".xxx" load-by-name directory
        const fs::path radar = findChildIgnoreCase(fs::path(dir.path), mapName + "_radar.glm");
        if (!radar.empty()) return radar.string();
    }
    return "";
}

std::string AssetLibrary::gameDataPath() const {
    const std::string value = config_->value("Data", "GameData");
    return value.empty() ? "" : resolve(value);
}

// ------------------------------------------------------------------------------------------
// MapBundle
// ------------------------------------------------------------------------------------------
MapBundle loadMap(const AssetLibrary& library, const std::string& mapName) {
    MapBundle bundle;
    bundle.folder = library.mapFolder(mapName);
    if (bundle.folder.empty()) throw FormatError("map not found: " + mapName);
    bundle.name = fs::path(bundle.folder).filename().string();
    const fs::path folder(bundle.folder);
    const fs::path mapPath = findChildIgnoreCase(folder, bundle.name + ".map");
    if (mapPath.empty()) throw FormatError("map folder has no .map: " + bundle.folder);
    bundle.map = readMap(mapPath.string());
    const fs::path visPath = findChildIgnoreCase(folder, bundle.name + ".vis");
    if (!visPath.empty()) bundle.vis = readVis(visPath.string());
    const std::string terrainPath = library.terrainArchivePath(bundle.name);
    if (!terrainPath.empty()) {
        bundle.terrainArchive = std::make_unique<GlmArchive>(terrainPath);
        bundle.terrain = readTerrain(*bundle.terrainArchive);
        bundle.textureLayers = readTextureLayers(*bundle.terrainArchive);
        bundle.tribbleSets = readTribbleSets(*bundle.terrainArchive);
        bundle.hasTerrain = true;
    }
    const fs::path lights = findChildIgnoreCase(folder, "lights.xml");
    if (!lights.empty()) bundle.lightsXml = readFile(lights.string());
    const fs::path audio = findChildIgnoreCase(folder, "audioemitters.xml");
    if (!audio.empty()) bundle.audioEmittersXml = readFile(audio.string());
    bundle.radarArchivePath = library.radarArchivePath(bundle.name);
    return bundle;
}

}  // namespace tr
