#include "BlenderPanel.h"

#include "EditorContext.h"
#include "assetlibrary.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

std::string stemOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string lower(const std::string& s) { return tr::toLowerAscii(s); }

std::string utf8(const fs::path& p) {
    const auto text = p.u8string();
    return std::string(text.begin(), text.end());
}

// <Documents>/RasaEdit/Blender (SDL's user folder; the working directory when there is none).
std::string defaultExportRoot() {
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    const fs::path base = documents ? fs::u8path(documents) : fs::current_path();
    return utf8(base / "RasaEdit" / "Blender");
}

// Where the add-on's schema goes for an export folder: the default root for the default <root>/<model> folders, else the folder.
std::string schemaPathFor(const std::string& exportFolder) {
    const fs::path folder = fs::u8path(exportFolder);
    std::error_code ec;
    const bool underRoot = fs::equivalent(folder.parent_path(), fs::u8path(defaultExportRoot()), ec) ||
                           lower(utf8(folder.parent_path())) == lower(defaultExportRoot());
    return utf8((underRoot ? folder.parent_path() : folder) / blenderSchemaFileName());
}

void openFolder(const std::string& folder) {
    const auto generic = fs::u8path(folder).generic_u8string();
    const std::string url = "file:///" + std::string(generic.begin(), generic.end());
    SDL_OpenURL(url.c_str());
}

void copyInto(char* buffer, size_t size, const std::string& text) {
    std::snprintf(buffer, size, "%s", text.c_str());
}

}  // namespace

void SDLCALL BlenderPanel::importDialogCallback(void* userdata, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto* self = static_cast<BlenderPanel*>(userdata);
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->dialogImportPath_ = files[0];
}

void SDLCALL BlenderPanel::exportDialogCallback(void* userdata, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto* self = static_cast<BlenderPanel*>(userdata);
    std::lock_guard<std::mutex> lock(self->dialogMutex_);
    self->dialogExportFolder_ = files[0];
}

void BlenderPanel::draw(EditorContext& ctx, const char* title, const std::string& selectedModel, const ModelAnimationLinks* links,
                        const std::function<void(const std::string&)>& showAsset) {
    if (!ImGui::Begin(title)) {
        ImGui::End();
        return;
    }
    if (!ctx.assets) {
        ImGui::TextDisabled("No game folder open.");
        ImGui::End();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        if (!dialogImportPath_.empty()) copyInto(importPath_, sizeof importPath_, dialogImportPath_), dialogImportPath_.clear();
        if (!dialogExportFolder_.empty()) copyInto(exportFolder_, sizeof exportFolder_, dialogExportFolder_), dialogExportFolder_.clear();
    }
    if (ImGui::CollapsingHeader("How it works")) {
    ImGui::TextWrapped("Models go to Blender as glTF (.gltf + .bin + PNG textures). Blender: File > Import > glTF 2.0. Back: File > Export > glTF 2.0, "
                       "format glTF Separate, with Include > Custom Properties on (it carries the game's material and piece settings) and Data > Mesh > "
                       "Attributes on (billboard sprite centres).");
    ImGui::TextWrapped("Easier: the RasaEdit add-on for Blender (blender/rasaedit_addon.zip in the RasaEdit folder, or tools/blender/make_addon_zip.py; "
                       "Blender: Edit > Preferences > Add-ons > Install from Disk). It adds File > Import / Export > Tabula Rasa model, keeps "
                       "low-detail pieces and collision in hidden collections on every trip, sets 30 fps and checks the game's limits; its "
                       "panels edit the material effect and parameters, piece settings and contact points, it has a game-like material "
                       "preview and collision tools. The panels read %s, which an export writes beside the model folders when it is missing.",
                       blenderSchemaFileName().c_str());
    }
    if (ImGui::CollapsingHeader("Export a model", ImGuiTreeNodeFlags_DefaultOpen)) drawExport(ctx, selectedModel, links);
    if (ImGui::CollapsingHeader("Import from Blender", ImGuiTreeNodeFlags_DefaultOpen)) drawImport(ctx, showAsset);
    if (ImGui::CollapsingHeader("Patch archive", ImGuiTreeNodeFlags_DefaultOpen)) drawPatchArchive(ctx);
    ImGui::End();
}

