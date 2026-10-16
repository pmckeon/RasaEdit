#pragma once

#include "Lookups.h"
#include "MissionValidation.h"
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
    // The server's start-up checks on the edited content (MissionValidation.h), recomputed after each change.
    const MissionValidationResult& validation();
    const MissionValidationResult& savedValidation() const { return savedValidation_; }
    void selectMission(int64_t missionId);            // its enabled revision, else its first
    // Missions by id (mission_content_definition as edited), labelled with their client name: for fields elsewhere that name one.
    LookupSource missionSource(EditorContext& ctx);
    struct MissionReference {
        const char* table = nullptr;                  // the table the column names a row of (nullptr: not a reference)
        const char* key = nullptr;                    // that row's own key column
        const char* what = "";
    };
    static MissionReference missionReference(const char* column);
    // The rows a reference column of `row` (in `table`) can name: this mission's (and revision's) rows of the target table; spawns of
    // the row's spawn group; every mission for required_mission_id. (id, label)
    std::vector<std::pair<int64_t, std::string>> referenceChoices(EditorContext& ctx, const DbTable& table, const std::vector<DbValue>& row,
                                                                  const MissionReference& ref);

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

    MissionValidationResult validateTables(bool edited) const;
    std::vector<const MissionDiagnostic*> rowDiagnostics(const TableSpec& spec, const DbTable& table, const std::vector<DbValue>& row) const;
    void drawDiagnostics(const std::vector<const MissionDiagnostic*>& list);

    std::vector<Table> tables_;                       // parents before children
    MissionReferences references_;
    MissionValidationResult validation_, savedValidation_;
    bool validationDirty_ = true;
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
