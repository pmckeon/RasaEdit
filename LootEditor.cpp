#include "LootEditor.h"

#include "EditorContext.h"
#include "Lookups.h"

#include "imgui.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

namespace {

int columnOf(const DbTable& t, const char* name) {
    const int c = t.column(name);
    if (c < 0) throw std::runtime_error(t.name + " has no column " + name);
    return c;
}
int64_t intAt(const std::vector<DbValue>& row, const DbTable& t, const char* name) { return row[size_t(t.column(name))].asInt(); }
const std::string& textAt(const std::vector<DbValue>& row, const DbTable& t, const char* name) { return row[size_t(t.column(name))].t; }
void setInt(std::vector<DbValue>& row, const DbTable& t, const char* name, int64_t v) { row[size_t(t.column(name))] = DbValue::integer(v); }
void setText(std::vector<DbValue>& row, const DbTable& t, const char* name, std::string v) { row[size_t(t.column(name))] = DbValue::text(std::move(v)); }
bool sameRows(const DbTable& a, const DbTable& b) { return a.rows == b.rows; }

const char* kindName(int64_t kind) {
    switch (kind) {
        case 1: return "Junk";
        case 2: return "Ammunition";
        case 3: return "Consumable";
        case 4: return "Equipment";
        default: return "?";
    }
}
// Whether a creature from a plain spawn pool needs a loot binding on a map that requires them (OrdinaryLootCoverage.Source):
// players may attack it - TargetCategories.PlayerMayAttack(FromFaction(faction)): Hostile 0 or Neutral 3, an undefined faction
// counting as Hostile - and, outside mission scenarios, kills reward (CreatureGameplayRules.RewardsKills(false, null) = true).
bool needsLootBinding(const Creature* c) {
    if (!c) return true;
    return c->faction == 0 || c->faction == 3 || c->faction < 0 || c->faction > 6;
}

const char* qualityName(int32_t quality) {
    static const char* names[] = {"?", "Mission", "Normal", "Uncommon", "Rare", "Epic", "Legendary", "Junk"};
    return quality >= 1 && quality <= 7 ? names[quality] : "?";
}

// An integer field bound to a row column; true when changed.
bool intField(const char* label, std::vector<DbValue>& row, const DbTable& t, const char* column, float width, int minimum, int maximum) {
    int value = int(intAt(row, t, column));
    ImGui::SetNextItemWidth(width);
    if (!ImGui::InputInt(label, &value, 0)) return false;
    value = std::clamp(value, minimum, maximum);
    setInt(row, t, column, value);
    return true;
}
bool textField(const char* label, std::vector<DbValue>& row, const DbTable& t, const char* column, size_t maxLength, bool multiline) {
    char buffer[2048];
    std::snprintf(buffer, sizeof buffer, "%s", textAt(row, t, column).c_str());
    const size_t size = std::min(sizeof buffer, maxLength + 1);
    const bool changed = multiline ? ImGui::InputTextMultiline(label, buffer, size, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.5f))
                                   : ImGui::InputText(label, buffer, size);
    if (changed) setText(row, t, column, buffer);
    return changed;
}

}  // namespace

void LootEditor::load(const WorldDatabase& database) {
    clear();
    if (!database.hasTable("loot_profile") || !database.hasTable("ordinary_loot_group") || !database.hasTable("loot_item") ||
        !database.hasTable("creature_loot_binding") || !database.hasTable("loot_map_policy"))
        return;                                       // an older world without the ordinary loot tables
    Table* tables[] = {&profiles_, &groups_, &items_, &bindings_, &policies_};
    const char* names[] = {"loot_profile", "ordinary_loot_group", "loot_item", "creature_loot_binding", "loot_map_policy"};
    for (int k = 0; k < 5; ++k) {
        tables[k]->saved = database.loadTable(names[k]);
        tables[k]->edited = tables[k]->saved;
    }
    for (const char* c : {"profile_key", "description", "content_revision", "provenance", "evidence_notes"}) columnOf(profiles_.saved, c);
    for (const char* c : {"profile_key", "group_key", "kind", "chance_basis_points", "minimum_rolls", "maximum_rolls", "sort_order"}) columnOf(groups_.saved, c);
    for (const char* c : {"profile_key", "group_key", "item_template_id", "weight", "minimum_quantity", "maximum_quantity", "sort_order"})
        columnOf(items_.saved, c);
    for (const char* c : {"map_context_id", "creature_id", "profile_key"}) columnOf(bindings_.saved, c);
    for (const char* c : {"map_context_id", "require_profiles"}) columnOf(policies_.saved, c);
    loaded_ = true;
}

