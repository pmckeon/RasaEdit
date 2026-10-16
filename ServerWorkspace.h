#pragma once

#include "LootEditor.h"
#include "ModuleEditor.h"
#include "RecipeEditor.h"
#include "MissionEditor.h"
#include "SoundPlayer.h"
#include "Panels.h"
#include "Workspace.h"
#include "WorldDatabase.h"

#include <memory>
#include <string>
#include <map>
#include <vector>

// Server-side content from the Rasa.NET World database: spawn pools, NPCs, teleporters, objects,
// map links, regions and mission areas. The static map is shown dimmed as context and cannot be
// selected. Edits are kept in memory with undo/redo and written to the database on Save.
class ServerWorkspace : public Workspace, public ViewportEditHooks {
public:
    explicit ServerWorkspace(EditorContext& ctx);
    const char* name() const override { return "Server"; }
    const char* dockspaceName() const override { return "ServerDockSpace"; }
    void buildDefaultLayout(ImGuiID dockspace) override;
    void beginFrame() override;
    void handleShortcuts() override;
    void drawEditMenu() override;
    void drawPanels() override;
    void renderViewport(SDL_GPUCommandBuffer* commands) override { viewport_.render(ctx_, commands, selection_); }
    bool hasUnsavedChanges() const override {
        return !cleanReachable_ || undoStack_.size() != cleanUndoDepth_ || creaturesChanged() || missions_.changed() || loot_.changed() ||
               modules_.changed() || recipes_.changed();
    }
    bool saveChanges() override {
        bool ok = true;
        if (!cleanReachable_ || undoStack_.size() != cleanUndoDepth_) ok = saveServer();
        if (creaturesChanged()) ok = saveCreatures() && ok;
        if (missions_.changed() && database_) {
            std::string message;
            const bool saved = missions_.save(*database_, message);
            ctx_.setStatus(message, !saved);
            ok = saved && ok;
        }
        if (recipes_.changed() && database_) {
            std::string message;
            const bool saved = recipes_.save(*database_, message);
            ctx_.setStatus(message, !saved);
            ok = saved && ok;
        }
        if (modules_.changed() && database_) {
            std::string message;
            const bool saved = modules_.save(*database_, message);
            ctx_.setStatus(message, !saved);
            ok = saved && ok;
        }
        if (loot_.changed() && database_) {
            std::string message;
            const bool saved = loot_.save(*database_, message);
            ctx_.setStatus(message, !saved);
            ok = saved && ok;
        }
        return ok;
    }
    void onGameOpened() override { loadServerContent(); }
    void onMapLoaded() override;

    void openDatabase(const std::string& path);      // call between frames
    void showDatabaseDialog();
    // MySQL / MariaDB World database (Rasa.NET's MySQL provider): a connect dialog, pre-filled from `spec`
    // (mysql://user@host:port/database) when given. The settings keep the spec without the password; with "remember" the
    // password is stored apart (databasePassword, plain text in the settings file).
    void showMySqlDialog(const std::string& spec = "");
    bool canEditServer() const { return database_ && !database_->readOnly() && serverContent_.mapContextId >= 0; }

    // ViewportEditHooks
    bool editable() const override { return canEditServer(); }
    tr::Vec3* selectedPosition() override;
    float* selectedRotation() override;
    void beginEdit() override;                       // snapshot for undo, before a change
    void contentChanged() override;
    void deleteSelected() override;
    bool& snapToGround() override { return snapToGround_; }
    void undo();
    void redo();
    bool creaturesChanged() const;
    bool saveCreatures();
    std::string creatureDisplayName(const CreatureDefinition& d) const;   // what the client shows (creature.py GetName)
    // A new object of that kind at the view centre, selected (the Add menu; public for the smoke test).
    void addServerObject(SelectionKind kind, ObjectKind objectKind);

private:
    static void SDLCALL databaseDialogCallback(void* userdata, const char* const* filelist, int filter);
    void drawMySqlDialog();
    bool mySqlDialogOpen_ = false;
    char mySqlHost_[128] = "127.0.0.1";
    int mySqlPort_ = 3306;
    char mySqlUser_[64] = "rasa";
    char mySqlPassword_[128] = "";
    char mySqlDatabase_[64] = "rasaworld";
    bool mySqlRemember_ = false;
    bool rememberPasswordOnOpen_ = false;          // the connection being opened came from the dialog with "remember" set
    void loadServerContent();
    bool selectionPosition(const Selection& s, tr::Vec3& position) const;
    std::string serverObjectLabel(const Selection& s) const;
    void drawServerPanel();
    void drawInspectorPanel();
    void drawInspectorContent();
    void drawServerObjectEditor();
    void noteEdit();                                 // after a widget changed a value: one snapshot per interaction
    void validateSelection();
    bool saveServer();
    void revertServer();
    void duplicateSelected();
    // Patrol steps: a new step of a saved pool after `afterStep` (-1: at the end; later steps are renumbered up by one), and
    // keeping content.patrolSteps sorted by pool then step with the selection on (pool, step).
    void insertPatrolStep(int64_t poolId, int32_t afterStep, const tr::Vec3& position);
    void sortPatrolSteps(int64_t selectPool, int32_t selectStep);
    bool creatureCombo(const char* id, int32_t& creatureId, bool allowNone);
    // Creatures panel: creature definitions (creature, creature_stat, creature_appearance), database-wide, with its own
    // Save / Revert (no undo).
    void drawCreaturePanel();
    void drawMissionPanel();
    void drawCreatureEditor(CreatureDefinition& d, bool isNew);
    void reloadCreatureDefinitions();
    std::string creatureLabel(const CreatureDefinition& d) const;

