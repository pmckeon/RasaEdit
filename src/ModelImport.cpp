// glTF -> .geo / .anm / .dds (see ModelExchange.h).

#include "ModelExchange.h"

#include "GameConfig.h"

#include "Json.h"
#include "assetlibrary.h"
#include "geofile.h"
#include "image.h"
#include "ModelMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string lower(const std::string& s) { return tr::toLowerAscii(s); }

std::string stem(const std::string& file) {
    const size_t slash = file.find_last_of("/\\");
    std::string name = slash == std::string::npos ? file : file.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

tr::Bytes readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw tr::FormatError("cannot read " + path.string());
    return tr::Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

tr::Bytes unbase64(const std::string& text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    tr::Bytes out;
    uint32_t bits = 0;
    int count = 0;
    for (char c : text) {
        const int v = value(c);
        if (v < 0) continue;
        bits = (bits << 6) | uint32_t(v);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(uint8_t(bits >> count));
        }
    }
    return out;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '%' && k + 2 < s.size()) {
            out += char(std::strtol(s.substr(k + 1, 2).c_str(), nullptr, 16));
            k += 2;
        } else {
            out += s[k];
        }
    }
    return out;
}

using namespace modelmath;

// ---- glTF reading ----

struct Gltf {
    JsonValue json;
    std::vector<tr::Bytes> buffers;
    fs::path folder;

    const JsonValue* list(const char* name) const { return json.member(name); }
    const JsonValue& item(const char* name, int64_t index) const {
        const JsonValue* l = list(name);
        if (!l || !l->isArray() || index < 0 || size_t(index) >= l->items.size())
            throw tr::FormatError(std::string("glTF: ") + name + " " + std::to_string(index) + " does not exist");
        return l->items[size_t(index)];
    }
};

int64_t integer(const JsonValue* v, int64_t fallback = -1) {
    int64_t out = fallback;
    if (v) v->integer(out);
    return out;
}

double number(const JsonValue* v, double fallback) { return v && v->isNumber() ? v->number() : fallback; }

std::string text(const JsonValue* v) { return v && v->isString() ? v->text : std::string(); }

Gltf loadGltf(const fs::path& path) {
    Gltf g;
    g.folder = path.parent_path();
    const tr::Bytes data = readAll(path);
    std::string jsonText;
    tr::Bytes glbBin;
    if (data.size() >= 12 && std::memcmp(data.data(), "glTF", 4) == 0) {
        size_t p = 12;
        while (p + 8 <= data.size()) {
            uint32_t length, type;
            std::memcpy(&length, &data[p], 4);
            std::memcpy(&type, &data[p + 4], 4);
            if (p + 8 + length > data.size()) throw tr::FormatError("GLB: chunk runs past the end");
            if (type == 0x4E4F534A) jsonText.assign(reinterpret_cast<const char*>(&data[p + 8]), length);
            else if (type == 0x004E4942) glbBin.assign(data.begin() + std::ptrdiff_t(p + 8), data.begin() + std::ptrdiff_t(p + 8 + length));
            p += 8 + length;
        }
    } else {
        jsonText.assign(data.begin(), data.end());
    }
    std::string error;
    if (!parseJson(jsonText, g.json, error)) throw tr::FormatError("glTF JSON: " + error);
    if (const JsonValue* buffers = g.json.member("buffers"))
        for (const JsonValue& b : buffers->items) {
            const std::string uri = text(b.member("uri"));
            if (uri.empty()) g.buffers.push_back(glbBin);
            else if (uri.rfind("data:", 0) == 0) g.buffers.push_back(unbase64(uri.substr(uri.find(',') + 1)));
            else g.buffers.push_back(readAll(g.folder / fs::u8path(percentDecode(uri))));
        }
    return g;
}

struct Accessor {
    std::vector<double> values;
    uint32_t components = 1;
    size_t count = 0;
    double get(size_t item, uint32_t c) const { return values[item * components + c]; }
};

Accessor readAccessor(const Gltf& g, int64_t index) {
    const JsonValue& a = g.item("accessors", index);
    if (a.member("sparse")) throw tr::FormatError("glTF: sparse accessors are not supported");
    Accessor out;
    const std::string type = text(a.member("type"));
    out.components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 0;
    if (!out.components) throw tr::FormatError("glTF: accessor type " + type + " is not supported");
    out.count = size_t(integer(a.member("count"), 0));
    const int64_t componentType = integer(a.member("componentType"), 5126);
    const bool normalized = a.member("normalized") && a.member("normalized")->boolean;
    const size_t size = componentType == 5126 || componentType == 5125 ? 4 : componentType == 5122 || componentType == 5123 ? 2 : 1;
    out.values.assign(out.count * out.components, 0.0);
    const int64_t viewIndex = integer(a.member("bufferView"));
    if (viewIndex < 0) return out;                         // all zeros
    const JsonValue& view = g.item("bufferViews", viewIndex);
    const int64_t buffer = integer(view.member("buffer"), 0);
    if (buffer < 0 || size_t(buffer) >= g.buffers.size()) throw tr::FormatError("glTF: buffer view names a missing buffer");
    const tr::Bytes& data = g.buffers[size_t(buffer)];
    const size_t stride = size_t(integer(view.member("byteStride"), int64_t(size * out.components)));
    const size_t base = size_t(integer(view.member("byteOffset"), 0)) + size_t(integer(a.member("byteOffset"), 0));
    for (size_t i = 0; i < out.count; ++i)
        for (uint32_t c = 0; c < out.components; ++c) {
            const size_t at = base + i * stride + c * size;
            if (at + size > data.size()) throw tr::FormatError("glTF: accessor runs past its buffer");
            const uint8_t* p = &data[at];
            double v = 0;
            switch (componentType) {
                case 5120: v = int8_t(p[0]); if (normalized) v = std::max(v / 127.0, -1.0); break;
                case 5121: v = p[0]; if (normalized) v /= 255.0; break;
                case 5122: { int16_t x; std::memcpy(&x, p, 2); v = x; if (normalized) v = std::max(v / 32767.0, -1.0); break; }
                case 5123: { uint16_t x; std::memcpy(&x, p, 2); v = x; if (normalized) v /= 65535.0; break; }
                case 5125: { uint32_t x; std::memcpy(&x, p, 4); v = x; break; }
                default: { float x; std::memcpy(&x, p, 4); v = x; break; }
            }
            out.values[i * out.components + c] = v;
        }
    return out;
}

// An image's pixels: an external PNG or one stored in a buffer view.
std::optional<tr::RgbaImage> readImage(const Gltf& g, int64_t index, std::string& name, std::vector<std::string>& warnings) {
    const JsonValue& image = g.item("images", index);
    name = text(image.member("name"));
    const std::string uri = text(image.member("uri"));
    try {
        tr::Bytes data;
        if (!uri.empty() && uri.rfind("data:", 0) == 0) {
            data = unbase64(uri.substr(uri.find(',') + 1));
        } else if (!uri.empty()) {
            data = readAll(g.folder / fs::u8path(percentDecode(uri)));
            if (name.empty()) name = stem(percentDecode(uri));
        } else {
            const JsonValue& view = g.item("bufferViews", integer(image.member("bufferView")));
            const tr::Bytes& buffer = g.buffers.at(size_t(integer(view.member("buffer"), 0)));
            const size_t offset = size_t(integer(view.member("byteOffset"), 0)), length = size_t(integer(view.member("byteLength"), 0));
            if (offset + length > buffer.size()) throw tr::FormatError("image runs past its buffer");
            data.assign(buffer.begin() + std::ptrdiff_t(offset), buffer.begin() + std::ptrdiff_t(offset + length));
        }
        if (data.size() < 8 || data[0] != 0x89 || data[1] != 'P') throw tr::FormatError("not a PNG (only PNG images are read; save the texture as PNG)");
        return tr::readPng(data);
    } catch (const std::exception& e) {
        warnings.push_back("image " + (name.empty() ? std::to_string(index) : name) + ": " + e.what());
        return std::nullopt;
    }
}

// ---- tangent frames ----

}  // namespace

void computeTangentFrames(const std::vector<tr::Vec3>& positions, const std::vector<tr::Vec3>& normals, const std::vector<tr::Vec2>& uvs,
                          const std::vector<uint32_t>& indices, std::vector<tr::Vec3>& tangents, std::vector<tr::Vec3>& binormals) {
    const size_t n = positions.size();
    std::vector<tr::Vec3> t(n), b(n);
    for (size_t k = 0; k + 2 < indices.size(); k += 3) {
        const uint32_t i0 = indices[k], i1 = indices[k + 1], i2 = indices[k + 2];
        if (i0 >= n || i1 >= n || i2 >= n) continue;
        const tr::Vec3 &p0 = positions[i0], &p1 = positions[i1], &p2 = positions[i2];
        const tr::Vec2 &u0 = uvs[i0], &u1 = uvs[i1], &u2 = uvs[i2];
        const float e1x = p1.x - p0.x, e1y = p1.y - p0.y, e1z = p1.z - p0.z, e2x = p2.x - p0.x, e2y = p2.y - p0.y, e2z = p2.z - p0.z;
        const float du1 = u1.x - u0.x, dv1 = u1.y - u0.y, du2 = u2.x - u0.x, dv2 = u2.y - u0.y;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-20f) continue;
        const float r = 1.0f / det;
        const tr::Vec3 sdir{(e1x * dv2 - e2x * dv1) * r, (e1y * dv2 - e2y * dv1) * r, (e1z * dv2 - e2z * dv1) * r};
        const tr::Vec3 tdir{(e2x * du1 - e1x * du2) * r, (e2y * du1 - e1y * du2) * r, (e2z * du1 - e1z * du2) * r};
        for (uint32_t i : {i0, i1, i2}) {
            t[i] = tr::Vec3{t[i].x + sdir.x, t[i].y + sdir.y, t[i].z + sdir.z};
            b[i] = tr::Vec3{b[i].x + tdir.x, b[i].y + tdir.y, b[i].z + tdir.z};
        }
    }
    tangents.resize(n);
    binormals.resize(n);
    auto normalise = [](tr::Vec3 v, const tr::Vec3& fallback) {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 1e-12f ? tr::Vec3{v.x / l, v.y / l, v.z / l} : fallback;
    };
    for (size_t i = 0; i < n; ++i) {
        const tr::Vec3& nn = normals[i];
        // Gram-Schmidt against the normal.
        const float dt = t[i].x * nn.x + t[i].y * nn.y + t[i].z * nn.z;
        tangents[i] = normalise(tr::Vec3{t[i].x - nn.x * dt, t[i].y - nn.y * dt, t[i].z - nn.z * dt}, tr::Vec3{1, 0, 0});
        const float db = b[i].x * nn.x + b[i].y * nn.y + b[i].z * nn.z;
        binormals[i] = normalise(tr::Vec3{b[i].x - nn.x * db, b[i].y - nn.y * db, b[i].z - nn.z * db}, tr::Vec3{0, 0, 1});
    }
}