void LootEditor::clear() {
    loaded_ = false;
    profiles_ = groups_ = items_ = bindings_ = policies_ = Table{};
    selected_.clear();
}

bool LootEditor::changed() const {
    return loaded_ && (!sameRows(profiles_.saved, profiles_.edited) || !sameRows(groups_.saved, groups_.edited) ||
                       !sameRows(items_.saved, items_.edited) || !sameRows(bindings_.saved, bindings_.edited) ||
                       !sameRows(policies_.saved, policies_.edited));
}

void LootEditor::revert() {
    for (Table* t : {&profiles_, &groups_, &items_, &bindings_, &policies_}) t->edited = t->saved;
}

bool LootEditor::save(WorldDatabase& database, std::string& message) {
    const std::vector<std::string> errors = validate(database);
    if (!errors.empty()) {
        message = "Loot not saved: " + std::to_string(errors.size()) + " problem(s) the server would refuse to start with - first: " + errors.front();
        return false;
    }
    WorldDatabase::SaveSummary summary;
    std::string error;
    if (!database.saveTables({{&profiles_.saved, &profiles_.edited}, {&groups_.saved, &groups_.edited}, {&items_.saved, &items_.edited},
                              {&bindings_.saved, &bindings_.edited}, {&policies_.saved, &policies_.edited}},
                             summary, error)) {
        message = "Saving loot failed, nothing was written: " + error;
        return false;
    }
    const std::string keep = selected_;
    load(database);
    selected_ = keep;
    message = "Loot saved: " + std::to_string(summary.inserted) + " added, " + std::to_string(summary.updated) + " changed, " +
              std::to_string(summary.deleted) + " deleted. Restart the server to apply.";
    return true;
}

std::string LootEditor::profileFor(int32_t mapContextId, int32_t creatureId) const {
    const DbTable& b = bindings_.edited;
    for (const auto& row : b.rows)
        if (intAt(row, b, "map_context_id") == mapContextId && intAt(row, b, "creature_id") == creatureId) return textAt(row, b, "profile_key");
    return "";
}

