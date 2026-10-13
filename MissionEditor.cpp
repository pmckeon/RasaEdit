#include "MissionEditor.h"

#include "EditorContext.h"
#include "gamedata.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

// ------------------------------------------------------------------------------------------
// Table specs
// ------------------------------------------------------------------------------------------
enum class FieldKind : uint8_t { Int, Real, Text, LongText, Bool, Enum, Creature, ItemTemplate, EntityClass, MissionText, MapContext };

using EnumList = std::vector<std::pair<int64_t, const char*>>;

struct MissionEditor::FieldSpec {
    const char* column;
    const char* label;
    FieldKind kind;
    const EnumList* values = nullptr;
    std::vector<int64_t> kinds;                       // the `kind` values that use it; empty = every row
};

struct MissionEditor::TableSpec {
    const char* table;
    const char* title;
    const char* parent;                               // parent table (nullptr: the root, mission_content_definition)
    const char* ownKey;                               // key column numbered within the parent (nullptr: one row per parent)
    const char* kindColumn;                           // the column that picks which fields apply (nullptr: none)
    std::vector<FieldSpec> fields;
    bool readOnly = false;
};

namespace {

// Rasa.NET enums (Rasa.DBL/Structures/World/Mission*Entry.cs, Rasa.Missions/Definitions, Rasa.Data).
const EnumList kRequirement = {{1, "required"}, {2, "optional"}};
const EnumList kAbandonment = {{1, "allowed"}, {2, "prohibited"}};
const EnumList kObjectiveState = {{0, "not assigned"}, {1, "incomplete"}, {2, "completed"}, {3, "failed"}, {4, "inactive"}};
const EnumList kChannel = {{1, "NPC"}, {2, "radio"}, {3, "mixed (NPC + radio)"}};
const EnumList kRepeat = {{0, "once"}, {1, "immediate"}, {2, "cooldown"}, {3, "daily"}};
const EnumList kPrerequisite = {{1, "mission completed"}, {2, "mission accepted"}, {3, "player level at least"}, {4, "player flag value"}};
const EnumList kTrigger = {{1, "conversation"}, {2, "progress event"}, {3, "objective state"}, {4, "area entered"}, {5, "timer elapsed"}};
const EnumList kEvent = {{0, "waypoint acquired"},  {1, "logos acquired"},        {2, "creature killed"},  {3, "mission completed"},
                         {4, "item acquired"},      {5, "item consumed"},         {6, "interaction used"}, {7, "area entered"},
                         {8, "item equipped"},      {9, "ability hit"},           {10, "scenario event"},  {11, "deadline elapsed"},
                         {12, "objective state reached"}, {13, "object hit"},     {14, "creature flag killed"}, {15, "creature class killed"}};
const EnumList kAction = {{1, "reveal objective"},  {2, "activate objective"},    {3, "complete objective"},   {4, "grant reward"},
                          {5, "start scenario"},    {6, "activate spawn group"},  {7, "show indicator"},       {8, "set player flag"},
                          {9, "show ambient conversation"}, {10, "issue mission item"}, {11, "consume mission item"},
                          {12, "remove mission items"}, {13, "fail related mission"}};
const EnumList kRewardItem = {{1, "fixed"}, {2, "selectable"}};
const EnumList kSpawnPolicy = {{0, "ordinary respawn"}, {1, "scenario controlled"}};
const EnumList kStartPolicy = {{1, "automatic"}, {2, "player triggered"}};
const EnumList kAreaShape = {{1, "sphere"}, {2, "cylinder"}, {3, "box"}};
const EnumList kStep = {{1, "spawn group"},        {2, "despawn group"},       {3, "enable interaction"},   {4, "disable interaction"},
                        {5, "reveal objective"},   {6, "activate objective"},  {7, "complete objective"},   {8, "fail objective"},
                        {9, "start deadline"},     {10, "cancel deadline"},    {11, "grant reward package"}, {12, "grant skill / ability"},
                        {13, "play tutorial"},     {14, "schedule scenario"},  {15, "reset attempt"},       {16, "emit scenario event"},
                        {17, "transfer player"},   {18, "set qualification"},  {19, "set account skip entitlement"},
                        {20, "spawn dynamic object"}, {21, "despawn dynamic object"}, {22, "escort spawn group"}, {23, "satisfy deadline"}};

using F = MissionEditor::FieldSpec;
using K = FieldKind;

// Which columns each kind uses: the trigger kinds as Rasa.NET documents them (MissionTriggerKind), the action and
// scenario step kinds from the columns the shipped rows fill (kinds no row uses show every field).
const std::vector<MissionEditor::TableSpec> kSpecs = {
    {"mission_content_definition", "Mission", nullptr, nullptr, nullptr,
     {F{"enabled", "Enabled", K::Bool}, F{"requirement", "Requirement", K::Enum, &kRequirement},
      F{"abandonment_policy", "Abandonment", K::Enum, &kAbandonment}, F{"client_name_text_id", "Name text", K::MissionText},
      F{"giver_id", "Giver", K::Creature}, F{"receiver_id", "Receiver", K::Creature}, F{"level", "Level", K::Int},
      F{"group_type", "Group type", K::Int}, F{"category_id", "Category", K::Int}, F{"shareable", "Shareable", K::Bool},
      F{"radio_completeable", "Radio completable", K::Bool}, F{"comment", "Comment", K::Text}}},
    {"mission_channel_policy", "Channels", "mission_content_definition", nullptr, nullptr,
     {F{"acceptance_channel", "Acceptance", K::Enum, &kChannel}, F{"completion_channel", "Completion", K::Enum, &kChannel},
      F{"radio_sources", "Radio sources (JSON)", K::LongText}}},
    {"mission_repeat_policy", "Repeat policy", "mission_content_definition", nullptr, "repeat_kind",
     {F{"repeat_kind", "Repeat", K::Enum, &kRepeat}, F{"cooldown_seconds", "Cooldown (s)", K::Int, nullptr, {2}},
      F{"reset_second_utc", "Reset at (s of UTC day)", K::Int, nullptr, {3}}}},
    {"mission_prerequisite", "Prerequisites", "mission_content_definition", "prerequisite_id", "kind",
     {F{"kind", "Kind", K::Enum, &kPrerequisite}, F{"requirement", "Requirement", K::Enum, &kRequirement},
      F{"required_mission_id", "Mission", K::Int, nullptr, {1, 2}}, F{"required_mission_state", "Mission state", K::Int, nullptr, {1, 2}},
      F{"required_level", "Level", K::Int, nullptr, {3}}, F{"player_flag_id", "Player flag", K::Int, nullptr, {4}},
      F{"player_flag_value", "Flag value", K::Int, nullptr, {4}}, F{"comment", "Comment", K::Text}}},
    {"mission_objective_definition", "Objectives", "mission_content_definition", "objective_id", nullptr,
     {F{"requirement", "Requirement", K::Enum, &kRequirement}, F{"client_name_text_id", "Name text", K::MissionText},
      F{"client_body_text_id", "Body text", K::MissionText}, F{"client_counter_0_text_id", "Counter 0 text", K::MissionText},
      F{"client_counter_1_text_id", "Counter 1 text", K::MissionText}, F{"client_counter_2_text_id", "Counter 2 text", K::MissionText},
      F{"ordinal", "Ordinal", K::Int}, F{"initial_state", "Initial state", K::Enum, &kObjectiveState},
      F{"is_required", "Required", K::Bool}, F{"comment", "Comment", K::Text}}},
    {"mission_objective_transition", "Transitions", "mission_objective_definition", "transition_id", nullptr,
     {F{"requirement", "Requirement", K::Enum, &kRequirement}, F{"sequence", "Sequence", K::Int},
      F{"from_state", "From state", K::Enum, &kObjectiveState}, F{"to_state", "To state", K::Enum, &kObjectiveState},
      F{"comment", "Comment", K::Text}}},
    {"mission_trigger", "Triggers", "mission_objective_transition", "trigger_id", "kind",
     {F{"kind", "Kind", K::Enum, &kTrigger}, F{"requirement", "Requirement", K::Enum, &kRequirement}, F{"sequence", "Sequence", K::Int},
      F{"npc_package_id", "NPC package", K::Int, nullptr, {1}}, F{"player_flag_id", "Player flag", K::Int, nullptr, {1}},
      F{"event_kind", "Event", K::Enum, &kEvent, {2}}, F{"subject_id", "Subject id", K::Int, nullptr, {2}},
      F{"counter_id", "Counter", K::Int, nullptr, {2}}, F{"initial_value", "Initial value", K::Int, nullptr, {2}},
      F{"target_value", "Target value", K::Int, nullptr, {2}}, F{"source_spawn_resolved", "Source spawn resolved", K::Int, nullptr, {2}},
      F{"related_objective_id", "Related objective", K::Int, nullptr, {3}},
      F{"related_state", "Related state", K::Enum, &kObjectiveState, {3}}, F{"area_id", "Area", K::Int, nullptr, {4}},
      F{"duration_seconds", "Duration (s)", K::Int, nullptr, {5}}, F{"comment", "Comment", K::Text}}},
    {"mission_action", "Actions", "mission_objective_transition", "action_id", "kind",
     {F{"kind", "Kind", K::Enum, &kAction}, F{"requirement", "Requirement", K::Enum, &kRequirement}, F{"sequence", "Sequence", K::Int},
      F{"target_objective_id", "Target objective", K::Int, nullptr, {1, 2, 3}},
      F{"objective_state", "Objective state", K::Enum, &kObjectiveState, {2, 3}}, F{"reward_id", "Reward", K::Int, nullptr, {4}},
      F{"scenario_id", "Scenario", K::Int, nullptr, {5}}, F{"spawn_group_id", "Spawn group", K::Int, nullptr, {6}},
      F{"indicator_id", "Indicator", K::Int, nullptr, {7}}, F{"player_flag_id", "Player flag", K::Int, nullptr, {8, 9}},
      F{"player_flag_value", "Flag value", K::Int, nullptr, {8}}, F{"npc_package_id", "NPC package", K::Int, nullptr, {9}},
      F{"item_intent", "Item intent (JSON)", K::LongText, nullptr, {10, 11, 12, 13}}, F{"comment", "Comment", K::Text}}},
    {"mission_indicator", "Indicators", "mission_objective_definition", "indicator_id", nullptr,
     {F{"requirement", "Requirement", K::Enum, &kRequirement}, F{"pos_x", "x", K::Real}, F{"pos_y", "y", K::Real}, F{"pos_z", "z", K::Real},
      F{"radius", "Radius", K::Real}, F{"show_3d_effect", "3D effect", K::Bool}, F{"comment", "Comment", K::Text}}},
    {"mission_reward_definition", "Rewards", "mission_content_definition", "reward_id", nullptr,
     {F{"requirement", "Requirement", K::Enum, &kRequirement}, F{"experience", "Experience", K::Int}, F{"credits", "Credits", K::Int},
      F{"prestige", "Prestige", K::Int}, F{"selection_count", "Selection count", K::Int}, F{"comment", "Comment", K::Text}}},
    {"mission_reward_item", "Items", "mission_reward_definition", "item_id", nullptr,
     {F{"kind", "Kind", K::Enum, &kRewardItem}, F{"item_template_id", "Item template", K::ItemTemplate}, F{"quantity", "Quantity", K::Int}}},
    {"mission_spawn_group", "Spawn groups", "mission_content_definition", "spawn_group_id", nullptr,
     {F{"requirement", "Requirement", K::Enum, &kRequirement}, F{"map_context_id", "Map", K::MapContext}, F{"area_id", "Area", K::Int},
      F{"enabled", "Enabled", K::Bool}, F{"respawn_seconds", "Respawn (s)", K::Int}, F{"spawn_policy", "Policy", K::Enum, &kSpawnPolicy},
      F{"comment", "Comment", K::Text}}},
    {"mission_spawn", "Spawns", "mission_spawn_group", "spawn_id", nullptr,
     {F{"creature_id", "Creature", K::Creature}, F{"pos_x", "x", K::Real}, F{"pos_y", "y", K::Real}, F{"pos_z", "z", K::Real},
      F{"rotation", "Rotation (rad)", K::Real}, F{"quantity", "Quantity", K::Int}}},
    {"mission_scenario", "Scenarios", "mission_content_definition", "scenario_id", nullptr,
     {F{"requirement", "Requirement", K::Enum, &kRequirement}, F{"start_policy", "Start", K::Enum, &kStartPolicy}, F{"name", "Name", K::Text},
      F{"comment", "Comment", K::Text}}},
    {"mission_scenario_step", "Steps", "mission_scenario", "step_id", "kind",
     {F{"kind", "Kind", K::Enum, &kStep}, F{"requirement", "Requirement", K::Enum, &kRequirement}, F{"sequence", "Sequence", K::Int},
      F{"target_objective_id", "Target objective", K::Int, nullptr, {5, 6, 7}}, F{"reward_id", "Reward", K::Int, nullptr, {11}},
      F{"spawn_group_id", "Spawn group", K::Int, nullptr, {1, 2, 22}}, F{"spawn_id", "Spawn", K::Int, nullptr, {1, 2, 22}},
      F{"dynamic_object_key", "Dynamic object key", K::Text, nullptr, {20}}, F{"entity_class_id", "Entity class", K::EntityClass, nullptr, {3, 4, 20}},
      F{"target_scenario_id", "Target scenario", K::Int, nullptr, {14, 15}}, F{"delay_milliseconds", "Delay (ms)", K::Int, nullptr, {14, 20}},
      F{"skill_id", "Skill", K::Int, nullptr, {12}}, F{"ability_id", "Ability", K::Int, nullptr, {12}},
      F{"skill_level", "Skill level", K::Int, nullptr, {12}}, F{"ability_slot", "Ability slot", K::Int, nullptr, {12}},
      F{"tutorial_id", "Tutorial", K::Int, nullptr, {13}}, F{"audio_set_id", "Audio set", K::Int, nullptr, {13}},
      F{"attempt_key", "Attempt key", K::Text, nullptr, {15}}, F{"scenario_event_id", "Scenario event", K::Int, nullptr, {16}},
      F{"map_context_id", "Map", K::MapContext, nullptr, {17, 20}}, F{"pos_x", "x", K::Real, nullptr, {17, 20}},
      F{"pos_y", "y", K::Real, nullptr, {17, 20}}, F{"pos_z", "z", K::Real, nullptr, {17, 20}},
      F{"orientation", "Orientation", K::Real, nullptr, {17, 20}}, F{"initial_interaction_enabled", "Interactable", K::Bool, nullptr, {20}},
      F{"qualification_key", "Qualification", K::Int, nullptr, {18}}, F{"qualification_value", "Qualification value", K::Int, nullptr, {18}},
      F{"account_skip_entitlement", "Account skip entitlement", K::Int, nullptr, {19}}, F{"comment", "Comment", K::Text}}},
    {"mission_area", "Areas (edit in Server objects)", "mission_content_definition", "area_id", nullptr,
     {F{"map_context_id", "Map", K::MapContext}, F{"shape", "Shape", K::Enum, &kAreaShape}, F{"pos_x", "x", K::Real}, F{"pos_y", "y", K::Real},
      F{"pos_z", "z", K::Real}, F{"radius", "Radius", K::Real}, F{"extent_x", "Extent x", K::Real}, F{"extent_z", "Extent z", K::Real},
      F{"comment", "Comment", K::Text}},
     true},
    {"mission_evidence", "Evidence (provenance, read-only)", "mission_content_definition", "evidence_id", nullptr,
     {F{"confidence", "Confidence", K::Int}, F{"source_kind", "Source kind", K::Int}, F{"source_uri", "Source", K::Text},
      F{"local_client_path", "Client path", K::Text}, F{"reconstruction_note", "Note", K::LongText}},
     true},
    {"mission_scene_binding", "Scene binding (read-only)", "mission_content_definition", nullptr, nullptr,
     {F{"script_key", "Script", K::Text}, F{"state_version", "State version", K::Int}, F{"bindings", "Bindings (JSON)", K::LongText}},
     true},
};

const char* enumName(const EnumList* values, int64_t v) {
    if (values)
        for (const auto& e : *values)
            if (e.first == v) return e.second;
    return "?";
}

bool isTextType(const std::string& declared) {
    std::string d;
    for (char ch : declared) d += char(std::tolower(static_cast<unsigned char>(ch)));
    return d.find("char") != std::string::npos || d.find("text") != std::string::npos || d.find("clob") != std::string::npos;
}

bool isRealType(const std::string& declared) {
    std::string d;
    for (char ch : declared) d += char(std::tolower(static_cast<unsigned char>(ch)));
    return d.find("real") != std::string::npos || d.find("floa") != std::string::npos || d.find("doub") != std::string::npos;
}

}  // namespace

