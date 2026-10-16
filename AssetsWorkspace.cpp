#include "AssetsWorkspace.h"

#include "EditorContext.h"
#include "assetlibrary.h"
#include "dds.h"
#include "image.h"

#include "imgui_internal.h"   // DockBuilder

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace {

std::string lower(const std::string& s) { return tr::toLowerAscii(s); }

std::string extensionOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot));
}

std::string stemOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string utf8(const fs::path& p) {
    const auto text = p.u8string();
    return std::string(text.begin(), text.end());
}

// What each kind of file is (the extensions the editor knows; others show as the bare extension).
const char* kindLabel(const std::string& extension) {
    static const std::map<std::string, const char*> labels = {
        {".geo", "models"},          {".dds", "textures"},         {".anm", "animations"},       {".mkr", "animation markers"},
        {".wav", "sounds (WAV)"},    {".ogg", "sounds (Ogg)"},     {".mp3", "sounds (MP3)"},     {".pfx", "particle systems"},
        {".pkg", "special-FX packages"}, {".sky", "skies"},        {".ltmp", "light templates"}, {".fxc", "effects"},
        {".shc", "shader libraries"},    {".cat", "asset catalogs"}, {".xml", "XML"},            {".map", "maps"},
        {".vis", "map visibility"},  {".lff", "fonts"},            {".ttf", "fonts"},            {".txt", "text"},
    };
    const auto it = labels.find(extension);
    return it == labels.end() ? nullptr : it->second;
}

void copyInto(char* buffer, size_t size, const std::string& text) { std::snprintf(buffer, size, "%s", text.c_str()); }

std::string defaultFolder() {
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    const fs::path base = documents ? fs::u8path(documents) : fs::current_path();
    return utf8(base / "RasaEdit" / "Assets");
}

void openFolder(const std::string& folder) {
    const auto generic = fs::u8path(folder).generic_u8string();
    SDL_OpenURL(("file:///" + std::string(generic.begin(), generic.end())).c_str());
}

void writeBytes(const fs::path& path, const tr::Bytes& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw tr::FormatError("cannot write " + utf8(path));
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) throw tr::FormatError("writing " + utf8(path) + " failed");
}

tr::Bytes readBytes(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) throw tr::FormatError("cannot read " + path);
    return tr::Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

void SDLCALL AssetsWorkspace::folderDialogCallback(void* userdata, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto* self = static_cast<AssetsWorkspace*>(userdata);
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->dialogFolder_ = files[0];
}

void SDLCALL AssetsWorkspace::replaceDialogCallback(void* userdata, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto* self = static_cast<AssetsWorkspace*>(userdata);
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->dialogReplace_ = files[0];
}

void SDLCALL AssetsWorkspace::pngDialogCallback(void* userdata, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto* self = static_cast<AssetsWorkspace*>(userdata);
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->dialogPng_ = files[0];
}

void AssetsWorkspace::buildDefaultLayout(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID left = 0, centre = 0, right = 0, rightBottom = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.30f, &left, &centre);
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.38f, &right, &centre);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.62f, &rightBottom, &right);
    ImGui::DockBuilderDockWindow("Browser###Assets.Browser", left);
    ImGui::DockBuilderDockWindow("Preview###Assets.Preview", centre);
    ImGui::DockBuilderDockWindow("Asset###Assets.Asset", right);
    ImGui::DockBuilderDockWindow("Blender###Assets.Blender", rightBottom);
    ImGui::DockBuilderFinish(dockspace);
}

void AssetsWorkspace::onGameOpened() {
    preview_.release();                              // its scene refers to the previous asset library
    selected_.clear();
    links_ = ModelAnimationLinks{};
    animationLinks_.reset();
    pending_.clear();
    previewing_ = false;
    readNames();
}

