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

// Writes a .glm laid out like the shipped ones (survey of all 181 archives, 2026-10-16): file data from offset 0, each entry on
// a 1 KiB boundary (zero padding), the name table after the data, the directory after the names, then u32 0 and the directory
// offset; entries sorted by name case-insensitively (180 of 181; the exception is the user's own re-packed ui.glm, which the
// game loads, so the order is a convention, not a lookup requirement). Compressed entries are one zlib stream at level 1
// (every shipped one starts 78 01) with flags 1; an entry is stored (flags 0) when compressing does not make it smaller or
// `compress` is false. Names must be unique ignoring case. The archive is built in memory and written to `path` through a
// temporary file. How the game is pointed at a new archive: see CLAUDE.md "How client changes reach the game".
struct GlmSource {
    std::string name;                  // bare file name, e.g. "npc_hum_m_officer_v01_df.dds"
    Bytes data;
    bool compress = true;
    uint32_t timestamp = 0;            // unix time; 0 = now
};
Bytes buildGlmArchive(std::vector<GlmSource> files);
void writeGlmArchive(const std::string& path, std::vector<GlmSource> files);

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
//   word: bits 0-8 integer height, bits 9-11 surface category, bits 12-15 fractional height. (The game reads the
//       word big-endian: 12-bit height + three flags; see terrainFlag. Bit 8 here is flag 0, not height.)
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
    uint8_t layerIndex = 0;      // 1-based: texture layer layerIndex - 1 of readTextureLayers(); 0 = none (textureLayerOf)
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
// The texlayers.tab entry a slot names, or -1 for layer 0 (no layer). Verified in tabula_rasa.exe: FUN_0072f3b0 maps
// layer n to entry n - 1 when 1 <= n <= the entry count, else -1; the largest index in shipped samples is the count.
inline int textureLayerOf(const LayerBlend& blend) { return int(blend.layerIndex) - 1; }
// How much of a sample one slot's layer covers, 0-255, as the client builds its terrain alpha masks
// (alphamaskmgr / FUN_00743230, weight FUN_0093fdd0): layers composite in layer-index order, higher indices over
// lower - the slot's weight times (1 - weight) of every slot with a higher layer index, 0 at or below 0.001, then
// int(x * 255 + 0.5). Not normalised: the colour is the sum of layer texture x coverage, and what no layer covers
// stays black (the base pass writes albedo 0). Layer 0 means no layer (it feeds only the base pass's mask), so its
// slots return 0. (A layer named in two slots would count twice; no shipped sample does that.)
uint8_t slotCoverage(const TerrainSample& sample, int index);

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

// t<id>_tribbleset.tab (optional; empty result if absent) - terrain clutter ("tribbles": grass, ferns, flowers,
// debris). Layout verified in tabula_rasa.exe (FUN_0072d040, entry readers FUN_0072dd40 format 1 / FUN_0072df60
// formats 2-5) and on all 50 shipped files (every byte parsed; all are version 1, every set format 5):
//   u32 version (1), u32 setCount,
//   setCount x { name\0, u32 format (1-5), u32 modelCount, modelCount x {
//       model.geo\0, f32 density (clamped 0.001..100), f32 drawDistance (at least 1), u32 orientation,
//       u32 randomYaw, f32 heightMin, f32 heightMax, f32 slopeMin, f32 slopeMax (radians, clamped 0..pi/2),
//       [format >= 2: f32 scaleMin, f32 scaleMax], u32 flags, u32 seed,
//       [format 3: u32 unknown54 | format >= 4: u8 unknown54], [format 5: f32 unknown24, f32 unknown28] } }
// The texlayers.tab entry of a terrain layer names its sets (TextureLayer::tribbleSets).
struct TribbleModel {
    std::string model;
    float density = 0.1f;        // instances per square metre at full layer weight
    float drawDistance = 25.0f;  // the cell visibility radius is the largest of a cell's models (FUN_0074c140)
    // 0 upright, 1 upright + random tilt (+-0.1 in x and z), 2 halfway between up and the terrain normal + random
    // tilt, 3 terrain normal, 4 terrain normal + random tilt (FUN_0074c690; see placeTribbleCell)
    uint32_t orientation = 0;
    uint32_t randomYaw = 1;      // 1: a random turn about the up axis
    float heightMin = -1e10f;    // height band (only applied when heightMin > 1e-5; 10 % fades at both ends)
    float heightMax = 1e10f;
    float slopeMin = 0.0f;       // slope band, radians (acos of the normal's y; 10 % fades)
    float slopeMax = 3.1415999f;  // default before reading (the reader clamps to pi/2)
    float scaleMin = 1.0f;
    float scaleMax = 1.0f;
    uint32_t flags = 0;          // bit 0 kept at entry + 0x4c; meaning UNKNOWN (not read by the placement code)
    uint32_t seed = 0;           // random stream = cell index + seed (0 -> 342)
    uint8_t unknown54 = 0;       // UNKNOWN (not read by the placement code)
    float unknown24 = 25.0f;     // UNKNOWN (format 5 only; not read by the placement code)
    float unknown28 = 1.0f;
};
struct TribbleSet {
    std::string name;
    uint32_t format = 0;
    std::vector<TribbleModel> models;
};
std::vector<TribbleSet> readTribbleSets(const GlmArchive& glm);

