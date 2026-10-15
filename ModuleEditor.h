#pragma once

#include "WorldDatabase.h"

#include <cstdint>
#include <string>
#include <vector>

class EditorContext;

// The Rasa.NET item module tables (upstream 2026-11; Rasa.Game/Managers/ItemModules.cs, ItemModuleBonuses.cs, read 2026-10-15),
// seeded from the client's crafting data (generated/shared/crafting):
//   module_class      a module: variant (1-54, the kind of bonus - an item takes one of a variant), level 1-5 (its strength, printed
//                     "[n]"), class set (212 armor, 1212 weapons, 1273 tools, 1274 salvage only, 0 none), its item as a thing in a pack
//                     (template + class), Mimeogel costs (extract / integrate / upgrade, 0 = cannot), salvage gain, the module an
//                     upgrade makes, comment (varchar 64).
//   module_effect     what a module does: an effect of the client's gameeffectdata (its tooltip is the line the client draws), set
//                     level (a set bonus needing n pieces worn, 0 = the module's own), amount at item level L =
//                     ceil(flat + linear L + exp 2^((L - 1) / 8)), args (e.g. a damage type; NULL = None, "all"). Sent in id order.
//   module_item       an item template that is a module (salvage pieces included): its module and strength 1-5.
//   modifiable_class  an item class that takes modules, with its class set (212 / 1212 / 1273); a class of no row can be given no
//                     module and is not salvaged as equipment.
// The server logs and skips an effect whose module is missing; the editor refuses to save that, a module item of a missing module
// and a duplicate key, and warns about an upgrade to a missing module or an unusual class set. Database-wide, own Save / Revert.
class ModuleEditor {
public:
    void load(const WorldDatabase& database);         // throws std::runtime_error
    void clear();
    bool loaded() const { return loaded_; }
    bool changed() const;
    bool save(WorldDatabase& database, std::string& message);
    void revert();
    struct Problem {
        bool error = true;                            // errors block saving
        std::string message;
    };
    std::vector<Problem> validate() const;
    // part 0: the modules (list, fields, effects, module items); 1: the modifiable classes. Save / Revert and problems head both.
    void draw(EditorContext& ctx, const WorldDatabase& database, bool writable, int part);

    // For tests and the panel: the module_class row index of a module id (-1 if none), the effects of a module (row indices).
    int moduleRow(int64_t moduleId) const;
    std::vector<size_t> effectRows(int64_t moduleId) const;
    // The tooltip line the client draws for an effect row at item level `level` (tooltipwindow.py _AddModuleEffects: "(set) " /
    // "[level] " / "[+] " prefix, amount with "+" when positive, %(amount)s / %(argN)s substituted, %% -> %; other $ tokens left).
    std::string effectLine(EditorContext& ctx, size_t effectRow, int level) const;
    static int64_t effectAmount(double flat, double linear, double exp, int level);

    struct Table {
        DbTable saved;
        DbTable edited;
    };
    Table classes_, effects_, items_, modifiable_;
    int64_t selected_ = -1;                           // module id

private:
    void drawModule(EditorContext& ctx, const WorldDatabase& database, int row, bool writable);
    void drawModifiable(EditorContext& ctx, bool writable);

    bool loaded_ = false;
    char filter_[64] = "";
    char classFilter_[64] = "";
    char effectFilter_[64] = "";
    int previewLevel_ = 30;
    int32_t newTemplate_ = 0;
    int32_t newClass_ = 0;
    int32_t newClassSet_ = 212;
    std::vector<std::pair<int32_t, std::string>> effectNames_;   // gameeffectdata constants (value, name), read on first use
    bool effectNamesRead_ = false;
};
