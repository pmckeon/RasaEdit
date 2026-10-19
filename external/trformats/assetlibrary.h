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

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
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

// The order tabula_rasa.exe takes a tabula_rasa.cfg section's keys in (traced 2026-10-19, CLAUDE.md "Which archive wins"): a VC8
// stdext hash table - djb2 of the lower-cased key (FUN_007086b0), linear-hashing buckets that grow at 4 keys per bucket (a split moves
// keys to the new, last bucket), each bucket sorted case-insensitively; the keys come out bucket by bucket. The game queues archives
// in this order, mounts them in reverse and uses a name's last-mounted entry, so for a file in several archives the FIRST key here
// wins. A repeated key keeps one place (its last value, FUN_00478430). Verified in game: AAA_RasaEdit beat Mesh03 / Animation02 /
// Texture03 but not Mesh10; AAA_RasaEdit39 beats every stock key.
std::vector<std::string> configKeyOrder(const std::vector<std::string>& keys);
uint32_t configKeyHash(const std::string& key);

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
    bool exists(const std::string& name) const { return hasOverride(name) || locate(name).has_value(); }
    Bytes read(const std::string& name) const;                       // throws FormatError if absent
    bool tryRead(const std::string& name, Bytes& out) const;
    std::vector<std::string> namesWithExtension(const std::string& extension) const;   // e.g. ".geo"

    // In-memory files that shadow the mounted ones (by name, case-insensitive) for read / tryRead / exists / namesWithExtension:
    // models and textures imported from glTF, previewed before they go into the patch archive. Thread-safe.
    void setOverride(const std::string& name, Bytes data);
    void clearOverrides();
    bool hasOverride(const std::string& name) const;
    std::vector<std::string> overrideNames() const;

    // Rewrites an archive the library may have mounted (the patch archive): the archive is closed first - Windows will not replace
    // an open file - then `write` runs, and the archive is mounted again with the index rebuilt (its entries moved), so reads see
    // the new contents. Reads wait meanwhile (all of them take the mount lock). A path the library has not mounted just runs
    // `write`. If `write` throws, the file as it then is is mounted again and the exception passed on.
    void rewriteArchive(const std::string& path, const std::function<void()>& write);

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
    std::optional<AssetLocation> locateLocked(const std::string& name) const;
    void rebuildIndex();

    std::string root_;
    std::unique_ptr<ConfigFile> config_;
    std::vector<MountedArchive> archives_;
    std::vector<LooseDirectory> loose_;
    std::unordered_map<std::string, AssetLocation> index_;          // lower-case name -> location
    std::vector<std::string> warnings_;
    mutable std::shared_mutex mountMutex_;                           // archives_ / index_: shared by reads, exclusive while rewriting
    mutable std::mutex overridesMutex_;
    std::unordered_map<std::string, std::pair<std::string, Bytes>> overrides_;   // lower-case name -> (name, data)
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
