#pragma once

#include "WorldDatabase.h"

#include <cstdint>
#include <string>
#include <vector>

class EditorContext;

// The Rasa.NET mission content tables (mission_content_definition and its children, keyed by mission id + content
// revision), edited generically: each table is a DbTable, its fields are described by a static spec (MissionEditor.cpp:
// labels, enums from Rasa.NET's mission kinds, references to creatures / item templates / classes / client mission text,
// and which columns each `kind` uses). mission_area is shown read-only here (the Server objects editor writes it), as are
// mission_evidence and mission_scene_binding. Database-wide, with its own Save / Revert.
class MissionEditor {
public:
    struct FieldSpec;
    struct TableSpec;

    void load(const WorldDatabase& database);         // throws std::runtime_error
    void clear();
    bool loaded() const { return !tables_.empty(); }
    bool changed() const;
    bool save(WorldDatabase& database, std::string& message);   // false: nothing written, message says why
    void revert();
    // The panel body: mission list and the selected mission's tree. mapContextId / mapCreatures pick "on this map".
    void draw(EditorContext& ctx, WorldDatabase& database, int32_t mapContextId, const std::vector<int32_t>& mapCreatures, bool writable);

private:
    struct Table {
        const TableSpec* spec = nullptr;
        DbTable saved;
        DbTable edited;
    };
    Table* table(const char* name);
    std::string missionLabel(EditorContext& ctx, const std::vector<DbValue>& row) const;
    void drawChildren(EditorContext& ctx, const WorldDatabase& database, const TableSpec& parent, const std::vector<DbValue>& parentRow, int depth,
                      bool writable);
    bool drawRowFields(EditorContext& ctx, const WorldDatabase& database, Table& t, std::vector<DbValue>& row, bool writable);
    std::vector<DbValue> newRow(const Table& t, const DbTable& parentTable, const std::vector<DbValue>* parentRow) const;
    void removeWithChildren(const char* tableName, const std::vector<DbValue>& row);
    bool rowUnder(const Table& child, const std::vector<DbValue>& row, const DbTable& parentTable, const std::vector<DbValue>& parentRow) const;

    std::vector<Table> tables_;                       // parents before children
    int64_t selectedMission_ = -1;
    std::string selectedRevision_;
    char filter_[64] = "";
    bool mapOnly_ = false;
    // Edits that change row vectors are queued and applied after drawing.
    struct PendingRemove {
        std::string table;
        std::vector<DbValue> row;
    };
    std::vector<PendingRemove> removes_;
    struct PendingAdd {
        std::string table;
        std::vector<DbValue> row;
    };
    std::vector<PendingAdd> adds_;
};
