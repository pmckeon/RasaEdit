#include "TextWorkspace.h"

#include "imgui_internal.h"   // DockBuilder

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace py = tr::py;

namespace {

const char* kPrefix = "generated.client.language.";

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The editable strings of an entry's value: the string itself, or the string items of a list / tuple.
std::vector<const py::Object*> strings(const py::Object* value) {
    std::vector<const py::Object*> out;
    if (!value) return out;
    if (value->kind == py::Kind::String) out.push_back(value);
    else if (value->kind == py::Kind::List || value->kind == py::Kind::Tuple)
        for (const py::Ref& item : value->items)
            if (item->kind == py::Kind::String) out.push_back(item.get());
    return out;
}

// A copy of `value` with its item-th string (counting string items only) set to utf8.
py::Ref withString(const py::Object& value, size_t item, const std::string& utf8) {
    if (value.kind == py::Kind::String) return py::makeText(utf8);
    std::vector<py::Ref> items;
    size_t k = 0;
    for (const py::Ref& i : value.items) {
        if (i->kind == py::Kind::String && k++ == item) items.push_back(py::makeText(utf8));
        else items.push_back(i);
    }
    return value.kind == py::Kind::List ? py::makeList(items) : py::makeTuple(items);
}

std::string firstLine(const std::string& text) {
    const size_t end = text.find_first_of("\r\n");
    std::string line = end == std::string::npos ? text : text.substr(0, end) + " ...";
    if (line.size() > 200) line = line.substr(0, 200) + "...";
    return line;
}

}  // namespace

TextWorkspace::TextWorkspace(EditorContext& ctx) : Workspace(ctx) {}

void TextWorkspace::buildDefaultLayout(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID left = 0, centre = 0, right = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.18f, &left, &centre);
    ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.34f, &right, &centre);
    ImGui::DockBuilderDockWindow("Modules###Text.Modules", left);
    ImGui::DockBuilderDockWindow("Texts###Text.Texts", centre);
    ImGui::DockBuilderDockWindow("Entry###Text.Entry", right);
    ImGui::DockBuilderFinish(dockspace);
}

void TextWorkspace::onGameOpened() {
    edited_.clear();
    undo_.clear();
    redo_.clear();
    version_ = savedVersion_ = 0;
    languages_.clear();
    missionReferences_.clear();
    if (!ctx_.gameData) return;
    std::set<std::string> found;
    for (const std::string& name : ctx_.gameData->moduleNames()) {
        if (name.compare(0, std::strlen(kPrefix), kPrefix) != 0) continue;
        const std::string rest = name.substr(std::strlen(kPrefix));
        const size_t dot = rest.find('.');
        if (dot != std::string::npos) found.insert(rest.substr(0, dot));
    }
    languages_.assign(found.begin(), found.end());
    std::string language = ctx_.setting("textLanguage", "english");
    if (!found.count(language)) language = found.count("english") ? "english" : (languages_.empty() ? "" : languages_.front());
    language_.clear();
    setLanguage(language);
    buildMissionReferences();
}

void TextWorkspace::setLanguage(const std::string& language) {
    if (language == language_) return;
    language_ = language;
    ctx_.setSetting("textLanguage", language);
    const std::string previous = module_ >= 0 && module_ < int(modules_.size()) ? modules_[size_t(module_)].label : std::string();
    scanModules();
    module_ = -1;
    for (size_t k = 0; k < modules_.size(); ++k)
        if (modules_[k].label == previous) module_ = int(k);
    selectedModule_ = -1;
    rowsDirty_ = true;
}

void TextWorkspace::scanModules() {
    modules_.clear();
    if (!ctx_.gameData) return;
    const std::string prefix = std::string(kPrefix) + language_ + ".";
    for (const std::string& name : ctx_.gameData->moduleNames()) {
        if (name.compare(0, prefix.size(), prefix) != 0 || name.find('.', prefix.size()) != std::string::npos) continue;
        const py::Object* lookup = lookupOf(name);
        if (!lookup || lookup->kind != py::Kind::Dict) continue;            // __init__ and modules without a lookup table
        ModuleInfo m;
        m.module = name;
        m.label = name.substr(prefix.size());
        if (m.label.size() > 8 && m.label.compare(m.label.size() - 8, 8, "language") == 0) m.label.resize(m.label.size() - 8);
        std::set<int64_t> ids;
        for (const auto& e : lookup->entries) {
            if (e.first->kind == py::Kind::Tuple) {
                m.gendered = true;
                ids.insert(py::asInt(py::item(e.first.get(), 0)));
            } else ids.insert(py::asInt(e.first.get()));
        }
        m.ids = ids.size();
        modules_.push_back(m);
    }
}

