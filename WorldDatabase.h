#pragma once

// Read access to a Rasa.NET World database (SQLite, e.g. rasaworld.db): the server-side content
// placed on maps. Positions are in the game's left-handed world space (same as .map files);
// rotations are yaw angles in radians (the server builds Quaternion.CreateFromYawPitchRoll(r, 0, 0)).
// Models are authored facing -z, so a rotation r faces (-sin r, 0, -cos r): 0 faces south (-z), as
// confirmed in game. Every row carries a map_context_id; map_info maps those
// to map folder names.
//
// The schema is read from the database itself (PRAGMA table_info); a missing table or column is
// reported in warnings() and that data is skipped, so the editor keeps working as Rasa.NET evolves.
//
// Editing: the file is opened read-write (read-only if that fails). save() compares the content as
// loaded with the edited content and writes only the differences, in a single transaction
// (BEGIN IMMEDIATE ... COMMIT; rolled back on any error). Rows with id 0 are new and are inserted
// (the database assigns the id). Deleting a spawn pool also deletes its spawnpool_arrival rows.
// Control points, regions and mission areas can be edited but not created or deleted. The server
// reads this data at start-up, so it needs a restart to see saved changes.

#include "trformats.h"

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct sqlite3;

struct MapInfo {
    int32_t mapContextId = 0;
    std::string name;
    int32_t version = 0;
    int32_t baseRegion = 0;
};

struct SpawnSlot {
    int32_t creatureId = 0;              // 0 = unused
    int32_t minCount = 0;
    int32_t maxCount = 0;
};

// spawnpool. mode: 0 automatic, 1 control point, 2 scripted. animType: 0 none, 1 hostile (Bane)
// dropship, 2 friendly dropship.
struct SpawnPool {
    int64_t id = 0;
    int32_t mode = 0;
    int32_t animType = 0;
    int32_t respawnTime = 0;
    tr::Vec3 position;
    float rotation = 0;
    int32_t mapContextId = 0;
    SpawnSlot slot1, slot2, slot3, slot4, slot5, slot6;
    float radius = 0;
};

const SpawnSlot& spawnSlot(const SpawnPool& pool, int index);          // index 0-5
SpawnSlot& spawnSlot(SpawnPool& pool, int index);
const char* spawnModeName(int32_t mode);
const char* spawnAnimationName(int32_t animType);

struct SpawnArrival {                    // spawnpool_arrival: where a pool's dropship lands
    int64_t id = 0;
    int64_t poolId = 0;
    int32_t kind = 0;
    tr::Vec3 position;
    float rotation = 0;
    int64_t entityId = 0;
    std::string comment;
};

struct Creature {
    int32_t id = 0;
    std::string comment;                 // usually a readable name, e.g. "Council Elder Moawi"
    int32_t classId = 0;
    int32_t faction = 0;
    int32_t level = 0;
    int32_t maxHp = 0;
    int32_t nameId = 0;
    int32_t runSpeed = 0;
    int32_t walkSpeed = 0;
};

// creature_appearance (id = creature id): one equipped item class per equipment slot (Rasa.NET EquipmentData:
// 2 Shoes, 13 Weapon, 14 Hair, 15 Torso, 16 Legs, 17 Face, ...). colour is ABGR (red in the low byte), the hue
// the client adds to the item's pieces; slot 17's colour is the skin tone.
struct AppearanceSlot {
    int32_t slotId = 0;
    int32_t classId = 0;
    uint32_t colour = 0;
};

struct Teleporter {
    int64_t id = 0;
    int32_t classId = 0;
    int32_t type = 0;
    std::string description;
    tr::Vec3 position;
    float rotation = 0;
    int32_t mapContextId = 0;
};

struct MapLink {
    int64_t id = 0;
    int32_t mapContextId = 0;
    tr::Vec3 position;
    float radius = 0;
    int32_t destinationMapContextId = 0;
    tr::Vec3 destinationPosition;
    float destinationRotation = 0;
    int32_t kind = 0;
    bool enabled = true;
    std::string comment;
};