// Terrain sample flags. The .tpc word is big-endian in the game's reader (FUN_0094d6b0; writer FUN_0094d540):
// height12 = word_be >> 4, flags 0-2 = word_be bits 0-2. In terms of the little-endian `word` stored here, flag n is
// bit 8 + n - so flag 0 is the bit heightInteger() includes as height bit 12 (worldHeight sinks samples without it by
// boundsMax.y), and flags 1 and 2 are surfaceCategory() bits 0 and 1. The in-memory sample keeps them as byte + 4
// bits 0-2 (FUN_0090a3f0), copied into the 28-byte cell record at + 0xC / 0xD / 0xE (FUN_00565010).
// Flag 0 feeds the terrain base mask (see terrain layer blending); flag 2 allows terrain clutter.
inline bool terrainFlag(uint16_t word, int index) { return ((word >> (8 + index)) & 0x1) != 0; }

// Terrain clutter placement, as TribbleLayer builds one cell (tabula_rasa.exe FUN_0074bc20 / FUN_0074c2c0 /
// FUN_0074c3a0 / FUN_0074c690). Cells are 32 x 32 samples, numbered row-major (tribbleCellsX across); a cell covers
// samples x0 .. x0 + 32, z0 .. z0 + 32 inclusive (clamped to the grid). The client keeps the cells within 3 of the
// camera's cell (FUN_0073b930).
struct TribbleInstance {
    uint32_t set = 0;            // index into the TribbleSet list
    uint32_t model = 0;          // index into that set's models
    uint32_t layer = 0;          // the sample layer (1-based, texlayers entry layer - 1) that placed it
    // Drawing: the cell keeps one TribbleGeoContainer per mesh name (FUN_0074c140), created by the first model entry
    // that uses the mesh (its draw distance and seed apply). When first shown, a container shuffles its instances
    // (seed cell index + that entry's seed: for i, j = trunc(random x (N - 1)), swap i and j - FUN_00751050) and draws
    // the first trunc(N x factor), factor = FloraDensity x the RQS flora scalar (1 at full quality). placeTribbleCell
    // returns each container's instances in shuffled order: draw an instance when rank < trunc(containerSize x factor).
    uint32_t container = 0;      // container index within the cell (creation order)
    uint32_t rank = 0;           // position after the shuffle
    uint32_t containerSize = 0;
    float containerDrawDistance = 0;
    Vec3 position;
    Quat rotation;               // D3DX quaternion (yaw about +y, then the tilt)
    float scale = 1.0f;
};
uint32_t tribbleCellsX(const Terrain& terrain);
uint32_t tribbleCellsZ(const Terrain& terrain);
std::vector<TribbleInstance> placeTribbleCell(const Terrain& terrain, const std::vector<TextureLayer>& layers,
                                              const std::vector<TribbleSet>& sets, uint32_t cellIndex);

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
    Bytes recordTail;                     // 2.43/2.44: the record's bytes after the 60 parsed ones (UNKNOWN; kept for writeMap)
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
    int32_t entityRecordSize = 0;         // 2.43/2.44: the file's entity record size (>= 60)
    Bytes trailing;                       // bytes after the parsed sections (none in the shipped maps; kept for writeMap)
    size_t bytesConsumed = 0;
    size_t fileSize = 0;
};

