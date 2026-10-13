#include "Avatar.h"

#include "WorldDatabase.h"
#include "gamedata.h"

AvatarLook resolveAvatar(tr::GameData& data, const WorldDatabase& database, const Creature& creature) {
    AvatarLook look;
    const int32_t baseMeshId = data.classMeshId(creature.classId);
    if (baseMeshId < 0) return look;
    look.baseMesh = data.stringById(baseMeshId);
    const int32_t skeleton = data.meshSkeletonId(baseMeshId);
    std::vector<std::string> idles = data.familyAnimations(skeleton, 1);
    if (idles.empty() && skeleton == 23) {
        for (std::string& name : data.familyAnimations(1, 1))
            if (name.rfind("m_", 0) == 0) idles.push_back("f_" + name.substr(2));
    }
    if (!idles.empty()) look.poseAnimation = idles.front();
    for (const AppearanceSlot& slot : database.appearance(creature.id)) {
        if (slot.slotId == 17) look.skinColour = slot.colour;
        for (const tr::SwapsetPart& sp : data.itemSwapsetParts(slot.classId, baseMeshId)) {
            AvatarPart part;
            part.mesh = data.suitMeshName(sp.suitId);
            part.swapsetId = uint32_t(sp.swapsetId);
            part.colour = slot.colour;
            part.slotId = slot.slotId;
            if (!part.mesh.empty()) look.parts.push_back(std::move(part));
        }
        for (const tr::AccessoryAttachment& a : data.itemAccessories(slot.classId, 1))
            look.accessories.push_back(AvatarAccessory{a.mesh, a.connectionPoint, slot.colour});
    }
    return look;
}