// ------------------------------------------------------------------------------------------
// Loading and saving
// ------------------------------------------------------------------------------------------
void MissionEditor::load(const WorldDatabase& database) {
    std::vector<Table> tables;
    for (const TableSpec& spec : kSpecs) {
        Table t;
        t.spec = &spec;
        t.saved = database.loadTable(spec.table);
        t.edited = t.saved;
        tables.push_back(std::move(t));
    }
    tables_ = std::move(tables);
}

void MissionEditor::clear() {
    tables_.clear();
    selectedMission_ = -1;
}

bool MissionEditor::changed() const {
    for (const Table& t : tables_)
        if (!t.spec->readOnly && t.saved.rows != t.edited.rows) return true;
    return false;
}

void MissionEditor::revert() {
    for (Table& t : tables_) t.edited = t.saved;
}

bool MissionEditor::save(WorldDatabase& database, std::string& message) {
    std::vector<std::pair<const DbTable*, const DbTable*>> pairs;
    for (const Table& t : tables_)
        if (!t.spec->readOnly) pairs.emplace_back(&t.saved, &t.edited);
    WorldDatabase::SaveSummary summary;
    std::string error;
    if (!database.saveTables(pairs, summary, error)) {
        message = "Saving missions failed, nothing was written: " + error;
        return false;
    }
    for (Table& t : tables_)
        if (!t.spec->readOnly) t.saved = t.edited;
    message = "Missions saved: " + std::to_string(summary.inserted) + " rows added, " + std::to_string(summary.updated) + " changed, " +
              std::to_string(summary.deleted) + " deleted. Restart the server to apply.";
    return true;
}

