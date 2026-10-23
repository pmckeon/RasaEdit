// navmesh.h - the server's navmesh (Rasa.NET: <repository>/navmesh/<map name>.nav, written by Rasa.NavMesh and read by the
// game server's NavMeshManager for creature pathing; Rasa.Navigation/NavMeshFile.cs).
//
// A Detour tiled navmesh set in DotRecast's MSET format (DotRecast.Detour.Io 2026.3.1: DtMeshSetReader / DtMeshDataReader /
// DtNavMeshParamsReader / NavMeshSetHeader / DtDetour constants), little-endian:
//   set:  i32 magic 'MSET', i32 version (1 = the C format, 0x8801 / 0x8802 = recast4j), i32 tileCount,
//         params { f32 origin[3], f32 tileWidth, f32 tileHeight, i32 maxTiles, i32 maxPolys },
//         [0x8802: i32 maxVertsPerPoly], then per tile { i64 tileRef, i32 dataSize, [version 1: i32 padding], tile data };
//         a tile with ref 0 or size 0 ends the list.
//   tile: i32 magic 'DNAV', i32 version (7 = the C format, 0x8807 .. 0x8809 = recast4j), header i32 x, y, layer, userId,
//         polyCount, vertCount, maxLinkCount, detailMeshCount, detailVertCount, detailTriCount, bvNodeCount, offMeshConCount,
//         offMeshBase, f32 walkableHeight, walkableRadius, walkableClimb, bmin[3], bmax[3], bvQuantFactor;
//         f32 verts[vertCount][3];
//         polys { [version < 0x8808: i32 firstLink], u16 verts[maxVertsPerPoly], u16 neis[maxVertsPerPoly], u16 flags,
//                 u8 vertCount, u8 areaAndType };
//         [version 7: maxLinkCount x 16 bytes of links];
//         detail meshes { i32 vertBase, i32 triBase, u8 vertCount, u8 triCount, [version 7: u16 padding] };
//         f32 detailVerts[detailVertCount][3]; u8 detailTris[detailTriCount][4];
//         bv nodes { (version < 0x8809: u16 / else i32) bmin[3], bmax[3], i32 i };
//         off-mesh connections { f32 pos[2][3], f32 radius, u16 poly, u8 flags, u8 side, i32 userId }.
// A polygon's area is areaAndType & 0x3f, its type areaAndType >> 6 (0 ground polygon, 1 off-mesh connection). A neighbour
// value 0 is an outer edge; with bit 0x8000 (DT_EXT_LINK) it leads into another tile. A detail triangle's vertex index below the
// polygon's vertCount is that polygon vertex, otherwise detailVerts[vertBase + index - vertCount].
// Rasa.NavMesh's values (Rasa.Navigation/NavMeshFlags.cs, Rasa.NavMesh/NavMeshBuilder.cs): flags 0x01 walkable (every polygon),
// 0x02 underground (under the terrain heightmap by more than 2 m); area 0 ground, 1 underground. Positions are game world
// coordinates (Rasa.Navigation passes x, y, z through unchanged).
#pragma once

#include "trformats.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tr {

struct NavPoly {
    uint16_t verts[6] = {};
    uint16_t neighbours[6] = {};
    uint16_t flags = 0;
    uint8_t vertCount = 0;
    uint8_t areaAndType = 0;
    uint8_t area() const { return areaAndType & 0x3f; }
    uint8_t type() const { return areaAndType >> 6; }
};

struct NavDetailMesh {
    uint32_t vertBase = 0, triBase = 0;
    uint8_t vertCount = 0, triCount = 0;
};

struct NavOffMeshConnection {
    Vec3 start, end;
    float radius = 0;
    uint16_t poly = 0;
    uint8_t flags = 0, side = 0;
    uint32_t userId = 0;
};

struct NavTile {
    uint64_t ref = 0;
    int32_t x = 0, y = 0, layer = 0;
    float walkableHeight = 0, walkableRadius = 0, walkableClimb = 0;
    Bounds bounds;
    std::vector<Vec3> verts;
    std::vector<NavPoly> polys;
    std::vector<NavDetailMesh> detailMeshes;
    std::vector<Vec3> detailVerts;
    std::vector<uint8_t> detailTris;           // 4 per triangle: three vertex indices and edge flags
    std::vector<NavOffMeshConnection> offMeshConnections;
};

struct NavMesh {
    Vec3 origin;
    float tileWidth = 0, tileHeight = 0;
    int32_t maxTiles = 0, maxPolys = 0, maxVertsPerPoly = 0;
    std::vector<NavTile> tiles;
    size_t trailingBytes = 0;                  // bytes after the last tile (0 for a file read exactly)
    size_t polyCount() const;
};

// Reads a .nav file; throws std::runtime_error on anything it cannot read.
NavMesh readNavMesh(const std::vector<uint8_t>& bytes);
NavMesh readNavMeshFile(const std::string& path);

// The triangles of a polygon's detail mesh, as world positions (three per triangle).
void navPolyTriangles(const NavTile& tile, size_t polyIndex, std::vector<Vec3>& out);

}  // namespace tr