    Selection selection_;
    std::unique_ptr<WorldDatabase> database_;
    WorldContent serverContent_;
    WorldContent savedContent_;                      // as loaded / last saved
    std::vector<WorldContent> undoStack_;
    std::vector<WorldContent> redoStack_;
    size_t cleanUndoDepth_ = 0;
    bool cleanReachable_ = true;
    ImGuiID editItem_ = 0;                           // widget whose interaction already has a snapshot
    bool snapToGround_ = true;
    char creatureFilter_[64] = "";
    // FX emitters: every .pkg in the client's string table (id, name), read on first use; the package combo's filter.
    std::vector<std::pair<int32_t, std::string>> fxPackages_;
    bool fxPackagesRead_ = false;
    char fxPackageFilter_[96] = "";
    bool fxPackageCombo(const char* id, int64_t& packageId);
    // Control points: a point's links (garrison pools, hospital, waypoint, bosses, clan lockbox) and, for a pool / teleporter /
    // footlocker, the point it belongs to.
    void drawControlPointLinks(const PlacedObject& point);
    void drawControlPointFields(PlacedObject& point);           // marker_entity_id / default_owner (Rasa.NET ControlPointEntry)
    bool controlPointsEditable() const;                         // the world has those columns: points can be added and deleted
    void drawControlPointMembership(std::initializer_list<int32_t> kinds, int64_t objectId);
    void removeControlPointLinks(std::initializer_list<int32_t> kinds, int64_t objectId);   // the object is gone
    std::string linkedObjectLabel(int32_t kind, int64_t objectId) const;
    // Map-screen markers: the client's factioned markers of this map's template (read with the content) and their map_marker
    // bindings. objectKind 1 teleporter, 2 kraftwerks; `types` = the marker types such an object can show.
    std::vector<tr::MapScreenMarker> clientMarkers_;
    void drawMarkerBinding(int32_t objectKind, int64_t objectId, const tr::Vec3& position, std::initializer_list<int32_t> types);
    void drawMarkerList();
    const tr::MapScreenMarker* clientMarker(int64_t entityId) const;
    void removeMarkerBindings(int32_t objectKind, int64_t objectId);       // the object is gone
    char serverFilter_[128] = "";
    MissionEditor missions_;
    LootEditor loot_;
    ModuleEditor modules_;
    RecipeEditor recipes_;
    int craftingTab_ = 0;                             // 0 modules, 1 modifiable classes, 2 recipes (preview hook)
    void drawCraftingPanel();
    void drawLootPanel();
    std::vector<CreatureDefinition> creatureDefs_;
    std::vector<CreatureDefinition> creatureDefsSaved_;
    std::map<int32_t, int32_t> classCries_, classCriesSaved_;     // creature_battlecry scope 1 (class id -> package)
    void drawBattlecry(CreatureDefinition& d);
    std::map<int32_t, std::set<int32_t>> classFlags_, classFlagsSaved_;   // creature_class_flag (class id -> flag ids)
    std::vector<std::pair<int32_t, std::string>> creatureFlagNames_;      // generated.client.constant.creatureflag (value, name)
    void drawClassFlags(const CreatureDefinition& d);
    SoundPlayer cryPlayer_;                                       // battle cry previews
    int32_t creatureSelected_ = 0;                    // creature id shown in the editor (0: none)
    int focusModules_ = 0;                            // likewise the Modules panel
    int focusCreatures_ = 0;                          // frames left to bring the Creatures panel to the front (preview hook)
    char creatureListFilter_[64] = "";
    bool creatureMapOnly_ = false;
    MapsPanel maps_;
    OverviewPanel overview_;
    ViewportPanel viewport_{true};
};
