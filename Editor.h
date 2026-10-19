#pragma once

#include "AssetsWorkspace.h"
#include "ClientWorkspace.h"
#include "EditorContext.h"
#include "ServerWorkspace.h"
#include "TextWorkspace.h"
#include "UiWorkspace.h"

#include <SDL3/SDL.h>

#include <memory>
#include <string>

// The editor shell: the shared context, the Client, UI, Text, Server and Assets workspaces (one active at a time,
// each with its own panels and docking layout), the menu bar, queued loads and unsaved-change prompts.
class Editor {
public:
    Editor(SDL_Window* window, SDL_GPUDevice* device);
    ~Editor();

    const char* imguiIniPath() const { return context_->imguiIniPath.c_str(); }

    // Once per frame before ImGui::NewFrame(): runs queued work (game/map/database loads, model
    // loading) at a point where no draw list references GPU resources that may be replaced.
    void processPendingActions();
    // Builds the UI for this frame (between ImGui::NewFrame() and ImGui::Render()).
    void drawUi();
    // Records the active workspace's 3D viewport (after ImGui::Render(), before the swapchain pass).
    void renderViewport(SDL_GPUCommandBuffer* commands);
    // Releases GPU resources. Call after SDL_WaitForGPUIdle(), before destroying the device.
    void releaseGpuResources();
    // Called when the window is asked to close: true if it may close now; otherwise the editor asks
    // about unsaved changes and quitApproved() turns true once the user has chosen.
    bool requestQuit();
    bool quitApproved() const { return quitApproved_; }

    void setActive(Workspace* workspace);           // switch workspaces (public for the smoke test)

private:
    enum class PromptAction : uint8_t { None, OpenGame, OpenMap, OpenDatabase, Quit };
    void drawMenuBar();
    void drawUnsavedPrompt();

    std::unique_ptr<EditorContext> context_;
    std::unique_ptr<ClientWorkspace> client_;
    std::unique_ptr<UiWorkspace> ui_;
    std::unique_ptr<TextWorkspace> text_;
    std::unique_ptr<ServerWorkspace> server_;
    std::unique_ptr<AssetsWorkspace> assets_;
    Workspace* active_ = nullptr;

    PromptAction promptAction_ = PromptAction::None;
    std::string promptTarget_;
    bool skipUnsavedCheck_ = false;
    bool quitApproved_ = false;
};
