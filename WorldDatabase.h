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

#include "DbConnection.h"
#include "trformats.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


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

// spawnpool_arrival (Rasa.NET SpawnPoolArrivalEntry): where a pool's creatures arrive - kind 1 a landing pad or dropship
// bay the Bane dropship comes down on, kind 2 a Bane teleporter (entity_id = the map's own entity id of it, switched on
// while they come through); the server picks one per spawn and the creatures walk from there to the pool.
const char* arrivalKindName(int32_t kind);
struct SpawnArrival {
    int64_t id = 0;
    int64_t poolId = 0;
    int32_t kind = 0;
    tr::Vec3 position;
    float rotation = 0;
    int64_t entityId = 0;
    std::string comment;
};

// spawnpool_patrol (Rasa.NET upstream SpawnPoolPatrolEntry, walked by Managers.Patrols): a pool's creature walks the steps
// from the lowest step number up, in straight lines (heights included, not on the navmesh: the positions are floor heights),
// and from the last back to the first. At a step it turns to `facing` (yaw radians, same convention as spawnpool.rotation)
// when it has one, stands `pauseMs`, then walks on. A pool without rows has no patrol; one creature to a beat; a creature
// without a walk speed has none; a pool with a patrol has no pose. Key (pool_id, step).
struct PatrolStep {
    int64_t poolId = 0;
    int32_t step = 0;
    tr::Vec3 position;
    bool hasFacing = false;              // facing NULL: keeps facing the way it came
    float facing = 0;
    int32_t pauseMs = 0;
};
// spawnpool_pose (Rasa.NET upstream SpawnPoolPoseEntry / Data.NpcPose): how a pool's creatures stand at their post; a pool
// without a row has no pose. 0 None, 1 Standing, 2 WeaponOut, 3 Crouched, 4 CrouchedWeaponOut, 5 Leaning, 6 Sitting,
// 7 LyingDown, 8 AtConsole, 9 HandTool (5-9: the client's ambient animations, humans only; 8 / 9 female skeleton only).
const char* npcPoseName(int32_t pose);
struct PoolPose {
    int64_t poolId = 0;
    int32_t pose = 0;
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
    // creature.action1..action8: creature_action ids (0 = none).
    int32_t action1 = 0, action2 = 0, action3 = 0, action4 = 0, action5 = 0, action6 = 0, action7 = 0, action8 = 0;
};
int32_t creatureAction(const Creature& creature, int index);   // index 0-7
int32_t& creatureAction(Creature& creature, int index);
// creature.faction is Rasa.NET's TargetCategory: 0 hostile, 1 friendly, 2 object, 3 neutral, 4 decoration, 5 decoration
// proxy, 6 ignore (other values count as hostile, Structures/Creature.cs).
const char* creatureFactionName(int32_t faction);
// Equipment slots (Rasa.NET Data/EquipmentData.cs): 1 Helmet, 2 Shoes, 3 Gloves, 13 Weapon, 14 Hair, 15 Torso, 16 Legs,
// 17 Face, 18 Wing, 19 Eyewear, 20 Beard, 21 Mask, ... "" if unnamed.
const char* equipmentSlotName(int32_t slot);

// creature_stat (id = creature id): the attributes the server gives the creature (CreatureManager: Body, Mind, Spirit,
// Health, Armor); creatures without a row get defaults.
struct CreatureStat {
    int32_t body = 0;
    int32_t mind = 0;
    int32_t spirit = 0;
    int32_t health = 0;
    int32_t armor = 0;
};

