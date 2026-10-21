#pragma once

// What the game makes of a model's collision (tabula_rasa.exe, traced 2026-10-19, CLAUDE.md "What the client collides with"):
// the player walks on and is blocked by walkable surfaces only - bone volumes of type 5 (BVWS) on a bone whose property group (the
// group whose id is the bone index) names a WSName; without the name the entity's surfaces are dropped. Box (BVBX) and sphere (BVSP)
// volumes serve scripted bodies (doors) and are not collided with on a map object. A map entity's .map flag bit 1 (0x2) keeps its
// surfaces out (World::AddToWorld).

#include "geofile.h"

#include <string>
#include <vector>

struct ModelCollision {
    bool read = false;                         // the .geo could be read
    size_t usedTriangles = 0;                  // walkable-surface triangles on bones with a WSName
    size_t droppedTriangles = 0;               // ... on bones without one
    size_t boxes = 0, spheres = 0;             // box / sphere volumes
    std::vector<std::string> unnamedBones;     // bones with a surface but no WSName
};

ModelCollision modelCollision(const tr::GeoFile& file);

// A map entity's collision problem in words ("" = none): surfaces dropped (a bone without WSName, or the entity's flag), or only
// box / sphere volumes. hasProblem is false for an entity without any collision volume (many props are meant to be walked through).
std::string collisionProblem(const ModelCollision& c, int32_t entityFlags);
// The plain verdict for an inspector: what the player meets.
std::string collisionVerdict(const ModelCollision& c, int32_t entityFlags);