namespace {

// glTF triangles (counter-clockwise front) back to the game's order: each triangle's second and third index swapped, from `first`
// on - the inverse of the export's swap (the z mirror reverses the winding test).
void flipWinding(std::vector<uint32_t>& indices, size_t first) {
    for (size_t k = first; k + 2 < indices.size(); k += 3) std::swap(indices[k + 1], indices[k + 2]);
}

// ---- building the model ----

struct BuiltVertex {
    tr::Vec3 position;                                // billboard sprites: the corner's offset from position1
    tr::Vec3 position1;                               // billboard sprites: the centre (POSITION1)
    tr::Vec3 normal{0, 1, 0};
    tr::Vec2 uv[4];
    float colour[4] = {0.5f, 0.5f, 0.5f, 1.0f};        // r g b a, 0..1 (128 grey: the game's neutral vertex colour)
    uint32_t bones[3] = {0, 0, 0};                    // skeleton bone indices
    float weights[3] = {1, 0, 0};
    tr::Vec3 tangent{1, 0, 0}, binormal{0, 0, 1};
};

// The default vertex layout of a new piece (the shipped pattern, see CLAUDE.md "Model exchange").
std::vector<tr::VertexElement> defaultLayout(bool skinned, bool tangents, uint32_t uvSets) {
    std::vector<tr::VertexElement> l;
    auto add = [&](uint8_t type, uint8_t usage, uint8_t index) {
        tr::VertexElement e;
        e.type = type;
        e.usage = usage;
        e.usageIndex = index;
        l.push_back(e);
    };
    add(2, 0, 0);
    if (skinned) {
        add(1, 1, 0);
        add(4, 2, 0);
    }
    add(2, 3, 0);
    add(4, 10, 0);
    for (uint32_t s = 0; s < std::max(1u, uvSets); ++s) add(1, 5, uint8_t(s));
    if (tangents) {
        add(2, 6, 0);
        add(2, 6, 1);
    }
    return l;
}

bool layoutHas(const std::vector<tr::VertexElement>& layout, uint8_t usage, uint8_t index) {
    for (const tr::VertexElement& e : layout)
        if (e.usage == usage && e.usageIndex == index) return true;
    return false;
}

tr::GeoFileVertices encodeVertices(const std::vector<tr::VertexElement>& layoutIn, const std::vector<BuiltVertex>& vertices,
                                   const tr::GeoFileVertices* headerFrom) {
    tr::GeoFileVertices v;
    if (headerFrom) {
        v.header = headerFrom->header;
        v.declHeader = headerFrom->declHeader;
    }
    v.layout = layoutIn;
    uint32_t offset = 0;
    for (tr::VertexElement& e : v.layout) {
        e.offset = offset;
        offset += tr::vertexElementSize(e.type);
    }
    v.stride = offset;
    v.count = uint32_t(vertices.size());
    v.data.assign(size_t(v.stride) * vertices.size(), 0);
    for (size_t i = 0; i < vertices.size(); ++i) {
        const BuiltVertex& bv = vertices[i];
        uint8_t* p = v.data.data() + i * v.stride;
        for (const tr::VertexElement& e : v.layout) {
            float in[4] = {0, 0, 0, 0};
            switch (e.usage) {
                case 0:
                    if (e.usageIndex == 1) { in[0] = bv.position1.x; in[1] = bv.position1.y; in[2] = bv.position1.z; in[3] = 1; break; }
                    if (e.usageIndex != 0) continue;
                    in[0] = bv.position.x; in[1] = bv.position.y; in[2] = bv.position.z; in[3] = 1;
                    break;
                case 1: in[0] = bv.weights[0]; in[1] = bv.weights[1]; in[2] = bv.weights[2]; break;
                case 2:
                    // BLENDINDICES: raw bytes 0, 1, 2 = palette slots (filled by the caller through `bones`, already palette slots).
                    p[e.offset + 0] = uint8_t(bv.bones[0]);
                    p[e.offset + 1] = uint8_t(bv.bones[1]);
                    p[e.offset + 2] = uint8_t(bv.bones[2]);
                    p[e.offset + 3] = 0;
                    continue;
                case 3: in[0] = bv.normal.x; in[1] = bv.normal.y; in[2] = bv.normal.z; break;
                case 5:
                    if (e.usageIndex < 4) { in[0] = bv.uv[e.usageIndex].x; in[1] = bv.uv[e.usageIndex].y; }
                    break;
                case 6:
                    if (e.usageIndex == 0) { in[0] = bv.tangent.x; in[1] = bv.tangent.y; in[2] = bv.tangent.z; }
                    else { in[0] = bv.binormal.x; in[1] = bv.binormal.y; in[2] = bv.binormal.z; }
                    break;
                case 10: std::memcpy(in, bv.colour, sizeof in); break;
                default: break;
            }
            tr::encodeVertexElement(e.type, in, p + e.offset);
        }
    }
    return v;
}

tr::GeoFileBounds boundsOf(const std::vector<tr::Vec3>& points) {
    tr::GeoFileBounds b;
    if (points.empty()) {
        b.isEmpty = 1;
        return b;
    }
    b.min = b.max = points[0];
    for (const tr::Vec3& p : points) {
        b.min = tr::Vec3{std::min(b.min.x, p.x), std::min(b.min.y, p.y), std::min(b.min.z, p.z)};
        b.max = tr::Vec3{std::max(b.max.x, p.x), std::max(b.max.y, p.y), std::max(b.max.z, p.z)};
    }
    // Centre = the box's middle, radius = the farthest point from it (the shipped convention in 13,627 of 18,748 pieces).
    b.centre = tr::Vec3{(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, (b.min.z + b.max.z) / 2};
    float r = 0;
    for (const tr::Vec3& p : points) {
        const float dx = p.x - b.centre.x, dy = p.y - b.centre.y, dz = p.z - b.centre.z;
        r = std::max(r, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    b.radius = r;
    return b;
}

// RasaEdit writes its extras as JSON text: Blender keeps a string custom property as it is, while it turns lists of mixed types
// into Python literal text. Objects are accepted too (other tools).
std::deque<JsonValue>& parsedExtras() {
    static std::deque<JsonValue> store;
    return store;
}

const JsonValue* extrasMember(const JsonValue& owner, const char* name) {
    const JsonValue* extras = owner.member("extras");
    const JsonValue* v = extras ? extras->member(name) : nullptr;
    if (!v || !v->isString()) return v;
    JsonValue parsed;
    std::string error;
    if (!parseJson(v->text, parsed, error)) return nullptr;
    parsedExtras().push_back(std::move(parsed));
    return &parsedExtras().back();
}

// glTF quaternion sample (normalised lerp with sign alignment), vectors linear.
std::vector<double> sampleChannel(const std::vector<double>& times, const Accessor& values, bool cubic, uint32_t components, double t, bool quaternion) {
    const size_t n = times.size();
    auto value = [&](size_t k, uint32_t c) { return cubic ? values.get(k * 3 + 1, c) : values.get(k, c); };
    std::vector<double> out(components);
    if (n == 0) return out;
    size_t k = 0;
    while (k + 1 < n && times[k + 1] <= t) ++k;
    if (t <= times[0] || k + 1 >= n) {
        const size_t at = t <= times[0] ? 0 : n - 1;
        for (uint32_t c = 0; c < components; ++c) out[c] = value(at, c);
        return out;
    }
    const double f = (t - times[k]) / std::max(1e-12, times[k + 1] - times[k]);
    double sign = 1;
    if (quaternion) {
        double dot = 0;
        for (uint32_t c = 0; c < 4; ++c) dot += value(k, c) * value(k + 1, c);
        if (dot < 0) sign = -1;
    }
    for (uint32_t c = 0; c < components; ++c) out[c] = value(k, c) + (sign * value(k + 1, c) - value(k, c)) * f;
    if (quaternion) {
        double l = 0;
        for (double x : out) l += x * x;
        l = std::sqrt(l);
        if (l > 0)
            for (double& x : out) x /= l;
    }
    return out;
}

// A shipped piece using an effect (for a new material's parameter set): the first model piece in archive order.
const tr::GeoFilePiece* shippedPieceWithEffect(tr::AssetLibrary& assets, const std::string& effect, bool skinned, tr::GeoFile& holder) {
    static std::map<std::string, std::string> cache;   // lower effect + skinned -> model name
    const std::string key = lower(effect) + (skinned ? "|s" : "|");
    auto findIn = [&](const std::string& model) -> const tr::GeoFilePiece* {
        try {
            holder = tr::readGeoFile(assets.read(model));
        } catch (const std::exception&) {
            return nullptr;
        }
        for (const tr::GeoFilePiece& p : holder.pieces)
            if (lower(p.effect.text) == lower(effect) && p.skinned == skinned) return &p;
        return nullptr;
    };
    if (auto it = cache.find(key); it != cache.end()) return findIn(it->second);
    // Preferred templates for the defaults (geofacts.py: the effect's most common layout).
    static const std::pair<const char*, const char*> known[] = {
        {"trdiffmapvertcolor.fx|", "arch_bane_commandcenter_mapboard01.geo"},
        {"trdiffmapvertcolor.fx|s", "ambient_f_lying_injured_bed_covered_v01.geo"},
    };
    for (const auto& k : known)
        if (key == k.first)
            if (const tr::GeoFilePiece* p = findIn(k.second)) {
                cache[key] = k.second;
                return p;
            }
    for (const std::string& model : assets.namesWithExtension(".geo")) {
        tr::Bytes data;
        if (!assets.tryRead(model, data)) continue;
        // Cheap test before parsing: the effect name occurs in the file.
        const std::string needle = effect;
        if (std::search(data.begin(), data.end(), needle.begin(), needle.end()) == data.end()) continue;
        if (const tr::GeoFilePiece* p = findIn(model)) {
            cache[key] = model;
            return p;
        }
    }
    return nullptr;
}

}  // namespace

ModelImportResult importModelGltf(tr::AssetLibrary& assets, const std::string& path, const ModelImportOptions& options) {
    ModelImportResult result;
    auto warn = [&](const std::string& w) { result.warnings.push_back(w); };
    const Gltf g = loadGltf(fs::u8path(path));
    const JsonValue* nodes = g.list("nodes");
    if (!nodes || nodes->items.empty()) throw tr::FormatError("glTF: no nodes");
    const size_t nodeCount = nodes->items.size();

    // Node hierarchy and rest-pose world matrices.
    std::vector<int64_t> parentOf(nodeCount, -1);
    for (size_t k = 0; k < nodeCount; ++k)
        if (const JsonValue* children = nodes->items[k].member("children"))
            for (const JsonValue& c : children->items) {
                const int64_t child = integer(&c);
                if (child >= 0 && size_t(child) < nodeCount) parentOf[size_t(child)] = int64_t(k);
            }
    std::vector<M4> local(nodeCount), world(nodeCount);
    for (size_t k = 0; k < nodeCount; ++k) {
        const JsonValue& n = nodes->items[k];
        if (const JsonValue* matrix = n.member("matrix"); matrix && matrix->items.size() == 16) {
            for (int a = 0; a < 16; ++a) local[k].m[a] = matrix->items[size_t(a)].number();
        } else {
            double t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
            if (const JsonValue* v = n.member("translation"); v && v->items.size() == 3)
                for (int c = 0; c < 3; ++c) t[c] = v->items[size_t(c)].number();
            if (const JsonValue* v = n.member("rotation"); v && v->items.size() == 4)
                for (int c = 0; c < 4; ++c) q[c] = v->items[size_t(c)].number();
            if (const JsonValue* v = n.member("scale"); v && v->items.size() == 3)
                for (int c = 0; c < 3; ++c) s[c] = v->items[size_t(c)].number();
            local[k] = fromTrs(t, q, s);
        }
    }
    std::vector<bool> done(nodeCount, false);
    std::function<const M4&(size_t)> worldOf = [&](size_t k) -> const M4& {
        if (!done[k]) {
            world[k] = parentOf[k] >= 0 ? worldOf(size_t(parentOf[k])) * local[k] : local[k];
            done[k] = true;
        }
        return world[k];
    };
    for (size_t k = 0; k < nodeCount; ++k) worldOf(k);
    auto nodeName = [&](int64_t k) { return text(nodes->items[size_t(k)].member("name")); };

    // The model's root (rasaModel) and source.
    const JsonValue* rasaModel = nullptr;
    for (size_t k = 0; k < nodeCount && !rasaModel; ++k) rasaModel = extrasMember(nodes->items[k], "rasaModel");
    std::string sourceName = options.source;
    if (rasaModel && sourceName.empty()) sourceName = text(rasaModel->member("source"));
    std::optional<tr::GeoFile> source;
    if (!sourceName.empty()) {
        tr::Bytes data;
        if (assets.tryRead(sourceName, data)) source = tr::readGeoFile(data);
        else warn("source model " + sourceName + " is not in the archives: imported as a new model");
    }
    const std::string modelName = !options.modelName.empty() ? options.modelName : source ? sourceName : stem(path) + ".geo";

    tr::GeoFile out;
    if (source) {
        out.header = source->header;
        out.gbodHeader = source->gbodHeader;
        out.gbodValue = source->gbodValue;
        out.hasShaderList = source->hasShaderList;
        out.shaderList = source->shaderList;
        out.cpdgHeader = source->cpdgHeader;
        out.pskeHeader = source->pskeHeader;
        out.tail = source->tail;
    } else {
        out.tail = {0};
    }
    if (rasaModel) {
        out.gbodValue = float(number(rasaModel->member("gbodValue"), out.gbodValue));
        if (const JsonValue* groups = rasaModel->member("properties")) {
            for (const JsonValue& gj : groups->items) {
                tr::GeoPropertyGroup group;
                group.id = int32_t(integer(gj.member("id"), 0));
                if (const JsonValue* pairs = gj.member("pairs"))
                    for (const JsonValue& pair : pairs->items)
                        if (pair.items.size() == 2) group.pairs.push_back(tr::GeoParameter{pair.items[0].text, pair.items[1].text});
                out.properties.push_back(std::move(group));
            }
        }
    } else if (source) {
        out.properties = source->properties;
    }

    // ---- skeleton ----
    // Joints of the first skin a mesh uses.
    const JsonValue* skins = g.list("skins");
    int64_t skinIndex = -1;
    for (size_t k = 0; k < nodeCount && skinIndex < 0; ++k)
        if (nodes->items[k].member("mesh") && nodes->items[k].member("skin")) skinIndex = integer(nodes->items[k].member("skin"));
    std::vector<int64_t> joints;
    std::vector<M4> jointInverseBind;
    if (skinIndex >= 0 && skins && size_t(skinIndex) < skins->items.size()) {
        const JsonValue& skin = skins->items[size_t(skinIndex)];
        if (const JsonValue* j = skin.member("joints"))
            for (const JsonValue& x : j->items) joints.push_back(integer(&x));
        jointInverseBind.assign(joints.size(), M4{});
        if (const JsonValue* ibm = skin.member("inverseBindMatrices")) {
            const Accessor a = readAccessor(g, integer(ibm));
            for (size_t k = 0; k < joints.size() && k < a.count; ++k)
                for (int e = 0; e < 16; ++e) jointInverseBind[k].m[e] = a.get(k, uint32_t(e));
        } else {
            for (size_t k = 0; k < joints.size(); ++k) jointInverseBind[k] = inverse(world[size_t(joints[k])]);
        }
        if (skins->items.size() > 1) warn("the file has " + std::to_string(skins->items.size()) + " skins: only the first used by a mesh is read");
    }
    std::set<int64_t> jointSet(joints.begin(), joints.end());
    // Bones that are not joints but parent meshes or contact points still count when they carry rasaBone (unskinned models).
    std::vector<int64_t> boneNodes = joints;
    for (size_t k = 0; k < nodeCount; ++k)
        if (!jointSet.count(int64_t(k)) && extrasMember(nodes->items[k], "rasaBone")) {
            boneNodes.push_back(int64_t(k));
            jointInverseBind.push_back(inverse(world[k]));
            jointSet.insert(int64_t(k));
        }

    std::map<std::string, size_t> boneIndex;            // lower-case name -> out.bones index
    std::vector<int64_t> boneNodeOf;                     // out.bones index -> glTF node (-1: kept from the source only)
    size_t keptBones = 0, changedBones = 0, newBones = 0;
    if (source) {
        out.bones = source->bones;
        for (size_t b = 0; b < out.bones.size(); ++b) boneIndex[lower(out.bones[b].name)] = b;
        boneNodeOf.assign(out.bones.size(), -1);
    }
    // Bone order: the source's, then new bones with parents first.
    std::vector<size_t> pending;
    for (size_t j = 0; j < boneNodes.size(); ++j) pending.push_back(j);
    auto nearestBoneAncestor = [&](int64_t node) -> int64_t {
        for (int64_t p = parentOf[size_t(node)]; p >= 0; p = parentOf[size_t(p)])
            if (jointSet.count(p)) return p;
        return -1;
    };
    for (size_t pass = 0; pass < boneNodes.size() + 1 && !pending.empty(); ++pass) {
        std::vector<size_t> later;
        for (size_t j : pending) {
            const int64_t node = boneNodes[j];
            const std::string name = nodeName(node);
            const int64_t parentNode = nearestBoneAncestor(node);
            const auto existing = boneIndex.find(lower(name));
            if (existing != boneIndex.end()) {
                // A source bone: unchanged when its inverse bind matches.
                const size_t b = existing->second;
                boneNodeOf[b] = node;
                tr::GeoFileBone& bone = out.bones[b];
                float ibm[16];
                mirrorArray(jointInverseBind[j].m, ibm);
                bool same = bone.hasInverseBind;
                for (int e = 0; e < 16 && same; ++e) same = std::fabs(ibm[e] - bone.inverseBind[e]) <= 1e-3f * std::max(1.0f, std::fabs(bone.inverseBind[e]));
                if (same) {
                    ++keptBones;
                } else {
                    ++changedBones;
                    std::memcpy(bone.inverseBind, ibm, sizeof ibm);
                    bone.hasInverseBind = true;
                    // The bind (PSKE) from the new rest pose relative to the parent's.
                    const M4 rest = inverse(jointInverseBind[j]);
                    const M4 parentRest = parentNode >= 0 ? inverse(jointInverseBind[size_t(std::find(boneNodes.begin(), boneNodes.end(), parentNode) - boneNodes.begin())]) : M4{};
                    const Trs l = decompose(inverse(parentRest) * rest);
                    bone.translation = mirror(l.translation);
                    bone.rotation = mirror(l.rotation);
                    bone.scale = l.scale;
                    bone.boundingVolume.clear();           // the collision volume no longer fits
                }
                continue;
            }
            // A new bone: its parent must be placed first.
            int32_t parentBone = -1;
            if (parentNode >= 0) {
                const auto p = boneIndex.find(lower(nodeName(parentNode)));
                if (p == boneIndex.end()) {
                    later.push_back(j);
                    continue;
                }
                parentBone = int32_t(p->second);
            }
            tr::GeoFileBone bone;
            bone.name = name;
            bone.parent = parentBone;
            if (const JsonValue* rb = extrasMember(nodes->items[size_t(node)], "rasaBone")) {
                if (const JsonValue* pbon = rb->member("pbon"); pbon && pbon->items.size() == 3) {
                    bone.unknownA = int32_t(pbon->items[0].number());
                    bone.unknownB = int32_t(pbon->items[1].number());
                    bone.unknownC = float(pbon->items[2].number());
                }
                const std::string bv = text(rb->member("boundingVolume"));
                if (!bv.empty()) bone.boundingVolume = unbase64(bv);
            }
            mirrorArray(jointInverseBind[j].m, bone.inverseBind);
            const M4 rest = inverse(jointInverseBind[j]);
            M4 parentRest;
            if (parentNode >= 0) parentRest = inverse(jointInverseBind[size_t(std::find(boneNodes.begin(), boneNodes.end(), parentNode) - boneNodes.begin())]);
            const Trs l = decompose(inverse(parentRest) * rest);
            bone.translation = mirror(l.translation);
            bone.rotation = mirror(l.rotation);
            bone.scale = l.scale;
            // The game's bind values when the export carried them (they can differ from the rest pose).
            if (const JsonValue* rb = extrasMember(nodes->items[size_t(node)], "rasaBone"))
                if (const JsonValue* bind = rb->member("bind"); bind && bind->items.size() == 10) {
                    auto f = [&](size_t k) { return float(bind->items[k].number()); };
                    bone.translation = tr::Vec3{f(0), f(1), f(2)};
                    bone.rotation = tr::Quat{f(3), f(4), f(5), f(6)};
                    bone.scale = tr::Vec3{f(7), f(8), f(9)};
                }
            boneIndex[lower(name)] = out.bones.size();
            out.bones.push_back(std::move(bone));
            boneNodeOf.push_back(node);
            ++newBones;
        }
        pending = later;
    }
    if (!pending.empty()) warn(std::to_string(pending.size()) + " bone(s) could not be placed under a parent");
    if (out.bones.empty()) {
        // A model without a skeleton: one identity bone (the shipped one-bone pattern: 828 of 1,665 one-bone models).
        tr::GeoFileBone bone;
        bone.name = "Bone_main";
        out.bones.push_back(bone);
        boneIndex[lower(bone.name)] = 0;
        boneNodeOf.push_back(-1);
        newBones = 1;
    }
    // Bind (PSKE) world of each bone in glTF space: static pieces and contact points are stored relative to it.
    std::vector<M4> boneBindWorld(out.bones.size());
    for (size_t b = 0; b < out.bones.size(); ++b) {
        const int32_t p = out.bones[b].parent;
        boneBindWorld[b] = (p >= 0 && size_t(p) < b ? boneBindWorld[size_t(p)] : M4{}) * gltfLocal(out.bones[b]);
    }
    // A glTF node's bone: the bone of that name, else its nearest bone ancestor's (by name); -1 = none (model space).
    auto boneForNode = [&](int64_t node) -> int64_t {
        for (int64_t k = node; k >= 0; k = parentOf[size_t(k)])
            if (jointSet.count(k))
                if (auto it = boneIndex.find(lower(nodeName(k))); it != boneIndex.end()) return int64_t(it->second);
        return -1;
    };
    // Where a glTF node's rest-pose frame is, relative to the game bone it belongs to (-1: model space). For a source bone kept
    // unchanged, Blender's rest frame (from the inverse bind) can differ from the game's bind (PSKE) frame, but children placed under
    // it keep their world position; the game draws static pieces and contact points in the bone's bind world, so go through world space.
    auto relativeToBone = [&](int64_t node, int64_t bone) {
        return bone >= 0 ? inverse(boneBindWorld[size_t(bone)]) * world[size_t(node)] : world[size_t(node)];
    };

    // ---- materials and textures ----
    std::map<std::string, std::string> textureRename;    // lower original texture -> name written
    std::set<std::string> textureDone;
    auto handleTexture = [&](const std::string& original, const std::optional<tr::RgbaImage>& image, const std::string& imageName) -> std::string {
        if (!image) return original;
        std::string name = original.empty() ? stem(imageName) + ".dds" : original;
        tr::Bytes originalDds;
        const bool exists = !original.empty() && assets.tryRead(original, originalDds);
        tr::DdsEncoding encoding = tr::DdsEncoding::Dxt1;
        if (exists) {
            try {
                const tr::DdsHeader header = tr::readDdsHeader(originalDds);
                const tr::RgbaImage before = tr::ddsImage(originalDds);
                bool same = before.width == image->width && before.height == image->height;
                for (size_t k = 0; same && k < before.pixels.size(); ++k) same = std::memcmp(&before.pixels[k], &image->pixels[k], 4) == 0;
                if (same) return original;
                encoding = tr::ddsEncodingLike(header);
            } catch (const std::exception&) {
            }
            if (options.renameChangedTextures) name = stem(modelName) + "_" + original;
        } else {
            bool alpha = false;
            for (const tr::Rgba8& p : image->pixels) alpha = alpha || p.alpha < 255;
            encoding = alpha ? tr::DdsEncoding::Dxt5 : tr::DdsEncoding::Dxt1;
        }
        if (!textureDone.count(lower(name))) {
            textureDone.insert(lower(name));
            ImportedFile f;
            f.name = name;
            f.data = tr::encodeDds(*image, encoding);
            f.note = std::string(exists ? "changed texture" : "new texture") + " (" + tr::ddsEncodingName(encoding) + ", " + std::to_string(image->width) + " x " +
                     std::to_string(image->height) + ")";
            result.files.push_back(std::move(f));
        }
        if (!original.empty()) textureRename[lower(original)] = name;
        return name;
    };
    // The image a glTF texture info points at.
    auto textureImage = [&](const JsonValue* info, std::string& imageName) -> std::optional<tr::RgbaImage> {
        if (!info) return std::nullopt;
        const int64_t t = integer(info->member("index"));
        if (t < 0) return std::nullopt;
        const int64_t source = integer(g.item("textures", t).member("source"));
        if (source < 0) return std::nullopt;
        return readImage(g, source, imageName, result.warnings);
    };
    // Images of the other slots: textures/<name>.png beside the file (as RasaEdit exported them), if changed.
    auto looseImage = [&](const std::string& original) -> std::optional<tr::RgbaImage> {
        for (const fs::path& candidate : {g.folder / "textures" / (stem(original) + ".png"), g.folder / (stem(original) + ".png")}) {
            std::error_code ec;
            if (!fs::exists(candidate, ec)) continue;
            try {
                return tr::readPng(readAll(candidate));
            } catch (const std::exception& e) {
                warn(candidate.filename().string() + ": " + e.what());
            }
        }
        return std::nullopt;
    };

    struct MaterialSetup {
        tr::GeoChunkHeader efctHeader{3, 0};
        tr::GeoAssetId effect;
        std::vector<tr::GeoFileParameter> parameters;
        bool valid = false;
    };
    std::map<std::pair<int64_t, bool>, MaterialSetup> materialCache;
    auto material = [&](int64_t index, bool skinned) -> MaterialSetup {
        const auto key = std::make_pair(index, skinned);
        if (auto it = materialCache.find(key); it != materialCache.end()) return it->second;
        MaterialSetup m;
        const JsonValue* mj = index >= 0 ? &g.item("materials", index) : nullptr;
        const JsonValue* rm = mj ? extrasMember(*mj, "rasaMaterial") : nullptr;
        std::string effect = rm ? text(rm->member("effect")) : "";
        if (effect.empty() && mj) effect = text(extrasMember(*mj, "rasaEffect"));
        if (rm && rm->member("parameters")) {
            for (const JsonValue& pj : rm->member("parameters")->items) {
                if (pj.items.size() < 3) continue;
                tr::GeoFileParameter p;
                p.key = pj.items[0].text;
                p.type = int32_t(pj.items[1].number());
                const JsonValue& v = pj.items[2];
                switch (p.type) {
                    case 1: p.boolValue = v.boolean ? 1 : 0; break;
                    case 2: { int64_t x = 0; v.integer(x); p.intValue = int32_t(x); break; }
                    case 3: for (const JsonValue& f : v.items) p.floats.push_back(float(f.number())); break;
                    case 4: p.text = v.text; break;
                    case 5: case 8: p.asset.text = v.text; break;
                    default: p.raw = unbase64(v.text); break;
                }
                m.parameters.push_back(std::move(p));
            }
            m.effect.text = effect;
            m.valid = true;
        } else {
            if (effect.empty()) effect = "TRDiffMapVertColor.fx";
            tr::GeoFile holder;
            if (const tr::GeoFilePiece* shipped = shippedPieceWithEffect(assets, effect, skinned, holder)) {
                m.parameters = shipped->parameters;
                m.effect = shipped->effect;
                m.efctHeader = shipped->efctHeader;
                m.valid = true;
            } else {
                warn("no shipped " + std::string(skinned ? "skinned" : "static") + " piece uses " + effect + ": its parameters are unknown");
                m.effect.text = effect;
                m.valid = true;
            }
        }
        // Textures: the glTF base colour / normal maps fill DiffuseTexture / NormalMapTexture; other slots from loose PNGs.
        std::string imageName;
        const JsonValue* pbr = mj ? mj->member("pbrMetallicRoughness") : nullptr;
        const std::optional<tr::RgbaImage> base = textureImage(pbr ? pbr->member("baseColorTexture") : nullptr, imageName);
        std::string normalName;
        const std::optional<tr::RgbaImage> normal = textureImage(mj ? mj->member("normalTexture") : nullptr, normalName);
        for (tr::GeoFileParameter& p : m.parameters) {
            if (p.type != 5 && p.type != 8) continue;
            const std::string original = p.asset.text;
            if (auto renamed = textureRename.find(lower(original)); renamed != textureRename.end()) {
                p.asset.text = renamed->second;
                continue;
            }
            if (p.key == "DiffuseTexture" && base) p.asset.text = handleTexture(rm ? original : "", base, imageName);
            else if (p.key == "NormalMapTexture" && normal) p.asset.text = handleTexture(rm ? original : "", normal, normalName);
            else if (rm && !original.empty() && !textureDone.count(lower(original))) p.asset.text = handleTexture(original, looseImage(original), original);
        }
        materialCache[key] = m;
        return m;
    };

    // ---- pieces ----
    std::vector<std::pair<int64_t, tr::GeoFilePiece>> builtPieces;     // (source piece index or -1, piece)
    const JsonValue* meshes = g.list("meshes");
    std::set<int64_t> sourcePiecesSeen;
    // Collision meshes (RasaEdit's rasaCollision data, or a node / mesh named "collision...") are not pieces.
    auto collisionData = [&](size_t nodeIndex) -> const JsonValue* {
        static const JsonValue empty = jsonObject();
        const JsonValue& node = nodes->items[nodeIndex];
        const int64_t meshIndex = integer(node.member("mesh"));
        if (meshIndex < 0 || !meshes || size_t(meshIndex) >= meshes->items.size()) return nullptr;
        const JsonValue& mesh = meshes->items[size_t(meshIndex)];
        if (const JsonValue* rc = extrasMember(node, "rasaCollision")) return rc;
        if (const JsonValue* rc = extrasMember(mesh, "rasaCollision")) return rc;
        if (lower(text(node.member("name"))).rfind("collision", 0) == 0 || lower(text(mesh.member("name"))).rfind("collision", 0) == 0) return &empty;
        return nullptr;
    };
    for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
        const JsonValue& node = nodes->items[nodeIndex];
        const int64_t meshIndex = integer(node.member("mesh"));
        if (meshIndex < 0 || !meshes || size_t(meshIndex) >= meshes->items.size()) continue;
        if (collisionData(nodeIndex)) continue;
        const JsonValue& mesh = meshes->items[size_t(meshIndex)];
        const JsonValue* rp = extrasMember(mesh, "rasaPiece");
        const bool skinned = node.member("skin") && !joints.empty();
        const JsonValue* primitives = mesh.member("primitives");
        if (!primitives) continue;
        int64_t sourcePiece = rp ? integer(rp->member("index")) : -1;
        if (sourcePiece >= 0 && (!source || size_t(sourcePiece) >= source->pieces.size())) sourcePiece = -1;
        for (size_t primIndex = 0; primIndex < primitives->items.size(); ++primIndex) {
            const JsonValue& prim = primitives->items[primIndex];
            const int64_t mode = integer(prim.member("mode"), 4);
            if (mode != 4) {
                warn("mesh " + text(mesh.member("name")) + ": primitive mode " + std::to_string(mode) + " is not triangles - left out");
                continue;
            }
            const JsonValue* attributes = prim.member("attributes");
            if (!attributes || !attributes->member("POSITION")) continue;
            const Accessor pos = readAccessor(g, integer(attributes->member("POSITION")));
            std::optional<Accessor> nrm, col, jnt, wgt, uv[4], centre;
            if (attributes->member("NORMAL")) nrm = readAccessor(g, integer(attributes->member("NORMAL")));
            if (attributes->member("_POSITION1")) centre = readAccessor(g, integer(attributes->member("_POSITION1")));
            if (attributes->member("COLOR_0")) col = readAccessor(g, integer(attributes->member("COLOR_0")));
            if (attributes->member("JOINTS_0")) jnt = readAccessor(g, integer(attributes->member("JOINTS_0")));
            if (attributes->member("WEIGHTS_0")) wgt = readAccessor(g, integer(attributes->member("WEIGHTS_0")));
            uint32_t uvSets = 0;
            for (uint32_t s = 0; s < 4; ++s)
                if (const JsonValue* u = attributes->member("TEXCOORD_" + std::to_string(s))) {
                    uv[s] = readAccessor(g, integer(u));
                    uvSets = s + 1;
                }
            std::vector<uint32_t> indices;
            if (prim.member("indices")) {
                const Accessor ia = readAccessor(g, integer(prim.member("indices")));
                for (size_t k = 0; k < ia.count; ++k) indices.push_back(uint32_t(ia.get(k, 0)));
            } else {
                for (size_t k = 0; k < pos.count; ++k) indices.push_back(uint32_t(k));
            }
            flipWinding(indices, 0);                     // glTF counter-clockwise -> the game's order (see the export)
            // Vertices in the game's piece space.
            int64_t pieceBone = skinned ? -1 : boneForNode(int64_t(nodeIndex));
            // Outside every bone (hidden low-detail pieces under their own root, or a mesh unparented in Blender): the bone its piece
            // data names, the mesh placed relative to that bone's bind world from where it is.
            if (!skinned && pieceBone < 0 && rp)
                if (auto it = boneIndex.find(lower(text(rp->member("bone")))); it != boneIndex.end()) pieceBone = int64_t(it->second);
            const M4 toPiece = skinned ? M4{} : relativeToBone(int64_t(nodeIndex), pieceBone);
            std::vector<BuiltVertex> verts(pos.count);
            for (size_t i = 0; i < pos.count; ++i) {
                BuiltVertex& v = verts[i];
                tr::Vec3 p{float(pos.get(i, 0)), float(pos.get(i, 1)), float(pos.get(i, 2))};
                v.position = mirror(transformPoint(toPiece, p));
                if (centre) {
                    // Billboard: the export placed the corner at centre + offset; the game stores the offset and the centre.
                    v.position1 = mirror(transformPoint(toPiece, tr::Vec3{float(centre->get(i, 0)), float(centre->get(i, 1)), float(centre->get(i, 2))}));
                    v.position = tr::Vec3{v.position.x - v.position1.x, v.position.y - v.position1.y, v.position.z - v.position1.z};
                }
                if (nrm) v.normal = mirror(transformNormal(toPiece, tr::Vec3{float(nrm->get(i, 0)), float(nrm->get(i, 1)), float(nrm->get(i, 2))}));
                for (uint32_t s = 0; s < 4; ++s)
                    if (uv[s]) v.uv[s] = tr::Vec2{float(uv[s]->get(i, 0)), float(uv[s]->get(i, 1))};
                for (uint32_t s = uvSets; s < 4; ++s) v.uv[s] = v.uv[0];
                if (col) {
                    for (uint32_t c = 0; c < 3; ++c) v.colour[c] = float(col->get(i, c));
                    v.colour[3] = col->components == 4 ? float(col->get(i, 3)) : 1.0f;
                }
                if (skinned && jnt && wgt) {
                    // The three largest influences, renormalised (the game blends three bones per vertex).
                    std::pair<float, uint32_t> inf[4];
                    for (uint32_t c = 0; c < 4; ++c) {
                        const int64_t joint = int64_t(jnt->get(i, std::min(c, jnt->components - 1)));
                        uint32_t bone = 0;
                        if (joint >= 0 && size_t(joint) < joints.size())
                            if (auto it = boneIndex.find(lower(nodeName(joints[size_t(joint)]))); it != boneIndex.end()) bone = uint32_t(it->second);
                        inf[c] = {c < wgt->components ? float(wgt->get(i, c)) : 0.0f, bone};
                    }
                    // A joint listed twice counts once with the summed weight.
                    for (uint32_t c = 0; c < 4; ++c)
                        for (uint32_t d = c + 1; d < 4; ++d)
                            if (inf[d].first > 0 && inf[d].second == inf[c].second) {
                                inf[c].first += inf[d].first;
                                inf[d].first = 0;
                            }
                    std::sort(std::begin(inf), std::end(inf), [](const auto& a, const auto& b) { return a.first > b.first; });
                    const float sum = inf[0].first + inf[1].first + inf[2].first;
                    for (uint32_t c = 0; c < 3; ++c) {
                        v.bones[c] = inf[c].second;
                        v.weights[c] = sum > 0 ? inf[c].first / sum : (c == 0 ? 1.0f : 0.0f);
                    }
                }
            }
            // Settings: the source piece's, else defaults.
            const tr::GeoFilePiece* sp = sourcePiece >= 0 ? &source->pieces[size_t(sourcePiece)] : nullptr;
            if (sp && primIndex == 0) sourcePiecesSeen.insert(sourcePiece);
            const MaterialSetup m = material(integer(prim.member("material")), skinned);
            std::vector<tr::VertexElement> layout;
            if (rp && rp->member("layout") && sp && sp->skinned == skinned) {
                for (const JsonValue& e : rp->member("layout")->items)
                    if (e.items.size() == 4) {
                        tr::VertexElement el;
                        el.type = uint8_t(e.items[0].number());
                        el.method = uint8_t(e.items[1].number());
                        el.usage = uint8_t(e.items[2].number());
                        el.usageIndex = uint8_t(e.items[3].number());
                        layout.push_back(el);
                    }
            }
            const std::string effectLower = lower(m.effect.text);
            const bool needsTangents = effectLower.rfind("paldiff", 0) == 0 || effectLower.rfind("trhair", 0) == 0 || effectLower.rfind("trrefract", 0) == 0;
            if (layout.empty()) layout = defaultLayout(skinned, needsTangents, std::max(uvSets, needsTangents ? 2u : 1u));
            if (layoutHas(layout, 0, 1) && !centre)
                warn("mesh " + text(mesh.member("name")) + ": a billboard piece without _POSITION1 (sprite centres) - the sprites collapse");
            if (layoutHas(layout, 6, 0)) {
                std::vector<tr::Vec3> p(verts.size()), n(verts.size()), t, b;
                std::vector<tr::Vec2> u(verts.size());
                for (size_t i = 0; i < verts.size(); ++i) {
                    p[i] = verts[i].position;
                    n[i] = verts[i].normal;
                    u[i] = verts[i].uv[0];
                }
                computeTangentFrames(p, n, u, indices, t, b);
                for (size_t i = 0; i < verts.size(); ++i) {
                    verts[i].tangent = t[i];
                    verts[i].binormal = b[i];
                }
            }

            // Split into pieces of at most 28 palette bones (the largest shipped palette) and 65,535 vertices (16-bit indices).
            const size_t maxPalette = 28;
            size_t triangle = 0;
            const size_t triangles = indices.size() / 3;
            while (triangle < triangles) {
                std::vector<uint32_t> remap(verts.size(), UINT32_MAX);
                std::vector<BuiltVertex> pv;
                std::vector<uint32_t> pi;
                std::vector<uint32_t> palette;                 // skeleton bone per palette slot
                std::map<uint32_t, uint32_t> slotOf;
                for (; triangle < triangles; ++triangle) {
                    // Would this triangle overflow the palette or the vertex count?
                    std::set<uint32_t> newBones;
                    size_t newVerts = 0;
                    for (int c = 0; c < 3; ++c) {
                        const uint32_t i = indices[triangle * 3 + size_t(c)];
                        if (i >= verts.size()) continue;
                        if (remap[i] == UINT32_MAX) ++newVerts;
                        if (skinned)
                            for (int w = 0; w < 3; ++w)
                                if (verts[i].weights[w] > 0 && !slotOf.count(verts[i].bones[w])) newBones.insert(verts[i].bones[w]);
                    }
                    if (!pi.empty() && (palette.size() + newBones.size() > maxPalette || pv.size() + newVerts > 65535)) break;
                    for (int c = 0; c < 3; ++c) {
                        const uint32_t i = indices[triangle * 3 + size_t(c)];
                        if (i >= verts.size()) {
                            pi.push_back(0);
                            continue;
                        }
                        if (remap[i] == UINT32_MAX) {
                            BuiltVertex v = verts[i];
                            if (skinned)
                                for (int w = 0; w < 3; ++w) {
                                    if (v.weights[w] <= 0) {
                                        v.bones[w] = 0;
                                        continue;
                                    }
                                    auto it = slotOf.find(v.bones[w]);
                                    if (it == slotOf.end()) {
                                        it = slotOf.emplace(v.bones[w], uint32_t(palette.size())).first;
                                        palette.push_back(v.bones[w]);
                                    }
                                    v.bones[w] = it->second;
                                }
                            remap[i] = uint32_t(pv.size());
                            pv.push_back(v);
                        }
                        pi.push_back(remap[i]);
                    }
                }
                if (pi.empty()) break;

                tr::GeoFilePiece piece;
                if (sp) {
                    piece.header = sp->header;
                    piece.gsknHeader = sp->gsknHeader;
                    piece.efctHeader = sp->efctHeader;
                    piece.hasMaterial = sp->hasMaterial;
                    piece.usdaHeader = sp->usdaHeader;
                    piece.primitiveType = sp->primitiveType;
                }
                if (piece.header.version < 12) piece.header.version = 12;   // rebuilt pieces are written in the current layout
                piece.skinned = skinned;
                piece.efctHeader = m.efctHeader;
                piece.effect = m.effect;
                piece.parameters = m.parameters;
                piece.indices.values = pi;
                piece.vertices = encodeVertices(layout, pv, sp ? &sp->vertices : nullptr);
                std::vector<tr::Vec3> pts;
                for (const BuiltVertex& v : pv) pts.push_back(v.position);
                piece.bounds = boundsOf(pts);
                // A static piece outside every bone is in model space: it keeps the bone name it had (empty or one not in the
                // skeleton), or goes on bone 0 when new (identity in the new-model case).
                if (skinned) piece.boneName = sp ? sp->boneName : std::string();
                else if (pieceBone >= 0) piece.boneName = out.bones[size_t(pieceBone)].name;
                else piece.boneName = rp ? text(rp->member("bone")) : out.bones[0].name;
                if (rp) {
                    piece.group = text(rp->member("group"));
                    piece.name = text(rp->member("name"));
                    piece.swapId = uint32_t(integer(rp->member("swapId"), 0));
                    piece.unknown0 = uint32_t(integer(rp->member("unknown0"), 0));
                    if (const JsonValue* ud = rp->member("userData"))
                        for (const JsonValue& pair : ud->items)
                            if (pair.items.size() == 2) piece.userData.push_back(tr::GeoParameter{pair.items[0].text, pair.items[1].text});
                } else {
                    piece.name = text(mesh.member("name"));
                    piece.group = piece.name;
                }
                if (skinned && piece.boneName.empty() && !palette.empty()) piece.boneName = out.bones[palette[0]].name;
                piece.bonesPerVertex = skinned ? 3 : 0;
                piece.triangleCount = uint32_t(pi.size() / 3);
                for (uint32_t b : palette) piece.skinBones.push_back(out.bones[b].name);
                // An unchanged source piece is kept as it was (its direction-sorted index lists stay valid).
                if (sp && sp->vertices.data == piece.vertices.data && sp->indices.values == piece.indices.values && sp->skinBones == piece.skinBones &&
                    triangle >= triangles && pi.size() == indices.size()) {
                    tr::GeoFilePiece kept = *sp;
                    kept.parameters = piece.parameters;
                    builtPieces.emplace_back(sourcePiece, std::move(kept));
                } else {
                    builtPieces.emplace_back(primIndex == 0 ? sourcePiece : -1, std::move(piece));
                }
            }
        }
    }
    // Pieces the export left out (low detail) come back from the source.
    if (source && rasaModel)
        if (const JsonValue* omitted = rasaModel->member("omitted"))
            for (const JsonValue& o : omitted->items) {
                const int64_t k = integer(&o);
                if (k >= 0 && size_t(k) < source->pieces.size() && !sourcePiecesSeen.count(k)) {
                    tr::GeoFilePiece kept = source->pieces[size_t(k)];
                    for (tr::GeoFileParameter& p : kept.parameters)
                        if ((p.type == 5 || p.type == 8))
                            if (auto it = textureRename.find(lower(p.asset.text)); it != textureRename.end()) p.asset.text = it->second;
                    builtPieces.emplace_back(k, std::move(kept));
                }
            }
    // Source order first, new pieces after.
    std::stable_sort(builtPieces.begin(), builtPieces.end(), [](const auto& a, const auto& b) {
        const int64_t ka = a.first < 0 ? INT64_MAX : a.first, kb = b.first < 0 ? INT64_MAX : b.first;
        return ka < kb;
    });
    for (auto& p : builtPieces) out.pieces.push_back(std::move(p.second));
    if (out.pieces.empty()) warn("the model has no pieces");

    // ---- collision ----
    std::string collisionNote;
    // A volume's radius: the bounding sphere about the bone's origin (verified on every shipped volume: BVWS = the farthest vertex,
    // 3,712 / 3,712; BVBX = |centre| + |half extents|, 46 / 46; the one BVSP = |centre| + its radius).
    auto finishVolume = [](tr::GeoBoneVolume& v) {
        auto length = [](const tr::Vec3& p) { return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z); };
        if (v.kind == tr::fourcc("BVWS")) {
            v.radius = 0;
            for (const tr::Vec3& p : v.vertices) v.radius = std::max(v.radius, length(p));
            v.header.version = 2;
            v.tail = {0};
        } else if (v.kind == tr::fourcc("BVBX")) {
            v.radius = length(v.centre) + length(v.halfExtents);
            v.header.version = 1;
        } else {
            v.radius = length(v.centre) + v.sphereRadius;
            v.header.version = 1;
        }
    };
    // A surface from triangles, duplicate positions merged; false when it needs more than 16-bit indices.
    auto surfaceVolume = [](const std::vector<tr::Vec3>& points, const std::vector<uint32_t>& indices, tr::GeoBoneVolume& v) {
        v.kind = tr::fourcc("BVWS");
        std::map<std::tuple<float, float, float>, uint16_t> welded;
        for (uint32_t i : indices) {
            if (i >= points.size()) continue;
            const tr::Vec3& p = points[i];
            const auto key = std::make_tuple(p.x, p.y, p.z);
            auto it = welded.find(key);
            if (it == welded.end()) {
                if (v.vertices.size() >= 65535) return false;
                it = welded.emplace(key, uint16_t(v.vertices.size())).first;
                v.vertices.push_back(p);
            }
            v.indices.push_back(it->second);
        }
        // Drop triangles that collapsed to a line or a point.
        std::vector<uint16_t> kept;
        for (size_t k = 0; k + 2 < v.indices.size(); k += 3)
            if (v.indices[k] != v.indices[k + 1] && v.indices[k + 1] != v.indices[k + 2] && v.indices[k] != v.indices[k + 2])
                kept.insert(kept.end(), {v.indices[k], v.indices[k + 1], v.indices[k + 2]});
        v.indices = kept;
        return !v.indices.empty();
    };
    {
        std::map<size_t, std::vector<std::pair<std::string, std::pair<std::vector<tr::Vec3>, std::vector<uint32_t>>>>> perBone;   // bone -> (kind, mesh)
        size_t collisionMeshes = 0;
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
            const JsonValue* rc = collisionData(nodeIndex);
            if (!rc) continue;
            ++collisionMeshes;
            const JsonValue& node = nodes->items[nodeIndex];
            const JsonValue& mesh = meshes->items[size_t(integer(node.member("mesh")))];
            int64_t bone = -1;
            if (auto it = boneIndex.find(lower(text(rc->member("bone")))); it != boneIndex.end()) bone = int64_t(it->second);
            if (bone < 0) bone = boneForNode(int64_t(nodeIndex));
            if (bone < 0) bone = 0;
            const M4 toBone = relativeToBone(int64_t(nodeIndex), bone);
            std::vector<tr::Vec3> points;
            std::vector<uint32_t> indices;
            if (const JsonValue* primitives = mesh.member("primitives"))
                for (const JsonValue& prim : primitives->items) {
                    if (integer(prim.member("mode"), 4) != 4) continue;
                    const JsonValue* attributes = prim.member("attributes");
                    if (!attributes || !attributes->member("POSITION")) continue;
                    const Accessor pos = readAccessor(g, integer(attributes->member("POSITION")));
                    const uint32_t base = uint32_t(points.size());
                    for (size_t i = 0; i < pos.count; ++i)
                        points.push_back(mirror(transformPoint(toBone, tr::Vec3{float(pos.get(i, 0)), float(pos.get(i, 1)), float(pos.get(i, 2))})));
                    const size_t first = indices.size();
                    if (prim.member("indices")) {
                        const Accessor ia = readAccessor(g, integer(prim.member("indices")));
                        for (size_t k = 0; k < ia.count; ++k) indices.push_back(base + uint32_t(ia.get(k, 0)));
                    } else {
                        for (size_t k = 0; k < pos.count; ++k) indices.push_back(base + uint32_t(k));
                    }
                    flipWinding(indices, first);
                }
            std::string kind = text(rc->member("kind"));
            if (kind.empty()) kind = "surface";
            perBone[size_t(bone)].push_back({kind, {std::move(points), std::move(indices)}});
        }
        auto boundsOfPoints = [](const std::vector<tr::Vec3>& points, tr::Vec3& lo, tr::Vec3& hi) {
            lo = hi = points.empty() ? tr::Vec3{} : points[0];
            for (const tr::Vec3& p : points) {
                lo = tr::Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = tr::Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        };
        auto encodeForBone = [&](size_t bone, std::vector<tr::GeoBoneVolume> volumes) {
            tr::GeoFileBone& b = out.bones[bone];
            if (!b.hasInverseBind && !volumes.empty()) {
                warn("bone " + b.name + " has no inverse bind matrix: its collision is not written");
                return;
            }
            for (tr::GeoBoneVolume& v : volumes) finishVolume(v);
            b.boundingVolume = tr::writeBoneVolumes(volumes);
        };
        if (collisionMeshes > 0) {
            // The file defines all collision: every bone gets what the file gives it, or none.
            size_t written = 0;
            for (size_t b = 0; b < out.bones.size(); ++b) {
                std::vector<tr::GeoBoneVolume> volumes;
                auto it = perBone.find(b);
                if (it != perBone.end()) {
                    // One box or sphere stays one; anything else becomes a single walkable surface (one volume per bone, as shipped).
                    if (it->second.size() == 1 && (it->second[0].first == "box" || it->second[0].first == "sphere") && !it->second[0].second.first.empty()) {
                        tr::GeoBoneVolume v;
                        tr::Vec3 lo, hi;
                        boundsOfPoints(it->second[0].second.first, lo, hi);
                        v.centre = tr::Vec3{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
                        if (it->second[0].first == "box") {
                            v.kind = tr::fourcc("BVBX");
                            v.halfExtents = tr::Vec3{(hi.x - lo.x) / 2, (hi.y - lo.y) / 2, (hi.z - lo.z) / 2};
                        } else {
                            v.kind = tr::fourcc("BVSP");
                            for (const tr::Vec3& p : it->second[0].second.first) {
                                const float dx = p.x - v.centre.x, dy = p.y - v.centre.y, dz = p.z - v.centre.z;
                                v.sphereRadius = std::max(v.sphereRadius, std::sqrt(dx * dx + dy * dy + dz * dz));
                            }
                        }
                        volumes.push_back(std::move(v));
                    } else {
                        std::vector<tr::Vec3> points;
                        std::vector<uint32_t> indices;
                        for (const auto& m : it->second) {
                            const uint32_t base = uint32_t(points.size());
                            points.insert(points.end(), m.second.first.begin(), m.second.first.end());
                            for (uint32_t i : m.second.second) indices.push_back(base + i);
                        }
                        tr::GeoBoneVolume v;
                        if (surfaceVolume(points, indices, v)) volumes.push_back(std::move(v));
                        else warn("bone " + out.bones[b].name + ": its collision mesh is empty or over 65,535 vertices - left out");
                    }
                }
                if (!volumes.empty()) ++written;
                encodeForBone(b, std::move(volumes));
            }
            collisionNote = "collision from the file on " + std::to_string(written) + " bone(s)";
        } else if (!source && options.generateCollision) {
            // A new model: one walkable surface from its opaque pieces, in bone 0's bind frame.
            const tr::GeoModel decoded = tr::readGeo(tr::writeGeoFile(out, true));
            std::vector<tr::Vec3> points;
            std::vector<uint32_t> indices;
            for (const tr::GeoPiece& piece : decoded.pieces) {
                const std::string effect = lower(piece.effectName);
                std::string phase;
                for (const tr::GeoParameter& p : piece.parameters)
                    if (p.key == "Phase") phase = lower(p.value);
                if (effect.rfind("tradditive", 0) == 0 || effect.rfind("trrefract", 0) == 0 || effect.rfind("trwater", 0) == 0 ||
                    effect.rfind("trsphbillboard", 0) == 0 || phase == "additive" || phase == "translucent" || piece.lod > 0 || piece.swapId == 99)
                    continue;
                M4 toModel;
                if (!piece.skinned)
                    if (auto it = boneIndex.find(lower(piece.boneName)); it != boneIndex.end()) toModel = boneBindWorld[it->second];
                const M4 toBone = inverse(boneBindWorld[0]) * toModel;
                const uint32_t base = uint32_t(points.size());
                for (const tr::GeoVertex& v : piece.vertices) points.push_back(mirror(transformPoint(toBone, mirror(v.position))));
                for (uint32_t i : piece.indices) indices.push_back(base + i);
            }
            tr::GeoBoneVolume v;
            if (surfaceVolume(points, indices, v)) {
                encodeForBone(0, {v});
                collisionNote = "collision generated: " + std::to_string(v.indices.size() / 3) + " triangles on bone " + out.bones[0].name;
            } else if (!points.empty()) {
                warn("collision not generated: the visible pieces have over 65,535 distinct vertices (add a simpler \"collision\" mesh)");
            }
        } else if (source) {
            size_t lost = 0;
            for (const tr::GeoFileBone& b : out.bones)
                for (const tr::GeoFileBone& sb : source->bones)
                    if (lower(sb.name) == lower(b.name) && !sb.boundingVolume.empty() && b.boundingVolume.empty()) ++lost;
            if (lost) warn(std::to_string(lost) + " changed bone(s) lost their collision (the file has no collision meshes)");
        }
        // What the client collides with (tabula_rasa.exe, traced 2026-10-19): WalkableSurfaceManager::AddWalkableSurfaces (FUN_00925590)
        // takes only bone volumes of type 5 = phyBVWalkableSurface (BVWS; box 2, sphere 1 are not walked on or blocked by) and needs
        // the bone's WSName - the property group whose id is the bone's index (FUN_00940840) - else "Walkable Surface on entity has no
        // name!" and none of the entity's surfaces are added. All 3,712 shipped BVWS carry one ("collision" in every model); give a
        // bone that has a surface and no name one.
        size_t named = 0, boxes = 0;
        for (size_t b = 0; b < out.bones.size(); ++b) {
            if (out.bones[b].boundingVolume.empty()) continue;
            bool surface = false;
            try {
                for (const tr::GeoBoneVolume& v : tr::readBoneVolumes(out.bones[b].boundingVolume)) {
                    surface = surface || v.kind == tr::fourcc("BVWS");
                    boxes += v.kind == tr::fourcc("BVBX") || v.kind == tr::fourcc("BVSP");
                }
            } catch (const std::exception&) { continue; }
            if (!surface) continue;
            tr::GeoPropertyGroup* group = nullptr;
            bool hasName = false;
            for (tr::GeoPropertyGroup& g : out.properties)
                if (g.id == int32_t(b)) {
                    group = group ? group : &g;
                    for (const tr::GeoParameter& p : g.pairs) hasName = hasName || p.key == "WSName";
                }
            if (hasName) continue;
            if (!group) {
                out.properties.push_back(tr::GeoPropertyGroup{int32_t(b), {}});
                group = &out.properties.back();
            }
            group->pairs.push_back(tr::GeoParameter{"WSName", "collision"});
            ++named;
        }
        if (named) collisionNote += (collisionNote.empty() ? "" : ", ") + std::to_string(named) + " collision bone(s) given WSName \"collision\"";
        if (boxes)
            warn(std::to_string(boxes) + " box / sphere collision volume(s): the client does not walk on or collide with these on a map "
                 "object (only walkable surfaces, BVWS, count) - make the collision a mesh (surface / convex hull) to block the player");
    }

    // ---- contact points ----
    std::set<uint32_t> usedIds;
    std::vector<std::pair<int64_t, tr::GeoFileContactPoint>> points;
    for (size_t k = 0; k < nodeCount; ++k) {
        const JsonValue& n = nodes->items[k];
        if (n.member("mesh") || jointSet.count(int64_t(k))) continue;
        const JsonValue* cj = extrasMember(n, "rasaContactPoint");
        const std::string name = text(n.member("name"));
        const std::string prefix = lower(name.substr(0, 4));
        const bool named = prefix == "hps_" || prefix == "hpp_" || prefix == "pps_" || prefix == "ppp_" || prefix == "sps_" || prefix == "spp_" ||
                           prefix == "dps_" || prefix == "dpp_";
        if (!cj && !named) continue;
        tr::GeoFileContactPoint cp;
        if (cj) {
            cp.point.id = uint32_t(integer(cj->member("id"), 0));
            if (const JsonValue* values = cj->member("values"))
                for (const JsonValue& pair : values->items)
                    if (pair.items.size() == 2) cp.point.values.push_back(tr::GeoParameter{pair.items[0].text, pair.items[1].text});
        } else {
            cp.point.values.push_back(tr::GeoParameter{"cpName", name});
            cp.point.id = UINT32_MAX;
        }
        const int64_t bone = boneForNode(int64_t(k));
        cp.point.bone = int32_t(bone);
        const Trs l = decompose(relativeToBone(int64_t(k), bone));
        cp.point.position = mirror(l.translation);
        cp.point.rotation = mirror(l.rotation);
        if (cp.point.id != UINT32_MAX) usedIds.insert(cp.point.id);
        points.emplace_back(cj ? int64_t(cp.point.id) : INT64_MAX, std::move(cp));
    }
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    uint32_t nextId = 0;
    for (auto& p : points) {
        if (p.second.point.id == UINT32_MAX) {
            while (usedIds.count(nextId)) ++nextId;
            p.second.point.id = nextId;
            usedIds.insert(nextId);
        }
        out.contactPoints.push_back(std::move(p.second));
    }
    if (source && out.contactPoints.size() < source->contactPoints.size())
        warn(std::to_string(source->contactPoints.size() - out.contactPoints.size()) + " contact point(s) of the source are not in the file");

    // ---- model bounds: every piece in model space (static pieces through their bone's bind world) ----
    {
        std::vector<tr::Vec3> corners;
        for (const tr::GeoFilePiece& piece : out.pieces) {
            if (piece.bounds.isEmpty || !piece.bounds.raw.empty()) continue;
            M4 toModel;
            if (!piece.skinned)
                if (auto it = boneIndex.find(lower(piece.boneName)); it != boneIndex.end()) toModel = boneBindWorld[it->second];
            for (int c = 0; c < 8; ++c) {
                const tr::Vec3 p{(c & 1) ? piece.bounds.max.x : piece.bounds.min.x, (c & 2) ? piece.bounds.max.y : piece.bounds.min.y,
                                 (c & 4) ? piece.bounds.max.z : piece.bounds.min.z};
                corners.push_back(mirror(transformPoint(toModel, mirror(p))));
            }
        }
        out.bounds = boundsOf(corners);
        if (source) out.bounds.header = source->bounds.header;
    }

    {
        ImportedFile f;
        f.name = modelName;
        f.data = tr::writeGeoFile(out, true);
        std::ostringstream note;
        note << (source ? "model (from " + sourceName + ")" : std::string("new model")) << ": " << out.pieces.size() << " pieces, " << out.bones.size()
             << " bones (" << keptBones << " kept, " << changedBones << " changed, " << newBones << " new), " << out.contactPoints.size() << " contact points";
        if (!collisionNote.empty()) note << "; " << collisionNote;
        f.note = note.str();
        // Check: the written file reads back.
        try {
            (void)tr::readGeo(f.data);
        } catch (const std::exception& e) {
            warn(std::string("the written model does not read back: ") + e.what());
        }
        result.files.insert(result.files.begin(), std::move(f));
    }

    // ---- animations ----
    // What the source .anm had: track order, key kinds, flags, which components each bone track stores, and the tracks glTF cannot
    // carry (texture scroll, morph). From the animation's rasaAnimation (on the animation, else in the root's rasaModel.animations
    // by name - Blender drops animation extras), else from the source .anm of that name in the archives.
    struct SourceTrack {
        uint32_t keyTag = 0;
        uint32_t flags = 0;
        bool scale = false, rotation = false, translation = false;
    };
    struct SourceAnim {
        std::string file;
        uint32_t version = 4;
        std::vector<std::string> order;
        std::map<std::string, SourceTrack> tracks;          // lower bone name
        std::vector<tr::AnimFileTrack> keptTracks;          // material and other tracks, as they go into the file
    };
    auto keyComponents = [](uint32_t tag, SourceTrack& t) {
        const std::string k = tr::tagName(tag);
        t.scale = k == "KEYS" || k == "KEST" || k == "KESR" || k == "KSRT" || k == "XKSR" || k == "XSRT";
        t.rotation = k == "KEYR" || k == "KERT" || k == "KESR" || k == "KSRT" || k == "XKER" || k == "XKRT" || k == "XKSR" || k == "XSRT";
        t.translation = k == "KEYT" || k == "KERT" || k == "KEST" || k == "KSRT" || k == "XKRT" || k == "XSRT";
    };
    auto sourceFromExtras = [&](const JsonValue& ra, SourceAnim& s) {
        s.file = text(ra.member("source"));
        s.version = uint32_t(integer(ra.member("animVersion"), 4));
        if (const JsonValue* tracks = ra.member("tracks"))
            for (const JsonValue& t : tracks->items) {
                SourceTrack st;
                const std::string kind = text(t.member("keys"));
                if (kind.size() == 4)
                    st.keyTag = (uint32_t(uint8_t(kind[0])) << 24) | (uint32_t(uint8_t(kind[1])) << 16) | (uint32_t(uint8_t(kind[2])) << 8) | uint8_t(kind[3]);
                st.flags = uint32_t(integer(t.member("flags"), 0));
                keyComponents(st.keyTag, st);
                const std::string bone = text(t.member("bone"));
                s.tracks[lower(bone)] = st;
                s.order.push_back(bone);
            }
        if (const JsonValue* mts = ra.member("materialTracks"))
            for (const JsonValue& mt : mts->items) {
                tr::AnimFileTrack track;
                track.target = tr::fourcc("EFCT");
                track.name = text(mt.member("group"));
                track.parameter = text(mt.member("parameter"));
                std::vector<tr::MaterialKey> keys;
                if (const JsonValue* kj = mt.member("keys"))
                    for (const JsonValue& k : kj->items)
                        if (k.items.size() == 8) {
                            tr::MaterialKey key;
                            key.scaleU = float(k.items[0].number());
                            key.scaleV = float(k.items[1].number());
                            key.rotation = tr::Vec3{float(k.items[2].number()), float(k.items[3].number()), float(k.items[4].number())};
                            key.translateU = float(k.items[5].number());
                            key.translateV = float(k.items[6].number());
                            key.time = float(k.items[7].number());
                            keys.push_back(key);
                        }
                tr::encodeMaterialKeys(track, keys);
                s.keptTracks.push_back(std::move(track));
            }
        if (const JsonValue* ots = ra.member("otherTracks"))
            for (const JsonValue& ot : ots->items) {
                tr::AnimFileTrack track;
                track.preTag = uint32_t(integer(ot.member("preTag"), 0));
                track.target = uint32_t(integer(ot.member("target"), 0));
                track.header.version = uint32_t(integer(ot.member("version"), 3));
                track.byte0 = uint8_t(integer(ot.member("byte0"), 0));
                track.raw = unbase64(text(ot.member("raw")));
                s.keptTracks.push_back(std::move(track));
            }
    };
    auto sourceFromFile = [&](const std::string& file, SourceAnim& s) -> bool {
        tr::Bytes data;
        if (!assets.tryRead(file, data)) return false;
        try {
            const tr::AnimFile anim = tr::readAnimFile(data);
            s.file = file;
            s.version = anim.animHeader.version;
            for (const tr::AnimFileTrack& t : anim.tracks) {
                if (t.target == tr::fourcc("SKEL")) {
                    SourceTrack st;
                    st.keyTag = t.keyTag;
                    st.flags = t.flags;
                    keyComponents(t.keyTag, st);
                    s.tracks[lower(t.name)] = st;
                    s.order.push_back(t.name);
                } else {
                    s.keptTracks.push_back(t);
                }
            }
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    const JsonValue* modelAnimations = rasaModel ? rasaModel->member("animations") : nullptr;
    if (const JsonValue* animations = g.list("animations")) {
        for (const JsonValue& a : animations->items) {
            const std::string animName = text(a.member("name"));
            SourceAnim src;
            const JsonValue* ra = extrasMember(a, "rasaAnimation");
            if (!ra && modelAnimations)
                for (const JsonValue& m : modelAnimations->items)
                    if (text(m.member("name")) == animName) ra = &m;
            if (ra) sourceFromExtras(*ra, src);
            std::string file = !src.file.empty() ? src.file : animName + ".anm";
            if (file == ".anm") file = stem(modelName) + ".anm";
            if (!ra && sourceFromFile(file, src)) warn("animation " + animName + ": no RasaEdit data in the file - track settings and texture tracks taken from " + file);
            // Channels per bone.
            struct Channel {
                std::vector<double> times;
                Accessor values;
                bool cubic = false;
            };
            std::map<size_t, std::map<std::string, Channel>> perBone;
            const JsonValue* samplers = a.member("samplers");
            if (const JsonValue* channels = a.member("channels"))
                for (const JsonValue& c : channels->items) {
                    const JsonValue* target = c.member("target");
                    if (!target || !samplers) continue;
                    const int64_t node = integer(target->member("node"));
                    const std::string pathName = text(target->member("path"));
                    if (node < 0 || (pathName != "translation" && pathName != "rotation" && pathName != "scale")) continue;
                    const auto bone = boneIndex.find(lower(nodeName(node)));
                    if (bone == boneIndex.end() || !jointSet.count(node)) continue;
                    const JsonValue& s = samplers->items.at(size_t(integer(c.member("sampler"), 0)));
                    Channel ch;
                    const Accessor input = readAccessor(g, integer(s.member("input")));
                    for (size_t k = 0; k < input.count; ++k) ch.times.push_back(input.get(k, 0));
                    ch.values = readAccessor(g, integer(s.member("output")));
                    ch.cubic = text(s.member("interpolation")) == "CUBICSPLINE";
                    perBone[bone->second][pathName] = std::move(ch);
                }
            std::map<uint32_t, size_t> flagCounts;
            for (const auto& st : src.tracks) ++flagCounts[st.second.flags];
            uint32_t commonFlags = 0;
            size_t best = 0;
            for (const auto& fc : flagCounts)
                if (fc.second > best) {
                    best = fc.second;
                    commonFlags = fc.first;
                }

            tr::AnimFile anim;
            anim.header = {'C', 'H', 'N', 'K', 'B', 'L', 'X', 'X'};
            anim.animHeader.version = src.version;
            std::vector<size_t> boneOrder;
            for (const std::string& name : src.order)
                if (auto it = boneIndex.find(lower(name)); it != boneIndex.end() && perBone.count(it->second)) boneOrder.push_back(it->second);
            for (const auto& pb : perBone)
                if (std::find(boneOrder.begin(), boneOrder.end(), pb.first) == boneOrder.end()) boneOrder.push_back(pb.first);
            for (size_t b : boneOrder) {
                const auto& chans = perBone[b];
                std::set<double> timeSet;
                for (const auto& c : chans) timeSet.insert(c.second.times.begin(), c.second.times.end());
                const std::vector<double> times(timeSet.begin(), timeSet.end());
                if (times.empty()) continue;
                const tr::GeoFileBone& bone = out.bones[b];
                std::vector<tr::AnimKey> keys(times.size());
                bool varyT = false, varyR = false, varyS = false;
                for (size_t k = 0; k < times.size(); ++k) {
                    tr::AnimKey& key = keys[k];
                    key.time = float(times[k]);
                    key.translation = bone.translation;
                    key.rotation = bone.rotation;
                    key.scale = bone.scale;
                    if (auto it = chans.find("translation"); it != chans.end()) {
                        const auto v = sampleChannel(it->second.times, it->second.values, it->second.cubic, 3, times[k], false);
                        key.translation = mirror(tr::Vec3{float(v[0]), float(v[1]), float(v[2])});
                    }
                    if (auto it = chans.find("rotation"); it != chans.end()) {
                        const auto v = sampleChannel(it->second.times, it->second.values, it->second.cubic, 4, times[k], true);
                        key.rotation = mirror(tr::Quat{float(v[0]), float(v[1]), float(v[2]), float(v[3])});
                    }
                    if (auto it = chans.find("scale"); it != chans.end()) {
                        const auto v = sampleChannel(it->second.times, it->second.values, it->second.cubic, 3, times[k], false);
                        key.scale = tr::Vec3{float(v[0]), float(v[1]), float(v[2])};
                    }
                    // A component goes in the track when the source track stored it, or it is not the bind value (the export
                    // writes the bind into components the source track left out, so they read back as left out).
                    auto differs = [](float x, float y, float tol) { return std::fabs(x - y) > tol; };
                    varyT = varyT || differs(key.translation.x, bone.translation.x, 1e-4f) || differs(key.translation.y, bone.translation.y, 1e-4f) ||
                            differs(key.translation.z, bone.translation.z, 1e-4f);
                    const float dot = std::fabs(key.rotation.x * bone.rotation.x + key.rotation.y * bone.rotation.y + key.rotation.z * bone.rotation.z +
                                                key.rotation.w * bone.rotation.w);
                    varyR = varyR || dot < 1.0f - 1e-6f;
                    varyS = varyS || differs(key.scale.x, bone.scale.x, 1e-4f) || differs(key.scale.y, bone.scale.y, 1e-4f) || differs(key.scale.z, bone.scale.z, 1e-4f);
                }
                const auto st = src.tracks.find(lower(bone.name));
                const bool hadTrack = st != src.tracks.end();
                const bool useT = varyT || (hadTrack && st->second.translation);
                const bool useR = varyR || (hadTrack && st->second.rotation);
                const bool useS = varyS || (hadTrack && st->second.scale);
                if (!useT && !useR && !useS) continue;     // the bone holds its bind: no track (as in the source)
                const bool compressed = hadTrack && (st->second.keyTag >> 24) == 'X';
                std::string kind;
                if (compressed && useR) kind = useS ? (useT ? "XSRT" : "XKSR") : (useT ? "XKRT" : "XKER");
                else kind = std::string("K") + (useS && useR && useT ? "SRT" : useS && useR ? "ESR" : useS && useT ? "EST" : useR && useT ? "ERT" : useS ? "EYS" : useR ? "EYR" : "EYT");
                // "K" + "EYS" / "EYR" / "EYT" spell KEYS / KEYR / KEYT.
                const uint32_t tag = (uint32_t(uint8_t(kind[0])) << 24) | (uint32_t(uint8_t(kind[1])) << 16) | (uint32_t(uint8_t(kind[2])) << 8) | uint8_t(kind[3]);
                tr::AnimFileTrack track;
                track.target = tr::fourcc("SKEL");
                track.name = bone.name;
                track.flags = hadTrack ? st->second.flags : commonFlags;
                tr::encodeAnimKeys(track, tag, keys);
                anim.tracks.push_back(std::move(track));
            }
            for (const tr::AnimFileTrack& t : src.keptTracks) anim.tracks.push_back(t);
            if (anim.tracks.empty()) {
                warn("animation " + file + ": no tracks for this model's bones - left out");
                continue;
            }
            ImportedFile f;
            f.name = file;
            f.data = tr::writeAnimFile(anim);
            f.note = "animation: " + std::to_string(anim.tracks.size()) + " tracks";
            try {
                (void)tr::readAnimation(f.data);
            } catch (const std::exception& e) {
                warn("animation " + file + " does not read back: " + e.what());
            }
            result.files.push_back(std::move(f));
            // Its markers ([type, ms, ...] from the export, edited as timeline markers in Blender): <anim>.mkr when they differ from the game's.
            if (const JsonValue* markers = ra ? ra->member("markers") : nullptr) {
                tr::AnimationMarkers m;
                m.version = 1;
                for (const JsonValue& e : markers->items) {
                    if (e.items.size() < 2) continue;
                    int64_t type = 0, ms = 0;
                    e.items[0].integer(type);
                    e.items[1].integer(ms);
                    if (type < 0 || type > 255 || ms < 0) {
                        warn("animation " + file + ": marker (" + std::to_string(type) + ", " + std::to_string(ms) + " ms) left out");
                        continue;
                    }
                    m.markers.push_back(tr::AnimMarker{uint8_t(type), uint32_t(ms)});
                }
                const std::string mkrName = file.substr(0, file.size() - 4) + ".mkr";
                tr::Bytes original;
                const bool had = assets.tryRead(mkrName, original);
                tr::Bytes header;
                if (had && original.size() >= 8) header.assign(original.begin(), original.begin() + 8);
                const tr::Bytes data = tr::writeAnimationMarkers(m, header);
                if ((had && data != original) || (!had && !m.markers.empty())) {
                    ImportedFile mf;
                    mf.name = mkrName;
                    mf.data = data;
                    mf.note = "animation markers: " + std::to_string(m.markers.size());
                    result.files.push_back(std::move(mf));
                }
            }
        }
    }
    // A model plays the animation its `anim` property names (CLAUDE.md "A model names its animation"). A new model with animations
    // and no such property gets one naming the first, in property group 0 (the group holding it in 760 of 1,526 shipped models; the
    // others use 1, 2, 3 or -1 - what the id means is not traced).
    bool hasAnim = false;
    for (const tr::GeoPropertyGroup& group : out.properties)
        for (const tr::GeoParameter& p : group.pairs) hasAnim = hasAnim || lower(p.key) == "anim";
    if (!hasAnim && !rasaModel)
        for (const ImportedFile& f : result.files)
            if (lower(f.name).size() > 4 && lower(f.name).compare(lower(f.name).size() - 4, 4, ".anm") == 0) {
                tr::GeoPropertyGroup group;
                group.id = 0;
                group.pairs.push_back(tr::GeoParameter{"anim", f.name});
                out.properties.push_back(std::move(group));
                result.files[0].data = tr::writeGeoFile(out, true);
                result.files[0].note += "; plays " + f.name;
                break;
            }
    return result;
}

// ---- patch archive ----

std::string patchArchivePath(const std::string& gameRoot) { return (fs::u8path(gameRoot) / "data" / "rasaedit_patch.glm").string(); }

std::string addToPatchArchive(const std::string& gameRoot, const std::vector<ImportedFile>& files, tr::AssetLibrary* library) {
    const std::string path = patchArchivePath(gameRoot);
    std::vector<tr::GlmSource> sources;
    std::set<std::string> names;
    for (const ImportedFile& f : files) {
        if (names.count(lower(f.name))) continue;
        names.insert(lower(f.name));
        tr::GlmSource s;
        s.name = f.name;
        s.data = f.data;
        s.compress = tr::glmShippedCompressed(f.name);   // stored like the shipped files (a compressed static model was ignored)
        sources.push_back(std::move(s));
    }
    size_t kept = 0;
    std::error_code ec;
    if (fs::exists(fs::u8path(path), ec)) {
        tr::GlmArchive existing(path);
        for (const tr::GlmEntry& e : existing.entries()) {
            if (names.count(lower(e.name))) continue;
            tr::GlmSource s;
            s.name = e.name;
            s.data = existing.read(e);
            s.timestamp = e.timestamp;
            s.compress = tr::glmShippedCompressed(e.name);
            sources.push_back(std::move(s));
            ++kept;
        }
    }
    const size_t added = names.size();
    if (library) library->rewriteArchive(path, [&] { tr::writeGlmArchive(path, std::move(sources)); });
    else tr::writeGlmArchive(path, std::move(sources));
    return "wrote " + path + ": " + std::to_string(added) + " file(s) added or replaced, " + std::to_string(kept) + " kept";
}

// The patch archive's [Paths] entry (GameConfig.h; the first key wins, CLAUDE.md "Which archive wins").
std::string patchArchiveKey(const std::string& gameRoot) { return gamecfg::entryKey(gameRoot, "Paths", "rasaedit_patch.glm"); }

bool patchArchiveRegistered(const std::string& gameRoot) { return !patchArchiveKey(gameRoot).empty(); }

bool patchArchiveWins(const std::string& gameRoot) { return gamecfg::entryFirst(gameRoot, "Paths", "rasaedit_patch.glm"); }

std::string registerPatchArchive(const std::string& gameRoot) {
    return gamecfg::registerFirst(gameRoot, "Paths", "AAA_RasaEdit", "data/rasaedit_patch.glm", "rasaedit_patch.glm");
}
