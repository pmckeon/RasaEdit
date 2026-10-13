#include "WorldDatabase.h"

#include <sqlite3.h>

#include <algorithm>
#include <stdexcept>

namespace {

// Minimal RAII prepared statement with typed column accessors.
class Statement {
public:
    Statement(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error("SQL error: " + std::string(sqlite3_errmsg(db)) + " in: " + sql);
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bindInt(int index, int64_t value) { sqlite3_bind_int64(statement_, index, value); }
    void bindReal(int index, double value) { sqlite3_bind_double(statement_, index, value); }
    void bindText(int index, const std::string& value) { sqlite3_bind_text(statement_, index, value.c_str(), int(value.size()), SQLITE_TRANSIENT); }
    bool step() { return sqlite3_step(statement_) == SQLITE_ROW; }
    // For INSERT / UPDATE / DELETE: throws on failure.
    void run(sqlite3* db) {
        const int rc = sqlite3_step(statement_);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) throw std::runtime_error(std::string(sqlite3_errmsg(db)));
    }
    int64_t integer(int column) const { return sqlite3_column_int64(statement_, column); }
    int32_t int32(int column) const { return int32_t(sqlite3_column_int64(statement_, column)); }
    float real(int column) const { return float(sqlite3_column_double(statement_, column)); }
    std::string text(int column) const {
        const unsigned char* t = sqlite3_column_text(statement_, column);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    tr::Vec3 vec3(int column) const { return tr::Vec3{real(column), real(column + 1), real(column + 2)}; }
private:
    sqlite3_stmt* statement_ = nullptr;
};

std::string lower(std::string s) { return tr::toLowerAscii(std::move(s)); }

}  // namespace

const SpawnSlot& spawnSlot(const SpawnPool& pool, int index) {
    switch (index) {
        case 0: return pool.slot1;
        case 1: return pool.slot2;
        case 2: return pool.slot3;
        case 3: return pool.slot4;
        case 4: return pool.slot5;
        default: return pool.slot6;
    }
}

SpawnSlot& spawnSlot(SpawnPool& pool, int index) { return const_cast<SpawnSlot&>(spawnSlot(static_cast<const SpawnPool&>(pool), index)); }

const char* arrivalKindName(int32_t kind) {
    switch (kind) {
        case 1: return "dropship pad";
        case 2: return "Bane teleporter";
        default: return "unknown kind";
    }
}

const char* spawnModeName(int32_t mode) {
    switch (mode) {
        case 0: return "automatic";
        case 1: return "control point";
        case 2: return "scripted";
        default: return "unknown";
    }
}

const char* spawnAnimationName(int32_t animType) {
    switch (animType) {
        case 0: return "none";
        case 1: return "hostile dropship";
        case 2: return "friendly dropship";
        default: return "unknown";
    }
}

const char* objectKindName(ObjectKind kind) {
    switch (kind) {
        case ObjectKind::Logos: return "Logos";
        case ObjectKind::ControlPoint: return "Control point";
        case ObjectKind::Footlocker: return "Footlocker";
        default: return "Kraftwerk";
    }
}

WorldDatabase::WorldDatabase(const std::string& path) : path_(path) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(db_);
        db_ = nullptr;
        readOnly_ = true;
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            const std::string message = db_ ? sqlite3_errmsg(db_) : "out of memory";
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("cannot open database: " + message);
        }
        warnings_.push_back("the file is not writable; opened read-only");
    }
    sqlite3_busy_timeout(db_, 2000);                 // the server may be using the file
    {
        Statement tables(db_, "SELECT name FROM sqlite_master WHERE type = 'table'");
        while (tables.step()) {
            const std::string table = tables.text(0);
            std::set<std::string>& columns = columns_[lower(table)];
            Statement info(db_, "PRAGMA table_info(\"" + table + "\")");
            while (info.step()) columns.insert(lower(info.text(1)));
        }
    }
    if (!hasTable("spawnpool") && !hasTable("creature") && !hasTable("map_info"))
        throw std::runtime_error("this does not look like a Rasa.NET World database (no spawnpool, creature or map_info table)");
    loadGlobals();
}

WorldDatabase::~WorldDatabase() {
    if (db_) sqlite3_close(db_);
}

bool WorldDatabase::hasTable(const std::string& table) const { return columns_.count(lower(table)) != 0; }

bool WorldDatabase::requireColumns(const std::string& table, std::initializer_list<const char*> columns) {
    const auto it = columns_.find(lower(table));
    if (it == columns_.end()) {
        warnings_.push_back("table " + table + " is missing");
        return false;
    }
    std::string missing;
    for (const char* c : columns)
        if (!it->second.count(lower(c))) missing += std::string(missing.empty() ? "" : ", ") + c;
    if (!missing.empty()) {
        warnings_.push_back("table " + table + " lacks column(s) " + missing + "; skipped");
        return false;
    }
    return true;
}

std::vector<std::pair<std::string, int64_t>> WorldDatabase::tableCounts() const {
    std::vector<std::pair<std::string, int64_t>> counts;
    std::set<std::string> names;
    for (const auto& t : columns_) names.insert(t.first);
    for (const std::string& t : names) {
        Statement s(db_, "SELECT COUNT(*) FROM \"" + t + "\"");
        counts.emplace_back(t, s.step() ? s.integer(0) : 0);
    }
    return counts;
}