MissionEditor::Table* MissionEditor::table(const char* name) {
    for (Table& t : tables_)
        if (std::strcmp(t.spec->table, name) == 0) return &t;
    return nullptr;
}

// A child row belongs to a parent row when every parent key column has the same value in both.
bool MissionEditor::rowUnder(const Table& child, const std::vector<DbValue>& row, const DbTable& parentTable, const std::vector<DbValue>& parentRow) const {
    for (int k : parentTable.key) {
        const int c = child.edited.column(parentTable.columns[size_t(k)]);
        if (c < 0 || row[size_t(c)] != parentRow[size_t(k)]) return false;
    }
    return true;
}

std::vector<DbValue> MissionEditor::newRow(const Table& t, const DbTable& parentTable, const std::vector<DbValue>* parentRow) const {
    const DbTable& d = t.edited;
    std::vector<DbValue> row(d.columns.size());
    for (size_t k = 0; k < d.columns.size(); ++k) {
        if (d.nullable[k]) continue;
        if (isTextType(d.declaredTypes[k])) row[k] = DbValue::text("");
        else if (isRealType(d.declaredTypes[k])) row[k] = DbValue::real(0);
        else row[k] = DbValue::integer(0);
    }
    auto set = [&](const char* column, DbValue v) {
        const int c = d.column(column);
        if (c >= 0) row[size_t(c)] = std::move(v);
    };
    set("requirement", DbValue::integer(1));
    set("enabled", DbValue::integer(1));
    set("quantity", DbValue::integer(1));
    set("comment", DbValue::text("new"));
    if (t.spec->kindColumn) {
        for (const FieldSpec& f : t.spec->fields)
            if (std::strcmp(f.column, t.spec->kindColumn) == 0 && f.values && !f.values->empty()) set(t.spec->kindColumn, DbValue::integer(f.values->front().first));
    }
    if (parentRow)
        for (int k : parentTable.key) set(parentTable.columns[size_t(k)].c_str(), (*parentRow)[size_t(k)]);
    if (t.spec->ownKey) {
        const int own = d.column(t.spec->ownKey);
        int64_t next = 1;
        for (const auto& other : d.rows)
            if (!parentRow || rowUnder(t, other, parentTable, *parentRow)) next = std::max(next, other[size_t(own)].asInt() + 1);
        row[size_t(own)] = DbValue::integer(next);
    }
    return row;
}

