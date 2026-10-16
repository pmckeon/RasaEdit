#pragma once

// Lossless .geo model files: every byte of a shipped model is held, so a file read with readGeoFile and written with
// writeGeoFile is byte-identical (checked on all 7,238 shipped models), and a model built from scratch can be written.
// readGeo (trformats.h) stays the renderer's decoded view; this is the editing view.
//
// Layout (surveyed 2026-10-16 over every .geo in the archives; scratchpad geosurvey.py / geotail.py):
//   "CHNKBLXX", then chunks; every chunk = u32 tag, u32 payload size, u32 version, u32 0, payload.
//   GBOD v1-3 { [v3: f32, 50.0 in every file], [a shader name list if the next tag is not BBOX - none shipped], BBOX,
//     CPDG v1 { u32 n, n x CPDF }, u32 property groups, groups x { i32 id, u32 n, n x (cstr key, cstr value) },
//     PSKE v1 { u32 n, n x PBON v1 { i32 parent, i32 a, i32 b, f32 c (UNKNOWN; copied), BDAT v1/v2 { cstr name, quat xyzw,
//       vec3 translation, [v2: vec3 scale], [inverse bind 4x4], [bounding volume chunks: BVWS v2 / BVBX v1 / BVSP v1, each
//       starting with a BVOL v1 radius chunk] } } }, i32 morph targets (0 in all), i32 pieces, pieces (GPCE / GSKN),
//     i32 shadows (0 in all), shadow chunks, then 1 byte (0 in all v2 / v3 files; v1 has none) }.
//   BBOX v2 { u8 isEmpty, vec3 min, vec3 max, vec3 centre, f32 radius }.
//   GPCE v8-v12 { [v9+: u8 hasMaterial (0 in all)], EFCT v3 { asset id (ISTR) = effect, PARM v2 chunks { cstr key, i32 type,
//       value: 1 bool u32, 2 i32, 3 i32 n + n floats, 4 cstr, 5 / 8 asset id } }, INDX v1 { i32 count, u16 indices },
//     [v8-v9: i32 streams, then VERT (and a second one if streams = 2)] [v10+: VERT v2 { DECL v2 { u32 n, n x (u8 type,
//     method, usage, usage index) }, i32 count, count x stride bytes }], BBOX, cstr bone name,
//     [v8+: u32 0, u32 swap id, cstr group, cstr piece name, USDA v1 { u32 n, n x (cstr, cstr) }],
//     u32 triangle count (= indices / 3 in every v12 piece), u32 4 (D3DPT_TRIANGLELIST), [v11+: u32 bones per vertex
//     (3 skinned, 0 static)], [v12: u32 sorted-set flag, u32 sorted-set count, that many INDX chunks (8 direction-sorted
//     index lists on 51 translucent pieces)] [v8-v10: u32 0 instead] }.
//   GSKN v1 { GPCE, u32 n, n x cstr bone name (the skinning palette) }.

#include "trformats.h"

