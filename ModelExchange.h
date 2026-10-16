#pragma once

// Models in and out of Blender (or any glTF 2.0 tool): a .geo with its textures, skeleton, contact points and .anm animations
// as glTF ("glTF Separate": .gltf + .bin + PNG textures), and back as a .geo / .anm / .dds set for the game's patch archive.
//
// Coordinates: the game is left-handed (x east, y up, z north), glTF right-handed (y up, -z forward) - z is mirrored
// (positions and normals (x, y, -z), rotations (-x, -y, z, w), matrices S M S with S = diag(1, 1, -1)). Mirroring turns the
// game's clockwise front faces into glTF's counter-clockwise ones, so index order is kept. UV origins agree (top left).
//
// What glTF cannot hold travels in "extras", which Blender keeps as custom properties (export from Blender with Include >
// Custom Properties on):
//   scene root node   rasaModel: { source .geo, GBOD fields, property groups }
//   bone nodes        rasaBone: { PBON fields, bounding volume (base64), BDAT version }
//   contact points    empties under their bone: rasaContactPoint: { id, values [[key, value], ...] }
//   materials         rasaMaterial: { effect, parameters [[key, type, value], ...] } - textures by file name; the diffuse and
//                     normal maps are also the glTF base colour / normal textures
//   meshes            rasaPiece: { group, piece name, swap id, user data, vertex layout, GPCE version, bones per vertex, ... }
//   animations        rasaAnimation: { source .anm, per-bone flags and key kinds, material (texture scroll) tracks, other tracks }

#include "trformats.h"

#include <string>
#include <vector>

namespace tr {
class AssetLibrary;
}

struct ModelExportOptions {
    bool textures = true;              // write every texture the materials name as PNG (textures/<name>.png)
    // Low-detail pieces (USDA LOD > 0, swap set 99 stand-ins) overlap the full-detail ones. Hidden: a second glTF scene, which
    // Blender (Import Scene as Collection, its default) imports as an excluded "Low detail" collection - KHR_node_visibility is not in
    // Blender 5.2's importer. Omitted: left out. Visible: with the other pieces. Hidden and omitted pieces are listed in
    // rasaModel.omitted, and an import restores them from the source model when the file does not carry them.
    enum class LowDetail : uint8_t { Hidden, Omitted, Visible };
    LowDetail lowDetail = LowDetail::Hidden;
    // Collision volumes as meshes in a hidden "Collision" collection (another glTF scene), at their bone's bind world.
    bool collision = true;
};

struct ModelExchangeReport {
    std::vector<std::string> written;  // files written
    std::vector<std::string> warnings;
};

// Writes <outPath> (.gltf) with <outPath minus .gltf>.bin and textures/ beside it. Throws on errors reading the model.
ModelExchangeReport exportModelGltf(tr::AssetLibrary& assets, const std::string& geoName, const std::vector<std::string>& animations,
                                    const std::string& outPath, const ModelExportOptions& options = {});

// ---- import ----
//
// A glTF (.gltf with its .bin and images, or .glb) becomes a .geo, the .anm files of its animations and the .dds files of the
// textures that changed. With a source model (the root node's rasaModel.source, else ModelImportOptions::source) the import
// builds on it: bones whose inverse bind is unchanged keep the source's skeleton data exactly (other animations of a shared
// skeleton depend on it), pieces left out of the export (rasaModel.omitted) are kept, materials keep their effect and
// parameters (rasaMaterial). Without one, a new model: one identity bone "Bone_main" when the file has no skin, effects from
// each material's custom property rasaEffect (default TRDiffMapVertColor.fx), parameters copied from a shipped piece of that
// effect. Static pieces are expressed in their bone's bind space (the engine draws them with the bone's world transform), so
// meshes parented to bones in Blender land where Blender shows them.
struct ModelImportOptions {
    std::string modelName;             // the .geo to write; empty = the source's name, else the glTF file's name + ".geo"
    std::string source;                // the source model when the file names none ("" = new model)
    bool renameChangedTextures = false; // changed textures under <model>_<texture>.dds (only this model) instead of their own name
    // Collision (bone volumes, what the client walks on and collides with): meshes with rasaCollision data, or named "collision...",
    // define it when the file has any (each bone gets its own, or none); otherwise the source model's is kept. A model with neither -
    // a new one - gets one walkable surface from its opaque pieces on bone 0 when this is on.
    bool generateCollision = true;
};

struct ImportedFile {
    std::string name;                  // archive entry name, e.g. "npc_eloh_hologram_large.geo"
    tr::Bytes data;
    std::string note;                  // what it is / what changed
};

struct ModelImportResult {
    std::vector<ImportedFile> files;
    std::vector<std::string> warnings;
};

ModelImportResult importModelGltf(tr::AssetLibrary& assets, const std::string& path, const ModelImportOptions& options = {});

// Per-vertex tangent (dP/du) and binormal (dP/dv) from UV set 0, accumulated over the triangles and made perpendicular to the
// normal (game space, the convention of the Pal TANGENT0 / TANGENT1 attributes - checked against shipped pieces in viewport_test).
void computeTangentFrames(const std::vector<tr::Vec3>& positions, const std::vector<tr::Vec3>& normals, const std::vector<tr::Vec2>& uvs,
                          const std::vector<uint32_t>& indices, std::vector<tr::Vec3>& tangents, std::vector<tr::Vec3>& binormals);

// The patch archive (verified mechanism, CLAUDE.md "How client changes reach the game"): data/rasaedit_patch.glm, registered in
// tabula_rasa.cfg [Paths] as AAA_RasaEdit (sorts before every shipped key, so its files win). addToPatchArchive merges the files
// into the archive (replacing entries of the same name); registerPatchArchive adds the [Paths] line once (first save keeps
// tabula_rasa.cfg.rasaedit-original). Both return a description of what they did.
std::string patchArchivePath(const std::string& gameRoot);
std::string addToPatchArchive(const std::string& gameRoot, const std::vector<ImportedFile>& files);
std::string registerPatchArchive(const std::string& gameRoot);
bool patchArchiveRegistered(const std::string& gameRoot);

// The animations a model plays: its `anim` property (the object animation the engine plays), X.anm for X.geo when present, and
// (for avatar skeletons) none - creatures' animations come from animationdata families, chosen by the caller.
std::vector<std::string> modelOwnAnimations(tr::AssetLibrary& assets, const std::string& geoName);