// creature_action (read-only here): an attack a creature can use.
struct CreatureAction {
    int32_t id = 0;
    std::string description;
    int32_t actionId = 0;
    int32_t actionArgId = 0;
    float rangeMin = 0;
    float rangeMax = 0;
    int32_t cooldown = 0;
    int32_t windup = 0;
    int32_t minDamage = 0;
    int32_t maxDamage = 0;
    int32_t damageType = 0;
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
    // map_region.underground (Rasa.NET MapRegionEntry): 0 any, 1 underground only, 2 surface only. "Underground" is the
    // player standing on navmesh flagged as lying under the terrain (Rasa.NavMesh NavMeshBuilder.IsUnderTerrain: most of the
    // polygon's corners more than 2 m below the heightmap) - it keeps a cavern's rectangle off the hillside above it.
    int32_t underground = 0;
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
// AmbientNpc, WorldUsable and WorldDestructible are Rasa.NET upstream tables (ambient_npc, world_usable, world_destructible;
// absent on older worlds): server-placed objects no .map places. Their yaw is sent as the other objects' is
// (WorldLocationDescriptorPacket: Quaternion.CreateFromYawPitchRoll(rotation, 0, 0)).
// Emitter: map_emitter (Rasa.NET MapEmitterEntry / EmitterManager): an FX package played at a spot - an FXPackageEmitter entity
// (class 2873, kEmitterClassId; the table has no class column) whose client body is particle_proxy.geo (no pieces) and which
// attaches the package while it is on (client/augmentations/fxpackageemitter.py: body.AttachFXPackage(stringtable[package])).
enum class ObjectKind : uint8_t { Logos, ControlPoint, Footlocker, Kraftwerk, AmbientNpc, WorldUsable, WorldDestructible, Emitter };
constexpr int32_t kEmitterClassId = 2873;
const char* objectKindName(ObjectKind kind);

// world_usable columns (Rasa.NET WorldUsableEntry; 0 in a number column = the kind's default).
struct UsableSettings {
    int64_t state = 0;                   // the usable state it starts in (UseObjectState); 0 = its kind's
    int64_t usable = 0;                  // 1: a player may use it
    int64_t itemTemplateId = 0;          // an item dispenser's item (itemtemplate.id)
    int64_t itemClassId = 0;             // the item an acceptor takes / a bomb is armed with (entity class)
    int64_t itemCount = 0;               // given or taken; 0 = one
    int64_t resetSeconds = 0;            // until it is back as it was (0 for a door or a switch: stays as left)
    int64_t intervalMinSeconds = 0;      // a random explosion's shortest wait, or a bomb's fuse
    int64_t intervalMaxSeconds = 0;      // a random explosion's longest wait
    int64_t damageMin = 0;               // a blast's least damage to each player it reaches
    int64_t damageMax = 0;
    float radius = 0;                    // how far a blast reaches (m)
    int64_t actionArg = 0;               // OBJ_DAMAGE arg: 1 small explosion, 2 large, 3 lightning, 4 mine, 5 bomb, 6 geyser, 7 hornito
};
// world_destructible columns (Rasa.NET WorldDestructibleEntry; 0 = the class's own default).
struct DestructibleSettings {
    int64_t hitPoints = 0;
    int64_t creatureId = 0;              // a creature spawner's creature (creature.id)
    int64_t creatureCount = 0;           // how many come out at a time
    int64_t side = 0;                    // a force field's side: 0 the class's default (Bane's), 1 AFS, 2 Bane
};

// control_point columns beyond the common ones (Rasa.NET ControlPointEntry; both NOT NULL): the client's entity id of the point's
// map marker (a CONTROL_POINT entry of uimapmarker.factionedmarkers - the point's owner is sent under it, 0 = no marker state), and
// who holds it until it first changes hands (0 Bane, 1 AFS; the server reads anything else as Bane). The id is not generated:
// the editor gives a new point the table's largest id + 1 (the server keeps each point's current owner by id in
// control_point_state, so an id is never reused).
struct ControlPointSettings {
    int64_t markerEntityId = 0;
    int64_t defaultOwner = 1;
};

// map_emitter columns.
struct EmitterSettings {
    int64_t packageId = 0;               // the client's string table id of the .pkg
    bool isOn = true;                    // plays from when the map loads (else placed but off until turned on)
};

struct PlacedObject {
    ObjectKind kind = ObjectKind::Logos;
    int64_t id = 0;
    int32_t classId = 0;
    int32_t mapContextId = 0;
    tr::Vec3 position;
    float rotation = 0;
    std::string name;                    // name or comment column
    UsableSettings usable;               // WorldUsable only
    DestructibleSettings destructible;   // WorldDestructible only
    EmitterSettings emitter;             // Emitter only
    ControlPointSettings controlPoint;   // ControlPoint only
};

// wormhole_lock (Rasa.NET upstream WormholeLockEntry, Managers.Wormholes): a condition on travelling between world wormholes
// (teleporter rows of type 3). A row matches a trip from fromTeleporterId to toTeleporterId (0 = any wormhole); every
// matching row must be met. kind 1: the player has taken mission `value` at least once; kind 2: level `value` (not enforced
// yet by the server: logged, the trip is open). Not tied to one map: every row is loaded with each map.
struct WormholeLock {
    int64_t id = 0;
    int64_t fromTeleporterId = 0;
    int64_t toTeleporterId = 0;
    int64_t kind = 1;
    int64_t value = 0;
    bool hasComment = false;             // comment NULL
    std::string comment;
};

// One creature as the editor edits it: its creature row, its creature_stat row (if any) and its appearance slots.
// The NPC and vendor rows are keyed by the creature id too (Rasa.NET CreatureManager.CreatureInit):
//   npc_package (package_id: the NPC's conversation package, sent in NPCInfoPacket; comment), npc_greeting (greeting_id:
//   the client's npcgreetinglanguage line; none = "Greetings."), creature_actor_name (a name the server gives in place of
//   creaturenamelanguage), vendor (package_id), vendor_price (one price for every item it sells, in place of buy_price),
//   vendor_item (its stock: item template ids). Only creatures whose class has the vendor augmentation get a vendor.
struct CreatureDefinition {
    Creature creature;
    bool hasStat = false;
    CreatureStat stat;
    std::vector<AppearanceSlot> appearance;          // sorted by slot
    bool hasNpcPackage = false;
    int32_t npcPackageId = 0;
    std::string npcPackageComment;
    bool hasGreeting = false;
    int32_t greetingId = 0;
    // npc_greeting.important (Rasa.NET upstream 2026-11): the line shows the speech bubble over the NPC until the character
    // has read it (NpcGreetings.IsUnread). Kept as false / not written on worlds without the column.
    bool greetingImportant = false;
    bool hasActorName = false;
    std::string actorName;
    bool hasVendor = false;
    int32_t vendorPackageId = 0;
    bool hasVendorPrice = false;
    int32_t vendorPrice = 0;
    std::vector<int32_t> vendorItems;                // item template ids, in row order
    // creature_battlecry scope 2 (this creature row): the client battle cry package it cries with, before its class's row
    // (scope 1, kept apart: WorldDatabase::classBattlecries). Rasa.NET Battlecries.PackageOf.
    bool hasBattlecry = false;
    int32_t battlecryPackage = 0;
};

struct ItemTemplateInfo {                            // itemtemplate + itemtemplate_itemclass (read-only)
    int32_t classId = 0;                             // an entity class id (e.g. 15542 Armor_T1_..._Boots)
    int32_t buyPrice = 0;
    int32_t qualityId = 0;                           // Rasa.NET LootQuality: 1 Mission, 2 Normal, 3 Uncommon, 4 Rare, 5 Epic, 6 Legendary, 7 Junk
};
bool sameCreatureDefinition(const CreatureDefinition& a, const CreatureDefinition& b);

// control_point_link (Rasa.NET upstream ControlPointLinkEntry / Managers.ControlPoints, read 2026-10-15): something that belongs to
// a control point; the key is all three columns. kind 1: object = a spawn pool of the Bane garrison (stands there while they hold
// it), 2: a spawn pool of the AFS garrison (medic, banker, vendors, soldiers), 3: the teleporter row of its hospital, 4: of its
// waypoint, 5: a creature row that counts as a boss of its garrison, 6: the footlocker row of its clan lockbox (one a point; on the
// point's map). The server finds a pool / teleporter by one point (the last link wins). Shipped: hospitals are type-5
// teleporters, waypoints type 2.
struct ControlPointLink {
    int64_t controlPointId = 0;
    int32_t kind = 1;
    int64_t objectId = 0;
};
const char* controlPointLinkKindName(int32_t kind);

// map_marker (Rasa.NET upstream MapMarkerEntry / MapMarkerManager, read 2026-10-15): ties one of the client's map-screen markers
// (tr::MapScreenMarker, by the entity id compiled into the client) to the server object whose state it shows. Key (marker entity
// id, map context). objectKind 1 = a teleporter row (waypoint 2, hospital 3, safe zone 19), 2 = a kraftwerks row (crafting
// station 4). matchDistance: the marker-to-object distance when bound (the seed matched by position, up to 25 m).
struct MapMarkerBinding {
    int64_t markerEntityId = 0;
    int32_t markerType = 0;
    int32_t objectKind = 1;
    int64_t objectId = 0;
    double matchDistance = 0;
    bool hasComment = false;                 // comment NULL
    std::string comment;
};

// Everything placed on one map context.
struct WorldContent {
    int32_t mapContextId = -1;
    std::vector<SpawnPool> spawnPools;
    std::vector<SpawnArrival> arrivals;  // of those pools
    std::vector<PatrolStep> patrolSteps; // of those pools, sorted by pool then step (empty on worlds without the table)
    std::vector<PoolPose> poses;         // of those pools
    std::vector<Teleporter> teleporters;
    std::vector<MapLink> mapLinks;
    std::vector<MapRegion> regions;
    std::vector<MissionArea> missionAreas;
    std::vector<PlacedObject> objects;
    std::vector<WormholeLock> wormholeLocks;   // all rows (empty on worlds without the table)
    std::vector<ControlPointLink> controlPointLinks;   // of this map's control points (empty on worlds without the table)
    std::vector<MapMarkerBinding> mapMarkers;          // map_marker rows of this map (empty on worlds without the table)
};

// A database value as SQLite stores it (dynamic typing kept), for tables edited generically (missions).
struct DbValue {
    enum class Type : uint8_t { Null, Int, Real, Text };
    Type type = Type::Null;
    int64_t i = 0;
    double r = 0;
    std::string t;
    static DbValue integer(int64_t v) { DbValue x; x.type = Type::Int; x.i = v; return x; }
    static DbValue real(double v) { DbValue x; x.type = Type::Real; x.r = v; return x; }
    static DbValue text(std::string v) { DbValue x; x.type = Type::Text; x.t = std::move(v); return x; }
    bool isNull() const { return type == Type::Null; }
    int64_t asInt() const { return type == Type::Int ? i : type == Type::Real ? int64_t(r) : 0; }
    double asReal() const { return type == Type::Real ? r : type == Type::Int ? double(i) : 0.0; }
};
bool operator==(const DbValue& a, const DbValue& b);
inline bool operator!=(const DbValue& a, const DbValue& b) { return !(a == b); }

// A whole table: column names in table order, the primary key columns (indices, in key order) and every row.
struct DbTable {
    std::string name;
    std::vector<std::string> columns;
    std::vector<std::string> declaredTypes;          // PRAGMA table_info type, e.g. "INTEGER", "varchar(64)"
    std::vector<bool> nullable;
    std::vector<int> key;
    std::vector<std::vector<DbValue>> rows;
    int column(const std::string& name) const;       // -1 if absent
};

class WorldDatabase {
public:
    // spec: a SQLite file (rasaworld.db) or mysql://user[:password]@host[:port]/database (Rasa.NET's MySQL provider; MySQL 8 or
    // MariaDB). mySqlPluginDir: MariaDB Connector/C's plugin folder (defaultMySqlPluginDir). Throws std::runtime_error.
    explicit WorldDatabase(const std::string& spec, const std::string& mySqlPluginDir = "");
    ~WorldDatabase();
    WorldDatabase(const WorldDatabase&) = delete;
    WorldDatabase& operator=(const WorldDatabase&) = delete;

