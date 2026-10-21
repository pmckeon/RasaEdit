#include "ModuleEditor.h"

#include "EditorContext.h"
#include "Lookups.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace {

int columnOf(const DbTable& t, const char* name) {
    const int c = t.column(name);
    if (c < 0) throw std::runtime_error(t.name + " has no column " + name);
    return c;
}
const DbValue& at(const std::vector<DbValue>& row, const DbTable& t, const char* name) { return row[size_t(t.column(name))]; }
int64_t intAt(const std::vector<DbValue>& row, const DbTable& t, const char* name) { return at(row, t, name).asInt(); }
double realAt(const std::vector<DbValue>& row, const DbTable& t, const char* name) { return at(row, t, name).asReal(); }
void setValue(std::vector<DbValue>& row, const DbTable& t, const char* name, DbValue v) { row[size_t(t.column(name))] = std::move(v); }

std::vector<DbValue> newRow(const DbTable& t) { return std::vector<DbValue>(t.columns.size()); }
int64_t nextId(const DbTable& t) {
    int64_t best = 0;
    for (const auto& row : t.rows) best = std::max(best, intAt(row, t, "id"));
    return best + 1;
}

const char* classSetName(int64_t set) {
    switch (set) {
        case 0: return "none";
        case 212: return "armor";
        case 1212: return "weapons";
        case 1273: return "tools";
        case 1274: return "salvage only";
        default: return "unusual";
    }
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
}

// A column that names something, picked by name (Lookups.h); true when changed.
bool lookupColumn(const char* label, std::vector<DbValue>& row, const DbTable& t, const char* column, const LookupSource& source, float width,
                  const char* tooltip = nullptr) {
    int64_t v = row[size_t(t.column(column))].asInt();
    ImGui::SetNextItemWidth(width);
    const bool changed = lookupCombo(label, v, source);
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    if (changed) row[size_t(t.column(column))] = DbValue::integer(v);
    return changed;
}
// An integer column as an input; true when changed.
bool intColumn(const char* label, std::vector<DbValue>& row, const DbTable& t, const char* column, int64_t minimum, int64_t maximum, float width = 140.0f,
               const char* tooltip = nullptr) {
    long long v = intAt(row, t, column);
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::InputScalar(label, ImGuiDataType_S64, &v);
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    if (!changed) return false;
    setValue(row, t, column, DbValue::integer(std::clamp<long long>(v, minimum, maximum)));
    return true;
}
bool realColumn(const char* label, std::vector<DbValue>& row, const DbTable& t, const char* column, float width) {
    double v = realAt(row, t, column);
    ImGui::SetNextItemWidth(width);
    if (!ImGui::InputDouble(label, &v, 0.0, 0.0, "%.4g")) return false;
    setValue(row, t, column, DbValue::real(v));
    return true;
}
// A nullable integer column as text: empty = NULL.
bool nullableIntColumn(const char* label, std::vector<DbValue>& row, const DbTable& t, const char* column, float width) {
    const DbValue& v = at(row, t, column);
    char buffer[32] = "";
    if (!v.isNull()) std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(v.asInt()));
    ImGui::SetNextItemWidth(width);
    if (!ImGui::InputTextWithHint(label, "None", buffer, sizeof buffer, ImGuiInputTextFlags_CharsDecimal)) return false;
    setValue(row, t, column, buffer[0] ? DbValue::integer(std::strtoll(buffer, nullptr, 10)) : DbValue{});
    return true;
}

}  // namespace

