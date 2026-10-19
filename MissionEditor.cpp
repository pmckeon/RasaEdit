#include "MissionEditor.h"

#include "EditorContext.h"
#include "Lookups.h"
#include "TextWorkspace.h"
#include "gamedata.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
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
// Rasa.Missions/Definitions/MissionState.cs (prerequisite required_mission_state).
const lookup::EnumValues kMissionState = {{0, "active"}, {1, "success"}, {2, "failed"}, {3, "not assigned"}, {4, "completed"}};
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
const EnumList kAreaShape = {{1, "sphere"}, {2, "cylinder"}, {3, "box"}, {4, "map"}};
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
    references_ = MissionReferences::load(database);
    placeholderTextId_ = -1;
    teleporters_.clear();
    if (database.hasTable("teleporter")) {
        const DbTable tp = database.loadTable("teleporter");
        const int id = tp.column("id"), description = tp.column("description"), map = tp.column("map_context_id");
        for (const auto& r : tp.rows) {
            if (id < 0) break;
            std::string label = description >= 0 && !r[size_t(description)].t.empty() ? r[size_t(description)].t : std::string("(no description)");
            if (map >= 0) {
                const int64_t m = r[size_t(map)].asInt();
                std::string mapName = std::to_string(m);
                for (const MapInfo& info : database.maps())
                    if (info.mapContextId == m) mapName = info.name;
                label += "  (" + mapName + ")";
            }
            teleporters_.emplace_back(r[size_t(id)].asInt(), label);
        }
    }
    savedValidation_ = validateTables(false);
    validation_ = savedValidation_;
    validationDirty_ = false;
}

LookupSource MissionEditor::missionSource(EditorContext& ctx) {
    LookupSource s;
    s.name = [this, &ctx](int64_t id) -> std::string {
        if (tables_.empty()) return "";
        const DbTable& d = tables_.front().edited;
        for (const auto& r : d.rows)
            if (r[size_t(d.column("mission_id"))].asInt() == id) return missionLabel(ctx, r);
        return "";
    };
    s.fill = [this, &ctx](LookupList& out) {
        if (tables_.empty()) return;
        const DbTable& d = tables_.front().edited;
        std::set<int64_t> seen;
        for (const auto& r : d.rows) {
            const int64_t id = r[size_t(d.column("mission_id"))].asInt();
            if (seen.insert(id).second) out.push_back(LookupChoice{id, missionLabel(ctx, r)});
        }
        std::sort(out.begin(), out.end(), [](const LookupChoice& a, const LookupChoice& b) { return a.id < b.id; });
    };
    return s;
}

void MissionEditor::selectMission(int64_t missionId) {
    if (tables_.empty()) return;
    const DbTable& d = tables_.front().edited;
    const int id = d.column("mission_id"), revision = d.column("content_revision"), enabled = d.column("enabled");
    for (const auto& r : d.rows) {
        if (r[size_t(id)].asInt() != missionId) continue;
        if (selectedMission_ != missionId || r[size_t(enabled)].asInt() != 0) {
            selectedMission_ = missionId;
            selectedRevision_ = r[size_t(revision)].t;
        }
    }
}

MissionValidationResult MissionEditor::validateTables(bool edited) const {
    std::map<std::string, const DbTable*> byName;
    for (const Table& t : tables_) byName[t.spec->table] = edited ? &t.edited : &t.saved;
    return validateMissionContent(byName, references_);
}

const MissionValidationResult& MissionEditor::validation() {
    if (validationDirty_) {
        validation_ = validateTables(true);
        validationDirty_ = false;
    }
    return validation_;
}

void MissionEditor::clear() {
    tables_.clear();
    teleporters_.clear();
    selectedMission_ = -1;
}

bool MissionEditor::changed() const {
    for (const Table& t : tables_)
        if (!t.spec->readOnly && t.saved.rows != t.edited.rows) return true;
    return false;
}

void MissionEditor::revert() {
    for (Table& t : tables_) t.edited = t.saved;
    validationDirty_ = true;
}

