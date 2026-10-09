// trformats.h - readers for Tabula Rasa data formats (C++17, depends only on zlib).
//
//   .glm   packed archive ("CHNKBLXX")                          GlmArchive
//   .thd/.tpc terrain, *_texlayers.tab, *_tribbleset.tab        readTerrain(), readTextureLayers(), readTribbleSets()
//   .map   map file (format 2.43 - 2.45)                        readMap()
//   .vis   axis-aligned BSP + potentially-visible sets          readVis()
//   .geo   model chunk file                                     readGeo()
//
// Status: .map follows the client's own loader (client/gamemap.py) and parses all 77 shipped maps
// byte-exactly. .vis follows the client's deserializers (BodyContainerAABSP / PVS) and parses all 77.
// Terrain layout and height formula are verified against in-game positions (Bootcamp, Concordia) and parse on all 66 terrains.
// Fields marked UNKNOWN were not decoded.
//
// COORDINATES: the game is LEFT-HANDED (Direct3D convention): +x right, +y up, +z forward.
// All readers return data exactly as stored. Use toRightHanded() to convert for right-handed tools
// (negate z; quaternion (w,x,y,z) -> (w,-x,-y,z); mirroring flips triangle winding).
#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace tr {

struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Bounds { Vec3 min; Vec3 max; };

using Bytes = std::vector<uint8_t>;

class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Bytes readFile(const std::string& path);