void WorldDatabase::loadCreatures() {
    creatureList_.clear();
    creatures_.clear();
    appearances_.clear();
    stats_.clear();
    actions_.clear();
    if (requireColumns("creature", {"id", "comment", "class_id", "faction", "level", "max_hp", "name_id", "run_speed", "walk_speed"})) {
        Statement s(db_, "SELECT id, comment, class_id, faction, level, max_hp, name_id, run_speed, walk_speed FROM creature");
        while (s.step()) {
            Creature c;
            c.id = s.int32(0);
            c.comment = s.text(1);
            c.classId = s.int32(2);
            c.faction = s.int32(3);
            c.level = s.int32(4);
            c.maxHp = s.int32(5);
            c.nameId = s.int32(6);
            c.runSpeed = s.int32(7);
            c.walkSpeed = s.int32(8);
            creatureList_.push_back(c);
            creatures_.emplace(c.id, std::move(c));
        }
        std::sort(creatureList_.begin(), creatureList_.end(), [](const Creature& a, const Creature& b) { return a.id < b.id; });
    }
    if (requireColumns("creature_appearance", {"id", "slot_id", "class_id", "color"})) {
        Statement s(db_, "SELECT id, slot_id, class_id, color FROM creature_appearance ORDER BY id, slot_id");
        while (s.step()) appearances_[s.int32(0)].push_back(AppearanceSlot{s.int32(1), s.int32(2), uint32_t(s.integer(3))});
    }
    if (requireColumns("creature", {"action1", "action2", "action3", "action4", "action5", "action6", "action7", "action8"})) {
        Statement s(db_, "SELECT id, action1, action2, action3, action4, action5, action6, action7, action8 FROM creature");
        while (s.step()) {
            const auto it = creatures_.find(s.int32(0));
            if (it == creatures_.end()) continue;
            for (int k = 0; k < 8; ++k) creatureAction(it->second, k) = s.int32(1 + k);
        }
        for (Creature& c : creatureList_) c = creatures_[c.id];
    }
    if (requireColumns("creature_stat", {"id", "body", "mind", "spirit", "health", "armor"})) {
        Statement s(db_, "SELECT id, body, mind, spirit, health, armor FROM creature_stat");
        while (s.step()) stats_[s.int32(0)] = CreatureStat{s.int32(1), s.int32(2), s.int32(3), s.int32(4), s.int32(5)};
    }
    if (requireColumns("creature_action", {"id", "description", "action_id", "action_arg_id", "range_min", "range_max", "cooldown", "windup",
                                           "min_damage", "max_damage", "damage_type"})) {
        Statement s(db_, "SELECT id, description, action_id, action_arg_id, range_min, range_max, cooldown, windup, min_damage, max_damage, damage_type "
                         "FROM creature_action ORDER BY id");
        while (s.step())
            actions_.push_back(CreatureAction{s.int32(0), s.text(1), s.int32(2), s.int32(3), s.real(4), s.real(5), s.int32(6), s.int32(7), s.int32(8),
                                              s.int32(9), s.int32(10)});
    }
}

void WorldDatabase::loadGlobals() {
    if (requireColumns("map_info", {"map_context_id", "map_name", "map_version", "base_region"})) {
        Statement s(db_, "SELECT map_context_id, map_name, map_version, base_region FROM map_info ORDER BY map_name");
        while (s.step()) maps_.push_back(MapInfo{s.int32(0), s.text(1), s.int32(2), s.int32(3)});
    }
    loadCreatures();
    if (requireColumns("entityclass", {"id", "aug_list"})) {
        Statement s(db_, "SELECT id, aug_list FROM entityclass");
        while (s.step()) {
            // aug_list is a list of augmentation numbers separated by non-digits, e.g. "1,52,59".
            const std::string list = s.text(1);
            int value = -1;
            for (size_t k = 0; k <= list.size(); ++k) {
                const char ch = k < list.size() ? list[k] : ',';
                if (ch >= '0' && ch <= '9') {
                    value = (value < 0 ? 0 : value * 10) + (ch - '0');
                } else if (value >= 0) {
                    if (value == 52) npcClasses_.insert(s.int32(0));
                    if (value == 12) vendorClasses_.insert(s.int32(0));
                    value = -1;
                }
            }
        }
    }
}

int32_t WorldDatabase::contextIdForMap(const std::string& mapName) const {
    const std::string wanted = lower(mapName);
    for (const MapInfo& m : maps_)
        if (lower(m.name) == wanted) return m.mapContextId;
    return -1;
}

int32_t creatureAction(const Creature& c, int index) {
    switch (index) {
        case 0: return c.action1;
        case 1: return c.action2;
        case 2: return c.action3;
        case 3: return c.action4;
        case 4: return c.action5;
        case 5: return c.action6;
        case 6: return c.action7;
        default: return c.action8;
    }
}

int32_t& creatureAction(Creature& c, int index) {
    switch (index) {
        case 0: return c.action1;
        case 1: return c.action2;
        case 2: return c.action3;
        case 3: return c.action4;
        case 4: return c.action5;
        case 5: return c.action6;
        case 6: return c.action7;
        default: return c.action8;
    }
}

const char* creatureFactionName(int32_t faction) {
    switch (faction) {
        case 0: return "hostile";
        case 1: return "friendly";
        case 2: return "object";
        case 3: return "neutral";
        case 4: return "decoration";
        case 5: return "decoration proxy";
        case 6: return "ignore";
        default: return "(hostile)";
    }
}