bool MissionEditor::save(WorldDatabase& database, std::string& message) {
    // The server's start-up checks (MissionValidation.h): a change that would keep the server from starting, or from reporting
    // ready (a required mission with a problem), is refused; problems the saved content already had are not held against it.
    const MissionValidationResult& v = validation();
    for (const MissionDiagnostic& d : v.diagnostics)
        if (d.startup && !savedValidation_.startupFails) {
            message = "Not saved: the Game server would not start - " + d.message;
            return false;
        }
    if (v.blocksReadiness && !savedValidation_.blocksReadiness) {
        const DbTable& missions = tables_.front().edited;
        for (const MissionDiagnostic& d : v.diagnostics) {
            if (d.startup || d.missionId < 0) continue;
            bool required = false;
            for (const auto& r : missions.rows)
                if (r[size_t(missions.column("mission_id"))].asInt() == d.missionId && r[size_t(missions.column("enabled"))].asInt() != 0 &&
                    r[size_t(missions.column("requirement"))].asInt() == 1)
                    required = true;
            if (!required) continue;
            message = "Not saved: required mission " + std::to_string(d.missionId) + " would keep the server from reporting ready - " + d.code + ": " + d.message;
            return false;
        }
    }
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
    savedValidation_ = validation_;
    message = "Missions saved: " + std::to_string(summary.inserted) + " rows added, " + std::to_string(summary.updated) + " changed, " +
              std::to_string(summary.deleted) + " deleted. Restart the server to apply.";
    if (!validation_.inactiveMissions.empty())
        message += " The server will leave out " + std::to_string(validation_.inactiveMissions.size()) + " mission(s) (see the problems list).";
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
    if (placeholderTextId_ > 0) {                         // ID_DEFAULT_TEXT until a text is chosen
        if (std::strcmp(t.spec->table, "mission_content_definition") == 0) set("client_name_text_id", DbValue::integer(placeholderTextId_));
        if (std::strcmp(t.spec->table, "mission_objective_definition") == 0) {
            set("client_name_text_id", DbValue::integer(placeholderTextId_));
            set("client_body_text_id", DbValue::integer(placeholderTextId_));
        }
    }
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

// Columns that name another row of the same mission (Rasa.NET's mission content keys) or a mission.
namespace {
// A dropdown over (id, label) pairs worked out by the caller.
LookupSource pairsSource(std::shared_ptr<std::vector<std::pair<int64_t, std::string>>> choices) {
    LookupSource source;
    source.name = [choices](int64_t id) {
        for (const auto& c : *choices)
            if (c.first == id) return c.second;
        return std::string();
    };
    source.fill = [choices](LookupList& out) {
        for (const auto& c : *choices) out.push_back(LookupChoice{c.first, c.second});
    };
    return source;
}
}  // namespace

MissionEditor::MissionReference MissionEditor::missionReference(const char* column) {
    static const struct { const char* column; MissionReference ref; } kRefs[] = {
        {"target_objective_id", {"mission_objective_definition", "objective_id", "objective"}},
        {"related_objective_id", {"mission_objective_definition", "objective_id", "objective"}},
        {"reward_id", {"mission_reward_definition", "reward_id", "reward"}},
        {"spawn_group_id", {"mission_spawn_group", "spawn_group_id", "spawn group"}},
        {"scenario_id", {"mission_scenario", "scenario_id", "scenario"}},
        {"target_scenario_id", {"mission_scenario", "scenario_id", "scenario"}},
        {"area_id", {"mission_area", "area_id", "area"}},
        {"indicator_id", {"mission_indicator", "indicator_id", "indicator"}},
        {"spawn_id", {"mission_spawn", "spawn_id", "spawn"}},
        {"required_mission_id", {"mission_content_definition", "mission_id", "mission"}}};
    for (const auto& r : kRefs)
        if (std::strcmp(r.column, column) == 0) return r.ref;
    return MissionReference{};
}

std::vector<std::pair<int64_t, std::string>> MissionEditor::referenceChoices(EditorContext& ctx, const DbTable& table, const std::vector<DbValue>& row,
                                                                            const MissionReference& ref) {
    std::vector<std::pair<int64_t, std::string>> out;
    const Table* target = this->table(ref.table);
    if (!target) return out;
    const DbTable& t = target->edited;
    const int key = t.column(ref.key);
    if (key < 0) return out;
    const auto value = [](const DbTable& tt, const std::vector<DbValue>& r, const char* column) -> const DbValue* {
        const int c = tt.column(column);
        return c < 0 ? nullptr : &r[size_t(c)];
    };
    const DbValue* mission = value(table, row, "mission_id");
    const DbValue* revision = value(table, row, "content_revision");
    const DbValue* group = value(table, row, "spawn_group_id");
    const bool missions = std::strcmp(ref.table, "mission_content_definition") == 0;
    std::set<int64_t> seen;
    for (const auto& r : t.rows) {
        if (!missions) {
            const DbValue* m = value(t, r, "mission_id");
            const DbValue* rv = value(t, r, "content_revision");
            if (!mission || !revision || !m || !rv || *m != *mission || *rv != *revision) continue;
            if (std::strcmp(ref.table, "mission_spawn") == 0) {
                const DbValue* g = value(t, r, "spawn_group_id");
                if (!group || !g || *g != *group) continue;
            }
        }
        const int64_t id = r[size_t(key)].asInt();
        if (!seen.insert(id).second) continue;
        std::string label = std::to_string(id);
        if (missions) {
            label = missionLabel(ctx, r);
        } else {
            for (const char* column : {"name", "comment"})
                if (const DbValue* text = value(t, r, column); text && !text->t.empty()) {
                    label += "  " + text->t;
                    break;
                }
        }
        out.emplace_back(id, label);
    }
    return out;
}

std::vector<std::pair<int64_t, std::string>> MissionEditor::playerFlagChoices() const {
    // flag -> its uses ("set by 1390 (Persist the reported Milpas outcome)", "read by 1992 (...)")
    std::map<int64_t, std::vector<std::string>> uses;
    uses[0x80000001ll].push_back("server flag: Bootcamp complete");
    static const struct { const char* table; const char* verb; } kTables[] = {
        {"mission_prerequisite", "required by"}, {"mission_trigger", "read by"}, {"mission_action", "set by"}};
    for (const auto& k : kTables) {
        const Table* t = const_cast<MissionEditor*>(this)->table(k.table);
        if (!t) continue;
        const DbTable& d = t->edited;
        const int flag = d.column("player_flag_id"), mission = d.column("mission_id"), comment = d.column("comment"), kind = d.column("kind");
        if (flag < 0 || mission < 0) continue;
        for (const auto& r : d.rows) {
            if (r[size_t(flag)].isNull()) continue;
            // mission_action kind 8 sets the flag; kind 9 (ambient conversation) names one it reads.
            const bool reads = std::strcmp(k.table, "mission_action") == 0 && kind >= 0 && r[size_t(kind)].asInt() == 9;
            std::string use = std::string(reads ? "read by" : k.verb) + " " + std::to_string(r[size_t(mission)].asInt());
            if (comment >= 0 && !r[size_t(comment)].t.empty()) use += " (" + r[size_t(comment)].t + ")";
            std::vector<std::string>& list = uses[r[size_t(flag)].asInt()];
            if (std::find(list.begin(), list.end(), use) == list.end()) list.push_back(use);
        }
    }
    std::vector<std::pair<int64_t, std::string>> out;
    for (const auto& [id, list] : uses) {
        std::string label;
        for (size_t i = 0; i < list.size() && i < 3; ++i) label += (i ? "; " : "") + list[i];
        if (list.size() > 3) label += "; +" + std::to_string(list.size() - 3) + " more";
        out.emplace_back(id, label);
    }
    return out;
}

namespace {
int64_t findPlaceholderTextId(tr::GameData& data) {
    std::set<int64_t> referenced;
    try {
        if (const tr::py::Object* t = data.global("generated.client.missionconversation", "lookup"))
            for (const auto& e : t->entries) referenced.insert(tr::py::asInt(tr::py::item(e.second.get(), 0), -1));
        if (const tr::py::Object* t = data.global("generated.client.objectiveconversation", "lookup"))
            for (const auto& e : t->entries) referenced.insert(tr::py::asInt(tr::py::item(e.second.get(), 0), -1));
        if (const tr::py::Object* t = data.global("generated.client.missionobjective", "lookup"))
            for (const auto& e : t->entries) {
                const tr::py::Object* texts = tr::py::item(e.second.get(), 0);
                for (size_t k = 0; k < tr::py::length(texts); ++k)
                    if (!tr::py::isNone(tr::py::item(texts, k))) referenced.insert(tr::py::asInt(tr::py::item(texts, k)));
            }
        std::vector<const tr::py::Object*> lookups;
        const std::string prefix = "generated.client.language.", suffix = ".missiontextlanguage";
        for (const std::string& m : data.moduleNames())
            if (m.compare(0, prefix.size(), prefix) == 0 && m.size() > suffix.size() && m.compare(m.size() - suffix.size(), suffix.size(), suffix) == 0)
                if (const tr::py::Object* l = data.global(m, "lookup")) lookups.push_back(l);
        const tr::py::Object* english = data.global("generated.client.language.english.missiontextlanguage", "lookup");
        if (!english) return 0;
        std::set<int64_t> ids;
        for (const auto& e : english->entries) ids.insert(tr::py::asInt(tr::py::item(e.first.get(), 0)));
        for (int64_t id : ids) {
            if (referenced.count(id)) continue;
            bool empty = true;
            for (const tr::py::Object* l : lookups)
                for (int64_t gender : {1, 2})
                    if (const tr::py::Object* v = tr::py::dictGetTuple(l, {id, gender}); v && !tr::py::asString(tr::py::item(v, 0)).empty()) empty = false;
            if (empty) return id;
        }
    } catch (const std::exception&) {
    }
    return 0;
}
}  // namespace

namespace {
const char* kConversationModule = "generated.client.missionconversation";
const char* kObjectiveModule = "generated.client.missionobjective";
const char* kObjectiveConversationModule = "generated.client.objectiveconversation";
const char* kMissionTextModule = "generated.client.language.english.missiontextlanguage";

// The client's current tables and texts: through the Text workspace (pending edits included) when there is one.
const tr::py::Object* clientLookup(EditorContext& ctx, const char* module) {
    if (ctx.textWorkspace) return ctx.textWorkspace->currentLookup(module);
    try {
        return ctx.gameData ? ctx.gameData->global(module, "lookup") : nullptr;
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::string clientMissionText(EditorContext& ctx, int64_t id) {
    if (ctx.textWorkspace) return ctx.textWorkspace->missionText(id);
    return ctx.gameData ? ctx.gameData->missionText(int32_t(id)) : std::string();
}

tr::py::Ref noneValue() { return std::make_shared<tr::py::Object>(); }
tr::py::Ref textValue(const std::string& text) { return tr::py::makeList({tr::py::makeText(text)}); }
}  // namespace

std::optional<int64_t> MissionEditor::clientTextId(EditorContext& ctx, const DbTable& table, const std::vector<DbValue>& row,
                                                   const char* column) const {
    if (!ctx.gameData) return std::nullopt;
    const auto value = [&](const char* c) {
        const int i = table.column(c);
        return i < 0 ? int64_t(-1) : row[size_t(i)].asInt();
    };
    const int64_t mission = value("mission_id");
    try {
        if (table.name == "mission_content_definition" && std::strcmp(column, "client_name_text_id") == 0) {
            const tr::py::Object* t = tr::py::dictGetTuple(clientLookup(ctx, kConversationModule), {mission, 1});
            if (t) return tr::py::asInt(tr::py::item(t, 0));
        }
        if (table.name == "mission_objective_definition") {
            static const char* kColumns[] = {"client_name_text_id", "client_body_text_id", "client_counter_0_text_id", "client_counter_1_text_id",
                                             "client_counter_2_text_id"};
            const tr::py::Object* t = tr::py::dictGetTuple(clientLookup(ctx, kObjectiveModule), {mission, value("objective_id")});
            const tr::py::Object* texts = tr::py::item(t, 0);
            for (size_t k = 0; k < 5; ++k)
                if (std::strcmp(column, kColumns[k]) == 0 && tr::py::item(texts, k) && !tr::py::isNone(tr::py::item(texts, k)))
                    return tr::py::asInt(tr::py::item(texts, k));
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

namespace {
std::string oneLine(std::string text, size_t limit = 120) {
    for (char& c : text)
        if (c == '\n' || c == '\r') c = ' ';
    if (text.size() > limit) text = text.substr(0, limit) + "...";
    return text;
}
}  // namespace

bool MissionEditor::missionTextField(EditorContext& ctx, const char* label, int64_t& value, std::optional<int64_t> client, bool showInGame) {
    bool changed = false;
    // The placeholder stops being one once its text is filled in (in the Text workspace): work it out again.
    if (placeholderTextId_ > 0 && ctx.gameData && !ctx.gameData->missionText(int32_t(placeholderTextId_)).empty()) placeholderTextId_ = -1;
    const bool placeholder = placeholderTextId_ > 0 && value == placeholderTextId_;
    const std::string current = value == 0 ? std::string("(none)") : "#" + std::to_string(value) + "  " + oneLine(clientMissionText(ctx, value));
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    const bool browse = ImGui::SmallButton("Browse...");
    ImGui::SetItemTooltip("Search and pick a text of the client's missiontextlanguage");
    ImGui::SameLine();
    if (placeholder) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "ID_DEFAULT_TEXT (placeholder - choose a text)");
        ImGui::SetItemTooltip("Mission text #%lld: empty in every language and named by no client mission table, the default of new missions "
                              "and objectives until a text is chosen.", static_cast<long long>(value));
    } else ImGui::TextDisabled("%s", current.c_str());
    if (browse) {
        ImGui::OpenPopup("Mission text browser");
        textBrowseFilter_[0] = 0;
        textBrowsePick_ = value;
        textBrowseAll_.clear();
        textBrowseShownFor_ = "\x01";
        if (ctx.gameData) {
            const tr::py::Object* lookup = clientLookup(ctx, kMissionTextModule);
            if (lookup)
                for (const auto& e : lookup->entries)
                    if (tr::py::asInt(tr::py::item(e.first.get(), 1)) == 1)
                        textBrowseAll_.emplace_back(tr::py::asInt(tr::py::item(e.first.get(), 0)), tr::py::asString(tr::py::item(e.second.get(), 0)));
            std::sort(textBrowseAll_.begin(), textBrowseAll_.end());
        }
    }
    // What the player sees: the client's own reference for this mission / objective.
    if (!showInGame) {
    } else if (client) {
        ImGui::Indent();
        if (*client == value) ImGui::TextDisabled("In game: %s", oneLine(clientMissionText(ctx, *client), 200).c_str());
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "In game: #%lld %s", static_cast<long long>(*client),
                               oneLine(clientMissionText(ctx, *client), 160).c_str());
            ImGui::SetItemTooltip("The client shows its own text for this mission, not the id stored here (the server never sends text ids).");
            ImGui::SameLine();
            if (ImGui::SmallButton("Use the client's")) {
                value = *client;
                changed = true;
            }
        }
        ImGui::Unindent();
    } else if (value != 0) {
        ImGui::Indent();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "In game: no text - the client has none for this mission / objective id");
        ImGui::SetItemTooltip("The client looks texts up by mission and objective id (missionconversation / missionobjective in game.zip).");
        ImGui::Unindent();
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 48, ImGui::GetFontSize() * 34), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Mission text browser", nullptr)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##find", "Search the text or an id", textBrowseFilter_, sizeof textBrowseFilter_);
        if (textBrowseShownFor_ != textBrowseFilter_) {
            textBrowseShownFor_ = textBrowseFilter_;
            std::string f = textBrowseFilter_;
            for (char& c : f) c = char(std::tolower(static_cast<unsigned char>(c)));
            textBrowseShown_.clear();
            for (const auto& e : textBrowseAll_) {
                bool keep = f.empty() || std::to_string(e.first) == f;
                if (!keep) {
                    std::string t = e.second;
                    for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
                    keep = t.find(f) != std::string::npos;
                }
                if (keep) textBrowseShown_.push_back(e);
            }
        }
        ImGui::TextDisabled("%zu of %zu texts", textBrowseShown_.size(), textBrowseAll_.size());
        const float previewHeight = ImGui::GetTextLineHeightWithSpacing() * 7;
        if (ImGui::BeginChild("list", ImVec2(0, -previewHeight - ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders)) {
            ImGuiListClipper clipper;
            clipper.Begin(int(textBrowseShown_.size()));
            while (clipper.Step())
                for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                    const auto& e = textBrowseShown_[size_t(k)];
                    const std::string line = std::to_string(e.first) + "  " + oneLine(e.second);
                    ImGui::PushID(k);
                    if (ImGui::Selectable(line.c_str(), e.first == textBrowsePick_, ImGuiSelectableFlags_AllowDoubleClick)) {
                        textBrowsePick_ = e.first;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            value = e.first;
                            changed = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::PopID();
                }
        }
        ImGui::EndChild();
        if (ImGui::BeginChild("preview", ImVec2(0, previewHeight), ImGuiChildFlags_Borders)) {
            if (textBrowsePick_ > 0 && ctx.gameData) ImGui::TextWrapped("%s", clientMissionText(ctx, textBrowsePick_).c_str());
        }
        ImGui::EndChild();
        if (ImGui::Button("Select") && textBrowsePick_ >= 0) {
            value = textBrowsePick_;
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("None")) {
            value = 0;
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Open in Text") && textBrowsePick_ > 0) {
            ctx.showTextModule = "generated.client.language.english.missiontextlanguage";
            ctx.showTextId = textBrowsePick_;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return changed;
}


// The client's texts of a mission (missionconversation: name, log, opening, finishing, reward, reminder) or an objective (missionobjective:
// name, body, counters; objectiveconversation lines), editable through the Text workspace.
void MissionEditor::drawClientTexts(EditorContext& ctx, const WorldDatabase& database, const DbTable& table, const std::vector<DbValue>& row) {
    if (!ctx.gameData) return;
    const bool isMission = table.name == "mission_content_definition";
    if (!isMission && table.name != "mission_objective_definition") return;
    const auto value = [&](const char* c) {
        const int i = table.column(c);
        return i < 0 ? int64_t(-1) : row[size_t(i)].asInt();
    };
    const int64_t mission = value("mission_id");
    const int64_t objective = isMission ? 0 : value("objective_id");
    TextWorkspace* text = ctx.textWorkspace;
    const char* header = isMission ? "Client texts (game.zip)" : "Client texts and conversation lines (game.zip)";
    // Preview hook: RASAEDIT_OPEN_CLIENT_TEXTS=1 opens the section once.
    if (std::getenv("RASAEDIT_OPEN_CLIENT_TEXTS")) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (!ImGui::TreeNode(header)) return;
    if (!text) ImGui::TextDisabled("Read only (no Text workspace).");
    else
        ImGui::TextDisabled("What the game shows for mission %lld%s. Changes are saved with the Text workspace (File > Save, Client > Text).",
                            static_cast<long long>(mission), isMission ? "" : (", objective " + std::to_string(objective)).c_str());
    if (!text) ImGui::BeginDisabled();

    // One text: its id (pick another, make a new one, remove) and its English text, edited in place for both genders.
    // setId(newId) stores the reference (0 = remove); returns whether the reference changed.
    int slot = 0;
    const auto textSlot = [&](const char* label, int64_t id, const std::function<std::vector<TextWorkspace::EntryEdit>(int64_t)>& setId,
                              bool removable) {
        ImGui::PushID(slot++);
        int64_t picked = id;
        if (missionTextField(ctx, label, picked, std::nullopt, false) && text && picked != id)
            text->editEntries(std::string("client text ") + label, setId(picked), false);
        ImGui::SameLine();
        if (ImGui::SmallButton("New text") && text) {
            const int64_t fresh = text->newMissionTextId();
            std::vector<TextWorkspace::EntryEdit> edits = setId(fresh);
            edits.push_back({kMissionTextModule, tr::py::makeTuple({tr::py::makeInt(fresh), tr::py::makeInt(1)}), textValue("")});
            edits.push_back({kMissionTextModule, tr::py::makeTuple({tr::py::makeInt(fresh), tr::py::makeInt(2)}), textValue("")});
            text->editEntries(std::string("new client text ") + label, edits, false);
            id = fresh;
        }
        ImGui::SetItemTooltip("A new missiontextlanguage entry (English, both genders; the next free id) for this text");
        if (id != 0 && removable) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove") && text) {
                text->editEntries(std::string("remove client text ") + label, setId(0), false);
                id = 0;
            }
            ImGui::SetItemTooltip("The game then shows no text here (the missiontextlanguage entry itself stays)");
        }
        if (id != 0 && text) {
            const tr::py::Object* lookup = text->currentLookup(kMissionTextModule);
            const tr::py::Object* male = tr::py::dictGetTuple(lookup, {id, 1});
            const tr::py::Object* female = tr::py::dictGetTuple(lookup, {id, 2});
            const std::string current = male ? tr::py::asString(tr::py::item(male, 0)) : std::string();
            if (!male) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "#%lld has no English text yet", static_cast<long long>(id));
                ImGui::SameLine();
                if (ImGui::SmallButton("Add one"))
                    text->editEntries("add text", {{kMissionTextModule, tr::py::makeTuple({tr::py::makeInt(id), tr::py::makeInt(1)}), textValue("")},
                                                   {kMissionTextModule, tr::py::makeTuple({tr::py::makeInt(id), tr::py::makeInt(2)}), textValue("")}},
                                      false);
            } else if (female && tr::py::asString(tr::py::item(female, 0)) != current) {
                ImGui::TextDisabled("The male and female character's texts differ - edit them in the Text workspace.");
                ImGui::SameLine();
                if (ImGui::SmallButton("Open in Text")) {
                    ctx.showTextModule = kMissionTextModule;
                    ctx.showTextId = id;
                }
            } else {
                // A couple of paragraphs tall, word-wrapped, scrolling vertically (as the Text workspace's entry box).
                std::vector<char> buffer(std::max<size_t>(current.size() + 4096, 1024));
                std::memcpy(buffer.data(), current.c_str(), current.size() + 1);
                if (ImGui::InputTextMultiline("##text", buffer.data(), buffer.size(), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 10.5f),
                                              ImGuiInputTextFlags_WordWrap)) {
                    const std::string updated = buffer.data();
                    text->editEntries("mission text " + std::to_string(id),
                                      {{kMissionTextModule, tr::py::makeTuple({tr::py::makeInt(id), tr::py::makeInt(1)}), textValue(updated)},
                                       {kMissionTextModule, tr::py::makeTuple({tr::py::makeInt(id), tr::py::makeInt(2)}), textValue(updated)}},
                                      true);
                }
            }
        }
        ImGui::PopID();
    };

    try {
        if (isMission) {
            static const char* kTypes[] = {"", "Name", "Log", "Opening (offer)", "Finishing", "Reward", "Reminder"};
            const tr::py::Object* conversation = clientLookup(ctx, kConversationModule);
            for (int type = 1; type <= 6; ++type) {
                const tr::py::Object* e = tr::py::dictGetTuple(conversation, {mission, type});
                const int64_t id = e ? tr::py::asInt(tr::py::item(e, 0)) : 0;
                textSlot(kTypes[type], id, [&](int64_t newId) {
                    return std::vector<TextWorkspace::EntryEdit>{{kConversationModule, tr::py::makeTuple({tr::py::makeInt(mission), tr::py::makeInt(type)}),
                                                                  newId ? tr::py::makeList({tr::py::makeInt(newId)}) : nullptr}};
                }, true);
            }
        } else {
            // missionobjective[(mission, objective)] = [(name, body, counter 0, counter 1, counter 2)] (None where unset).
            static const char* kSlots[] = {"Name", "Body", "Counter 1", "Counter 2", "Counter 3"};
            const tr::py::Object* objectives = clientLookup(ctx, kObjectiveModule);
            const tr::py::Object* entry = tr::py::item(tr::py::dictGetTuple(objectives, {mission, objective}), 0);
            int64_t ids[5] = {0, 0, 0, 0, 0};
            for (size_t k = 0; k < 5; ++k)
                if (const tr::py::Object* v = tr::py::item(entry, k); v && !tr::py::isNone(v)) ids[k] = tr::py::asInt(v);
            if (!entry) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "The client has no texts for this objective (a missing-translation message in game).");
            for (size_t k = 0; k < 5; ++k)
                textSlot(kSlots[k], ids[k], [&, k](int64_t newId) {
                    std::vector<tr::py::Ref> items;
                    for (size_t q = 0; q < 5; ++q) {
                        const int64_t v = q == k ? newId : ids[q];
                        items.push_back(v ? tr::py::makeInt(v) : noneValue());
                    }
                    const bool empty = std::all_of(items.begin(), items.end(), [](const tr::py::Ref& r) { return tr::py::isNone(r.get()); });
                    return std::vector<TextWorkspace::EntryEdit>{{kObjectiveModule, tr::py::makeTuple({tr::py::makeInt(mission), tr::py::makeInt(objective)}),
                                                                  empty ? nullptr : tr::py::makeList({tr::py::makeTuple(items)})}};
                }, true);

            // objectiveconversation[(mission, objective, NPC package, player flag, kind)] = [text]: the NPC lines of this objective.
            ImGui::SeparatorText("Conversation lines");
            static const char* kKinds[] = {"", "Completion", "Reminder", "Choice body", "Choice 1", "Choice 2", "Choice 3"};
            const tr::py::Object* lines = clientLookup(ctx, kObjectiveConversationModule);
            std::vector<std::pair<tr::py::Ref, int64_t>> mine;             // key, text id
            for (const auto& e : lines ? lines->entries : decltype(lines->entries){}) {
                const tr::py::Object* k = e.first.get();
                if (tr::py::asInt(tr::py::item(k, 0)) == mission && tr::py::asInt(tr::py::item(k, 1)) == objective)
                    mine.emplace_back(e.first, tr::py::asInt(tr::py::item(e.second.get(), 0)));
            }
            if (mine.empty()) ImGui::TextDisabled("None.");
            for (const auto& [key, id] : mine) {
                const int64_t kind = tr::py::asInt(tr::py::item(key.get(), 4));
                const std::string label = "Package " + std::to_string(tr::py::asInt(tr::py::item(key.get(), 2))) + ", flag " +
                                          std::to_string(tr::py::asInt(tr::py::item(key.get(), 3))) + ", " + (kind >= 1 && kind <= 6 ? kKinds[kind] : "?");
                const tr::py::Ref lineKey = key;
                textSlot(label.c_str(), id, [&, lineKey](int64_t newId) {
                    return std::vector<TextWorkspace::EntryEdit>{{kObjectiveConversationModule, lineKey,
                                                                  newId ? tr::py::makeList({tr::py::makeInt(newId)}) : nullptr}};
                }, true);
            }
            // A new line: the NPC package that says it, the player flag, the kind - with a new text.
            ImGui::Spacing();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            lookupCombo("NPC package", newConversationPackage_, lookup::npcPackage(database, "(pick a package)"));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
            ImGui::InputScalar("Player flag", ImGuiDataType_S64, &newConversationFlag_);
            ImGui::SetItemTooltip("The objectiveconversation key's fourth part (1 in most shipped lines)");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            int kindIndex = int(std::clamp<int64_t>(newConversationKind_, 1, 6)) - 1;
            if (ImGui::Combo("Kind", &kindIndex, "Completion\0Reminder\0Choice body\0Choice 1\0Choice 2\0Choice 3\0")) newConversationKind_ = kindIndex + 1;
            const tr::py::Ref newKey = tr::py::makeTuple({tr::py::makeInt(mission), tr::py::makeInt(objective), tr::py::makeInt(newConversationPackage_),
                                                          tr::py::makeInt(newConversationFlag_), tr::py::makeInt(newConversationKind_)});
            const bool exists = lines && tr::py::dictGetRef(*lines, *newKey);
            if (newConversationPackage_ <= 0 || exists || !text) ImGui::BeginDisabled();
            if (ImGui::Button("Add a line with a new text")) {
                const int64_t fresh = text->newMissionTextId();
                text->editEntries("new conversation line",
                                  {{kObjectiveConversationModule, newKey, tr::py::makeList({tr::py::makeInt(fresh)})},
                                   {kMissionTextModule, tr::py::makeTuple({tr::py::makeInt(fresh), tr::py::makeInt(1)}), textValue("")},
                                   {kMissionTextModule, tr::py::makeTuple({tr::py::makeInt(fresh), tr::py::makeInt(2)}), textValue("")}},
                                  false);
            }
            if (newConversationPackage_ <= 0 || exists || !text) ImGui::EndDisabled();
            if (exists) {
                ImGui::SameLine();
                ImGui::TextDisabled("(a line with that package, flag and kind exists)");
            }
        }
    } catch (const std::exception& e) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", e.what());
    }
    if (!text) ImGui::EndDisabled();
    ImGui::TreePop();
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
                if (f.kind == FieldKind::MissionText) {
                    if (missionTextField(ctx, f.label, i, clientTextId(ctx, d, row, f.column))) { v = DbValue::integer(i); changed = true; }
                    break;
                }
                // A value that names something is picked by name (Lookups.h): another row of this mission (objective, reward, spawn
                // group, scenario, area, indicator, spawn, or a mission), a database row, or an entry of the client's data.
                LookupSource source;
                bool named = true;
                if (const MissionReference ref = missionReference(f.column); ref.table) {
                    source = pairsSource(std::make_shared<std::vector<std::pair<int64_t, std::string>>>(referenceChoices(ctx, d, row, ref)));
                } else if (f.kind == FieldKind::Creature) {
                    source = lookup::creature(ctx, database);
                } else if (f.kind == FieldKind::ItemTemplate) {
                    source = lookup::itemTemplate(ctx, database);
                } else if (f.kind == FieldKind::EntityClass) {
                    source = lookup::entityClass(ctx);
                } else if (f.kind == FieldKind::MapContext) {
                    source = lookup::map(database);
                } else if (std::strcmp(f.column, "npc_package_id") == 0) {
                    source = lookup::npcPackage(database);
                    source.allowOther = true;
                } else if (std::strcmp(f.column, "tutorial_id") == 0) {
                    source = lookup::constants(ctx, "generated.client.tutorialdata");      // Rasa.Game TutorialId = the client's ids
                } else if (std::strcmp(f.column, "skill_id") == 0) {
                    source = lookup::constants(ctx, "generated.client.skilldata");         // Rasa.Game SkillId = skilldata's ids
                } else if (std::strcmp(f.column, "qualification_key") == 0) {
                    source = lookup::values(lookup::kQualificationKeys);
                } else if (std::strcmp(f.column, "audio_set_id") == 0) {
                    source = lookup::audioSet(ctx);
                } else if (std::strcmp(f.column, "category_id") == 0) {
                    source = lookup::missionCategory(ctx, "(none)");
                } else if (std::strcmp(f.column, "required_mission_state") == 0) {
                    source = lookup::values(kMissionState);
                } else if (std::strcmp(f.column, "ability_id") == 0) {
                    source = lookup::constants(ctx, "generated.client.actiondata");        // action ids (WEAPON_ATTACK = 1, as the action table)
                } else if (std::strcmp(f.column, "player_flag_id") == 0) {
                    source = pairsSource(std::make_shared<std::vector<std::pair<int64_t, std::string>>>(playerFlagChoices()));
                    source.allowOther = true;                                                // a new flag is a new number
                } else if (std::strcmp(f.column, "subject_id") == 0) {
                    // MissionProgressEvent (Rasa.Missions/Runtime): the subject by event kind.
                    const int ek = d.column("event_kind");
                    const int64_t eventKind = ek >= 0 && !row[size_t(ek)].isNull() ? row[size_t(ek)].asInt() : -1;
                    const auto local = [&](const char* column) {
                        return pairsSource(std::make_shared<std::vector<std::pair<int64_t, std::string>>>(
                            referenceChoices(ctx, d, row, missionReference(column))));
                    };
                    switch (eventKind) {
                        case 0: source = pairsSource(std::make_shared<std::vector<std::pair<int64_t, std::string>>>(teleporters_)); break;  // teleporter id
                        case 1: source = lookup::constants(ctx, "generated.client.logosstone"); break;   // logos id
                        case 2: source = lookup::creature(ctx, database); break;                         // creature row
                        case 3: source = local("required_mission_id"); break;                            // the completed mission
                        case 4: case 5: case 6: case 8: case 13: case 15:                                // item / interaction / object / creature class
                            source = lookup::entityClass(ctx); break;
                        case 7: source = local("area_id"); break;                                        // this mission's area
                        case 9: source = lookup::constants(ctx, "generated.client.actiondata"); break;   // action id
                        case 11: case 12: source = local("related_objective_id"); break;                 // this mission's objective
                        case 14: source = lookup::constants(ctx, "generated.client.constant.creatureflag"); break;
                        default: named = false; break;                                                   // scenario event ids are plain numbers
                    }
                } else {
                    named = false;
                }
                if (named) {
                    ImGui::SetNextItemWidth(width * 1.6f);
                    if (lookupCombo(f.label, i, source)) { v = DbValue::integer(i); changed = true; }
                } else {
                    ImGui::SetNextItemWidth(width);
                    if (ImGui::InputScalar(f.label, ImGuiDataType_S64, &i)) { v = DbValue::integer(i); changed = true; }
                }
                break;
            }
        }
        ImGui::PopID();
    }
    if (!editable) ImGui::EndDisabled();
    drawClientTexts(ctx, database, d, row);
    if (changed) validationDirty_ = true;
    return changed;
}