void ModuleEditor::load(const WorldDatabase& database) {
    clear();
    if (!database.hasTable("module_class") || !database.hasTable("module_effect") || !database.hasTable("module_item") ||
        !database.hasTable("modifiable_class"))
        return;                                       // an older world without item modules
    Table* tables[] = {&classes_, &effects_, &items_, &modifiable_};
    const char* names[] = {"module_class", "module_effect", "module_item", "modifiable_class"};
    for (int k = 0; k < 4; ++k) {
        tables[k]->saved = database.loadTable(names[k]);
        tables[k]->edited = tables[k]->saved;
    }
    for (const char* c : {"id", "variant_id", "level", "class_set_id", "item_template_id", "item_class_id", "extract_cost", "integrate_cost",
                          "salvage_gain", "upgrade_cost", "upgrade_module_id", "comment"})
        columnOf(classes_.saved, c);
    for (const char* c : {"id", "module_id", "effect_id", "set_level", "flat_value", "linear_value", "exp_value", "arg1", "arg2", "arg3", "arg4"})
        columnOf(effects_.saved, c);
    for (const char* c : {"id", "module_id", "strength"}) columnOf(items_.saved, c);
    for (const char* c : {"id", "class_set_id"}) columnOf(modifiable_.saved, c);
    loaded_ = true;
}

void ModuleEditor::clear() {
    loaded_ = false;
    classes_ = effects_ = items_ = modifiable_ = Table{};
    selected_ = -1;
}

bool ModuleEditor::changed() const {
    return loaded_ && (classes_.saved.rows != classes_.edited.rows || effects_.saved.rows != effects_.edited.rows ||
                       items_.saved.rows != items_.edited.rows || modifiable_.saved.rows != modifiable_.edited.rows);
}

void ModuleEditor::revert() {
    for (Table* t : {&classes_, &effects_, &items_, &modifiable_}) t->edited = t->saved;
}

int ModuleEditor::moduleRow(int64_t moduleId) const {
    const DbTable& t = classes_.edited;
    for (size_t k = 0; k < t.rows.size(); ++k)
        if (intAt(t.rows[k], t, "id") == moduleId) return int(k);
    return -1;
}

std::vector<size_t> ModuleEditor::effectRows(int64_t moduleId) const {
    std::vector<size_t> rows;
    const DbTable& t = effects_.edited;
    for (size_t k = 0; k < t.rows.size(); ++k)
        if (intAt(t.rows[k], t, "module_id") == moduleId) rows.push_back(k);
    // Sent in the order of their ids (Rasa.NET ModuleEffectEntry).
    std::sort(rows.begin(), rows.end(), [&](size_t a, size_t b) { return intAt(t.rows[a], t, "id") < intAt(t.rows[b], t, "id"); });
    return rows;
}

int64_t ModuleEditor::effectAmount(double flat, double linear, double exp, int level) {
    // tooltipwindow.py: int(math.ceil(flatValue + linearValue * itemLevel + expValue * pow(2, (itemLevel - 1) / 8.0)))
    return int64_t(std::ceil(flat + linear * double(level) + exp * std::pow(2.0, double(level - 1) / 8.0)));
}

std::string ModuleEditor::effectLine(EditorContext& ctx, size_t effectRow, int level) const {
    const DbTable& t = effects_.edited;
    if (effectRow >= t.rows.size()) return "";
    const auto& row = t.rows[effectRow];
    std::string text;
    if (ctx.gameData) {
        try { text = ctx.gameData->effectTooltip(int32_t(intAt(row, t, "effect_id"))); } catch (const std::exception&) {}
    }
    if (text.empty()) return "(the effect has no tooltip: the client draws no line)";
    const int64_t setLevel = intAt(row, t, "set_level");
    const int moduleIndex = moduleRow(intAt(row, t, "module_id"));
    const int64_t moduleLevel = moduleIndex >= 0 ? intAt(classes_.edited.rows[size_t(moduleIndex)], classes_.edited, "level") : 0;
    // The prefix: a set bonus "(n) ", else the module's level "[n] " (GetModuleLevel), else "[+] " (UNVERIFIED: that level 0 is
    // the client's None).
    const std::string prefix = setLevel ? "(" + std::to_string(setLevel) + ") " : moduleLevel > 0 ? "[" + std::to_string(moduleLevel) + "] " : "[+] ";
    const int64_t amount = effectAmount(realAt(row, t, "flat_value"), realAt(row, t, "linear_value"), realAt(row, t, "exp_value"), level);
    replaceAll(text, "%(amount)s", (amount > 0 ? "+" : "") + std::to_string(amount));
    for (int k = 1; k <= 4; ++k) {
        const std::string name = "arg" + std::to_string(k);
        const DbValue& v = at(row, t, name.c_str());
        replaceAll(text, "%(" + name + ")s", v.isNull() ? "None" : std::to_string(v.asInt()));
    }
    replaceAll(text, "%%", "%");
    return prefix + text;
}