const char* equipmentSlotName(int32_t slot) {
    switch (slot) {
        case 1: return "Helmet";
        case 2: return "Shoes";
        case 3: return "Gloves";
        case 13: return "Weapon";
        case 14: return "Hair";
        case 15: return "Torso";
        case 16: return "Legs";
        case 17: return "Face";
        case 18: return "Wing";
        case 19: return "Eyewear";
        case 20: return "Beard";
        case 21: return "Mask";
        default: return "";
    }
}

bool sameCreatureDefinition(const CreatureDefinition& a, const CreatureDefinition& b) {
    const Creature& x = a.creature;
    const Creature& y = b.creature;
    if (x.id != y.id || x.comment != y.comment || x.classId != y.classId || x.faction != y.faction || x.level != y.level || x.maxHp != y.maxHp ||
        x.nameId != y.nameId || x.runSpeed != y.runSpeed || x.walkSpeed != y.walkSpeed)
        return false;
    for (int k = 0; k < 8; ++k)
        if (creatureAction(x, k) != creatureAction(y, k)) return false;
    if (a.hasStat != b.hasStat) return false;
    if (a.hasStat && (a.stat.body != b.stat.body || a.stat.mind != b.stat.mind || a.stat.spirit != b.stat.spirit || a.stat.health != b.stat.health ||
                      a.stat.armor != b.stat.armor))
        return false;
    if (a.appearance.size() != b.appearance.size()) return false;
    for (size_t k = 0; k < a.appearance.size(); ++k)
        if (a.appearance[k].slotId != b.appearance[k].slotId || a.appearance[k].classId != b.appearance[k].classId ||
            a.appearance[k].colour != b.appearance[k].colour)
            return false;
    return true;
}

const CreatureStat* WorldDatabase::creatureStat(int32_t creatureId) const {
    const auto it = stats_.find(creatureId);
    return it == stats_.end() ? nullptr : &it->second;
}

std::vector<CreatureDefinition> WorldDatabase::creatureDefinitions() const {
    std::vector<CreatureDefinition> out;
    out.reserve(creatureList_.size());
    for (const Creature& c : creatureList_) {
        CreatureDefinition d;
        d.creature = c;
        if (const CreatureStat* stat = creatureStat(c.id)) {
            d.hasStat = true;
            d.stat = *stat;
        }
        d.appearance = appearance(c.id);
        out.push_back(std::move(d));
    }
    return out;
}

