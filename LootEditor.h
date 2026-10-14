#pragma once

#include "WorldDatabase.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class EditorContext;

// The Rasa.NET ordinary loot tables (Rasa.Game/Loot, read 2026-10-14): loot_profile -> ordinary_loot_group -> loot_item, bound
// to (map, creature) by creature_loot_binding, with loot_map_policy saying which maps require every creature to have one.
// On a kill (OrdinaryLootProfile.Roll) each group of the creature's profile drops when a roll 0..9999 is under its
// chance_basis_points; it then picks minimum_rolls..maximum_rolls times, each a weighted choice among its items with a quantity in
// minimum..maximum_quantity. Groups and items in sort_order. The server validates the tables at start-up and refuses to start
// on any error (OrdinaryLootCatalog.Validation); the editor runs the same structural checks (validate()) and does not save
// while one fails. Database-wide, with its own Save / Revert.
class LootEditor {
public:
    void load(const WorldDatabase& database);         // throws std::runtime_error
    void clear();
    bool loaded() const { return loaded_; }
    bool changed() const;
    bool save(WorldDatabase& database, std::string& message);   // false: nothing written, message says why
    void revert();
    // Every error the server's start-up validation would report for the edited tables (structural and item checks; the full
    // coverage check of required maps - mission scenes, control point garrisons - is the server's).
    std::vector<std::string> validate(const WorldDatabase& database) const;
    // The panel body. mapContextId / mapCreatures: the open map and the creatures its spawn pools can spawn (max count > 0);
    // creatureCombo: the workspace's creature picker.
    void draw(EditorContext& ctx, const WorldDatabase& database, int32_t mapContextId, const std::vector<int32_t>& mapCreatures, bool writable,
              const std::function<bool(const char*, int32_t&)>& creatureCombo);
    // The profile bound to (map, creature), "" if none.
    std::string profileFor(int32_t mapContextId, int32_t creatureId) const;

private:
    struct Table {
        DbTable saved;
        DbTable edited;
    };
    std::string itemLabel(EditorContext& ctx, const WorldDatabase& database, int32_t templateId) const;
    void drawProfile(EditorContext& ctx, const WorldDatabase& database, size_t row, int32_t mapContextId, bool writable,
                     const std::function<bool(const char*, int32_t&)>& creatureCombo);
    void drawPolicy(EditorContext& ctx, const WorldDatabase& database, int32_t mapContextId, const std::vector<int32_t>& mapCreatures, bool writable);

    bool loaded_ = false;
    Table profiles_, groups_, items_, bindings_, policies_;
    std::string selected_;                            // profile key
    char filter_[64] = "";
    char newProfileKey_[65] = "";
    int32_t newItemTemplate_ = 0;
    int32_t newBindingCreature_ = 0;
};