void BlenderPanel::drawExport(EditorContext& ctx, const std::string& selectedModel, const ModelAnimationLinks* links) {
    // The model is the browser's selection (any model of the game: filter the Browser by "models" and pick one).
    exportModel_ = selectedModel;
    if (exportModel_.empty()) {
        ImGui::TextDisabled("Select a model (.geo) in the Browser.");
        return;
    }
    ImGui::Text("Model  %s", exportModel_.c_str());
    const std::string model = exportModel_;
    const bool known = ctx.assets->exists(model);
    if (!known) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Not in the archives.");
    // The animations to take along: the model's own by default.
    if (known && lower(model) != lower(exportModelFor_)) {
        exportModelFor_ = model;
        exportAnimations_ = modelOwnAnimations(*ctx.assets, model);
        copyInto(exportFolder_, sizeof exportFolder_, utf8(fs::u8path(defaultExportRoot()) / stemOf(model)));
    }
    // What the model is to the game and every animation tied to it: ticked = exported.
    if (links && lower(links->model) == lower(model)) {
        for (const std::string& kind : links->kinds) ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", kind.c_str());
        if (!links->groups.empty()) {
            auto chosen = [&](const std::string& a) {
                return std::find_if(exportAnimations_.begin(), exportAnimations_.end(), [&](const std::string& x) { return lower(x) == lower(a); });
            };
            ImGui::TextUnformatted("Linked animations:");
            drawAnimationGroups(
                *links,
                [&](const AnimationChoice& c) {
                    bool on = chosen(c.name) != exportAnimations_.end();
                    const std::string label = c.frequency > 0 ? c.name + "  (weight " + std::to_string(c.frequency) + ")" : c.name;
                    if (ImGui::Checkbox(label.c_str(), &on)) {
                        if (on) exportAnimations_.push_back(c.name);
                        else exportAnimations_.erase(chosen(c.name));
                    }
                },
                [&](const AnimationGroup& g) {
                    if (ImGui::SmallButton("all"))
                        for (const AnimationChoice& c : g.animations)
                            if (chosen(c.name) == exportAnimations_.end()) exportAnimations_.push_back(c.name);
                    ImGui::SetItemTooltip("Export every animation of this group");
                });
        }
    }
    ImGui::TextUnformatted("Animations to export:");
    int remove = -1;
    for (size_t k = 0; k < exportAnimations_.size(); ++k) {
        ImGui::PushID(int(k));
        if (ImGui::SmallButton("x")) remove = int(k);
        ImGui::SameLine();
        ImGui::TextUnformatted(exportAnimations_[k].c_str());
        ImGui::PopID();
    }
    if (remove >= 0) exportAnimations_.erase(exportAnimations_.begin() + remove);
    if (exportAnimations_.empty()) ImGui::TextDisabled("  none");
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##animfilter", "add an animation (search .anm names)", animationFilter_, sizeof animationFilter_);
    if (animationFilter_[0]) {
        if (allAnimations_.empty()) allAnimations_ = ctx.assets->namesWithExtension(".anm");
        const std::string f = lower(animationFilter_);
        if (ImGui::BeginListBox("##animchoices", ImVec2(420, 6 * ImGui::GetTextLineHeightWithSpacing()))) {
            size_t shown = 0;
            for (const std::string& a : allAnimations_) {
                if (lower(a).find(f) == std::string::npos) continue;
                if (++shown > 200) {
                    ImGui::TextDisabled("(more - type more of the name)");
                    break;
                }
                if (ImGui::Selectable(a.c_str()) &&
                    std::none_of(exportAnimations_.begin(), exportAnimations_.end(), [&](const std::string& x) { return lower(x) == lower(a); }))
                    exportAnimations_.push_back(a);
            }
            ImGui::EndListBox();
        }
    }
    {
        static const char* const choices[] = {"in a hidden collection", "left out", "shown with the others"};
        ImGui::SetNextItemWidth(220);
        ImGui::Combo("Low-detail pieces", &lowDetail_, choices, 3);
        ImGui::SetItemTooltip("LOD pieces and stand-ins overlap the full-detail ones. Hidden: Blender puts them in an excluded \"Low detail\" "
                              "collection (tick it in the outliner to see them). Hidden or left out, an import keeps the original's unless "
                              "the file carries them.");
    }
    ImGui::Checkbox("Collision", &exportCollision_);
    ImGui::SetItemTooltip("The bones' collision volumes (what players walk on and bump into) as meshes in a hidden Collision collection.");
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("Folder", exportFolder_, sizeof exportFolder_);
    ImGui::SameLine();
    if (ImGui::SmallButton("...##exportfolder")) SDL_ShowOpenFolderDialog(exportDialogCallback, this, ctx.window, exportFolder_[0] ? exportFolder_ : nullptr, false);
    ImGui::BeginDisabled(!known || !exportFolder_[0]);
    if (ImGui::Button("Export")) {
        exportLog_.clear();
        try {
            ModelExportOptions options;
            options.collision = exportCollision_;
            options.lowDetail = lowDetail_ == 1 ? ModelExportOptions::LowDetail::Omitted
                                : lowDetail_ == 2 ? ModelExportOptions::LowDetail::Visible
                                                  : ModelExportOptions::LowDetail::Hidden;
            const std::string out = utf8(fs::u8path(exportFolder_) / (stemOf(model) + ".gltf"));
            const ModelExchangeReport report = exportModelGltf(*ctx.assets, model, exportAnimations_, out, options);
            exportLog_.push_back("Wrote " + out + " (" + std::to_string(report.written.size()) + " files)");
            // The add-on's schema, once: beside the model folders (the add-on looks in the file's folder and the one above it).
            const std::string schema = schemaPathFor(exportFolder_);
            std::error_code ec;
            if (!fs::exists(fs::u8path(schema), ec)) {
                std::string error;
                exportLog_.push_back(writeBlenderSchema(*ctx.assets, schema, error) ? "Wrote the add-on schema " + schema
                                                                                     : "warning: " + error);
            }
            for (const std::string& w : report.warnings) exportLog_.push_back("warning: " + w);
            ctx.setStatus("Exported " + model + " for Blender: " + out, false);
        } catch (const std::exception& e) {
            exportLog_.push_back(std::string("Export failed: ") + e.what());
            ctx.setStatus(std::string("Export failed: ") + e.what(), true);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!exportFolder_[0]);
    if (ImGui::Button("Open folder##export")) openFolder(exportFolder_);
    ImGui::SameLine();
    if (ImGui::Button("Rewrite the add-on schema")) {
        const std::string schema = schemaPathFor(exportFolder_);
        std::string error;
        BlenderSchemaStats stats;
        if (writeBlenderSchema(*ctx.assets, schema, error, &stats))
            exportLog_.push_back("Wrote " + schema + " (" + std::to_string(stats.effects) + " effects from " + std::to_string(stats.models) +
                                 " models)");
        else
            exportLog_.push_back("warning: " + error);
    }
    ImGui::SetItemTooltip("The effects, parameters, connection points and names the Blender add-on's panels offer, from the game's files "
                          "(written by the first export; rewrite after the game's models change).");
    ImGui::EndDisabled();
    for (const std::string& line : exportLog_) ImGui::TextWrapped("%s", line.c_str());
}

void BlenderPanel::drawImport(EditorContext& ctx, const std::function<void(const std::string&)>& showAsset) {
    ImGui::SetNextItemWidth(420);
    ImGui::InputTextWithHint("File", ".gltf or .glb from Blender", importPath_, sizeof importPath_);
    ImGui::SameLine();
    if (ImGui::SmallButton("...##importfile")) {
        static const SDL_DialogFileFilter filters[] = {{"glTF", "gltf;glb"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(importDialogCallback, this, ctx.window, filters, 2, importPath_[0] ? importPath_ : nullptr, false);
    }
    ImGui::SetNextItemWidth(320);
    ImGui::InputTextWithHint("Save as", "model name (empty: the original's, or the file's)", importModelName_, sizeof importModelName_);
    ImGui::SetItemTooltip("Naming an existing model replaces it in the game (every map entity using it); a new name is a new model.");
    ImGui::SetNextItemWidth(320);
    ImGui::InputTextWithHint("Based on", "original model when the file names none (empty: new model)", importSource_, sizeof importSource_);
    ImGui::Checkbox("Generate collision for a new model", &generateCollision_);
    ImGui::SetItemTooltip("Collision (what players walk on and bump into) comes from meshes named \"collision...\" (the exported ones are in the "
                          "hidden Collision collection). A file without any keeps the original model's; a new model without any gets a "
                          "walkable surface made from its opaque pieces when this is on.");
    ImGui::Checkbox("Changed textures under a new name", &renameChangedTextures_);
    ImGui::SetItemTooltip("Off: a texture you edited replaces the game's texture of that name (every model using it). On: it is saved as "
                          "<model>_<texture> and only this model uses it.");
    ImGui::BeginDisabled(!importPath_[0]);
    if (ImGui::Button("Import")) {
        log_.clear();
        imported_.reset();
        try {
            ModelImportOptions options;
            options.modelName = importModelName_;
            options.source = importSource_;
            options.renameChangedTextures = renameChangedTextures_;
            options.generateCollision = generateCollision_;
            imported_ = importModelGltf(*ctx.assets, importPath_, options);
            importedFrom_ = importPath_;
            ctx.setStatus("Imported " + std::string(importPath_) + ": " + std::to_string(imported_->files.size()) + " file(s)", false);
        } catch (const std::exception& e) {
            log_.push_back(std::string("Import failed: ") + e.what());
            ctx.setStatus(std::string("Import failed: ") + e.what(), true);
        }
    }
    ImGui::EndDisabled();
    if (!imported_) {
        for (const std::string& line : log_) ImGui::TextWrapped("%s", line.c_str());
        return;
    }
    ImGui::Separator();
    ImGui::Text("From %s:", utf8(fs::u8path(importedFrom_).filename()).c_str());
    for (const ImportedFile& f : imported_->files) ImGui::BulletText("%s (%zu KB) - %s", f.name.c_str(), (f.data.size() + 1023) / 1024, f.note.c_str());
    for (const std::string& w : imported_->warnings) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "warning: %s", w.c_str());
    // Preview: the files override the archives for this session; the map is reloaded so its entities use them.
    if (ImGui::Button(previewing_ ? "Preview again" : "Preview in the editor")) {
        ctx.assets->clearOverrides();
        for (const ImportedFile& f : imported_->files) ctx.assets->setOverride(f.name, f.data);
        previewing_ = true;
        if (!ctx.currentMapName.empty()) ctx.queueMap(ctx.currentMapName);
        if (!imported_->files.empty() && showAsset) showAsset(imported_->files[0].name);
        ctx.setStatus("Previewing " + imported_->files[0].name + " (the map reloads; the game is unchanged)", false);
    }
    ImGui::SetItemTooltip("The editor uses the imported files instead of the game's for this session: map entities with this model and the "
                          "Assets preview show it. Nothing is written.");
    if (previewing_) {
        ImGui::SameLine();
        if (ImGui::Button("End preview")) {
            ctx.assets->clearOverrides();
            previewing_ = false;
            if (!ctx.currentMapName.empty()) ctx.queueMap(ctx.currentMapName);
            ctx.setStatus("Preview ended", false);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add to the patch archive")) {
        try {
            log_.push_back(addToPatchArchive(ctx.gameRoot, imported_->files, ctx.assets.get()));
            ctx.setStatus(log_.back(), false);
        } catch (const std::exception& e) {
            log_.push_back(std::string("Writing the patch archive failed: ") + e.what());
            ctx.setStatus(log_.back(), true);
        }
    }
    ImGui::SetItemTooltip("Writes the files into data/rasaedit_patch.glm (replacing entries of the same name). The game reads it once "
                          "tabula_rasa.cfg names it (below); restart the game to see changes.");
    for (const std::string& line : log_) ImGui::TextWrapped("%s", line.c_str());
}

void BlenderPanel::drawPatchArchive(EditorContext& ctx) {
    const std::string path = patchArchivePath(ctx.gameRoot);
    std::error_code ec;
    const bool exists = fs::exists(fs::u8path(path), ec);
    const bool registered = patchArchiveRegistered(ctx.gameRoot);
    ImGui::TextWrapped("%s: %s", path.c_str(), exists ? "exists" : "not written yet");
    if (exists) {
        try {
            tr::GlmArchive archive(path);
            ImGui::Text("%zu file(s) in it", archive.entries().size());
            if (ImGui::TreeNode("Contents")) {
                for (const tr::GlmEntry& e : archive.entries()) ImGui::BulletText("%s (%u KB)", e.name.c_str(), (e.rawSize + 1023) / 1024);
                ImGui::TreePop();
            }
        } catch (const std::exception& e) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "unreadable: %s", e.what());
        }
    }
    if (registered) {
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "tabula_rasa.cfg names it ([Paths] AAA_RasaEdit): the game loads its files first.");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "tabula_rasa.cfg does not name it yet - the game ignores it.");
        if (ImGui::Button("Add it to tabula_rasa.cfg")) {
            try {
                ctx.setStatus(registerPatchArchive(ctx.gameRoot), false);
            } catch (const std::exception& e) {
                ctx.setStatus(std::string("Changing tabula_rasa.cfg failed: ") + e.what(), true);
            }
        }
        ImGui::SetItemTooltip("Adds AAA_RasaEdit = \"data/rasaedit_patch.glm\" under [Paths] (the key sorts before every shipped one, so its "
                              "files win). The first change keeps tabula_rasa.cfg.rasaedit-original.");
    }
}
