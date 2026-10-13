#pragma once

// Marker colours shared by the 3D viewport, the 2D overview and their legend (Panels: drawMarkerLegend).
// Yellow is reserved for the selection.

#include "GpuMath.h"
#include "WorldDatabase.h"

namespace markerColour {

inline const Vec4f selected{1.0f, 0.85f, 0.1f, 1};             // the selection (any kind)
inline const Vec4f hostileCreature{1.0f, 0.3f, 0.3f, 1};       // spawn pool of creatures
inline const Vec4f npc{0.35f, 1.0f, 0.45f, 1};                 // spawn pool of an NPC
inline const Vec4f vendor{0.35f, 0.6f, 1.0f, 1};               // spawn pool of a vendor
inline const Vec4f teleporter{0.7f, 0.5f, 1.0f, 1};            // teleporters, waypoints, hospitals
inline const Vec4f object{1.0f, 0.55f, 0.15f, 1};              // logos, shrines and other placed objects
inline const Vec4f ambientNpc{0.75f, 1.0f, 0.65f, 1};          // ambient NPC figures (ambient_npc)
inline const Vec4f worldUsable{1.0f, 0.8f, 0.55f, 1};          // world usables: switches, doors, dispensers, blasts (world_usable)
inline const Vec4f worldDestructible{0.7f, 0.35f, 0.15f, 1};   // world destructibles: barrels, pods, hives, force fields
inline const Vec4f arrival{0.8f, 0.15f, 0.2f, 1};              // spawn arrival points (dropship pads, Bane teleporters)
inline const Vec4f patrol{0.1f, 0.75f, 0.6f, 1};                // spawn pool patrol steps and the circuit between them
inline const Vec4f mapLink{1.0f, 0.35f, 0.85f, 1};             // map links (enabled)
inline const Vec4f mapLinkDisabled{0.55f, 0.45f, 0.55f, 1};    // map links (disabled)
inline const Vec4f missionArea{0.8f, 0.65f, 0.45f, 1};         // mission areas
inline const Vec4f region{0.85f, 0.85f, 0.85f, 1};             // regions (surface)
inline const Vec4f regionUnderground{0.5f, 0.55f, 0.7f, 1};    // regions (underground)
inline const Vec4f placeholder{0.3f, 0.9f, 1.0f, 1};           // map entity boxes: no model, or an effect
inline const Vec4f entityModel{0.92f, 0.92f, 0.92f, 0.8f};     // overview: map entity with a model
inline const Vec4f entityNoMesh{0.47f, 0.63f, 1.0f, 0.86f};    // overview: map entity whose class has no mesh
inline const Vec4f entityUnknown{1.0f, 0.31f, 0.27f, 1};       // overview: map entity of an unknown class

inline const Vec4f& ofObject(ObjectKind kind) {
    switch (kind) {
        case ObjectKind::AmbientNpc: return ambientNpc;
        case ObjectKind::WorldUsable: return worldUsable;
        case ObjectKind::WorldDestructible: return worldDestructible;
        default: return object;
    }
}

}  // namespace markerColour
