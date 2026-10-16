#include "geofile.h"

#include <cmath>
#include <cstring>

namespace tr {

namespace {

struct Header {
    uint32_t tag = 0;
    GeoChunkHeader header;
    size_t start = 0;
    size_t end = 0;
};

Header readHeader(ByteReader& r) {
    Header h;
    h.start = r.tell();
    h.tag = r.u32();
    const uint32_t size = r.u32();
    h.header.version = r.u32();
    h.header.reserved = r.u32();
    h.end = r.tell() + size;
    if (h.end > r.end()) throw FormatError("chunk " + tagName(h.tag) + " runs past the end of its data");
    return h;
}

Header expectHeader(ByteReader& r, uint32_t tag) {
    const Header h = readHeader(r);
    if (h.tag != tag) throw FormatError("expected " + tagName(tag) + " but found " + tagName(h.tag) + " at offset " + std::to_string(h.start));
    return h;
}

void expectEnd(const ByteReader& r, const Header& h) {
    if (r.tell() != h.end)
        throw FormatError(tagName(h.tag) + ": " + std::to_string(int64_t(h.end) - int64_t(r.tell())) + " bytes not decoded at offset " +
                          std::to_string(r.tell()));
}

Bytes rest(ByteReader& r, size_t end) { return r.bytes(end - r.tell()); }

Quat readQuat(ByteReader& r) {
    Quat q;
    q.x = r.f32();
    q.y = r.f32();
    q.z = r.f32();
    q.w = r.f32();
    return q;
}

GeoAssetId readAsset(ByteReader& r) {
    const Header h = readHeader(r);
    GeoAssetId a;
    a.kind = h.tag;
    a.header = h.header;
    if (h.tag == fourcc("ISTR")) a.text = r.cstr();
    else if (h.tag == fourcc("IINT")) a.number = r.i32();
    a.raw = rest(r, h.end);
    return a;
}

GeoFileBounds readBounds(ByteReader& r) {
    const Header h = expectHeader(r, fourcc("BBOX"));
    GeoFileBounds b;
    b.header = h.header;
    if (h.header.version == 2 && h.end - r.tell() == 41) {
        b.isEmpty = r.u8();
        b.min = r.vec3();
        b.max = r.vec3();
        b.centre = r.vec3();
        b.radius = r.f32();
    } else {
        b.raw = rest(r, h.end);
    }
    return b;
}

GeoFileIndices readIndices(ByteReader& r) {
    const Header h = expectHeader(r, fourcc("INDX"));
    if (h.header.version != 1) throw FormatError("INDX v" + std::to_string(h.header.version) + " is not supported");
    GeoFileIndices x;
    x.header = h.header;
    const int32_t count = r.i32();
    const size_t available = h.end - r.tell();
    if (count < 0) throw FormatError("INDX: negative count");
    x.wide = count > 0 && size_t(count) * 4 == available;
    if (size_t(count) * (x.wide ? 4 : 2) != available) throw FormatError("INDX: size does not match the count");
    x.values.resize(size_t(count));
    for (int32_t k = 0; k < count; ++k) x.values[size_t(k)] = x.wide ? r.u32() : r.u16();
    expectEnd(r, h);
    return x;
}

GeoFileVertices readVertices(ByteReader& r) {
    const Header h = expectHeader(r, fourcc("VERT"));
    if (h.header.version != 2) throw FormatError("VERT v" + std::to_string(h.header.version) + " is not supported");
    GeoFileVertices v;
    v.header = h.header;
    const Header d = expectHeader(r, fourcc("DECL"));
    if (d.header.version != 2) throw FormatError("DECL v" + std::to_string(d.header.version) + " is not supported");
    v.declHeader = d.header;
    const uint32_t n = r.u32();
    uint32_t offset = 0;
    for (uint32_t k = 0; k < n; ++k) {
        VertexElement e;
        e.type = r.u8();
        e.method = r.u8();
        e.usage = r.u8();
        e.usageIndex = r.u8();
        e.offset = offset;
        offset += vertexElementSize(e.type);
        v.layout.push_back(e);
    }
    expectEnd(r, d);
    const int32_t count = r.i32();
    if (count < 0) throw FormatError("VERT: negative count");
    v.count = uint32_t(count);
    v.data = rest(r, h.end);
    v.stride = offset;
    if (size_t(v.count) * v.stride != v.data.size()) throw FormatError("VERT: data size does not match the declaration");
    return v;
}

GeoFileParameter readParameter(ByteReader& r) {
    const Header h = expectHeader(r, fourcc("PARM"));
    GeoFileParameter p;
    p.header = h.header;
    p.key = r.cstr();
    p.type = r.i32();
    if (h.header.version == 2) {
        switch (p.type) {
            case 1: p.boolValue = r.u32(); break;
            case 2: p.intValue = r.i32(); break;
            case 3: {
                const int32_t n = r.i32();
                if (n < 0) throw FormatError("PARM: negative float count");
                for (int32_t k = 0; k < n; ++k) p.floats.push_back(r.f32());
                break;
            }
            case 4: p.text = r.cstr(); break;
            case 5: case 8: p.asset = readAsset(r); break;
            default: break;
        }
    }
    p.raw = rest(r, h.end);
    return p;
}

GeoFilePiece readPiece(ByteReader& r, uint32_t tag) {
    GeoFilePiece piece;
    Header skin;
    if (tag == fourcc("GSKN")) {
        skin = expectHeader(r, fourcc("GSKN"));
        piece.skinned = true;
        piece.gsknHeader = skin.header;
    }
    const Header h = expectHeader(r, fourcc("GPCE"));
    const uint32_t v = h.header.version;
    if (v < 8 || v > 12) throw FormatError("GPCE v" + std::to_string(v) + " is not supported");
    piece.header = h.header;
    if (v >= 9) piece.hasMaterial = r.u8();
    {
        const Header e = expectHeader(r, fourcc("EFCT"));
        if (e.header.version < 3) throw FormatError("EFCT v" + std::to_string(e.header.version) + " is not supported");
        piece.efctHeader = e.header;
        piece.effect = readAsset(r);
        while (r.tell() + 16 <= e.end && r.peekU32() == fourcc("PARM")) piece.parameters.push_back(readParameter(r));
        expectEnd(r, e);
    }
    piece.indices = readIndices(r);
    if (v >= 10) {
        piece.vertices = readVertices(r);
    } else {
        piece.streams = r.i32();
        piece.vertices = readVertices(r);
        if (piece.streams == 2) {
            const size_t start = r.tell();
            const Header s = readHeader(r);
            r.seek(start);
            piece.secondStream = r.bytes(s.end - start);
        }
    }
    piece.bounds = readBounds(r);
    piece.boneName = r.cstr();
    if (r.tell() + 8 <= h.end) {
        piece.unknown0 = r.u32();
        piece.swapId = r.u32();
        piece.group = r.cstr();
        piece.name = r.cstr();
        piece.hasUserData = r.tell() + 16 <= h.end && r.peekU32() == fourcc("USDA");
        if (piece.hasUserData) {
            const Header u = expectHeader(r, fourcc("USDA"));
            piece.usdaHeader = u.header;
            const uint32_t n = r.u32();
            for (uint32_t k = 0; k < n; ++k) {
                GeoParameter p;
                p.key = r.cstr();
                p.value = r.cstr();
                piece.userData.push_back(std::move(p));
            }
            expectEnd(r, u);
        }
    }
    if (r.tell() + 8 <= h.end) {
        piece.triangleCount = r.u32();
        piece.primitiveType = r.u32();
        if (v >= 11 && r.tell() + 4 <= h.end) piece.bonesPerVertex = r.u32();
        if (r.tell() + 4 <= h.end) piece.sortedFlag = r.u32();
        if (v >= 12 && r.tell() + 4 <= h.end) {
            const uint32_t n = r.u32();
            for (uint32_t k = 0; k < n; ++k) piece.sortedIndices.push_back(readIndices(r));
        }
    }
    piece.tail = rest(r, h.end);
    if (piece.skinned) {
        const uint32_t n = r.u32();
        for (uint32_t k = 0; k < n; ++k) piece.skinBones.push_back(r.cstr());
        expectEnd(r, skin);
    }
    return piece;
}

GeoFileBone readBone(ByteReader& r) {
    const Header h = expectHeader(r, fourcc("PBON"));
    GeoFileBone b;
    b.pbonHeader = h.header;
    b.parent = r.i32();
    b.unknownA = r.i32();
    b.unknownB = r.i32();
    b.unknownC = r.f32();
    const Header d = expectHeader(r, fourcc("BDAT"));
    b.bdatHeader = d.header;
    b.name = r.cstr();
    b.rotation = readQuat(r);
    b.translation = r.vec3();
    if (d.header.version == 2) b.scale = r.vec3();
    b.hasInverseBind = r.tell() + 64 <= d.end;
    if (b.hasInverseBind)
        for (float& f : b.inverseBind) f = r.f32();
    b.boundingVolume = rest(r, d.end);
    expectEnd(r, h);
    return b;
}

GeoFile readGbod(ByteReader& r, GeoFile file) {
    const Header g = expectHeader(r, fourcc("GBOD"));
    file.gbodHeader = g.header;
    if (g.header.version == 3) file.gbodValue = r.f32();
    if (r.peekU32() != fourcc("BBOX")) {
        file.hasShaderList = true;
        const uint32_t n = r.u32();
        for (uint32_t k = 0; k < n; ++k) file.shaderList.push_back(r.cstr());
    }
    file.bounds = readBounds(r);
    {
        const Header c = expectHeader(r, fourcc("CPDG"));
        file.cpdgHeader = c.header;
        const uint32_t n = r.u32();
        for (uint32_t k = 0; k < n; ++k) {
            const Header p = expectHeader(r, fourcc("CPDF"));
            GeoFileContactPoint cp;
            cp.header = p.header;
            cp.point.id = r.u32();
            cp.point.bone = r.i32();
            cp.point.rotation = readQuat(r);
            cp.point.position = r.vec3();
            const uint32_t pairs = r.u32();
            for (uint32_t j = 0; j < pairs; ++j) {
                GeoParameter value;
                value.key = r.cstr();
                value.value = r.cstr();
                cp.point.values.push_back(std::move(value));
            }
            expectEnd(r, p);
            file.contactPoints.push_back(std::move(cp));
        }
        expectEnd(r, c);
    }
    const uint32_t groups = r.u32();
    for (uint32_t k = 0; k < groups; ++k) {
        GeoPropertyGroup group;
        group.id = r.i32();
        const uint32_t n = r.u32();
        for (uint32_t j = 0; j < n; ++j) {
            GeoParameter p;
            p.key = r.cstr();
            p.value = r.cstr();
            group.pairs.push_back(std::move(p));
        }
        file.properties.push_back(std::move(group));
    }
    {
        const Header s = expectHeader(r, fourcc("PSKE"));
        file.pskeHeader = s.header;
        const uint32_t n = r.u32();
        for (uint32_t k = 0; k < n; ++k) file.bones.push_back(readBone(r));
        expectEnd(r, s);
    }
    file.morphTargets = r.i32();
    if (file.morphTargets != 0) throw FormatError("GBOD: morph targets are not supported");
    const int32_t pieces = r.i32();
    for (int32_t k = 0; k < pieces; ++k) {
        const uint32_t tag = r.peekU32();
        if (tag != fourcc("GPCE") && tag != fourcc("GSKN")) throw FormatError("GBOD: unknown piece chunk " + tagName(tag));
        file.pieces.push_back(readPiece(r, tag));
    }
    const int32_t shadows = r.i32();
    for (int32_t k = 0; k < shadows; ++k) {
        const size_t start = r.tell();
        const Header s = readHeader(r);
        r.seek(start);
        file.shadows.push_back(r.bytes(s.end - start));
    }
    file.tail = rest(r, g.end);
    return file;
}

// ---- writing ----

class Writer {
public:
    Bytes out;
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { for (int b = 0; b < 4; ++b) out.push_back(uint8_t(v >> (8 * b))); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void f32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
    void vec3(const Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
    void quat(const Quat& q) { f32(q.x); f32(q.y); f32(q.z); f32(q.w); }
    void cstr(const std::string& s) { out.insert(out.end(), s.begin(), s.end()); out.push_back(0); }
    void bytes(const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); }
    size_t begin(uint32_t tag, const GeoChunkHeader& h) {
        u32(tag);
        const size_t at = out.size();
        u32(0);
        u32(h.version);
        u32(h.reserved);
        return at;
    }
    void end(size_t at) {
        const uint32_t size = uint32_t(out.size() - (at + 12));
        for (int b = 0; b < 4; ++b) out[at + size_t(b)] = uint8_t(size >> (8 * b));
    }
};

void writeAsset(Writer& w, const GeoAssetId& a) {
    const size_t at = w.begin(a.kind, a.header);
    if (a.kind == fourcc("ISTR")) w.cstr(a.text);
    else if (a.kind == fourcc("IINT")) w.i32(a.number);
    w.bytes(a.raw);
    w.end(at);
}

void writeBounds(Writer& w, const GeoFileBounds& b) {
    const size_t at = w.begin(fourcc("BBOX"), b.header);
    if (b.raw.empty()) {
        w.u8(b.isEmpty);
        w.vec3(b.min);
        w.vec3(b.max);
        w.vec3(b.centre);
        w.f32(b.radius);
    } else {
        w.bytes(b.raw);
    }
    w.end(at);
}

void writeIndices(Writer& w, const GeoFileIndices& x) {
    const size_t at = w.begin(fourcc("INDX"), x.header);
    w.i32(int32_t(x.values.size()));
    for (uint32_t v : x.values) {
        if (x.wide) w.u32(v);
        else w.u16(uint16_t(v));
    }
    w.end(at);
}

void writeVertices(Writer& w, const GeoFileVertices& v) {
    const size_t at = w.begin(fourcc("VERT"), v.header);
    const size_t d = w.begin(fourcc("DECL"), v.declHeader);
    w.u32(uint32_t(v.layout.size()));
    for (const VertexElement& e : v.layout) {
        w.u8(e.type);
        w.u8(e.method);
        w.u8(e.usage);
        w.u8(e.usageIndex);
    }
    w.end(d);
    w.i32(int32_t(v.count));
    w.bytes(v.data);
    w.end(at);
}

void writeParameter(Writer& w, const GeoFileParameter& p) {
    const size_t at = w.begin(fourcc("PARM"), p.header);
    w.cstr(p.key);
    w.i32(p.type);
    if (p.header.version == 2) {
        switch (p.type) {
            case 1: w.u32(p.boolValue); break;
            case 2: w.i32(p.intValue); break;
            case 3:
                w.i32(int32_t(p.floats.size()));
                for (float f : p.floats) w.f32(f);
                break;
            case 4: w.cstr(p.text); break;
            case 5: case 8: writeAsset(w, p.asset); break;
            default: break;
        }
    }
    w.bytes(p.raw);
    w.end(at);
}

void writePiece(Writer& w, const GeoFilePiece& piece, bool recount) {
    size_t skin = 0;
    if (piece.skinned) skin = w.begin(fourcc("GSKN"), piece.gsknHeader);
    const size_t at = w.begin(fourcc("GPCE"), piece.header);
    const uint32_t v = piece.header.version;
    if (v >= 9) w.u8(piece.hasMaterial);
    {
        const size_t e = w.begin(fourcc("EFCT"), piece.efctHeader);
        writeAsset(w, piece.effect);
        for (const GeoFileParameter& p : piece.parameters) writeParameter(w, p);
        w.end(e);
    }
    writeIndices(w, piece.indices);
    if (v >= 10) {
        writeVertices(w, piece.vertices);
    } else {
        w.i32(piece.streams);
        writeVertices(w, piece.vertices);
        if (piece.streams == 2) w.bytes(piece.secondStream);
    }
    writeBounds(w, piece.bounds);
    w.cstr(piece.boneName);
    if (v >= 8) {
        w.u32(piece.unknown0);
        w.u32(piece.swapId);
        w.cstr(piece.group);
        w.cstr(piece.name);
        if (piece.hasUserData) {
            const size_t u = w.begin(fourcc("USDA"), piece.usdaHeader);
            w.u32(uint32_t(piece.userData.size()));
            for (const GeoParameter& p : piece.userData) {
                w.cstr(p.key);
                w.cstr(p.value);
            }
            w.end(u);
        }
        w.u32(recount ? uint32_t(piece.indices.values.size() / 3) : piece.triangleCount);
        w.u32(piece.primitiveType);
        if (v >= 11) w.u32(piece.bonesPerVertex);
        w.u32(piece.sortedFlag);
        if (v >= 12) {
            w.u32(uint32_t(piece.sortedIndices.size()));
            for (const GeoFileIndices& x : piece.sortedIndices) writeIndices(w, x);
        }
    }
    w.bytes(piece.tail);
    w.end(at);
    if (piece.skinned) {
        w.u32(uint32_t(piece.skinBones.size()));
        for (const std::string& b : piece.skinBones) w.cstr(b);
        w.end(skin);
    }
}

void writeBone(Writer& w, const GeoFileBone& b) {
    const size_t at = w.begin(fourcc("PBON"), b.pbonHeader);
    w.i32(b.parent);
    w.i32(b.unknownA);
    w.i32(b.unknownB);
    w.f32(b.unknownC);
    const size_t d = w.begin(fourcc("BDAT"), b.bdatHeader);
    w.cstr(b.name);
    w.quat(b.rotation);
    w.vec3(b.translation);
    if (b.bdatHeader.version == 2) w.vec3(b.scale);
    if (b.hasInverseBind)
        for (float f : b.inverseBind) w.f32(f);
    w.bytes(b.boundingVolume);
    w.end(d);
    w.end(at);
}

}  // namespace

GeoFile readGeoFile(const Bytes& data) {
    if (data.size() < 8 || std::memcmp(data.data(), "CHNK", 4) != 0) throw FormatError("not a chunk file (no CHNK header)");
    if (data[4] != 'B') throw FormatError("text chunk files are not supported");
    GeoFile file;
    file.header.assign(data.begin(), data.begin() + 8);
    ByteReader r(data, 8);
    const size_t start = r.tell();
    while (r.remaining() >= 16 && r.peekU32() != fourcc("GBOD")) {
        const Header h = readHeader(r);
        r.seek(h.end);
    }
    if (r.remaining() < 16) throw FormatError("no GBOD chunk found");
    file.before.assign(data.begin() + std::ptrdiff_t(start), data.begin() + std::ptrdiff_t(r.tell()));
    file = readGbod(r, std::move(file));
    file.after = rest(r, data.size());
    return file;
}

Bytes writeGeoFile(const GeoFile& file, bool recountTriangles) {
    Writer w;
    if (file.header.empty()) {
        const char* h = "CHNKBLXX";
        w.out.assign(h, h + 8);
    } else {
        w.bytes(file.header);
    }
    w.bytes(file.before);
    const size_t g = w.begin(fourcc("GBOD"), file.gbodHeader);
    if (file.gbodHeader.version == 3) w.f32(file.gbodValue);
    if (file.hasShaderList) {
        w.u32(uint32_t(file.shaderList.size()));
        for (const std::string& s : file.shaderList) w.cstr(s);
    }
    writeBounds(w, file.bounds);
    {
        const size_t c = w.begin(fourcc("CPDG"), file.cpdgHeader);
        w.u32(uint32_t(file.contactPoints.size()));
        for (const GeoFileContactPoint& cp : file.contactPoints) {
            const size_t p = w.begin(fourcc("CPDF"), cp.header);
            w.u32(cp.point.id);
            w.i32(cp.point.bone);
            w.quat(cp.point.rotation);
            w.vec3(cp.point.position);
            w.u32(uint32_t(cp.point.values.size()));
            for (const GeoParameter& v : cp.point.values) {
                w.cstr(v.key);
                w.cstr(v.value);
            }
            w.end(p);
        }
        w.end(c);
    }
    w.u32(uint32_t(file.properties.size()));
    for (const GeoPropertyGroup& group : file.properties) {
        w.i32(group.id);
        w.u32(uint32_t(group.pairs.size()));
        for (const GeoParameter& p : group.pairs) {
            w.cstr(p.key);
            w.cstr(p.value);
        }
    }
    {
        const size_t s = w.begin(fourcc("PSKE"), file.pskeHeader);
        w.u32(uint32_t(file.bones.size()));
        for (const GeoFileBone& b : file.bones) writeBone(w, b);
        w.end(s);
    }
    w.i32(file.morphTargets);
    w.i32(int32_t(file.pieces.size()));
    for (const GeoFilePiece& piece : file.pieces) writePiece(w, piece, recountTriangles);
    w.i32(int32_t(file.shadows.size()));
    for (const Bytes& s : file.shadows) w.bytes(s);
    w.bytes(file.tail);
    w.end(g);
    w.bytes(file.after);
    return std::move(w.out);
}

GeoModel decodeGeoFile(const GeoFile& file) { return readGeo(writeGeoFile(file)); }

std::vector<GeoBoneVolume> readBoneVolumes(const Bytes& data) {
    std::vector<GeoBoneVolume> out;
    ByteReader r(data);
    while (r.remaining() >= 16) {
        const Header h = readHeader(r);
        GeoBoneVolume v;
        v.kind = h.tag;
        v.header = h.header;
        const bool known = h.tag == fourcc("BVWS") || h.tag == fourcc("BVBX") || h.tag == fourcc("BVSP");
        if (known) {
            const Header b = expectHeader(r, fourcc("BVOL"));
            v.bvolHeader = b.header;
            v.radius = r.f32();
            expectEnd(r, b);
            if (h.tag == fourcc("BVWS")) {
                const int32_t triangles = r.i32(), vertices = r.i32();
                if (triangles < 0 || vertices < 0) throw FormatError("BVWS: negative counts");
                for (int32_t k = 0; k < vertices; ++k) v.vertices.push_back(r.vec3());
                for (int32_t k = 0; k < triangles * 3; ++k) v.indices.push_back(r.u16());
            } else if (h.tag == fourcc("BVBX")) {
                v.centre = r.vec3();
                v.halfExtents = r.vec3();
            } else {
                v.centre = r.vec3();
                v.sphereRadius = r.f32();
            }
            v.tail = rest(r, h.end);
        } else {
            v.raw = rest(r, h.end);
        }
        out.push_back(std::move(v));
    }
    if (r.remaining() != 0) throw FormatError("bone volumes: " + std::to_string(r.remaining()) + " bytes after the last chunk");
    return out;
}

Bytes writeBoneVolumes(const std::vector<GeoBoneVolume>& volumes) {
    Writer w;
    for (const GeoBoneVolume& v : volumes) {
        const size_t at = w.begin(v.kind, v.header);
        if (v.kind == fourcc("BVWS") || v.kind == fourcc("BVBX") || v.kind == fourcc("BVSP")) {
            const size_t b = w.begin(fourcc("BVOL"), v.bvolHeader);
            w.f32(v.radius);
            w.end(b);
            if (v.kind == fourcc("BVWS")) {
                w.i32(int32_t(v.indices.size() / 3));
                w.i32(int32_t(v.vertices.size()));
                for (const Vec3& p : v.vertices) w.vec3(p);
                for (uint16_t i : v.indices) w.u16(i);
            } else if (v.kind == fourcc("BVBX")) {
                w.vec3(v.centre);
                w.vec3(v.halfExtents);
            } else {
                w.vec3(v.centre);
                w.f32(v.sphereRadius);
            }
            w.bytes(v.tail);
        } else {
            w.bytes(v.raw);
        }
        w.end(at);
    }
    return std::move(w.out);
}

uint16_t floatToHalf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exponent = int32_t((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFF;
    if (((bits >> 23) & 0xFF) == 0xFF) return uint16_t(sign | 0x7C00 | (mantissa ? 0x200 : 0));   // inf / nan
    if (exponent >= 31) return uint16_t(sign | 0x7C00);
    if (exponent <= 0) {
        if (exponent < -10) return uint16_t(sign);
        mantissa |= 0x800000;
        const uint32_t shift = uint32_t(14 - exponent);
        uint32_t half = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (half & 1))) ++half;
        return uint16_t(sign | half);
    }
    uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
    const uint32_t remainder = mantissa & 0x1FFF;
    if (remainder > 0x1000 || (remainder == 0x1000 && (half & 1))) ++half;   // round to nearest even (may carry into the exponent)
    return uint16_t(sign | half);
}

void encodeVertexElement(uint8_t type, const float in[4], uint8_t* p) {
    auto putF = [&](int k, float f) { std::memcpy(p + 4 * k, &f, 4); };
    auto putI16 = [&](int k, int32_t v) { const int16_t s = int16_t(std::max(-32768, std::min(32767, v))); std::memcpy(p + 2 * k, &s, 2); };
    auto putU16 = [&](int k, int32_t v) { const uint16_t s = uint16_t(std::max(0, std::min(65535, v))); std::memcpy(p + 2 * k, &s, 2); };
    auto byte = [](float f) { return uint8_t(std::lround(std::min(std::max(f, 0.0f), 1.0f) * 255.0f)); };
    auto ubyte = [](float f) { return uint8_t(std::max(0L, std::min(255L, std::lround(f)))); };
    switch (type) {
        case 0: putF(0, in[0]); break;
        case 1: putF(0, in[0]); putF(1, in[1]); break;
        case 2: putF(0, in[0]); putF(1, in[1]); putF(2, in[2]); break;
        case 3: putF(0, in[0]); putF(1, in[1]); putF(2, in[2]); putF(3, in[3]); break;
        case 4: p[0] = byte(in[2]); p[1] = byte(in[1]); p[2] = byte(in[0]); p[3] = byte(in[3]); break;   // D3DCOLOR B G R A
        case 5: for (int k = 0; k < 4; ++k) p[k] = ubyte(in[k]); break;
        case 6: putI16(0, int32_t(std::lround(in[0]))); putI16(1, int32_t(std::lround(in[1]))); break;
        case 7: for (int k = 0; k < 4; ++k) putI16(k, int32_t(std::lround(in[k]))); break;
        case 8: for (int k = 0; k < 4; ++k) p[k] = byte(in[k]); break;
        case 9: putI16(0, int32_t(std::lround(in[0] * 32767.0f))); putI16(1, int32_t(std::lround(in[1] * 32767.0f))); break;
        case 10: for (int k = 0; k < 4; ++k) putI16(k, int32_t(std::lround(in[k] * 32767.0f))); break;
        case 11: putU16(0, int32_t(std::lround(in[0] * 65535.0f))); putU16(1, int32_t(std::lround(in[1] * 65535.0f))); break;
        case 12: for (int k = 0; k < 4; ++k) putU16(k, int32_t(std::lround(in[k] * 65535.0f))); break;
        case 13: {
            auto u10 = [](float f) { return uint32_t(std::max(0L, std::min(1023L, std::lround(f)))); };
            const uint32_t w = u10(in[0]) | (u10(in[1]) << 10) | (u10(in[2]) << 20);
            std::memcpy(p, &w, 4);
            break;
        }
        case 14: {
            auto s10 = [](float f) { return uint32_t(std::max(-511L, std::min(511L, std::lround(f * 511.0f)))) & 0x3FF; };
            const uint32_t w = s10(in[0]) | (s10(in[1]) << 10) | (s10(in[2]) << 20);
            std::memcpy(p, &w, 4);
            break;
        }
        case 15: {
            const uint16_t a = floatToHalf(in[0]), b = floatToHalf(in[1]);
            std::memcpy(p, &a, 2);
            std::memcpy(p + 2, &b, 2);
            break;
        }
        case 16:
            for (int k = 0; k < 4; ++k) {
                const uint16_t h = floatToHalf(in[k]);
                std::memcpy(p + 2 * k, &h, 2);
            }
            break;
        default: break;
    }
}

// ---- animations ----

AnimFile readAnimFile(const Bytes& data) {
    if (data.size() < 8 || std::memcmp(data.data(), "CHNK", 4) != 0) throw FormatError("not a chunk file (no CHNK header)");
    if (data[4] != 'B') throw FormatError("text chunk files are not supported");
    AnimFile file;
    file.header.assign(data.begin(), data.begin() + 8);
    ByteReader r(data, 8);
    const Header a = expectHeader(r, fourcc("ANIM"));
    file.animHeader = a.header;
    const uint32_t tracks = r.u32();
    file.byte0 = r.u8();
    for (uint32_t k = 0; k < tracks; ++k) {
        AnimFileTrack t;
        t.preTag = r.u32();
        const Header h = expectHeader(r, fourcc("TRAK"));
        t.header = h.header;
        t.byte0 = r.u8();
        t.target = r.u32();
        if (t.target == fourcc("SKEL") || t.target == fourcc("EFCT")) {
            t.name = r.cstr();
            if (t.target == fourcc("SKEL")) {
                t.byte1 = r.u8();
                t.flags = r.u32();
            } else {
                t.parameter = r.cstr();
                t.zero = r.u32();
            }
            t.keyCount = r.u32();
            const Header kh = readHeader(r);
            t.keyTag = kh.tag;
            t.keyHeader = kh.header;
            t.keyData = rest(r, kh.end);
            expectEnd(r, h);
        } else {
            t.raw = rest(r, h.end);
        }
        file.tracks.push_back(std::move(t));
    }
    file.tail = rest(r, a.end);
    file.after = rest(r, data.size());
    return file;
}

Bytes writeAnimFile(const AnimFile& file) {
    Writer w;
    if (file.header.empty()) {
        const char* h = "CHNKBLXX";
        w.out.assign(h, h + 8);
    } else {
        w.bytes(file.header);
    }
    const size_t a = w.begin(fourcc("ANIM"), file.animHeader);
    w.u32(uint32_t(file.tracks.size()));
    w.u8(file.byte0);
    for (const AnimFileTrack& t : file.tracks) {
        w.u32(t.preTag);
        const size_t at = w.begin(fourcc("TRAK"), t.header);
        w.u8(t.byte0);
        w.u32(t.target);
        if (t.target == fourcc("SKEL") || t.target == fourcc("EFCT")) {
            w.cstr(t.name);
            if (t.target == fourcc("SKEL")) {
                w.u8(t.byte1);
                w.u32(t.flags);
            } else {
                w.cstr(t.parameter);
                w.u32(t.zero);
            }
            w.u32(t.keyCount);
            const size_t k = w.begin(t.keyTag, t.keyHeader);
            w.bytes(t.keyData);
            w.end(k);
        } else {
            w.bytes(t.raw);
        }
        w.end(at);
    }
    w.bytes(file.tail);
    w.end(a);
    w.bytes(file.after);
    return std::move(w.out);
}

namespace {

struct KeyKind {
    uint32_t tag;
    bool compressed, scale, rotation, translation;
};

const KeyKind* keyKind(uint32_t tag) {
    static const KeyKind table[] = {
        {fourcc("KEYT"), false, false, false, true}, {fourcc("KEYR"), false, false, true, false}, {fourcc("KEYS"), false, true, false, false},
        {fourcc("KERT"), false, false, true, true},  {fourcc("KEST"), false, true, false, true},  {fourcc("KESR"), false, true, true, false},
        {fourcc("KSRT"), false, true, true, true},   {fourcc("XKER"), true, false, true, false},  {fourcc("XKRT"), true, false, true, true},
        {fourcc("XKSR"), true, true, true, false},   {fourcc("XSRT"), true, true, true, true},
    };
    for (const KeyKind& k : table)
        if (k.tag == tag) return &k;
    return nullptr;
}

}  // namespace

std::vector<AnimKey> decodeAnimKeys(const AnimFileTrack& track) {
    const KeyKind* kind = keyKind(track.keyTag);
    if (!kind) throw FormatError("unknown key chunk " + tagName(track.keyTag));
    ByteReader r(track.keyData);
    auto value = [&](bool rotation) {
        if (!kind->compressed) return r.f32();
        const uint16_t bits = r.u16();
        return rotation ? float(int16_t(bits)) / 32767.0f : halfToFloatValue(bits);
    };
    std::vector<AnimKey> keys(track.keyCount);
    for (AnimKey& key : keys) {
        if (kind->scale) {
            key.scale.x = value(false);
            key.scale.y = value(false);
            key.scale.z = value(false);
        }
        if (kind->rotation) {
            key.rotation.x = value(true);
            key.rotation.y = value(true);
            key.rotation.z = value(true);
            key.rotation.w = value(true);
            const float n = std::sqrt(key.rotation.x * key.rotation.x + key.rotation.y * key.rotation.y + key.rotation.z * key.rotation.z +
                                      key.rotation.w * key.rotation.w);
            if (n > 0) key.rotation = Quat{key.rotation.x / n, key.rotation.y / n, key.rotation.z / n, key.rotation.w / n};
        }
        if (kind->translation) {
            key.translation.x = value(false);
            key.translation.y = value(false);
            key.translation.z = value(false);
        }
        key.time = value(false);
    }
    return keys;
}

void encodeAnimKeys(AnimFileTrack& track, uint32_t keyTag, const std::vector<AnimKey>& keys) {
    const KeyKind* kind = keyKind(keyTag);
    if (!kind) throw FormatError("unknown key chunk " + tagName(keyTag));
    Writer w;
    auto value = [&](float f, bool rotation) {
        if (!kind->compressed) w.f32(f);
        else if (rotation) w.u16(uint16_t(int16_t(std::max(-32767L, std::min(32767L, std::lround(f * 32767.0f))))));
        else w.u16(floatToHalf(f));
    };
    for (const AnimKey& key : keys) {
        if (kind->scale) {
            value(key.scale.x, false);
            value(key.scale.y, false);
            value(key.scale.z, false);
        }
        if (kind->rotation) {
            value(key.rotation.x, true);
            value(key.rotation.y, true);
            value(key.rotation.z, true);
            value(key.rotation.w, true);
        }
        if (kind->translation) {
            value(key.translation.x, false);
            value(key.translation.y, false);
            value(key.translation.z, false);
        }
        value(key.time, false);
    }
    track.keyTag = keyTag;
    track.preTag = keyTag;
    track.keyCount = uint32_t(keys.size());
    track.keyData = std::move(w.out);
}

std::vector<MaterialKey> decodeMaterialKeys(const AnimFileTrack& track) {
    if (track.keyTag != fourcc("KEUV")) throw FormatError("material track key chunk " + tagName(track.keyTag) + " is not KEUV");
    ByteReader r(track.keyData);
    std::vector<MaterialKey> keys(track.keyCount);
    for (MaterialKey& key : keys) {
        key.scaleU = r.f32();
        key.scaleV = r.f32();
        key.rotation = r.vec3();
        key.translateU = r.f32();
        key.translateV = r.f32();
        key.time = r.f32();
    }
    return keys;
}

void encodeMaterialKeys(AnimFileTrack& track, const std::vector<MaterialKey>& keys) {
    Writer w;
    for (const MaterialKey& key : keys) {
        w.f32(key.scaleU);
        w.f32(key.scaleV);
        w.vec3(key.rotation);
        w.f32(key.translateU);
        w.f32(key.translateV);
        w.f32(key.time);
    }
    track.keyTag = fourcc("KEUV");
    track.preTag = track.keyTag;
    track.keyCount = uint32_t(keys.size());
    track.keyData = std::move(w.out);
}

}  // namespace tr