inline float component(const Vec3& v, uint32_t axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

// Left-handed (game) -> right-handed conversion.
inline Vec3 toRightHanded(const Vec3& v) { return Vec3{v.x, v.y, -v.z}; }
inline Quat toRightHanded(const Quat& q) { return Quat{-q.x, -q.y, q.z, q.w}; }

// Little-endian, bounds-checked reader over a byte buffer (or a window of it).
class ByteReader {
public:
    explicit ByteReader(const Bytes& data, size_t start = 0, size_t end = SIZE_MAX);
    size_t tell() const { return pos_; }
    size_t end() const { return end_; }
    size_t remaining() const { return end_ - pos_; }
    void seek(size_t pos);
    void skip(size_t count) { seek(pos_ + count); }
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    int32_t i32();
    uint64_t u64();
    float f32();
    Vec3 vec3();
    std::string cstr();
    Bytes bytes(size_t count);
    uint32_t peekU32() const;          // 0 if fewer than 4 bytes remain
    const uint8_t* at(size_t pos) const;
private:
    void need(size_t count) const;
    const Bytes* data_;
    size_t pos_;
    size_t end_;
};

// ------------------------------------------------------------------------------------------
// .glm packed archive
//   [file data: one zlib stream per file, each starting on a 1 KiB boundary]
//   [name table: NUL-terminated names, same order as the directory]
//   [directory: char[8] "CHNKBLXX", u32 nameTableOffset, u32 nameTableLength, u32 fileCount,
//               fileCount x { u32 offset, u32 storedSize, u32 rawSize, u32 nameOffset,
//                             u16 flags, u32 unixTime }]          (22 bytes each, byte-order sorted by name)
//   [footer: u32 0, u32 directoryOffset]                          <- read the last u32 first
//   flags != 0 means zlib-compressed. File names are unique across all shipped archives.
// ------------------------------------------------------------------------------------------
struct GlmEntry {
    std::string name;
    uint32_t offset = 0;
    uint32_t storedSize = 0;
    uint32_t rawSize = 0;
    uint32_t nameOffset = 0;
    uint16_t flags = 0;
    uint32_t timestamp = 0;
};

// Streams from disk: only the directory and name table are read up front; entries are read and
// inflated on demand (thread-safe). Name lookup is case-insensitive.
class GlmArchive {
public:
    explicit GlmArchive(const std::string& path);
    GlmArchive(const GlmArchive&) = delete;
    GlmArchive& operator=(const GlmArchive&) = delete;
    const std::string& path() const { return path_; }
    const std::vector<GlmEntry>& entries() const { return entries_; }
    const GlmEntry* find(const std::string& name) const;
    Bytes read(const GlmEntry& entry) const;
    Bytes read(const std::string& name) const;
private:
    std::string path_;
    mutable std::ifstream file_;
    mutable std::mutex mutex_;
    std::vector<GlmEntry> entries_;
    std::unordered_map<std::string, size_t> index_;     // lower-case name -> entry
};

// ASCII lower-casing used for every case-insensitive name comparison.
std::string toLowerAscii(std::string text);

// ------------------------------------------------------------------------------------------
// Terrain (inside t<id hex8>_terrain.glm)
//   t<id>_<terrainId>.thd (60 bytes): u32 version (1), u32 terrainFormat (1 or 2; meaning UNKNOWN),
//       u32 UNKNOWN x2 (always 12, 12), f32 boundsMin x,y,z, f32 boundsMax x,y,z, f32 sampleSpacing (1.0),
//       u32 samplesPerPatch x,z (128), u32 patchCount x,z
//   t<id>_<patch index, lowercase hex>_<terrainId>.tpc: u32 type (2), then samplesPerPatch^2 samples
//       of 12 bytes, row-major. Patch n is at patch row n / patchesX, column n % patchesX.
//       Patches do NOT share edges: patch column p covers samples p*128 .. p*128+127 (world x =
//       boundsMin.x + sample * spacing). Patches may be absent (treat as no data).
//   Sample: u16 word, 4 x { u8 layerIndex, u8 weight }, u8[2] unused.
//   word: bits 0-8 integer height, bits 9-11 surface category, bits 12-15 fractional height.
//   World height: h13 = (integer << 4) | fraction;  y = h13 * 2 * boundsMax.y / 8192 - boundsMax.y
//   World position of grid sample (row, column): x = boundsMin.x + column * spacing,
//                                                z = boundsMin.z + row * spacing
//   Surface category (meaning inferred from slope statistics over all maps, not from code):
//       0 = mostly out-of-bounds padding, 2 = steep/cliff, 3 = ordinary ground, 1/4-7 = special (UNKNOWN)
// ------------------------------------------------------------------------------------------
struct TerrainHeader {
    uint32_t version = 0;
    uint32_t terrainFormat = 0;
    uint32_t unknownGridX = 0;
    uint32_t unknownGridZ = 0;
    Bounds bounds;
    float spacing = 1.0f;
    uint32_t samplesPerPatchX = 0;
    uint32_t samplesPerPatchZ = 0;
    uint32_t patchesX = 0;
    uint32_t patchesZ = 0;
};

struct LayerBlend {
    uint8_t layerIndex = 0;      // index into readTextureLayers()
    uint8_t weight = 0;          // 0-255 (the client turns these into per-patch RGBA alpha masks)
};

struct TerrainSample {
    uint16_t word = 0;
    LayerBlend layer0;
    LayerBlend layer1;
    LayerBlend layer2;
    LayerBlend layer3;
};

inline uint16_t heightInteger(uint16_t word) { return static_cast<uint16_t>(word & 0x1FF); }
inline uint8_t surfaceCategory(uint16_t word) { return static_cast<uint8_t>((word >> 9) & 0x7); }
inline uint8_t heightFraction(uint16_t word) { return static_cast<uint8_t>(word >> 12); }
inline uint16_t heightFixed13(uint16_t word) {
    return static_cast<uint16_t>((heightInteger(word) << 4) | heightFraction(word));
}
float worldHeight(const TerrainHeader& header, uint16_t word);
const LayerBlend& sampleLayer(const TerrainSample& sample, int index);    // index 0-3

struct Terrain {
    TerrainHeader header;
    uint32_t terrainId = 0;
    uint32_t rows = 0;                          // full grid, including padding past the bounds
    uint32_t columns = 0;
    std::vector<TerrainSample> samples;         // row-major
    std::vector<uint8_t> sampleHasData;         // 1 where a present patch covers the sample
    std::vector<uint8_t> patchPresent;          // patchesX * patchesZ, row-major

    const TerrainSample& sampleAt(uint32_t row, uint32_t column) const {
        return samples[static_cast<size_t>(row) * columns + column];
    }
    bool hasData(uint32_t row, uint32_t column) const {
        return sampleHasData[static_cast<size_t>(row) * columns + column] != 0;
    }
    bool hasPatch(uint32_t patchX, uint32_t patchZ) const {
        return patchPresent[static_cast<size_t>(patchZ) * header.patchesX + patchX] != 0;
    }
    float worldX(uint32_t column) const { return header.bounds.min.x + float(column) * header.spacing; }
    float worldZ(uint32_t row) const { return header.bounds.min.z + float(row) * header.spacing; }
    float worldHeightAt(uint32_t row, uint32_t column) const {
        return worldHeight(header, sampleAt(row, column).word);
    }
    // Bilinear height at a world position; returns false outside the grid or where data is missing.
    bool heightAtWorld(float x, float z, float& height) const;
    uint32_t boundedRows() const;               // rows inside the header bounds
    uint32_t boundedColumns() const;
};

Terrain readTerrain(const GlmArchive& glm);
// OBJ export. rightHanded = true negates z and reverses winding (correct for OBJ / Blender / OpenGL).
void writeTerrainObj(const Terrain& terrain, const std::string& path, uint32_t step, bool rightHanded = true);

// t<id>_texlayers.tab: u32 version (3 or 4), u32 count,
//   count x { name\0, texturePath\0, [version 4: normalMapPath\0, extraMapPath\0], f32 tiling,
//             u32 n, n x tribbleSetName\0 }
// In version 4 the extra paths are often empty or placeholders (normal_standin_terrain.dds,
// default_black.dds); the meaning of the third map is UNKNOWN (gloss or specular are likely).
struct TextureLayer {
    std::string name;
    std::string texture;
    std::string normalMap;               // version 4 only
    std::string extraMap;                // version 4 only
    float tiling = 0;
    std::vector<std::string> tribbleSets;
};
std::vector<TextureLayer> readTextureLayers(const GlmArchive& glm);

// t<id>_tribbleset.tab (optional; empty result if absent): u32 version, u32 setCount,
//   setCount x { name\0, u32 UNKNOWN, u32 modelCount, modelCount x { model.geo\0, 57-byte block } }
// Named block fields are guesses from their values; the full block is kept in `raw`.
struct TribbleModel {
    std::string model;
    float density = 0;           // +0
    float drawDistance = 0;      // +4
    float rotationRange = 0;     // +28
    float scaleMin = 0;          // +32
    float scaleMax = 0;          // +36
    uint32_t seed = 0;           // +44
    Bytes raw;
};
struct TribbleSet {
    std::string name;
    uint32_t unknown = 0;
    std::vector<TribbleModel> models;
};
std::vector<TribbleSet> readTribbleSets(const GlmArchive& glm);

// ------------------------------------------------------------------------------------------
// .map (format 2.43 - 2.45, as read by the client's client/gamemap.py)
//   u16 minorVersion, u16 majorVersion (2), i32 templateVersion (checked against the server)
//   MapDesc:            i32 status
//   Terrain:            i32 count, count x i32 terrainId   (archive t<mapTemplateId hex8>_terrain.glm)
//   TerrainVisSettings: i32 detailLevel, i32 algorithm, f32 maxDistance, f32 fadeDistance
//   Entities 2.45:      i32 count, each { i32 classId, u32 idLow, u32 idHigh, f32 location x,y,z,
//                         f32 orientation w,x,y,z, u8 hue1 rgba, u8 hue2 rgba, f32 scale, i32 flags,
//                         i32 cullLayer, i32 numParticles, numParticles x particle, f32 boundsMin x,y,z,
//                         f32 boundsMax x,y,z }
//   Entities 2.43/2.44: i32 entitySize, i32 count, each { entitySize bytes (first 60 parsed: classId,
//                         idLow, idHigh, location, orientation w,x,y,z, hue rgba, scale, flags, cullLayer,
//                         numParticles), particles, i32 isWater [, i32 waterVersion, 172 or 44 bytes] }
//   particle:           i32 assetId (string table), i32 connectionPoint, f32 direction x,y,z, u8 type
//   GeneratedABVs:      i32 count, each { u32 idLow, u32 idHigh, u8 shape, f32 extents x,y,z }
//   TimeSettings:       i32 dayLengthSeconds, i32 startingSkyTime
//   CameraScripts:      i32 count, each { i32 id, i32 returnTimeMs, i32 n, n x { i32 frame, f32 position x,y,z,
//                         f32 rotation w,x,y,z, f32 focalLength, i32 arriveTimeMs, i32 holdTimeMs } }
//   RegionSettings:     i32 count, 2.44+: 8 x i32 each; 2.43: 10 x i32 each (three music ids)
//   PointLights / AudioEmitters: no data (the client loads lights.xml / audioemitters.xml)
// Mesh of an entity: generated.client.entityclass.lookup[classId][1] -> stringtable -> .geo name.
// ------------------------------------------------------------------------------------------
struct HueColour {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint8_t alpha = 0;
};

struct MapParticle {
    int32_t assetId = 0;
    int32_t connectionPoint = 0;
    Vec3 direction;
    uint8_t type = 0;
};

struct MapEntity {
    int32_t classId = 0;
    uint64_t entityId = 0;
    Vec3 location;
    Quat orientation;                     // unit quaternion; stored in the file as w, x, y, z
    HueColour hue1;
    HueColour hue2;                       // 2.45 only
    float scale = 1;
    int32_t flags = 0;
    int32_t cullLayer = 0;
    std::vector<MapParticle> particles;
    bool hasBounds = false;               // 2.45 only
    Bounds bounds;                        // local (model-space) bounds
    bool hasWater = false;                // 2.43/2.44 only
    int32_t waterVersion = 0;
    Bytes waterData;                      // UNKNOWN layout (172 bytes for version 1, 44 for version 2)
};

inline uint32_t entityIdLow(uint64_t id) { return static_cast<uint32_t>(id & 0xFFFFFFFF); }
inline uint32_t entityIdHigh(uint64_t id) { return static_cast<uint32_t>(id >> 32); }
inline uint64_t makeEntityId(uint32_t low, uint32_t high) { return (static_cast<uint64_t>(high) << 32) | low; }

struct MapGeneratedAbv {
    uint64_t entityId = 0;
    uint8_t shape = 0;
    Vec3 extents;
};

struct MapCameraKeyframe {
    int32_t frameNumber = 0;
    Vec3 position;
    Quat rotation;
    float focalLength = 0;
    int32_t arriveTimeMs = 0;
    int32_t holdTimeMs = 0;
};

struct MapCameraScript {
    int32_t id = 0;
    int32_t returnTimeMs = 0;
    std::vector<MapCameraKeyframe> keyframes;
};

struct MapRegion {
    int32_t regionId = 0;
    int32_t priority = 0;
    int32_t enabledFlags = 0;
    int32_t ambient = 0;
    int32_t music = 0;
    int32_t music2 = -1;                  // 2.43 only
    int32_t music3 = -1;                  // 2.43 only
    int32_t environmentMap = 0;
    int32_t sky = 0;
    int32_t displayName = 0;
};

struct MapTerrainVisSettings {
    int32_t detailLevel = 0;
    int32_t algorithm = 0;
    float maxDistance = 0;
    float fadeDistance = 0;
};

struct MapFile {
    uint16_t minorVersion = 0;
    uint16_t majorVersion = 0;
    int32_t templateVersion = 0;
    int32_t status = 0;
    std::vector<int32_t> terrainIds;
    MapTerrainVisSettings terrainVis;
    std::vector<MapEntity> entities;
    std::vector<MapGeneratedAbv> generatedAbvs;
    int32_t dayLengthSeconds = 0;
    int32_t startingSkyTime = 0;
    std::vector<MapCameraScript> cameraScripts;
    std::vector<MapRegion> regions;
    size_t bytesConsumed = 0;
    size_t fileSize = 0;
};

MapFile readMap(const Bytes& data);
MapFile readMap(const std::string& path);

// ------------------------------------------------------------------------------------------
// .vis (as read by the client's BodyContainerAABSP / PVS deserializers)
//   u8 streamFlags (2 = binary), u32 containerVersion (1), u8 hasTree
//   tree: u32 treeVersion (1-3; all shipped maps are 3), u32 nodeFormatVersion (1), u32 nodeCount,
//         nodes (52 bytes): u32 index, i32 parent, i32 child0, i32 child1, u32 leafIndex (0xFFFFFFFF = interior),
//         f32 boundsMin x,y,z, f32 boundsMax x,y,z, u32 splitAxis (0..2, 3 = leaf), f32 d (split = -d)
//   u8 hasPvs; PVS: u32 version (1-2), u32 leafCount,
//         leafCount x { u32 dataSize, u64 timestamp, u32 compressed (version 2 only), data }
//         data = zlib stream of a bitset: leaf i visible <=> bit (i & 0x1F) of u32 word (i >> 5)
//   u8 flag; then one trailing byte the client never reads
// ------------------------------------------------------------------------------------------
struct VisNode {
    uint32_t index = 0;
    int32_t parent = -1;
    int32_t child0 = -1;
    int32_t child1 = -1;
    uint32_t leafIndex = 0xFFFFFFFF;
    Bounds bounds;
    uint32_t splitAxis = 3;
    float planeD = 0;

    bool isLeaf() const { return splitAxis == 3; }
    float splitPosition() const { return -planeD; }
};

struct VisPvsRow {
    uint32_t dataSize = 0;
    uint64_t timestamp = 0;
    bool compressed = false;
    bool damaged = false;                 // stream ended early (seen in characterselection.vis); padded with zeros
    std::vector<uint32_t> words;          // empty when the leaf has no PVS data
};

inline bool pvsHasData(const VisPvsRow& row) { return !row.words.empty(); }
inline bool pvsLeafVisible(const VisPvsRow& row, uint32_t leaf) {
    return ((row.words[leaf >> 5] >> (leaf & 0x1F)) & 0x1) != 0;
}

struct VisFile {
    uint8_t streamFlags = 0;
    uint32_t containerVersion = 0;
    bool hasTree = false;
    uint32_t treeVersion = 0;
    uint32_t nodeFormatVersion = 0;
    std::vector<VisNode> nodes;
    bool hasPvs = false;
    uint32_t pvsVersion = 0;
    uint32_t leafCount = 0;
    std::vector<VisPvsRow> rows;          // indexed by leaf index
    bool hasTrailingFlag = false;
    uint8_t trailingFlag = 0;
    size_t damagedRows = 0;
    size_t bytesConsumed = 0;
    size_t fileSize = 0;
};

VisFile readVis(const Bytes& data);
VisFile readVis(const std::string& path);
// Index of the leaf node containing a point (descends by split planes); -1 if outside the root.
int32_t findLeafNode(const VisFile& vis, const Vec3& point);

// ------------------------------------------------------------------------------------------
// .geo model (chunk file). File starts "CHNK" 'B' 'L' "XX" (binary, little-endian). Every chunk:
//   u32 tag (FourCC as a little-endian u32, so "GBOD" is stored as the bytes D O B G),
//   u32 payloadSize, u32 version, u32 reserved (0), then payloadSize bytes.
// GBOD (gfxBody) -> BBOX, CPDG, property table, PSKE skeleton, pieces (GPCE / GSKN), shadows.
// GPCE (gfxGeometryPiece) -> EFCT material, INDX indices, VERT vertices (DECL layout), BBOX, bone name.
// GSKN wraps a GPCE plus the bone palette for skinning (3 bones per vertex: BLENDWEIGHT FLOAT2 +
// BLENDINDICES D3DCOLOR). Sub-chunks that are not needed are skipped by size.
// ------------------------------------------------------------------------------------------
constexpr uint32_t fourcc(const char (&tag)[5]) {
    return (uint32_t(uint8_t(tag[0])) << 24) | (uint32_t(uint8_t(tag[1])) << 16) |
           (uint32_t(uint8_t(tag[2])) << 8) | uint32_t(uint8_t(tag[3]));
}
std::string tagName(uint32_t tag);

// D3D9 vertex element: type = D3DDECLTYPE, method = D3DDECLMETHOD, usage = D3DDECLUSAGE.
struct VertexElement {
    uint8_t type = 0;
    uint8_t method = 0;
    uint8_t usage = 0;
    uint8_t usageIndex = 0;
    uint32_t offset = 0;
};

struct GeoVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    uint32_t colour = 0xFFFFFFFF;      // D3DCOLOR (ARGB)
};

