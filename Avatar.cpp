#include "Avatar.h"

#include "WorldDatabase.h"
#include "gamedata.h"

AvatarLook resolveAvatar(tr::GameData& data, const WorldDatabase& database, const Creature& creature) {
    AvatarLook look;
    const int32_t baseMeshId = data.classMeshId(creature.classId);
    if (baseMeshId < 0) return look;
    look.baseMesh = data.stringById(baseMeshId);
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
    }
    return look;
}
