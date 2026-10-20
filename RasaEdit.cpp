#include <algorithm>
#include "RasaEdit.h"
#include "Editor.h"
#include "Renderer.h"

// Main loop follows imgui 1.92.8's examples/example_sdl3_sdlgpu3.
// ImGui sizes a TrueType font so its hhea ascent - descent spans the requested pixel size (stb_truetype ScaleForPixelHeight).
// The CJK system fonts reserve a lot of room above and below their em (Malgun Gothic's ascent - descent is ~1.3 em), so merged at
// the default font's size their glyphs came out far smaller than the Latin text. This reads that ratio from the font's own tables
// (the first face of a .ttc): (hhea ascender - descender) / head unitsPerEm; 1 when the file cannot be read.
static float fontEmScale(const char* path) {
    SDL_IOStream* io = SDL_IOFromFile(path, "rb");
    if (!io) return 1.0f;
    auto u16 = [&](Sint64 at) {
        Uint8 b[2] = {};
        if (SDL_SeekIO(io, at, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(io, b, 2) != 2) return 0u;
        return unsigned(b[0]) << 8 | unsigned(b[1]);
    };
    auto u32 = [&](Sint64 at) { return (u16(at) << 16) | u16(at + 2); };
    Sint64 face = 0;
    if (u32(0) == 0x74746366u) face = Sint64(u32(12));          // 'ttcf': the first font's offset
    const unsigned tables = u16(face + 4);
    unsigned unitsPerEm = 0;
    int ascender = 0, descender = 0;
    for (unsigned t = 0; t < tables && t < 64; ++t) {
        const Sint64 record = face + 12 + Sint64(t) * 16;
        const unsigned tag = u32(record), offset = u32(record + 8);
        if (tag == 0x68656164u) unitsPerEm = u16(offset + 18);                    // 'head'
        if (tag == 0x68686561u) {                                                   // 'hhea'
            ascender = int(int16_t(u16(offset + 4)));
            descender = int(int16_t(u16(offset + 6)));
        }
    }
    SDL_CloseIO(io);
    if (unitsPerEm == 0 || ascender - descender <= 0) return 1.0f;
    return std::clamp(float(ascender - descender) / float(unitsPerEm), 1.0f, 2.0f);
}

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
                    merge.ExtraSizeScale = fontEmScale(path);     // its em the size of the text around it
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