std::vector<ModuleEditor::Problem> ModuleEditor::validate() const {
    std::vector<Problem> out;
    if (!loaded_) return out;
    std::set<int64_t> modules;
    for (const auto& row : classes_.edited.rows) modules.insert(intAt(row, classes_.edited, "id"));
    for (const auto& row : effects_.edited.rows) {
        const int64_t m = intAt(row, effects_.edited, "module_id");
        if (!modules.count(m))
            out.push_back({true, "module_effect " + std::to_string(intAt(row, effects_.edited, "id")) + ": module " + std::to_string(m) +
                                     " is not in module_class (the server logs and skips it)"});
    }
    for (const auto& row : items_.edited.rows) {
        const int64_t m = intAt(row, items_.edited, "module_id");
        if (!modules.count(m))
            out.push_back({true, "module_item " + std::to_string(intAt(row, items_.edited, "id")) + ": module " + std::to_string(m) + " is not in module_class"});
        const int64_t s = intAt(row, items_.edited, "strength");
        if (s < 1 || s > 5)
            out.push_back({false, "module_item " + std::to_string(intAt(row, items_.edited, "id")) + ": strength " + std::to_string(s) + " is not 1 - 5"});
    }
    for (const auto& row : classes_.edited.rows) {
        const std::string id = std::to_string(intAt(row, classes_.edited, "id"));
        const int64_t upgrade = intAt(row, classes_.edited, "upgrade_module_id");
        if (upgrade != 0 && !modules.count(upgrade))
            out.push_back({false, "module " + id + ": its upgrade, module " + std::to_string(upgrade) + ", does not exist (no upgrade)"});
        const int64_t set = intAt(row, classes_.edited, "class_set_id");
        if (std::string(classSetName(set)) == "unusual")
            out.push_back({false, "module " + id + ": class set " + std::to_string(set) + " is none of 212 / 1212 / 1273 / 1274 (no item takes it)"});
        const int64_t level = intAt(row, classes_.edited, "level");
        if (level < 0 || level > 5) out.push_back({false, "module " + id + ": level " + std::to_string(level) + " is not 0 - 5"});
        if (classes_.edited.rows.size() && at(row, classes_.edited, "comment").t.size() > 64)
            out.push_back({false, "module " + id + ": comment longer than its column (varchar 64)"});
    }
    for (const auto& row : modifiable_.edited.rows) {
        const int64_t set = intAt(row, modifiable_.edited, "class_set_id");
        if (set != 212 && set != 1212 && set != 1273)
            out.push_back({false, "modifiable class " + std::to_string(intAt(row, modifiable_.edited, "id")) + ": class set " + std::to_string(set) +
                                      " - no module goes into it (modules use 212 / 1212 / 1273)"});
    }
    return out;
}

