#include "UiWorkspace.h"

#include "ClientWorkspace.h"
#include "GpuTexture.h"

#include "imgui_internal.h"   // DockBuilder

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <sstream>

UiWorkspace::UiWorkspace(EditorContext& ctx) : Workspace(ctx) {}

UiWorkspace::~UiWorkspace() = default;

void UiWorkspace::releaseGpuResources() {
    if (texture_) SDL_ReleaseGPUTexture(ctx_.device, texture_);
    texture_ = nullptr;
    if (previewTexture_) SDL_ReleaseGPUTexture(ctx_.device, previewTexture_);
    previewTexture_ = nullptr;
    previewLoaded_.clear();
}

void UiWorkspace::buildDefaultLayout(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    // Every split first, then the windows into the leaf nodes (a node that is split afterwards cannot hold its windows).
    ImGuiID left = 0, centre = 0, right = 0, leftBottom = 0, bottom = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.22f, &left, &centre);
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.26f, &right, &centre);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.35f, &leftBottom, &left);
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.22f, &bottom, &centre);
    ImGui::DockBuilderDockWindow("Widgets###UI.Widgets", left);
    ImGui::DockBuilderDockWindow("Toolbox###UI.Toolbox", leftBottom);
    ImGui::DockBuilderDockWindow("Canvas###UI.Canvas", centre);
    ImGui::DockBuilderDockWindow("Element###UI.Element", right);
    ImGui::DockBuilderDockWindow("Definitions###UI.Definitions", right);
    ImGui::DockBuilderDockWindow("Problems###UI.Problems", bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void UiWorkspace::onGameOpened() { loadExport(); }

void UiWorkspace::loadExport() {
    doc_.reset();
    issues_.clear();
    originalErrors_.clear();
    validatedRevision_ = size_t(-1);
    editBuffers_.clear();
    loadError_.clear();
    branch_.clear();
    selected_.clear();
    extraSelected_.clear();
    hasBranch_ = hasSelection_ = false;
    drawn_.clear();
    dirty_ = true;
    defIndex_ = -1;
    defBuffers_.clear();
    referencesRevision_ = size_t(-1);
    if (ctx_.gameRoot.empty()) return;
    // The game reads the loose file (tabula_rasa.cfg UIXML = "data/xml/|.xml"; no archive holds it), so edits are written there.
    filePath_ = !testFilePath_.empty() ? testFilePath_ : (std::filesystem::path(ctx_.gameRoot) / "data" / "xml" / "Tabula_Rasa_UI_EXPORT.xml").string();
    try {
        doc_ = std::make_unique<tr::UiDocument>(tr::readFile(filePath_));
        seenRevision_ = doc_->revision();
    } catch (const std::exception& e) {
        loadError_ = e.what();
        doc_.reset();
    }
    canvas_.setSources(ui(), ctx_.assets.get(), ctx_.gameData.get(), ctx_.gameRoot);
    if (doc_) {
        validateNow();
        for (const auto& i : issues_)
            if (i.error) originalErrors_.insert(i.message);
    }
}

bool UiWorkspace::showBranch(const std::string& uiidPath) {
    if (!doc_) loadExport();
    if (!doc_) return false;
    std::vector<int32_t> path;
    std::stringstream parts(uiidPath);
    std::string part;
    canvas_.setTemplates(false);
    const tr::XmlNode* n = canvas_.root();
    while (std::getline(parts, part, '/')) {
        int32_t found = -1;
        for (size_t k = 0; k < n->children.size() && found < 0; ++k)
            if (n->children[k].attribute("uiid") == part) found = int32_t(k);
        if (found < 0) return false;
        path.push_back(found);
        n = &n->children[size_t(found)];
    }
    select(path, true, true);
    return true;
}

void UiWorkspace::select(const std::vector<int32_t>& path, bool drawBranch, bool revealInTree) {
    selected_ = path;
    hasSelection_ = true;
    extraSelected_.clear();
    if (drawBranch) {
        branch_ = path;
        hasBranch_ = true;
        dirty_ = true;
    }
    if (revealInTree) revealSelection_ = true;
}

void UiWorkspace::beginFrame() {
    if (!ctx_.gameRoot.empty() && !doc_ && loadError_.empty()) loadExport();
    // RASAEDIT_UI_SELECT=<uiid>/<uiid>/...: start with that element selected, drawn in its window (previews and captures).
    static bool preselected = false;
    if (!preselected && doc_) {
        preselected = true;
        if (const char* show = std::getenv("RASAEDIT_UI_SELECT"); show && *show && showBranch(show)) {
            const std::vector<int32_t> element = selected_;
            select(branchFor(element), true, false);
            select(element, false, true);
        }
        // RASAEDIT_UI_DEFINITION=<Colors|Fonts|Textures|Animations>/<id>[@zoom]: that definition shown in the Definitions panel
        // (@zoom: a texture's preview zoomed to its region).
        if (const char* def = std::getenv("RASAEDIT_UI_DEFINITION"); def && *def) {
            std::string spec = def;
            if (spec.size() > 5 && spec.compare(spec.size() - 5, 5, "@zoom") == 0) {
                spec.resize(spec.size() - 5);
                zoomPreviewToRegion_ = true;
            }
            const size_t slash = spec.find('/');
            const std::string section = spec.substr(0, slash), id = slash == std::string::npos ? "" : spec.substr(slash + 1);
            for (tr::UiSection s : {tr::UiSection::Colours, tr::UiSection::Fonts, tr::UiSection::Textures, tr::UiSection::Animations}) {
                if (section != (s == tr::UiSection::Colours ? "Colors" : s == tr::UiSection::Fonts ? "Fonts" : s == tr::UiSection::Textures ? "Textures" : "Animations"))
                    continue;
                const tr::XmlNode* root = doc_->node(s, {});
                for (size_t k = 0; root && k < root->children.size(); ++k)
                    if (definitionId(s, k) == id) selectDefinition(s, int32_t(k));
                focusDefinitions_ = defScroll_ = true;
            }
        }
    }
    if (doc_ && doc_->revision() != seenRevision_) syncWithDocument();
    updateDefinitionPreview();
    // Validation takes a moment on the whole file: run it once edits pause.
    if (doc_ && validatedRevision_ != doc_->revision() && ImGui::GetTime() - lastEditTime_ > 0.5) validateNow();
    if (!dirty_ || !doc_ || !hasBranch_) return;
    dirty_ = false;
    drawn_ = canvas_.render(branch_, options_, rgba_);
    // Replaced between frames: no draw list of this frame references the old texture yet.
    if (texture_) SDL_ReleaseGPUTexture(ctx_.device, texture_);
    texture_ = uploadRgbaTexture(ctx_.device, rgba_.data(), uint32_t(canvas_.pixelWidth()), uint32_t(canvas_.pixelHeight()));
}

std::string UiWorkspace::label(const tr::XmlNode& node) const {
    const std::string uiid = node.attribute("uiid");
    return uiid.empty() ? node.tag : node.tag + "  " + uiid;
}

std::string UiWorkspace::pathLabel(const std::vector<int32_t>& path) const {
    std::string out;
    const tr::XmlNode* n = canvas_.root();
    for (int32_t i : path) {
        if (!n || i < 0 || size_t(i) >= n->children.size()) break;
        n = &n->children[size_t(i)];
        const std::string uiid = n->attribute("uiid");
        out += (out.empty() ? "" : " / ") + (uiid.empty() ? n->tag : uiid);
    }
    return out;
}

void UiWorkspace::drawPanels() {
    drawToolboxPanel();
    drawTreePanel();
    drawElementPanel();
    drawProblemsPanel();
    drawDefinitionsPanel();
    drawCanvasPanel();                        // last: on first creation it becomes the shared node's visible tab
}

// ------------------------------------------------------------------------------------------
// Tree
// ------------------------------------------------------------------------------------------
void UiWorkspace::drawTreeNode(const tr::XmlNode& node, std::vector<int32_t>& path) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    const bool isSelected = hasSelection_ && path == selected_;
    if (isSelected || std::find(extraSelected_.begin(), extraSelected_.end(), path) != extraSelected_.end()) flags |= ImGuiTreeNodeFlags_Selected;
    // Reveal: open every ancestor of the selection.
    if (revealSelection_ && hasSelection_ && path.size() < selected_.size() && std::equal(path.begin(), path.end(), selected_.begin()))
        ImGui::SetNextItemOpen(true);
    const bool hidden = node.attribute("visible") == "false";
    if (hidden) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    // An integer id: ImGui chains string ids by concatenation, so child "0" of child "2" collided with child "20".
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(intptr_t(path.back())), flags, "%s", label(node).c_str());
    if (hidden) ImGui::PopStyleColor();
    if (isFreeElement(path) && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("UI_ELEMENT", path.data(), path.size() * sizeof(int32_t));
        ImGui::Text("Move %s", label(node).c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        // Onto a container: becomes its last child; onto another free element: goes before it, in its parent.
        const std::string cls = uischema::elementClass(node.tag);
        const bool container = uischema::takesFreeChildren(cls);
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("UI_ELEMENT")) {
            const std::vector<int32_t> from(static_cast<const int32_t*>(payload->Data), static_cast<const int32_t*>(payload->Data) + payload->DataSize / sizeof(int32_t));
            select(from, false, false);
            if (container) moveSelectionInto(path, node.children.size(), keepPlaceOnMove_);
            else if (isFreeElement(path)) moveSelectionInto(std::vector<int32_t>(path.begin(), path.end() - 1), size_t(path.back()), keepPlaceOnMove_);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("UI_TOOLBOX")) {
            if (container) {
                select(path, false, false);
                addChild(std::string(static_cast<const char*>(payload->Data), size_t(payload->DataSize)));
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem()) {
        if (!(hasSelection_ && path == selected_)) select(path, true, false);
        const std::string cls = uischema::elementClass(node.tag);
        if (uischema::takesFreeChildren(cls) && ImGui::BeginMenu("Add child")) {
            for (const char* kind : {"WidgetComposite", "Window", "Bitmap", "Text", "ColoredRect"})
                if (ImGui::MenuItem(kind)) addChild(kind);
            ImGui::EndMenu();
        }
        const bool free = isFreeElement(path);
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, free)) cutSelection();
        if (ImGui::MenuItem("Copy", "Ctrl+C")) copySelection();
        if (ImGui::MenuItem("Paste", "Ctrl+V")) pasteClipboard();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, free)) duplicateSelection();
        if (ImGui::MenuItem("Move up", "Alt+Up", false, free && path.back() > 0)) moveSelection(-1);
        if (ImGui::MenuItem("Move down", "Alt+Down", false, free)) moveSelection(1);
        if (ImGui::MenuItem("Delete", "Del", false, free)) deleteSelection();
        if (!free) ImGui::TextDisabled("A %s's fixed part: edit its attributes only.", uischema::elementClass(canvas_.node(std::vector<int32_t>(path.begin(), path.end() - 1))->tag).c_str());
        ImGui::EndPopup();
    }
    if (isSelected && revealSelection_) {
        ImGui::SetScrollHereY(0.4f);
        revealSelection_ = false;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyCtrl) toggleSelect(path);
        else select(path, true, false);
    }
    if (open && !node.children.empty()) {
        for (size_t k = 0; k < node.children.size(); ++k) {
            path.push_back(int32_t(k));
            drawTreeNode(node.children[k], path);
            path.pop_back();
        }
        ImGui::TreePop();
    }
}

