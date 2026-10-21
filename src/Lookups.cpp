#include "Lookups.h"

#include "EditorContext.h"
#include "WorldDatabase.h"
#include "gamedata.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The list of the open dropdown (one is open at a time) and its filter text.
LookupList g_openList;
std::string g_openFilter;

}  // namespace

bool lookupCombo(const char* label, int64_t& value, const LookupSource& source) {
    const bool isNone = value == 0 && source.none;
    const std::string name = isNone || !source.name ? std::string() : source.name(value);
    const bool missing = !isNone && name.empty();
    const std::string preview = isNone ? std::string(source.none) : std::to_string(value) + "  " + (missing ? "(unknown)" : name);
    if (missing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
    const bool open = ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLarge);
    if (missing) ImGui::PopStyleColor();
    if (!open) {
        if (missing && ImGui::IsItemHovered()) ImGui::SetTooltip("%lld is not one of the values this field takes", static_cast<long long>(value));
        return false;
    }
    bool changed = false;
    if (ImGui::IsWindowAppearing()) {
        g_openList.clear();
        if (source.fill) source.fill(g_openList);
        g_openFilter.clear();
        ImGui::SetKeyboardFocusHere();
    }
    char filter[128];
    std::snprintf(filter, sizeof filter, "%s", g_openFilter.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##lookupfilter", "search by name or id", filter, sizeof filter)) g_openFilter = filter;
    const std::string needle = lower(g_openFilter);
    if (source.none && (needle.empty() || lower(source.none).find(needle) != std::string::npos) && ImGui::Selectable(source.none, value == 0)) {
        value = 0;
        changed = true;
    }
    std::vector<const LookupChoice*> shown;
    shown.reserve(g_openList.size());
    for (const LookupChoice& c : g_openList)
        if (needle.empty() || lower(std::to_string(c.id) + "  " + c.label).find(needle) != std::string::npos) shown.push_back(&c);
    ImGuiListClipper clipper;
    clipper.Begin(int(shown.size()));
    while (clipper.Step())
        for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
            const LookupChoice& c = *shown[size_t(k)];
            ImGui::PushID(k);
            if (ImGui::Selectable((std::to_string(c.id) + "  " + c.label).c_str(), c.id == value)) {
                value = c.id;
                changed = true;
            }
            ImGui::PopID();
        }
    if (shown.empty() && !source.none) ImGui::TextDisabled("(nothing matches)");
    if (source.allowOther) {
        ImGui::Separator();
        static long long other = 0;
        if (ImGui::IsWindowAppearing()) other = value;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputScalar("##other", ImGuiDataType_S64, &other);
        ImGui::SameLine();
        if (ImGui::Button("Use this id")) {
            value = other;
            changed = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndCombo();
    return changed;
}

bool lookupCombo(const char* label, int32_t& value, const LookupSource& source) {
    int64_t v = value;
    if (!lookupCombo(label, v, source)) return false;
    value = int32_t(v);
    return true;
}

namespace lookup {

const EnumValues kWaypointTypes = {{1, "local teleporter"}, {2, "waypoint"}, {3, "wormhole"}, {4, "dropship"}, {5, "hospital"}};
const EnumValues kEquipmentSlots = {{1, "Helmet"},           {2, "Shoes"},           {3, "Gloves"},          {13, "Weapon"},         {14, "Hair"},
                                    {15, "Torso"},           {16, "Legs"},           {17, "Face"},           {18, "Wing"},           {19, "Eyewear"},
                                    {20, "Beard"},           {21, "Mask"},           {25, "MechWeapon1"},    {26, "MechWeapon2"},    {27, "MechWeapon3"},
                                    {28, "MechWeapon4"},     {29, "MechWeapon5"},    {30, "MechAbility1"},   {31, "MechAbility2"},   {32, "MechAbility3"},
                                    {33, "MechAbility4"},    {34, "MechAbility5"},   {35, "MechArmorPlating"}, {36, "MechShieldGenerator"},
                                    {37, "MechMobilitySystem"}, {38, "MechTargetingComputer"}, {39, "MechPowerPlant"}, {10000001, "HandToHand"}};
const EnumValues kQualificationKeys = {{1, "BootcampComplete"}};

namespace {

// A source over GameData pairs (id, name), read once per dropdown opening.
LookupSource fromPairs(EditorContext& ctx, std::function<std::vector<std::pair<int32_t, std::string>>(tr::GameData&)> read,
                       std::function<std::string(tr::GameData&, int64_t)> one, const char* none) {
    LookupSource s;
    s.none = none;
    s.name = [&ctx, one](int64_t id) -> std::string {
        if (!ctx.gameData) return "";
        try { return one(*ctx.gameData, id); } catch (const std::exception&) { return ""; }
    };
    s.fill = [&ctx, read](LookupList& out) {
        if (!ctx.gameData) return;
        try {
            for (const auto& p : read(*ctx.gameData)) out.push_back(LookupChoice{p.first, p.second});
        } catch (const std::exception&) {}
    };
    return s;
}

}  // namespace

LookupSource entityClass(EditorContext& ctx, const char* none) {
    LookupSource s;
    s.none = none;
    s.name = [&ctx](int64_t id) {
        const ClassInfo& info = ctx.classInfo(int32_t(id));
        return info.known ? info.name : std::string();
    };
    s.fill = [&ctx](LookupList& out) {
        if (!ctx.gameData) return;
        try {
            for (const auto& p : ctx.gameData->lookupNames("generated.client.entityclass", "lookup")) out.push_back(LookupChoice{p.first, p.second});
        } catch (const std::exception&) {}
    };
    return s;
}

LookupSource creature(EditorContext& ctx, const WorldDatabase& database, const char* none) {
    LookupSource s;
    s.none = none;
    const auto label = [&ctx, &database](const Creature& c) {
        const ClassInfo& info = ctx.classInfo(c.classId);
        std::string name = c.comment;
        if (c.nameId > 0 && ctx.gameData) {
            try {
                const std::string n = ctx.gameData->creatureName(c.nameId);
                if (!n.empty()) name = n;
            } catch (const std::exception&) {}
        }
        return name + "  (lvl " + std::to_string(c.level) + ", " + info.name + ")" +
               (database.classIsVendor(c.classId) ? " vendor" : database.classIsNpc(c.classId) ? " NPC" : "");
    };
    s.name = [&database, label](int64_t id) {
        const Creature* c = database.creature(int32_t(id));
        return c ? label(*c) : std::string();
    };
    s.fill = [&database, label](LookupList& out) {
        for (const Creature& c : database.creatureList()) out.push_back(LookupChoice{c.id, label(c)});
    };
    return s;
}

LookupSource itemTemplate(EditorContext& ctx, const WorldDatabase& database, const char* none) {
    LookupSource s;
    s.none = none;
    const auto label = [&ctx](int32_t id, const ItemTemplateInfo& info) {
        std::string name;
        if (ctx.gameData) {
            try { name = ctx.gameData->itemName(id, info.classId); } catch (const std::exception&) {}
        }
        if (name.empty()) name = ctx.classInfo(info.classId).name;
        return name;
    };
    s.name = [&database, label](int64_t id) {
        const ItemTemplateInfo* info = database.itemTemplate(int32_t(id));
        return info ? label(int32_t(id), *info) : std::string();
    };
    s.fill = [&database, label](LookupList& out) {
        for (const auto& t : database.itemTemplates()) out.push_back(LookupChoice{t.first, label(t.first, t.second)});
        std::sort(out.begin(), out.end(), [](const LookupChoice& a, const LookupChoice& b) { return a.id < b.id; });
    };
    return s;
}

LookupSource map(const WorldDatabase& database, const char* none) {
    LookupSource s;
    s.none = none;
    s.name = [&database](int64_t id) {
        for (const MapInfo& m : database.maps())
            if (m.mapContextId == id) return m.name;
        return std::string();
    };
    s.fill = [&database](LookupList& out) {
        for (const MapInfo& m : database.maps()) out.push_back(LookupChoice{m.mapContextId, m.name});
    };
    return s;
}

LookupSource npcPackage(const WorldDatabase& database, const char* none) {
    LookupSource s;
    s.none = none;
    s.name = [&database](int64_t id) {
        for (const auto& p : database.npcPackageList())
            if (p.first == id) return p.second.empty() ? std::string("(no comment)") : p.second;
        return std::string();
    };
    s.fill = [&database](LookupList& out) {
        for (const auto& p : database.npcPackageList()) out.push_back(LookupChoice{p.first, p.second});
    };
    return s;
}

LookupSource missionText(EditorContext& ctx, const char* none) {
    return fromPairs(ctx, [](tr::GameData& g) { return g.lookupNames("generated.client.language.english.missiontextlanguage", "lookup", 1); },
                     [](tr::GameData& g, int64_t id) { return g.missionText(int32_t(id)); }, none);
}

LookupSource creatureName(EditorContext& ctx, const char* none) {
    return fromPairs(ctx, [](tr::GameData& g) { return g.lookupNames("generated.client.language.english.creaturenamelanguage", "lookup"); },
                     [](tr::GameData& g, int64_t id) { return g.creatureName(int32_t(id)); }, none);
}

LookupSource npcGreeting(EditorContext& ctx, const char* none) {
    return fromPairs(ctx, [](tr::GameData& g) { return g.lookupNames("generated.client.language.english.npcgreetinglanguage", "lookup", 1); },
                     [](tr::GameData& g, int64_t id) { return g.npcGreeting(int32_t(id)); }, none);
}

LookupSource pairs(std::shared_ptr<std::vector<std::pair<int64_t, std::string>>> choices) {
    LookupSource source;
    source.name = [choices](int64_t id) {
        for (const auto& c : *choices)
            if (c.first == id) return c.second;
        return std::string();
    };
    source.fill = [choices](LookupList& out) {
        for (const auto& c : *choices) out.push_back(LookupChoice{c.first, c.second});
    };
    return source;
}

LookupSource bark(EditorContext& ctx, const char* none) {
    return fromPairs(ctx, [](tr::GameData& g) { return g.lookupNames("generated.client.language.english.barklanguage", "lookup", 1); },
                     [](tr::GameData& g, int64_t id) { return g.barkText(int32_t(id)); }, none);
}

LookupSource audioSet(EditorContext& ctx, const char* none) {
    return fromPairs(ctx, [](tr::GameData& g) { return g.lookupNames("generated.client.audiosetdata", "audioSet"); },
                     [](tr::GameData& g, int64_t id) { return g.audioSetName(int32_t(id)); }, none);
}

LookupSource missionCategory(EditorContext& ctx, const char* none) {
    const auto all = [](tr::GameData& g) { return g.lookupNames("generated.client.language.english.missioncategorylanguage", "lookup"); };
    return fromPairs(ctx, all,
                     [all](tr::GameData& g, int64_t id) {
                         for (const auto& p : all(g))
                             if (p.first == id) return p.second;
                         return std::string();
                     },
                     none);
}

LookupSource vendorPackage(EditorContext& ctx, const char* none) {
    // vendordata.vendorpackages[id] = (vendor type, ...); the type's name from vendordata's constants (ARMOR, WEAPONS, ...).
    const auto all = [](tr::GameData& g) {
        std::vector<std::pair<int32_t, std::string>> out;
        const auto types = g.moduleConstants("generated.client.vendordata");
        const tr::py::Object* packages = g.global("generated.client.vendordata", "vendorpackages");
        if (!packages || packages->kind != tr::py::Kind::Dict) return out;
        for (const auto& entry : packages->entries) {
            const int64_t type = tr::py::asInt(tr::py::item(entry.second.get(), 0), -1);
            std::string name = "type " + std::to_string(type);
            for (const auto& t : types)
                if (t.first == type) name = t.second;
            out.push_back({int32_t(tr::py::asInt(entry.first.get())), name});
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    return fromPairs(ctx, all,
                     [all](tr::GameData& g, int64_t id) {
                         for (const auto& p : all(g))
                             if (p.first == id) return p.second;
                         return std::string();
                     },
                     none);
}

LookupSource constants(EditorContext& ctx, const char* module, const char* none) {
    const std::string m = module;
    const auto all = [m](tr::GameData& g) { return g.moduleConstants(m); };
    return fromPairs(ctx, all,
                     [all](tr::GameData& g, int64_t id) {
                         std::string names;
                         for (const auto& p : all(g))
                             if (p.first == id) names += (names.empty() ? "" : " / ") + p.second;
                         return names;
                     },
                     none);
}

LookupSource values(const EnumValues& list, const char* none) {
    LookupSource s;
    s.none = none;
    s.name = [list](int64_t id) {
        for (const auto& v : list)
            if (v.first == id) return std::string(v.second);
        return std::string();
    };
    s.fill = [list](LookupList& out) {
        for (const auto& v : list) out.push_back(LookupChoice{v.first, v.second});
    };
    return s;
}

}  // namespace lookup