const py::Object* TextWorkspace::lookupOf(const std::string& module) const {
    const auto it = edited_.find(module);
    if (it != edited_.end()) return py::dictGet(it->second.get(), "lookup");
    if (!ctx_.gameData || !ctx_.gameData->hasModule(module)) return nullptr;
    try {
        return ctx_.gameData->global(module, "lookup");
    } catch (const std::exception&) {
        return nullptr;
    }
}

py::Object& TextWorkspace::editableLookup(const std::string& module) {
    auto it = edited_.find(module);
    if (it == edited_.end()) {
        // A copy of the game's globals; the cached original stays as it is until the save.
        const py::Object& globals = ctx_.gameData->module(module);
        py::Ref copy = py::makeDict();
        for (const auto& e : globals.entries) py::dictPut(*copy, e.first, py::deepCopy(e.second));
        it = edited_.emplace(module, copy).first;
    }
    return *const_cast<py::Object*>(py::dictGet(it->second.get(), "lookup"));
}

int TextWorkspace::moduleIndex(const std::string& module) const {
    for (size_t k = 0; k < modules_.size(); ++k)
        if (modules_[k].module == module) return int(k);
    return -1;
}

py::Ref TextWorkspace::keyOf(const ModuleInfo& m, int64_t id, int gender) const {
    return m.gendered ? py::makeTuple({py::makeInt(id), py::makeInt(gender)}) : py::makeInt(id);
}

const py::Object* TextWorkspace::valueOf(const ModuleInfo& m, int64_t id, int gender) const {
    const py::Object* lookup = lookupOf(m.module);
    return m.gendered ? py::dictGetTuple(lookup, {id, gender}) : py::dictGet(lookup, id);
}

void TextWorkspace::put(const std::string& module, const py::Ref& key, const py::Ref& value) {
    py::Object& lookup = editableLookup(module);
    if (value) py::dictPut(lookup, key, value);
    else py::dictRemove(lookup, *key);
}

void TextWorkspace::record(Step step, bool coalesce) {
    if (step.changes.empty()) return;
    // Typing in one field extends the step it began (one undo for a whole edit of a text).
    if (coalesce && !undo_.empty() && undo_.back().label == step.label && undo_.back().changes.size() == step.changes.size()) {
        for (size_t k = 0; k < step.changes.size(); ++k) undo_.back().changes[k].after = step.changes[k].after;
    } else undo_.push_back(std::move(step));
    redo_.clear();
    ++version_;
    rowsDirty_ = true;
}

void TextWorkspace::applyStep(const Step& step, bool forward) {
    if (forward)
        for (const Change& c : step.changes) put(c.module, c.key, c.after);
    else
        for (auto it = step.changes.rbegin(); it != step.changes.rend(); ++it) put(it->module, it->key, it->before);
    ++version_;
    rowsDirty_ = true;
    scanModules();
}

void TextWorkspace::undo() {
    if (undo_.empty()) return;
    Step step = std::move(undo_.back());
    undo_.pop_back();
    applyStep(step, false);
    redo_.push_back(std::move(step));
    coalescing_ = false;
}

void TextWorkspace::redo() {
    if (redo_.empty()) return;
    Step step = std::move(redo_.back());
    redo_.pop_back();
    applyStep(step, true);
    undo_.push_back(std::move(step));
    coalescing_ = false;
}

