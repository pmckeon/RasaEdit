#include "WorldDatabase.h"

#include <functional>
#include <map>


#include <algorithm>
#include <stdexcept>

namespace {

// A statement of the connection's backend (DbConnection.h), with the typed accessors the loaders use.
class Statement {
public:
    Statement(DbConnection* db, const std::string& sql) : statement_(db->prepare(sql)) {}
    void bindInt(int index, int64_t value) { statement_->bindInt(index, value); }
    void bindReal(int index, double value) { statement_->bindReal(index, value); }
    void bindText(int index, const std::string& value) { statement_->bindText(index, value); }
    void bindNull(int index) { statement_->bindNull(index); }
    DbType columnType(int column) const { return statement_->columnType(column); }
    bool isNull(int column) const { return statement_->columnType(column) == DbType::Null; }
    double realDouble(int column) const { return statement_->realDouble(column); }
    bool step() { return statement_->step(); }
    void run(DbConnection*) { statement_->run(); }     // INSERT / UPDATE / DELETE: throws on failure
    int64_t integer(int column) const { return statement_->integer(column); }
    int32_t int32(int column) const { return int32_t(statement_->integer(column)); }
    float real(int column) const { return float(statement_->realDouble(column)); }
    std::string text(int column) const { return statement_->text(column); }
    tr::Vec3 vec3(int column) const { return tr::Vec3{real(column), real(column + 1), real(column + 2)}; }
private:
    std::unique_ptr<DbStatement> statement_;
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

const char* npcPoseName(int32_t pose) {
    switch (pose) {
        case 0: return "none";
        case 1: return "standing";
        case 2: return "weapon out";
        case 3: return "crouched";
        case 4: return "crouched, weapon out";
        case 5: return "leaning";
        case 6: return "sitting";
        case 7: return "lying down";
        case 8: return "at a console";
        case 9: return "hand tool";
        default: return "unknown";
    }
}

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

const char* controlPointLinkKindName(int32_t kind) {
    switch (kind) {
        case 1: return "Bane garrison pool";
        case 2: return "AFS garrison pool";
        case 3: return "Hospital";
        case 4: return "Waypoint";
        case 5: return "Boss";
        case 6: return "Clan lockbox";
        default: return "unknown";
    }
}

const char* objectKindName(ObjectKind kind) {
    switch (kind) {
        case ObjectKind::Logos: return "Logos";
        case ObjectKind::ControlPoint: return "Control point";
        case ObjectKind::Footlocker: return "Footlocker";
        case ObjectKind::AmbientNpc: return "Ambient NPC";
        case ObjectKind::WorldUsable: return "World usable";
        case ObjectKind::WorldDestructible: return "World destructible";
        case ObjectKind::Emitter: return "FX emitter";
        default: return "Kraftwerk";
    }
}

WorldDatabase::WorldDatabase(const std::string& spec, const std::string& mySqlPluginDir) : path_(spec) {
    if (isMySqlSpec(spec)) {
        MySqlSpec m;
        if (!parseMySqlSpec(spec, m)) throw std::runtime_error("not a MySQL connection spec (mysql://user[:password]@host[:port]/database): " + spec);
        connection_ = openMySql(m, mySqlPluginDir);
        path_ = mySqlSpecString(m);                    // shown and remembered without the password
        mySql_ = true;
    } else {
        connection_ = openSqlite(spec, warnings_);
    }
    db_ = connection_.get();
    readOnly_ = db_->readOnly();
    for (const std::string& table : db_->tables()) {
        std::set<std::string>& columns = columns_[lower(table)];
        for (const DbColumnInfo& c : db_->columns(table)) columns.insert(lower(c.name));
    }
    if (!hasTable("spawnpool") && !hasTable("creature") && !hasTable("map_info"))
        throw std::runtime_error("this does not look like a Rasa.NET World database (no spawnpool, creature or map_info table)");
    loadGlobals();
}

WorldDatabase::~WorldDatabase() = default;

bool WorldDatabase::hasTable(const std::string& table) const { return columns_.count(lower(table)) != 0; }

bool WorldDatabase::hasColumn(const std::string& table, const std::string& column) const {
    const auto it = columns_.find(lower(table));
    return it != columns_.end() && it->second.count(lower(column)) != 0;
}

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
    npcPackages_.clear();
    greetings_.clear();
    greetingImportant_.clear();
    actorNames_.clear();
    vendors_.clear();
    vendorPrices_.clear();
    vendorItems_.clear();
    itemTemplates_.clear();
    creatureBattlecries_.clear();
    classBattlecries_.clear();
    classFlags_.clear();
    if (hasTable("creature_class_flag") && requireColumns("creature_class_flag", {"class_id", "flag_id"})) {
        Statement s(db_, "SELECT class_id, flag_id FROM creature_class_flag");
        while (s.step()) classFlags_[s.int32(0)].insert(s.int32(1));
    }
    if (requireColumns("creature_battlecry", {"scope", "target_id", "package_id"})) {
        Statement s(db_, "SELECT scope, target_id, package_id FROM creature_battlecry");
        while (s.step()) {
            if (s.int32(0) == 1) classBattlecries_[s.int32(1)] = s.int32(2);
            else if (s.int32(0) == 2) creatureBattlecries_[s.int32(1)] = s.int32(2);
        }
    }
    if (requireColumns("npc_package", {"id", "package_id", "comment"})) {
        Statement s(db_, "SELECT id, package_id, comment FROM npc_package");
        while (s.step()) npcPackages_[s.int32(0)] = {s.int32(1), s.text(2)};
    }
    if (requireColumns("npc_greeting", {"id", "greeting_id"})) {
        const bool important = hasColumn("npc_greeting", "important");
        Statement s(db_, important ? "SELECT id, greeting_id, important FROM npc_greeting" : "SELECT id, greeting_id FROM npc_greeting");
        while (s.step()) {
            greetings_[s.int32(0)] = s.int32(1);
            if (important) greetingImportant_[s.int32(0)] = s.int32(2) != 0;
        }
    }
    if (requireColumns("creature_actor_name", {"id", "actor_name"})) {
        Statement s(db_, "SELECT id, actor_name FROM creature_actor_name");
        while (s.step()) actorNames_[s.int32(0)] = s.text(1);
    }
    if (requireColumns("vendor", {"id", "package_id"})) {
        Statement s(db_, "SELECT id, package_id FROM vendor");
        while (s.step()) vendors_[s.int32(0)] = s.int32(1);
    }
    if (requireColumns("vendor_price", {"id", "item_price"})) {
        Statement s(db_, "SELECT id, item_price FROM vendor_price");
        while (s.step()) vendorPrices_[s.int32(0)] = s.int32(1);
    }
    if (requireColumns("vendor_item", {"id", "item_template_id"})) {
        Statement s(db_, std::string("SELECT id, item_template_id FROM vendor_item") + (db_->hasRowId() ? " ORDER BY rowid" : ""));
        while (s.step()) vendorItems_[s.int32(0)].push_back(s.int32(1));
    }
    if (requireColumns("itemtemplate", {"id", "buy_price"}) && requireColumns("itemtemplate_itemclass", {"itemtemplateid", "itemclassid"})) {
        const bool quality = hasColumn("itemtemplate", "quality_id");
        Statement s(db_, std::string("SELECT t.id, c.itemClassId, t.buy_price") + (quality ? ", t.quality_id" : "") +
                             " FROM itemtemplate t LEFT JOIN itemtemplate_itemclass c ON c.itemTemplateId = t.id");
        while (s.step()) itemTemplates_[s.int32(0)] = ItemTemplateInfo{s.int32(1), s.int32(2), quality ? s.int32(3) : 0};
    }
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
    if (a.hasNpcPackage != b.hasNpcPackage || (a.hasNpcPackage && (a.npcPackageId != b.npcPackageId || a.npcPackageComment != b.npcPackageComment)))
        return false;
    if (a.hasGreeting != b.hasGreeting || (a.hasGreeting && (a.greetingId != b.greetingId || a.greetingImportant != b.greetingImportant))) return false;
    if (a.hasActorName != b.hasActorName || (a.hasActorName && a.actorName != b.actorName)) return false;
    if (a.hasVendor != b.hasVendor || (a.hasVendor && a.vendorPackageId != b.vendorPackageId)) return false;
    if (a.hasVendorPrice != b.hasVendorPrice || (a.hasVendorPrice && a.vendorPrice != b.vendorPrice)) return false;
    if (a.vendorItems != b.vendorItems) return false;
    if (a.hasBattlecry != b.hasBattlecry || (a.hasBattlecry && a.battlecryPackage != b.battlecryPackage)) return false;
    if (a.appearance.size() != b.appearance.size()) return false;
    for (size_t k = 0; k < a.appearance.size(); ++k)
        if (a.appearance[k].slotId != b.appearance[k].slotId || a.appearance[k].classId != b.appearance[k].classId ||
            a.appearance[k].colour != b.appearance[k].colour)
            return false;
    return true;
}

bool operator==(const DbValue& a, const DbValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case DbValue::Type::Null: return true;
        case DbValue::Type::Int: return a.i == b.i;
        case DbValue::Type::Real: return a.r == b.r;
        default: return a.t == b.t;
    }
}