MapFile readMap(const Bytes& data);
MapFile readMap(const std::string& path);
// The inverse of readMap: the same layout for the map's own version. A map read and written back unchanged is byte-identical
// (checked on every shipped map: viewport_test RASA_MAPROUNDTRIP=1). New entities in a 2.43/2.44 map get a zero-filled record tail.
Bytes writeMap(const MapFile& map);

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
    Vec3 position1;                    // POSITION1, if present (TRSphBillboard: the sprite's centre; POSITION is the corner offset)
    Vec3 normal;
    Vec2 uv;
    Vec2 uv1;                          // TEXCOORD1, if present (e.g. GlowTexCoord 1)
    Vec2 uv2;                          // TEXCOORD2, if present (e.g. DecalTexCoord 2)
    Vec2 uv3;                          // TEXCOORD3, if present (water: VolMapTexCoord 3)
    // Tangent frame for normal maps (Pal* effects): TANGENT0 = tangent, TANGENT1 = binormal (the Pal vertex shader
    // vs2x_OffsetVSNoParallax reads dcl_tangent / dcl_tangent1; verified 2026-10-11).
    Vec3 tangent;
    Vec3 binormal;
    uint32_t colour = 0xFFFFFFFF;      // D3DCOLOR (ARGB)
    // Skinning (GSKN pieces): BLENDWEIGHT holds weightCount - 1 explicit weights, the last is 1 - their sum;
    // BLENDINDICES is a D3DCOLOR whose bytes 0, 1, 2, 3 (no swizzle; verified 2026-10-10) index the piece's skinBones.
    float blendWeight0 = 1, blendWeight1 = 0, blendWeight2 = 0, blendWeight3 = 0;
    uint32_t blendIndices = 0;
};

inline uint32_t blendIndex(const GeoVertex& v, uint32_t i) { return (v.blendIndices >> (i * 8)) & 0xFF; }
inline float blendWeight(const GeoVertex& v, uint32_t i) {
    switch (i) {
        case 0: return v.blendWeight0;
        case 1: return v.blendWeight1;
        case 2: return v.blendWeight2;
        default: return v.blendWeight3;
    }
}

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
    // Avatar swap sets (GPCE v8+, verified over all 18,748 such pieces): the body region this piece belongs to
    // (equipmentdata.swapset: 1 Torso, 2 Legs, 3 Boots, 4 Skin/Face, 5 Hair/Helm, 6 Gloves, 7 Face Hi, 8 Face Lo;
    // 0 = not swappable, 99 = low-LOD stand-in), its group ("flesh", "undies", "hair", the suit's name) and name.
    uint32_t swapId = 0;
    std::string swapGroup;
    std::string pieceName;             // e.g. "Torso", "Torso_LOD1"
    std::vector<GeoParameter> userData;    // USDA key/value pairs, e.g. SwapID, LOD, LODPercentage
    uint32_t lod = 0;                  // USDA "LOD" (any case); 0 = full detail
};

// Avatar skin/hair/clothing hueing: effects with "Hue" in the name add a colour weighted by the diffuse alpha.
bool isHuedEffect(const std::string& effectName);

struct GeoBone {
    std::string name;
    int32_t parent = -1;
    Quat rotation;
    Vec3 translation;
    Vec3 scale{1, 1, 1};
    // BDAT's 4x4 matrix: the inverse bind pose in mesh space (Direct3D row-vector layout, translation in the last row):
    // bone-local = v.x * inverseBindX + v.y * inverseBindY + v.z * inverseBindZ + inverseBindT. Skinning uses it as
    // posed world x inverse bind; it can differ from the inverse of the PSKE transforms (npc_eloh_hologram_large).
    bool hasInverseBind = false;
    Vec3 inverseBindX{1, 0, 0};
    Vec3 inverseBindY{0, 1, 0};
    Vec3 inverseBindZ{0, 0, 1};
    Vec3 inverseBindT{0, 0, 0};
};

