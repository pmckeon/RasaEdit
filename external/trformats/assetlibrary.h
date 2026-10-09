// assetlibrary.h - client-style asset resolution over a Tabula Rasa install.
//
// Mirrors how tabula_rasa.exe mounts data (read from tabula_rasa.cfg):
//   * Sections [Paths.<Language>] (non-English only) and then [Paths] are read in order.
//   * A value containing ".glm" is an archive. All archive entries form one flat, case-insensitive
//     name index (names are unique across the 40 shipped archives; the first registration wins, so a
//     language archive overrides).
//   * A value "dir|.ext|.ext" maps those extensions to loose files under dir (searched recursively,
//     so data/maps/<map>/<map>.map resolves). Loose files never replace an archive entry (the
//     client's precedence is not confirmed; the shipped data never puts a name in both).
//   * "dir|.xxx" (UIRadar) is not indexed: the client opens <map>_radar.glm by name when a map loads.
//   * [Data] GameData is the zip of compiled data tables (generated.client.*).
// Maps: data/maps/<name>/ holds <name>.map, <name>.vis, lights.xml, audioemitters.xml and
// t<mapTemplateId as %08x>_terrain.glm (no terrain for interior-only maps).
#pragma once

#include "trformats.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tr {

// Minimal reader for the client's INI-style .cfg files: [Section], key = "value", '#' comments.
class ConfigFile {
public:
    explicit ConfigFile(const std::string& path);
    struct Item {
        std::string key;
        std::string value;
    };
    const std::vector<Item>* section(const std::string& name) const;     // case-insensitive
    std::string value(const std::string& section, const std::string& key, const std::string& fallback = "") const;
private:
    std::vector<std::pair<std::string, std::vector<Item>>> sections_;
};

struct AssetLocation {
    bool inArchive = false;
    uint32_t archiveIndex = 0;            // into AssetLibrary::archives()
    const GlmEntry* entry = nullptr;      // when inArchive
    std::string filePath;                 // when loose
};

struct MountedArchive {
    std::string configKey;                // e.g. "Mesh02"
    std::unique_ptr<GlmArchive> archive;
};

struct LooseDirectory {
    std::string configKey;                // e.g. "Maps"
    std::string path;                     // absolute
    std::vector<std::string> extensions;  // lower-case, with the dot
    bool indexed = false;                 // false for the ".xxx" (load-by-name) form
};

class AssetLibrary {
public:
    explicit AssetLibrary(const std::string& gameRoot, const std::string& language = "English");

    const std::string& root() const { return root_; }
    const std::vector<MountedArchive>& archives() const { return archives_; }
    const std::vector<LooseDirectory>& looseDirectories() const { return loose_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    size_t indexedCount() const { return index_.size(); }

    // Any asset by bare file name (case-insensitive), e.g. "arch_hum_tent_medic_01_v01.geo".
    std::optional<AssetLocation> locate(const std::string& name) const;
    bool exists(const std::string& name) const { return locate(name).has_value(); }
    Bytes read(const std::string& name) const;                       // throws FormatError if absent
    bool tryRead(const std::string& name, Bytes& out) const;
    std::vector<std::string> namesWithExtension(const std::string& extension) const;   // e.g. ".geo"

    // Maps.
    std::vector<std::string> mapNames() const;                       // folder names under the Maps directory
    std::string mapFolder(const std::string& mapName) const;         // "" if absent; case-insensitive
    std::string terrainArchivePath(const std::string& mapName) const; // "" for interior-only maps
    std::string radarArchivePath(const std::string& mapName) const;   // "" if absent
    std::string gameDataPath() const;                                 // [Data] GameData, absolute

private:
    std::string resolve(const std::string& relative) const;
    void mountSection(const ConfigFile& cfg, const std::string& name);
    void indexLooseDirectory(uint32_t directoryIndex);

    std::string root_;
    std::unique_ptr<ConfigFile> config_;
    std::vector<MountedArchive> archives_;
    std::vector<LooseDirectory> loose_;
    std::unordered_map<std::string, AssetLocation> index_;          // lower-case name -> location
    std::vector<std::string> warnings_;
};

// Everything the editor needs for one map, loaded through the library.
struct MapBundle {
    std::string name;
    std::string folder;
    MapFile map;
    VisFile vis;
    bool hasTerrain = false;
    std::unique_ptr<GlmArchive> terrainArchive;
    Terrain terrain;
    std::vector<TextureLayer> textureLayers;
    std::vector<TribbleSet> tribbleSets;
    Bytes lightsXml;                      // empty if the file is absent
    Bytes audioEmittersXml;
    std::string radarArchivePath;
};

MapBundle loadMap(const AssetLibrary& library, const std::string& mapName);

}  // namespace tr