bool TextWorkspace::setText(const std::string& module, int64_t id, int gender, size_t item, const std::string& utf8) {
    const int index = moduleIndex(module);
    if (index < 0) return false;
    const ModuleInfo& m = modules_[size_t(index)];
    const py::Object* value = valueOf(m, id, m.gendered ? (gender ? gender : 1) : 0);
    if (!value || item >= strings(value).size()) return false;
    Step step;
    step.label = module + "/" + std::to_string(id) + "/" + std::to_string(gender) + "/" + std::to_string(item);
    const auto change = [&](int g) {
        const py::Object* v = valueOf(m, id, g);
        if (!v || item >= strings(v).size()) return;
        py::Ref key = keyOf(m, id, g);
        py::Ref before = py::dictGetRef(*lookupOf(module), *key);   // owned: values are replaced, never changed in place
        py::Ref after = withString(*v, item, utf8);
        step.changes.push_back(Change{module, key, before, after});
        put(module, key, after);
    };
    if (!m.gendered) change(0);
    else if (gender == 0) {                                    // both genders
        change(1);
        change(2);
    } else change(gender);
    // Keep the first "before" of a coalesced step: record() only replaces "after".
    record(std::move(step), coalescing_);
    return true;
}

std::string TextWorkspace::text(const std::string& module, int64_t id, int gender, size_t item) const {
    const int index = moduleIndex(module);
    if (index < 0) return "";
    const ModuleInfo& m = modules_[size_t(index)];
    const auto list = strings(valueOf(m, id, m.gendered ? (gender ? gender : 1) : 0));
    return item < list.size() ? list[item]->text : std::string();
}

void TextWorkspace::showEntry(const std::string& module, int64_t id) {
    const std::string rest = module.compare(0, std::strlen(kPrefix), kPrefix) == 0 ? module.substr(std::strlen(kPrefix)) : std::string();
    const size_t dot = rest.find('.');
    if (dot == std::string::npos) return;
    setLanguage(rest.substr(0, dot));
    const int index = moduleIndex(module);
    if (index < 0) return;
    setModule(index);
    filter_[0] = 0;
    selectedModule_ = index;
    selectedId_ = id;
    coalescing_ = false;
    sameForBoth_ = text(module, id, 1, 0) == text(module, id, 2, 0);
    scrollToSelection_ = true;
}

void TextWorkspace::setModule(int index) {
    module_ = index;
    rowsDirty_ = true;
}

void TextWorkspace::setFilter(const std::string& filter) {
    std::snprintf(filter_, sizeof filter_, "%s", filter.c_str());
    rowsDirty_ = true;
}

size_t TextWorkspace::rowCount() {
    if (rowsDirty_) rebuildRows();
    return rows_.size();
}

void TextWorkspace::rebuildRows() {
    rowsDirty_ = false;
    rows_.clear();
    std::string filter = lowerAscii(filter_);
    while (!filter.empty() && filter.back() == ' ') filter.pop_back();
    // "#123" or a bare number also matches that id.
    int64_t idFilter = -1;
    {
        const std::string digits = !filter.empty() && filter[0] == '#' ? filter.substr(1) : filter;
        if (!digits.empty() && std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            idFilter = std::strtoll(digits.c_str(), nullptr, 10);
    }
    const bool idOnly = !filter.empty() && filter[0] == '#';
    for (size_t mi = 0; mi < modules_.size(); ++mi) {
        if (module_ >= 0 && int(mi) != module_) continue;
        const ModuleInfo& m = modules_[mi];
        const py::Object* lookup = lookupOf(m.module);
        if (!lookup) continue;
        std::vector<Row> moduleRows;
        std::map<int64_t, size_t> rowOfId;
        for (const auto& e : lookup->entries) {
            const int64_t id = m.gendered ? py::asInt(py::item(e.first.get(), 0)) : py::asInt(e.first.get());
            const int gender = m.gendered ? int(py::asInt(py::item(e.first.get(), 1))) : 0;
            const auto list = strings(e.second.get());
            const std::string first = list.empty() ? py::repr(e.second.get(), 4) : list[0]->text;
            auto r = rowOfId.find(id);
            if (r == rowOfId.end()) {
                Row row;
                row.module = int(mi);
                row.id = id;
                rowOfId.emplace(id, moduleRows.size());
                moduleRows.push_back(row);
                r = rowOfId.find(id);
            }
            Row& row = moduleRows[r->second];
            if (gender <= 1 || row.preview.empty()) row.preview = first;
            else if (gender == 2 && row.preview != first) row.gendersDiffer = true;
        }
        for (Row& row : moduleRows) {
            bool keep = filter.empty();
            if (!keep && row.id == idFilter) keep = true;
            if (!keep && !idOnly) {
                // Search every string of every gender of the id.
                for (int g : m.gendered ? std::vector<int>{1, 2} : std::vector<int>{0}) {
                    for (const py::Object* s : strings(valueOf(m, row.id, g)))
                        if (lowerAscii(s->text).find(filter) != std::string::npos) keep = true;
                    if (keep) break;
                }
            }
            if (!keep) continue;
            row.preview = firstLine(row.preview);
            rows_.push_back(std::move(row));
        }
    }
    if (module_ >= 0)
        std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) { return a.id < b.id; });
}

