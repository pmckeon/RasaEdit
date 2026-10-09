#pragma once

#include "Panels.h"
#include "Workspace.h"
#include "WorldDatabase.h"

#include <memory>
#include <string>
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
    bool hasUnsavedChanges() const override { return !cleanReachable_ || undoStack_.size() != cleanUndoDepth_; }
    bool saveChanges() override { return saveServer(); }
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
    void undo();
    void redo();
    bool saveServer();
    void revertServer();
    void duplicateSelected();
    void addServerObject(SelectionKind kind, ObjectKind objectKind);
    bool creatureCombo(const char* id, int32_t& creatureId, bool allowNone);

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
    MapsPanel maps_;
    OverviewPanel overview_;
    ViewportPanel viewport_{true};
};
