// Headless smoke test of the whole editor: real ImGui frames (no window or platform backend), the
// real Editor with both workspaces, and the GPU viewport on any Vulkan driver (e.g. lavapipe).
// Settings come from $XDG_DATA_HOME/RasaEdit/RasaEdit/settings.ini (see the test script).
#define private public                       // the test inspects workspace internals
#define protected public
#include "Editor.h"
#undef private
#undef protected

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilderGetNode

#include <cstdio>
#include <cstdlib>

static int failures = 0;
static void check(bool ok, const char* what) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// Dockspace IDs are computed inside a frame (as the editor does), at the root of the ID stack.
static bool dockspaceExists(const char* name) {
    ImGui::NewFrame();
    const bool exists = ImGui::DockBuilderGetNode(ImGui::GetID(name)) != nullptr;
    ImGui::EndFrame();
    return exists;
}

static void frames(Editor& editor, SDL_GPUDevice* device, int count) {
    for (int k = 0; k < count; ++k) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        editor.processPendingActions();
        ImGui::NewFrame();
        editor.drawUi();
        ImGui::Render();
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
        editor.renderViewport(commands);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
    }
}

static void saveViewport(EditorContext& ctx, SDL_GPUDevice* device, const char* path) {
    const uint32_t w = ctx.renderer->width(), h = ctx.renderer->height();
    SDL_GPUTransferBufferCreateInfo info = {};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = w * h * 4;
    SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(device, &info);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureRegion source = {};
    source.texture = ctx.renderer->colourTarget();
    source.w = w;
    source.h = h;
    source.d = 1;
    SDL_GPUTextureTransferInfo destination = {};
    destination.transfer_buffer = download;
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    FILE* f = std::fopen(path, "wb");
    std::fprintf(f, "%u %u\n", w, h);
    std::fwrite(SDL_MapGPUTransferBuffer(device, download, false), 1, w * h * 4, f);
    std::fclose(f);
    SDL_UnmapGPUTransferBuffer(device, download);
    SDL_ReleaseGPUTransferBuffer(device, download);
}

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (!device) { std::printf("no GPU device: %s\n", SDL_GetError()); return 1; }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600, 900);
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;   // as the SDL GPU backend declares
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    {
        Editor editor(nullptr, device);
        EditorContext& ctx = *editor.context_;
        ServerWorkspace& server = *editor.server_;
        frames(editor, device, 3);
        check(ctx.map != nullptr, "map reopened from settings");
        check(editor.active_ == editor.server_.get(), "starts in the Server workspace (from settings)");
        check(dockspaceExists("ServerDockSpace"), "server layout built");
        check(server.database_ && !server.database_->readOnly(), "server database open read-write");
        check(server.serverContent_.spawnPools.size() == 251, "251 spawn pools loaded for Concordia");
        if (const char* names[] = {"Viewport###Server.Viewport", "Overview###Server.Overview"}; true)
            for (const char* n : names) {
                ImGuiWindow* w = ImGui::FindWindowByName(n);
                std::printf("      %-28s exists %d active %d hidden %d docked %d selectedTab %s\n", n, w != nullptr, w ? w->Active : 0, w ? w->Hidden : 0,
                            w && w->DockNode ? 1 : 0,
                            w && w->DockNode ? (w->DockNode->SelectedTabId == w->TabId ? "this" : "other") : "-");
            }
        check(server.viewport_.visible_, "server viewport drawn");
        check(ctx.scene && ctx.scene->dimMapEntities, "static map dimmed in the server viewport");
        frames(editor, device, 60);                                  // let models load
        saveViewport(ctx, device, "/tmp/smoke_server.rgba");

        // Edit through the viewport hooks: select pool 0, move it, check the unsaved state through undo and redo.
        server.selection_ = Selection{SelectionKind::SpawnPool, 0};
        const tr::Vec3 before = server.serverContent_.spawnPools[0].position;
        server.beginEdit();
        server.selectedPosition()->x += 3.0f;
        server.contentChanged();
        check(server.hasUnsavedChanges(), "edit marks unsaved changes");
        server.undo();
        check(!server.hasUnsavedChanges() && server.serverContent_.spawnPools[0].position.x == before.x, "undo restores the saved state");
        server.redo();
        check(server.hasUnsavedChanges() && server.serverContent_.spawnPools[0].position.x == before.x + 3.0f, "redo reapplies the edit");

        // Requesting another map with unsaved changes must ask, not load.
        ctx.queueMap("adv_bootcamp");
        frames(editor, device, 2);
        check(editor.promptAction_ == Editor::PromptAction::OpenMap && ctx.currentMapName == "adv_foreas_concordia_wilderness",
              "map switch with unsaved changes opens the prompt instead");
        editor.promptAction_ = Editor::PromptAction::None;          // (cancel)

        check(server.saveChanges() && !server.hasUnsavedChanges(), "save succeeds and clears the unsaved state");

        // Switch to the client workspace.
        editor.setActive(editor.client_.get());
        frames(editor, device, 3);
        check(dockspaceExists("ClientDockSpace"), "client layout built");
        check(editor.client_->viewport_.visible_, "client viewport drawn");
        check(!ctx.scene->showServer && !ctx.scene->dimMapEntities, "client viewport hides the server overlay, map not dimmed");
        check(editor.client_->visibleEntities_.size() == ctx.map->map.entities.size(), "client entity table filled");
        saveViewport(ctx, device, "/tmp/smoke_client.rgba");

        // Back to server: its selection and options survive the switch.
        editor.setActive(editor.server_.get());
        frames(editor, device, 2);
        check(server.selection_.kind == SelectionKind::SpawnPool && ctx.scene->showServer, "server selection and layers kept across switches");
        SDL_WaitForGPUIdle(device);
        editor.releaseGpuResources();
    }
    ImGui::DestroyContext();
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