void TextWorkspace::buildMissionReferences() {
    missionReferences_.clear();
    if (!ctx_.gameData) return;
    static const char* kTypes[] = {"", "name", "log", "opening", "finishing", "reward", "reminder"};
    try {
        if (const py::Object* t = ctx_.gameData->global("generated.client.missionconversation", "lookup"))
            for (const auto& e : t->entries) {
                const int64_t mission = py::asInt(py::item(e.first.get(), 0)), type = py::asInt(py::item(e.first.get(), 1));
                const int64_t id = py::asInt(py::item(e.second.get(), 0), -1);
                missionReferences_.emplace(id, "mission " + std::to_string(mission) + ": " + (type >= 1 && type <= 6 ? kTypes[type] : "?") + " text");
            }
        if (const py::Object* t = ctx_.gameData->global("generated.client.missionobjective", "lookup"))
            for (const auto& e : t->entries) {
                const std::string where = "mission " + std::to_string(py::asInt(py::item(e.first.get(), 0))) + " objective " +
                                          std::to_string(py::asInt(py::item(e.first.get(), 1)));
                const py::Object* texts = py::item(e.second.get(), 0);
                static const char* kFields[] = {"name", "body", "counter 0", "counter 1", "counter 2"};
                for (size_t k = 0; k < py::length(texts) && k < 5; ++k)
                    if (!py::isNone(py::item(texts, k))) missionReferences_.emplace(py::asInt(py::item(texts, k)), where + ": " + kFields[k] + " text");
            }
        if (const py::Object* t = ctx_.gameData->global("generated.client.objectiveconversation", "lookup"))
            for (const auto& e : t->entries) {
                const py::Object* k = e.first.get();
                missionReferences_.emplace(py::asInt(py::item(e.second.get(), 0), -1),
                                           "mission " + std::to_string(py::asInt(py::item(k, 0))) + " objective " + std::to_string(py::asInt(py::item(k, 1))) +
                                               ": conversation (package " + std::to_string(py::asInt(py::item(k, 2))) + ", flag " +
                                               std::to_string(py::asInt(py::item(k, 3))) + ", kind " + std::to_string(py::asInt(py::item(k, 4))) + ")");
            }
    } catch (const std::exception&) {
    }
}

// ------------------------------------------------------------------------------------------
// Panels
// ------------------------------------------------------------------------------------------
void TextWorkspace::drawPanels() {
    drawModules();
    drawTexts();
    drawEntry();
}