void UiWorkspace::drawTreePanel() {
    if (!ImGui::Begin("Widgets###UI.Widgets")) {
        ImGui::End();
        return;
    }
    if (ctx_.gameRoot.empty()) {
        ImGui::TextDisabled("Open a game folder first (File > Open Game Folder).");
        ImGui::End();
        return;
    }
    if (!doc_) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Could not read Tabula_Rasa_UI_EXPORT.xml");
        ImGui::TextWrapped("%s", loadError_.c_str());
        ImGui::End();
        return;
    }
    // The screen's widgets, or the templates the scripts instantiate (Definitions/TemplateWidgets).
    int source = canvas_.templates() ? 1 : 0;
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##source", &source, "Widgets\0Templates\0") && (source == 1) != canvas_.templates()) {
        canvas_.setTemplates(source == 1);
        branch_.clear();
        selected_.clear();
        extraSelected_.clear();
        hasBranch_ = hasSelection_ = false;
        drawn_.clear();
        dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Keep place", &keepPlaceOnMove_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Dragging an element into another container keeps it where it is drawn (new anchor offsets).\n"
                          "Off: it keeps its offsets, now relative to the new parent.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter by uiid or element", filter_, sizeof filter_);
    ImGui::Separator();
    ImGui::BeginChild("tree");
    const std::string filter = filter_;
    if (filter.empty()) {
        std::vector<int32_t> path;
        const tr::XmlNode& root = *canvas_.root();
        for (size_t k = 0; k < root.children.size(); ++k) {
            path.assign(1, int32_t(k));
            drawTreeNode(root.children[k], path);
        }
    } else {
        // Matches as a flat list of paths.
        auto lowerOf = [](std::string s) {
            for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string needle = lowerOf(filter);
        size_t shown = 0;
        std::vector<int32_t> path;
        std::function<void(const tr::XmlNode&)> visit = [&](const tr::XmlNode& n) {
            for (size_t k = 0; k < n.children.size() && shown < 1000; ++k) {
                path.push_back(int32_t(k));
                const tr::XmlNode& c = n.children[k];
                if (lowerOf(c.attribute("uiid")).find(needle) != std::string::npos || lowerOf(c.tag).find(needle) != std::string::npos) {
                    const bool isSelected = hasSelection_ && path == selected_;
                    ImGui::PushID(&c);
                    if (ImGui::Selectable(pathLabel(path).c_str(), isSelected)) select(path, true, false);
                    ImGui::PopID();
                    ++shown;
                }
                visit(c);
                path.pop_back();
            }
        };
        visit(*canvas_.root());
        if (shown >= 1000) ImGui::TextDisabled("(first 1000 matches)");
    }
    ImGui::EndChild();
    ImGui::End();
}

