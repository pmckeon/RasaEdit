#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tr { class GameData; }
class WorldDatabase;
struct Creature;

// How the client assembles a swap-set creature (NPCs wearing clothing items), from the client's
// generated.client.equipmentdata tables (see CLAUDE.md, "Avatar swap sets"):
//   the base suit is the creature class's mesh (e.g. avatar_hum_m_nude_caucasian_v01.geo);
//   each creature_appearance item replaces the base pieces of its swap set with the same swap set's pieces
//   of another suit (equipableClassMeshSwapset[(itemClass, baseMesh)] = [(suit, swapset)]).
struct AvatarPart {
    std::string mesh;              // suit .geo
    uint32_t swapsetId = 0;        // 1 Torso, 2 Legs, 3 Boots, 4 Skin/Face, 5 Hair/Helm, 6 Gloves, 7 Face Hi, 8 Face Lo
    uint32_t colour = 0;           // hue, ABGR (red in the low byte); 0 = none
    int32_t slotId = 0;            // creature_appearance slot that supplied it
};

struct AvatarLook {
    std::string baseMesh;          // "" if the creature class has no mesh
    // Pose: the first "Idle - Non Combat" animation of the base mesh's skeleton (animationdata); "" = bind pose.
    // Skeleton 23 (human female) has no families: the male list with "m_" -> "f_" is used (UNVERIFIED rule).
    std::string poseAnimation;
    uint32_t skinColour = 0;       // ABGR hue for "flesh" pieces: the Face slot (17) colour; 0 = none (UNVERIFIED rule)
    std::vector<AvatarPart> parts;
};

AvatarLook resolveAvatar(tr::GameData& data, const WorldDatabase& database, const Creature& creature);