int DbTable::column(const std::string& wanted) const {
    for (size_t k = 0; k < columns.size(); ++k)
        if (columns[k] == wanted) return int(k);
    return -1;
}

DbTable WorldDatabase::loadTable(const std::string& table) const {
    DbTable t;
    t.name = table;
    std::vector<std::pair<int, int>> keyOrder;             // (pk position, column)
    for (const DbColumnInfo& c : db_->columns(table)) {
        const int index = int(t.columns.size());
        t.columns.push_back(c.name);
        t.declaredTypes.push_back(c.declaredType);
        t.nullable.push_back(c.nullable);
        if (c.keyPosition > 0) keyOrder.emplace_back(c.keyPosition, index);
    }
    if (t.columns.empty()) throw std::runtime_error("no table " + table);
    std::sort(keyOrder.begin(), keyOrder.end());
    for (const auto& k : keyOrder) t.key.push_back(k.second);
    std::string sql = "SELECT ";
    for (size_t k = 0; k < t.columns.size(); ++k) sql += (k ? ", \"" : "\"") + t.columns[k] + "\"";
    sql += " FROM \"" + table + "\"";
    if (!t.key.empty()) {
        sql += " ORDER BY ";
        for (size_t k = 0; k < t.key.size(); ++k) sql += (k ? ", \"" : "\"") + t.columns[size_t(t.key[k])] + "\"";
    }
    Statement s(db_, sql);
    while (s.step()) {
        std::vector<DbValue> row(t.columns.size());
        for (size_t k = 0; k < t.columns.size(); ++k) {
            switch (s.columnType(int(k))) {
                case DbType::Int: row[k] = DbValue::integer(s.integer(int(k))); break;
                case DbType::Real: row[k] = DbValue::real(s.realDouble(int(k))); break;
                case DbType::Null: break;
                default: row[k] = DbValue::text(s.text(int(k))); break;
            }
        }
        t.rows.push_back(std::move(row));
    }
    return t;
}

namespace {

void bindValue(Statement& s, int index, const DbValue& v) {
    switch (v.type) {
        case DbValue::Type::Null: s.bindNull(index); break;
        case DbValue::Type::Int: s.bindInt(index, v.i); break;
        case DbValue::Type::Real: s.bindReal(index, v.r); break;
        default: s.bindText(index, v.t); break;
    }
}

std::string keyString(const DbTable& t, const std::vector<DbValue>& row) {
    std::string out;
    for (int k : t.key) {
        const DbValue& v = row[size_t(k)];
        out += v.type == DbValue::Type::Text ? "s:" + v.t : v.type == DbValue::Type::Null ? "n:" : "i:" + std::to_string(v.asInt());
        out += '\x1f';
    }
    return out;
}

}  // namespace

