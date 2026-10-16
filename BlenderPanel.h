#pragma once

// The Assets workspace's "Blender" panel: models out to glTF for Blender and back (ModelExchange.h), a preview of an import in the
// editor (the imported files override the archives' in the asset library), and the patch archive the game reads them from.

#include "ModelAnimations.h"
#include "ModelExchange.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class EditorContext;

class BlenderPanel {
public:
    // selectedModel: the model to export (the Assets browser's selection, "" = none); showAsset: shows an imported model in the
    // preview.
    // links: what the selected model is and the animations tied to it (offered as checkboxes; nullptr = none).
    void draw(EditorContext& ctx, const char* title, const std::string& selectedModel, const ModelAnimationLinks* links,
              const std::function<void(const std::string&)>& showAsset);

private:
    void drawExport(EditorContext& ctx, const std::string& selectedModel, const ModelAnimationLinks* links);
    void drawImport(EditorContext& ctx, const std::function<void(const std::string&)>& showAsset);
    void drawPatchArchive(EditorContext& ctx);

    // Export.
    std::string exportModel_;
    std::string exportModelFor_;                     // the model the animation list was made for
    std::vector<std::string> exportAnimations_;
    char exportFolder_[512] = "";
    char animationFilter_[128] = "";
    std::vector<std::string> allAnimations_;         // every .anm (search list), read on first use
    bool exportCollision_ = true;
    int lowDetail_ = 0;                               // 0 hidden collection, 1 left out, 2 shown
    std::vector<std::string> exportLog_;

    // Import.
    char importPath_[512] = "";
    char importModelName_[256] = "";
    char importSource_[256] = "";
    bool renameChangedTextures_ = false;
    bool generateCollision_ = true;
    std::optional<ModelImportResult> imported_;
    std::string importedFrom_;
    std::vector<std::string> log_;
    bool previewing_ = false;

    // File dialogs answer on another thread.
    std::mutex dialogMutex_;
    std::string dialogImportPath_;
    std::string dialogExportFolder_;
    static void importDialogCallback(void* userdata, const char* const* files, int filter);
    static void exportDialogCallback(void* userdata, const char* const* files, int filter);
};