bool ModuleEditor::save(WorldDatabase& database, std::string& message) {
    std::vector<std::string> errors;
    for (const Problem& p : validate())
        if (p.error) errors.push_back(p.message);
    if (!errors.empty()) {
        message = "Modules not saved: " + std::to_string(errors.size()) + " error(s), first: " + errors.front();
        return false;
    }
    WorldDatabase::SaveSummary summary;
    std::string error;
    if (!database.saveTables({{&classes_.saved, &classes_.edited}, {&effects_.saved, &effects_.edited}, {&items_.saved, &items_.edited},
                              {&modifiable_.saved, &modifiable_.edited}},
                             summary, error)) {
        message = "Saving modules failed, nothing was written: " + error;
        return false;
    }
    const int64_t keep = selected_;
    load(database);
    selected_ = keep;
    message = "Modules saved: " + std::to_string(summary.inserted) + " added, " + std::to_string(summary.updated) + " changed, " +
              std::to_string(summary.deleted) + " deleted (the server reads them at start-up).";
    return true;
}

// ------------------------------------------------------------------------------------------
// Panel
// ------------------------------------------------------------------------------------------
void ModuleEditor::draw(EditorContext& ctx, const WorldDatabase& database, bool writable, int part) {
    if (!loaded_) {
        ImGui::TextDisabled("This world has no item module tables (module_class, module_effect, module_item, modifiable_class).");
        return;
    }
    const std::vector<Problem> problems = validate();
    size_t errors = 0;
    for (const Problem& p : problems) errors += p.error ? 1 : 0;
    if (writable) {
        const bool dirty = changed();
        ImGui::BeginDisabled(!dirty);
        if (ImGui::Button("Save modules")) {
            std::string message;
            const bool ok = save(const_cast<WorldDatabase&>(database), message);
            ctx.setStatus(message, !ok);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) revert();
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    ImGui::TextDisabled("%zu modules, %zu effects, %zu module items, %zu modifiable classes", classes_.edited.rows.size(), effects_.edited.rows.size(),
                        items_.edited.rows.size(), modifiable_.edited.rows.size());
    if (!problems.empty() && ImGui::TreeNode("problems", "%zu error(s), %zu warning(s)", errors, problems.size() - errors)) {
        for (const Problem& p : problems)
            ImGui::TextColored(p.error ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", p.message.c_str());
        ImGui::TreePop();
    }
    if (part == 1) {
        drawModifiable(ctx, writable);
        return;
    }
    {
        {
            ImGui::SetNextItemWidth(220);
            ImGui::InputTextWithHint("##filter", "filter (id, comment, set name)", filter_, sizeof filter_);
            if (writable) {
                ImGui::SameLine();
                if (ImGui::Button("New module")) {
                    DbTable& t = classes_.edited;
                    auto row = newRow(t);
                    const int64_t id = nextId(t);
                    for (const char* c : {"variant_id", "level", "class_set_id", "item_template_id", "item_class_id", "extract_cost", "integrate_cost",
                                          "salvage_gain", "upgrade_cost", "upgrade_module_id"})
                        setValue(row, t, c, DbValue::integer(0));
                    setValue(row, t, "id", DbValue::integer(id));
                    setValue(row, t, "comment", DbValue::text("New module"));
                    t.rows.push_back(row);
                    selected_ = id;
                }
                ImGui::SameLine();
                const int row = moduleRow(selected_);
                ImGui::BeginDisabled(row < 0);
                if (ImGui::Button("Delete module") && row >= 0) {
                    // Its effects and module items go with it (nothing else in the world tables names a module).
                    const int64_t id = selected_;
                    auto& e = effects_.edited.rows;
                    e.erase(std::remove_if(e.begin(), e.end(), [&](const auto& r) { return intAt(r, effects_.edited, "module_id") == id; }), e.end());
                    auto& i = items_.edited.rows;
                    i.erase(std::remove_if(i.begin(), i.end(), [&](const auto& r) { return intAt(r, items_.edited, "module_id") == id; }), i.end());
                    classes_.edited.rows.erase(classes_.edited.rows.begin() + row);
                    selected_ = -1;
                    ctx.setStatus("Module " + std::to_string(id) + " deleted with its effects and module items (not saved yet). Items players hold "
                                  "may still carry its id in a slot (character database).", false);
                }
                ImGui::EndDisabled();
            }
            // The list.
            const DbTable& t = classes_.edited;
            const std::string needle = lower(filter_);
            std::vector<size_t> shown;
            for (size_t k = 0; k < t.rows.size(); ++k) {
                if (needle.empty()) {
                    shown.push_back(k);
                    continue;
                }
                const int64_t id = intAt(t.rows[k], t, "id");
                std::string hay = std::to_string(id) + " " + at(t.rows[k], t, "comment").t;
                if (ctx.gameData) {
                    try { hay += " " + ctx.gameData->moduleSetName(int32_t(id)); } catch (const std::exception&) {}
                }
                if (lower(hay).find(needle) != std::string::npos) shown.push_back(k);
            }
            std::sort(shown.begin(), shown.end(), [&](size_t a, size_t b) { return intAt(t.rows[a], t, "id") < intAt(t.rows[b], t, "id"); });
            ImGui::BeginChild("modulelist", ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.3f)), ImGuiChildFlags_Borders);
            ImGuiListClipper clipper;
            clipper.Begin(int(shown.size()));
            while (clipper.Step())
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                    const auto& row = t.rows[shown[size_t(r)]];
                    const int64_t id = intAt(row, t, "id");
                    char label[160];
                    std::snprintf(label, sizeof label, "%lld  %s  (variant %lld, level %lld, %s)", static_cast<long long>(id), at(row, t, "comment").t.c_str(),
                                  static_cast<long long>(intAt(row, t, "variant_id")), static_cast<long long>(intAt(row, t, "level")),
                                  classSetName(intAt(row, t, "class_set_id")));
                    ImGui::PushID(int(id));
                    if (ImGui::Selectable(label, id == selected_)) selected_ = id;
                    ImGui::PopID();
                }
            ImGui::EndChild();
            const int row = moduleRow(selected_);
            if (row >= 0) {
                ImGui::BeginChild("moduledetail");
                drawModule(ctx, database, row, writable);
                ImGui::EndChild();
            } else ImGui::TextDisabled("Choose a module.");
        }
    }
}