struct GeoProperty {
    int32_t id = 0;
    std::string key;
    std::string value;
};

// Contact point (GBOD CPDG chunk; verified 2026-10-10 on all 7,238 models, 36,840 points): CPDG v1 { u32 count,
// count x CPDF v1 { u32 id, i32 bone (PSKE index, -1 = model space), quaternion x y z w, position, u32 pairCount,
// pairCount x (cstr key, cstr value) } }. Keys: cpName, lsys "<cp>,<light>.ltmp", psys "<cp>,<system>.pfx", light
// parameters (Range, ConeAngle, ...).
struct GeoContactPoint {
    uint32_t id = 0;
    int32_t bone = -1;
    Quat rotation;
    Vec3 position;
    std::vector<GeoParameter> values;
};

// A contact point's connection-point identity (TRasa::ConnectionPointUtil::ParseName, tabula_rasa.exe FUN_00924470,
// verified 2026-10-13): from its "cpNameId" value ("<type letter><usage letter><number>": H 1, P 2, S 3, D 4; S 1, P 2)
// or else its "cpName": a 4-character prefix from the table at 0x00d02aa0 (HPP_ 1/2, HPS_ 1/1, PPP_ 2/2, PPS_ 2/1,
// SPP_ 3/2, SPS_ 3/1, DPP_ 4/2, DPS_ 4/1 = type / usage) and the rest matched case-insensitively against the name table at
// 0x00d02b10 (= generated.client.connectionpoint: 11 Right_Hand, 14 Left_Hand, 28 VFX1, 66 Pistol_Holster, ...).
// The CPDF id is NOT this id (avatar_hum_m_nude_caucasian_v01: HPS_Right_Hand has CPDF id 8). Lookups by connection point
// (FUN_00924880) match type 1 and the requested usage: 1 (HPS_, the socket on a body; special-FX events, an avatar's side
// of an accessory) or 2 (HPP_, the plug on an accessory, TRasa::EntityBody::FinalizeAddAccessory FUN_00502060).
struct ConnectionPointName {
    bool valid = false;
    uint32_t type = 0;
    uint32_t usage = 0;
    uint32_t id = 0;
};
ConnectionPointName parseConnectionPointName(const GeoContactPoint& cp);
std::string connectionPointIdName(uint32_t id);       // "Right_Hand" etc.; "" if unknown

struct GeoModel {
    uint32_t version = 0;
    Bounds bounds;
    std::vector<GeoContactPoint> contactPoints;
    std::vector<GeoProperty> properties;
    std::vector<GeoBone> bones;
    std::vector<GeoPiece> pieces;
    uint32_t shadowCount = 0;
};

GeoModel readGeo(const Bytes& data);
// OBJ export. Pieces attached to a bone are moved by it (rotate, scale, translate - as TRRM does).
// rightHanded = true negates z (Direct3D winding then reads correctly as OBJ's counter-clockwise).
void writeGeoObj(const GeoModel& model, const std::string& objPath, bool applyBoneTransforms, bool rightHanded = true);

// ------------------------------------------------------------------------------------------
// Animation (.anm). Verified 2026-10-10 over all 6,238 files (6,236 parse; 2 are 68-byte stubs):
//   ANIM v4 { u32 trackCount, u8 0, then per track: fourcc (UNKNOWN; often the key chunk's), TRAK v3 {
//     u8 0, fourcc target ("SKEL" bone / "EFCT" material), cstr name, u8 0, u32 flags (UNKNOWN: 95, 26, 0),
//     u32 keyCount, key chunk } }, then 4 UNKNOWN bytes.
// Key chunks list their components in the order scale, rotation, translation, then time:
//   K* (KEYT, KEYR, KEYS, KERT, KEST, KESR, KSRT): float32 scale xyz, quaternion xyzw, translation xyz, time
//   X* (XKER, XKRT, XKSR, XSRT): half scale, int16/32767 quaternion, half translation, half time
// Keys are absolute local bone transforms in the .geo skeleton's convention (track names = PSKE bone names);
// 30 keys per second. Components a track lacks keep the bone's bind value.
// ------------------------------------------------------------------------------------------
struct AnimKey {
    float time = 0;                    // seconds
    Vec3 scale{1, 1, 1};
    Quat rotation;
    Vec3 translation;
};

