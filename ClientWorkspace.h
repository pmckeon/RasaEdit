#pragma once

#include "Panels.h"
#include "Workspace.h"

#include <vector>

// Client-side content: the static map (entities from the .map file), models, textures and other assets.
// Map entities can be edited (move, rotate, scale, class, hue, flags, cull layer; duplicate, delete, add) with undo, and saved
// back to the map folder's .map (tr::writeMap; the first save keeps the original as <name>.map.rasaedit-original). Maps are loose
// files the client reads directly (tabula_rasa.cfg Maps = "data/maps/|.map"), so no patch archive is needed.
class ClientWorkspace : public Workspace, public ViewportEditHooks {
public:
    explicit ClientWorkspace(EditorContext& ctx) : Workspace(ctx) {}
    void handleShortcuts() override;
    void drawEditMenu() override;
    bool hasUnsavedChanges() const override { return !cleanReachable_ || undoStack_.size() != cleanUndoDepth_; }
    bool saveChanges() override { return !hasUnsavedChanges() || saveMap(); }
    bool saveMap();
    void revertMap();
    void undo();
    void redo();
    void duplicateSelected();
    // ViewportEditHooks (map entities)
    bool editable() const override { return ctx_.map != nullptr; }
    tr::Vec3* selectedPosition() override;
    float* selectedRotation() override;
    void beginEdit() override;
    void contentChanged() override;
    void deleteSelected() override;
    bool& snapToGround() override { return snapToGround_; }
    const char* name() const override { return "Client"; }
    const char* dockspaceName() const override { return "ClientDockSpace"; }
    void buildDefaultLayout(ImGuiID dockspace) override;
    void drawPanels() override;
    void renderViewport(SDL_GPUCommandBuffer* commands) override {
        viewport_.render(ctx_, commands, selection_);
        assets_.render(ctx_, commands);             // the Assets panel's model preview, when shown
    }
    void releaseGpuResources() { assets_.releaseGpuResources(); }
    void onGameOpened() override;
    void onMapLoaded() override;

private:
    void drawEntitiesPanel();
    void drawLightsPanel();
    void drawEntityPanel();
    void drawLightInspector(size_t index);
    void rebuildEntityFilter();
    int selectedEntity() const { return selection_.kind == SelectionKind::MapEntity ? int(selection_.index) : -1; }
    // Map editing: whole snapshots of the entities and the generated collision volumes (keyed by entity id) for undo.
    struct MapEdit {
        std::vector<tr::MapEntity> entities;
        std::vector<tr::MapGeneratedAbv> abvs;
    };
    MapEdit snapshot() const;
    void restore(const MapEdit& edit);
    void noteEdit();                                 // one undo step per edited inspector field
    void addEntity(int32_t classId);
    void entityChanged(size_t index);                // after one entity changed: its placement
    void entitiesChanged();                          // after entities were added or removed
    uint64_t newEntityId() const;
    std::vector<MapEdit> undoStack_, redoStack_;
    size_t cleanUndoDepth_ = 0;
    bool cleanReachable_ = true;
    MapEdit saved_;                                  // as loaded / last saved (Revert)
    float yawHandle_ = 0;                            // selectedRotation: the viewport adds to it; contentChanged turns it into a rotation
    bool snapToGround_ = false;
    unsigned editItem_ = 0;
    int32_t newClass_ = 0;
    std::vector<size_t> fitBounds_;                  // entities whose class changed: take the new model's bounds once it is loaded

    Selection selection_;
    std::vector<size_t> visibleEntities_;            // indices into the map's entities passing the filter
    char entityFilter_[128] = "";
    MapsPanel maps_;
    AssetsPanel assets_;
    OverviewPanel overview_;
    ViewportPanel viewport_{false};
};