// ------------------------------------------------------------------------------------------
// Canvas
// ------------------------------------------------------------------------------------------
void UiWorkspace::drawCanvasPanel() {
    if (!ImGui::Begin("Canvas###UI.Canvas")) {
        ImGui::End();
        return;
    }
    UiCanvas::Options before = options_;
    ImGui::Checkbox("Hidden widgets", &options_.showHidden);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draw widgets with visible=\"false\" inside the branch (the branch itself is always drawn)");
    ImGui::SameLine();
    int mode = interactive_ ? 1 : 0;
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("##mode", &mode, "Edit\0Interactive\0") && (mode == 1) != interactive_) {
        interactive_ = mode == 1;
        toggled_.clear();
        pushed_ = nullptr;
        options_.interactiveButtons = interactive_;
        options_.hoveredButton = options_.pushedButton = nullptr;
        options_.toggledButtons = &toggled_;
        options_.mouseHighlight = interactive_;
        options_.animTime = interactive_ ? 0.0f : -1.0f;
        if (interactive_) options_.mouseX = options_.mouseY = -10000.0f;     // until the mouse is over the canvas
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Edit: every widget as exported - no animations, no mouse effects; a click selects the widget under it.\n"
                          "Interactive: animations play, the window wires glow around the mouse, buttons highlight, push and toggle\n"
                          "under it as in game.");
    if (interactive_) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart animations")) options_.animTime = 0.0f;
        options_.animTime += ImGui::GetIO().DeltaTime * 1000.0f;
        if (!canvas_.animating()) before.animTime = options_.animTime;      // nothing moves: no redraw for the clock alone
    } else {
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &snap_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moving and resizing snap to the edges and centres of the siblings and the parent");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        int gridChoice = grid_ == 4 ? 1 : grid_ == 8 ? 2 : grid_ == 16 ? 3 : 0;
        if (ImGui::Combo("##grid", &gridChoice, "No grid\0Grid 4\0Grid 8\0Grid 16\0")) grid_ = gridChoice == 1 ? 4 : gridChoice == 2 ? 8 : gridChoice == 3 ? 16 : 0;
        ImGui::SameLine();
        if (ImGui::Button("Arrange")) ImGui::OpenPopup("arrange");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Align, size and space the selected elements (Ctrl+click adds to the selection; the first one chosen is the\n"
                              "primary, which the others follow)");
        if (ImGui::BeginPopup("arrange")) {
            drawArrangeMenu();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("##buttons", &options_.buttonState, "Buttons as exported\0Buttons up\0Buttons highlighted\0Buttons down\0Buttons disabled\0Buttons pressed\0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every button's state (its texture, text colour and text offset, as the game applies them).\n"
                          "As exported: the export's own children (Up, a few Disabled).");
    ImGui::SameLine();
    ImGui::Checkbox("Toggled on", &options_.toggledOn);
    ImGui::SameLine();
    bool ownScroll = options_.scroll < 0;
    if (ImGui::Checkbox("Thumbs as exported", &ownScroll)) options_.scroll = ownScroll ? -1.0f : 0.0f;
    if (!ownScroll) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderFloat("##scroll", &options_.scroll, 0.0f, 1.0f, "%.2f");
    }
    ImGui::SameLine();
    bool ownProgress = options_.progress < 0;
    if (ImGui::Checkbox("Bars as exported", &ownProgress)) options_.progress = ownProgress ? -1.0f : 0.5f;
    if (!ownProgress) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderFloat("##progress", &options_.progress, 0.0f, 1.0f, "%.2f");
    }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##background", &options_.background, "Black\0Grey\0Checkerboard\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::Combo("##zoom", &zoom_, "Fit\0" "50%\0" "100%\0" "200%\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    // The screen the UI is drawn for: the export's 1280 x 960 at one pixel per unit, or a real screen size (wider than 1.5 the
    // game's root is 1706 x 960 logical; text is rasterised at the screen's pixels).
    static const int32_t screens[][2] = {{0, 0}, {1280, 960}, {1600, 1200}, {1280, 720}, {1920, 1080}, {1920, 1200}, {2560, 1440}};
    if (ImGui::Combo("##screen", &screen_, "Logical 1280 x 960\0" "1280 x 960 (4:3)\0" "1600 x 1200 (4:3)\0" "1280 x 720 (16:9)\0"
                     "1920 x 1080 (16:9)\0" "1920 x 1200 (16:10)\0" "2560 x 1440 (16:9)\0")) {
        options_.screenWidth = screens[screen_][0];
        options_.screenHeight = screens[screen_][1];
        dirty_ = true;
    }
    if (options_.showHidden != before.showHidden || options_.mouseHighlight != before.mouseHighlight || options_.background != before.background ||
        options_.interactiveButtons != before.interactiveButtons ||
        options_.buttonState != before.buttonState || options_.toggledOn != before.toggledOn || options_.progress != before.progress || options_.scroll != before.scroll ||
        options_.animTime != before.animTime)
        dirty_ = true;
    if (doc_ && hasBranch_) {
        size_t visual = 0;
        for (const auto& d : drawn_) visual += d.visual ? 1 : 0;
        ImGui::TextDisabled("%s: %zu widgets, %zu drawing", pathLabel(branch_).c_str(), drawn_.size(), visual);
        const size_t missing = canvas_.missingTextures().size() + canvas_.missingFonts().size() + canvas_.missingTextIds().size();
        if (missing > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(%zu unresolved)", missing);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                for (const auto& t : canvas_.missingTextures()) ImGui::Text("texture %s", t.c_str());
                for (const auto& f : canvas_.missingFonts()) ImGui::Text("font face %s", f.c_str());
                size_t shown = 0;
                for (const auto& t : canvas_.missingTextIds())
                    if (shown++ < 30) ImGui::Text("text id %s", t.c_str());
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::Separator();
    if (!doc_ || !texture_ || !hasBranch_) {
        ImGui::TextDisabled(doc_ ? "Choose an element in the Widgets tree." : "No UI export loaded.");
        ImGui::End();
        return;
    }
    const float w = float(canvas_.pixelWidth()), h = float(canvas_.pixelHeight());
    const float sx = float(canvas_.pixelWidth()) / float(std::max(1, canvas_.width())), sy = float(canvas_.pixelHeight()) / float(std::max(1, canvas_.height()));
    ImGui::BeginChild("canvas", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    float scale = zoom_ == 1 ? 0.5f : zoom_ == 2 ? 1.0f : zoom_ == 3 ? 2.0f : std::max(0.05f, std::min(avail.x / w, avail.y / h));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)texture_), ImVec2(w * scale, h * scale));
    const bool hovered = ImGui::IsItemHovered();
    // Toolbox drops onto the canvas (the drop target must follow the image item).
    if (!interactive_ && ImGui::BeginDragDropTarget()) {
        const ImVec2 m = ImGui::GetMousePos();
        const int32_t x = int32_t((m.x - origin.x) / scale / sx), y = int32_t((m.y - origin.y) / scale / sy);
        dropTarget_ = containerAt(x, y);
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("UI_TOOLBOX")) {
            dropNewElement(std::string(static_cast<const char*>(payload->Data), size_t(payload->DataSize)), x, y);
            dropTarget_.clear();
        }
        ImGui::EndDragDropTarget();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto outline = [&](const UiCanvas::Rect& r, ImU32 colour, float thickness) {
        dl->AddRect(ImVec2(origin.x + float(r.x0) * sx * scale, origin.y + float(r.y0) * sy * scale), ImVec2(origin.x + float(r.x1) * sx * scale, origin.y + float(r.y1) * sy * scale),
                    colour, 0.0f, 0, thickness);
    };
    if (hovered) {
        const ImVec2 m = ImGui::GetMousePos();
        const int32_t x = int32_t((m.x - origin.x) / scale / sx), y = int32_t((m.y - origin.y) / scale / sy);    // logical units
        // The mouse highlight follows the cursor (redrawn when it moves 4 units or more).
        if (options_.mouseHighlight && (std::fabs(float(x) - options_.mouseX) >= 4.0f || std::fabs(float(y) - options_.mouseY) >= 4.0f)) {
            options_.mouseX = float(x);
            options_.mouseY = float(y);
            dirty_ = true;
        }
        const std::vector<int32_t> under = canvas_.pick(x, y);
        if (interactive_) {
            // Buttons respond to the mouse: hover highlights, the button pressed stays pushed while the mouse is down on it, a release
            // over it toggles a toggle button.
            const tr::XmlNode* button = canvas_.buttonAt(x, y);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pushed_ = button;
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (pushed_ && pushed_ == button && button->attribute("enabled") == "true" && (button->tag == "Button" || button->tag == "ToggleButton")) {
                    if (!toggled_.erase(button)) toggled_.insert(button);
                    dirty_ = true;
                }
                pushed_ = nullptr;
            }
            const tr::XmlNode* pushedNow = ImGui::IsMouseDown(ImGuiMouseButton_Left) ? pushed_ : nullptr;
            if (button != options_.hoveredButton || pushedNow != options_.pushedButton) {
                options_.hoveredButton = button;
                options_.pushedButton = pushedNow;
                dirty_ = true;
            }
        } else {
            // Designer: a press on a selection handle resizes, inside the selection moves (Alt: select instead), elsewhere selects.
            const tr::XmlNode* sel = hasSelection_ ? canvas_.node(selected_) : nullptr;
            const bool resizable = sel && !selected_.empty() && sel->attribute("resizeToParent") != "true";
            int handle = -1;
            if (resizable && !dragging_) {
                const UiCanvas::Rect r = canvas_.layout(selected_);
                const float hx[8] = {float(r.x0), float(r.x0 + r.x1) / 2, float(r.x1), float(r.x1), float(r.x1), float(r.x0 + r.x1) / 2, float(r.x0), float(r.x0)};
                const float hy[8] = {float(r.y0), float(r.y0), float(r.y0), float(r.y0 + r.y1) / 2, float(r.y1), float(r.y1), float(r.y1), float(r.y0 + r.y1) / 2};
                for (int k = 0; k < 8 && handle < 0; ++k)
                    if (std::fabs(origin.x + hx[k] * sx * scale - m.x) <= 5 && std::fabs(origin.y + hy[k] * sy * scale - m.y) <= 5) handle = k;
                if (handle >= 0) ImGui::SetMouseCursor(handle == 1 || handle == 5 ? ImGuiMouseCursor_ResizeNS : handle == 3 || handle == 7 ? ImGuiMouseCursor_ResizeEW
                                                       : handle == 0 || handle == 4 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
            }
            bool insideSelection = false;
            if (sel && !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyCtrl)
                for (const auto& p : selection())
                    if (!p.empty() && canvas_.layout(p).contains(x, y)) insideSelection = true;
            // An element inside a selected window / composite is dragged itself, not its container: the free element under the
            // cursor (an owned part, such as a button's bitmap, stands for its control) when it lies below a selected element.
            std::vector<int32_t> child = under;
            while (child.size() > 1 && !isFreeElement(child)) child.pop_back();
            bool dragChild = false;
            if (insideSelection && isFreeElement(child)) {
                const std::vector<std::vector<int32_t>> current = selection();
                if (std::find(current.begin(), current.end(), child) == current.end())
                    for (const auto& p : current)
                        if (!p.empty() && p.size() < child.size() && std::equal(p.begin(), p.end(), child.begin())) dragChild = true;
            }
            if (!under.empty() && !dragging_ && handle < 0) {
                outline(canvas_.layout(dragChild ? child : under), IM_COL32(255, 255, 255, 140), 1.0f);
                ImGui::SetTooltip("%s\n(%d, %d)%s", pathLabel(dragChild ? child : under).c_str(), x, y,
                                  dragChild ? "\ndrag: move this element" : insideSelection ? "\ndrag: move the selection" : "");
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (handle < 0 && dragChild) {
                    select(child, false, true);
                    dragging_ = true;
                    dragHandle_ = -1;
                    dragStart_ = m;
                    dragStartRect_ = canvas_.layout(selected_);
                } else if (handle >= 0 || insideSelection) {
                    dragging_ = true;
                    dragHandle_ = handle;
                    dragStart_ = m;
                    dragStartRect_ = canvas_.layout(selected_);
                } else if (!under.empty()) {
                    if (ImGui::GetIO().KeyCtrl) toggleSelect(under);
                    else select(under, false, true);
                }
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !under.empty() && handle < 0 && !ImGui::GetIO().KeyCtrl) select(under, true, true);
        }
    }
    if (!ImGui::GetDragDropPayload()) dropTarget_.clear();
    if (!dropTarget_.empty() && canvas_.node(dropTarget_)) outline(canvas_.layout(dropTarget_), IM_COL32(80, 200, 255, 255), 2.0f);
    if (dragging_) {
        const ImVec2 m = ImGui::GetMousePos();
        const float dx = (m.x - dragStart_.x) / scale / sx, dy = (m.y - dragStart_.y) / scale / sy;
        const int hd = dragHandle_;
        UiCanvas::Rect r = dragStartRect_;
        const bool moveX0 = hd < 0 || hd == 0 || hd == 6 || hd == 7, moveX1 = hd < 0 || hd == 2 || hd == 3 || hd == 4;
        const bool moveY0 = hd < 0 || hd == 0 || hd == 1 || hd == 2, moveY1 = hd < 0 || hd == 4 || hd == 5 || hd == 6;
        const int32_t ix = int32_t(std::lround(dx)), iy = int32_t(std::lround(dy));
        if (moveX0) r.x0 += ix;
        if (moveX1) r.x1 += ix;
        if (moveY0) r.y0 += iy;
        if (moveY1) r.y1 += iy;
        r = snapRect(r, moveX0, moveX1, moveY0, moveY1, sx * scale);
        if (hd >= 0) {                                        // at least one unit across
            if (r.x1 <= r.x0) (moveX0 ? r.x0 : r.x1) = moveX0 ? r.x1 - 1 : r.x0 + 1;
            if (r.y1 <= r.y0) (moveY0 ? r.y0 : r.y1) = moveY0 ? r.y1 - 1 : r.y0 + 1;
        }
        const tr::XmlNode* n = canvas_.node(selected_);
        int32_t ox = 0, oy = 0;
        canvas_.offsetsFor(selected_, r, ox, oy);
        const auto attr = [&](const char* name) { return n ? int32_t(std::strtol(n->attribute(name).c_str(), nullptr, 10)) : 0; };
        const int32_t curX = attr("anchorOffsetX"), curY = attr("anchorOffsetY"), curW = attr("sizeW"), curH = attr("sizeH");
        const int32_t newW = hd >= 0 ? r.width() : curW, newH = hd >= 0 ? r.height() : curH;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            dragging_ = false;
            guides_.clear();
            options_.dragNode = nullptr;
            options_.dragX = options_.dragY = options_.dragW = options_.dragH = 0;
            dirty_ = true;
            if (n && (ox != curX || oy != curY || newW != curW || newH != curH)) {
                // One edit: offsets at 0 and sizes at 0 are left out, as the export writes them.
                struct Change {
                    std::vector<int32_t> path;
                    std::vector<std::pair<std::string, std::string>> set;
                    std::vector<std::string> remove;
                };
                std::vector<Change> changes(1);
                changes[0].path = selected_;
                const auto put = [&](const char* name, int32_t v) { v == 0 ? changes[0].remove.push_back(name) : changes[0].set.push_back({name, std::to_string(v)}); };
                put("anchorOffsetX", ox);
                put("anchorOffsetY", oy);
                if (hd >= 0) {
                    put("sizeW", newW);
                    put("sizeH", newH);
                } else {
                    // Moving: the other selected elements by the same amount (worked out before the first edit re-parses the document).
                    const int32_t mx = r.x0 - dragStartRect_.x0, my = r.y0 - dragStartRect_.y0;
                    for (const auto& p : movableSelection()) {
                        if (p == selected_) continue;
                        UiCanvas::Rect pr = canvas_.layout(p);
                        pr.x0 += mx; pr.x1 += mx; pr.y0 += my; pr.y1 += my;
                        Change c{p, {}, {}};
                        if (placementEdit(p, pr, c.set, c.remove)) changes.push_back(std::move(c));
                    }
                }
                runEdit([&] {
                    doc_->beginGroup();
                    try {
                        for (const auto& c : changes) doc_->setAttributes(canvas_.templates(), c.path, c.set, c.remove);
                    } catch (...) {
                        doc_->endGroup();
                        throw;
                    }
                    doc_->endGroup();
                });
            }
        } else if (n && (ox - curX != options_.dragX || oy - curY != options_.dragY || newW - curW != options_.dragW || newH - curH != options_.dragH)) {
            options_.dragNode = n;
            options_.dragX = ox - curX;
            options_.dragY = oy - curY;
            options_.dragW = newW - curW;
            options_.dragH = newH - curH;
            dirty_ = true;
        }
        if (hd < 0 && dragging_) {
            const int32_t mx = r.x0 - dragStartRect_.x0, my = r.y0 - dragStartRect_.y0;
            for (const auto& p : movableSelection()) {
                if (p == selected_) continue;
                UiCanvas::Rect pr = canvas_.layout(p);
                pr.x0 += mx; pr.x1 += mx; pr.y0 += my; pr.y1 += my;
                outline(pr, IM_COL32(255, 220, 60, 160), 1.0f);
            }
        }
        for (const auto& [vertical, at] : guides_) {
            if (vertical) dl->AddLine(ImVec2(origin.x + float(at) * sx * scale, origin.y), ImVec2(origin.x + float(at) * sx * scale, origin.y + float(canvas_.pixelHeight()) * scale),
                                      IM_COL32(255, 80, 200, 200));
            else dl->AddLine(ImVec2(origin.x, origin.y + float(at) * sy * scale), ImVec2(origin.x + float(canvas_.pixelWidth()) * scale, origin.y + float(at) * sy * scale),
                             IM_COL32(255, 80, 200, 200));
        }
    }
    if (!interactive_ && hasSelection_ && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
        const int step = ImGui::GetIO().KeyShift ? 10 : 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nudgeSelection(-step, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nudgeSelection(step, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !ImGui::GetIO().KeyAlt) nudgeSelection(0, -step);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !ImGui::GetIO().KeyAlt) nudgeSelection(0, step);
    }
    if (interactive_ && !hovered && (options_.hoveredButton || options_.mouseX > -10000.0f)) {
        // The mouse left the canvas: no hover, no glow.
        options_.hoveredButton = options_.pushedButton = nullptr;
        options_.mouseX = options_.mouseY = -10000.0f;
        dirty_ = true;
    }
    for (const auto& p : extraSelected_)
        if (canvas_.node(p)) outline(canvas_.layout(p), IM_COL32(255, 160, 40, 255), 1.5f);
    if (hasSelection_) {
        const UiCanvas::Rect r = canvas_.layout(selected_);
        outline(r, IM_COL32(255, 220, 60, 255), 2.0f);
        const tr::XmlNode* sel = canvas_.node(selected_);
        if (!interactive_ && sel && !selected_.empty()) {
            const auto toScreen = [&](float lx, float ly) { return ImVec2(origin.x + lx * sx * scale, origin.y + ly * sy * scale); };
            // Handles (resizable unless resizeToParent).
            if (sel->attribute("resizeToParent") != "true") {
                const float hx[8] = {float(r.x0), float(r.x0 + r.x1) / 2, float(r.x1), float(r.x1), float(r.x1), float(r.x0 + r.x1) / 2, float(r.x0), float(r.x0)};
                const float hy[8] = {float(r.y0), float(r.y0), float(r.y0), float(r.y0 + r.y1) / 2, float(r.y1), float(r.y1), float(r.y1), float(r.y0 + r.y1) / 2};
                for (int k = 0; k < 8; ++k) {
                    const ImVec2 c = toScreen(hx[k], hy[k]);
                    dl->AddRectFilled(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y + 3), IM_COL32(255, 255, 255, 255));
                    dl->AddRect(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y + 3), IM_COL32(40, 40, 40, 255));
                }
            }
            // Anchors: the parent's anchor point (diamond) joined to the element's own anchor point (circle).
            const std::vector<int32_t> parentPath(selected_.begin(), selected_.end() - 1);
            const UiCanvas::Rect pr = canvas_.layout(parentPath);
            const auto anchorAt = [](const UiCanvas::Rect& rect, long a) {
                const float xs[9] = {0, 0.5f, 1, 1, 1, 0.5f, 0, 0, 0.5f}, ys[9] = {0, 0, 0, 0.5f, 1, 1, 1, 0.5f, 0.5f};
                if (a < 0 || a > 8) a = 0;
                return std::pair<float, float>(float(rect.x0) + xs[a] * float(rect.width()), float(rect.y0) + ys[a] * float(rect.height()));
            };
            const auto [pax, pay] = anchorAt(pr, std::strtol(sel->attribute("parentAnchor").c_str(), nullptr, 10));
            const auto [cax, cay] = anchorAt(r, std::strtol(sel->attribute("childAnchor").c_str(), nullptr, 10));
            const ImVec2 p = toScreen(pax, pay), c = toScreen(cax, cay);
            dl->AddLine(p, c, IM_COL32(80, 255, 120, 200), 1.0f);
            dl->AddQuadFilled(ImVec2(p.x, p.y - 5), ImVec2(p.x + 5, p.y), ImVec2(p.x, p.y + 5), ImVec2(p.x - 5, p.y), IM_COL32(80, 255, 120, 255));
            dl->AddCircleFilled(c, 3.5f, IM_COL32(80, 255, 120, 255));
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ------------------------------------------------------------------------------------------
// Element inspector
// ------------------------------------------------------------------------------------------
void UiWorkspace::drawElementPanel() {
    if (!ImGui::Begin("Element###UI.Element")) {
        ImGui::End();
        return;
    }
    const tr::XmlNode* n = doc_ && hasSelection_ ? canvas_.node(selected_) : nullptr;
    if (!n) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::End();
        return;
    }
    ImGui::Text("%s", n->tag.c_str());
    if (!extraSelected_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.63f, 0.16f, 1.0f), "(primary; %zu more selected)", extraSelected_.size());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const auto& p : extraSelected_) ImGui::TextUnformatted(pathLabel(p).c_str());
            ImGui::EndTooltip();
        }
    }
    ImGui::TextWrapped("%s", pathLabel(selected_).c_str());
    const UiCanvas::Rect r = canvas_.layout(selected_);
    ImGui::Text("Screen rectangle: (%d, %d) - (%d, %d), %d x %d", r.x0, r.y0, r.x1, r.y1, r.width(), r.height());
    if (selected_ != branch_ && ImGui::Button("Draw this branch")) select(selected_, true, false);
    ImGui::Text("%zu children", n->children.size());
    const std::string cls = uischema::elementClass(n->tag);
    drawStructureTools(*n, cls);
    // This element's problems.
    for (const auto& i : issues_)
        if (i.section.empty() && i.templates == canvas_.templates() && i.path == selected_)
            ImGui::TextColored(i.error ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s %s", i.error ? "Error:" : "Warning:",
                               i.message.c_str());
    ImGui::Separator();
    n = canvas_.node(selected_);              // the structure tools may have edited the document
    if (!n) {
        ImGui::End();
        return;
    }
    drawAttributeEditor(*n, uischema::elementClass(n->tag));
    n = canvas_.node(selected_);
    if (!n) {
        ImGui::End();
        return;
    }
    // Resolved references ("edit": the definition in the Definitions panel).
    const auto editLink = [&](tr::UiSection section, const std::string& id) {
        const tr::XmlNode* root = doc_->node(section, {});
        for (size_t k = 0; root && k < root->children.size(); ++k)
            if (definitionId(section, k) == id || (section == tr::UiSection::Fonts && definitionId(section, k) == "Fonts." + id)) {
                ImGui::SameLine();
                ImGui::PushID(id.c_str());
                if (ImGui::SmallButton("edit")) {
                    selectDefinition(section, int32_t(k));
                    focusDefinitions_ = defScroll_ = true;
                }
                ImGui::PopID();
                return;
            }
    };
    auto showTexture = [&](const char* key) {
        const std::string id = n->attribute(key);
        if (id.empty()) return;
        const tr::UiTextureDef* def = ui()->texture(id);
        if (!def) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s %s: no such textureDef", key, id.c_str());
            return;
        }
        ImGui::Text("%s %s: %s (%d, %d) - (%d, %d)%s%s", key, id.c_str(), def->name.c_str(), def->left, def->top, def->right, def->bottom,
                    def->tileU ? ", tile U" : "", def->tileV ? ", tile V" : "");
        editLink(tr::UiSection::Textures, id);
    };
    ImGui::Separator();
    for (const char* key : {"textureDef", "UpTextureDef", "HighlightedTextureDef", "DownTextureDef", "DisabledTextureDef"}) showTexture(key);
    if (n->hasAttribute("textId")) {
        const std::string s = canvas_.text(*n);
        ImGui::Text("Text:");
        ImGui::SameLine();
        if (s.empty()) ImGui::TextDisabled("(unknown text id)");
        else ImGui::TextWrapped("%s", s.c_str());
    }
    if (n->hasAttribute("fontId")) {
        const tr::UiFont* font = ui()->font(n->attribute("fontId"));
        if (!font) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Font %s: not defined", n->attribute("fontId").c_str());
        else {
            ImGui::Text("Font %s", n->attribute("fontId").c_str());
            editLink(tr::UiSection::Fonts, n->attribute("fontId"));
        }
        if (font)
            for (const tr::UiFontRange& range : font->ranges)
                ImGui::Text("Font %u-%u: %s %d%s%s%s", range.begin, range.end, range.face.c_str(), range.pointSize, range.shadow ? " shadow" : "",
                            range.bold ? " bold" : "", range.italic ? " italic" : "");
    }
    for (const char* key : {"widgetAnimName", "bitmapAnimName"})
        if (n->hasAttribute(key)) {
            ImGui::Text("%s %s", key, n->attribute(key).c_str());
            editLink(tr::UiSection::Animations, n->attribute(key));
        }
    ImGui::End();
}

