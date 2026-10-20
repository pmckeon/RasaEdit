#pragma once

// Dropdowns for values that name something: a row of a table (creatures, item templates, maps, NPC packages, ...), an entry of the
// client's data (entity classes, texts, audio sets, constants) or an enum. The editor's rule: such a value is picked by its name,
// never typed as a bare number. The dropdown is searchable (name or id) and lists long tables with a clipper; a value that is not in
// its list stays as it is, shown as "(unknown)" in warning colour.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class EditorContext;
class WorldDatabase;

struct LookupChoice {
    int64_t id = 0;
    std::string label;
};
using LookupList = std::vector<LookupChoice>;

struct LookupSource {
    std::function<std::string(int64_t)> name;          // the readable name of one value; "" when it is not in the list
    std::function<void(LookupList&)> fill;             // every value with its name, called when the dropdown opens
    const char* none = nullptr;                        // a label for 0 when 0 means "none" (then 0 is never flagged)
    bool allowOther = false;                           // an id not in the list may be entered (e.g. a new conversation package)
};

bool lookupCombo(const char* label, int64_t& value, const LookupSource& source);
bool lookupCombo(const char* label, int32_t& value, const LookupSource& source);

namespace lookup {

using EnumValues = std::vector<std::pair<int64_t, const char*>>;

LookupSource entityClass(EditorContext& ctx, const char* none = nullptr);                   // generated.client.entityclass
LookupSource creature(EditorContext& ctx, const WorldDatabase& database, const char* none = nullptr);   // creature table
LookupSource itemTemplate(EditorContext& ctx, const WorldDatabase& database, const char* none = nullptr);   // itemtemplate
LookupSource map(const WorldDatabase& database, const char* none = nullptr);                  // map_info
LookupSource npcPackage(const WorldDatabase& database, const char* none = nullptr);           // npc_package (by comment)
LookupSource missionText(EditorContext& ctx, const char* none = nullptr);                     // missiontextlanguage
LookupSource creatureName(EditorContext& ctx, const char* none = nullptr);                    // creaturenamelanguage
LookupSource npcGreeting(EditorContext& ctx, const char* none = nullptr);                     // npcgreetinglanguage
LookupSource bark(EditorContext& ctx, const char* none = nullptr);                            // barklanguage
LookupSource vendorPackage(EditorContext& ctx, const char* none = nullptr);                   // vendordata.vendorpackages, by type
LookupSource audioSet(EditorContext& ctx, const char* none = nullptr);                        // audiosetdata.audioSet
LookupSource missionCategory(EditorContext& ctx, const char* none = nullptr);                 // missioncategorylanguage
// A client constants module, e.g. generated.client.tutorialdata, generated.client.skilldata, generated.client.usabledata.
LookupSource constants(EditorContext& ctx, const char* module, const char* none = nullptr);
LookupSource values(const EnumValues& values, const char* none = nullptr);                    // a fixed enum
LookupSource pairs(std::shared_ptr<std::vector<std::pair<int64_t, std::string>>> choices);   // (id, label) pairs worked out by the caller

// Rasa.NET enums.
extern const EnumValues kWaypointTypes;          // Rasa.Game/Data/WaypointType.cs (teleporter.type)
extern const EnumValues kEquipmentSlots;         // Rasa.Game/Data/EquipmentData.cs (creature_appearance.slot_id)
extern const EnumValues kQualificationKeys;      // Rasa.Missions CharacterQualificationKey

}  // namespace lookup
