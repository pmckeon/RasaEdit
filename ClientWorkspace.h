#pragma once

#include "Panels.h"
#include "PvsBuilder.h"
#include "SoundPlayer.h"
#include "Workspace.h"

#include <memory>
#include <vector>

// Client-side content: the static map (entities from the .map file), models, textures and other assets.
// Map entities can be edited (move, rotate, scale, class, hue, flags, cull layer; duplicate, delete, add) with undo, and saved
// back to the map folder's .map (tr::writeMap; the first save keeps the original as <name>.map.rasaedit-original). Maps are loose
// files the client reads directly (tabula_rasa.cfg Maps = "data/maps/|.map"), so no patch archive is needed.
class ClientWorkspace : public Workspace, public ViewportEditHooks {
public:
    // Writes a file in place, keeping the first original as <file>.rasaedit-original (also used by the UI workspace).
    static bool writeFileWithBackup(const std::string& path, const tr::Bytes& data, std::string& error);
    explicit ClientWorkspace(EditorContext& ctx) : Workspace(ctx) {}
    void handleShortcuts() override;
    void drawEditMenu() override;
    bool hasUnsavedChanges() const override { return !cleanReachable_ || undoStack_.size() != cleanUndoDepth_ || pvsChanged_; }
    bool saveChanges() override { return !hasUnsavedChanges() || saveMap(); }
    bool saveMap();
    void revertMap();
    void undo();
    void redo();
    void duplicateSelected();
    void addEmitter();                               // a sound emitter at the view centre's first surface, as Add places things
    // PVS rows after edits (ClientPvs.cpp; public for the smoke test).
    void pvsStep();                                  // this frame's share of a rebuild (between frames' draw lists)
    std::vector<uint32_t> pvsAffectedRows() const;
    void pvsStartRebuild(const std::vector<uint32_t>& rows);
    void pvsClearRows(const std::vector<uint32_t>& rows);
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
    }
    void onGameOpened() override;
    void onMapLoaded() override;

private:
    void drawEntitiesPanel();
    void drawLightsPanel();
    void drawEntityPanel();
    void drawLightInspector(size_t index);
    void drawLightModifiers(size_t index, const tr::PlacedLight& light);   // its <Mod> tracks, edited (lights.xml)
    void rebuildEntityFilter();
    int selectedEntity() const { return selection_.kind == SelectionKind::MapEntity ? int(selection_.index) : -1; }
    // Map editing: whole snapshots of the entities and the generated collision volumes (keyed by entity id) for undo.
    struct MapEdit {
        std::vector<tr::MapEntity> entities;
        std::vector<tr::MapGeneratedAbv> abvs;
        tr::Bytes lights;                            // the map's lights.xml text
        tr::Bytes emitters;                          // the map's audioemitters.xml text
    };
    // Sound emitters (audioemitters.xml), edited as text like the lights (tr::audioEmittersDocument).
    int selectedEmitter() const { return selection_.kind == SelectionKind::AudioEmitter ? int(selection_.index) : -1; }
    void setEmitterProperty(size_t emitter, const std::string& name, const std::string& value, const std::string& type);
    void emittersChanged();                          // after the audioemitters.xml text changed
    void drawSoundsPanel();
    void drawEmitterInspector(size_t index);
    tr::Vec3 emitterPosition_;
    SoundPlayer soundPlayer_;
    int selectedLight() const { return selection_.kind == SelectionKind::MapLight ? int(selection_.index) : -1; }
    void setLightProperty(size_t light, const std::string& name, const std::string& value, const std::string& type);
    void lightsChanged();                            // after the lights.xml text changed
    tr::Vec3 lightPosition_;                         // selectedPosition() for a light: synced from its Position, written back
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

    // PVS (.vis) rows after map edits (ClientPvs.cpp). The rows were made for the entities in pvsBaseline_ (as loaded, then as of
    // the last rebuild / clear); an entity that moved, changed or went away can uncover what it hid, so every row that sees a leaf
    // its old box overlaps is affected (and, optionally, rows seeing its new box, which could only get tighter).
    void drawPvsPanel();
    std::vector<tr::MapEntity> pvsBaseline_;
    std::unique_ptr<PvsBuilder> pvsBuilder_;
    std::vector<uint32_t> pvsQueue_;                 // rows still to rebuild
    size_t pvsQueueTotal_ = 0;
    bool pvsRowStarted_ = false;
    std::vector<tr::MapEntity> pvsRebuildBaseline_;  // the entities the running rebuild started from
    bool pvsChanged_ = false;                        // rows changed since the .vis was loaded / saved
    bool pvsIncludeNewBoxes_ = true;
    bool pvsMerge_ = true;                           // OR rebuilt rows into the existing ones (never lose visibility)
    size_t pvsViewsPerFrame_ = 60;
    std::string pvsResult_;
    tr::VisFile pvsSavedVis_;                        // as loaded / last saved (Revert)

    Selection selection_;
    std::vector<size_t> visibleEntities_;            // indices into the map's entities passing the filter
    char entityFilter_[128] = "";
    MapsPanel maps_;
    OverviewPanel overview_;
    ViewportPanel viewport_{false};
};