// ------------------------------------------------------------------------------------------
// Editing
// ------------------------------------------------------------------------------------------
void UiWorkspace::runEdit(const std::function<void()>& edit) {
    if (!doc_) return;
    try {
        edit();
    } catch (const std::exception& e) {
        ctx_.setStatus(std::string("UI edit failed: ") + e.what(), true);
    }
    lastEditTime_ = ImGui::GetTime();
    syncWithDocument();
}

void UiWorkspace::syncWithDocument() {
    if (!doc_) return;
    seenRevision_ = doc_->revision();
    canvas_.setExport(ui());
    // Node pointers changed with the re-parse.
    toggled_.clear();
    pushed_ = nullptr;
    options_.hoveredButton = options_.pushedButton = nullptr;
    options_.dragNode = nullptr;
    if (hasSelection_ && !canvas_.node(selected_)) {
        hasSelection_ = false;
        selected_.clear();
    }
    if (!hasSelection_) extraSelected_.clear();
    extraSelected_.erase(std::remove_if(extraSelected_.begin(), extraSelected_.end(), [&](const auto& p) { return !canvas_.node(p); }), extraSelected_.end());
    if (hasBranch_ && !canvas_.node(branch_)) {
        hasBranch_ = false;
        branch_.clear();
    }
    editBuffers_.clear();
    dirty_ = true;
}

void UiWorkspace::validateNow() {
    if (!doc_) return;
    issues_ = uischema::validate(doc_->ui(), ctx_.assets.get(), ctx_.gameData.get());
    std::stable_sort(issues_.begin(), issues_.end(), [](const auto& a, const auto& b) { return a.error > b.error; });
    validatedRevision_ = doc_->revision();
}

bool UiWorkspace::isFreeElement(const std::vector<int32_t>& path) const {
    if (path.empty()) return false;
    if (path.size() == 1) return true;                                     // a root widget or template
    const tr::XmlNode* parent = canvas_.node(std::vector<int32_t>(path.begin(), path.end() - 1));
    return parent && uischema::takesFreeChildren(uischema::elementClass(parent->tag));
}

std::vector<int32_t> UiWorkspace::branchFor(const std::vector<int32_t>& path) const {
    // The nearest Window around the element (else its root widget), so the element is drawn where it sits.
    std::vector<int32_t> best(path.begin(), path.begin() + std::min<size_t>(path.size(), 1));
    std::vector<int32_t> p;
    for (int32_t i : path) {
        p.push_back(i);
        const tr::XmlNode* n = canvas_.node(p);
        if (n && n->tag == "Window") best = p;
    }
    return best;
}

void UiWorkspace::addChild(const std::string& kind) {
    if (!hasSelection_) return;
    const tr::XmlNode e = newElement(kind);
    if (e.tag.empty()) return;
    const std::vector<int32_t> parent = selected_;
    const tr::XmlNode* p = canvas_.node(parent);
    if (!p) return;
    const size_t index = p->children.size();
    std::vector<int32_t> added;
    runEdit([&] { added = doc_->insertElement(canvas_.templates(), parent, index, e); });
    if (!added.empty()) select(added, false, true);
}

void UiWorkspace::deleteSelection() {
    if (!hasSelection_ || !isFreeElement(selected_)) return;
    const std::vector<int32_t> path = selected_;
    // The others too (fixed parts stay); last in document order first, so the paths still to go stay valid.
    std::vector<std::vector<int32_t>> doomed;
    for (const auto& p : movableSelection())
        if (isFreeElement(p)) doomed.push_back(p);
    std::sort(doomed.begin(), doomed.end(), std::greater<>());
    runEdit([&] {
        doc_->beginGroup();
        try {
            for (const auto& p : doomed) doc_->removeElement(canvas_.templates(), p);
        } catch (...) {
            doc_->endGroup();
            throw;
        }
        doc_->endGroup();
    });
    std::vector<int32_t> parent(path.begin(), path.end() - 1);
    // Gone with a deleted ancestor: up to what is left. Then shifted where an earlier sibling of one of its ancestors went.
    for (const auto& d : doomed)
        if (d.size() <= parent.size() && std::equal(d.begin(), d.end(), parent.begin())) parent.resize(d.size() - 1);
    for (const auto& d : doomed)
        if (d.size() <= parent.size() && std::equal(d.begin(), d.end() - 1, parent.begin()) && d.back() < parent[d.size() - 1]) --parent[d.size() - 1];
    if (!parent.empty()) select(parent, false, true);
    else {
        hasSelection_ = false;
        selected_.clear();
    }
}

void UiWorkspace::duplicateSelection() {
    if (!hasSelection_ || !isFreeElement(selected_)) return;
    const std::vector<int32_t> path = selected_;
    std::vector<int32_t> copy;
    runEdit([&] { copy = doc_->duplicateElement(canvas_.templates(), path); });
    if (!copy.empty()) select(copy, false, true);
}

void UiWorkspace::moveSelection(int delta) {
    if (!hasSelection_ || !isFreeElement(selected_)) return;
    const std::vector<int32_t> path = selected_;
    std::vector<int32_t> moved = path;
    runEdit([&] { moved = doc_->moveElement(canvas_.templates(), path, delta); });
    select(moved, false, true);
}