// The diagnostics of the selected mission that belong to this row (objective / transition / trigger / action / scenario / step).
std::vector<const MissionDiagnostic*> MissionEditor::rowDiagnostics(const TableSpec& spec, const DbTable& table, const std::vector<DbValue>& row) const {
    std::vector<const MissionDiagnostic*> out;
    const auto value = [&](const char* column) {
        const int c = table.column(column);
        return c < 0 ? int64_t(-1) : row[size_t(c)].asInt();
    };
    const std::string name = spec.table;
    for (const MissionDiagnostic& d : validation_.diagnostics) {
        if (d.missionId != selectedMission_ || d.revision != selectedRevision_) continue;
        bool match = false;
        if (name == "mission_objective_definition")
            match = d.objectiveId == value("objective_id") && d.transitionId < 0 && d.triggerId < 0 && d.actionId < 0;
        else if (name == "mission_objective_transition")
            match = d.objectiveId == value("objective_id") && d.transitionId == value("transition_id") && d.triggerId < 0 && d.actionId < 0;
        else if (name == "mission_trigger")
            match = d.objectiveId == value("objective_id") && d.transitionId == value("transition_id") && d.triggerId == value("trigger_id");
        else if (name == "mission_action")
            match = d.objectiveId == value("objective_id") && d.transitionId == value("transition_id") && d.actionId == value("action_id");
        else if (name == "mission_scenario")
            match = d.scenarioId == value("scenario_id") && d.stepId < 0;
        else if (name == "mission_scenario_step")
            match = d.scenarioId == value("scenario_id") && d.stepId == value("step_id");
        if (match) out.push_back(&d);
    }
    return out;
}