// Rasa.NET OrdinaryLootCatalog.Validation (structure, items) and OrdinaryLootCoverage (spawn pools of required maps).
std::vector<std::string> LootEditor::validate(const WorldDatabase& database) const {
    std::vector<std::string> errors;
    if (!loaded_) return errors;
    auto error = [&](const std::string& e) { errors.push_back(e); };
    const DbTable &p = profiles_.edited, &g = groups_.edited, &it = items_.edited, &b = bindings_.edited, &m = policies_.edited;
    auto key = [&](const std::string& k, size_t limit, const std::string& label) {
        if (k.empty() || k.size() > limit) error(label + ": key must be 1.." + std::to_string(limit) + " characters.");
    };
    std::set<std::string> profiles;
    for (const auto& row : p.rows) {
        const std::string& k = textAt(row, p, "profile_key");
        key(k, 64, "profile " + k);
        if (!profiles.insert(k).second) error("profile " + k + ": duplicate profile key.");
        if (textAt(row, p, "content_revision").size() > 32) error("profile " + k + ": content revision is longer than 32 characters.");
    }
    std::map<std::pair<std::string, std::string>, int64_t> groupKind;
    std::map<std::string, int> junkGroups;
    std::map<std::string, int64_t> rolls;
    for (const auto& row : g.rows) {
        const std::string &pk = textAt(row, g, "profile_key"), &gk = textAt(row, g, "group_key");
        const std::string label = "profile " + pk + ", group " + gk;
        key(pk, 64, label);
        key(gk, 32, label);
        if (!profiles.count(pk)) error(label + ": missing profile.");
        const int64_t kind = intAt(row, g, "kind"), chance = intAt(row, g, "chance_basis_points");
        const int64_t minimum = intAt(row, g, "minimum_rolls"), maximum = intAt(row, g, "maximum_rolls");
        if (!groupKind.emplace(std::make_pair(pk, gk), kind).second) error(label + ": duplicate group key.");
        if (kind < 1 || kind > 4) error(label + ": undefined kind " + std::to_string(kind) + ".");
        if (chance < 0 || chance > 10000) error(label + ": chance must be in 0..10000.");
        if (minimum < 0 || maximum < minimum || maximum >= INT_MAX) error(label + ": roll bounds must be nonnegative, ordered and RNG-safe.");
        if (kind == 1 && maximum > 1) error(label + ": Junk groups may select at most one type.");
        if (kind == 1) ++junkGroups[pk];
        rolls[pk] += maximum;
    }
    for (const auto& [pk, n] : junkGroups)
        if (n > 1) error("profile " + pk + ": multiple Junk groups bypass the one-type cap.");
    for (const auto& [pk, n] : rolls)
        if (n > 32) error("profile " + pk + ": aggregate maximum rolls exceed the 32-roll safety bound.");
    std::set<std::tuple<std::string, std::string, int64_t>> itemKeys;
    std::map<std::pair<std::string, std::string>, int64_t> weights;
    for (const auto& row : it.rows) {
        const std::string &pk = textAt(row, it, "profile_key"), &gk = textAt(row, it, "group_key");
        const int64_t templateId = intAt(row, it, "item_template_id");
        const std::string label = "profile " + pk + ", group " + gk + ", template " + std::to_string(templateId);
        const auto group = groupKind.find({pk, gk});
        if (group == groupKind.end()) error(label + ": missing group.");
        if (!itemKeys.insert({pk, gk, templateId}).second) error(label + ": duplicate candidate key.");
        if (templateId <= 0) error(label + ": template ID must be positive.");
        const int64_t weight = intAt(row, it, "weight"), minimum = intAt(row, it, "minimum_quantity"), maximum = intAt(row, it, "maximum_quantity");
        if (weight <= 0) error(label + ": weight must be positive.");
        if (minimum <= 0 || maximum < minimum || maximum >= INT_MAX) error(label + ": quantity bounds must be positive, ordered and safe.");
        weights[{pk, gk}] += std::max<int64_t>(weight, 0);
        const ItemTemplateInfo* info = database.itemTemplate(int32_t(templateId));
        if (!info) {
            error(label + ": expected one native template, found 0.");
            continue;
        }
        if (info->qualityId < 1 || info->qualityId > 7) error(label + ": unknown native quality " + std::to_string(info->qualityId) + ".");
        if (info->qualityId == 1) error(label + ": Mission-quality templates are not ordinary loot.");
        if (group != groupKind.end() && (info->qualityId == 7) != (group->second == 1))
            error(label + ": Junk templates must belong only to Junk groups; Junk groups require Junk templates.");
        if (info->classId <= 0) error(label + ": expected one native item-class link, found 0.");
    }
    for (const auto& [k, kind] : groupKind) {
        const auto w = weights.find(k);
        if (w == weights.end()) error("profile " + k.first + ", group " + k.second + ": group has no candidates.");
        else if (w->second > INT_MAX) error("profile " + k.first + ", group " + k.second + ": total weight exceeds the integer RNG range.");
    }
    std::set<int64_t> maps;
    for (const MapInfo& info : database.maps()) maps.insert(info.mapContextId);
    std::set<std::pair<int64_t, int64_t>> bound;
    for (const auto& row : b.rows) {
        const int64_t map = intAt(row, b, "map_context_id"), creature = intAt(row, b, "creature_id");
        const std::string& pk = textAt(row, b, "profile_key");
        const std::string label = "map " + std::to_string(map) + ", creature " + std::to_string(creature) + ", profile " + pk;
        key(pk, 64, label);
        if (!profiles.count(pk)) error(label + ": missing profile.");
        if (map == 0 || !maps.count(map)) error(label + ": missing native map.");
        if (creature == 0 || !database.creature(int32_t(creature))) error(label + ": missing native creature.");
        if (!bound.insert({map, creature}).second) error(label + ": duplicate binding.");
    }
    for (const auto& row : m.rows) {
        const int64_t map = intAt(row, m, "map_context_id");
        if (map == 0 || !maps.count(map)) error("map policy " + std::to_string(map) + ": missing native map.");
        if (intAt(row, m, "require_profiles") == 0) continue;
        for (int32_t creature : database.spawnableCreatures(int32_t(map)))
            if (needsLootBinding(database.creature(creature)) && !bound.count({map, creature}))
                error("map " + std::to_string(map) + " requires loot profiles: creature " + std::to_string(creature) + " (spawn pools) has no binding.");
    }
    return errors;
}

