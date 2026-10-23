// navmesh.cpp - see navmesh.h.
#include "navmesh.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace tr {
namespace {

constexpr int32_t kSetMagic = 'M' << 24 | 'S' << 16 | 'E' << 8 | 'T';
constexpr int32_t kSetVersionC = 1, kSetVersionRecast4j1 = 0x8801, kSetVersionRecast4j = 0x8802;
constexpr int32_t kTileMagic = 'D' << 24 | 'N' << 16 | 'A' << 8 | 'V';
constexpr int32_t kTileVersionC = 7, kTileVersionFirst = 0x8807, kTileVersionNoFirstLink = 0x8808, kTileVersion32BitBvTree = 0x8809;

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    template <typename T> T get() {
        if (pos_ + sizeof(T) > bytes_.size()) throw FormatError("navmesh: unexpected end of file at byte " + std::to_string(pos_));
        T v;
        std::memcpy(&v, bytes_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    int32_t i32() { return get<int32_t>(); }
    float f32() { return get<float>(); }
    Vec3 vec3() {
        Vec3 v;
        v.x = f32(); v.y = f32(); v.z = f32();
        return v;
    }
    void skip(size_t n) {
        if (pos_ + n > bytes_.size()) throw FormatError("navmesh: unexpected end of file at byte " + std::to_string(pos_));
        pos_ += n;
    }
    size_t position() const { return pos_; }
private:
    const std::vector<uint8_t>& bytes_;
    size_t pos_ = 0;
};

uint32_t count(int32_t value, const char* what) {
    if (value < 0 || value > 50000000) throw FormatError(std::string("navmesh: bad ") + what + " count " + std::to_string(value));
    return uint32_t(value);
}

NavTile readTile(Reader& in, int32_t maxVertsPerPoly) {
    NavTile t;
    if (in.i32() != kTileMagic) throw FormatError("navmesh: tile without the DNAV magic (big-endian files are not read)");
    const int32_t version = in.i32();
    if (version != kTileVersionC && (version < kTileVersionFirst || version > kTileVersion32BitBvTree))
        throw FormatError("navmesh: unknown tile version " + std::to_string(version));
    const bool cFormat = version == kTileVersionC;
    t.x = in.i32();
    t.y = in.i32();
    t.layer = in.i32();
    in.i32();                                            // userId
    const uint32_t polyCount = count(in.i32(), "polygon");
    const uint32_t vertCount = count(in.i32(), "vertex");
    const uint32_t maxLinkCount = count(in.i32(), "link");
    const uint32_t detailMeshCount = count(in.i32(), "detail mesh");
    const uint32_t detailVertCount = count(in.i32(), "detail vertex");
    const uint32_t detailTriCount = count(in.i32(), "detail triangle");
    const uint32_t bvNodeCount = count(in.i32(), "bv node");
    const uint32_t offMeshConCount = count(in.i32(), "off-mesh connection");
    in.i32();                                            // offMeshBase
    t.walkableHeight = in.f32();
    t.walkableRadius = in.f32();
    t.walkableClimb = in.f32();
    t.bounds.min = in.vec3();
    t.bounds.max = in.vec3();
    in.f32();                                            // bvQuantFactor
    t.verts.resize(vertCount);
    for (Vec3& v : t.verts) v = in.vec3();
    t.polys.resize(polyCount);
    for (NavPoly& p : t.polys) {
        if (version < kTileVersionNoFirstLink) in.i32();     // firstLink
        for (int32_t j = 0; j < maxVertsPerPoly; ++j) {
            const uint16_t v = in.get<uint16_t>();
            if (j < 6) p.verts[j] = v;
        }
        for (int32_t j = 0; j < maxVertsPerPoly; ++j) {
            const uint16_t n = in.get<uint16_t>();
            if (j < 6) p.neighbours[j] = n;
        }
        p.flags = in.get<uint16_t>();
        p.vertCount = in.get<uint8_t>();
        p.areaAndType = in.get<uint8_t>();
        if (p.vertCount > 6 || p.vertCount > maxVertsPerPoly) throw FormatError("navmesh: polygon with " + std::to_string(p.vertCount) + " vertices");
        for (int j = 0; j < p.vertCount; ++j)
            if (p.verts[j] >= vertCount) throw FormatError("navmesh: polygon vertex out of range");
    }
    if (cFormat) in.skip(size_t(maxLinkCount) * 16);
    t.detailMeshes.resize(detailMeshCount);
    for (NavDetailMesh& d : t.detailMeshes) {
        d.vertBase = uint32_t(in.i32());
        d.triBase = uint32_t(in.i32());
        d.vertCount = in.get<uint8_t>();
        d.triCount = in.get<uint8_t>();
        if (cFormat) in.get<uint16_t>();
    }
    t.detailVerts.resize(detailVertCount);
    for (Vec3& v : t.detailVerts) v = in.vec3();
    t.detailTris.resize(size_t(detailTriCount) * 4);
    for (uint8_t& b : t.detailTris) b = in.get<uint8_t>();
    in.skip(size_t(bvNodeCount) * (version < kTileVersion32BitBvTree ? 6 * 2 + 4 : 6 * 4 + 4));
    t.offMeshConnections.resize(offMeshConCount);
    for (NavOffMeshConnection& c : t.offMeshConnections) {
        c.start = in.vec3();
        c.end = in.vec3();
        c.radius = in.f32();
        c.poly = in.get<uint16_t>();
        c.flags = in.get<uint8_t>();
        c.side = in.get<uint8_t>();
        c.userId = uint32_t(in.i32());
    }
    return t;
}

}  // namespace

size_t NavMesh::polyCount() const {
    size_t n = 0;
    for (const NavTile& t : tiles) n += t.polys.size();
    return n;
}

NavMesh readNavMesh(const std::vector<uint8_t>& bytes) {
    Reader in(bytes);
    NavMesh mesh;
    if (in.i32() != kSetMagic) throw FormatError("navmesh: not an MSET navmesh set (big-endian files are not read)");
    const int32_t version = in.i32();
    if (version != kSetVersionC && version != kSetVersionRecast4j1 && version != kSetVersionRecast4j)
        throw FormatError("navmesh: unknown set version " + std::to_string(version));
    const uint32_t tileCount = count(in.i32(), "tile");
    mesh.origin = in.vec3();
    mesh.tileWidth = in.f32();
    mesh.tileHeight = in.f32();
    mesh.maxTiles = in.i32();
    mesh.maxPolys = in.i32();
    // The C format and recast4j's first version leave the polygon size implicit: Detour's DT_VERTS_PER_POLYGON, 6.
    mesh.maxVertsPerPoly = version == kSetVersionRecast4j ? in.i32() : 6;
    if (mesh.maxVertsPerPoly < 3 || mesh.maxVertsPerPoly > 6)
        throw FormatError("navmesh: " + std::to_string(mesh.maxVertsPerPoly) + " vertices per polygon (the editor reads up to 6)");
    for (uint32_t i = 0; i < tileCount; ++i) {
        const uint64_t ref = in.get<uint64_t>();
        const int32_t dataSize = in.i32();
        if (ref == 0 || dataSize == 0) break;
        if (version == kSetVersionC) in.i32();           // C struct padding
        // dataSize is the C struct size; DotRecast's reader ignores it and parses the tile, as here.
        NavTile tile = readTile(in, mesh.maxVertsPerPoly);
        tile.ref = ref;
        mesh.tiles.push_back(std::move(tile));
    }
    mesh.trailingBytes = bytes.size() - in.position();
    return mesh;
}

NavMesh readNavMeshFile(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) throw FormatError("cannot open " + path);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return readNavMesh(bytes);
}

