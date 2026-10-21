#include "RecipeEditor.h"

#include "EditorContext.h"
#include "Lookups.h"
#include "gamedata.h"

#include "imgui.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace {

int columnOf(const DbTable& t, const char* name) {
    const int c = t.column(name);
    if (c < 0) throw std::runtime_error(t.name + " has no column " + name);
    return c;
}
int64_t intAt(const std::vector<DbValue>& row, const DbTable& t, const char* name) { return row[size_t(t.column(name))].asInt(); }
void setInt(std::vector<DbValue>& row, const DbTable& t, const char* name, int64_t v) { row[size_t(t.column(name))] = DbValue::integer(v); }
int64_t nextId(const DbTable& t) {
    int64_t best = 0;
    for (const auto& row : t.rows) best = std::max(best, intAt(row, t, "id"));
    return best + 1;
}
bool intColumn(const char* label, std::vector<DbValue>& row, const DbTable& t, const char* column, int64_t minimum, int64_t maximum, float width,
               const char* tooltip = nullptr) {
    long long v = intAt(row, t, column);
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::InputScalar(label, ImGuiDataType_S64, &v);
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    if (!changed) return false;
    setInt(row, t, column, std::clamp<long long>(v, minimum, maximum));
    return true;
}
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string itemLabel(EditorContext& ctx, const WorldDatabase& database, int32_t templateId) {
    const ItemTemplateInfo* info = database.itemTemplate(templateId);
    if (!info) return "(not an item template)";
    std::string name;
    if (ctx.gameData) {
        try { name = ctx.gameData->itemName(templateId, info->classId); } catch (const std::exception&) {}
    }
    return name.empty() ? ctx.classInfo(info->classId).name : name;
}

// The client's recipes, read once per game data.
const std::vector<tr::ClientRecipe>& clientRecipes(EditorContext& ctx) {
    static const tr::GameData* readFrom = nullptr;
    static std::vector<tr::ClientRecipe> recipes;
    if (ctx.gameData.get() != readFrom) {
        readFrom = ctx.gameData.get();
        recipes.clear();
        if (ctx.gameData) {
            try { recipes = ctx.gameData->clientRecipes(); } catch (const std::exception&) {}
        }
    }
    return recipes;
}
const tr::ClientRecipe* clientRecipe(const std::vector<tr::ClientRecipe>& client, int64_t templateId) {
    for (const tr::ClientRecipe& r : client)
        if (r.templateId == templateId) return &r;
    return nullptr;
}

}  // namespace

void RecipeEditor::load(const WorldDatabase& database) {
    clear();
    if (!database.hasTable("recipe") || !database.hasTable("recipe_input")) return;
    recipes_.saved = database.loadTable("recipe");
    inputs_.saved = database.loadTable("recipe_input");
    recipes_.edited = recipes_.saved;
    inputs_.edited = inputs_.saved;
    for (const char* c : {"id", "energy_cost", "kraftwerks_seconds", "result_template_id", "result_amount", "min_level"}) columnOf(recipes_.saved, c);
    for (const char* c : {"id", "recipe_id", "input_class_id", "quantity"}) columnOf(inputs_.saved, c);
    loaded_ = true;
}

void RecipeEditor::clear() {
    loaded_ = false;
    recipes_ = inputs_ = Table{};
    selected_ = -1;
}

bool RecipeEditor::changed() const { return loaded_ && (recipes_.saved.rows != recipes_.edited.rows || inputs_.saved.rows != inputs_.edited.rows); }

void RecipeEditor::revert() {
    recipes_.edited = recipes_.saved;
    inputs_.edited = inputs_.saved;
}

int RecipeEditor::recipeRow(int64_t templateId) const {
    const DbTable& t = recipes_.edited;
    for (size_t k = 0; k < t.rows.size(); ++k)
        if (intAt(t.rows[k], t, "id") == templateId) return int(k);
    return -1;
}