std::string LootEditor::itemLabel(EditorContext& ctx, const WorldDatabase& database, int32_t templateId) const {
    const ItemTemplateInfo* info = database.itemTemplate(templateId);
    if (!info) return "(not in itemtemplate)";
    std::string name;
    if (ctx.gameData) {
        try { name = ctx.gameData->itemName(templateId, info->classId); } catch (const std::exception&) {}
    }
    if (name.empty()) name = ctx.classInfo(info->classId).name;
    return name + "  [" + qualityName(info->qualityId) + "]";
}

void LootEditor::draw(EditorContext& ctx, const WorldDatabase& database, int32_t mapContextId, const std::vector<int32_t>& mapCreatures, bool writable,
                      const std::function<bool(const char*, int32_t&)>& creatureCombo) {
    if (!loaded_) {
        ImGui::TextDisabled("This database has no ordinary loot tables (loot_profile, ordinary_loot_group, loot_item, ...).");
        return;
    }
    const bool dirty = changed();
    const std::vector<std::string> errors = validate(database);
    if (writable) {
        if (!dirty || !errors.empty()) ImGui::BeginDisabled();
        if (ImGui::Button("Save loot")) {
            std::string message;
            const bool ok = save(const_cast<WorldDatabase&>(database), message);
            ctx.setStatus(message, !ok);
        }
        if (!dirty || !errors.empty()) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!dirty) ImGui::BeginDisabled();
        if (ImGui::Button("Revert")) revert();
        if (!dirty) ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (errors.empty()) ImGui::TextDisabled("%s", dirty ? "Unsaved changes; the server's checks pass." : "The server's checks pass.");
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%zu problem(s): the server would refuse to start", errors.size());
        if (ImGui::BeginItemTooltip()) {
            for (size_t k = 0; k < errors.size() && k < 30; ++k) ImGui::BulletText("%s", errors[k].c_str());
            if (errors.size() > 30) ImGui::Text("... and %zu more", errors.size() - 30);
            ImGui::EndTooltip();
        }
    }
    if (!errors.empty() && ImGui::TreeNode("Problems")) {
        for (const std::string& e : errors) ImGui::BulletText("%s", e.c_str());
        ImGui::TreePop();
    }

    drawPolicy(ctx, database, mapContextId, mapCreatures, writable);

    // Profile list.
    ImGui::SeparatorText("Profiles (loot_profile)");
    DbTable& p = profiles_.edited;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##lootfilter", "filter profiles", filter_, sizeof filter_);
    if (writable) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::InputTextWithHint("##newprofile", "new profile key", newProfileKey_, sizeof newProfileKey_);
        ImGui::SameLine();
        const std::string key = newProfileKey_;
        bool exists = false;
        for (const auto& row : p.rows) exists = exists || textAt(row, p, "profile_key") == key;
        if (key.empty() || exists) ImGui::BeginDisabled();
        if (ImGui::Button("Add profile")) {
            std::vector<DbValue> row(p.columns.size(), DbValue::text(""));
            setText(row, p, "profile_key", key);
            setText(row, p, "content_revision", "custom");
            p.rows.push_back(row);
            selected_ = key;
            newProfileKey_[0] = 0;
        }
        if (key.empty() || exists) ImGui::EndDisabled();
    }
    const std::string needle = tr::toLowerAscii(filter_);
    if (ImGui::BeginListBox("##profiles", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 7))) {
        for (const auto& row : p.rows) {
            const std::string& key = textAt(row, p, "profile_key");
            const std::string label = key + "  -  " + textAt(row, p, "description");
            if (!needle.empty() && tr::toLowerAscii(label).find(needle) == std::string::npos) continue;
            if (ImGui::Selectable(label.c_str(), key == selected_)) selected_ = key;
        }
        ImGui::EndListBox();
    }
    for (size_t k = 0; k < p.rows.size(); ++k)
        if (textAt(p.rows[k], p, "profile_key") == selected_) {
            drawProfile(ctx, database, k, mapContextId, writable, creatureCombo);
            break;
        }
}