void TextWorkspace::drawModules() {
    if (!ImGui::Begin("Modules###Text.Modules")) {
        ImGui::End();
        return;
    }
    if (!ctx_.gameData) {
        ImGui::TextDisabled("Open a game folder first.");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s", language_.c_str());
    ImGui::SetItemTooltip("generated/client/language/%s - the language is chosen at the top of Texts", language_.c_str());
    if (ImGui::BeginTable("modules", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Ids", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("000000").x);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable("All modules", module_ < 0, ImGuiSelectableFlags_SpanAllColumns)) setModule(-1);
        for (size_t k = 0; k < modules_.size(); ++k) {
            const ModuleInfo& m = modules_[k];
            const bool edited = edited_.count(m.module) != 0;
            const std::string label = m.label + (edited ? " *" : "") + "###" + m.module;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(label.c_str(), module_ == int(k), ImGuiSelectableFlags_SpanAllColumns)) setModule(int(k));
            ImGui::SetItemTooltip("%s\n%zu ids%s%s", m.module.c_str(), m.ids, m.gendered ? ", a text per character gender" : "",
                                  edited ? "\nunsaved changes" : "");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%zu", m.ids);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void TextWorkspace::drawTexts() {
    if (!ImGui::Begin("Texts###Text.Texts")) {
        ImGui::End();
        return;
    }
    if (!ctx_.gameData) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("Language");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (ImGui::BeginCombo("##textLanguage", language_.c_str())) {
        for (const std::string& l : languages_)
            if (ImGui::Selectable(l.c_str(), l == language_)) setLanguage(l);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const float addWidth = ImGui::CalcTextSize("Add entry").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 8, ImGui::GetContentRegionAvail().x - addWidth - ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputTextWithHint("##filter", "Search text, or an id (#id: the id only)", filter_, sizeof filter_)) rowsDirty_ = true;
    if (rowsDirty_) rebuildRows();
    ImGui::SameLine();
    ImGui::BeginDisabled(module_ < 0);
    if (ImGui::Button("Add entry")) addEntry();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(module_ < 0 ? "Choose a module first" : "A new id (the module's largest + 1) with empty text");
    ImGui::TextDisabled("%zu entr%s in %s", rows_.size(), rows_.size() == 1 ? "y" : "ies",
                        module_ >= 0 ? modules_[size_t(module_)].label.c_str() : "all modules");

    const bool all = module_ < 0;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("texts", all ? 3 : 2, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        if (all) ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10);
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int scrollRow = -1;
        if (scrollToSelection_) {
            for (size_t k = 0; k < rows_.size(); ++k)
                if (rows_[k].module == selectedModule_ && rows_[k].id == selectedId_) scrollRow = int(k);
            scrollToSelection_ = false;
        }
        ImGuiListClipper clipper;
        clipper.Begin(int(rows_.size()));
        if (scrollRow >= 0) clipper.IncludeItemByIndex(scrollRow);
        while (clipper.Step())
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const Row& row = rows_[size_t(r)];
                ImGui::TableNextRow();
                ImGui::PushID(r);
                const bool selected = row.module == selectedModule_ && row.id == selectedId_;
                if (all) {
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(modules_[size_t(row.module)].label.c_str());
                }
                ImGui::TableNextColumn();
                char id[32];
                std::snprintf(id, sizeof id, "%lld", static_cast<long long>(row.id));
                if (ImGui::Selectable(id, selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    selectedModule_ = row.module;
                    selectedId_ = row.id;
                    coalescing_ = false;
                    const ModuleInfo& m = modules_[size_t(row.module)];
                    sameForBoth_ = !m.gendered || !row.gendersDiffer;
                }
                if (r == scrollRow) ImGui::SetScrollHereY(0.4f);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.preview.c_str());
                if (row.gendersDiffer) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(m/f)");
                    ImGui::SetItemTooltip("The female character's text differs");
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    ImGui::End();
}

void TextWorkspace::drawEntry() {
    if (!ImGui::Begin("Entry###Text.Entry")) {
        ImGui::End();
        return;
    }
    if (selectedModule_ < 0 || selectedModule_ >= int(modules_.size())) {
        ImGui::TextDisabled("Choose an entry in Texts.");
        ImGui::End();
        return;
    }
    const ModuleInfo& m = modules_[size_t(selectedModule_)];
    ImGui::Text("%s  #%lld", m.label.c_str(), static_cast<long long>(selectedId_));
    ImGui::TextDisabled("%s, lookup[%s]", m.module.c_str(), m.gendered ? ("(" + std::to_string(selectedId_) + ", gender)").c_str()
                                                                         : std::to_string(selectedId_).c_str());
    const py::Object* first = valueOf(m, selectedId_, m.gendered ? 1 : 0);
    if (!first) {
        ImGui::TextDisabled("(no such entry - deleted?)");
        ImGui::End();
        return;
    }
    ImGui::Separator();
    // One multi-line field per string; a gendered entry is edited for both genders at once unless they differ.
    const auto field = [&](const char* label, int gender, size_t item, const std::string& current) {
        std::vector<char> buffer(std::max<size_t>(current.size() + 4096, 1024));
        std::memcpy(buffer.data(), current.c_str(), current.size() + 1);
        ImGui::TextUnformatted(label);
        ImGui::PushID(gender * 100 + int(item));
        const float lines = std::clamp(float(std::count(current.begin(), current.end(), '\n') + 2), 3.0f, 14.0f);
        if (ImGui::InputTextMultiline("##text", buffer.data(), buffer.size(), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * lines))) {
            setText(m.module, selectedId_, gender, item, buffer.data());
            coalescing_ = true;
        }
        if (ImGui::IsItemDeactivated()) coalescing_ = false;
        ImGui::PopID();
    };
    const size_t count = strings(first).size();
    if (count == 0) ImGui::TextDisabled("No text in this entry: %s", py::repr(first, 6).c_str());
    if (m.gendered) {
        if (ImGui::Checkbox("Same text for male and female characters", &sameForBoth_) && sameForBoth_) {
            // Make the female text equal to the male one (one undo step).
            for (size_t k = 0; k < count; ++k) setText(m.module, selectedId_, 2, k, text(m.module, selectedId_, 1, k));
            coalescing_ = false;
        }
        for (size_t k = 0; k < count; ++k) {
            if (sameForBoth_) field(count > 1 ? ("Text " + std::to_string(k + 1)).c_str() : "Text", 0, k, text(m.module, selectedId_, 1, k));
            else {
                field("Male character", 1, k, text(m.module, selectedId_, 1, k));
                field("Female character", 2, k, text(m.module, selectedId_, 2, k));
            }
        }
    } else {
        for (size_t k = 0; k < count; ++k)
            field(count > 1 ? ("Item " + std::to_string(k + 1)).c_str() : "Text", 0, k, text(m.module, selectedId_, 0, k));
        if (first->kind == py::Kind::Tuple) ImGui::TextDisabled("Value: %s (numbers are not edited here)", py::repr(first, 6).c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button("Delete entry")) ImGui::OpenPopup("Delete text entry");
    if (ImGui::BeginPopupModal("Delete text entry", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete %s #%lld%s?", m.label.c_str(), static_cast<long long>(selectedId_), m.gendered ? " (both genders)" : "");
        ImGui::TextDisabled("The client shows a missing-translation message where something still names it.");
        if (ImGui::Button("Delete")) {
            deleteEntry();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // Where the client uses a mission text.
    if (m.label == "missiontext") {
        ImGui::SeparatorText("Used by (client mission tables)");
        const auto range = missionReferences_.equal_range(selectedId_);
        if (range.first == range.second) ImGui::TextDisabled("No mission, objective or conversation names it.");
        for (auto it = range.first; it != range.second; ++it) ImGui::BulletText("%s", it->second.c_str());
    }
    ImGui::End();
}

void TextWorkspace::addEntry() {
    if (module_ < 0) return;
    const ModuleInfo& m = modules_[size_t(module_)];
    const py::Object* lookup = lookupOf(m.module);
    int64_t next = 1;
    const py::Object* sample = nullptr;
    for (const auto& e : lookup->entries) {
        const int64_t id = m.gendered ? py::asInt(py::item(e.first.get(), 0)) : py::asInt(e.first.get());
        next = std::max(next, id + 1);
        if (!sample) sample = e.second.get();
    }
    // The new value has the module's shape with empty strings (numbers of a tuple kept from the first entry).
    py::Ref value = py::makeText("");
    if (sample && sample->kind == py::Kind::List) value = py::makeList({py::makeText("")});
    if (sample && sample->kind == py::Kind::Tuple) {
        std::vector<py::Ref> items;
        for (const py::Ref& i : sample->items) items.push_back(i->kind == py::Kind::String ? py::makeText("") : i);
        value = py::makeTuple(items);
    }
    Step step;
    step.label = "add";
    for (int g : m.gendered ? std::vector<int>{1, 2} : std::vector<int>{0}) {
        py::Ref key = keyOf(m, next, g);
        step.changes.push_back(Change{m.module, key, nullptr, value});
        put(m.module, key, value);
    }
    record(std::move(step), false);
    scanModules();
    selectedModule_ = module_;
    selectedId_ = next;
    sameForBoth_ = true;
    filter_[0] = 0;
    rowsDirty_ = true;
    scrollToSelection_ = true;
    ctx_.setStatus("Added " + m.label + " #" + std::to_string(next), false);
}

void TextWorkspace::deleteEntry() {
    if (selectedModule_ < 0) return;
    const ModuleInfo& m = modules_[size_t(selectedModule_)];
    Step step;
    step.label = "delete";
    for (int g : m.gendered ? std::vector<int>{1, 2} : std::vector<int>{0}) {
        const py::Object* v = valueOf(m, selectedId_, g);
        if (!v) continue;
        py::Ref key = keyOf(m, selectedId_, g);
        step.changes.push_back(Change{m.module, key, py::dictGetRef(*lookupOf(m.module), *key), nullptr});
        put(m.module, key, nullptr);
    }
    record(std::move(step), false);
    scanModules();
    ctx_.setStatus("Deleted " + m.label + " #" + std::to_string(selectedId_), false);
    selectedModule_ = -1;
}

// ------------------------------------------------------------------------------------------
// Edit menu, shortcuts, save
// ------------------------------------------------------------------------------------------
void TextWorkspace::handleShortcuts() {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal) && hasUnsavedChanges()) saveChanges();
    if (ImGui::GetIO().WantTextInput) return;                 // the text field's own undo while typing
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
        redo();
}

void TextWorkspace::drawEditMenu() {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_.empty())) undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_.empty())) redo();
    ImGui::Separator();
    if (ImGui::MenuItem("Save Text Changes", "Ctrl+S", false, hasUnsavedChanges())) saveChanges();
    if (ImGui::MenuItem("Revert Text Changes", nullptr, false, hasUnsavedChanges())) revertChanges();
}

