#pragma once

#include "EditorContext.h"

#include <SDL3/SDL.h>
#include "imgui.h"

// A workspace is one editor layout with its own panels, selection and (where it edits) undo history.
// Only the active workspace is drawn; each has its own dockspace, so each keeps its own layout.
class Workspace {
public:
    explicit Workspace(EditorContext& ctx) : ctx_(ctx) {}
    virtual ~Workspace() = default;
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    virtual const char* name() const = 0;
    virtual const char* dockspaceName() const = 0;
    virtual void buildDefaultLayout(ImGuiID dockspace) = 0;     // first use, when no saved layout exists

    virtual void beginFrame() {}
    virtual void handleShortcuts() {}
    virtual void drawEditMenu() {}                               // contents of the Edit menu
    virtual void drawPanels() = 0;
    virtual void renderViewport(SDL_GPUCommandBuffer* commands) = 0;

    virtual bool hasUnsavedChanges() const { return false; }
    virtual bool saveChanges() { return true; }

    virtual void onGameOpened() {}
    virtual void onMapLoaded() {}

protected:
    EditorContext& ctx_;
};
