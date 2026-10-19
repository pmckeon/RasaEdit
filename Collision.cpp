#include "Collision.h"

ModelCollision modelCollision(const tr::GeoFile& file) {
    ModelCollision c;
    c.read = true;
    for (size_t b = 0; b < file.bones.size(); ++b) {
        const tr::GeoFileBone& bone = file.bones[b];
        if (bone.boundingVolume.empty()) continue;
        bool named = false;
        for (const tr::GeoPropertyGroup& g : file.properties)
            if (g.id == int32_t(b))
                for (const tr::GeoParameter& p : g.pairs) named = named || p.key == "WSName";
        std::vector<tr::GeoBoneVolume> volumes;
        try { volumes = tr::readBoneVolumes(bone.boundingVolume); } catch (const std::exception&) { continue; }
        bool surface = false;
        for (const tr::GeoBoneVolume& v : volumes) {
            if (v.kind == tr::fourcc("BVWS")) {
                surface = true;
                (named ? c.usedTriangles : c.droppedTriangles) += v.indices.size() / 3;
            } else if (v.kind == tr::fourcc("BVBX")) {
                ++c.boxes;
            } else if (v.kind == tr::fourcc("BVSP")) {
                ++c.spheres;
            }
        }
        if (surface && !named) c.unnamedBones.push_back(bone.name);
    }
    return c;
}

std::string collisionProblem(const ModelCollision& c, int32_t entityFlags) {
    if (!c.read) return {};
    const size_t surfaces = c.usedTriangles + c.droppedTriangles;
    if (surfaces && (entityFlags & 0x2)) return "its map flags (bit 1) turn its walkable surfaces off: the player walks through it";
    if (!c.unnamedBones.empty()) {
        std::string bones;
        for (const std::string& b : c.unnamedBones) bones += (bones.empty() ? "" : ", ") + b;
        return "bone " + bones + " has a walkable surface but no WSName: the game drops the entity's surfaces";
    }
    if (!surfaces && c.boxes + c.spheres)
        return "only box / sphere volumes, which map objects do not collide with: the player walks through it";
    return {};
}

std::string collisionVerdict(const ModelCollision& c, int32_t entityFlags) {
    if (!c.read) return "Collision unknown (the model could not be read).";
    const std::string problem = collisionProblem(c, entityFlags);
    if (!problem.empty()) return "Problem: " + problem + ".";
    if (c.usedTriangles)
        return "The player walks on and is blocked by its walkable surfaces (" + std::to_string(c.usedTriangles) + " triangles).";
    return "No collision: the player walks through it.";
}