void UiWorkspace::nudgeSelection(int dx, int dy) {
    if (!hasSelection_ || !canvas_.node(selected_)) return;
    struct Change {
        std::vector<int32_t> path;
        std::vector<std::pair<std::string, std::string>> set;
        std::vector<std::string> remove;
    };
    std::vector<Change> changes;
    for (const auto& path : movableSelection()) {
        const tr::XmlNode* n = canvas_.node(path);
        if (!n) continue;
        const auto value = [&](const char* name) { return int32_t(std::strtol(n->attribute(name).c_str(), nullptr, 10)); };
        const int32_t x = value("anchorOffsetX") + dx, y = value("anchorOffsetY") + dy;
        // Offsets at their default (0) are left out, as the export writes them.
        Change c{path, {}, {}};
        (x == 0 ? c.remove.push_back("anchorOffsetX") : c.set.push_back({"anchorOffsetX", std::to_string(x)}));
        (y == 0 ? c.remove.push_back("anchorOffsetY") : c.set.push_back({"anchorOffsetY", std::to_string(y)}));
        changes.push_back(std::move(c));
    }
    runEdit([&] {
        doc_->beginGroup();
        try {
            for (const auto& c : changes) doc_->setAttributes(canvas_.templates(), c.path, c.set, c.remove);
        } catch (...) {
            doc_->endGroup();
            throw;
        }
        doc_->endGroup();
    });
}

// ------------------------------------------------------------------------------------------
// Multiple selection: align, same size, spacing
// ------------------------------------------------------------------------------------------
void UiWorkspace::toggleSelect(const std::vector<int32_t>& path) {
    if (path.empty() || !canvas_.node(path)) return;
    if (!hasSelection_) {
        select(path, false, true);
        return;
    }
    // Only elements laid out on the canvas: align and size work in the drawn branch's coordinates.
    const auto onCanvas = [&](const std::vector<int32_t>& p) {
        return hasBranch_ && p.size() >= branch_.size() && std::equal(branch_.begin(), branch_.end(), p.begin());
    };
    if (!onCanvas(path) || !onCanvas(selected_)) {
        ctx_.setStatus("Only elements of the branch on the canvas can be selected together.", true);
        return;
    }
    if (path == selected_) {
        // The primary leaves: the next one chosen takes its place.
        if (extraSelected_.empty()) return;
        selected_ = extraSelected_.front();
        extraSelected_.erase(extraSelected_.begin());
        editBuffers_.clear();
        return;
    }
    const auto it = std::find(extraSelected_.begin(), extraSelected_.end(), path);
    if (it != extraSelected_.end()) extraSelected_.erase(it);
    else extraSelected_.push_back(path);
}

std::vector<std::vector<int32_t>> UiWorkspace::selection() const {
    std::vector<std::vector<int32_t>> all;
    if (!hasSelection_) return all;
    all.push_back(selected_);
    all.insert(all.end(), extraSelected_.begin(), extraSelected_.end());
    return all;
}

std::vector<std::vector<int32_t>> UiWorkspace::movableSelection() const {
    const std::vector<std::vector<int32_t>> all = selection();
    std::vector<std::vector<int32_t>> out;
    for (const auto& p : all) {
        bool underAnother = false;
        for (const auto& q : all)
            if (q.size() < p.size() && std::equal(q.begin(), q.end(), p.begin())) underAnother = true;
        if (!underAnother && !p.empty() && canvas_.node(p)) out.push_back(p);
    }
    return out;
}

bool UiWorkspace::placementEdit(const std::vector<int32_t>& path, const UiCanvas::Rect& r, std::vector<std::pair<std::string, std::string>>& set,
                                std::vector<std::string>& remove) const {
    const tr::XmlNode* n = canvas_.node(path);
    if (!n || path.empty()) return false;
    const UiCanvas::Rect current = canvas_.layout(path);
    int32_t ox = 0, oy = 0;
    canvas_.offsetsFor(path, r, ox, oy);
    const auto value = [&](const char* name) { return int32_t(std::strtol(n->attribute(name).c_str(), nullptr, 10)); };
    bool changed = false;
    // Values at their default (0) are left out, as the export writes them.
    const auto put = [&](const char* name, int32_t v) {
        if (v == value(name) && (v != 0 || n->attribute(name).empty())) return;
        v == 0 ? remove.push_back(name) : set.push_back({name, std::to_string(v)});
        changed = true;
    };
    put("anchorOffsetX", ox);
    put("anchorOffsetY", oy);
    if (r.width() != current.width() || r.height() != current.height()) {
        put("sizeW", r.width());
        put("sizeH", r.height());
    }
    return changed;
}

bool UiWorkspace::canArrange(Arrange op) const {
    if (interactive_ || !hasSelection_ || !doc_) return false;
    const size_t n = movableSelection().size();
    switch (op) {
        case Arrange::CentreAcross:
        case Arrange::CentreDown: return n >= 1;
        case Arrange::SpaceAcross:
        case Arrange::SpaceDown: return n >= 3;
        default: return n >= 2 && !extraSelected_.empty();
    }
}

bool UiWorkspace::arrangeSelection(Arrange op) {
    if (!canArrange(op)) return false;
    const std::vector<std::vector<int32_t>> items = movableSelection();
    std::vector<UiCanvas::Rect> rects;
    for (const auto& p : items) rects.push_back(canvas_.layout(p));
    // The primary, unless it moves with a selected ancestor: then the first movable element leads.
    size_t lead = 0;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i] == selected_) lead = i;
    const UiCanvas::Rect ref = rects[lead];
    std::vector<UiCanvas::Rect> target = rects;
    const auto moveTo = [](UiCanvas::Rect& r, int32_t x0, int32_t y0) {
        const int32_t w = r.width(), h = r.height();
        r = UiCanvas::Rect{x0, y0, x0 + w, y0 + h};
    };
    const auto resizable = [&](size_t i) {
        const tr::XmlNode* n = canvas_.node(items[i]);
        return n && n->attribute("resizeToParent") != "true";
    };
    switch (op) {
        case Arrange::Lefts: for (auto& r : target) moveTo(r, ref.x0, r.y0); break;
        case Arrange::Rights: for (auto& r : target) moveTo(r, ref.x1 - r.width(), r.y0); break;
        case Arrange::Centres: for (auto& r : target) moveTo(r, (ref.x0 + ref.x1) / 2 - r.width() / 2, r.y0); break;
        case Arrange::Tops: for (auto& r : target) moveTo(r, r.x0, ref.y0); break;
        case Arrange::Bottoms: for (auto& r : target) moveTo(r, r.x0, ref.y1 - r.height()); break;
        case Arrange::Middles: for (auto& r : target) moveTo(r, r.x0, (ref.y0 + ref.y1) / 2 - r.height() / 2); break;
        case Arrange::SameWidth:
        case Arrange::SameHeight:
        case Arrange::SameSize:
            // The top-left corner stays, as in Visual Studio; elements sized by their parent (resizeToParent) are left alone.
            for (size_t i = 0; i < target.size(); ++i) {
                if (!resizable(i)) continue;
                if (op != Arrange::SameHeight) target[i].x1 = target[i].x0 + ref.width();
                if (op != Arrange::SameWidth) target[i].y1 = target[i].y0 + ref.height();
            }
            break;
        case Arrange::SpaceAcross:
        case Arrange::SpaceDown: {
            // The outermost two stay; the others go between them with equal gaps.
            const bool across = op == Arrange::SpaceAcross;
            std::vector<size_t> order(items.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return across ? rects[a].x0 < rects[b].x0 : rects[a].y0 < rects[b].y0; });
            const UiCanvas::Rect& first = rects[order.front()];
            const UiCanvas::Rect& last = rects[order.back()];
            int64_t total = 0;
            for (size_t i : order) total += across ? rects[i].width() : rects[i].height();
            const double gap = double((across ? last.x1 - first.x0 : last.y1 - first.y0) - total) / double(order.size() - 1);
            double at = across ? first.x0 : first.y0;
            for (size_t k = 0; k < order.size(); ++k) {
                UiCanvas::Rect& r = target[order[k]];
                const int32_t pos = k + 1 == order.size() ? (across ? last.x0 : last.y0) : int32_t(std::lround(at));
                if (across) moveTo(r, pos, r.y0);
                else moveTo(r, r.x0, pos);
                at += double(across ? r.width() : r.height()) + gap;
            }
            break;
        }
        case Arrange::CentreAcross:
        case Arrange::CentreDown:
            // Each in its own parent (Visual Studio's Center in Form).
            for (size_t i = 0; i < target.size(); ++i) {
                const UiCanvas::Rect pr = canvas_.layout(std::vector<int32_t>(items[i].begin(), items[i].end() - 1));
                UiCanvas::Rect& r = target[i];
                if (op == Arrange::CentreAcross) moveTo(r, (pr.x0 + pr.x1) / 2 - r.width() / 2, r.y0);
                else moveTo(r, r.x0, (pr.y0 + pr.y1) / 2 - r.height() / 2);
            }
            break;
    }
    // Every change worked out first: the first edit re-parses the document and the canvas's nodes with it.
    struct Change {
        std::vector<int32_t> path;
        std::vector<std::pair<std::string, std::string>> set;
        std::vector<std::string> remove;
    };
    std::vector<Change> changes;
    for (size_t i = 0; i < items.size(); ++i) {
        Change c{items[i], {}, {}};
        if (placementEdit(items[i], target[i], c.set, c.remove)) changes.push_back(std::move(c));
    }
    if (changes.empty()) return false;
    runEdit([&] {
        doc_->beginGroup();
        try {
            for (const auto& c : changes) doc_->setAttributes(canvas_.templates(), c.path, c.set, c.remove);
        } catch (...) {
            doc_->endGroup();
            throw;
        }
        doc_->endGroup();
    });
    return true;
}

void UiWorkspace::drawArrangeMenu() {
    const auto item = [&](const char* label, Arrange op) {
        if (ImGui::MenuItem(label, nullptr, false, canArrange(op))) arrangeSelection(op);
    };
    ImGui::TextDisabled(extraSelected_.empty() ? "Ctrl+click more elements to align them" : "Following the primary (bright outline)");
    item("Align Lefts", Arrange::Lefts);
    item("Align Centres", Arrange::Centres);
    item("Align Rights", Arrange::Rights);
    ImGui::Separator();
    item("Align Tops", Arrange::Tops);
    item("Align Middles", Arrange::Middles);
    item("Align Bottoms", Arrange::Bottoms);
    ImGui::Separator();
    item("Make Same Width", Arrange::SameWidth);
    item("Make Same Height", Arrange::SameHeight);
    item("Make Same Size", Arrange::SameSize);
    ImGui::Separator();
    item("Space Evenly Across", Arrange::SpaceAcross);
    item("Space Evenly Down", Arrange::SpaceDown);
    ImGui::Separator();
    item("Centre in Parent Horizontally", Arrange::CentreAcross);
    item("Centre in Parent Vertically", Arrange::CentreDown);
}

void UiWorkspace::commitAttribute(const std::string& cls, const std::string& name, const std::string& value) {
    const uischema::Attribute* spec = uischema::attribute(cls, name);
    const std::vector<int32_t> path = selected_;
    if (spec && !value.empty()) {
        const std::string problem = uischema::checkValue(*spec, value, doc_->ui(), ctx_.assets.get(), ctx_.gameData.get());
        const bool warningOnly = spec->type == uischema::Type::TextId || spec->type == uischema::Type::Effect;
        if (!problem.empty() && !warningOnly) {
            ctx_.setStatus(name + "=\"" + value + "\" refused: " + problem, true);
            return;
        }
    }
    // A value equal to the constructor's default is left out (the export writes only what differs); uiid is always kept.
    if (value.empty() || (spec && !spec->defaultValue.empty() && value == spec->defaultValue && name != "uiid"))
        runEdit([&] { doc_->removeAttribute(canvas_.templates(), path, name); });
    else runEdit([&] { doc_->setAttribute(canvas_.templates(), path, name, value); });
}

