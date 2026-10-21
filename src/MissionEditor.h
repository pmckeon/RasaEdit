#pragma once

#include "Lookups.h"
#include "MissionValidation.h"
#include "WorldDatabase.h"

#include <cstdint>
#include <optional>
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
    // Player flags (player_flag_id): no table or client module names them - Rasa.NET's mission flags are authored numbers below
    // 0x80000000 (CharacterFlagIds.IsMissionFlag), its own server flags above (BootcampComplete 0x80000001). The choices are the flags
    // the edited mission content sets or reads, each labelled by its uses, plus the server flag. (id, label)
    std::vector<std::pair<int64_t, std::string>> playerFlagChoices() const;

    // A table as edited, and a new row of it with its defaults (public for the smoke test: MSVC mangles access into the name).
    struct Table {
        const TableSpec* spec = nullptr;
        DbTable saved;
        DbTable edited;
    };
    Table* table(const char* name);
    std::vector<DbValue> newRow(const Table& t, const DbTable& parentTable, const std::vector<DbValue>* parentRow) const;

    // The mission text placeholder, shown as ID_DEFAULT_TEXT (as the UI's ID_DEFAULT_TEXT: an id the client has, with no text): the
    // lowest missiontextlanguage id that is empty in every language that has it (both genders) and that no client mission table
    // (missionconversation / missionobjective / objectiveconversation) names - 17240 in the shipped data. New missions get it as their
    // name text, new objectives as name and body text, until another is chosen. -1 = not worked out yet, 0 = none found.
    int64_t placeholderTextId(EditorContext& ctx);   // worked out on first use after a load (findPlaceholderTextId)

private:
    std::string missionLabel(EditorContext& ctx, const std::vector<DbValue>& row) const;
    // Mission text as the client shows it. The server never sends text ids: the client looks a mission's texts up by mission id
    // (generated.client.missionconversation[(mission, type)], missionobjective[(mission, objective)], objectiveconversation[(mission,
    // objective, package, flag, kind)]) in missiontextlanguage. The database's client_*_text_id columns are copies the server only
    // checks, so a field shows the client's text, warns when the column names another, and picks ids from a searchable browser.
    std::optional<int64_t> clientTextId(EditorContext& ctx, const DbTable& table, const std::vector<DbValue>& row, const char* column) const;
    // showInGame false: no "In game" line (the client-text editor, where the field IS the client's reference).
    bool missionTextField(EditorContext& ctx, const char* label, int64_t& value, std::optional<int64_t> client, bool showInGame = true);
    // The client's mission texts in game.zip, editable (2026-10-19): per mission its six missionconversation texts, per objective its
    // missionobjective texts (name, body, three counters) and its objectiveconversation lines - pick an existing text, make a new
    // one, edit the English text in place (both genders), remove. Edits go through the Text workspace (EditorContext::textWorkspace:
    // one undo history, saved into game.zip with its other changes, the modules written as stored source).
    void drawClientTexts(EditorContext& ctx, const WorldDatabase& database, const DbTable& table, const std::vector<DbValue>& row);
    int64_t newConversationPackage_ = 0, newConversationFlag_ = 1, newConversationKind_ = 1;
    void drawChildren(EditorContext& ctx, const WorldDatabase& database, const TableSpec& parent, const std::vector<DbValue>& parentRow, int depth,
                      bool writable);
    bool drawRowFields(EditorContext& ctx, const WorldDatabase& database, Table& t, std::vector<DbValue>& row, bool writable);
    void removeWithChildren(const char* tableName, const std::vector<DbValue>& row);
    bool rowUnder(const Table& child, const std::vector<DbValue>& row, const DbTable& parentTable, const std::vector<DbValue>& parentRow) const;

    MissionValidationResult validateTables(bool edited) const;
    std::vector<const MissionDiagnostic*> rowDiagnostics(const TableSpec& spec, const DbTable& table, const std::vector<DbValue>& row) const;
    void drawDiagnostics(const std::vector<const MissionDiagnostic*>& list);

    std::vector<Table> tables_;                       // parents before children
    MissionReferences references_;
    std::vector<std::pair<int64_t, std::string>> modules_;       // module_class rows (id, comment): reward item modules
    std::vector<std::pair<int64_t, std::string>> teleporters_;   // teleporter rows (id, "description  (map)"): waypoint subjects
    MissionValidationResult validation_, savedValidation_;
    bool validationDirty_ = true;
    int64_t selectedMission_ = -1;
    std::string selectedRevision_;
    char filter_[64] = "";
    int64_t placeholderTextId_ = -1;
    char textBrowseFilter_[128] = "";                    // the mission text browser (one popup at a time)
    std::vector<std::pair<int64_t, std::string>> textBrowseAll_, textBrowseShown_;
    int64_t textBrowsePick_ = -1;
    std::string textBrowseShownFor_;
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