void MissionEditor::removeWithChildren(const char* tableName, const std::vector<DbValue>& row) {
    Table* t = table(tableName);
    if (!t) return;
    for (const TableSpec& spec : kSpecs) {
        if (!spec.parent || std::strcmp(spec.parent, tableName) != 0) continue;
        Table* child = table(spec.table);
        if (!child || spec.readOnly) continue;
        std::vector<std::vector<DbValue>> children;
        for (const auto& r : child->edited.rows)
            if (rowUnder(*child, r, t->edited, row)) children.push_back(r);
        for (const auto& r : children) removeWithChildren(spec.table, r);
    }
    t->edited.rows.erase(std::remove(t->edited.rows.begin(), t->edited.rows.end(), row), t->edited.rows.end());
}

// ------------------------------------------------------------------------------------------
// Drawing
// ------------------------------------------------------------------------------------------
std::string MissionEditor::missionLabel(EditorContext& ctx, const std::vector<DbValue>& row) const {
    const DbTable& d = tables_.front().edited;
    const int64_t id = row[size_t(d.column("mission_id"))].asInt();
    const std::string revision = row[size_t(d.column("content_revision"))].t;
    const int64_t textId = row[size_t(d.column("client_name_text_id"))].asInt();
    std::string name;
    if (ctx.gameData && textId > 0) {
        try { name = ctx.gameData->missionText(int32_t(textId)); } catch (const std::exception&) {}
    }
    const std::string comment = row[size_t(d.column("comment"))].t;
    if (name.empty()) name = comment;
    char text[256];
    std::snprintf(text, sizeof text, "%lld [%s]  %s%s  (lvl %lld)", static_cast<long long>(id), revision.c_str(), name.c_str(),
                  row[size_t(d.column("enabled"))].asInt() ? "" : "  (disabled)", static_cast<long long>(row[size_t(d.column("level"))].asInt()));
    return text;
}