void navPolyTriangles(const NavTile& tile, size_t polyIndex, std::vector<Vec3>& out) {
    const NavPoly& p = tile.polys[polyIndex];
    if (p.type() != 0) return;                           // off-mesh connections have no surface
    auto corner = [&](const NavDetailMesh* d, uint8_t index) -> const Vec3* {
        if (index < p.vertCount) return &tile.verts[p.verts[index]];
        if (!d) return nullptr;
        const size_t k = size_t(d->vertBase) + index - p.vertCount;
        return k < tile.detailVerts.size() ? &tile.detailVerts[k] : nullptr;
    };
    if (polyIndex < tile.detailMeshes.size()) {
        const NavDetailMesh& d = tile.detailMeshes[polyIndex];
        for (uint32_t t = 0; t < d.triCount; ++t) {
            const size_t base = (size_t(d.triBase) + t) * 4;
            if (base + 3 > tile.detailTris.size()) break;
            const Vec3 *a = corner(&d, tile.detailTris[base]), *b = corner(&d, tile.detailTris[base + 1]), *c = corner(&d, tile.detailTris[base + 2]);
            if (!a || !b || !c) continue;
            out.push_back(*a);
            out.push_back(*b);
            out.push_back(*c);
        }
        return;
    }
    for (int j = 2; j < p.vertCount; ++j) {              // no detail mesh: the polygon as a fan
        out.push_back(tile.verts[p.verts[0]]);
        out.push_back(tile.verts[p.verts[j - 1]]);
        out.push_back(tile.verts[p.verts[j]]);
    }
}

}  // namespace tr