// map_region and mission_area. shape: 1 circle (radius), 2 box (halfX / halfZ around position).
struct AreaShape {
    int32_t shape = 1;
    tr::Vec3 position;
    float radius = 0;
    float halfX = 0;
    float halfZ = 0;
    float minY = -2000;
    float maxY = 2000;
};

struct MapRegion {
    int64_t id = 0;
    int32_t mapContextId = 0;
    int32_t regionId = 0;
    AreaShape area;
    bool underground = false;
    bool enabled = true;
    std::string comment;
};

struct MissionArea {
    int32_t missionId = 0;
    std::string contentRevision;         // part of the key (mission_id, content_revision, area_id)
    int32_t areaId = 0;
    int32_t mapContextId = 0;
    AreaShape area;
    std::string comment;
};

// Other placed objects with an entity class: Logos stones, control points, footlockers, Kraftwerks.
enum class ObjectKind : uint8_t { Logos, ControlPoint, Footlocker, Kraftwerk };
const char* objectKindName(ObjectKind kind);

struct PlacedObject {
    ObjectKind kind = ObjectKind::Logos;
    int64_t id = 0;
    int32_t classId = 0;
    int32_t mapContextId = 0;
    tr::Vec3 position;
    float rotation = 0;
    std::string name;                    // name or comment column
};

// Everything placed on one map context.
struct WorldContent {
    int32_t mapContextId = -1;
    std::vector<SpawnPool> spawnPools;
    std::vector<SpawnArrival> arrivals;  // of those pools
    std::vector<Teleporter> teleporters;
    std::vector<MapLink> mapLinks;
    std::vector<MapRegion> regions;
    std::vector<MissionArea> missionAreas;
    std::vector<PlacedObject> objects;
};

class WorldDatabase {
public:
    explicit WorldDatabase(const std::string& path);       // throws std::runtime_error
    ~WorldDatabase();
    WorldDatabase(const WorldDatabase&) = delete;
    WorldDatabase& operator=(const WorldDatabase&) = delete;

    const std::string& path() const { return path_; }
    bool readOnly() const { return readOnly_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    std::vector<std::pair<std::string, int64_t>> tableCounts() const;

    const std::vector<MapInfo>& maps() const { return maps_; }
    int32_t contextIdForMap(const std::string& mapName) const;    // case-insensitive; -1 if unknown
    const Creature* creature(int32_t id) const;
    size_t creatureCount() const { return creatures_.size(); }
    const std::vector<AppearanceSlot>& appearance(int32_t creatureId) const;     // empty if none, sorted by slot
    // From entityclass.aug_list: augmentation 52 = NPC, 12 = vendor.
    bool classIsNpc(int32_t classId) const { return npcClasses_.count(classId) != 0; }
    bool classIsVendor(int32_t classId) const { return vendorClasses_.count(classId) != 0; }

    WorldContent loadMap(int32_t mapContextId);

    struct SaveSummary {
        int inserted = 0;
        int updated = 0;
        int deleted = 0;
    };
    // Writes the differences between `saved` (as loaded) and `edited` in one transaction.
    // Returns false (with nothing written) and sets error on failure.
    bool save(const WorldContent& saved, const WorldContent& edited, SaveSummary& summary, std::string& error);
    const std::vector<Creature>& creatureList() const { return creatureList_; }   // sorted by id

private:
    bool hasTable(const std::string& table) const;
    bool requireColumns(const std::string& table, std::initializer_list<const char*> columns);
    void loadGlobals();

    void execute(const std::string& sql);

    sqlite3* db_ = nullptr;
    bool readOnly_ = false;
    std::vector<Creature> creatureList_;
    std::string path_;
    std::vector<std::string> warnings_;
    std::unordered_map<std::string, std::set<std::string>> columns_;    // table -> column names
    std::vector<MapInfo> maps_;
    std::unordered_map<int32_t, Creature> creatures_;
    std::unordered_map<int32_t, std::vector<AppearanceSlot>> appearances_;   // creature id -> slots
    std::unordered_set<int32_t> npcClasses_;
    std::unordered_set<int32_t> vendorClasses_;
};