// The open map's policy and which of its spawnable creatures have a binding.
void LootEditor::drawPolicy(EditorContext& ctx, const WorldDatabase& database, int32_t mapContextId, const std::vector<int32_t>& mapCreatures,
                            bool writable) {
    if (mapContextId < 0) return;
    ImGui::SeparatorText("This map (loot_map_policy)");
    DbTable& m = policies_.edited;
    std::vector<DbValue>* policy = nullptr;
    for (auto& row : m.rows)
        if (intAt(row, m, "map_context_id") == mapContextId) policy = &row;
    bool required = policy && intAt(*policy, m, "require_profiles") != 0;
    if (!writable) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Every creature needs a loot profile", &required)) {
        if (!policy) {
            std::vector<DbValue> row(m.columns.size(), DbValue::integer(0));
            setInt(row, m, "map_context_id", mapContextId);
            m.rows.push_back(row);
            policy = &m.rows.back();
        }
        setInt(*policy, m, "require_profiles", required ? 1 : 0);
    }
    if (!writable) ImGui::EndDisabled();
    ImGui::SetItemTooltip("loot_map_policy.require_profiles: the server refuses to start if a creature this map can spawn has no binding\n"
                          "(and drops nothing for unbound creatures on other maps).");
    int unbound = 0;
    for (int32_t creature : mapCreatures)
        unbound += needsLootBinding(database.creature(creature)) && profileFor(mapContextId, creature).empty() ? 1 : 0;
    const std::string header = std::to_string(mapCreatures.size()) + " creature(s) spawn here, " + std::to_string(unbound) +
                               " attackable without a profile###lootcoverage";
    if (ImGui::TreeNode(header.c_str())) {
        for (int32_t creature : mapCreatures) {
            const Creature* c = database.creature(creature);
            const std::string profile = profileFor(mapContextId, creature);
            const bool needs = needsLootBinding(c);
            const std::string what = !profile.empty() ? profile : needs ? "(none)" : "(none - not attackable, needs none)";
            if (needs && profile.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.4f, 1.0f));
            ImGui::BulletText("%d  %s  ->  %s", creature, c ? c->comment.c_str() : "?", what.c_str());
            if (needs && profile.empty()) ImGui::PopStyleColor();
            if (!profile.empty() && ImGui::IsItemClicked()) selected_ = profile;
        }
        ImGui::TreePop();
    }
    (void)ctx;
}