bool WorldDatabase::saveTables(const std::vector<std::pair<const DbTable*, const DbTable*>>& tables, SaveSummary& summary, std::string& error) {
    summary = SaveSummary{};
    if (readOnly_) {
        error = "the database was opened read-only";
        return false;
    }
    try {
        db_->begin();
    } catch (const std::exception& e) {
        error = std::string("cannot start a transaction (is the server writing to the database?): ") + e.what();
        return false;
    }
    try {
        // Deletes, children first (reverse order).
        for (auto it = tables.rbegin(); it != tables.rend(); ++it) {
            const DbTable& saved = *it->first;
            const DbTable& edited = *it->second;
            if (saved.key.empty()) throw std::runtime_error(saved.name + " has no primary key");
            std::set<std::string> kept;
            for (const auto& row : edited.rows) kept.insert(keyString(edited, row));
            for (const auto& row : saved.rows) {
                if (kept.count(keyString(saved, row))) continue;
                std::string sql = "DELETE FROM \"" + saved.name + "\" WHERE ";
                for (size_t k = 0; k < saved.key.size(); ++k)
                    sql += (k ? " AND \"" : "\"") + saved.columns[size_t(saved.key[k])] + "\" = ?" + std::to_string(k + 1);
                Statement s(db_, sql);
                for (size_t k = 0; k < saved.key.size(); ++k) bindValue(s, int(k + 1), row[size_t(saved.key[k])]);
                s.run(db_);
                ++summary.deleted;
            }
        }
        // Inserts and updates, parents first.
        for (const auto& pair : tables) {
            const DbTable& saved = *pair.first;
            const DbTable& edited = *pair.second;
            std::map<std::string, const std::vector<DbValue>*> before;
            for (const auto& row : saved.rows) before[keyString(saved, row)] = &row;
            std::set<std::string> seen;
            for (const auto& row : edited.rows) {
                const std::string key = keyString(edited, row);
                if (!seen.insert(key).second) throw std::runtime_error(edited.name + ": two rows have the same key");
                const auto found = before.find(key);
                if (found == before.end()) {
                    std::string sql = "INSERT INTO \"" + edited.name + "\" (";
                    for (size_t k = 0; k < edited.columns.size(); ++k) sql += (k ? ", \"" : "\"") + edited.columns[k] + "\"";
                    sql += ") VALUES (";
                    for (size_t k = 0; k < edited.columns.size(); ++k) sql += (k ? ", ?" : "?") + std::to_string(k + 1);
                    sql += ")";
                    Statement s(db_, sql);
                    for (size_t k = 0; k < edited.columns.size(); ++k) bindValue(s, int(k + 1), row[k]);
                    s.run(db_);
                    ++summary.inserted;
                } else if (*found->second != row) {
                    std::string sql = "UPDATE \"" + edited.name + "\" SET ";
                    int index = 1;
                    bool first = true;
                    for (size_t k = 0; k < edited.columns.size(); ++k) {
                        if (std::find(edited.key.begin(), edited.key.end(), int(k)) != edited.key.end()) continue;
                        sql += (first ? "\"" : ", \"") + edited.columns[k] + "\" = ?" + std::to_string(index++);
                        first = false;
                    }
                    if (first) continue;                     // only key columns: nothing to update
                    sql += " WHERE ";
                    for (size_t k = 0; k < edited.key.size(); ++k)
                        sql += (k ? " AND \"" : "\"") + edited.columns[size_t(edited.key[k])] + "\" = ?" + std::to_string(index + int(k));
                    Statement s(db_, sql);
                    int bind = 1;
                    for (size_t k = 0; k < edited.columns.size(); ++k) {
                        if (std::find(edited.key.begin(), edited.key.end(), int(k)) != edited.key.end()) continue;
                        bindValue(s, bind++, row[k]);
                    }
                    for (size_t k = 0; k < edited.key.size(); ++k) bindValue(s, bind++, row[size_t(edited.key[k])]);
                    s.run(db_);
                    ++summary.updated;
                }
            }
        }
        db_->commit();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        try { db_->rollback(); } catch (const std::exception&) {}
        summary = SaveSummary{};
        return false;
    }
}

std::vector<int32_t> WorldDatabase::spawnableCreatures(int32_t mapContextId) const {
    std::set<int32_t> ids;
    std::string sql = "SELECT ";
    for (int k = 1; k <= 6; ++k)
        sql += std::string(k > 1 ? ", " : "") + "creature_" + std::to_string(k) + "_Id, creature_" + std::to_string(k) + "_max_count";
    Statement s(db_, sql + " FROM spawnpool WHERE map_context_id = ?1");
    s.bindInt(1, mapContextId);
    while (s.step())
        for (int k = 0; k < 6; ++k)
            if (s.int32(2 * k) > 0 && s.int32(2 * k + 1) > 0) ids.insert(s.int32(2 * k));
    return std::vector<int32_t>(ids.begin(), ids.end());
}

const ItemTemplateInfo* WorldDatabase::itemTemplate(int32_t templateId) const {
    const auto it = itemTemplates_.find(templateId);
    return it == itemTemplates_.end() ? nullptr : &it->second;
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
        if (const auto it = npcPackages_.find(c.id); it != npcPackages_.end()) {
            d.hasNpcPackage = true;
            d.npcPackageId = it->second.first;
            d.npcPackageComment = it->second.second;
        }
        if (const auto it = greetings_.find(c.id); it != greetings_.end()) { d.hasGreeting = true; d.greetingId = it->second; }
        if (const auto it = greetingImportant_.find(c.id); it != greetingImportant_.end()) d.greetingImportant = it->second;
        if (const auto it = actorNames_.find(c.id); it != actorNames_.end()) { d.hasActorName = true; d.actorName = it->second; }
        if (const auto it = vendors_.find(c.id); it != vendors_.end()) { d.hasVendor = true; d.vendorPackageId = it->second; }
        if (const auto it = vendorPrices_.find(c.id); it != vendorPrices_.end()) { d.hasVendorPrice = true; d.vendorPrice = it->second; }
        if (const auto it = vendorItems_.find(c.id); it != vendorItems_.end()) d.vendorItems = it->second;
        if (const auto it = creatureBattlecries_.find(c.id); it != creatureBattlecries_.end()) { d.hasBattlecry = true; d.battlecryPackage = it->second; }
        out.push_back(std::move(d));
    }
    return out;
}