struct AnimTrack {
    std::string name;                  // bone name for "SKEL" tracks
    uint32_t target = 0;               // fourcc as read (u32 little-endian of the reversed text, like chunk tags)
    uint32_t keyKind = 0;              // key chunk tag
    uint32_t flags = 0;                // UNKNOWN
    bool hasScale = false;
    bool hasRotation = false;
    bool hasTranslation = false;
    std::vector<AnimKey> keys;
};

// Material ("EFCT") tracks animate a texture transform of every .geo piece whose GPCE group (GeoPiece::swapGroup) is the
// track's group. Layout (verified on all .anm, 106,229 keys): cstr group, cstr parameter (TexCoordTransform0..2), u32 0,
// u32 keyCount, KEUV chunk of keyCount x 8 f32. Key = scaleU, scaleV, rotation (3), translateU, translateV, time (exe
// FUN_00609c90 reader, FUN_006d2360 interpolates every component linearly). Verified in game 2026-10-13: the Bootcamp
// hologram's "hologram 2d" pieces (TexCoordTransform0 translateV 0 -> 3 over 6 s) repeat every 1.99 s in a capture.
struct MaterialKey {
    float scaleU = 1, scaleV = 1;
    Vec3 rotation;                     // only z (component 4) is used, radians
    float translateU = 0, translateV = 0;
    float time = 0;                    // seconds
};

struct MaterialTrack {
    std::string group;                 // .geo piece group
    std::string parameter;             // effect parameter, e.g. "TexCoordTransform0"
    std::vector<MaterialKey> keys;
};

struct Animation {
    uint32_t version = 0;
    float duration = 0;                // last key time over all tracks (bone and material)
    std::vector<AnimTrack> tracks;     // bone ("SKEL") tracks
    std::vector<MaterialTrack> materialTracks;
};

Animation readAnimation(const Bytes& data);

// ------------------------------------------------------------------------------------------
// Animation markers (.mkr, anim01/02.glm): events at times in the animation of the same name (x.mkr <-> x.anm), e.g. the
// strike of an attack, footsteps, sounds. Chunk file AMRK v1 { u32 count, count x { u8 type, u32 time (milliseconds) } }.
// Marker types: the engine's name table (tabula_rasa.exe, the switch at 0x00820e00, 61 cases; anything else "unknown") - 0-28
// equal client/animmarkers.py (START .. UNPAUSE_HEAD_TRACKING), 29 PreStrike, 30-36 Footstep2-8, 37-44 Audio1-8, 45-48
// Windup5-8, 49-52 Resolve5-8, 53-60 Timer1-8.
// ------------------------------------------------------------------------------------------
struct AnimMarker {
    uint8_t type = 0;
    uint32_t timeMs = 0;
};

struct AnimationMarkers {
    uint32_t version = 0;
    std::vector<AnimMarker> markers;   // in file order
};

AnimationMarkers readAnimationMarkers(const Bytes& data);
const char* animMarkerTypeName(int type);   // the engine's names ("Strike", "Footstep1", ...); "unknown" outside 0-60
const AnimTrack* findTrack(const Animation& animation, const std::string& boneName);
// Interpolated key at time (clamped to the track's range): linear scale/translation, normalised lerp rotation.
AnimKey sampleTrack(const AnimTrack& track, float time);
MaterialKey sampleMaterialTrack(const MaterialTrack& track, float time);   // linear, clamped to the track's range
// A key as the TexCoordTransform the vertex shaders apply: u' = dot((u, v, 1), rowU), v' = dot((u, v, 1), rowV), from
// c = (u, v) - 0.5 rotated by rotation.z, scaled, + 0.5 + translation. The exe's conversion is not found; this is fitted
// exactly to the .geo files' stored matrices (written from the first key; see CLAUDE.md "Material animation tracks").
void materialKeyTransform(const MaterialKey& key, Vec3& rowU, Vec3& rowV);

}  // namespace tr