std::vector<std::string> RecipeEditor::errors(const WorldDatabase& database) const {
    std::vector<std::string> out;
    if (!loaded_) return out;
    const DbTable& t = recipes_.edited;
    for (const auto& row : t.rows) {
        const int64_t id = intAt(row, t, "id"), result = intAt(row, t, "result_template_id");
        if (!database.itemTemplate(int32_t(id))) out.push_back("recipe " + std::to_string(id) + ": the schematic is not an item template (the server skips it)");
        if (!database.itemTemplate(int32_t(result)))
            out.push_back("recipe " + std::to_string(id) + ": result template " + std::to_string(result) + " is not an item template (the server skips it)");
    }
    return out;
}

std::vector<std::string> RecipeEditor::differences(int64_t templateId, const std::vector<tr::ClientRecipe>& client) const {
    std::vector<std::string> out;
    const int row = recipeRow(templateId);
    const tr::ClientRecipe* c = clientRecipe(client, templateId);
    if (row < 0) return out;
    if (!c) {
        out.push_back("the client has no such recipe");
        return out;
    }
    const DbTable& t = recipes_.edited;
    const auto& r = t.rows[size_t(row)];
    const auto field = [&](const char* name, const char* column, int64_t clientValue) {
        const int64_t v = intAt(r, t, column);
        if (v != clientValue) out.push_back(std::string(name) + " " + std::to_string(v) + " (client " + std::to_string(clientValue) + ")");
    };
    field("energy cost", "energy_cost", c->energyCost);
    field("seconds", "kraftwerks_seconds", c->kraftwerksSeconds);
    field("result", "result_template_id", c->resultTemplateId);
    field("amount", "result_amount", c->resultAmount);
    field("minimum level", "min_level", c->minLevel);
    // Inputs as a multiset of (class, quantity).
    std::vector<std::pair<int64_t, int64_t>> server, own;
    const DbTable& in = inputs_.edited;
    for (const auto& ir : in.rows)
        if (intAt(ir, in, "recipe_id") == templateId) server.push_back({intAt(ir, in, "input_class_id"), intAt(ir, in, "quantity")});
    for (const auto& [cls, qty] : c->inputs) own.push_back({cls, qty});
    std::sort(server.begin(), server.end());
    std::sort(own.begin(), own.end());
    if (server != own) out.push_back("inputs differ (" + std::to_string(server.size()) + " here, " + std::to_string(own.size()) + " in the client)");
    return out;
}

void RecipeEditor::resetToClient(int64_t templateId, const tr::ClientRecipe& c) {
    DbTable& t = recipes_.edited;
    int row = recipeRow(templateId);
    if (row < 0) {
        t.rows.push_back(std::vector<DbValue>(t.columns.size()));
        row = int(t.rows.size()) - 1;
        setInt(t.rows[size_t(row)], t, "id", templateId);
    }
    auto& r = t.rows[size_t(row)];
    setInt(r, t, "energy_cost", c.energyCost);
    setInt(r, t, "kraftwerks_seconds", c.kraftwerksSeconds);
    setInt(r, t, "result_template_id", c.resultTemplateId);
    setInt(r, t, "result_amount", c.resultAmount);
    setInt(r, t, "min_level", c.minLevel);
    DbTable& in = inputs_.edited;
    in.rows.erase(std::remove_if(in.rows.begin(), in.rows.end(), [&](const auto& ir) { return intAt(ir, in, "recipe_id") == templateId; }), in.rows.end());
    for (const auto& [cls, qty] : c.inputs) {
        std::vector<DbValue> ir(in.columns.size());
        setInt(ir, in, "id", nextId(in));
        setInt(ir, in, "recipe_id", templateId);
        setInt(ir, in, "input_class_id", cls);
        setInt(ir, in, "quantity", qty);
        in.rows.push_back(ir);
    }
}

