#pragma once

// The Client workspace's "Blender" panel: models out to glTF for Blender and back (ModelExchange.h), a preview of an import in the
// editor (the imported files override the archives' in the asset library), and the patch archive the game reads them from.

#include "ModelExchange.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

class AssetsPanel;
class EditorContext;

class BlenderPanel {
public:
    // suggestedModel: the selected map entity's model (offered for export); assets: to show an imported model in its preview.
    void draw(EditorContext& ctx, const char* title, const std::string& suggestedModel, AssetsPanel& assets);

private:
    void drawExport(EditorContext& ctx, const std::string& suggestedModel);
    void drawImport(EditorContext& ctx, AssetsPanel& assets);
    void drawPatchArchive(EditorContext& ctx);

    // Export.
    char exportModel_[256] = "";
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