namespace tr {

struct GeoChunkHeader {
    uint32_t version = 0;
    uint32_t reserved = 0;
};

// An asset id chunk: ISTR (a string), IINT (a number), INON (none) or any other kind, kept as its payload.
struct GeoAssetId {
    uint32_t kind = fourcc("ISTR");
    GeoChunkHeader header{1, 0};
    std::string text;                  // ISTR
    int32_t number = 0;                // IINT
    Bytes raw;                         // other kinds: the payload as stored
};

// An effect parameter (PARM). type: 1 bool, 2 int, 3 floats, 4 string, 5 / 8 asset (texture file names).
struct GeoFileParameter {
    GeoChunkHeader header{2, 0};
    std::string key;
    int32_t type = 0;
    uint32_t boolValue = 0;
    int32_t intValue = 0;
    std::vector<float> floats;
    std::string text;
    GeoAssetId asset;
    Bytes raw;                         // unknown types, or bytes after the value
};

struct GeoFileBounds {
    GeoChunkHeader header{2, 0};
    uint8_t isEmpty = 0;
    Vec3 min, max, centre;
    float radius = 0;
    Bytes raw;                         // v1 / other versions: the payload as stored
};

struct GeoFileIndices {
    GeoChunkHeader header{1, 0};
    bool wide = false;                 // 32-bit indices (none shipped)
    std::vector<uint32_t> values;
};

struct GeoFileVertices {
    GeoChunkHeader header{2, 0};
    GeoChunkHeader declHeader{2, 0};
    std::vector<VertexElement> layout; // offsets are recomputed from the types on write
    uint32_t stride = 0;
    uint32_t count = 0;
    Bytes data;                        // count x stride bytes, as stored
};

struct GeoFilePiece {
    bool skinned = false;              // GSKN around the GPCE
    GeoChunkHeader gsknHeader{1, 0};
    GeoChunkHeader header{12, 0};      // GPCE
    uint8_t hasMaterial = 0;           // v9+
    GeoChunkHeader efctHeader{3, 0};
    GeoAssetId effect;
    std::vector<GeoFileParameter> parameters;
    GeoFileIndices indices;
    int32_t streams = 1;               // v8-v9
    GeoFileVertices vertices;
    Bytes secondStream;                // v8-v9 with streams = 2: the second VERT chunk as stored
    GeoFileBounds bounds;
    std::string boneName;
    // v8+
    uint32_t unknown0 = 0;
    uint32_t swapId = 0;
    std::string group;
    std::string name;
    bool hasUserData = true;
    GeoChunkHeader usdaHeader{1, 0};
    std::vector<GeoParameter> userData;
    // After the user data.
    uint32_t triangleCount = 0;        // written as indices / 3 when `recountTriangles`
    uint32_t primitiveType = 4;
    uint32_t bonesPerVertex = 0;       // v11+
    uint32_t sortedFlag = 0;           // v12
    std::vector<GeoFileIndices> sortedIndices;     // v12
    Bytes tail;                        // anything after the decoded fields, as stored
    std::vector<std::string> skinBones;            // GSKN palette
};

struct GeoFileBone {
    GeoChunkHeader pbonHeader{1, 0};
    int32_t parent = -1;
    int32_t unknownA = -1;
    int32_t unknownB = 0;
    float unknownC = 0;
    GeoChunkHeader bdatHeader{2, 0};
    std::string name;
    Quat rotation;
    Vec3 translation;
    Vec3 scale{1, 1, 1};               // BDAT v2
    bool hasInverseBind = true;
    float inverseBind[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // row-major, Direct3D row vectors
    Bytes boundingVolume;              // the chunks after the matrix (BVWS / BVBX / BVSP), as stored
};

struct GeoPropertyGroup {
    int32_t id = 0;
    std::vector<GeoParameter> pairs;
};

struct GeoFileContactPoint {
    GeoChunkHeader header{1, 0};
    GeoContactPoint point;
};

struct GeoFile {
    Bytes header;                      // "CHNKBLXX"
    Bytes before;                      // chunks before GBOD, as stored (none shipped)
    Bytes after;                       // bytes after GBOD, as stored (none shipped)
    GeoChunkHeader gbodHeader{3, 0};
    float gbodValue = 50.0f;           // v3 (UNKNOWN; 50 in every file)
    bool hasShaderList = false;
    std::vector<std::string> shaderList;
    GeoFileBounds bounds;
    GeoChunkHeader cpdgHeader{1, 0};
    std::vector<GeoFileContactPoint> contactPoints;
    std::vector<GeoPropertyGroup> properties;
    GeoChunkHeader pskeHeader{1, 0};
    std::vector<GeoFileBone> bones;
    int32_t morphTargets = 0;
    std::vector<GeoFilePiece> pieces;
    std::vector<Bytes> shadows;        // shadow chunks as stored (none shipped)
    Bytes tail;                        // after the shadows: 1 byte (0) in v2 / v3
};

// Bone collision volumes (the chunks after BDAT's inverse bind; GeoFileBone::boundingVolume holds them as stored). What the client
// walks on and collides with (Rasa.NET Rasa.ClientData/GeoMesh.cs, which builds the server's navmesh from them; consistent with the
// moved / rescaled Bootcamp fountain colliding where it was put, 2026-10-15). Surveyed 2026-10-16 over every .geo (scratchpad
// bvsurvey.py): 3,712 BVWS v2 (walkable surface: BVOL v1 { f32 radius }, i32 triangles, i32 vertices, vec3 x vertices, u16 x 3
// x triangles, then one 0 byte), 46 BVBX v1 (box: BVOL, vec3 centre, vec3 half extents), 1 BVSP v1 (sphere: BVOL, vec3 centre, f32
// radius). Coordinates are in the bone's bind frame (bind world matrix: the fit with the model's bounds is better than the bone's local
// transform alone; most collision bones are roots, where the two agree).
struct GeoBoneVolume {
    uint32_t kind = fourcc("BVWS");    // BVWS, BVBX, BVSP; other kinds keep `raw`
    GeoChunkHeader header{2, 0};
    GeoChunkHeader bvolHeader{1, 0};
    float radius = 0;                  // BVOL
    std::vector<Vec3> vertices;        // BVWS
    std::vector<uint16_t> indices;     // BVWS, 3 per triangle
    Vec3 centre;                       // BVBX / BVSP
    Vec3 halfExtents;                  // BVBX
    float sphereRadius = 0;            // BVSP
    Bytes tail;                        // after the decoded fields (BVWS: one 0 byte)
    Bytes raw;                         // unknown kinds: the payload
};
std::vector<GeoBoneVolume> readBoneVolumes(const Bytes& boundingVolume);
Bytes writeBoneVolumes(const std::vector<GeoBoneVolume>& volumes);

GeoFile readGeoFile(const Bytes& data);
// recountTriangles: write each piece's triangle count as indices / 3 (for edited pieces); off writes the stored value.
Bytes writeGeoFile(const GeoFile& file, bool recountTriangles = false);

// ------------------------------------------------------------------------------------------
// Lossless animation files (.anm). Surveyed 2026-10-16 over all 6,238 (scratchpad anmsurvey2.py):
//   "CHNKBLXX", ANIM v4 { u32 tracks, u8 0, tracks x (fourcc = the track's key chunk tag in every track, TRAK v3 {
//     u8 0, fourcc target, SKEL: cstr bone, u8 0, u32 flags (0-100, UNKNOWN - per bone, 95 on most human bones, 0 on
//     object bones; copied), u32 key count, key chunk (KEYT .. XSRT, v0); EFCT: cstr group, cstr parameter, u32 0, u32 key
//     count, KEUV v0; MRPH (71 tracks) and others: kept as stored }), u32 0 }.
// ------------------------------------------------------------------------------------------
struct AnimFileTrack {
    uint32_t preTag = 0;               // written before TRAK (= keyTag for bone and material tracks)
    GeoChunkHeader header{3, 0};
    uint8_t byte0 = 0;
    uint32_t target = fourcc("SKEL");
    std::string name;                  // SKEL: bone; EFCT: group
    std::string parameter;             // EFCT
    uint8_t byte1 = 0;                 // SKEL
    uint32_t flags = 0;                // SKEL
    uint32_t zero = 0;                 // EFCT
    uint32_t keyCount = 0;
    uint32_t keyTag = 0;
    GeoChunkHeader keyHeader{0, 0};
    Bytes keyData;                     // the key chunk's payload as stored
    Bytes raw;                         // other targets: everything after the target fourcc
};

struct AnimFile {
    Bytes header;                      // "CHNKBLXX"
    GeoChunkHeader animHeader{4, 0};
    uint8_t byte0 = 0;
    std::vector<AnimFileTrack> tracks;
    Bytes tail{0, 0, 0, 0};
    Bytes after;
};

AnimFile readAnimFile(const Bytes& data);
Bytes writeAnimFile(const AnimFile& file);
// A bone track's keys (any K* / X* chunk) as absolute local transforms; components the chunk lacks keep their defaults.
std::vector<AnimKey> decodeAnimKeys(const AnimFileTrack& track);
// Stores keys in a key chunk kind: KEYT KEYR KEYS KERT KEST KESR KSRT (floats) or XKER XKRT XKSR XSRT (half / int16).
void encodeAnimKeys(AnimFileTrack& track, uint32_t keyTag, const std::vector<AnimKey>& keys);
// Material track keys (KEUV).
std::vector<MaterialKey> decodeMaterialKeys(const AnimFileTrack& track);
void encodeMaterialKeys(AnimFileTrack& track, const std::vector<MaterialKey>& keys);

// Vertex attribute access for edited / built pieces (D3D9 vertex declarations).
uint32_t vertexElementSize(uint8_t type);
// Decodes one element into up to four floats (D3DCOLOR as 0..1 r, g, b, a).
void decodeVertexElement(uint8_t type, const uint8_t* p, float out[4]);
// Encodes four floats as the element type (the inverse of decodeVertexElement; D3DCOLOR from 0..1 r, g, b, a).
void encodeVertexElement(uint8_t type, const float in[4], uint8_t* p);
uint16_t floatToHalf(float value);
float halfToFloatValue(uint16_t half);

// The renderer's decoded model (readGeo's GeoModel) of a lossless file - the same as readGeo(writeGeoFile(file)).
GeoModel decodeGeoFile(const GeoFile& file);

}  // namespace tr