void MissionEditor::drawDiagnostics(const std::vector<const MissionDiagnostic*>& list) {
    for (const MissionDiagnostic* d : list) {
        ImGui::PushStyleColor(ImGuiCol_Text, d->startup ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f) : ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s: %s", d->code.c_str(), d->message.c_str());
        ImGui::PopStyleColor();
    }
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
                const std::vector<const MissionDiagnostic*> problems = rowDiagnostics(spec, t->edited, row);
                if (!problems.empty()) label = "(!) " + label;
                ImGui::PushID(int(k));
                if (!problems.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                const bool rowOpen = ImGui::TreeNode((label + "###row").c_str());
                if (!problems.empty()) {
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
                        for (const MissionDiagnostic* d : problems) ImGui::Text("%s: %s", d->code.c_str(), d->message.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                }
                if (writable && !spec.readOnly) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) removes_.push_back(PendingRemove{spec.table, row});
                    ImGui::SetItemTooltip("Remove (with its children)");
                }
                if (rowOpen) {
                    drawDiagnostics(problems);
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
    if (placeholderTextId_ < 0 && ctx.gameData) placeholderTextId_ = findPlaceholderTextId(*ctx.gameData);
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
            validationDirty_ = true;
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
    }
    ImGui::Checkbox("On this map", &mapOnly_);
    ImGui::SetItemTooltip("Missions whose giver or receiver spawns here, or with a spawn group or area on this map.");
    ImGui::SameLine();
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
    // The server's start-up checks on the edited content.
    {
        const MissionValidationResult& v = validation();
        size_t startupCount = 0;
        for (const MissionDiagnostic& d : v.diagnostics) startupCount += d.startup ? 1 : 0;
        const ImVec4 colour = v.startupFails ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f) : v.diagnostics.empty() ? ImVec4(0.5f, 0.85f, 0.5f, 1.0f)
                                                                                                         : ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
        char summary[320];
        if (v.diagnostics.empty())
            std::snprintf(summary, sizeof summary, "Server checks: all %zu enabled missions pass", v.enabledMissions);
        else
            std::snprintf(summary, sizeof summary, "%s%zu/%zu missions left out, %zu problem(s)%s###servercheck",
                          v.startupFails ? "SERVER WOULD NOT START. " : "", v.inactiveMissions.size(), v.enabledMissions, v.diagnostics.size() - startupCount,
                          v.blocksReadiness ? ", not ready" : "");
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        const bool open = !v.diagnostics.empty() && ImGui::TreeNode(summary);
        if (v.diagnostics.empty()) ImGui::TextUnformatted(summary);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Rasa.NET's mission content checks at start-up (MissionContentValidator), run on the edited tables: a mission with a problem\n"
                              "is left out by the server; a problem on a required mission keeps it from reporting ready; a start-up error stops it.\n"
                              "Click a problem to open its mission. Saving refuses new start-up and readiness problems.");
        if (open) {
            if (ImGui::BeginChild("problems", ImVec2(0, ImGui::GetFontSize() * 9), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY)) {
                for (size_t k = 0; k < v.diagnostics.size(); ++k) {
                    const MissionDiagnostic& d = v.diagnostics[k];
                    std::string where;
                    if (d.missionId >= 0) where = std::to_string(d.missionId) + " [" + d.revision + "]";
                    if (d.objectiveId >= 0) where += " objective " + std::to_string(d.objectiveId);
                    if (d.transitionId >= 0) where += " transition " + std::to_string(d.transitionId);
                    if (d.triggerId >= 0) where += " trigger " + std::to_string(d.triggerId);
                    if (d.actionId >= 0) where += " action " + std::to_string(d.actionId);
                    if (d.scenarioId >= 0) where += " scenario " + std::to_string(d.scenarioId);
                    if (d.stepId >= 0) where += " step " + std::to_string(d.stepId);
                    const std::string line = (where.empty() ? std::string() : where + ": ") + d.code + " - " + d.message;
                    ImGui::PushID(int(k));
                    ImGui::PushStyleColor(ImGuiCol_Text, d.startup ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f) : ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                    if (ImGui::Selectable(line.c_str(), d.missionId == selectedMission_ && d.revision == selectedRevision_) && d.missionId >= 0) {
                        selectedMission_ = d.missionId;
                        selectedRevision_ = d.revision;
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::TreePop();
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
            const bool leftOut = r[size_t(missions.column("enabled"))].asInt() != 0 && validation_.inactiveMissions.count(id);
            ImGui::PushID(int(k));
            if (leftOut) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
            if (ImGui::Selectable(((leftOut ? "(!) " : "") + label).c_str(), id == selectedMission_ && revision == selectedRevision_)) {
                selectedMission_ = id;
                selectedRevision_ = revision;
            }
            if (leftOut) {
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("The server would leave this mission out (see its problems).");
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
                    validationDirty_ = true;
                } else if (hasChildren) {
                    ImGui::TextDisabled("(id / revision can only change before rows are added under the mission)");
                }
            }
        }
        if (writable) {
            if (ImGui::SmallButton("Delete mission...")) ImGui::OpenPopup("deletemission");
            if (ImGui::BeginPopup("deletemission")) {
                // mission_area and mission_scene_binding refer to the mission with ON DELETE RESTRICT (rasaworld.db schema) and
                // are not written here (areas are edited in Server objects), so the mission cannot go while it has any.
                // mission_evidence is ON DELETE CASCADE: the database removes it with the mission.
                std::string blockers;
                for (const char* name : {"mission_area", "mission_scene_binding"}) {
                    const Table* child = table(name);
                    if (!child) continue;
                    size_t count = 0;
                    std::set<int64_t> maps;
                    const int mapColumn = child->edited.column("map_context_id");
                    for (const auto& r : child->edited.rows)
                        if (rowUnder(*child, r, root.edited, *selected)) {
                            ++count;
                            if (mapColumn >= 0) maps.insert(r[size_t(mapColumn)].asInt());
                        }
                    if (!count) continue;
                    blockers += "\n  " + std::to_string(count) + " " + name + " row(s)";
                    if (!maps.empty()) {
                        blockers += " on map";
                        for (int64_t m : maps) blockers += " " + std::to_string(m);
                    }
                }
                if (!blockers.empty()) {
                    ImGui::TextWrapped("This mission cannot be deleted: the database keeps these rows from losing their mission:%s\n"
                                       "Delete the areas in Server objects (on each map) and save first.", blockers.c_str());
                    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
                } else {
                    ImGui::Text("Delete this mission and every row under it (not saved until Save missions)?");
                    if (ImGui::Button("Delete")) {
                        removes_.push_back(PendingRemove{root.spec->table, *selected});
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
        // This mission's problems that belong to no row below (mission-wide ones), and how many in all.
        {
            std::vector<const MissionDiagnostic*> own;
            size_t all = 0;
            for (const MissionDiagnostic& d : validation_.diagnostics) {
                if (d.missionId != selectedMission_ || d.revision != selectedRevision_) continue;
                ++all;
                if (d.objectiveId < 0 && d.scenarioId < 0) own.push_back(&d);
            }
            if (all) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                ImGui::TextWrapped("The server would leave this mission out: %zu problem(s)%s.", all, all > own.size() ? " (rows marked (!) below)" : "");
                ImGui::PopStyleColor();
                drawDiagnostics(own);
            } else if ((*selected)[size_t(missions.column("enabled"))].asInt() != 0) {
                ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.0f), "Passes the server's checks.");
            } else {
                ImGui::TextDisabled("Not enabled: the server does not load this revision.");
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
    if (!removes_.empty() || !adds_.empty()) validationDirty_ = true;
    removes_.clear();
    adds_.clear();
}