bool WorldDatabase::saveCreatures(const std::vector<CreatureDefinition>& saved, const std::vector<CreatureDefinition>& edited, SaveSummary& summary,
                                  std::string& error, const std::map<int32_t, int32_t>* savedClassCries,
                                  const std::map<int32_t, int32_t>* editedClassCries, const std::map<int32_t, std::set<int32_t>>* savedClassFlags,
                                  const std::map<int32_t, std::set<int32_t>>* editedClassFlags) {
    summary = SaveSummary{};
    if (readOnly_) {
        error = "the database was opened read-only";
        return false;
    }
    try {
        db_->begin();
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
            // One-row tables keyed by the creature id: insert, update or delete the row as the flag and value say.
            auto oneRow = [&](const char* table, bool had, bool has, bool same, const std::string& insertSql, const std::string& updateSql,
                              const std::function<void(Statement&)>& bind) {
                if (has && !had) {
                    Statement s(db_, insertSql);
                    bind(s);
                    s.run(db_);
                    ++summary.inserted;
                } else if (has && had && !same) {
                    Statement s(db_, updateSql);
                    bind(s);
                    s.run(db_);
                    ++summary.updated;
                } else if (!has && had) {
                    Statement s(db_, std::string("DELETE FROM ") + table + " WHERE id = ?1");
                    s.bindInt(1, c.id);
                    s.run(db_);
                    ++summary.deleted;
                }
            };
            oneRow("npc_package", b && b->hasNpcPackage, d.hasNpcPackage,
                   b && b->npcPackageId == d.npcPackageId && b->npcPackageComment == d.npcPackageComment,
                   "INSERT INTO npc_package (id, package_id, comment) VALUES (?1, ?2, ?3)",
                   "UPDATE npc_package SET package_id = ?2, comment = ?3 WHERE id = ?1", [&](Statement& s) {
                       s.bindInt(1, c.id);
                       s.bindInt(2, d.npcPackageId);
                       s.bindText(3, d.npcPackageComment);
                   });
            // npc_greeting.important only where the world has the column (Rasa.NET upstream, migration
            // 20261129000000_Add_npc_greeting_important: NOT NULL DEFAULT 0).
            const bool importantColumn = hasColumn("npc_greeting", "important");
            oneRow("npc_greeting", b && b->hasGreeting, d.hasGreeting,
                   b && b->greetingId == d.greetingId && (!importantColumn || b->greetingImportant == d.greetingImportant),
                   importantColumn ? "INSERT INTO npc_greeting (id, greeting_id, important) VALUES (?1, ?2, ?3)"
                                   : "INSERT INTO npc_greeting (id, greeting_id) VALUES (?1, ?2)",
                   importantColumn ? "UPDATE npc_greeting SET greeting_id = ?2, important = ?3 WHERE id = ?1"
                                   : "UPDATE npc_greeting SET greeting_id = ?2 WHERE id = ?1",
                   [&](Statement& s) {
                       s.bindInt(1, c.id);
                       s.bindInt(2, d.greetingId);
                       if (importantColumn) s.bindInt(3, d.greetingImportant ? 1 : 0);
                   });
            oneRow("creature_actor_name", b && b->hasActorName, d.hasActorName, b && b->actorName == d.actorName,
                   "INSERT INTO creature_actor_name (id, actor_name) VALUES (?1, ?2)", "UPDATE creature_actor_name SET actor_name = ?2 WHERE id = ?1",
                   [&](Statement& s) {
                       s.bindInt(1, c.id);
                       s.bindText(2, d.actorName);
                   });
            oneRow("vendor", b && b->hasVendor, d.hasVendor, b && b->vendorPackageId == d.vendorPackageId,
                   "INSERT INTO vendor (id, package_id) VALUES (?1, ?2)", "UPDATE vendor SET package_id = ?2 WHERE id = ?1", [&](Statement& s) {
                       s.bindInt(1, c.id);
                       s.bindInt(2, d.vendorPackageId);
                   });
            oneRow("vendor_price", b && b->hasVendorPrice, d.hasVendorPrice, b && b->vendorPrice == d.vendorPrice,
                   "INSERT INTO vendor_price (id, item_price) VALUES (?1, ?2)", "UPDATE vendor_price SET item_price = ?2 WHERE id = ?1",
                   [&](Statement& s) {
                       s.bindInt(1, c.id);
                       s.bindInt(2, d.vendorPrice);
                   });
            if (!b || b->vendorItems != d.vendorItems) {     // vendor_item rows have no key of their own: replace the stock
                Statement del(db_, "DELETE FROM vendor_item WHERE id = ?1");
                del.bindInt(1, c.id);
                del.run(db_);
                for (int32_t item : d.vendorItems) {
                    Statement s(db_, "INSERT INTO vendor_item (id, item_template_id) VALUES (?1, ?2)");
                    s.bindInt(1, c.id);
                    s.bindInt(2, item);
                    s.run(db_);
                }
                if (b || !d.vendorItems.empty()) ++summary.updated;
            }
            // creature_battlecry scope 2 (key: scope, target_id).
            const bool hadCry = b && b->hasBattlecry;
            if (d.hasBattlecry && (!hadCry || b->battlecryPackage != d.battlecryPackage)) {
                Statement s(db_, hadCry ? "UPDATE creature_battlecry SET package_id = ?2 WHERE scope = 2 AND target_id = ?1"
                                        : "INSERT INTO creature_battlecry (scope, target_id, package_id) VALUES (2, ?1, ?2)");
                s.bindInt(1, c.id);
                s.bindInt(2, d.battlecryPackage);
                s.run(db_);
                ++(hadCry ? summary.updated : summary.inserted);
            } else if (!d.hasBattlecry && hadCry) {
                Statement s(db_, "DELETE FROM creature_battlecry WHERE scope = 2 AND target_id = ?1");
                s.bindInt(1, c.id);
                s.run(db_);
                ++summary.deleted;
            }
        }
        // creature_battlecry scope 1: class rows that differ.
        if (savedClassCries && editedClassCries) {
            for (const auto& [classId, package] : *editedClassCries) {
                const auto before = savedClassCries->find(classId);
                if (before != savedClassCries->end() && before->second == package) continue;
                Statement s(db_, before != savedClassCries->end()
                                     ? "UPDATE creature_battlecry SET package_id = ?2 WHERE scope = 1 AND target_id = ?1"
                                     : "INSERT INTO creature_battlecry (scope, target_id, package_id) VALUES (1, ?1, ?2)");
                s.bindInt(1, classId);
                s.bindInt(2, package);
                s.run(db_);
                ++(before != savedClassCries->end() ? summary.updated : summary.inserted);
            }
            for (const auto& [classId, package] : *savedClassCries) {
                if (editedClassCries->count(classId)) continue;
                Statement s(db_, "DELETE FROM creature_battlecry WHERE scope = 1 AND target_id = ?1");
                s.bindInt(1, classId);
                s.run(db_);
                ++summary.deleted;
            }
        }
        // creature_class_flag: the (class, flag) rows that differ.
        if (savedClassFlags && editedClassFlags && hasTable("creature_class_flag")) {
            const auto has = [](const std::map<int32_t, std::set<int32_t>>& m, int32_t classId, int32_t flag) {
                const auto it = m.find(classId);
                return it != m.end() && it->second.count(flag) != 0;
            };
            for (const auto& [classId, flags] : *savedClassFlags)
                for (int32_t flag : flags) {
                    if (has(*editedClassFlags, classId, flag)) continue;
                    Statement s(db_, "DELETE FROM creature_class_flag WHERE class_id = ?1 AND flag_id = ?2");
                    s.bindInt(1, classId);
                    s.bindInt(2, flag);
                    s.run(db_);
                    ++summary.deleted;
                }
            for (const auto& [classId, flags] : *editedClassFlags)
                for (int32_t flag : flags) {
                    if (has(*savedClassFlags, classId, flag)) continue;
                    Statement s(db_, "INSERT INTO creature_class_flag (class_id, flag_id) VALUES (?1, ?2)");
                    s.bindInt(1, classId);
                    s.bindInt(2, flag);
                    s.run(db_);
                    ++summary.inserted;
                }
        }
        db_->commit();
    } catch (const std::exception& e) {
        error = e.what();
        try { db_->rollback(); } catch (const std::exception&) {}
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
    // Patrols and poses (Rasa.NET upstream; older worlds have neither table).
    if (!content.spawnPools.empty() && hasTable("spawnpool_patrol") &&
        requireColumns("spawnpool_patrol", {"pool_id", "step", "pos_x", "pos_y", "pos_z", "facing", "pause_ms"})) {
        Statement s(db_, "SELECT a.pool_id, a.step, a.pos_x, a.pos_y, a.pos_z, a.facing, a.pause_ms FROM spawnpool_patrol a "
                         "JOIN spawnpool p ON p.id = a.pool_id WHERE p.map_context_id = ?1 ORDER BY a.pool_id, a.step");
        s.bindInt(1, mapContextId);
        while (s.step()) {
            PatrolStep step;
            step.poolId = s.integer(0);
            step.step = s.int32(1);
            step.position = s.vec3(2);
            step.hasFacing = !s.isNull(5);
            step.facing = step.hasFacing ? s.real(5) : 0.0f;
            step.pauseMs = s.int32(6);
            content.patrolSteps.push_back(step);
        }
    }
    if (!content.spawnPools.empty() && hasTable("spawnpool_pose") && requireColumns("spawnpool_pose", {"id", "pose"})) {
        Statement s(db_, "SELECT a.id, a.pose FROM spawnpool_pose a JOIN spawnpool p ON p.id = a.id WHERE p.map_context_id = ?1 ORDER BY a.id");
        s.bindInt(1, mapContextId);
        while (s.step()) content.poses.push_back(PoolPose{s.integer(0), s.int32(1)});
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
            r.underground = s.int32(12);
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
    // The control points' own columns (Rasa.NET upstream).
    if (hasColumn("control_point", "marker_entity_id") && hasColumn("control_point", "default_owner")) {
        Statement s(db_, "SELECT id, marker_entity_id, default_owner FROM control_point WHERE map_context_id = ?1");
        s.bindInt(1, mapContextId);
        while (s.step())
            for (PlacedObject& o : content.objects)
                if (o.kind == ObjectKind::ControlPoint && o.id == s.integer(0)) {
                    o.controlPoint.markerEntityId = s.integer(1);
                    o.controlPoint.defaultOwner = s.integer(2);
                }
    }
    // Rasa.NET upstream object tables (older worlds have none of them).
    if (hasTable("ambient_npc") && requireColumns("ambient_npc", {"id", "map_context_id", "class_id", "pos_x", "pos_y", "pos_z", "rotation", "comment"})) {
        Statement s(db_, "SELECT id, class_id, map_context_id, pos_x, pos_y, pos_z, rotation, comment FROM ambient_npc WHERE map_context_id = ?1 ORDER BY id");
        s.bindInt(1, mapContextId);
        while (s.step())
            content.objects.push_back(PlacedObject{ObjectKind::AmbientNpc, s.integer(0), s.int32(1), s.int32(2), s.vec3(3), s.real(6), s.text(7)});
    }
    if (hasTable("world_usable") &&
        requireColumns("world_usable", {"id", "map_context_id", "class_id", "pos_x", "pos_y", "pos_z", "rotation", "state", "usable", "item_template_id",
                                        "item_class_id", "item_count", "reset_seconds", "interval_min_seconds", "interval_max_seconds", "damage_min",
                                        "damage_max", "radius", "action_arg", "comment"})) {
        Statement s(db_, "SELECT id, class_id, map_context_id, pos_x, pos_y, pos_z, rotation, comment, state, usable, item_template_id, item_class_id, "
                         "item_count, reset_seconds, interval_min_seconds, interval_max_seconds, damage_min, damage_max, radius, action_arg "
                         "FROM world_usable WHERE map_context_id = ?1 ORDER BY id");
        s.bindInt(1, mapContextId);
        while (s.step()) {
            PlacedObject o{ObjectKind::WorldUsable, s.integer(0), s.int32(1), s.int32(2), s.vec3(3), s.real(6), s.text(7)};
            UsableSettings& u = o.usable;
            u.state = s.integer(8);
            u.usable = s.integer(9);
            u.itemTemplateId = s.integer(10);
            u.itemClassId = s.integer(11);
            u.itemCount = s.integer(12);
            u.resetSeconds = s.integer(13);
            u.intervalMinSeconds = s.integer(14);
            u.intervalMaxSeconds = s.integer(15);
            u.damageMin = s.integer(16);
            u.damageMax = s.integer(17);
            u.radius = s.real(18);
            u.actionArg = s.integer(19);
            content.objects.push_back(o);
        }
    }
    if (hasTable("world_destructible") &&
        requireColumns("world_destructible", {"id", "map_context_id", "class_id", "pos_x", "pos_y", "pos_z", "rotation", "hit_points", "creature_id",
                                              "creature_count", "side", "comment"})) {
        Statement s(db_, "SELECT id, class_id, map_context_id, pos_x, pos_y, pos_z, rotation, comment, hit_points, creature_id, creature_count, side "
                         "FROM world_destructible WHERE map_context_id = ?1 ORDER BY id");
        s.bindInt(1, mapContextId);
        while (s.step()) {
            PlacedObject o{ObjectKind::WorldDestructible, s.integer(0), s.int32(1), s.int32(2), s.vec3(3), s.real(6), s.text(7)};
            DestructibleSettings& d = o.destructible;
            d.hitPoints = s.integer(8);
            d.creatureId = s.integer(9);
            d.creatureCount = s.integer(10);
            d.side = s.integer(11);
            content.objects.push_back(o);
        }
    }
    if (hasTable("map_emitter") &&
        requireColumns("map_emitter", {"id", "map_context_id", "pos_x", "pos_y", "pos_z", "rotation", "package_id", "is_on", "comment"})) {
        Statement s(db_, "SELECT id, map_context_id, pos_x, pos_y, pos_z, rotation, comment, package_id, is_on FROM map_emitter "
                         "WHERE map_context_id = ?1 ORDER BY id");
        s.bindInt(1, mapContextId);
        while (s.step()) {
            PlacedObject o{ObjectKind::Emitter, s.integer(0), kEmitterClassId, s.int32(1), s.vec3(2), s.real(5), s.text(6)};
            o.emitter.packageId = s.integer(7);
            o.emitter.isOn = s.integer(8) != 0;
            content.objects.push_back(o);
        }
    }
    if (hasTable("map_marker") &&
        requireColumns("map_marker", {"marker_entity_id", "map_context_id", "marker_type", "object_kind", "object_id", "match_distance", "comment"})) {
        Statement s(db_, "SELECT marker_entity_id, marker_type, object_kind, object_id, match_distance, comment FROM map_marker WHERE map_context_id = ?1 "
                         "ORDER BY marker_entity_id");
        s.bindInt(1, mapContextId);
        while (s.step()) {
            MapMarkerBinding m;
            m.markerEntityId = s.integer(0);
            m.markerType = s.int32(1);
            m.objectKind = s.int32(2);
            m.objectId = s.integer(3);
            m.matchDistance = s.real(4);
            m.hasComment = !s.isNull(5);
            m.comment = m.hasComment ? s.text(5) : std::string();
            content.mapMarkers.push_back(m);
        }
    }
    if (hasTable("control_point_link") && requireColumns("control_point_link", {"control_point_id", "kind", "object_id"})) {
        Statement s(db_, "SELECT l.control_point_id, l.kind, l.object_id FROM control_point_link l JOIN control_point p ON p.id = l.control_point_id "
                         "WHERE p.map_context_id = ?1 ORDER BY l.control_point_id, l.kind, l.object_id");
        s.bindInt(1, mapContextId);
        while (s.step()) content.controlPointLinks.push_back(ControlPointLink{s.integer(0), s.int32(1), s.integer(2)});
    }
    if (hasTable("wormhole_lock") && requireColumns("wormhole_lock", {"id", "from_teleporter_id", "to_teleporter_id", "kind", "value", "comment"})) {
        Statement s(db_, "SELECT id, from_teleporter_id, to_teleporter_id, kind, value, comment FROM wormhole_lock ORDER BY id");
        while (s.step()) {
            WormholeLock w;
            w.id = s.integer(0);
            w.fromTeleporterId = s.integer(1);
            w.toTeleporterId = s.integer(2);
            w.kind = s.integer(3);
            w.value = s.integer(4);
            w.hasComment = !s.isNull(5);
            w.comment = w.hasComment ? s.text(5) : std::string();
            content.wormholeLocks.push_back(w);
        }
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

bool same(const PatrolStep& a, const PatrolStep& b) {
    return a.poolId == b.poolId && a.step == b.step && sameVec(a.position, b.position) && a.hasFacing == b.hasFacing &&
           (!a.hasFacing || a.facing == b.facing) && a.pauseMs == b.pauseMs;
}

bool same(const Teleporter& a, const Teleporter& b) {
    return a.classId == b.classId && a.type == b.type && a.description == b.description && sameVec(a.position, b.position) && a.rotation == b.rotation;
}
bool same(const UsableSettings& a, const UsableSettings& b) {
    return a.state == b.state && a.usable == b.usable && a.itemTemplateId == b.itemTemplateId && a.itemClassId == b.itemClassId &&
           a.itemCount == b.itemCount && a.resetSeconds == b.resetSeconds && a.intervalMinSeconds == b.intervalMinSeconds &&
           a.intervalMaxSeconds == b.intervalMaxSeconds && a.damageMin == b.damageMin && a.damageMax == b.damageMax && a.radius == b.radius &&
           a.actionArg == b.actionArg;
}
bool same(const DestructibleSettings& a, const DestructibleSettings& b) {
    return a.hitPoints == b.hitPoints && a.creatureId == b.creatureId && a.creatureCount == b.creatureCount && a.side == b.side;
}
bool same(const PlacedObject& a, const PlacedObject& b) {
    return a.classId == b.classId && sameVec(a.position, b.position) && a.rotation == b.rotation && a.name == b.name &&
           (a.kind != ObjectKind::WorldUsable || same(a.usable, b.usable)) && (a.kind != ObjectKind::WorldDestructible || same(a.destructible, b.destructible)) &&
           (a.kind != ObjectKind::Emitter || (a.emitter.packageId == b.emitter.packageId && a.emitter.isOn == b.emitter.isOn)) &&
           (a.kind != ObjectKind::ControlPoint ||
            (a.controlPoint.markerEntityId == b.controlPoint.markerEntityId && a.controlPoint.defaultOwner == b.controlPoint.defaultOwner));
}
bool same(const WormholeLock& a, const WormholeLock& b) {
    return a.fromTeleporterId == b.fromTeleporterId && a.toTeleporterId == b.toTeleporterId && a.kind == b.kind && a.value == b.value &&
           a.hasComment == b.hasComment && (!a.hasComment || a.comment == b.comment);
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
        case ObjectKind::AmbientNpc: return "ambient_npc";
        case ObjectKind::WorldUsable: return "world_usable";
        case ObjectKind::WorldDestructible: return "world_destructible";
        case ObjectKind::Emitter: return "map_emitter";
        default: return "kraftwerks";
    }
}

}  // namespace

void WorldDatabase::execute(const std::string& sql) { db_->execute(sql); }

bool WorldDatabase::save(const WorldContent& saved, const WorldContent& edited, SaveSummary& summary, std::string& error) {
    summary = SaveSummary{};
    if (readOnly_) {
        error = "the database was opened read-only";
        return false;
    }
    const int64_t context = edited.mapContextId;
    try {
        db_->begin();
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

        // Spawn pools, and the rows keyed by a deleted pool's id: its arrival points, patrol steps and pose (Rasa.NET
        // spawnpool_patrol.pool_id / spawnpool_pose.id = spawnpool.id; no foreign key removes them).
        for (int64_t id : deletedIds(saved.spawnPools, edited.spawnPools)) {
            for (const char* sql : {"DELETE FROM spawnpool_arrival WHERE pool_id = ?1", "DELETE FROM spawnpool_patrol WHERE pool_id = ?1",
                                    "DELETE FROM spawnpool_pose WHERE id = ?1"}) {
                const std::string table = std::string(sql).substr(12, std::string(sql).find(' ', 12) - 12);
                if (!hasTable(table)) continue;               // older worlds lack the patrol / pose tables
                Statement rows(db_, sql);
                rows.bindInt(1, id);
                rows.run(db_);
            }
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

        // Patrol steps, by (pool, step): renumbering a pool's steps updates the rows of the numbers that stay and inserts /
        // deletes the rest. Steps of unsaved pools (id 0) are not written.
        if (hasTable("spawnpool_patrol")) {
            auto find = [](const std::vector<PatrolStep>& steps, int64_t pool, int32_t number) -> const PatrolStep* {
                for (const PatrolStep& s : steps)
                    if (s.poolId == pool && s.step == number) return &s;
                return nullptr;
            };
            for (const PatrolStep& b : saved.patrolSteps) {
                if (find(edited.patrolSteps, b.poolId, b.step)) continue;
                Statement s(db_, "DELETE FROM spawnpool_patrol WHERE pool_id = ?1 AND step = ?2");
                s.bindInt(1, b.poolId);
                s.bindInt(2, b.step);
                s.run(db_);
                ++summary.deleted;
            }
            for (const PatrolStep& e : edited.patrolSteps) {
                if (e.poolId <= 0) continue;
                const PatrolStep* before = find(saved.patrolSteps, e.poolId, e.step);
                if (before && same(*before, e)) continue;
                Statement s(db_, before ? "UPDATE spawnpool_patrol SET pos_x = ?3, pos_y = ?4, pos_z = ?5, facing = ?6, pause_ms = ?7 "
                                          "WHERE pool_id = ?1 AND step = ?2"
                                        : "INSERT INTO spawnpool_patrol (pool_id, step, pos_x, pos_y, pos_z, facing, pause_ms) "
                                          "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
                s.bindInt(1, e.poolId);
                s.bindInt(2, e.step);
                s.bindReal(3, e.position.x);
                s.bindReal(4, e.position.y);
                s.bindReal(5, e.position.z);
                if (e.hasFacing) s.bindReal(6, e.facing);
                else s.bindNull(6);
                s.bindInt(7, e.pauseMs);
                s.run(db_);
                ++(before ? summary.updated : summary.inserted);
            }
        }
        // Poses, by pool.
        if (hasTable("spawnpool_pose")) {
            auto find = [](const std::vector<PoolPose>& poses, int64_t pool) -> const PoolPose* {
                for (const PoolPose& p : poses)
                    if (p.poolId == pool) return &p;
                return nullptr;
            };
            for (const PoolPose& b : saved.poses) {
                if (find(edited.poses, b.poolId)) continue;
                Statement s(db_, "DELETE FROM spawnpool_pose WHERE id = ?1");
                s.bindInt(1, b.poolId);
                s.run(db_);
                ++summary.deleted;
            }
            for (const PoolPose& e : edited.poses) {
                if (e.poolId <= 0) continue;
                const PoolPose* before = find(saved.poses, e.poolId);
                if (before && before->pose == e.pose) continue;
                Statement s(db_, before ? "UPDATE spawnpool_pose SET pose = ?2 WHERE id = ?1" : "INSERT INTO spawnpool_pose (id, pose) VALUES (?1, ?2)");
                s.bindInt(1, e.poolId);
                s.bindInt(2, e.pose);
                s.run(db_);
                ++(before ? summary.updated : summary.inserted);
            }
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
            if (kept) continue;
            if (b.kind == ObjectKind::ControlPoint && hasTable("control_point_link")) {
                // Its links go with it (the server logs a link to a missing point as an error).
                Statement links(db_, "DELETE FROM control_point_link WHERE control_point_id = ?1");
                links.bindInt(1, b.id);
                links.run(db_);
            }
            Statement s(db_, std::string("DELETE FROM ") + objectTable(b.kind) + " WHERE id = ?1");
            s.bindInt(1, b.id);
            s.run(db_);
            ++summary.deleted;
        }
        for (const PlacedObject& o : edited.objects) {
            const PlacedObject* before = nullptr;
            for (const PlacedObject& b : saved.objects) if (b.kind == o.kind && b.id == o.id) { before = &b; break; }
            if (o.id != 0 && (!before || same(*before, o))) continue;
            if (o.kind == ObjectKind::ControlPoint) {
                // Columns per Rasa.NET ControlPointEntry; the id is not generated (largest + 1).
                const bool extra = hasColumn("control_point", "marker_entity_id") && hasColumn("control_point", "default_owner");
                int64_t id = o.id;
                if (id == 0) {
                    Statement next(db_, "SELECT COALESCE(MAX(id), 0) + 1 FROM control_point");
                    if (next.step()) id = next.integer(0);
                }
                const std::string sql =
                    o.id == 0 ? std::string("INSERT INTO control_point (class_id, map_context_id, pos_x, pos_y, pos_z, name, rotation") +
                                    (extra ? ", marker_entity_id, default_owner" : "") + ", id) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7" +
                                    (extra ? ", ?8, ?9" : "") + ", ?10)"
                              : std::string("UPDATE control_point SET class_id = ?1, map_context_id = ?2, pos_x = ?3, pos_y = ?4, pos_z = ?5, name = ?6, "
                                            "rotation = ?7") + (extra ? ", marker_entity_id = ?8, default_owner = ?9" : "") + " WHERE id = ?10";
                Statement s(db_, sql);
                s.bindInt(1, o.classId);
                s.bindInt(2, context);
                s.bindReal(3, o.position.x);
                s.bindReal(4, o.position.y);
                s.bindReal(5, o.position.z);
                s.bindText(6, o.name);
                s.bindReal(7, o.rotation);
                if (extra) {
                    s.bindInt(8, o.controlPoint.markerEntityId);
                    s.bindInt(9, o.controlPoint.defaultOwner);
                }
                s.bindInt(10, id);
                s.run(db_);
                ++(o.id == 0 ? summary.inserted : summary.updated);
                continue;
            }
            if (o.kind == ObjectKind::Emitter) {
                // Columns per Rasa.NET MapEmitterEntry (no class column: always FXPackageEmitter).
                Statement s(db_, o.id == 0 ? "INSERT INTO map_emitter (map_context_id, pos_x, pos_y, pos_z, rotation, package_id, is_on, comment) "
                                             "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)"
                                           : "UPDATE map_emitter SET map_context_id = ?1, pos_x = ?2, pos_y = ?3, pos_z = ?4, rotation = ?5, "
                                             "package_id = ?6, is_on = ?7, comment = ?8 WHERE id = ?9");
                s.bindInt(1, context);
                s.bindReal(2, o.position.x);
                s.bindReal(3, o.position.y);
                s.bindReal(4, o.position.z);
                s.bindReal(5, o.rotation);
                s.bindInt(6, o.emitter.packageId);
                s.bindInt(7, o.emitter.isOn ? 1 : 0);
                s.bindText(8, o.name);
                if (o.id != 0) s.bindInt(9, o.id);
                s.run(db_);
                ++(o.id == 0 ? summary.inserted : summary.updated);
                continue;
            }
            if (o.kind == ObjectKind::AmbientNpc || o.kind == ObjectKind::WorldUsable || o.kind == ObjectKind::WorldDestructible) {
                // Columns per Rasa.NET AmbientNpcEntry / WorldUsableEntry / WorldDestructibleEntry.
                std::vector<std::string> columns = {"class_id", "map_context_id", "pos_x", "pos_y", "pos_z", "rotation", "comment"};
                if (o.kind == ObjectKind::WorldUsable)
                    for (const char* c : {"state", "usable", "item_template_id", "item_class_id", "item_count", "reset_seconds", "interval_min_seconds",
                                          "interval_max_seconds", "damage_min", "damage_max", "radius", "action_arg"})
                        columns.push_back(c);
                if (o.kind == ObjectKind::WorldDestructible)
                    for (const char* c : {"hit_points", "creature_id", "creature_count", "side"}) columns.push_back(c);
                std::string sql;
                if (o.id == 0) {
                    std::string names, values;
                    for (size_t k = 0; k < columns.size(); ++k) {
                        names += (k ? ", " : "") + columns[k];
                        values += (k ? ", ?" : "?") + std::to_string(k + 1);
                    }
                    sql = std::string("INSERT INTO ") + objectTable(o.kind) + " (" + names + ") VALUES (" + values + ")";
                } else {
                    std::string sets;
                    for (size_t k = 0; k < columns.size(); ++k) sets += (k ? ", " : "") + columns[k] + " = ?" + std::to_string(k + 1);
                    sql = std::string("UPDATE ") + objectTable(o.kind) + " SET " + sets + " WHERE id = ?" + std::to_string(columns.size() + 1);
                }
                Statement s(db_, sql);
                int n = 1;
                s.bindInt(n++, o.classId);
                s.bindInt(n++, context);
                s.bindReal(n++, o.position.x);
                s.bindReal(n++, o.position.y);
                s.bindReal(n++, o.position.z);
                s.bindReal(n++, o.rotation);
                s.bindText(n++, o.name);
                if (o.kind == ObjectKind::WorldUsable) {
                    const UsableSettings& u = o.usable;
                    for (int64_t v : {u.state, u.usable, u.itemTemplateId, u.itemClassId, u.itemCount, u.resetSeconds, u.intervalMinSeconds,
                                      u.intervalMaxSeconds, u.damageMin, u.damageMax})
                        s.bindInt(n++, v);
                    s.bindReal(n++, u.radius);
                    s.bindInt(n++, u.actionArg);
                }
                if (o.kind == ObjectKind::WorldDestructible) {
                    const DestructibleSettings& d = o.destructible;
                    for (int64_t v : {d.hitPoints, d.creatureId, d.creatureCount, d.side}) s.bindInt(n++, v);
                }
                if (o.id != 0) s.bindInt(n, o.id);
                s.run(db_);
                ++(o.id == 0 ? summary.inserted : summary.updated);
                continue;
            }
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

        // Wormhole locks (all rows, keyed by id).
        if (hasTable("wormhole_lock")) {
            for (int64_t id : deletedIds(saved.wormholeLocks, edited.wormholeLocks)) {
                Statement s(db_, "DELETE FROM wormhole_lock WHERE id = ?1");
                s.bindInt(1, id);
                s.run(db_);
                ++summary.deleted;
            }
            for (const WormholeLock& w : edited.wormholeLocks) {
                const WormholeLock* before = w.id ? findById(saved.wormholeLocks, w.id) : nullptr;
                if (w.id != 0 && (!before || same(*before, w))) continue;
                Statement s(db_, w.id == 0 ? "INSERT INTO wormhole_lock (from_teleporter_id, to_teleporter_id, kind, value, comment) VALUES (?1, ?2, ?3, ?4, ?5)"
                                           : "UPDATE wormhole_lock SET from_teleporter_id = ?1, to_teleporter_id = ?2, kind = ?3, value = ?4, comment = ?5 "
                                             "WHERE id = ?6");
                s.bindInt(1, w.fromTeleporterId);
                s.bindInt(2, w.toTeleporterId);
                s.bindInt(3, w.kind);
                s.bindInt(4, w.value);
                if (w.hasComment) s.bindText(5, w.comment);
                else s.bindNull(5);
                if (w.id != 0) s.bindInt(6, w.id);
                s.run(db_);
                ++(w.id == 0 ? summary.inserted : summary.updated);
            }
        }

        // Map markers, keyed by (marker entity id, this map).
        if (hasTable("map_marker")) {
            const auto find = [](const std::vector<MapMarkerBinding>& list, int64_t id) -> const MapMarkerBinding* {
                for (const MapMarkerBinding& m : list)
                    if (m.markerEntityId == id) return &m;
                return nullptr;
            };
            for (const MapMarkerBinding& m : saved.mapMarkers) {
                if (find(edited.mapMarkers, m.markerEntityId)) continue;
                Statement s(db_, "DELETE FROM map_marker WHERE marker_entity_id = ?1 AND map_context_id = ?2");
                s.bindInt(1, m.markerEntityId);
                s.bindInt(2, context);
                s.run(db_);
                ++summary.deleted;
            }
            for (const MapMarkerBinding& m : edited.mapMarkers) {
                const MapMarkerBinding* before = find(saved.mapMarkers, m.markerEntityId);
                if (before && before->markerType == m.markerType && before->objectKind == m.objectKind && before->objectId == m.objectId &&
                    before->matchDistance == m.matchDistance && before->hasComment == m.hasComment && (!m.hasComment || before->comment == m.comment))
                    continue;
                Statement s(db_, before ? "UPDATE map_marker SET marker_type = ?3, object_kind = ?4, object_id = ?5, match_distance = ?6, comment = ?7 "
                                          "WHERE marker_entity_id = ?1 AND map_context_id = ?2"
                                        : "INSERT INTO map_marker (marker_entity_id, map_context_id, marker_type, object_kind, object_id, match_distance, comment) "
                                          "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
                s.bindInt(1, m.markerEntityId);
                s.bindInt(2, context);
                s.bindInt(3, m.markerType);
                s.bindInt(4, m.objectKind);
                s.bindInt(5, m.objectId);
                s.bindReal(6, m.matchDistance);
                if (m.hasComment) s.bindText(7, m.comment);
                s.run(db_);
                ++(before ? summary.updated : summary.inserted);
            }
        }

        // Control point links: the key is the whole row, so a change is a delete and an insert.
        if (hasTable("control_point_link")) {
            const auto sameLink = [](const ControlPointLink& a, const ControlPointLink& b) {
                return a.controlPointId == b.controlPointId && a.kind == b.kind && a.objectId == b.objectId;
            };
            const auto contains = [&](const std::vector<ControlPointLink>& list, const ControlPointLink& l) {
                return std::any_of(list.begin(), list.end(), [&](const ControlPointLink& x) { return sameLink(x, l); });
            };
            for (const ControlPointLink& l : saved.controlPointLinks) {
                if (contains(edited.controlPointLinks, l)) continue;
                Statement s(db_, "DELETE FROM control_point_link WHERE control_point_id = ?1 AND kind = ?2 AND object_id = ?3");
                s.bindInt(1, l.controlPointId);
                s.bindInt(2, l.kind);
                s.bindInt(3, l.objectId);
                s.run(db_);
                ++summary.deleted;
            }
            for (const ControlPointLink& l : edited.controlPointLinks) {
                if (contains(saved.controlPointLinks, l) || l.controlPointId == 0 || l.objectId == 0) continue;
                Statement s(db_, "INSERT INTO control_point_link (control_point_id, kind, object_id) VALUES (?1, ?2, ?3)");
                s.bindInt(1, l.controlPointId);
                s.bindInt(2, l.kind);
                s.bindInt(3, l.objectId);
                s.run(db_);
                ++summary.inserted;
            }
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
            s.bindInt(11, r.underground);
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
        db_->commit();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        try { db_->rollback(); } catch (const std::exception&) {}
        summary = SaveSummary{};
        return false;
    }
}