void UiWorkspace::drawStructureTools(const tr::XmlNode& n, const std::string& cls) {
    if (cls.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "The game does not know <%s>.", n.tag.c_str());
        return;
    }
    const bool free = isFreeElement(selected_);
    ImGui::TextDisabled("%s%s", cls == n.tag ? cls.c_str() : (n.tag + " (a " + cls + ")").c_str(),
                        free ? "" : " - a fixed part of its parent: attributes only");
    if (uischema::takesFreeChildren(cls)) {
        if (ImGui::Button("Add child...")) ImGui::OpenPopup("addchild");
        if (ImGui::BeginPopup("addchild")) {
            for (const char* kind : {"WidgetComposite", "Window", "Bitmap", "Text", "ColoredRect"})
                if (ImGui::MenuItem(kind)) addChild(kind);
            ImGui::TextDisabled("Controls (buttons, bars, frames, ...): duplicate an existing one.");
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(!free);
    if (ImGui::Button("Duplicate")) duplicateSelection();
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_.empty() || selected_.back() == 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveSelection(-1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) moveSelection(1);
    ImGui::SameLine();
    if (ImGui::Button("Delete")) deleteSelection();
    ImGui::EndDisabled();
}

void UiWorkspace::drawAttributeEditor(const tr::XmlNode& n, const std::string& cls) {
    if (editBuffersFor_ != selected_ || editBuffersTemplates_ != canvas_.templates()) {
        editBuffers_.clear();
        colourBuffers_.clear();
        editBuffersFor_ = selected_;
        editBuffersTemplates_ = canvas_.templates();
    }
    const auto& specs = uischema::attributes(cls);
    if (!ImGui::BeginTable("attributes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) return;
    // Names and values share the width (the panel is often narrow); the reset button keeps its own.
    ImGui::TableSetupColumn("Attribute", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
    std::string group;
    // The game's uianchor names (eWidgetAnchor_TopLeft .. Center = 0 .. 8, FUN_0051f2f0); the file stores the number.
    static const char* anchorNames[] = {"Top Left", "Top", "Top Right", "Right", "Bottom Right", "Bottom", "Bottom Left", "Left", "Center"};
    static const char* anchorEnums[] = {"TopLeft", "Top", "TopRight", "Right", "BotRight", "Bot", "BotLeft", "Left", "Center"};
    for (size_t k = 0; k < specs.size(); ++k) {
        const uischema::Attribute& a = specs[k];
        if (a.group != group) {
            group = a.group;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", group.c_str());
        }
        const bool present = n.hasAttribute(a.name);
        std::string value = present ? n.attribute(a.name) : a.defaultValue;
        // A value of the wrong form is shown (and treated) as the default; "x" removes it, which gives the default in the file too.
        std::string invalidValue;
        const auto formOk = [&](const uischema::Attribute& spec, const std::string& v) {
            return !uischema::isFormatType(spec.type) || uischema::checkValue(spec, v, doc_->ui(), ctx_.assets.get(), ctx_.gameData.get()).empty();
        };
        if (present && !formOk(a, value)) {
            invalidValue = value;
            value = a.defaultValue;
        }
        ImGui::PushID(a.name.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // Colour channels R G B A of one prefix: one colour picker, written as one edit.
        const bool colourStart = a.type == uischema::Type::Byte && a.name.size() > 1 && a.name.back() == 'R' && k + 3 < specs.size() &&
                                 specs[k + 3].name == a.name.substr(0, a.name.size() - 1) + "A";
        if (colourStart) {
            const std::string prefix = a.name.substr(0, a.name.size() - 1);
            bool anyPresent = false;
            float rgba[4];
            for (int c = 0; c < 4; ++c) {
                const std::string name = prefix + "RGBA"[c];
                anyPresent |= n.hasAttribute(name);
                std::string v = n.hasAttribute(name) ? n.attribute(name) : specs[k + size_t(c)].defaultValue;
                if (!formOk(specs[k + size_t(c)], v)) v = specs[k + size_t(c)].defaultValue;    // invalid channel: the default
                rgba[c] = float(v.empty() ? 255 : std::strtol(v.c_str(), nullptr, 10)) / 255.0f;
            }
            if (anyPresent) ImGui::TextUnformatted(prefix.c_str());
            else ImGui::TextDisabled("%s", prefix.c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            // While the picker is in use its value lives here; it is written as one edit when the picker is let go.
            auto held = colourBuffers_.find(prefix);
            if (held != colourBuffers_.end()) std::copy(held->second.begin(), held->second.end(), rgba);
            float shown[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
            if (ImGui::ColorEdit4("##colour", shown, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_AlphaBar))
                colourBuffers_[prefix] = {shown[0], shown[1], shown[2], shown[3]};
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                colourBuffers_.erase(prefix);
                std::vector<std::pair<std::string, std::string>> set;
                std::vector<std::string> remove;
                for (int c = 0; c < 4; ++c)
                    set.push_back({prefix + "RGBA"[c], std::to_string(int(std::lround(std::clamp(shown[c], 0.0f, 1.0f) * 255.0f)))});
                // The export writes all four channels or none: all four at the default are left out.
                bool allDefault = true;
                for (int c = 0; c < 4; ++c) allDefault &= set[size_t(c)].second == specs[k + size_t(c)].defaultValue;
                if (allDefault) {
                    for (const auto& s : set) remove.push_back(s.first);
                    set.clear();
                }
                const std::vector<int32_t> path = selected_;
                runEdit([&] { doc_->setAttributes(canvas_.templates(), path, set, remove); });
            }
            ImGui::TableNextColumn();
            if (anyPresent && ImGui::SmallButton("x")) {
                const std::vector<int32_t> path = selected_;
                std::vector<std::string> remove;
                for (int c = 0; c < 4; ++c) remove.push_back(prefix + "RGBA"[c]);
                runEdit([&] { doc_->setAttributes(canvas_.templates(), path, {}, remove); });
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the default (white)");
            ImGui::PopID();
            k += 3;
            continue;
        }
        if (!invalidValue.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", a.name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The file has %s=\"%s\", which is not a valid value: shown as the default (%s).\n"
                                  "\"x\" removes it from the file, so the game uses the default too.",
                                  a.name.c_str(), invalidValue.c_str(), a.defaultValue.empty() ? "none" : a.defaultValue.c_str());
        } else {
            if (present) ImGui::TextUnformatted(a.name.c_str());
            else ImGui::TextDisabled("%s", a.name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("default: %s", a.defaultValue.empty() ? "(none / not traced)" : a.defaultValue.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        std::vector<std::string> choices;          // reference lists
        switch (a.type) {
            case uischema::Type::TextureDef:
                for (const auto& [id, def] : ui()->textures) choices.push_back(id);
                break;
            case uischema::Type::Font:
                for (const auto& [id, font] : ui()->fonts) choices.push_back(id.rfind("Fonts.", 0) == 0 ? id.substr(6) : id);
                break;
            case uischema::Type::WidgetAnim:
            case uischema::Type::BitmapAnim:
                for (const tr::XmlNode& an : ui()->animations.children) {
                    const tr::XmlNode* id = an.child("id");
                    const tr::XmlNode* type = an.child("animType");
                    if (id && type && type->text == (a.type == uischema::Type::WidgetAnim ? "Widget" : "Bitmap")) choices.push_back(id->text);
                }
                break;
            default: break;
        }
        if (a.type == uischema::Type::Bool) {
            bool v = value == "true";
            if (ImGui::Checkbox("##v", &v)) commitAttribute(cls, a.name, v ? "true" : "false");
        } else if (a.type == uischema::Type::Anchor) {
            const long v = std::strtol(value.c_str(), nullptr, 10);
            const bool known = v >= 0 && v <= 8 && !value.empty();
            const std::string current = known ? anchorNames[v] : value.empty() ? std::string(anchorNames[0]) : "invalid (" + value + ")";
            if (ImGui::BeginCombo("##v", current.c_str())) {
                for (int i = 0; i < 9; ++i) {
                    if (ImGui::Selectable(anchorNames[i], known && i == v)) commitAttribute(cls, a.name, std::to_string(i));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("eWidgetAnchor_%s, written as %d", anchorEnums[i], i);
                }
                ImGui::EndCombo();
            }
        } else if (a.type == uischema::Type::Enum) {
            if (ImGui::BeginCombo("##v", value.empty() ? "(not set)" : value.c_str())) {
                for (const std::string& option : a.values)
                    if (ImGui::Selectable(option.c_str(), option == value)) commitAttribute(cls, a.name, option);
                ImGui::EndCombo();
            }
        } else if (!choices.empty() || a.type == uischema::Type::TextureDef || a.type == uischema::Type::Font ||
                   a.type == uischema::Type::WidgetAnim || a.type == uischema::Type::BitmapAnim) {
            if (ImGui::BeginCombo("##v", value.empty() ? "(none)" : value.c_str(), ImGuiComboFlags_HeightLarge)) {
                std::string& filter = editBuffers_["filter:" + a.name];
                char buffer[128] = {};
                std::snprintf(buffer, sizeof buffer, "%s", filter.c_str());
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputTextWithHint("##filter", "filter", buffer, sizeof buffer)) filter = buffer;
                std::string lowerFilter = filter;
                for (char& c : lowerFilter) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (ImGui::Selectable("(none)", value.empty())) commitAttribute(cls, a.name, "");
                size_t listed = 0;
                for (const std::string& option : choices) {
                    std::string lower = option;
                    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (!lowerFilter.empty() && lower.find(lowerFilter) == std::string::npos) continue;
                    if (++listed > 400) {
                        ImGui::TextDisabled("(more: filter)");
                        break;
                    }
                    if (ImGui::Selectable(option.c_str(), option == value)) commitAttribute(cls, a.name, option);
                }
                ImGui::EndCombo();
            }
        } else {
            // Free text, numbers, text ids, effects, sounds: written when the field is left (Enter or focus elsewhere).
            auto it = editBuffers_.find(a.name);
            if (it == editBuffers_.end()) it = editBuffers_.emplace(a.name, present && invalidValue.empty() ? value : "").first;
            char buffer[512] = {};
            std::snprintf(buffer, sizeof buffer, "%s", it->second.c_str());
            if (ImGui::InputTextWithHint("##v", a.defaultValue.c_str(), buffer, sizeof buffer)) it->second = buffer;
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const std::string v = it->second;
                editBuffers_.erase(it);
                commitAttribute(cls, a.name, v);
            }
            if (!ImGui::IsItemActive() && present && invalidValue.empty() && it != editBuffers_.end() && it->second != value) it->second = value;
        }
        ImGui::TableNextColumn();
        if (present && ImGui::SmallButton("x")) commitAttribute(cls, a.name, "");
        if (present && ImGui::IsItemHovered()) ImGui::SetTooltip("Remove (back to the default%s%s)", a.defaultValue.empty() ? "" : " ",
                                                                a.defaultValue.c_str());
        ImGui::PopID();
    }
    // Attributes the game does not read for this kind.
    for (const auto& [name, value] : n.attributes) {
        if (uischema::attribute(cls, name)) continue;
        ImGui::PushID(name.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The game does not read %s on a %s.", name.c_str(), cls.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(value.c_str());
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("x")) commitAttribute(cls, name, "");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void UiWorkspace::drawProblemsPanel() {
    // A layout saved before this panel existed: dock it with the Element panel on its first appearance.
    if (const ImGuiWindow* element = ImGui::FindWindowByName("Element###UI.Element"); element && element->DockId)
        ImGui::SetNextWindowDockID(element->DockId, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Problems###UI.Problems")) {
        ImGui::End();
        return;
    }
    if (!doc_) {
        ImGui::End();
        return;
    }
    size_t errors = 0, newErrors = 0;
    std::multiset<std::string> known = originalErrors_;
    std::vector<bool> isNew(issues_.size(), false);
    for (size_t k = 0; k < issues_.size(); ++k) {
        if (!issues_[k].error) continue;
        ++errors;
        const auto it = known.find(issues_[k].message);
        if (it != known.end()) known.erase(it);
        else {
            isNew[k] = true;
            ++newErrors;
        }
    }
    ImGui::Text("%zu errors (%zu new), %zu warnings%s", errors, newErrors, issues_.size() - errors,
                validatedRevision_ != doc_->revision() ? "  (checking...)" : "");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Errors: what the game would fail to load or read differently. Saving is refused while there are new\n"
                          "errors; errors the file already had when it was opened (shipped with the game) are allowed.");
    ImGui::SameLine();
    ImGui::Checkbox("Errors only", &problemsErrorsOnly_);
    size_t resettable = 0;
    for (const auto& i : issues_) resettable += i.resettable ? 1 : 0;
    if (resettable > 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton(("Reset " + std::to_string(resettable) + " invalid value(s) to the default").c_str())) {
            // Each invalid value removed from its element: the game then uses the constructor's default.
            const std::vector<uischema::Issue> todo = issues_;
            runEdit([&] {
                for (const auto& i : todo)
                    if (i.resettable && doc_->node(i.templates, i.path)) doc_->removeAttribute(i.templates, i.path, i.attribute);
            });
            validateNow();
        }
    }
    if (!saveRefusal_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", saveRefusal_.c_str());
    }
    ImGui::Separator();
    ImGui::BeginChild("issues");
    ImGuiListClipper clipper;
    std::vector<size_t> shown;
    for (size_t k = 0; k < issues_.size(); ++k)
        if (!problemsErrorsOnly_ || issues_[k].error) shown.push_back(k);
    clipper.Begin(int(shown.size()));
    while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const uischema::Issue& i = issues_[shown[size_t(row)]];
            const ImVec4 colour = i.error ? (isNew[shown[size_t(row)]] ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f) : ImVec4(0.85f, 0.55f, 0.5f, 1.0f))
                                          : ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
            std::string where;
            if (!i.section.empty()) where = "Definitions/" + i.section;
            else {
                const bool t = canvas_.templates();
                if (t != i.templates) where = i.templates ? "Templates" : "Widgets";
                else where = pathLabel(i.path);
                if (where.empty()) where = i.templates ? "Templates" : "Widgets";
            }
            ImGui::PushID(row);
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            const std::string text = std::string(i.error ? (isNew[shown[size_t(row)]] ? "NEW ERROR  " : "error  ") : "warning  ") + where + ": " + i.message;
            const bool clicked = ImGui::Selectable(text.c_str(), false);
            if (clicked && !i.section.empty()) {
                // A Definitions entry: shown in the Definitions panel.
                for (tr::UiSection s : {tr::UiSection::Colours, tr::UiSection::Fonts, tr::UiSection::Textures, tr::UiSection::Animations})
                    if (i.section == (s == tr::UiSection::Colours ? "Colors" : s == tr::UiSection::Fonts ? "Fonts" : s == tr::UiSection::Textures ? "Textures" : "Animations")) {
                        selectDefinition(s, i.path.empty() ? -1 : i.path[0]);
                        focusDefinitions_ = defScroll_ = true;
                    }
            } else if (clicked) {
                if (canvas_.templates() != i.templates) {
                    canvas_.setTemplates(i.templates);
                    syncWithDocument();
                }
                const std::vector<int32_t> branch = branchFor(i.path);
                select(branch, true, false);
                select(i.path, false, true);
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    ImGui::EndChild();
    ImGui::End();
}

bool UiWorkspace::saveChanges() {
    if (!doc_ || !doc_->modified()) return true;
    validateNow();
    std::multiset<std::string> known = originalErrors_;
    std::vector<std::string> fresh;
    for (const auto& i : issues_) {
        if (!i.error) continue;
        const auto it = known.find(i.message);
        if (it != known.end()) known.erase(it);
        else fresh.push_back(i.message);
    }
    if (!fresh.empty()) {
        saveRefusal_ = "Not saved: " + std::to_string(fresh.size()) + " new error(s), first: " + fresh.front();
        ctx_.setStatus("UI layout not saved: " + std::to_string(fresh.size()) + " new error(s) - see Problems.", true);
        return false;
    }
    try {
        tr::parseXml(tr::Bytes(doc_->text().begin(), doc_->text().end()));     // must still parse as a whole
        std::string error;
        if (!ClientWorkspace::writeFileWithBackup(filePath_, tr::Bytes(doc_->text().begin(), doc_->text().end()), error)) throw std::runtime_error(error);
    } catch (const std::exception& e) {
        ctx_.setStatus(std::string("Saving the UI layout failed: ") + e.what(), true);
        return false;
    }
    doc_->markSaved();
    saveRefusal_.clear();
    ctx_.setStatus("Saved " + filePath_ + " (the game reads it at start-up).", false);
    return true;
}

void UiWorkspace::revertChanges() {
    if (!doc_ || !doc_->modified()) return;
    const bool templates = canvas_.templates();
    const std::vector<int32_t> branch = branch_, selected = selected_;
    loadExport();
    canvas_.setTemplates(templates);
    if (canvas_.node(branch)) select(branch, true, false);
    if (canvas_.node(selected)) select(selected, false, true);
    ctx_.setStatus("UI layout changes reverted.", false);
}

void UiWorkspace::drawEditMenu() {
    const bool can = doc_ != nullptr;
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, can && doc_->canUndo())) runEdit([&] { doc_->undo(); });
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, can && doc_->canRedo())) runEdit([&] { doc_->redo(); });
    ImGui::Separator();
    const bool free = can && hasSelection_ && isFreeElement(selected_);
    if (ImGui::MenuItem("Cut", "Ctrl+X", false, free)) cutSelection();
    if (ImGui::MenuItem("Copy", "Ctrl+C", false, can && hasSelection_)) copySelection();
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, can && hasSelection_)) pasteClipboard();
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, free)) duplicateSelection();
    if (ImGui::MenuItem("Delete", "Del", false, free)) deleteSelection();
    if (ImGui::MenuItem("Move Up", "Alt+Up", false, free)) moveSelection(-1);
    if (ImGui::MenuItem("Move Down", "Alt+Down", false, free)) moveSelection(1);
    if (ImGui::BeginMenu("Arrange", can && hasSelection_ && !interactive_)) {
        drawArrangeMenu();
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save UI Layout", "Ctrl+S", false, hasUnsavedChanges())) saveChanges();
    if (ImGui::MenuItem("Revert UI Layout Changes", nullptr, false, hasUnsavedChanges())) revertChanges();
}