void TextWorkspace::revertChanges() {
    edited_.clear();
    undo_.clear();
    redo_.clear();
    version_ = savedVersion_ = 0;
    scanModules();
    rowsDirty_ = true;
}

bool TextWorkspace::saveChanges() {
    if (!hasUnsavedChanges()) return true;
    if (!ctx_.gameData) return false;
    namespace fs = std::filesystem;
    tr::GameData& data = *ctx_.gameData;
    try {
        tr::ZipArchive archive(data.path());
        std::map<std::string, tr::Bytes> replace;
        std::vector<std::string> remove;
        std::map<std::string, py::Ref> updated;
        for (const auto& e : edited_) {
            const std::string entry = data.moduleEntryName(e.first);
            if (entry.empty()) continue;
            const std::string base = entry.substr(0, entry.rfind('.'));
            const std::string source = py::moduleSource(*e.second);
            replace[base + ".py"] = tr::Bytes(source.begin(), source.end());
            // Python's zip import takes a compiled module before the source: drop every compiled copy.
            for (const tr::ZipEntry& z : archive.entries())
                if (lowerAscii(z.name) == lowerAscii(base + ".pyo") || lowerAscii(z.name) == lowerAscii(base + ".pyc")) remove.push_back(z.name);
            updated[e.first] = e.second;
        }
        if (!savePathForTests_.empty()) {
            tr::writeZip(archive, savePathForTests_, replace, remove);
            savedVersion_ = version_;
            ctx_.setStatus("Saved " + std::to_string(updated.size()) + " text module(s) to " + savePathForTests_, false);
            return true;
        }
        const fs::path path(data.path());
        fs::path backup = path;
        backup += ".rasaedit-original";
        std::error_code ec;
        if (!fs::exists(backup, ec)) fs::copy_file(path, backup, ec);
        if (ec) throw tr::FormatError("cannot keep the original as " + backup.string() + ": " + ec.message());
        fs::path temp = path;
        temp += ".rasaedit-new";
        tr::writeZip(archive, temp.string(), replace, remove);
        { tr::ZipArchive check(temp.string()); }              // reads back (a broken write throws here, before the swap)
        fs::rename(temp, path, ec);
        if (ec) throw tr::FormatError("cannot replace " + path.string() + " (is the game running?): " + ec.message());
        data.reloadArchive(updated);
        edited_.clear();
        savedVersion_ = version_;
        scanModules();
        rowsDirty_ = true;
        ctx_.setStatus("Saved " + std::to_string(updated.size()) + " text module(s) to " + path.filename().string(), false);
        return true;
    } catch (const std::exception& e) {
        ctx_.setStatus(std::string("Text save failed: ") + e.what(), true);
        return false;
    }
}
