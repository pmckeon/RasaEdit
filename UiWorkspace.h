#pragma once

#include "UiCanvas.h"
#include "Workspace.h"
#include "ui.h"

#include <memory>
#include <string>
#include <vector>

// The UI workspace: the game's UI layout export (data\xml\Tabula_Rasa_UI_EXPORT.xml) - its <Widgets> tree, a canvas that draws
// the chosen branch where it sits on the 1280 x 960 logical screen (UiCanvas), and the chosen element's attributes. Read-only.
// Choosing an element in the tree draws its branch; a click on the canvas selects the widget under the cursor within that
// branch (double-click: draw the selected widget's branch instead).
class UiWorkspace : public Workspace {
public:
    explicit UiWorkspace(EditorContext& ctx);
    ~UiWorkspace() override;

    const char* name() const override { return "UI"; }
    const char* dockspaceName() const override { return "UIDockSpace"; }
    void buildDefaultLayout(ImGuiID dockspace) override;
    void beginFrame() override;
    void drawPanels() override;
    void renderViewport(SDL_GPUCommandBuffer*) override {}
    void onGameOpened() override;
    void releaseGpuResources();

    // For the smoke test: choose a branch by uiids from <Widgets> ("InGame/…"); false if not found.
    bool showBranch(const std::string& uiidPath);
    const std::vector<UiCanvas::DrawnWidget>& drawnWidgets() const { return drawn_; }
    const tr::UiExport* uiExport() const { return ui_.get(); }

private:
    void loadExport();
    void select(const std::vector<int32_t>& path, bool drawBranch, bool revealInTree);
    void drawTreePanel();
    void drawTreeNode(const tr::XmlNode& node, std::vector<int32_t>& path);
    void drawCanvasPanel();
    void drawElementPanel();
    std::string label(const tr::XmlNode& node) const;
    std::string pathLabel(const std::vector<int32_t>& path) const;

    std::unique_ptr<tr::UiExport> ui_;
    std::string loadError_;
    UiCanvas canvas_;
    UiCanvas::Options options_;
    std::vector<int32_t> branch_;           // the branch drawn on the canvas
    std::vector<int32_t> selected_;         // the element shown in the inspector and outlined
    bool hasBranch_ = false;
    bool hasSelection_ = false;
    bool dirty_ = true;
    bool revealSelection_ = false;          // open the tree down to the selection and scroll to it, once
    std::vector<uint8_t> rgba_;
    std::vector<UiCanvas::DrawnWidget> drawn_;
    SDL_GPUTexture* texture_ = nullptr;
    int zoom_ = 0;                          // 0 fit, 1 50 %, 2 100 %, 3 200 %
    int screen_ = 0;                        // the screen-size choice (Options::screenWidth / screenHeight)
    char filter_[128] = {};
};