void UiWorkspace::handleShortcuts() {
    if (!doc_) return;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) runEdit([&] { doc_->undo(); });
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
        runEdit([&] { doc_->redo(); });
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal) && hasUnsavedChanges()) saveChanges();
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, ImGuiInputFlags_RouteGlobal)) duplicateSelection();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteGlobal)) copySelection();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X, ImGuiInputFlags_RouteGlobal)) cutSelection();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, ImGuiInputFlags_RouteGlobal)) pasteClipboard();
    if (ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteGlobal)) deleteSelection();
    if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_UpArrow, ImGuiInputFlags_RouteGlobal)) moveSelection(-1);
    if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_DownArrow, ImGuiInputFlags_RouteGlobal)) moveSelection(1);
}

// ------------------------------------------------------------------------------------------
// Form designer: toolbox, drops, clipboard, reparenting, snapping
// ------------------------------------------------------------------------------------------
namespace {

// The toolbox: the element kinds the factory creates. Basic ones are made fresh; controls (with their fixed parts) are copied from
// the first complete one in the layout.
struct ToolboxItem {
    const char* kind;
    const char* description;
};
const ToolboxItem kToolbox[] = {
    {"WidgetComposite", "a container: holds other elements, clips them to itself"},
    {"Window", "a container scripts attach to (script)"},
    {"Bitmap", "a texture region (textureDef)"},
    {"Text", "a string from the UI text tables (textId, fontId)"},
    {"RichText", "formatted text"},
    {"ColoredRect", "a plain colour rectangle"},
    {"PushButton", "a button: Bitmap + Text parts, Up / Highlighted / Down / Disabled states"},
    {"ToggleButton", "an on / off button: a Bitmap part"},
    {"CheckBox", "a toggle button and a label"},
    {"ComboBox", "a drop-down list"},
    {"ProgressBar", "a bar with a Filler part"},
    {"ScrollBar", "arrows, track and thumb"},
    {"Slider", "track and thumb"},
    {"EditBox", "a text field (Caret, Highlight)"},
    {"Frame", "a 17-piece bordered frame"},
};

bool isBasicKind(const std::string& kind) {
    return kind == "WidgetComposite" || kind == "Window" || kind == "Bitmap" || kind == "Text" || kind == "RichText" || kind == "ColoredRect";
}

}  // namespace

tr::XmlNode UiWorkspace::newElement(const std::string& kind) const {
    tr::XmlNode e;
    if (!doc_) return e;
    if (isBasicKind(kind)) {
        // Sized to be seen; everything else at the class defaults.
        e.tag = kind;
        e.attributes.emplace_back("uiid", "New" + kind);
        if (kind == "Bitmap") e.attributes.insert(e.attributes.end(), {{"sizeW", "64"}, {"sizeH", "64"}, {"enabled", "true"}, {"textureDef", "DEFAULT"}});
        else if (kind == "Text" || kind == "RichText") e.attributes.insert(e.attributes.end(), {{"sizeW", "200"}, {"sizeH", "20"}, {"enabled", "true"}});
        else if (kind == "ColoredRect")
            e.attributes.insert(e.attributes.end(), {{"sizeW", "64"}, {"sizeH", "64"}, {"colorR", "0"}, {"colorG", "0"}, {"colorB", "0"}, {"colorA", "128"},
                                                     {"effect", "PalUIColoredRect.fx"}, {"enabled", "true"}});
        else e.attributes.insert(e.attributes.end(), {{"sizeW", "100"}, {"sizeH", "100"}, {"pickable", "true"}, {"enabled", "true"}});
        return e;
    }
    // A control: a copy of the first one in the layout that has all its parts, renamed, visible, at its parent's top left.
    const auto& parts = uischema::ownedParts(kind);
    const tr::XmlNode* found = nullptr;
    std::function<void(const tr::XmlNode&)> visit = [&](const tr::XmlNode& n) {
        if (found) return;
        if (n.tag == kind) {
            bool complete = true;
            for (const std::string& part : parts) complete &= n.child(part) != nullptr;
            if (complete) {
                found = &n;
                return;
            }
        }
        for (const tr::XmlNode& c : n.children) visit(c);
    };
    visit(doc_->ui().widgets);
    visit(doc_->ui().templates);
    if (!found) return e;
    e = *found;
    std::vector<std::pair<std::string, std::string>> attributes;
    attributes.emplace_back("uiid", "New" + kind);
    for (const auto& a : e.attributes)
        if (a.first != "uiid" && a.first != "anchorOffsetX" && a.first != "anchorOffsetY" && a.first != "childAnchor" && a.first != "parentAnchor" &&
            a.first != "visible" && a.first != "widgetAnimName")
            attributes.push_back(a);
    e.attributes = std::move(attributes);
    return e;
}