bool MissionEditor::drawRowFields(EditorContext& ctx, const WorldDatabase& database, Table& t, std::vector<DbValue>& row, bool writable) {
    const DbTable& d = t.edited;
    const float width = ImGui::GetFontSize() * 11;
    bool changed = false;
    int64_t kind = -1;
    std::set<int64_t> mappedKinds;
    if (t.spec->kindColumn) {
        const int c = d.column(t.spec->kindColumn);
        if (c >= 0) kind = row[size_t(c)].asInt();
        for (const FieldSpec& f : t.spec->fields) mappedKinds.insert(f.kinds.begin(), f.kinds.end());
    }
    const bool editable = writable && !t.spec->readOnly;
    if (!editable) ImGui::BeginDisabled();
    for (const FieldSpec& f : t.spec->fields) {
        const int c = d.column(f.column);
        if (c < 0) continue;
        if (!f.kinds.empty() && mappedKinds.count(kind) && std::find(f.kinds.begin(), f.kinds.end(), kind) == f.kinds.end()) continue;
        DbValue& v = row[size_t(c)];
        ImGui::PushID(f.column);
        const bool nullable = d.nullable[size_t(c)];
        if (nullable) {
            bool set = !v.isNull();
            if (ImGui::Checkbox("##set", &set)) {
                v = set ? (f.kind == FieldKind::Real ? DbValue::real(0)
                           : (f.kind == FieldKind::Text || f.kind == FieldKind::LongText) ? DbValue::text("")
                                                                                           : DbValue::integer(f.values && !f.values->empty() ? f.values->front().first : 0))
                        : DbValue{};
                changed = true;
            }
            ImGui::SetItemTooltip("Set (unchecked = NULL)");
            ImGui::SameLine();
            if (!set) {
                ImGui::TextDisabled("%s: (null)", f.label);
                ImGui::PopID();
                continue;
            }
        }
        switch (f.kind) {
            case FieldKind::Bool: {
                bool b = v.asInt() != 0;
                if (ImGui::Checkbox(f.label, &b)) { v = DbValue::integer(b ? 1 : 0); changed = true; }
                break;
            }
            case FieldKind::Enum: {
                const int64_t current = v.asInt();
                ImGui::SetNextItemWidth(width);
                if (ImGui::BeginCombo(f.label, (std::to_string(current) + " " + enumName(f.values, current)).c_str())) {
                    for (const auto& e : *f.values)
                        if (ImGui::Selectable((std::to_string(e.first) + " " + e.second).c_str(), e.first == current)) {
                            v = DbValue::integer(e.first);
                            changed = true;
                            // A new kind: clear the nullable kind-specific columns it does not use.
                            if (t.spec->kindColumn && std::strcmp(f.column, t.spec->kindColumn) == 0)
                                for (const FieldSpec& other : t.spec->fields) {
                                    const int oc = d.column(other.column);
                                    if (oc >= 0 && !other.kinds.empty() && d.nullable[size_t(oc)] &&
                                        std::find(other.kinds.begin(), other.kinds.end(), e.first) == other.kinds.end())
                                        row[size_t(oc)] = DbValue{};
                                }
                        }
                    ImGui::EndCombo();
                }
                break;
            }
            case FieldKind::Real: {
                double r = v.asReal();
                ImGui::SetNextItemWidth(width);
                if (ImGui::InputDouble(f.label, &r, 0, 0, "%.3f")) { v = DbValue::real(r); changed = true; }
                break;
            }
            case FieldKind::Text:
            case FieldKind::LongText: {
                std::vector<char> buffer(std::max<size_t>(v.t.size() + 1024, 256));
                std::memcpy(buffer.data(), v.t.c_str(), v.t.size() + 1);
                bool edited;
                if (f.kind == FieldKind::LongText) {
                    ImGui::TextUnformatted(f.label);
                    edited = ImGui::InputTextMultiline("##text", buffer.data(), buffer.size(), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));
                } else {
                    ImGui::SetNextItemWidth(width * 1.6f);
                    edited = ImGui::InputText(f.label, buffer.data(), buffer.size());
                }
                if (edited) { v = DbValue::text(buffer.data()); changed = true; }
                break;
            }
            default: {
                int64_t i = v.asInt();
                ImGui::SetNextItemWidth(width);
                if (ImGui::InputScalar(f.label, ImGuiDataType_S64, &i)) { v = DbValue::integer(i); changed = true; }
                // What the number refers to.
                std::string hint;
                if (f.kind == FieldKind::Creature) {
                    const Creature* cr = database.creature(int32_t(i));
                    hint = cr ? cr->comment : i ? "(not in creature)" : "";
                } else if (f.kind == FieldKind::ItemTemplate) {
                    const ItemTemplateInfo* info = database.itemTemplate(int32_t(i));
                    hint = info ? ctx.classInfo(info->classId).name : "(not in itemtemplate)";
                } else if (f.kind == FieldKind::EntityClass) {
                    hint = ctx.classInfo(int32_t(i)).name;
                } else if (f.kind == FieldKind::MissionText && ctx.gameData && i > 0) {
                    try { hint = ctx.gameData->missionText(int32_t(i)); } catch (const std::exception&) {}
                    if (hint.empty()) hint = "(not in missiontextlanguage)";
                } else if (f.kind == FieldKind::MapContext) {
                    for (const MapInfo& m : database.maps())
                        if (m.mapContextId == i) hint = m.name;
                } else if (f.kind == FieldKind::Enum) {
                    hint = enumName(f.values, i);
                }
                if (!hint.empty()) {
                    ImGui::SameLine();
                    if (hint.size() > 70) ImGui::TextDisabled("%s...", hint.substr(0, 70).c_str());
                    else ImGui::TextDisabled("%s", hint.c_str());
                    if (hint.size() > 70) ImGui::SetItemTooltip("%s", hint.c_str());
                }
                break;
            }
        }
        ImGui::PopID();
    }
    if (!editable) ImGui::EndDisabled();
    return changed;
}