void AssetsWorkspace::readNames() {
    names_ = ctx_.assets ? ctx_.assets->namesWithExtension("") : std::vector<std::string>();
    std::sort(names_.begin(), names_.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    std::map<std::string, size_t> counts;
    for (const std::string& n : names_) ++counts[extensionOf(n)];
    const std::string current = kind_ >= 0 && size_t(kind_) < kinds_.size() ? kinds_[size_t(kind_)].extension : std::string();
    kinds_.clear();
    kinds_.push_back(Kind{"", "everything", names_.size()});
    for (const auto& [extension, count] : counts) {
        const char* label = kindLabel(extension);
        kinds_.push_back(Kind{extension, label ? label : (extension.empty() ? "no extension" : extension.substr(1)), count});
    }
    std::sort(kinds_.begin() + 1, kinds_.end(), [](const Kind& a, const Kind& b) { return a.count > b.count; });
    kind_ = 0;
    for (size_t k = 0; k < kinds_.size(); ++k)
        if (kinds_[k].extension == current) kind_ = int(k);
    rebuild();
}

void AssetsWorkspace::rebuild() {
    visible_.clear();
    const std::string needle = lower(filter_);
    const std::string extension = size_t(kind_) < kinds_.size() ? kinds_[size_t(kind_)].extension : std::string();
    for (size_t k = 0; k < names_.size(); ++k) {
        if (kind_ != 0 && extensionOf(names_[k]) != extension) continue;
        if (!needle.empty() && lower(names_[k]).find(needle) == std::string::npos) continue;
        visible_.push_back(k);
    }
}

void AssetsWorkspace::select(const std::string& name) {
    if (lower(name) == lower(selected_)) return;
    selected_ = name;
    links_ = ModelAnimationLinks{};
    if (extensionOf(name) == ".geo" && ctx_.assets) links_ = animationLinks_.find(*ctx_.assets, ctx_.gameData.get(), name);
    preview_.select(ctx_, name);
    preview_.setAnimationLinks(extensionOf(name) == ".geo" ? &links_ : nullptr);
    animationModelsFinal_ = false;
    if (extensionOf(name) == ".anm" && ctx_.assets) {
        preview_.setAnimationModels(ctx_, animationLinks_.modelsPlaying(*ctx_.assets, ctx_.gameData.get(), name), !animationLinks_.modelIndexDone());
        animationModelsFinal_ = animationLinks_.modelIndexDone();
    }
}

void AssetsWorkspace::showAsset(const std::string& name) {
    if (!ctx_.assets) return;
    readNames();
    // Shown in the list whatever the filter.
    bool listed = false;
    for (size_t k : visible_) listed = listed || lower(names_[k]) == lower(name);
    if (!listed) {
        filter_[0] = 0;
        kind_ = 0;
        rebuild();
    }
    selected_ = name;
    reloadSelected();
    scrollToSelected_ = true;
}

void AssetsWorkspace::reloadSelected() {
    preview_.release();                              // a model it cached under this name would be the old one
    const std::string name = selected_;
    selected_.clear();
    select(name);
}

std::string AssetsWorkspace::whereFrom(const std::string& name) const {
    if (!ctx_.assets) return "";
    if (ctx_.assets->hasOverride(name)) return "imported (preview)";
    const std::optional<tr::AssetLocation> at = ctx_.assets->locate(name);
    if (!at) return "";
    if (at->inArchive) return at->archiveIndex < ctx_.assets->archives().size() ? ctx_.assets->archives()[at->archiveIndex].configKey : "archive";
    return "loose";
}

void AssetsWorkspace::drawPanels() {
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (!dialogFolder_.empty()) copyInto(folder_, sizeof folder_, dialogFolder_), dialogFolder_.clear();
        if (!dialogReplace_.empty() && !selected_.empty()) {
            try {
                pending_ = {ImportedFile{selected_, readBytes(dialogReplace_), "replaced by " + utf8(fs::u8path(dialogReplace_).filename())}};
                log_ = {"Read " + dialogReplace_ + " as " + selected_ + " (not checked: the game must be able to read it)."};
            } catch (const std::exception& e) {
                log_ = {std::string("Reading the file failed: ") + e.what()};
            }
            dialogReplace_.clear();
        }
        if (!dialogPng_.empty() && !selected_.empty()) {
            try {
                // Encoded in the original's format, with mip levels when the original has them.
                const tr::Bytes original = ctx_.assets->read(selected_);
                const tr::DdsHeader header = tr::readDdsHeader(original);
                const tr::RgbaImage image = tr::readPng(readBytes(dialogPng_));
                const tr::DdsEncoding encoding = tr::ddsEncodingLike(header);
                pending_ = {ImportedFile{selected_, tr::encodeDds(image, encoding, header.mipCount > 1),
                                         std::to_string(image.width) + " x " + std::to_string(image.height) + " " + tr::ddsEncodingName(encoding) +
                                             " from " + utf8(fs::u8path(dialogPng_).filename())}};
                log_.clear();
                if (image.width != header.width || image.height != header.height)
                    log_.push_back("Note: the original is " + std::to_string(header.width) + " x " + std::to_string(header.height) + ".");
            } catch (const std::exception& e) {
                log_ = {std::string("Importing the PNG failed: ") + e.what()};
            }
            dialogPng_.clear();
        }
    }
    if (folder_[0] == 0) copyInto(folder_, sizeof folder_, defaultFolder());
    // Preview hook: RASAEDIT_ASSET=<file name> starts with that file selected.
    static bool hooked = false;
    if (!hooked && ctx_.assets) {
        hooked = true;
        if (const char* name = std::getenv("RASAEDIT_ASSET"); name && *name) showAsset(name);
    }
    // Every model's `anim` property, a few milliseconds a frame (which models an animation plays on); a selected animation's list
    // is made again once it is complete.
    if (ctx_.assets && !animationLinks_.modelIndexDone()) {
        animationLinks_.indexModels(*ctx_.assets, 4.0);
        if (animationLinks_.modelIndexDone() && extensionOf(selected_) == ".anm" && !animationModelsFinal_) {
            preview_.setAnimationModels(ctx_, animationLinks_.modelsPlaying(*ctx_.assets, ctx_.gameData.get(), selected_), false);
            animationModelsFinal_ = true;
        }
    }
    drawBrowser();
    drawDetails();
    // Models: the Blender exchange follows the browser's selection.
    blender_.draw(ctx_, "Blender###Assets.Blender", extensionOf(selected_) == ".geo" ? selected_ : std::string(), &links_,
                  [this](const std::string& name) { showAsset(name); });
    // Drawn last: on first creation the newest window becomes the selected tab of a shared dock node.
    drawPreview();
}