bool RecipeEditor::save(WorldDatabase& database, std::string& message) {
    const std::vector<std::string> problems = errors(database);
    if (!problems.empty()) {
        message = "Recipes not saved: " + std::to_string(problems.size()) + " error(s), first: " + problems.front();
        return false;
    }
    WorldDatabase::SaveSummary summary;
    std::string error;
    if (!database.saveTables({{&recipes_.saved, &recipes_.edited}, {&inputs_.saved, &inputs_.edited}}, summary, error)) {
        message = "Saving recipes failed, nothing was written: " + error;
        return false;
    }
    const int64_t keep = selected_;
    load(database);
    selected_ = keep;
    message = "Recipes saved: " + std::to_string(summary.inserted) + " added, " + std::to_string(summary.updated) + " changed, " +
              std::to_string(summary.deleted) + " deleted (the server reads them at start-up).";
    return true;
}

void RecipeEditor::draw(EditorContext& ctx, const WorldDatabase& database, bool writable) {
    if (!loaded_) {
        ImGui::TextDisabled("This world has no recipe tables.");
        return;
    }
    const std::vector<tr::ClientRecipe>& client = clientRecipes(ctx);
    const std::vector<std::string> problems = errors(database);
    if (writable) {
        ImGui::BeginDisabled(!changed());
        if (ImGui::Button("Save recipes")) {
            std::string message;
            const bool ok = save(const_cast<WorldDatabase&>(database), message);
            ctx.setStatus(message, !ok);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) revert();
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    size_t differing = 0, missing = 0;
    for (const auto& row : recipes_.edited.rows)
        if (!differences(intAt(row, recipes_.edited, "id"), client).empty()) ++differing;
    for (const tr::ClientRecipe& c : client)
        if (recipeRow(c.templateId) < 0) ++missing;
    ImGui::TextDisabled("%zu recipes, %zu inputs; client %zu: %zu differ, %zu missing here", recipes_.edited.rows.size(), inputs_.edited.rows.size(),
                        client.size(), differing, missing);
    for (const std::string& p : problems) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", p.c_str());
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##filter", "filter (schematic or result)", filter_, sizeof filter_);
    ImGui::SameLine();
    ImGui::Checkbox("Only those not as the client", &onlyDifferent_);
    if (writable && missing > 0) {
        ImGui::SameLine();
        if (ImGui::Button("Add the client's missing recipes")) {
            for (const tr::ClientRecipe& c : client)
                if (recipeRow(c.templateId) < 0) resetToClient(c.templateId, c);
        }
    }
    // The list.
    const DbTable& t = recipes_.edited;
    const std::string needle = lower(filter_);
    std::vector<size_t> shown;
    std::vector<std::string> labels(t.rows.size());
    for (size_t k = 0; k < t.rows.size(); ++k) {
        const int64_t id = intAt(t.rows[k], t, "id");
        const bool differs = !differences(id, client).empty();
        if (onlyDifferent_ && !differs) continue;
        const int64_t result = intAt(t.rows[k], t, "result_template_id");
        labels[k] = std::to_string(id) + " " + itemLabel(ctx, database, int32_t(id)) + "  ->  " + std::to_string(intAt(t.rows[k], t, "result_amount")) + " x " +
                    itemLabel(ctx, database, int32_t(result)) + (differs ? "  [not as the client]" : "");
        if (!needle.empty() && lower(labels[k]).find(needle) == std::string::npos) continue;
        shown.push_back(k);
    }
    std::sort(shown.begin(), shown.end(), [&](size_t a, size_t b) { return intAt(t.rows[a], t, "id") < intAt(t.rows[b], t, "id"); });
    ImGui::BeginChild("recipelist", ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.35f)), ImGuiChildFlags_Borders);
    for (size_t k : shown) {
        const int64_t id = intAt(t.rows[k], t, "id");
        ImGui::PushID(int(id));
        if (ImGui::Selectable(labels[k].c_str(), id == selected_)) selected_ = id;
        ImGui::PopID();
    }
    ImGui::EndChild();
    const int row = recipeRow(selected_);
    if (row < 0) {
        ImGui::TextDisabled("Choose a recipe.");
        return;
    }
    ImGui::BeginChild("recipedetail");
    ImGui::BeginDisabled(!writable);
    DbTable& rt = recipes_.edited;
    auto& r = rt.rows[size_t(row)];
    ImGui::Text("Schematic %lld: %s", static_cast<long long>(selected_), itemLabel(ctx, database, int32_t(selected_)).c_str());
    intColumn("Energy cost", r, rt, "energy_cost", 0, INT32_MAX, 140, "Credits charged when the job is started (EnergyUnitCost).");
    intColumn("Seconds", r, rt, "kraftwerks_seconds", 0, INT32_MAX, 140, "How long the station works on it (KraftwerksTimeSeconds).");
    {
        int64_t v = intAt(r, rt, "result_template_id");
        ImGui::SetNextItemWidth(320);
        if (lookupCombo("Result template", v, lookup::itemTemplate(ctx, database))) setInt(r, rt, "result_template_id", v);
    }
    intColumn("Result amount", r, rt, "result_amount", 1, INT32_MAX, 140);
    intColumn("Minimum level", r, rt, "min_level", 0, 50, 140, "MinimumPlayerLevel.");
    ImGui::SeparatorText("Inputs (recipe_input)");
    DbTable& in = inputs_.edited;
    int remove = -1;
    for (size_t k = 0; k < in.rows.size(); ++k) {
        auto& ir = in.rows[k];
        if (intAt(ir, in, "recipe_id") != selected_) continue;
        ImGui::PushID(int(k));
        if (writable && ImGui::SmallButton("x")) remove = int(k);
        ImGui::SameLine();
        intColumn("##qty", ir, in, "quantity", 1, INT32_MAX, 80);
        ImGui::SameLine();
        const int32_t cls = int32_t(intAt(ir, in, "input_class_id"));
        ImGui::Text("x  %d %s", cls, ctx.classInfo(cls).known ? ctx.classInfo(cls).name.c_str() : "(unknown class)");
        ImGui::PopID();
    }
    if (remove >= 0) in.rows.erase(in.rows.begin() + remove);
    if (writable) {
        ImGui::SetNextItemWidth(260);
        lookupCombo("##newinput", newInputClass_, lookup::entityClass(ctx, "(choose a class)"));
        ImGui::SameLine();
        ImGui::BeginDisabled(newInputClass_ <= 0 || !ctx.classInfo(newInputClass_).known);
        if (ImGui::Button("Add input class")) {
            std::vector<DbValue> ir(in.columns.size());
            setInt(ir, in, "id", nextId(in));
            setInt(ir, in, "recipe_id", selected_);
            setInt(ir, in, "input_class_id", newInputClass_);
            setInt(ir, in, "quantity", 1);
            in.rows.push_back(ir);
            newInputClass_ = 0;
        }
        ImGui::EndDisabled();
    }
    // Against the client's own recipe.
    ImGui::SeparatorText("The client's recipe (generated.shared.recipe)");
    const std::vector<std::string> diff = differences(selected_, client);
    if (diff.empty()) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "As the client has it.");
    for (const std::string& d : diff) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", d.c_str());
    if (const tr::ClientRecipe* c = clientRecipe(client, selected_); c && !diff.empty() && writable) {
        if (ImGui::Button("Reset to the client's")) resetToClient(selected_, *c);
        ImGui::SetItemTooltip("The client shows its own recipe in the schematic's tooltip and the Kraftwerks window; a server recipe that\n"
                              "differs makes and charges something other than what the player is shown.");
    }
    if (writable && ImGui::Button("Delete recipe")) {
        in.rows.erase(std::remove_if(in.rows.begin(), in.rows.end(), [&](const auto& ir) { return intAt(ir, in, "recipe_id") == selected_; }), in.rows.end());
        rt.rows.erase(rt.rows.begin() + row);
        selected_ = -1;
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
}
