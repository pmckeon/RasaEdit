#include "RasaEdit.h"
#include "Editor.h"
#include "Renderer.h"

// Main loop follows imgui 1.92.8's examples/example_sdl3_sdlgpu3.
int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "Failed to init SDL: " << SDL_GetError() << std::endl;
        return -1;
    }

    const float mainScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_Window* window = SDL_CreateWindow("RasaEdit", int(1280 * mainScale), int(800 * mainScale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
        return -1;
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    SDL_GPUDevice* gpuDevice = createGpuDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL |
                                               SDL_GPU_SHADERFORMAT_METALLIB, true);
    if (!gpuDevice) {
        std::cerr << "Failed to create GPU Device: " << SDL_GetError() << std::endl;
        return -1;
    }
    if (!SDL_ClaimWindowForGPUDevice(gpuDevice, window)) {
        std::cerr << "Failed to claim window: " << SDL_GetError() << std::endl;
        return -1;
    }
    SDL_SetGPUSwapchainParameters(gpuDevice, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(mainScale);
    style.FontScaleDpi = mainScale;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // ImGui's default font, with system fonts merged in for the client's Korean, Japanese and Chinese text (the Text workspace):
    // the first existing file of each script, probed by path (no platform API). ImGui 1.92 rasterises glyphs on demand.
    io.Fonts->AddFontDefault();
    {
        const char* scripts[][6] = {
            {"C:/Windows/Fonts/malgun.ttf", "/System/Library/Fonts/AppleSDGothicNeo.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", nullptr, nullptr},
            {"C:/Windows/Fonts/msgothic.ttc", "C:/Windows/Fonts/YuGothM.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc", nullptr, nullptr, nullptr},
            {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc", "/System/Library/Fonts/PingFang.ttc", nullptr, nullptr, nullptr}};
        for (const auto& candidates : scripts)
            for (const char* path : candidates) {
                if (!path) break;
                if (SDL_GetPathInfo(path, nullptr)) {
                    ImFontConfig merge;
                    merge.MergeMode = true;
                    io.Fonts->AddFontFromFileTTF(path, 0.0f, &merge);
                    break;
                }
            }
    }

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo initInfo = {};
    initInfo.Device = gpuDevice;
    initInfo.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpuDevice, window);
    initInfo.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    initInfo.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    initInfo.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    ImGui_ImplSDLGPU3_Init(&initInfo);

    {
        Editor editor(window, gpuDevice);
        io.IniFilename = editor.imguiIniPath();      // layout saved next to the editor's settings

        bool done = false;
        while (!done) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                const bool closeRequested = event.type == SDL_EVENT_QUIT ||
                                            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window));
                if (closeRequested && editor.requestQuit()) done = true;      // otherwise the editor asks about unsaved changes
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(10);
                continue;
            }

            if (editor.quitApproved()) break;
            editor.processPendingActions();          // loads happen here, outside any frame's draw lists

            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            editor.drawUi();
            ImGui::Render();

            ImDrawData* drawData = ImGui::GetDrawData();
            const bool isMinimized = drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f;
            SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(gpuDevice);
            editor.renderViewport(commandBuffer);        // offscreen pass, sampled by the ImGui pass below
            SDL_GPUTexture* swapchainTexture = nullptr;
            SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchainTexture, nullptr, nullptr);
            if (swapchainTexture != nullptr && !isMinimized) {
                ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commandBuffer);
                SDL_GPUColorTargetInfo target = {};
                target.texture = swapchainTexture;
                target.clear_color = SDL_FColor{0.10f, 0.11f, 0.12f, 1.0f};
                target.load_op = SDL_GPU_LOADOP_CLEAR;
                target.store_op = SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &target, 1, nullptr);
                ImGui_ImplSDLGPU3_RenderDrawData(drawData, commandBuffer, renderPass);
                SDL_EndGPURenderPass(renderPass);
            }
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
            }
            SDL_SubmitGPUCommandBuffer(commandBuffer);
        }

        SDL_WaitForGPUIdle(gpuDevice);
        editor.releaseGpuResources();
        ImGui::SaveIniSettingsToDisk(io.IniFilename);
        io.IniFilename = nullptr;                      // the path string belongs to the editor
    }

    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(gpuDevice, window);
    SDL_DestroyGPUDevice(gpuDevice);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