void AssetsWorkspace::drawBrowser() {
    if (!ImGui::Begin("Browser###Assets.Browser")) { ImGui::End(); return; }
    if (!ctx_.assets) { ImGui::TextDisabled("No game folder open."); ImGui::End(); return; }
    if (names_.empty()) readNames();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##assetfilter", "search names (any part)", filter_, sizeof filter_)) rebuild();
    ImGui::SetNextItemWidth(-1);
    if (size_t(kind_) < kinds_.size()) {
        const Kind& k = kinds_[size_t(kind_)];
        const std::string preview = k.label + "  (" + std::to_string(k.count) + ")";
        if (ImGui::BeginCombo("##kind", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (size_t i = 0; i < kinds_.size(); ++i) {
                const std::string label = kinds_[i].label + (kinds_[i].extension.empty() ? "" : "  " + kinds_[i].extension) + "  (" +
                                          std::to_string(kinds_[i].count) + ")";
                if (ImGui::Selectable(label.c_str(), int(i) == kind_)) {
                    kind_ = int(i);
                    rebuild();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::TextDisabled("%zu of %zu files", visible_.size(), names_.size());
    // Up / down step through the list while the browser has focus (not while typing in the search field).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !visible_.empty()) {
        const int step = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 0;
        if (step != 0) {
            int at = -1;
            for (size_t k = 0; k < visible_.size(); ++k)
                if (lower(names_[visible_[k]]) == lower(selected_)) at = int(k);
            select(names_[visible_[size_t(std::clamp(at < 0 ? 0 : at + step, 0, int(visible_.size()) - 1))]]);
            scrollToSelected_ = true;
        }
    }
    if (ImGui::BeginTable("assets", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
        ImGui::TableHeadersRow();
        const std::string chosen = lower(selected_);
        int selectedRow = -1;
        if (scrollToSelected_)
            for (size_t k = 0; k < visible_.size(); ++k)
                if (lower(names_[visible_[k]]) == chosen) selectedRow = int(k);
        ImGuiListClipper clipper;
        clipper.Begin(int(visible_.size()));
        if (selectedRow >= 0) clipper.IncludeItemByIndex(selectedRow);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const std::string& name = names_[visible_[size_t(row)]];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool isSelected = lower(name) == chosen;
                ImGui::PushID(row);
                if (ImGui::Selectable(name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) select(name);
                ImGui::PopID();
                if (isSelected && row == selectedRow) {
                    ImGui::SetScrollHereY(0.5f);
                    scrollToSelected_ = false;
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", whereFrom(name).c_str());
            }
        }
        if (selectedRow < 0) scrollToSelected_ = false;
        ImGui::EndTable();
    }
    ImGui::End();
}

void AssetsWorkspace::drawPreview() {
    if (!ImGui::Begin("Preview###Assets.Preview")) { ImGui::End(); return; }
    if (!ctx_.assets) ImGui::TextDisabled("No game folder open.");
    else preview_.draw(ctx_);
    ImGui::End();
}

void AssetsWorkspace::drawDetails() {
    if (!ImGui::Begin("Asset###Assets.Asset")) { ImGui::End(); return; }
    if (!ctx_.assets || selected_.empty()) {
        ImGui::TextDisabled(ctx_.assets ? "Select a file in the Browser." : "No game folder open.");
        drawPending();
        ImGui::End();
        return;
    }
    const std::string extension = extensionOf(selected_);
    ImGui::TextWrapped("%s", selected_.c_str());
    const char* label = kindLabel(extension);
    ImGui::TextDisabled("%s", label ? label : extension.c_str());
    const std::optional<tr::AssetLocation> at = ctx_.assets->locate(selected_);
    if (ctx_.assets->hasOverride(selected_)) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "Shown from an import (preview); the game's copy is unchanged.");
    if (at && at->inArchive && at->entry) {
        const std::string key = at->archiveIndex < ctx_.assets->archives().size() ? ctx_.assets->archives()[at->archiveIndex].configKey : "?";
        ImGui::Text("Archive %s (tabula_rasa.cfg key), %s KB%s", key.c_str(), std::to_string((at->entry->rawSize + 1023) / 1024).c_str(),
                    at->entry->flags != 0 ? ", compressed" : "");
    } else if (at) {
        ImGui::TextWrapped("Loose file %s", at->filePath.c_str());
    }

    ImGui::SeparatorText("Out");
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 3);
    ImGui::InputText("##folder", folder_, sizeof folder_);
    ImGui::SameLine();
    if (ImGui::SmallButton("...##folder")) SDL_ShowOpenFolderDialog(folderDialogCallback, this, ctx_.window, folder_, false);
    ImGui::SetItemTooltip("The folder files are written to.");
    if (ImGui::Button("Extract")) {
        try {
            const fs::path out = fs::u8path(folder_) / fs::u8path(selected_);
            writeBytes(out, ctx_.assets->read(selected_));
            log_ = {"Wrote " + utf8(out)};
            ctx_.setStatus(log_.back(), false);
        } catch (const std::exception& e) {
            log_ = {std::string("Extract failed: ") + e.what()};
            ctx_.setStatus(log_.back(), true);
        }
    }
    ImGui::SetItemTooltip("The file exactly as the game reads it (decompressed).");
    if (extension == ".dds") {
        ImGui::SameLine();
        if (ImGui::Button("Export as PNG")) {
            try {
                const fs::path out = fs::u8path(folder_) / fs::u8path(stemOf(selected_) + ".png");
                writeBytes(out, tr::writePng(tr::ddsImage(ctx_.assets->read(selected_))));
                log_ = {"Wrote " + utf8(out) + " (the top mip level)"};
                ctx_.setStatus(log_.back(), false);
            } catch (const std::exception& e) {
                log_ = {std::string("PNG export failed: ") + e.what()};
                ctx_.setStatus(log_.back(), true);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) openFolder(folder_);

    ImGui::SeparatorText("In");
    if (extension == ".geo") ImGui::TextWrapped("Models: export and import through Blender in the Blender panel (it keeps bones, materials and collision).");
    if (extension == ".dds") {
        if (ImGui::Button("Import PNG...")) {
            static const SDL_DialogFileFilter filters[] = {{"PNG", "png"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(pngDialogCallback, this, ctx_.window, filters, 2, folder_, false);
        }
        ImGui::SetItemTooltip("Replaces this texture with a PNG, encoded like the original (DXT1 / DXT3 / DXT5 / ARGB, mip levels if it has them).");
        ImGui::SameLine();
    }
    if (ImGui::Button("Replace with a file...")) SDL_ShowOpenFileDialog(replaceDialogCallback, this, ctx_.window, nullptr, 0, folder_, false);
    ImGui::SetItemTooltip("Any file of yours under this name, byte for byte - not checked: the game must be able to read it.");
    drawPending();
    ImGui::End();
}

void AssetsWorkspace::drawPending() {
    for (const std::string& line : log_) ImGui::TextWrapped("%s", line.c_str());
    if (pending_.empty()) return;
    ImGui::SeparatorText("Imported (not in the game yet)");
    for (const ImportedFile& f : pending_) ImGui::BulletText("%s (%zu KB) - %s", f.name.c_str(), (f.data.size() + 1023) / 1024, f.note.c_str());
    if (ImGui::Button(previewing_ ? "Preview again" : "Preview in the editor")) {
        ctx_.assets->clearOverrides();
        for (const ImportedFile& f : pending_) ctx_.assets->setOverride(f.name, f.data);
        previewing_ = true;
        if (!ctx_.currentMapName.empty()) ctx_.queueMap(ctx_.currentMapName);
        const std::string name = pending_.front().name;
        selected_ = name;
        reloadSelected();                            // re-read through the override
        ctx_.setStatus("Previewing " + name + " (the map reloads; the game is unchanged)", false);
    }
    ImGui::SetItemTooltip("The editor uses the imported file instead of the game's for this session (this preview and the map). Nothing is written.");
    if (previewing_) {
        ImGui::SameLine();
        if (ImGui::Button("End preview")) {
            ctx_.assets->clearOverrides();
            previewing_ = false;
            if (!ctx_.currentMapName.empty()) ctx_.queueMap(ctx_.currentMapName);
            reloadSelected();
            ctx_.setStatus("Preview ended", false);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add to the patch archive")) {
        try {
            log_ = {addToPatchArchive(ctx_.gameRoot, pending_)};
            ctx_.setStatus(log_.back(), false);
        } catch (const std::exception& e) {
            log_ = {std::string("Writing the patch archive failed: ") + e.what()};
            ctx_.setStatus(log_.back(), true);
        }
    }
    ImGui::SetItemTooltip("Writes it into data/rasaedit_patch.glm (the Blender panel shows the archive and whether tabula_rasa.cfg names it).");
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        if (previewing_) {
            ctx_.assets->clearOverrides();
            previewing_ = false;
            if (!ctx_.currentMapName.empty()) ctx_.queueMap(ctx_.currentMapName);
        }
        pending_.clear();
        log_.clear();
    }
}
