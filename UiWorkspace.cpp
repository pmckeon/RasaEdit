#include "UiWorkspace.h"

#include "GpuTexture.h"

#include "imgui_internal.h"   // DockBuilder

#include <algorithm>
#include <filesystem>
#include <functional>
#include <sstream>

UiWorkspace::UiWorkspace(EditorContext& ctx) : Workspace(ctx) {}

UiWorkspace::~UiWorkspace() = default;

void UiWorkspace::releaseGpuResources() {
    if (texture_) SDL_ReleaseGPUTexture(ctx_.device, texture_);
    texture_ = nullptr;
}

void UiWorkspace::buildDefaultLayout(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID left = 0, centre = 0, right = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.22f, &left, &centre);
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.26f, &right, &centre);
    ImGui::DockBuilderDockWindow("Widgets###UI.Widgets", left);
    ImGui::DockBuilderDockWindow("Canvas###UI.Canvas", centre);
    ImGui::DockBuilderDockWindow("Element###UI.Element", right);
    ImGui::DockBuilderFinish(dockspace);
}

void UiWorkspace::onGameOpened() { loadExport(); }

void UiWorkspace::loadExport() {
    ui_.reset();
    loadError_.clear();
    branch_.clear();
    selected_.clear();
    hasBranch_ = hasSelection_ = false;
    drawn_.clear();
    dirty_ = true;
    if (ctx_.gameRoot.empty()) return;
    try {
        tr::Bytes xml;
        if (!ctx_.assets || !ctx_.assets->tryRead("Tabula_Rasa_UI_EXPORT.xml", xml))
            xml = tr::readFile((std::filesystem::path(ctx_.gameRoot) / "data" / "xml" / "Tabula_Rasa_UI_EXPORT.xml").string());
        ui_ = std::make_unique<tr::UiExport>(tr::readUiExport(xml));
    } catch (const std::exception& e) {
        loadError_ = e.what();
        ui_.reset();
    }
    canvas_.setSources(ui_.get(), ctx_.assets.get(), ctx_.gameData.get(), ctx_.gameRoot);
}

bool UiWorkspace::showBranch(const std::string& uiidPath) {
    if (!ui_) loadExport();
    if (!ui_) return false;
    std::vector<int32_t> path;
    std::stringstream parts(uiidPath);
    std::string part;
    const tr::XmlNode* n = &ui_->widgets;
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
    if (drawBranch) {
        branch_ = path;
        hasBranch_ = true;
        dirty_ = true;
    }
    if (revealInTree) revealSelection_ = true;
}

void UiWorkspace::beginFrame() {
    if (!ctx_.gameRoot.empty() && !ui_ && loadError_.empty()) loadExport();
    if (!dirty_ || !ui_ || !hasBranch_) return;
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
    const tr::XmlNode* n = ui_ ? &ui_->widgets : nullptr;
    for (int32_t i : path) {
        if (!n || i < 0 || size_t(i) >= n->children.size()) break;
        n = &n->children[size_t(i)];
        const std::string uiid = n->attribute("uiid");
        out += (out.empty() ? "" : " / ") + (uiid.empty() ? n->tag : uiid);
    }
    return out;
}

void UiWorkspace::drawPanels() {
    drawTreePanel();
    drawElementPanel();
    drawCanvasPanel();                        // last: on first creation it becomes the shared node's visible tab
}

