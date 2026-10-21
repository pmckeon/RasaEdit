#pragma once

#include "AssetPreview.h"
#include "BlenderPanel.h"
#include "ModelAnimations.h"
#include "ModelExchange.h"
#include "Workspace.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Every file the game reads (the mounted archives and indexed loose folders, in the game's lookup order), independent of any map:
// a browser (search, by kind), the preview, the file's details and exchange - any file out as it is or back in from a file, textures
// out as PNG and back (re-encoded in the original's format), models to and from Blender (BlenderPanel). What comes back is previewed
// through the asset library's overrides and goes to the game through the patch archive.
class AssetsWorkspace : public Workspace {
public:
    explicit AssetsWorkspace(EditorContext& ctx) : Workspace(ctx) {}
    const char* name() const override { return "Assets"; }
    const char* dockspaceName() const override { return "AssetsDockSpace"; }
    void buildDefaultLayout(ImGuiID dockspace) override;
    void drawPanels() override;
    void renderViewport(SDL_GPUCommandBuffer* commands) override { preview_.render(ctx_, commands); }
    void releaseGpuResources() { preview_.release(); }
    void onGameOpened() override;
    void showAsset(const std::string& name);         // selects it (the list re-read: new names included) and previews it
    const std::string& selected() const { return selected_; }

private:
    void readNames();
    void rebuild();
    void select(const std::string& name);
    void reloadSelected();                           // the same name again, read anew (an override began or ended)
    void drawBrowser();
    void drawPreview();
    void drawDetails();
    void drawPending();
    std::string whereFrom(const std::string& name) const;   // archive key, loose folder or "imported (preview)"

    std::vector<std::string> names_;                 // every asset, sorted
    struct Kind {
        std::string extension;                       // "" = all
        std::string label;
        size_t count = 0;
    };
    std::vector<Kind> kinds_;                        // by count; kinds_[0] = all
    int kind_ = 0;
    char filter_[128] = "";
    std::vector<size_t> visible_;
    std::string selected_;
    bool scrollToSelected_ = false;
    AssetPreview preview_;
    BlenderPanel blender_;
    AnimationLinkFinder animationLinks_;
    ModelAnimationLinks links_;                      // of the selected model (empty otherwise)
    bool animationModelsFinal_ = false;              // the selected .anm's model list was made with the full model index

    // Exchange: files out to a folder, files back in (pending until previewed or written to the patch archive).
    char folder_[512] = "";
    std::vector<std::string> log_;
    std::vector<ImportedFile> pending_;
    bool previewing_ = false;
    std::mutex dialogMutex_;
    std::string dialogFolder_, dialogReplace_, dialogPng_;
    static void folderDialogCallback(void* userdata, const char* const* files, int filter);
    static void replaceDialogCallback(void* userdata, const char* const* files, int filter);
    static void pngDialogCallback(void* userdata, const char* const* files, int filter);
};