    const std::string& path() const { return path_; }            // the file, or the MySQL spec without the password
    bool isMySql() const { return mySql_; }
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
    // Generic table access (the mission tables): the whole table with its primary key (throws std::runtime_error).
    DbTable loadTable(const std::string& table) const;
    // Writes the differences between each (saved, edited) pair by primary key in one transaction: inserts and updates in
    // the given table order, deletes in reverse order. Returns false (nothing written) and sets error on failure.
    bool saveTables(const std::vector<std::pair<const DbTable*, const DbTable*>>& tables, SaveSummary& summary, std::string& error);
    const CreatureStat* creatureStat(int32_t creatureId) const;                     // nullptr if the creature has no row
    const ItemTemplateInfo* itemTemplate(int32_t templateId) const;                 // nullptr if unknown
    // The creatures a map's spawn pools can spawn (slots with a creature and max_count > 0), ascending - the set Rasa.NET's
    // ordinary loot coverage check walks for a map that requires loot profiles (OrdinaryLootCoverage: spawn pools).
    std::vector<int32_t> spawnableCreatures(int32_t mapContextId) const;
    const std::vector<CreatureAction>& creatureActions() const { return actions_; }  // sorted by id
    std::vector<CreatureDefinition> creatureDefinitions() const;                    // every creature, sorted by id
    // creature_battlecry scope 1: entity class id -> battle cry package (every creature of the class; empty without the table).
    const std::map<int32_t, int32_t>& classBattlecries() const { return classBattlecries_; }
    // creature_class_flag (Rasa.NET CreatureClassFlagEntry): entity class id -> the client's creatureflag ids (sent in CreatureInfo; the
    // client reads BIOLOGICAL / MECHANICAL / MACHINA for Salvage, Tissue Extraction and healing, SPECIES_* for harvest yields).
    const std::map<int32_t, std::set<int32_t>>& classFlags() const { return classFlags_; }
    // Writes the creature definitions that differ (creature, creature_stat, creature_appearance, the NPC / vendor rows, the
    // creature's own battle cry) in one transaction; new creatures are those whose id is not in `saved`. Creatures are never
    // deleted. With savedClassCries / editedClassCries, the class battle cry rows (scope 1) that differ are written in the same
    // transaction. Reloads the creature tables on success.
    bool saveCreatures(const std::vector<CreatureDefinition>& saved, const std::vector<CreatureDefinition>& edited, SaveSummary& summary,
                       std::string& error, const std::map<int32_t, int32_t>* savedClassCries = nullptr,
                       const std::map<int32_t, int32_t>* editedClassCries = nullptr,
                       const std::map<int32_t, std::set<int32_t>>* savedClassFlags = nullptr,
                       const std::map<int32_t, std::set<int32_t>>* editedClassFlags = nullptr);