std::vector<int32_t> UiWorkspace::containerAt(int32_t x, int32_t y) const {
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it) {
        if (!it->rect.contains(x, y)) continue;
        const tr::XmlNode* n = canvas_.node(it->path);
        if (n && uischema::takesFreeChildren(uischema::elementClass(n->tag))) return it->path;
    }
    return {};
}

bool UiWorkspace::dropNewElement(const std::string& kind, int32_t x, int32_t y) {
    if (!doc_) return false;
    const std::vector<int32_t> parent = containerAt(x, y);
    const tr::XmlNode* p = parent.empty() ? nullptr : canvas_.node(parent);
    if (!p) {
        ctx_.setStatus("Drop " + kind + " onto a WidgetComposite or Window (only they take new elements).", true);
        return false;
    }
    tr::XmlNode e = newElement(kind);
    if (e.tag.empty()) {
        ctx_.setStatus("No complete " + kind + " in the layout to copy.", true);
        return false;
    }
    const size_t index = p->children.size();
    std::vector<int32_t> added;
    runEdit([&] {
        doc_->beginGroup();
        try {
            added = doc_->insertElement(canvas_.templates(), parent, index, e);
            // Placed with its top left at the drop point.
            const tr::XmlNode* n = doc_->node(canvas_.templates(), added);
            const int32_t w = int32_t(std::strtol(n->attribute("sizeW").c_str(), nullptr, 10)), h = int32_t(std::strtol(n->attribute("sizeH").c_str(), nullptr, 10));
            canvas_.setExport(&doc_->ui());
            int32_t ox = 0, oy = 0;
            canvas_.offsetsFor(added, UiCanvas::Rect{x, y, x + w, y + h}, ox, oy);
            std::vector<std::pair<std::string, std::string>> set;
            if (ox != 0) set.push_back({"anchorOffsetX", std::to_string(ox)});
            if (oy != 0) set.push_back({"anchorOffsetY", std::to_string(oy)});
            if (!set.empty()) doc_->setAttributes(canvas_.templates(), added, set, {});
        } catch (...) {
            doc_->endGroup();
            throw;
        }
        doc_->endGroup();
    });
    if (added.empty()) return false;
    if (!hasBranch_) select(branchFor(added), true, false);
    select(added, false, true);
    return true;
}

void UiWorkspace::copySelection() {
    if (!doc_ || !hasSelection_ || selected_.empty()) return;
    try {
        ImGui::SetClipboardText(doc_->elementText(canvas_.templates(), selected_).c_str());
        ctx_.setStatus("Copied " + pathLabel(selected_) + ".", false);
    } catch (const std::exception& e) {
        ctx_.setStatus(std::string("Copy failed: ") + e.what(), true);
    }
}

void UiWorkspace::cutSelection() {
    if (!hasSelection_ || !isFreeElement(selected_)) return;
    copySelection();
    deleteSelection();
}

bool UiWorkspace::pasteClipboard() {
    if (!doc_ || !hasSelection_) return false;
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) return false;
    tr::XmlNode e;
    try {
        e = tr::UiDocument::parseElement(clip);
    } catch (const std::exception& ex) {
        ctx_.setStatus(std::string("The clipboard is not an element: ") + ex.what(), true);
        return false;
    }
    if (!uischema::isFactoryKind(e.tag)) {
        ctx_.setStatus("The clipboard holds a <" + e.tag + ">, which cannot be pasted as an element of its own.", true);
        return false;
    }
    // Into the selection when it is a container, else after it in its parent.
    std::vector<int32_t> parent = selected_;
    size_t index = 0;
    const tr::XmlNode* sel = canvas_.node(selected_);
    if (sel && uischema::takesFreeChildren(uischema::elementClass(sel->tag))) index = sel->children.size();
    else if (isFreeElement(selected_)) {
        parent.pop_back();
        index = size_t(selected_.back()) + 1;
    } else {
        ctx_.setStatus("Select a container (or an element in one) to paste into.", true);
        return false;
    }
    std::vector<int32_t> added;
    runEdit([&] { added = doc_->insertElement(canvas_.templates(), parent, index, e); });
    if (!added.empty()) select(added, false, true);
    return !added.empty();
}

bool UiWorkspace::moveSelectionInto(const std::vector<int32_t>& parent, size_t index, bool keepScreenPosition) {
    if (!doc_ || !hasSelection_ || !isFreeElement(selected_)) return false;
    const tr::XmlNode* p = canvas_.node(parent);
    if (!(parent.empty() || (p && uischema::takesFreeChildren(uischema::elementClass(p->tag))))) {
        ctx_.setStatus("Only a WidgetComposite or Window takes other elements.", true);
        return false;
    }
    const std::vector<int32_t> from = selected_;
    const UiCanvas::Rect was = canvas_.layout(from);
    std::vector<int32_t> moved;
    runEdit([&] {
        doc_->beginGroup();
        try {
            moved = doc_->moveElementTo(canvas_.templates(), from, parent, index);
            if (keepScreenPosition && !moved.empty()) {
                // Laid out in its new place (the canvas reads the re-parsed document), then put back at its old top-left corner; its
                // size is its own (or its new parent's, with resizeToParent) and is not written.
                canvas_.setExport(ui());
                const UiCanvas::Rect now = canvas_.layout(moved);
                std::vector<std::pair<std::string, std::string>> set;
                std::vector<std::string> remove;
                if (placementEdit(moved, UiCanvas::Rect{was.x0, was.y0, was.x0 + now.width(), was.y0 + now.height()}, set, remove))
                    doc_->setAttributes(canvas_.templates(), moved, set, remove);
            }
        } catch (...) {
            doc_->endGroup();
            throw;
        }
        doc_->endGroup();
    });
    if (!moved.empty()) select(moved, false, true);
    return !moved.empty();
}

UiCanvas::Rect UiWorkspace::snapRect(const UiCanvas::Rect& r, bool moveX0, bool moveX1, bool moveY0, bool moveY1, float pixelsPerUnit) {
    guides_.clear();
    UiCanvas::Rect out = r;
    if (selected_.empty()) return out;
    const std::vector<int32_t> parentPath(selected_.begin(), selected_.end() - 1);
    const UiCanvas::Rect pr = canvas_.layout(parentPath);
    if (snap_) {
        // Candidate lines: the parent's and every sibling's edges and centres.
        std::vector<int32_t> xs{pr.x0, pr.x1, (pr.x0 + pr.x1) / 2}, ys{pr.y0, pr.y1, (pr.y0 + pr.y1) / 2};
        if (const tr::XmlNode* parent = canvas_.node(parentPath))
            for (size_t k = 0; k < parent->children.size(); ++k) {
                if (int32_t(k) == selected_.back()) continue;
                std::vector<int32_t> sibling = parentPath;
                sibling.push_back(int32_t(k));
                if (parent->children[k].attribute("visible") == "false") continue;
                const UiCanvas::Rect s = canvas_.layout(sibling);
                xs.insert(xs.end(), {s.x0, s.x1, (s.x0 + s.x1) / 2});
                ys.insert(ys.end(), {s.y0, s.y1, (s.y0 + s.y1) / 2});
            }
        const int32_t threshold = std::max(1, int32_t(std::lround(6.0f / std::max(0.01f, pixelsPerUnit))));
        // The best shift for the moving edges (and the centre when the whole rectangle moves).
        auto best = [&](const std::vector<int32_t>& lines, const std::vector<int32_t>& edges, int32_t& shift, int32_t& line) {
            int32_t bestDistance = threshold + 1;
            for (int32_t e : edges)
                for (int32_t l : lines)
                    if (std::abs(l - e) < bestDistance) {
                        bestDistance = std::abs(l - e);
                        shift = l - e;
                        line = l;
                    }
            return bestDistance <= threshold;
        };
        std::vector<int32_t> ex, ey;
        if (moveX0) ex.push_back(out.x0);
        if (moveX1) ex.push_back(out.x1);
        if (moveX0 && moveX1) ex.push_back((out.x0 + out.x1) / 2);
        if (moveY0) ey.push_back(out.y0);
        if (moveY1) ey.push_back(out.y1);
        if (moveY0 && moveY1) ey.push_back((out.y0 + out.y1) / 2);
        int32_t shift = 0, line = 0;
        if (!ex.empty() && best(xs, ex, shift, line)) {
            if (moveX0) out.x0 += shift;
            if (moveX1) out.x1 += shift;
            guides_.push_back({true, line});
            moveX0 = moveX1 = false;                 // snapped: no grid for this axis
        }
        if (!ey.empty() && best(ys, ey, shift, line)) {
            if (moveY0) out.y0 += shift;
            if (moveY1) out.y1 += shift;
            guides_.push_back({false, line});
            moveY0 = moveY1 = false;
        }
    }
    if (grid_ > 0) {
        // Grid steps from the parent's top left.
        const auto toGrid = [&](int32_t v, int32_t origin) { return origin + int32_t(std::lround(float(v - origin) / float(grid_))) * grid_; };
        if (moveX0 && moveX1) {
            const int32_t d = toGrid(out.x0, pr.x0) - out.x0;
            out.x0 += d;
            out.x1 += d;
        } else if (moveX0) out.x0 = toGrid(out.x0, pr.x0);
        else if (moveX1) out.x1 = toGrid(out.x1, pr.x0);
        if (moveY0 && moveY1) {
            const int32_t d = toGrid(out.y0, pr.y0) - out.y0;
            out.y0 += d;
            out.y1 += d;
        } else if (moveY0) out.y0 = toGrid(out.y0, pr.y0);
        else if (moveY1) out.y1 = toGrid(out.y1, pr.y0);
    }
    return out;
}

void UiWorkspace::drawToolboxPanel() {
    // Likewise: with the Widgets tree.
    if (const ImGuiWindow* tree = ImGui::FindWindowByName("Widgets###UI.Widgets"); tree && tree->DockId)
        ImGui::SetNextWindowDockID(tree->DockId, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Toolbox###UI.Toolbox")) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Drag onto the canvas or the outline; double-click: into the selection.");
    for (const ToolboxItem& item : kToolbox) {
        ImGui::PushID(item.kind);
        ImGui::Selectable(item.kind, false, ImGuiSelectableFlags_AllowDoubleClick);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s%s", item.description, isBasicKind(item.kind) ? "" : "\n(a copy of the first complete one in the layout)");
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hasSelection_) {
                const tr::XmlNode* sel = canvas_.node(selected_);
                if (sel && uischema::takesFreeChildren(uischema::elementClass(sel->tag))) addChild(item.kind);
                else ctx_.setStatus("Select a WidgetComposite or Window to add into.", true);
            }
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("UI_TOOLBOX", item.kind, std::strlen(item.kind));
            ImGui::Text("New %s", item.kind);
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }
    ImGui::End();
}