void ModuleEditor::drawModule(EditorContext& ctx, const WorldDatabase& database, int rowIndex, bool writable) {
    DbTable& t = classes_.edited;
    auto& row = t.rows[size_t(rowIndex)];
    const int64_t id = intAt(row, t, "id");
    ImGui::BeginDisabled(!writable);
    ImGui::Text("Module %lld", static_cast<long long>(id));
    std::string setName;
    if (ctx.gameData) {
        try { setName = ctx.gameData->moduleSetName(int32_t(id)); } catch (const std::exception&) {}
    }
    if (!setName.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", setName.c_str());
        ImGui::SetItemTooltip("The client's set name (modulesetnamelanguage), drawn above the set's lines");
    }
    {
        char buffer[65];
        std::snprintf(buffer, sizeof buffer, "%s", at(row, t, "comment").t.c_str());
        ImGui::SetNextItemWidth(-120);
        if (ImGui::InputText("Comment", buffer, sizeof buffer)) setValue(row, t, "comment", DbValue::text(buffer));
    }
    intColumn("Variant", row, t, "variant_id", 0, 1000, 140, "ModuleVariantId 1 - 54: the kind of bonus (Body, Fire Resistance, Steal Health); an item takes one "
                                                             "module of a variant. 0 for sets and the others with none.");
    intColumn("Level", row, t, "level", 0, 5, 140, "The strength, 1 (Basic) to 5 (Exceptional), printed before each line as [n]; 0 for a module with no item.");
    {
        const int64_t set = intAt(row, t, "class_set_id");
        static const int64_t sets[] = {0, 212, 1212, 1273, 1274};
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("Class set", (std::to_string(set) + " " + classSetName(set)).c_str())) {
            for (int64_t s : sets)
                if (ImGui::Selectable((std::to_string(s) + " " + classSetName(s)).c_str(), s == set)) setValue(row, t, "class_set_id", DbValue::integer(s));
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The item classes it goes into (modifiable_class): 212 armor, 1212 weapons, 1273 tools, 1274 only salvaged, 0 none.");
    }
    lookupColumn("Item template", row, t, "item_template_id", lookup::itemTemplate(ctx, database, "(none)"), 320,
                 "ModuleItemTemplateId: the module as an item in a pack (what an extraction makes).");
    lookupColumn("Item class", row, t, "item_class_id", lookup::entityClass(ctx, "(none)"), 320, "That item's entity class.");
    intColumn("Extract cost", row, t, "extract_cost", 0, INT32_MAX, 140, "MimeogelCostToExtract; 0: it cannot be taken out again.");
    intColumn("Integrate cost", row, t, "integrate_cost", 0, INT32_MAX, 140, "MimeogelCostToIntegrate, before the client's formula scales it.");
    intColumn("Salvage gain", row, t, "salvage_gain", 0, INT32_MAX, 140, "MimeogelGainFromSalvage.");
    intColumn("Upgrade cost", row, t, "upgrade_cost", 0, INT32_MAX, 140, "MimeogelCostToUpgrade; 0: it cannot be upgraded.");
    {
        // The modules of this table (as edited), by comment.
        LookupSource modules;
        modules.none = "(none)";
        modules.name = [this](int64_t id) -> std::string {
            const int u = moduleRow(id);
            return u < 0 ? std::string() : at(classes_.edited.rows[size_t(u)], classes_.edited, "comment").t;
        };
        modules.fill = [this](LookupList& out) {
            for (const auto& r : classes_.edited.rows) out.push_back(LookupChoice{intAt(r, classes_.edited, "id"), at(r, classes_.edited, "comment").t});
        };
        lookupColumn("Upgrade to", row, t, "upgrade_module_id", modules, 320, "UpgradeModuleClassId: the module an upgrade makes of this one.");
    }

    // Effects.
    ImGui::SeparatorText("Effects (module_effect, in id order)");
    if (!effectNamesRead_ && ctx.gameData) {
        effectNamesRead_ = true;
        try { effectNames_ = ctx.gameData->moduleConstants("generated.client.gameeffectdata"); } catch (const std::exception&) {}
    }
    const auto effectName = [&](int64_t effect) {
        for (const auto& [value, name] : effectNames_)
            if (value == effect) return name;
        return std::string("effect ") + std::to_string(effect);
    };
    DbTable& e = effects_.edited;
    const std::vector<size_t> rows = effectRows(id);
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Item level", &previewLevel_, 0, 50);
    ImGui::SetItemTooltip("An item's level requirement (0 for none): the amount is ceil(flat + linear L + exp 2^((L - 1) / 8)).");
    int removeRow = -1;
    if (ImGui::BeginTable("effects", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                          ImVec2(0, ImGui::GetFrameHeightWithSpacing() * float(rows.size() + 1) + ImGui::GetStyle().ScrollbarSize + 4))) {
        const float em = ImGui::GetFontSize();
        ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 3.5f * em);
        ImGui::TableSetupColumn("effect", ImGuiTableColumnFlags_WidthFixed, 14 * em);
        ImGui::TableSetupColumn("set", ImGuiTableColumnFlags_WidthFixed, 3 * em);
        ImGui::TableSetupColumn("flat", ImGuiTableColumnFlags_WidthFixed, 4.5f * em);
        ImGui::TableSetupColumn("linear", ImGuiTableColumnFlags_WidthFixed, 4.5f * em);
        ImGui::TableSetupColumn("exp", ImGuiTableColumnFlags_WidthFixed, 4.5f * em);
        ImGui::TableSetupColumn("args (empty: None)", ImGuiTableColumnFlags_WidthFixed, 14 * em);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("line", ImGuiTableColumnFlags_WidthFixed, 22 * em);
        ImGui::TableHeadersRow();
        for (size_t r : rows) {
            auto& er = e.rows[r];
            ImGui::PushID(int(intAt(er, e, "id")));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(intAt(er, e, "id")));
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const int64_t effect = intAt(er, e, "effect_id");
            if (ImGui::BeginCombo("##effect", (std::to_string(effect) + " " + effectName(effect)).c_str(), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::IsWindowAppearing()) {
                    effectFilter_[0] = 0;
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::InputTextWithHint("##f", "filter", effectFilter_, sizeof effectFilter_);
                const std::string f = lower(effectFilter_);
                size_t listed = 0;
                for (const auto& [value, name] : effectNames_) {
                    const std::string label = std::to_string(value) + " " + name;
                    if (!f.empty() && lower(label).find(f) == std::string::npos) continue;
                    if (++listed > 400) {
                        ImGui::TextDisabled("(more: filter)");
                        break;
                    }
                    if (ImGui::Selectable(label.c_str(), value == effect)) setValue(er, e, "effect_id", DbValue::integer(value));
                }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            intColumn("##set", er, e, "set_level", 0, 99, -FLT_MIN);
            ImGui::TableNextColumn();
            realColumn("##flat", er, e, "flat_value", -FLT_MIN);
            ImGui::TableNextColumn();
            realColumn("##linear", er, e, "linear_value", -FLT_MIN);
            ImGui::TableNextColumn();
            realColumn("##exp", er, e, "exp_value", -FLT_MIN);
            ImGui::TableNextColumn();
            for (int k = 1; k <= 4; ++k) {
                if (k > 1) ImGui::SameLine();
                const std::string column = "arg" + std::to_string(k);
                nullableIntColumn(("##" + column).c_str(), er, e, column.c_str(), 3.1f * em);
            }
            ImGui::TableNextColumn();
            if (writable && ImGui::SmallButton("x")) removeRow = int(r);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(effectLine(ctx, r, previewLevel_).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeRow >= 0) e.rows.erase(e.rows.begin() + removeRow);
    if (writable && ImGui::Button("Add effect")) {
        auto er = newRow(e);
        setValue(er, e, "id", DbValue::integer(nextId(e)));
        setValue(er, e, "module_id", DbValue::integer(id));
        setValue(er, e, "effect_id", DbValue::integer(rows.empty() ? 0 : intAt(e.rows[rows.back()], e, "effect_id")));
        setValue(er, e, "set_level", DbValue::integer(0));
        for (const char* c : {"flat_value", "linear_value", "exp_value"}) setValue(er, e, c, DbValue::real(0));
        e.rows.push_back(er);
    }

    // Module items.
    ImGui::SeparatorText("Module items (module_item: item templates that are this module)");
    DbTable& it = items_.edited;
    int removeItem = -1;
    size_t count = 0;
    for (size_t k = 0; k < it.rows.size(); ++k) {
        auto& ir = it.rows[k];
        if (intAt(ir, it, "module_id") != id) continue;
        if (++count > 200) {
            ImGui::TextDisabled("(more)");
            break;
        }
        ImGui::PushID(int(k));
        if (writable && ImGui::SmallButton("x")) removeItem = int(k);
        ImGui::SameLine();
        const int32_t templ = int32_t(intAt(ir, it, "id"));
        const ItemTemplateInfo* info = database.itemTemplate(templ);
        std::string name;
        if (info && ctx.gameData) {
            try { name = ctx.gameData->itemName(templ, info->classId); } catch (const std::exception&) {}
        }
        ImGui::Text("%d %s", templ, info ? name.c_str() : "(no such item template)");
        ImGui::SameLine();
        intColumn("strength", ir, it, "strength", 1, 5, 80);
        ImGui::PopID();
    }
    if (removeItem >= 0) it.rows.erase(it.rows.begin() + removeItem);
    if (count == 0) ImGui::TextDisabled("None.");
    if (writable) {
        ImGui::SetNextItemWidth(320);
        lookupCombo("##newtemplate", newTemplate_, lookup::itemTemplate(ctx, database, "(choose an item)"));
        ImGui::SameLine();
        bool taken = false;
        for (const auto& ir : it.rows) taken = taken || intAt(ir, it, "id") == newTemplate_;
        ImGui::BeginDisabled(newTemplate_ <= 0 || taken || !database.itemTemplate(newTemplate_));
        if (ImGui::Button("Add module item")) {
            auto ir = newRow(it);
            setValue(ir, it, "id", DbValue::integer(newTemplate_));
            setValue(ir, it, "module_id", DbValue::integer(id));
            setValue(ir, it, "strength", DbValue::integer(std::clamp<int64_t>(intAt(row, t, "level"), 1, 5)));
            it.rows.push_back(ir);
            newTemplate_ = 0;
        }
        ImGui::EndDisabled();
        if (taken) {
            ImGui::SameLine();
            ImGui::TextDisabled("(already a module item: one module a template)");
        }
    }
    ImGui::EndDisabled();
}

void ModuleEditor::drawModifiable(EditorContext& ctx, bool writable) {
    DbTable& t = modifiable_.edited;
    ImGui::TextDisabled("Item classes that take modules, with their class set; a module goes into the classes of its own set.");
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##classfilter", "filter (id or class name)", classFilter_, sizeof classFilter_);
    if (writable) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        lookupCombo("##newclass", newClass_, lookup::entityClass(ctx, "(choose a class)"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        static const int32_t sets[] = {212, 1212, 1273};
        if (ImGui::BeginCombo("##newset", (std::to_string(newClassSet_) + " " + classSetName(newClassSet_)).c_str())) {
            for (int32_t s : sets)
                if (ImGui::Selectable((std::to_string(s) + " " + classSetName(s)).c_str(), s == newClassSet_)) newClassSet_ = s;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        bool taken = false;
        for (const auto& r : t.rows) taken = taken || intAt(r, t, "id") == newClass_;
        ImGui::BeginDisabled(newClass_ <= 0 || taken || !ctx.classInfo(newClass_).known);
        if (ImGui::Button("Add class")) {
            auto r = newRow(t);
            setValue(r, t, "id", DbValue::integer(newClass_));
            setValue(r, t, "class_set_id", DbValue::integer(newClassSet_));
            t.rows.push_back(r);
            newClass_ = 0;
        }
        ImGui::EndDisabled();
    }
    const std::string needle = lower(classFilter_);
    std::vector<size_t> shown;
    for (size_t k = 0; k < t.rows.size(); ++k) {
        const int32_t id = int32_t(intAt(t.rows[k], t, "id"));
        if (needle.empty() || lower(std::to_string(id) + " " + ctx.classInfo(id).name).find(needle) != std::string::npos) shown.push_back(k);
    }
    std::sort(shown.begin(), shown.end(), [&](size_t a, size_t b) { return intAt(t.rows[a], t, "id") < intAt(t.rows[b], t, "id"); });
    int remove = -1;
    ImGui::BeginChild("classes");
    ImGuiListClipper clipper;
    clipper.Begin(int(shown.size()));
    while (clipper.Step())
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            auto& row = t.rows[shown[size_t(r)]];
            const int32_t id = int32_t(intAt(row, t, "id"));
            ImGui::PushID(id);
            if (writable && ImGui::SmallButton("x")) remove = int(shown[size_t(r)]);
            ImGui::SameLine();
            const int64_t set = intAt(row, t, "class_set_id");
            ImGui::Text("%d %s - %lld %s", id, ctx.classInfo(id).name.c_str(), static_cast<long long>(set), classSetName(set));
            ImGui::PopID();
        }
    ImGui::EndChild();
    if (remove >= 0) t.rows.erase(t.rows.begin() + remove);
}
