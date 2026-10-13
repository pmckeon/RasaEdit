// gamedata.h - read the client's compiled data tables (data/game.zip) without Python.
//
// game.zip holds Python 2.4 compiled modules (.pyo), e.g. generated/client/entityclass.pyo, whose
// module code builds literal tables such as  lookup = {classId: (name, meshId, ...), ...}.
// This reads the .pyo format (magic 6D F2 0D 0A, timestamp, marshalled code object) and evaluates the
// module code with a small stack machine. Across the 369 shipped modules the module-level code uses only:
//   LOAD_CONST LOAD_NAME STORE_NAME BUILD_TUPLE BUILD_LIST BUILD_MAP DUP_TOP ROT_THREE STORE_SUBSCR
//   UNARY_NEGATIVE RETURN_VALUE, plus (crafting only) MAKE_FUNCTION CALL_FUNCTION BUILD_CLASS
//   STORE_ATTR BINARY_SUBSCR.
// Classes are not executed: every data class's __init__ just stores its parameters
// (self.X = X), so calling a class binds the arguments to __init__'s parameter names.
#pragma once

#include "trformats.h"

#include <fstream>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tr {

namespace py {

enum class Kind : uint8_t {
    None, Bool, Int, Float, String, Tuple, List, Dict, Set, Code, Function, ClassBody, Class, Instance, Opaque
};

struct Object;
using Ref = std::shared_ptr<Object>;

struct Code {
    int32_t argCount = 0;
    int32_t flags = 0;
    std::string name;
    std::string bytecode;
    std::vector<Ref> constants;
    std::vector<std::string> names;
    std::vector<std::string> varNames;
};

// Key index of a Dict: int and string keys directly; other hashable keys (tuples, None, floats)
// by a canonical byte encoding. Allocated only for Dict objects.
struct DictIndex {
    std::unordered_map<int64_t, size_t> intKeys;
    std::unordered_map<std::string, size_t> stringKeys;
    std::unordered_map<std::string, size_t> otherKeys;
};

struct Object {
    Kind kind = Kind::None;
    bool boolean = false;
    int64_t integer = 0;
    double real = 0;
    std::string text;                                      // String bytes; Class name; Opaque description
    bool isUnicode = false;                                // String: text is UTF-8 (else raw 8-bit bytes)
    std::vector<Ref> items;                                // Tuple, List, Set
    std::vector<std::pair<Ref, Ref>> entries;              // Dict, insertion order
    std::unique_ptr<DictIndex> index;                      // Dict: key -> entry
    std::shared_ptr<Code> code;                            // Code, Function, ClassBody
    std::vector<std::string> fieldNames;                   // Class: __init__ parameter names
    Ref instanceClass;                                     // Instance
    std::vector<std::pair<std::string, Ref>> attributes;   // Instance; Class (class-level attributes)
};

// Accessors. All take possibly-null pointers where a lookup can fail.
bool isNone(const Object* o);
bool isInt(const Object* o);                               // Int or Bool
bool isString(const Object* o);
int64_t asInt(const Object* o, int64_t fallback = 0);
double asFloat(const Object* o, double fallback = 0);      // Float or Int
std::string asString(const Object* o, const std::string& fallback = "");
size_t length(const Object* o);                            // Tuple / List / Set / Dict, else 0
const Object* item(const Object* sequence, size_t index);  // Tuple / List; nullptr if out of range
const Object* dictGet(const Object* dict, int64_t key);
const Object* dictGet(const Object* dict, const std::string& key);
const Object* dictGetTuple(const Object* dict, std::initializer_list<int64_t> key);    // e.g. {missionId, 1}
const Object* attribute(const Object* object, const std::string& name);   // instance, then its class
std::string repr(const Object* o, size_t maxItems = 8);    // Python-like text, for debugging

}  // namespace py

// Evaluate one compiled module; returns its globals (a Dict keyed by variable name).
py::Ref evaluateModule(const Bytes& pyo, const std::string& moduleName);

// Zip archive (stored and deflate entries), streamed from disk.
struct ZipEntry {
    std::string name;
    uint16_t method = 0;                 // 0 stored, 8 deflate
    uint32_t compressedSize = 0;
    uint32_t size = 0;
    uint32_t localHeaderOffset = 0;
};

class ZipArchive {
public:
    explicit ZipArchive(const std::string& path);
    ZipArchive(const ZipArchive&) = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;
    const std::vector<ZipEntry>& entries() const { return entries_; }
    Bytes read(const ZipEntry& entry) const;
private:
    std::string path_;
    mutable std::ifstream file_;
    mutable std::mutex mutex_;
    std::vector<ZipEntry> entries_;
};

// Radar (mini-map) placement for one game context and region, from
// generated.client.gamecontextuiradarinfo: lookup[(contextId, regionId)] = [(extentX, extentZ, offsetX, offsetZ)].
// The radar is a grid of 256x256 DXT1 tiles at 1 pixel per metre, named
// ui_radar_<mapTemplateId>_<regionId>_<n>.dds (n from 1, row-major, row 0 at the top = largest z),
// stored in data/ui_radar/<map>_radar.glm. Formulas from the client's ui/radarwindow.py.
struct RadarInfo {
    int32_t extentX = 0;
    int32_t extentZ = 0;
    int32_t offsetX = 0;
    int32_t offsetZ = 0;
};

