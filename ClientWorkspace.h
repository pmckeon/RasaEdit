#pragma once

#include "Panels.h"
#include "Workspace.h"

#include <vector>

// Client-side content: the static map (entities from the .map file), models, textures and other assets.
// Read-only for now; the server overlay is hidden and only map entities can be selected.
class ClientWorkspace : public Workspace {
public:
    explicit ClientWorkspace(EditorContext& ctx) : Workspace(ctx) {}
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
    void drawEntityPanel();
    void rebuildEntityFilter();
    int selectedEntity() const { return selection_.kind == SelectionKind::MapEntity ? int(selection_.index) : -1; }

    Selection selection_;
    std::vector<size_t> visibleEntities_;            // indices into the map's entities passing the filter
    char entityFilter_[128] = "";
    MapsPanel maps_;
    AssetsPanel assets_;
    OverviewPanel overview_;
    ViewportPanel viewport_{false};
};