struct GeoParameter {
    std::string key;
    std::string value;                 // stringified
};

struct GeoPiece {
    bool skinned = false;
    uint32_t version = 0;
    std::string effectName;
    std::vector<GeoParameter> parameters;
    std::string diffuseTexture;        // the "DiffuseTexture" parameter, if present
    std::vector<VertexElement> layout;
    uint32_t stride = 0;
    std::vector<GeoVertex> vertices;
    std::vector<uint32_t> indices;     // triangle list, Direct3D (clockwise-front) winding
    Bounds bounds;
    std::string boneName;              // bone this piece is attached to
    std::vector<std::string> skinBones;
};

struct GeoBone {
    std::string name;
    int32_t parent = -1;
    Quat rotation;
    Vec3 translation;
    Vec3 scale{1, 1, 1};
};

struct GeoProperty {
    int32_t id = 0;
    std::string key;
    std::string value;
};

struct GeoModel {
    uint32_t version = 0;
    Bounds bounds;
    std::vector<GeoProperty> properties;
    std::vector<GeoBone> bones;
    std::vector<GeoPiece> pieces;
    uint32_t shadowCount = 0;
};

GeoModel readGeo(const Bytes& data);
// OBJ export. Pieces attached to a bone are moved by it (rotate, scale, translate - as TRRM does).
// rightHanded = true negates z (Direct3D winding then reads correctly as OBJ's counter-clockwise).
void writeGeoObj(const GeoModel& model, const std::string& objPath, bool applyBoneTransforms, bool rightHanded = true);

}  // namespace tr
