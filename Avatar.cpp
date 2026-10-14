#include "Avatar.h"

#include "WorldDatabase.h"
#include "gamedata.h"

#include <algorithm>
#include <map>

AvatarLook resolveAvatar(tr::GameData& data, const WorldDatabase& database, const Creature& creature) {
    AvatarLook look;
    const int32_t baseMeshId = data.classMeshId(creature.classId);
    if (baseMeshId < 0) return look;
    look.baseMesh = data.stringById(baseMeshId);
    const int32_t skeleton = data.meshSkeletonId(baseMeshId);
    look.idles = data.familyAnimations(1, skeleton);
    if (!look.idles.empty()) look.poseAnimation = look.idles.front().name;
    // The client's appearance data is a dict keyed by slot (actor.py); its values() order for small int keys is taken as
    // ascending slot (UNVERIFIED: Python 2.4 orders by hash mod table size, which can differ for 5 or fewer items - it only
    // matters when two non-hair items claim the same swap set).
    std::vector<AppearanceSlot> items = database.appearance(creature.id);
    std::stable_sort(items.begin(), items.end(), [](const AppearanceSlot& a, const AppearanceSlot& b) { return a.slotId < b.slotId; });
    // actor.py _ProcessSwapsets: items whose equipment slot (by item class, not the database slot) another item excludes are
    // skipped (an item already excluded does not exclude further; creatures show helmets); hair (14) and beard (20) only
    // fill swap sets no other item claimed, hair first. equipmentdata.suitSwapsetNoSkin is empty in the shipped data.
    std::vector<int32_t> excluded;
    auto isExcluded = [&](int32_t slot) { return std::find(excluded.begin(), excluded.end(), slot) != excluded.end(); };
    for (const AppearanceSlot& item : items) {
        if (isExcluded(data.itemEquipmentSlot(item.classId))) continue;
        for (int32_t s : data.itemSlotExclusions(item.classId)) excluded.push_back(s);
    }
    std::map<int32_t, AvatarPart> bySwapset;
    const AppearanceSlot* hair = nullptr;
    const AppearanceSlot* beard = nullptr;
    auto claim = [&](const AppearanceSlot& item, bool onlyFree) {
        for (const tr::SwapsetPart& sp : data.itemSwapsetParts(item.classId, baseMeshId)) {
            if (onlyFree && bySwapset.count(sp.swapsetId)) continue;
            AvatarPart part;
            part.mesh = data.suitMeshName(sp.suitId);
            part.swapsetId = uint32_t(sp.swapsetId);
            part.colour = item.colour;
            part.slotId = item.slotId;
            bySwapset[sp.swapsetId] = std::move(part);
        }
    };
    for (const AppearanceSlot& item : items) {
        if (item.slotId == 17) look.skinColour = item.colour;     // _GetSkinColor: the FACE slot's hue
        const int32_t slot = data.itemEquipmentSlot(item.classId);
        if (isExcluded(slot)) continue;
        if (slot == 14) hair = &item;
        else if (slot == 20) beard = &item;
        else claim(item, false);
    }
    if (hair) claim(*hair, true);
    if (beard) claim(*beard, true);
    for (auto& [swapset, part] : bySwapset)
        if (!part.mesh.empty()) look.parts.push_back(std::move(part));
    // _AddAccessory runs for every appearance item, excluded or not.
    for (const AppearanceSlot& item : items)
        for (const tr::AccessoryAttachment& a : data.itemAccessories(item.classId, 1))
            look.accessories.push_back(AvatarAccessory{a.mesh, a.connectionPoint, item.colour});
    return look;
}
