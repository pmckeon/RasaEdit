#pragma once

#include "WorldDatabase.h"

#include <cstdint>
#include <string>
#include <vector>

class EditorContext;
namespace tr {
struct ClientRecipe;
}

// The Rasa.NET fabrication recipes (recipe, recipe_input; Rasa.Game/Managers/RecipeManager.cs, read 2026-10-15): what a schematic
// item makes at a Kraftwerks station. recipe.id = the schematic's item template id; energy_cost (credits charged when the job
// starts), kraftwerks_seconds, result_template_id x result_amount, min_level; recipe_input rows (id AUTOINCREMENT) give its input
// item classes and quantities. The server skips a recipe whose schematic or result is not an item template, and ignores inputs of
// a missing recipe. The tables mirror the client's own generated.shared.recipe (both sides read the same recipe), so the editor
// shows where a row differs from the client's and can reset it, and lists client recipes the server lacks.
class RecipeEditor {
public:
    void load(const WorldDatabase& database);         // throws std::runtime_error
    void clear();
    bool loaded() const { return loaded_; }
    bool changed() const;
    bool save(WorldDatabase& database, std::string& message);
    void revert();
    std::vector<std::string> errors(const WorldDatabase& database) const;   // what the server would skip (blocks saving)
    void draw(EditorContext& ctx, const WorldDatabase& database, bool writable);

    // The differences between the server's recipe (by schematic template id) and the client's; empty when they agree.
    std::vector<std::string> differences(int64_t templateId, const std::vector<tr::ClientRecipe>& client) const;
    void resetToClient(int64_t templateId, const tr::ClientRecipe& client);   // the row and its inputs as the client has them

    struct Table {
        DbTable saved;
        DbTable edited;
    };
    Table recipes_, inputs_;
    int64_t selected_ = -1;

private:
    int recipeRow(int64_t templateId) const;
    bool loaded_ = false;
    char filter_[64] = "";
    bool onlyDifferent_ = false;
    int32_t newInputClass_ = 0;
};