void MissionEditor::drawChildren(EditorContext& ctx, const WorldDatabase& database, const TableSpec& parentSpec, const std::vector<DbValue>& parentRow,
                                 int depth, bool writable) {
    Table* parentTable = table(parentSpec.table);
    if (!parentTable || depth > 6) return;
    for (const TableSpec& spec : kSpecs) {
        if (!spec.parent || std::strcmp(spec.parent, parentSpec.table) != 0) continue;
        Table* t = table(spec.table);
        if (!t) continue;
        std::vector<size_t> rows;
        for (size_t k = 0; k < t->edited.rows.size(); ++k)
            if (rowUnder(*t, t->edited.rows[k], parentTable->edited, parentRow)) rows.push_back(k);
        ImGui::PushID(spec.table);
        char header[128];
        std::snprintf(header, sizeof header, "%s (%zu)###%s", spec.title, rows.size(), spec.table);
        const bool open = ImGui::TreeNodeEx(header, rows.empty() ? ImGuiTreeNodeFlags_Leaf : 0);
        if (writable && !spec.readOnly && (spec.ownKey || rows.empty())) {
            ImGui::SameLine();
            if (ImGui::SmallButton("+")) adds_.push_back(PendingAdd{spec.table, newRow(*t, parentTable->edited, &parentRow)});
            ImGui::SetItemTooltip("Add a row");
        }
        if (open) {
            const int own = spec.ownKey ? t->edited.column(spec.ownKey) : -1;
            const int comment = t->edited.column("comment");
            const int kind = spec.kindColumn ? t->edited.column(spec.kindColumn) : -1;
            for (size_t k : rows) {
                std::vector<DbValue>& row = t->edited.rows[k];
                std::string label = own >= 0 ? std::to_string(row[size_t(own)].asInt()) : std::string(spec.title);
                if (kind >= 0) {
                    const FieldSpec* kf = nullptr;
                    for (const FieldSpec& f : spec.fields)
                        if (std::strcmp(f.column, spec.kindColumn) == 0) kf = &f;
                    label += "  " + std::string(enumName(kf ? kf->values : nullptr, row[size_t(kind)].asInt()));
                }
                if (comment >= 0 && !row[size_t(comment)].t.empty()) label += "  - " + row[size_t(comment)].t;
                ImGui::PushID(int(k));
                const bool rowOpen = ImGui::TreeNode((label + "###row").c_str());
                if (writable && !spec.readOnly) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) removes_.push_back(PendingRemove{spec.table, row});
                    ImGui::SetItemTooltip("Remove (with its children)");
                }
                if (rowOpen) {
                    drawRowFields(ctx, database, *t, row, writable);
                    drawChildren(ctx, database, spec, row, depth + 1, writable);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void MissionEditor::draw(EditorContext& ctx, WorldDatabase& database, int32_t mapContextId, const std::vector<int32_t>& mapCreatures, bool writable) {
    if (tables_.empty()) {
        ImGui::TextDisabled("No mission tables.");
        return;
    }
    Table& root = tables_.front();
    DbTable& missions = root.edited;
    const int idColumn = missions.column("mission_id"), revisionColumn = missions.column("content_revision");
    const int giverColumn = missions.column("giver_id"), receiverColumn = missions.column("receiver_id");
    if (writable) {
        if (ImGui::Button("New mission")) {
            std::vector<DbValue> row = newRow(root, missions, nullptr);
            int64_t next = 1;
            for (const auto& r : missions.rows) next = std::max(next, r[size_t(idColumn)].asInt() + 1);
            row[size_t(idColumn)] = DbValue::integer(next);
            row[size_t(revisionColumn)] = DbValue::text("custom");
            missions.rows.push_back(row);
            selectedMission_ = next;
            selectedRevision_ = "custom";
        }
        ImGui::SameLine();
        const bool changedNow = changed();
        if (!changedNow) ImGui::BeginDisabled();
        if (ImGui::Button("Save missions")) {
            std::string message;
            const bool ok = save(database, message);
            ctx.setStatus(message, !ok);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) revert();
        if (!changedNow) ImGui::EndDisabled();
        ImGui::SameLine();
    }
    ImGui::Checkbox("On this map", &mapOnly_);
    ImGui::SetItemTooltip("Missions whose giver or receiver spawns here, or with a spawn group or area on this map.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##missionfilter", "filter by id, name or comment", filter_, sizeof filter_);
    const std::string needle = tr::toLowerAscii(filter_);
    std::set<std::pair<int64_t, std::string>> onMap;
    if (mapOnly_) {
        const std::set<int32_t> creatures(mapCreatures.begin(), mapCreatures.end());
        for (const auto& r : missions.rows)
            if ((!r[size_t(giverColumn)].isNull() && creatures.count(int32_t(r[size_t(giverColumn)].asInt()))) ||
                (!r[size_t(receiverColumn)].isNull() && creatures.count(int32_t(r[size_t(receiverColumn)].asInt()))))
                onMap.insert({r[size_t(idColumn)].asInt(), r[size_t(revisionColumn)].t});
        for (const char* name : {"mission_spawn_group", "mission_area"}) {
            const Table* t = table(name);
            if (!t) continue;
            const int map = t->edited.column("map_context_id"), id = t->edited.column("mission_id"), rev = t->edited.column("content_revision");
            for (const auto& r : t->edited.rows)
                if (r[size_t(map)].asInt() == mapContextId) onMap.insert({r[size_t(id)].asInt(), r[size_t(rev)].t});
        }
    }
    if (ImGui::BeginChild("missionlist", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.3f), ImGuiChildFlags_Borders)) {
        for (size_t k = 0; k < missions.rows.size(); ++k) {
            const auto& r = missions.rows[k];
            const int64_t id = r[size_t(idColumn)].asInt();
            const std::string& revision = r[size_t(revisionColumn)].t;
            if (mapOnly_ && !onMap.count({id, revision})) continue;
            const std::string label = missionLabel(ctx, r);
            if (!needle.empty() && tr::toLowerAscii(label).find(needle) == std::string::npos) continue;
            ImGui::PushID(int(k));
            if (ImGui::Selectable(label.c_str(), id == selectedMission_ && revision == selectedRevision_)) {
                selectedMission_ = id;
                selectedRevision_ = revision;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    std::vector<DbValue>* selected = nullptr;
    for (auto& r : missions.rows)
        if (r[size_t(idColumn)].asInt() == selectedMission_ && r[size_t(revisionColumn)].t == selectedRevision_) selected = &r;
    if (!selected) {
        ImGui::TextDisabled("Select a mission.");
        return;
    }
    if (ImGui::BeginChild("missioneditor")) {
        ImGui::TextWrapped("%s", missionLabel(ctx, *selected).c_str());
        bool isNew = true;
        for (const auto& r : root.saved.rows)
            if (r[size_t(idColumn)] == (*selected)[size_t(idColumn)] && r[size_t(revisionColumn)] == (*selected)[size_t(revisionColumn)]) isNew = false;
        if (writable && isNew) {
            // A mission not saved yet can still change its id and revision (they key every child row).
            int64_t id = (*selected)[size_t(idColumn)].asInt();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            ImGui::InputScalar("Mission id (new)", ImGuiDataType_S64, &id);
            char revision[33];
            std::snprintf(revision, sizeof revision, "%s", (*selected)[size_t(revisionColumn)].t.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            ImGui::InputText("Content revision (new)", revision, sizeof revision);
            if (id != selectedMission_ || revision != selectedRevision_) {
                bool taken = false;
                for (const auto& r : missions.rows)
                    taken = taken || (&r != selected && r[size_t(idColumn)].asInt() == id && r[size_t(revisionColumn)].t == revision);
                bool hasChildren = false;
                for (size_t k = 1; k < tables_.size(); ++k)
                    for (const auto& r : tables_[k].edited.rows) hasChildren = hasChildren || rowUnder(tables_[k], r, missions, *selected);
                if (!taken && !hasChildren && id > 0) {
                    (*selected)[size_t(idColumn)] = DbValue::integer(id);
                    (*selected)[size_t(revisionColumn)] = DbValue::text(revision);
                    selectedMission_ = id;
                    selectedRevision_ = revision;
                } else if (hasChildren) {
                    ImGui::TextDisabled("(id / revision can only change before rows are added under the mission)");
                }
            }
        }
        if (writable) {
            if (ImGui::SmallButton("Delete mission...")) ImGui::OpenPopup("deletemission");
            if (ImGui::BeginPopup("deletemission")) {
                ImGui::Text("Delete this mission and every row under it (not saved until Save missions)?");
                if (ImGui::Button("Delete")) {
                    removes_.push_back(PendingRemove{root.spec->table, *selected});
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        ImGui::Separator();
        drawRowFields(ctx, database, root, *selected, writable);
        ImGui::Separator();
        drawChildren(ctx, database, *root.spec, *selected, 0, writable);
    }
    ImGui::EndChild();
    // Structural edits after drawing (they invalidate row references).
    for (const PendingRemove& r : removes_) removeWithChildren(r.table.c_str(), r.row);
    for (PendingAdd& a : adds_)
        if (Table* t = table(a.table.c_str())) t->edited.rows.push_back(std::move(a.row));
    removes_.clear();
    adds_.clear();
}