bool WorldDatabase::saveCreatures(const std::vector<CreatureDefinition>& saved, const std::vector<CreatureDefinition>& edited, SaveSummary& summary,
                                  std::string& error) {
    summary = SaveSummary{};
    if (readOnly_) {
        error = "the database was opened read-only";
        return false;
    }
    try {
        execute("BEGIN IMMEDIATE");
    } catch (const std::exception& e) {
        error = std::string("cannot start a transaction (is the server writing to the database?): ") + e.what();
        return false;
    }
    try {
        std::unordered_map<int32_t, const CreatureDefinition*> before;
        for (const CreatureDefinition& d : saved) before[d.creature.id] = &d;
        std::set<int32_t> ids;
        for (const CreatureDefinition& d : edited) {
            const Creature& c = d.creature;
            if (c.id <= 0) throw std::runtime_error("creature id " + std::to_string(c.id) + " is not valid");
            if (!ids.insert(c.id).second) throw std::runtime_error("creature id " + std::to_string(c.id) + " is used twice");
            const auto found = before.find(c.id);
            const CreatureDefinition* b = found == before.end() ? nullptr : found->second;
            if (b && sameCreatureDefinition(*b, d)) continue;
            const bool creatureChanged = !b || c.comment != b->creature.comment || c.classId != b->creature.classId ||
                                         c.faction != b->creature.faction || c.level != b->creature.level || c.maxHp != b->creature.maxHp ||
                                         c.nameId != b->creature.nameId || c.runSpeed != b->creature.runSpeed || c.walkSpeed != b->creature.walkSpeed ||
                                         [&] {
                                             for (int k = 0; k < 8; ++k)
                                                 if (creatureAction(c, k) != creatureAction(b->creature, k)) return true;
                                             return false;
                                         }();
            if (creatureChanged) {
                Statement s(db_, b ? "UPDATE creature SET comment = ?2, class_id = ?3, faction = ?4, level = ?5, max_hp = ?6, name_id = ?7, run_speed = ?8, "
                                     "walk_speed = ?9, action1 = ?10, action2 = ?11, action3 = ?12, action4 = ?13, action5 = ?14, action6 = ?15, "
                                     "action7 = ?16, action8 = ?17 WHERE id = ?1"
                                   : "INSERT INTO creature (id, comment, class_id, faction, level, max_hp, name_id, run_speed, walk_speed, action1, action2, "
                                     "action3, action4, action5, action6, action7, action8) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
                                     "?13, ?14, ?15, ?16, ?17)");
                s.bindInt(1, c.id);
                s.bindText(2, c.comment);
                s.bindInt(3, c.classId);
                s.bindInt(4, c.faction);
                s.bindInt(5, c.level);
                s.bindInt(6, c.maxHp);
                s.bindInt(7, c.nameId);
                s.bindInt(8, c.runSpeed);
                s.bindInt(9, c.walkSpeed);
                for (int k = 0; k < 8; ++k) s.bindInt(10 + k, creatureAction(c, k));
                s.run(db_);
                ++(b ? summary.updated : summary.inserted);
            }
            const bool hadStat = b && b->hasStat;
            if (d.hasStat && (!hadStat || d.stat.body != b->stat.body || d.stat.mind != b->stat.mind || d.stat.spirit != b->stat.spirit ||
                              d.stat.health != b->stat.health || d.stat.armor != b->stat.armor)) {
                Statement s(db_, hadStat ? "UPDATE creature_stat SET body = ?2, mind = ?3, spirit = ?4, health = ?5, armor = ?6 WHERE id = ?1"
                                         : "INSERT INTO creature_stat (id, body, mind, spirit, health, armor) VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
                s.bindInt(1, c.id);
                s.bindInt(2, d.stat.body);
                s.bindInt(3, d.stat.mind);
                s.bindInt(4, d.stat.spirit);
                s.bindInt(5, d.stat.health);
                s.bindInt(6, d.stat.armor);
                s.run(db_);
                ++(hadStat ? summary.updated : summary.inserted);
            } else if (!d.hasStat && hadStat) {
                Statement s(db_, "DELETE FROM creature_stat WHERE id = ?1");
                s.bindInt(1, c.id);
                s.run(db_);
                ++summary.deleted;
            }
            bool appearanceChanged = !b || b->appearance.size() != d.appearance.size();
            for (size_t k = 0; !appearanceChanged && k < d.appearance.size(); ++k)
                appearanceChanged = d.appearance[k].slotId != b->appearance[k].slotId || d.appearance[k].classId != b->appearance[k].classId ||
                                    d.appearance[k].colour != b->appearance[k].colour;
            if (appearanceChanged) {                     // the rows have no key of their own: replace the creature's set
                Statement del(db_, "DELETE FROM creature_appearance WHERE id = ?1");
                del.bindInt(1, c.id);
                del.run(db_);
                for (const AppearanceSlot& slot : d.appearance) {
                    Statement s(db_, "INSERT INTO creature_appearance (id, slot_id, Class_id, color) VALUES (?1, ?2, ?3, ?4)");
                    s.bindInt(1, c.id);
                    s.bindInt(2, slot.slotId);
                    s.bindInt(3, slot.classId);
                    s.bindInt(4, int64_t(slot.colour));
                    s.run(db_);
                }
                ++summary.updated;
            }
        }
        execute("COMMIT");
    } catch (const std::exception& e) {
        error = e.what();
        try { execute("ROLLBACK"); } catch (const std::exception&) {}
        summary = SaveSummary{};
        return false;
    }
    loadCreatures();
    return true;
}

const Creature* WorldDatabase::creature(int32_t id) const {
    const auto it = creatures_.find(id);
    return it == creatures_.end() ? nullptr : &it->second;
}

const std::vector<AppearanceSlot>& WorldDatabase::appearance(int32_t creatureId) const {
    static const std::vector<AppearanceSlot> none;
    const auto it = appearances_.find(creatureId);
    return it == appearances_.end() ? none : it->second;
}

WorldContent WorldDatabase::loadMap(int32_t mapContextId) {
    WorldContent content;
    content.mapContextId = mapContextId;

    if (requireColumns("spawnpool", {"id", "mode", "anim_type", "respown_time", "pos_x", "pos_y", "pos_z", "rotation", "map_context_id", "radius",
                                     "creature_1_Id", "creature_1_min_count", "creature_1_max_count", "creature_6_Id", "creature_6_min_count",
                                     "creature_6_max_count"})) {
        std::string sql = "SELECT id, mode, anim_type, respown_time, pos_x, pos_y, pos_z, rotation, map_context_id, radius";
        for (int k = 1; k <= 6; ++k)
            sql += ", creature_" + std::to_string(k) + "_Id, creature_" + std::to_string(k) + "_min_count, creature_" + std::to_string(k) + "_max_count";
        Statement s(db_, sql + " FROM spawnpool WHERE map_context_id = ?1 ORDER BY id");
        s.bindInt(1, mapContextId);
        while (s.step()) {
            SpawnPool p;
            p.id = s.integer(0);
            p.mode = s.int32(1);
            p.animType = s.int32(2);
            p.respawnTime = s.int32(3);
            p.position = s.vec3(4);
            p.rotation = s.real(7);
            p.mapContextId = s.int32(8);
            p.radius = s.real(9);
            for (int k = 0; k < 6; ++k) {
                SpawnSlot& slot = spawnSlot(p, k);
                slot.creatureId = s.int32(10 + 3 * k);
                slot.minCount = s.int32(11 + 3 * k);
                slot.maxCount = s.int32(12 + 3 * k);
            }
            content.spawnPools.push_back(p);
        }
    }
    if (!content.spawnPools.empty() && requireColumns("spawnpool_arrival", {"id", "pool_id", "kind", "pos_x", "pos_y", "pos_z", "rotation", "entity_id", "comment"})) {
        Statement s(db_, "SELECT a.id, a.pool_id, a.kind, a.pos_x, a.pos_y, a.pos_z, a.rotation, a.entity_id, a.comment FROM spawnpool_arrival a "
                         "JOIN spawnpool p ON p.id = a.pool_id WHERE p.map_context_id = ?1 ORDER BY a.id");
        s.bindInt(1, mapContextId);
        while (s.step())
            content.arrivals.push_back(SpawnArrival{s.integer(0), s.integer(1), s.int32(2), s.vec3(3), s.real(6), s.integer(7), s.text(8)});
    }
    if (requireColumns("teleporter", {"id", "class_id", "type", "description", "pos_x", "pos_y", "pos_z", "rotation", "map_context_id"})) {
        Statement s(db_, "SELECT id, class_id, type, description, pos_x, pos_y, pos_z, rotation, map_context_id FROM teleporter WHERE map_context_id = ?1 ORDER BY id");
        s.bindInt(1, mapContextId);
        while (s.step()) content.teleporters.push_back(Teleporter{s.integer(0), s.int32(1), s.int32(2), s.text(3), s.vec3(4), s.real(7), s.int32(8)});
    }
    if (requireColumns("map_link", {"id", "map_context_id", "pos_x", "pos_y", "pos_z", "radius", "dest_map_context_id", "dest_pos_x", "dest_pos_y",
                                    "dest_pos_z", "dest_rotation", "kind", "enabled", "comment"})) {
        Statement s(db_, "SELECT id, map_context_id, pos_x, pos_y, pos_z, radius, dest_map_context_id, dest_pos_x, dest_pos_y, dest_pos_z, dest_rotation, "
                         "kind, enabled, comment FROM map_link WHERE map_context_id = ?1 ORDER BY id");
        s.bindInt(1, mapContextId);
        while (s.step()) {
            MapLink l;
            l.id = s.integer(0);
            l.mapContextId = s.int32(1);
            l.position = s.vec3(2);
            l.radius = s.real(5);
            l.destinationMapContextId = s.int32(6);
            l.destinationPosition = s.vec3(7);
            l.destinationRotation = s.real(10);
            l.kind = s.int32(11);
            l.enabled = s.integer(12) != 0;
            l.comment = s.text(13);
            content.mapLinks.push_back(l);
        }
    }
    if (requireColumns("map_region", {"id", "map_context_id", "region_id", "shape", "pos_x", "pos_y", "pos_z", "radius", "half_x", "half_z", "min_y",
                                      "max_y", "underground", "enabled", "comment"})) {
        Statement s(db_, "SELECT id, map_context_id, region_id, shape, pos_x, pos_y, pos_z, radius, half_x, half_z, min_y, max_y, underground, enabled, "
                         "comment FROM map_region WHERE map_context_id = ?1 ORDER BY id");
        s.bindInt(1, mapContextId);
        while (s.step()) {
            MapRegion r;
            r.id = s.integer(0);
            r.mapContextId = s.int32(1);
            r.regionId = s.int32(2);
            r.area = AreaShape{s.int32(3), s.vec3(4), s.real(7), s.real(8), s.real(9), s.real(10), s.real(11)};
            r.underground = s.integer(12) != 0;
            r.enabled = s.integer(13) != 0;
            r.comment = s.text(14);
            content.regions.push_back(r);
        }
    }
    if (requireColumns("mission_area", {"mission_id", "content_revision", "area_id", "map_context_id", "shape", "pos_x", "pos_y", "pos_z", "radius",
                                        "extent_x", "extent_y", "extent_z", "comment"})) {
        Statement s(db_, "SELECT mission_id, area_id, map_context_id, shape, pos_x, pos_y, pos_z, radius, extent_x, extent_y, extent_z, comment, "
                         "content_revision FROM mission_area WHERE map_context_id = ?1 ORDER BY mission_id, area_id");
        s.bindInt(1, mapContextId);
        while (s.step()) {
            MissionArea a;
            a.missionId = s.int32(0);
            a.areaId = s.int32(1);
            a.mapContextId = s.int32(2);
            const float extentY = s.real(9);
            a.area = AreaShape{s.int32(3), s.vec3(4), s.real(7), s.real(8), s.real(10), -2000.0f, 2000.0f};
            if (extentY > 0) {
                a.area.minY = a.area.position.y - extentY;
                a.area.maxY = a.area.position.y + extentY;
            }
            a.comment = s.text(11);
            a.contentRevision = s.text(12);
            content.missionAreas.push_back(a);
        }
    }
    struct ObjectTable {
        const char* table;
        ObjectKind kind;
        const char* nameColumn;
        bool hasRotation;
    };
    const ObjectTable objectTables[] = {{"logos", ObjectKind::Logos, "name", false},
                                        {"control_point", ObjectKind::ControlPoint, "name", true},
                                        {"footlocker", ObjectKind::Footlocker, "comment", true},
                                        {"kraftwerks", ObjectKind::Kraftwerk, "comment", true}};
    for (const ObjectTable& t : objectTables) {
        const bool ok = t.hasRotation ? requireColumns(t.table, {"id", "class_id", "map_context_id", "pos_x", "pos_y", "pos_z", "rotation", t.nameColumn})
                                      : requireColumns(t.table, {"id", "class_id", "map_context_id", "pos_x", "pos_y", "pos_z", t.nameColumn});
        if (!ok) continue;
        Statement s(db_, std::string("SELECT id, class_id, map_context_id, pos_x, pos_y, pos_z, ") + (t.hasRotation ? "rotation" : "0") + ", " +
                             t.nameColumn + " FROM " + t.table + " WHERE map_context_id = ?1 ORDER BY id");
        s.bindInt(1, mapContextId);
        while (s.step()) content.objects.push_back(PlacedObject{t.kind, s.integer(0), s.int32(1), s.int32(2), s.vec3(3), s.real(6), s.text(7)});
    }
    return content;
}

// ------------------------------------------------------------------------------------------
// Saving
// ------------------------------------------------------------------------------------------
namespace {

bool sameVec(const tr::Vec3& a, const tr::Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool sameSlot(const SpawnSlot& a, const SpawnSlot& b) { return a.creatureId == b.creatureId && a.minCount == b.minCount && a.maxCount == b.maxCount; }
bool sameArea(const AreaShape& a, const AreaShape& b) {
    return a.shape == b.shape && sameVec(a.position, b.position) && a.radius == b.radius && a.halfX == b.halfX && a.halfZ == b.halfZ &&
           a.minY == b.minY && a.maxY == b.maxY;
}
bool same(const SpawnPool& a, const SpawnPool& b) {
    for (int k = 0; k < 6; ++k)
        if (!sameSlot(spawnSlot(a, k), spawnSlot(b, k))) return false;
    return a.mode == b.mode && a.animType == b.animType && a.respawnTime == b.respawnTime && sameVec(a.position, b.position) &&
           a.rotation == b.rotation && a.radius == b.radius;
}
bool same(const SpawnArrival& a, const SpawnArrival& b) {
    return a.poolId == b.poolId && a.kind == b.kind && sameVec(a.position, b.position) && a.rotation == b.rotation && a.entityId == b.entityId &&
           a.comment == b.comment;
}

bool same(const Teleporter& a, const Teleporter& b) {
    return a.classId == b.classId && a.type == b.type && a.description == b.description && sameVec(a.position, b.position) && a.rotation == b.rotation;
}
bool same(const PlacedObject& a, const PlacedObject& b) {
    return a.classId == b.classId && sameVec(a.position, b.position) && a.rotation == b.rotation && a.name == b.name;
}
bool same(const MapLink& a, const MapLink& b) {
    return sameVec(a.position, b.position) && a.radius == b.radius && a.destinationMapContextId == b.destinationMapContextId &&
           sameVec(a.destinationPosition, b.destinationPosition) && a.destinationRotation == b.destinationRotation && a.kind == b.kind &&
           a.enabled == b.enabled && a.comment == b.comment;
}
bool same(const MapRegion& a, const MapRegion& b) {
    return a.regionId == b.regionId && sameArea(a.area, b.area) && a.underground == b.underground && a.enabled == b.enabled && a.comment == b.comment;
}
bool same(const MissionArea& a, const MissionArea& b) { return sameArea(a.area, b.area) && a.comment == b.comment; }

const char* objectTable(ObjectKind kind) {
    switch (kind) {
        case ObjectKind::Logos: return "logos";
        case ObjectKind::ControlPoint: return "control_point";
        case ObjectKind::Footlocker: return "footlocker";
        default: return "kraftwerks";
    }
}

}  // namespace

void WorldDatabase::execute(const std::string& sql) {
    char* message = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string text = message ? message : "unknown error";
        sqlite3_free(message);
        throw std::runtime_error(text);
    }
}

bool WorldDatabase::save(const WorldContent& saved, const WorldContent& edited, SaveSummary& summary, std::string& error) {
    summary = SaveSummary{};
    if (readOnly_) {
        error = "the database was opened read-only";
        return false;
    }
    const int64_t context = edited.mapContextId;
    try {
        execute("BEGIN IMMEDIATE");
    } catch (const std::exception& e) {
        error = std::string("cannot start a transaction (is the server writing to the database?): ") + e.what();
        return false;
    }
    try {
        // Rows that disappeared, by id.
        auto deletedIds = [](const auto& before, const auto& after) {
            std::vector<int64_t> ids;
            for (const auto& b : before) {
                bool kept = false;
                for (const auto& a : after) if (a.id == b.id) { kept = true; break; }
                if (!kept && b.id > 0) ids.push_back(b.id);
            }
            return ids;
        };
        auto findById = [](const auto& rows, int64_t id) -> decltype(&rows[0]) {
            for (const auto& r : rows) if (r.id == id) return &r;
            return nullptr;
        };

        // Spawn pools (and their arrival points when deleted).
        for (int64_t id : deletedIds(saved.spawnPools, edited.spawnPools)) {
            Statement arrivals(db_, "DELETE FROM spawnpool_arrival WHERE pool_id = ?1");
            arrivals.bindInt(1, id);
            arrivals.run(db_);
            Statement s(db_, "DELETE FROM spawnpool WHERE id = ?1");
            s.bindInt(1, id);
            s.run(db_);
            ++summary.deleted;
        }
        std::string columns = "mode, anim_type, respown_time, pos_x, pos_y, pos_z, rotation, map_context_id, radius";
        for (int k = 1; k <= 6; ++k)
            columns += ", creature_" + std::to_string(k) + "_Id, creature_" + std::to_string(k) + "_min_count, creature_" + std::to_string(k) + "_max_count";
        auto bindPool = [&](Statement& s, const SpawnPool& p) {
            s.bindInt(1, p.mode);
            s.bindInt(2, p.animType);
            s.bindInt(3, p.respawnTime);
            s.bindReal(4, p.position.x);
            s.bindReal(5, p.position.y);
            s.bindReal(6, p.position.z);
            s.bindReal(7, p.rotation);
            s.bindInt(8, context);
            s.bindReal(9, p.radius);
            for (int k = 0; k < 6; ++k) {
                const SpawnSlot& slot = spawnSlot(p, k);
                s.bindInt(10 + 3 * k, slot.creatureId);
                s.bindInt(11 + 3 * k, slot.minCount);
                s.bindInt(12 + 3 * k, slot.maxCount);
            }
        };
        for (const SpawnPool& p : edited.spawnPools) {
            if (p.id == 0) {
                Statement s(db_, "INSERT INTO spawnpool (" + columns + ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, "
                                 "?19, ?20, ?21, ?22, ?23, ?24, ?25, ?26, ?27)");
                bindPool(s, p);
                s.run(db_);
                ++summary.inserted;
            } else if (const SpawnPool* before = findById(saved.spawnPools, p.id); before && !same(*before, p)) {
                std::string sets;
                size_t start = 0;
                int index = 1;
                while (start < columns.size()) {
                    const size_t comma = columns.find(", ", start);
                    const std::string column = columns.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    sets += (sets.empty() ? "" : ", ") + column + " = ?" + std::to_string(index++);
                    if (comma == std::string::npos) break;
                    start = comma + 2;
                }
                Statement s(db_, "UPDATE spawnpool SET " + sets + " WHERE id = ?28");
                bindPool(s, p);
                s.bindInt(28, p.id);
                s.run(db_);
                ++summary.updated;
            }
        }

        // Spawn arrival points (those of deleted pools are already gone).
        for (int64_t id : deletedIds(saved.arrivals, edited.arrivals)) {
            Statement s(db_, "DELETE FROM spawnpool_arrival WHERE id = ?1");
            s.bindInt(1, id);
            s.run(db_);
            ++summary.deleted;
        }
        for (const SpawnArrival& a : edited.arrivals) {
            const SpawnArrival* before = a.id ? findById(saved.arrivals, a.id) : nullptr;
            if (a.id != 0 && (!before || same(*before, a))) continue;
            if (a.poolId <= 0) continue;                                    // only pools that have a database id
            Statement s(db_, a.id == 0 ? "INSERT INTO spawnpool_arrival (pool_id, kind, pos_x, pos_y, pos_z, rotation, entity_id, comment) "
                                         "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)"
                                       : "UPDATE spawnpool_arrival SET pool_id = ?1, kind = ?2, pos_x = ?3, pos_y = ?4, pos_z = ?5, rotation = ?6, "
                                         "entity_id = ?7, comment = ?8 WHERE id = ?9");
            s.bindInt(1, a.poolId);
            s.bindInt(2, a.kind);
            s.bindReal(3, a.position.x);
            s.bindReal(4, a.position.y);
            s.bindReal(5, a.position.z);
            s.bindReal(6, a.rotation);
            s.bindInt(7, a.entityId);
            s.bindText(8, a.comment);
            if (a.id != 0) s.bindInt(9, a.id);
            s.run(db_);
            ++(a.id == 0 ? summary.inserted : summary.updated);
        }

        // Teleporters.
        for (int64_t id : deletedIds(saved.teleporters, edited.teleporters)) {
            Statement s(db_, "DELETE FROM teleporter WHERE id = ?1");
            s.bindInt(1, id);
            s.run(db_);
            ++summary.deleted;
        }
        for (const Teleporter& t : edited.teleporters) {
            const Teleporter* before = t.id ? findById(saved.teleporters, t.id) : nullptr;
            if (t.id != 0 && (!before || same(*before, t))) continue;
            Statement s(db_, t.id == 0 ? "INSERT INTO teleporter (class_id, type, description, pos_x, pos_y, pos_z, rotation, map_context_id) "
                                         "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)"
                                       : "UPDATE teleporter SET class_id = ?1, type = ?2, description = ?3, pos_x = ?4, pos_y = ?5, pos_z = ?6, "
                                         "rotation = ?7, map_context_id = ?8 WHERE id = ?9");
            s.bindInt(1, t.classId);
            s.bindInt(2, t.type);
            s.bindText(3, t.description);
            s.bindReal(4, t.position.x);
            s.bindReal(5, t.position.y);
            s.bindReal(6, t.position.z);
            s.bindReal(7, t.rotation);
            s.bindInt(8, context);
            if (t.id != 0) s.bindInt(9, t.id);
            s.run(db_);
            ++(t.id == 0 ? summary.inserted : summary.updated);
        }

        // Objects (Logos, control points, footlockers, Kraftwerks).
        for (const PlacedObject& b : saved.objects) {
            bool kept = false;
            for (const PlacedObject& a : edited.objects) if (a.kind == b.kind && a.id == b.id) { kept = true; break; }
            if (kept || b.kind == ObjectKind::ControlPoint) continue;          // control points are never deleted
            Statement s(db_, std::string("DELETE FROM ") + objectTable(b.kind) + " WHERE id = ?1");
            s.bindInt(1, b.id);
            s.run(db_);
            ++summary.deleted;
        }
        for (const PlacedObject& o : edited.objects) {
            const PlacedObject* before = nullptr;
            for (const PlacedObject& b : saved.objects) if (b.kind == o.kind && b.id == o.id) { before = &b; break; }
            if (o.id != 0 && (!before || same(*before, o))) continue;
            if (o.id == 0 && o.kind == ObjectKind::ControlPoint) continue;     // not creatable
            const bool logos = o.kind == ObjectKind::Logos;
            const std::string table = objectTable(o.kind);
            const std::string nameColumn = (logos || o.kind == ObjectKind::ControlPoint) ? "name" : "comment";
            std::string sql;
            if (o.id == 0)
                sql = "INSERT INTO " + table + " (class_id, map_context_id, pos_x, pos_y, pos_z, " + nameColumn + (logos ? "" : ", rotation") +
                      ") VALUES (?1, ?2, ?3, ?4, ?5, ?6" + (logos ? "" : ", ?7") + ")";
            else
                sql = "UPDATE " + table + " SET class_id = ?1, map_context_id = ?2, pos_x = ?3, pos_y = ?4, pos_z = ?5, " + nameColumn + " = ?6" +
                      (logos ? "" : ", rotation = ?7") + " WHERE id = ?8";
            Statement s(db_, sql);
            s.bindInt(1, o.classId);
            s.bindInt(2, context);
            s.bindReal(3, o.position.x);
            s.bindReal(4, o.position.y);
            s.bindReal(5, o.position.z);
            s.bindText(6, o.name);
            if (!logos) s.bindReal(7, o.rotation);
            if (o.id != 0) s.bindInt(8, o.id);
            s.run(db_);
            ++(o.id == 0 ? summary.inserted : summary.updated);
        }

        // Map links.
        for (int64_t id : deletedIds(saved.mapLinks, edited.mapLinks)) {
            Statement s(db_, "DELETE FROM map_link WHERE id = ?1");
            s.bindInt(1, id);
            s.run(db_);
            ++summary.deleted;
        }
        for (const MapLink& l : edited.mapLinks) {
            const MapLink* before = l.id ? findById(saved.mapLinks, l.id) : nullptr;
            if (l.id != 0 && (!before || same(*before, l))) continue;
            Statement s(db_, l.id == 0 ? "INSERT INTO map_link (map_context_id, pos_x, pos_y, pos_z, radius, dest_map_context_id, dest_pos_x, dest_pos_y, "
                                         "dest_pos_z, dest_rotation, kind, enabled, comment) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)"
                                       : "UPDATE map_link SET map_context_id = ?1, pos_x = ?2, pos_y = ?3, pos_z = ?4, radius = ?5, dest_map_context_id = ?6, "
                                         "dest_pos_x = ?7, dest_pos_y = ?8, dest_pos_z = ?9, dest_rotation = ?10, kind = ?11, enabled = ?12, comment = ?13 "
                                         "WHERE id = ?14");
            s.bindInt(1, context);
            s.bindReal(2, l.position.x);
            s.bindReal(3, l.position.y);
            s.bindReal(4, l.position.z);
            s.bindReal(5, l.radius);
            s.bindInt(6, l.destinationMapContextId);
            s.bindReal(7, l.destinationPosition.x);
            s.bindReal(8, l.destinationPosition.y);
            s.bindReal(9, l.destinationPosition.z);
            s.bindReal(10, l.destinationRotation);
            s.bindInt(11, l.kind);
            s.bindInt(12, l.enabled ? 1 : 0);
            s.bindText(13, l.comment);
            if (l.id != 0) s.bindInt(14, l.id);
            s.run(db_);
            ++(l.id == 0 ? summary.inserted : summary.updated);
        }

        // Regions and mission areas: updates only.
        for (const MapRegion& r : edited.regions) {
            const MapRegion* before = findById(saved.regions, r.id);
            if (!before || same(*before, r)) continue;
            Statement s(db_, "UPDATE map_region SET region_id = ?1, shape = ?2, pos_x = ?3, pos_y = ?4, pos_z = ?5, radius = ?6, half_x = ?7, half_z = ?8, "
                             "min_y = ?9, max_y = ?10, underground = ?11, enabled = ?12, comment = ?13 WHERE id = ?14");
            s.bindInt(1, r.regionId);
            s.bindInt(2, r.area.shape);
            s.bindReal(3, r.area.position.x);
            s.bindReal(4, r.area.position.y);
            s.bindReal(5, r.area.position.z);
            s.bindReal(6, r.area.radius);
            s.bindReal(7, r.area.halfX);
            s.bindReal(8, r.area.halfZ);
            s.bindReal(9, r.area.minY);
            s.bindReal(10, r.area.maxY);
            s.bindInt(11, r.underground ? 1 : 0);
            s.bindInt(12, r.enabled ? 1 : 0);
            s.bindText(13, r.comment);
            s.bindInt(14, r.id);
            s.run(db_);
            ++summary.updated;
        }
        for (const MissionArea& a : edited.missionAreas) {
            const MissionArea* before = nullptr;
            for (const MissionArea& b : saved.missionAreas)
                if (b.missionId == a.missionId && b.contentRevision == a.contentRevision && b.areaId == a.areaId) before = &b;
            if (!before || same(*before, a)) continue;
            Statement s(db_, "UPDATE mission_area SET shape = ?1, pos_x = ?2, pos_y = ?3, pos_z = ?4, radius = ?5, extent_x = ?6, extent_z = ?7, comment = ?8 "
                             "WHERE mission_id = ?9 AND content_revision = ?10 AND area_id = ?11");
            s.bindInt(1, a.area.shape);
            s.bindReal(2, a.area.position.x);
            s.bindReal(3, a.area.position.y);
            s.bindReal(4, a.area.position.z);
            s.bindReal(5, a.area.radius);
            s.bindReal(6, a.area.halfX);
            s.bindReal(7, a.area.halfZ);
            s.bindText(8, a.comment);
            s.bindInt(9, a.missionId);
            s.bindText(10, a.contentRevision);
            s.bindInt(11, a.areaId);
            s.run(db_);
            ++summary.updated;
        }
        execute("COMMIT");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        try { execute("ROLLBACK"); } catch (const std::exception&) {}
        summary = SaveSummary{};
        return false;
    }
}