inline int32_t radarMetresPerTile() { return 256; }
inline int32_t radarColumns(const RadarInfo& r) { return (r.extentX + 255) / 256; }
inline int32_t radarRows(const RadarInfo& r) { return (r.extentZ + 255) / 256; }
inline float radarLeftX(const RadarInfo& r) { return float(r.offsetX) - float(radarColumns(r)) * 128.0f; }   // world x of pixel column 0
inline float radarTopZ(const RadarInfo& r) { return float(radarRows(r)) * 128.0f - float(r.offsetZ); }       // world z of pixel row 0
inline std::string radarTileName(int32_t mapTemplateId, int32_t regionId, int32_t tileIndex) {           // tileIndex from 0
    return "ui_radar_" + std::to_string(mapTemplateId) + "_" + std::to_string(regionId) + "_" + std::to_string(tileIndex + 1) + ".dds";
}

// One part an equipable item contributes to an avatar: the pieces of swap set swapsetId taken from suit suitId
// (generated.client.equipmentdata.equipableClassMeshSwapset[(itemClassId, baseMeshId)] = [(suitId, swapsetId)]).
struct SwapsetPart {
    int32_t suitId = 0;
    int32_t swapsetId = 0;
};

// An accessory an item attaches to its wearer (client/augmentations/actor.py _AddAccessory -> body.AddAccessory).
struct AccessoryAttachment {
    std::string mesh;
    int32_t connectionPoint = 0;       // connection point id (see tr::ConnectionPointName)
};

class GameData {
public:
    explicit GameData(const std::string& gameZipPath);       // e.g. AssetLibrary::gameDataPath()

    // Modules are named by dotted path from "generated", e.g. "generated.client.entityclass".
    std::vector<std::string> moduleNames() const;
    bool hasModule(const std::string& module) const;
    const py::Object& module(const std::string& module);     // globals; evaluated once and cached
    const py::Object* global(const std::string& module, const std::string& variable);

    // Typed lookups over the generated client tables.
    std::string stringById(int32_t stringId);                 // generated.client.stringtable
    std::string className(int32_t classId);                   // generated.client.entityclass [0]
    int32_t classMeshId(int32_t classId);                     // entityclass [1]; -1 if none
    std::string classMeshName(int32_t classId);               // mesh file name via the string table; "" if none
    // generated.client.vizsetting [2] (cullingLayerType); 3 when the class has no row, as client/classmanager.py does.
    // Layer 6 is never drawn: TRasa::World::AddToWorld (FUN_00543940) skips registering the body with the graphics
    // scene (PalantirApp + 0x240, FUN_007fd5e0) when the body's culling layer is 6.
    int32_t classCullingLayer(int32_t classId);
    // generated.client.physicalentityclasshue: (r, g, b, a, r2, g2, b2, a2) bytes; client/gamemap.py uses it in place of the
    // map entity's own hues when the class has a row. false when it has none.
    bool classHue(int32_t classId, HueColour& hue1, HueColour& hue2);
    // The special-FX package (.pkg asset name) a usable class shows in a use state: generated.client.usabledata
    // specialFX[(classId, stateId, USE_STATE_NULL 97)] through the string table, as client/augmentations/usable.py
    // GetStatePkgId / AttachStateSFX. "" if none.
    std::string usableStatePackage(int32_t classId, int32_t stateId);
    std::string mapTemplateName(int32_t templateId);          // generated.client.maptemplate
    std::vector<int32_t> contextIdsForMapTemplate(int32_t templateId);   // generated.client.gamecontext [3]
    bool radarInfo(int32_t contextId, int32_t regionId, RadarInfo& info); // false if the context has no radar

    // Avatar swap sets (generated.client.equipmentdata). baseMeshId is the body's mesh string id
    // (classMeshId of e.g. NPC_Human_Swapset_Male = 4257 avatar_hum_m_nude_caucasian_v01.geo).
    // A creature's in-game name: generated.client.language.english.creaturenamelanguage lookup[nameId] (the client's
    // creaturenamelanguage module; Rasa.NET sends creature.name_id in CreatureInfoPacket). "" if none.
    std::string creatureName(int32_t nameId);
    std::vector<SwapsetPart> itemSwapsetParts(int32_t itemClassId, int32_t baseMeshId);
    // equipmentdata.equipableClassAccessoryConnectionPointWeaponOverride[(itemClass, weaponPosition)] (1 stowed, 2 drawn) if
    // present, else equipableClassAccessoryConnectionPoint[itemClass]; each (mesh string id, connection point id).
    std::vector<AccessoryAttachment> itemAccessories(int32_t itemClassId, int32_t weaponPosition);
    std::string suitName(int32_t suitId);                     // suit[id][0], e.g. "NPC Officer 1 Male"
    std::string suitMeshName(int32_t suitId);                 // suit[id][1] via the string table; "" if none
    std::string swapsetName(int32_t swapsetId);               // swapset[id], e.g. "Torso"

    // Animation sets (generated.client.animationdata): meshSkeleton[meshId] = skeletonId;
    // animationFamilyData[(skeletonId, familyId)] = [(animation string id, weight, ...)], family 1 = "Idle - Non Combat".
    int32_t meshSkeletonId(int32_t meshId);                   // -1 if the mesh has none
    std::vector<std::string> familyAnimations(int32_t skeletonId, int32_t familyId);   // .anm names, table order

private:
    ZipArchive zip_;
    std::unordered_map<std::string, size_t> moduleEntries_;   // dotted name -> zip entry
    std::unordered_map<std::string, py::Ref> cache_;
    std::mutex cacheMutex_;
};

}  // namespace tr
