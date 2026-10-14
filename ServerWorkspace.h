#pragma once

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
        return !cleanReachable_ || undoStack_.size() != cleanUndoDepth_ || creaturesChanged() || missions_.changed();
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
        return ok;
    }
    void onGameOpened() override { loadServerContent(); }
    void onMapLoaded() override;

    void openDatabase(const std::string& path);      // call between frames
    void showDatabaseDialog();
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

private:
    static void SDLCALL databaseDialogCallback(void* userdata, const char* const* filelist, int filter);
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
    void addServerObject(SelectionKind kind, ObjectKind objectKind);
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
    char serverFilter_[128] = "";
    MissionEditor missions_;
    std::vector<CreatureDefinition> creatureDefs_;
    std::vector<CreatureDefinition> creatureDefsSaved_;
    std::map<int32_t, int32_t> classCries_, classCriesSaved_;     // creature_battlecry scope 1 (class id -> package)
    void drawBattlecry(CreatureDefinition& d);
    SoundPlayer cryPlayer_;                                       // battle cry previews
    int32_t creatureSelected_ = 0;                    // creature id shown in the editor (0: none)
    char creatureListFilter_[64] = "";
    bool creatureMapOnly_ = false;
    MapsPanel maps_;
    OverviewPanel overview_;
    ViewportPanel viewport_{true};
};