// ------------------------------------------------------------------------------------------
// Tree
// ------------------------------------------------------------------------------------------
void UiWorkspace::drawTreeNode(const tr::XmlNode& node, std::vector<int32_t>& path) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    const bool isSelected = hasSelection_ && path == selected_;
    if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;
    // Reveal: open every ancestor of the selection.
    if (revealSelection_ && hasSelection_ && path.size() < selected_.size() && std::equal(path.begin(), path.end(), selected_.begin()))
        ImGui::SetNextItemOpen(true);
    const bool hidden = node.attribute("visible") == "false";
    if (hidden) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool open = ImGui::TreeNodeEx(&node, flags, "%s", label(node).c_str());
    if (hidden) ImGui::PopStyleColor();
    if (isSelected && revealSelection_) {
        ImGui::SetScrollHereY(0.4f);
        revealSelection_ = false;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) select(path, true, false);
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
    if (!ui_) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Could not read Tabula_Rasa_UI_EXPORT.xml");
        ImGui::TextWrapped("%s", loadError_.c_str());
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter by uiid or element", filter_, sizeof filter_);
    ImGui::Separator();
    ImGui::BeginChild("tree");
    const std::string filter = filter_;
    if (filter.empty()) {
        std::vector<int32_t> path;
        for (size_t k = 0; k < ui_->widgets.children.size(); ++k) {
            path.assign(1, int32_t(k));
            drawTreeNode(ui_->widgets.children[k], path);
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
        visit(ui_->widgets);
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
    ImGui::Checkbox("Mouse highlight", &options_.mouseHighlight);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The window wires (TRUIBackgroundMask) glow around the mouse in game (ui_mouse_mask.dds, 256 units across).\n"
                          "On: they follow the mouse over the canvas.");
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
    if (options_.showHidden != before.showHidden || options_.mouseHighlight != before.mouseHighlight || options_.background != before.background)
        dirty_ = true;
    if (ui_ && hasBranch_) {
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
    if (!ui_ || !texture_ || !hasBranch_) {
        ImGui::TextDisabled(ui_ ? "Choose an element in the Widgets tree." : "No UI export loaded.");
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
        if (!under.empty()) {
            outline(canvas_.layout(under), IM_COL32(255, 255, 255, 140), 1.0f);
            ImGui::SetTooltip("%s\n(%d, %d)", pathLabel(under).c_str(), x, y);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) select(under, false, true);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) select(under, true, true);
        }
    }
    if (hasSelection_) outline(canvas_.layout(selected_), IM_COL32(255, 220, 60, 255), 2.0f);
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
    const tr::XmlNode* n = ui_ && hasSelection_ ? UiCanvas::node(*ui_, selected_) : nullptr;
    if (!n) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::End();
        return;
    }
    ImGui::Text("%s", n->tag.c_str());
    ImGui::TextWrapped("%s", pathLabel(selected_).c_str());
    const UiCanvas::Rect r = canvas_.layout(selected_);
    ImGui::Text("Screen rectangle: (%d, %d) - (%d, %d), %d x %d", r.x0, r.y0, r.x1, r.y1, r.width(), r.height());
    if (selected_ != branch_ && ImGui::Button("Draw this branch")) select(selected_, true, false);
    ImGui::Text("%zu children", n->children.size());
    ImGui::Separator();
    if (ImGui::BeginTable("attributes", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Attribute", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Value");
        for (const auto& a : n->attributes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.first.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", a.second.c_str());
        }
        ImGui::EndTable();
    }
    // Resolved references.
    auto showTexture = [&](const char* key) {
        const std::string id = n->attribute(key);
        if (id.empty()) return;
        const tr::UiTextureDef* def = ui_->texture(id);
        if (!def) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s %s: no such textureDef", key, id.c_str());
            return;
        }
        ImGui::Text("%s %s: %s (%d, %d) - (%d, %d)%s%s", key, id.c_str(), def->name.c_str(), def->left, def->top, def->right, def->bottom,
                    def->tileU ? ", tile U" : "", def->tileV ? ", tile V" : "");
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
        const tr::UiFont* font = ui_->font(n->attribute("fontId"));
        if (!font) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Font %s: not defined", n->attribute("fontId").c_str());
        else
            for (const tr::UiFontRange& range : font->ranges)
                ImGui::Text("Font %u-%u: %s %d%s%s%s", range.begin, range.end, range.face.c_str(), range.pointSize, range.shadow ? " shadow" : "",
                            range.bold ? " bold" : "", range.italic ? " italic" : "");
    }
    ImGui::End();
}