    // Schema checks (the Rasa.NET world schema grows with upstream development; newer tables / columns are optional).
    bool hasTable(const std::string& table) const;
    bool hasColumn(const std::string& table, const std::string& column) const;

private:
    bool requireColumns(const std::string& table, std::initializer_list<const char*> columns);
    void loadGlobals();
    void loadCreatures();                            // creature, creature_appearance, creature_stat, creature_action

    void execute(const std::string& sql);

    std::unique_ptr<DbConnection> connection_;
    DbConnection* db_ = nullptr;                     // connection_.get()
    bool mySql_ = false;
    bool readOnly_ = false;
    std::vector<Creature> creatureList_;
    std::string path_;
    std::vector<std::string> warnings_;
    std::unordered_map<std::string, std::set<std::string>> columns_;    // table -> column names
    std::vector<MapInfo> maps_;
    std::unordered_map<int32_t, Creature> creatures_;
    std::unordered_map<int32_t, std::vector<AppearanceSlot>> appearances_;   // creature id -> slots
    std::unordered_map<int32_t, CreatureStat> stats_;                        // creature id -> creature_stat
    std::unordered_map<int32_t, std::pair<int32_t, std::string>> npcPackages_;   // creature id -> (package id, comment)
    std::unordered_map<int32_t, int32_t> greetings_;
    std::unordered_map<int32_t, bool> greetingImportant_;                   // creature id -> npc_greeting.important
    std::unordered_map<int32_t, std::string> actorNames_;
    std::unordered_map<int32_t, int32_t> vendors_;                           // creature id -> vendor package id
    std::unordered_map<int32_t, int32_t> vendorPrices_;
    std::unordered_map<int32_t, std::vector<int32_t>> vendorItems_;
    std::unordered_map<int32_t, ItemTemplateInfo> itemTemplates_;
    std::unordered_map<int32_t, int32_t> creatureBattlecries_;              // creature_battlecry scope 2: creature id -> package
    std::map<int32_t, int32_t> classBattlecries_;                            // scope 1: class id -> package
    std::map<int32_t, std::set<int32_t>> classFlags_;                        // creature_class_flag
    std::vector<CreatureAction> actions_;
    std::unordered_set<int32_t> npcClasses_;
    std::unordered_set<int32_t> vendorClasses_;
};