void LootEditor::drawProfile(EditorContext& ctx, const WorldDatabase& database, size_t profileRow, int32_t mapContextId, bool writable,
                             const std::function<bool(const char*, int32_t&)>& creatureCombo) {
    DbTable& p = profiles_.edited;
    DbTable& g = groups_.edited;
    DbTable& it = items_.edited;
    DbTable& b = bindings_.edited;
    const std::string key = selected_;
    ImGui::PushID(key.c_str());
    ImGui::SeparatorText(("Profile " + key).c_str());
    if (!writable) ImGui::BeginDisabled();
    {
        std::vector<DbValue>& row = p.rows[profileRow];
        ImGui::SetNextItemWidth(-FLT_MIN);
        textField("##description", row, p, "description", 1000, false);
        ImGui::SetItemTooltip("description");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        textField("Content revision", row, p, "content_revision", 32, false);
        if (ImGui::TreeNode("Provenance and evidence notes")) {
            textField("##provenance", row, p, "provenance", 2000, true);
            textField("##evidence", row, p, "evidence_notes", 2000, true);
            ImGui::TreePop();
        }
    }
    int bindingCount = 0;
    for (const auto& row : b.rows) bindingCount += textAt(row, b, "profile_key") == key ? 1 : 0;
    if (writable) {
        if (bindingCount > 0) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Delete profile")) {
            auto ofProfile = [&](DbTable& t) {
                t.rows.erase(std::remove_if(t.rows.begin(), t.rows.end(), [&](const std::vector<DbValue>& r) { return textAt(r, t, "profile_key") == key; }),
                             t.rows.end());
            };
            ofProfile(it);
            ofProfile(g);
            ofProfile(p);
            selected_.clear();
            if (bindingCount > 0) ImGui::EndDisabled();
            if (!writable) ImGui::EndDisabled();
            ImGui::PopID();
            return;
        }
        if (bindingCount > 0) ImGui::EndDisabled();
        if (bindingCount > 0) ImGui::SetItemTooltip("Remove its %d binding(s) first.", bindingCount);
    }

    // Groups and their items, in sort order.
    std::vector<size_t> groupRows;
    for (size_t k = 0; k < g.rows.size(); ++k)
        if (textAt(g.rows[k], g, "profile_key") == key) groupRows.push_back(k);
    std::stable_sort(groupRows.begin(), groupRows.end(), [&](size_t a, size_t c) { return intAt(g.rows[a], g, "sort_order") < intAt(g.rows[c], g, "sort_order"); });
    ImGui::SeparatorText("Groups (ordinary_loot_group) and items (loot_item)");
    ImGui::TextDisabled("Per kill each group drops with its chance, then picks min..max times, each a weighted choice of one item.");
    const float w = ImGui::GetFontSize() * 4;
    std::string removeGroup;
    for (size_t gi : groupRows) {
        std::vector<DbValue>& grow = g.rows[gi];
        const std::string groupKey = textAt(grow, g, "group_key");
        ImGui::PushID(groupKey.c_str());
        const double chance = double(intAt(grow, g, "chance_basis_points")) / 100.0;
        const std::string title = groupKey + "  (" + kindName(intAt(grow, g, "kind")) + ", " + std::to_string(chance).substr(0, std::to_string(chance).find('.') + 3) +
                                  " %, " + std::to_string(intAt(grow, g, "minimum_rolls")) + ".." + std::to_string(intAt(grow, g, "maximum_rolls")) +
                                  " picks)###group";
        const bool open = ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (open) {
            int kind = int(intAt(grow, g, "kind"));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::Combo("Kind", &kind, "?\0Junk\0Ammunition\0Consumable\0Equipment\0")) setInt(grow, g, "kind", std::max(kind, 1));
            ImGui::SameLine();
            float percent = float(intAt(grow, g, "chance_basis_points")) / 100.0f;
            ImGui::SetNextItemWidth(w * 1.4f);
            if (ImGui::InputFloat("Chance %", &percent, 0, 0, "%.2f")) setInt(grow, g, "chance_basis_points", int64_t(std::clamp(percent, 0.0f, 100.0f) * 100.0f + 0.5f));
            ImGui::SetItemTooltip("chance_basis_points / 100: the group drops when a roll 0..9999 is under it.");
            intField("Min picks", grow, g, "minimum_rolls", w, 0, 32);
            ImGui::SameLine();
            intField("Max picks", grow, g, "maximum_rolls", w, 0, 32);
            ImGui::SameLine();
            intField("Order", grow, g, "sort_order", w, INT_MIN, INT_MAX);
            // Items of the group.
            std::vector<size_t> itemRows;
            int64_t total = 0;
            for (size_t k = 0; k < it.rows.size(); ++k)
                if (textAt(it.rows[k], it, "profile_key") == key && textAt(it.rows[k], it, "group_key") == groupKey) {
                    itemRows.push_back(k);
                    total += std::max<int64_t>(intAt(it.rows[k], it, "weight"), 0);
                }
            std::stable_sort(itemRows.begin(), itemRows.end(), [&](size_t a, size_t c) { return intAt(it.rows[a], it, "sort_order") < intAt(it.rows[c], it, "sort_order"); });
            int removeItem = -1;
            if (ImGui::BeginTable("items", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Template");
                ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Weight");
                ImGui::TableSetupColumn("Per pick");
                ImGui::TableSetupColumn("Quantity");
                ImGui::TableSetupColumn("Order");
                ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
                for (size_t ii : itemRows) {
                    std::vector<DbValue>& irow = it.rows[ii];
                    const int32_t templateId = int32_t(intAt(irow, it, "item_template_id"));
                    ImGui::PushID(int(ii));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", templateId);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(itemLabel(ctx, database, templateId).c_str());
                    ImGui::TableNextColumn();
                    intField("##weight", irow, it, "weight", w, 1, INT_MAX);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f %%", total > 0 ? 100.0 * double(intAt(irow, it, "weight")) / double(total) : 0.0);
                    ImGui::TableNextColumn();
                    intField("##qmin", irow, it, "minimum_quantity", w * 0.8f, 1, INT_MAX - 1);
                    ImGui::SameLine();
                    intField("##qmax", irow, it, "maximum_quantity", w * 0.8f, 1, INT_MAX - 1);
                    ImGui::TableNextColumn();
                    intField("##order", irow, it, "sort_order", w, INT_MIN, INT_MAX);
                    ImGui::TableNextColumn();
                    if (writable && ImGui::SmallButton("Remove")) removeItem = int(ii);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (removeItem >= 0) it.rows.erase(it.rows.begin() + removeItem);
            if (writable) {
                ImGui::SetNextItemWidth(w * 3.0f);
                lookupCombo("##newitem", newItemTemplate_, lookup::itemTemplate(ctx, database, "(choose an item)"));
                ImGui::SameLine();
                const ItemTemplateInfo* candidate = database.itemTemplate(newItemTemplate_);
                bool already = false;
                for (size_t ii : itemRows) already = already || intAt(it.rows[ii], it, "item_template_id") == newItemTemplate_;
                if (!candidate || already) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Add item")) {
                    std::vector<DbValue> row(it.columns.size(), DbValue::integer(1));
                    setText(row, it, "profile_key", key);
                    setText(row, it, "group_key", groupKey);
                    setInt(row, it, "item_template_id", newItemTemplate_);
                    setInt(row, it, "sort_order", 10 * int64_t(itemRows.size() + 1));
                    it.rows.push_back(row);
                }
                if (!candidate || already) ImGui::EndDisabled();
                if (ImGui::SmallButton("Remove group")) removeGroup = groupKey;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (!removeGroup.empty()) {
        it.rows.erase(std::remove_if(it.rows.begin(), it.rows.end(), [&](const std::vector<DbValue>& r) {
                          return textAt(r, it, "profile_key") == key && textAt(r, it, "group_key") == removeGroup;
                      }), it.rows.end());
        g.rows.erase(std::remove_if(g.rows.begin(), g.rows.end(), [&](const std::vector<DbValue>& r) {
                         return textAt(r, g, "profile_key") == key && textAt(r, g, "group_key") == removeGroup;
                     }), g.rows.end());
    }
    if (writable) {
        static char newGroup[33] = "";
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        ImGui::InputTextWithHint("##newgroup", "new group key", newGroup, sizeof newGroup);
        ImGui::SameLine();
        bool exists = false;
        for (size_t gi : groupRows) exists = exists || textAt(g.rows[gi], g, "group_key") == newGroup;
        if (!newGroup[0] || exists) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Add group")) {
            std::vector<DbValue> row(g.columns.size(), DbValue::integer(0));
            setText(row, g, "profile_key", key);
            setText(row, g, "group_key", newGroup);
            setInt(row, g, "kind", 3);
            setInt(row, g, "chance_basis_points", 1000);
            setInt(row, g, "minimum_rolls", 1);
            setInt(row, g, "maximum_rolls", 1);
            setInt(row, g, "sort_order", 10 * int64_t(groupRows.size() + 1));
            g.rows.push_back(row);
            newGroup[0] = 0;
        }
        if (!newGroup[0] || exists) ImGui::EndDisabled();
    }

    // Bindings to this profile.
    ImGui::SeparatorText("Creatures dropping it (creature_loot_binding)");
    int removeBinding = -1;
    for (size_t k = 0; k < b.rows.size(); ++k) {
        const auto& row = b.rows[k];
        if (textAt(row, b, "profile_key") != key) continue;
        const int64_t map = intAt(row, b, "map_context_id"), creature = intAt(row, b, "creature_id");
        std::string mapName = std::to_string(map);
        for (const MapInfo& info : database.maps())
            if (info.mapContextId == map) mapName = info.name;
        const Creature* c = database.creature(int32_t(creature));
        ImGui::PushID(int(k));
        if (writable && ImGui::SmallButton("Remove")) removeBinding = int(k);
        ImGui::SameLine();
        ImGui::Text("%s: creature %lld  %s", mapName.c_str(), static_cast<long long>(creature), c ? c->comment.c_str() : "(not in the creature table)");
        ImGui::PopID();
    }
    if (removeBinding >= 0) b.rows.erase(b.rows.begin() + removeBinding);
    if (writable && mapContextId >= 0) {
        creatureCombo("##bindcreature", newBindingCreature_);
        ImGui::SameLine();
        const std::string existing = profileFor(mapContextId, newBindingCreature_);
        if (newBindingCreature_ <= 0 || existing == key) ImGui::BeginDisabled();
        if (ImGui::SmallButton(existing.empty() ? "Bind on this map" : "Rebind on this map")) {
            bool replaced = false;
            for (auto& row : b.rows)
                if (intAt(row, b, "map_context_id") == mapContextId && intAt(row, b, "creature_id") == newBindingCreature_) {
                    setText(row, b, "profile_key", key);
                    replaced = true;
                }
            if (!replaced) {
                std::vector<DbValue> row(b.columns.size(), DbValue::integer(0));
                setInt(row, b, "map_context_id", mapContextId);
                setInt(row, b, "creature_id", newBindingCreature_);
                setText(row, b, "profile_key", key);
                b.rows.push_back(row);
            }
        }
        if (newBindingCreature_ <= 0 || existing == key) ImGui::EndDisabled();
        if (!existing.empty() && existing != key) ImGui::SetItemTooltip("Now bound to %s on this map.", existing.c_str());
    }
    if (!writable) ImGui::EndDisabled();
    ImGui::PopID();
}
