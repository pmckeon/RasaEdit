#include "ModelExchange.h"

#include "Json.h"
#include "assetlibrary.h"
#include "geofile.h"
#include "image.h"
#include "ModelMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>

namespace {

// ---- small helpers ----

std::string lower(std::string s) { return tr::toLowerAscii(s); }

std::string baseName(const std::string& file) {
    const size_t slash = file.find_last_of("/\\");
    std::string name = slash == std::string::npos ? file : file.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const tr::Bytes& data) {
    std::string out;
    for (size_t k = 0; k < data.size(); k += 3) {
        const uint32_t a = data[k], b = k + 1 < data.size() ? data[k + 1] : 0, c = k + 2 < data.size() ? data[k + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += k + 1 < data.size() ? kBase64[(v >> 6) & 63] : '=';
        out += k + 2 < data.size() ? kBase64[v & 63] : '=';
    }
    return out;
}

JsonValue vec(std::initializer_list<double> values) {
    JsonValue a = jsonArray();
    for (double v : values) a.push(jsonNumber(v));
    return a;
}

// Mirroring z between the game (left-handed) and glTF (right-handed).
tr::Vec3 mirror(const tr::Vec3& v) { return tr::Vec3{v.x, v.y, -v.z}; }
tr::Quat mirror(const tr::Quat& q) { return tr::Quat{-q.x, -q.y, q.z, q.w}; }

// A game row-vector matrix (row-major, translation in the last row) as a glTF column-major array, mirrored: the column-major
// array of the column-vector form equals the row-major array of the row-vector form; mirroring negates the entries with exactly
// one index on z.
std::vector<double> mirrorMatrix(const float (&m)[16]) {
    std::vector<double> out(16);
    for (int a = 0; a < 16; ++a) {
        const int col = a / 4, row = a % 4;
        out[size_t(a)] = ((row == 2) != (col == 2)) ? -double(m[a]) : double(m[a]);
    }
    return out;
}

// ---- binary buffer with accessors ----

struct Gltf {
    JsonValue root = jsonObject();
    tr::Bytes bin;

    JsonValue& array(const char* name) {
        if (JsonValue* existing = const_cast<JsonValue*>(root.member(name))) return *existing;
        return root.set(name, jsonArray());
    }

    void align() {
        while (bin.size() % 4) bin.push_back(0);
    }

    // A buffer view over bytes just appended; returns its index.
    size_t view(size_t offset, size_t length, int target) {
        JsonValue v = jsonObject();
        v.set("buffer", jsonInteger(0));
        v.set("byteOffset", jsonInteger(int64_t(offset)));
        v.set("byteLength", jsonInteger(int64_t(length)));
        if (target) v.set("target", jsonInteger(target));
        JsonValue& views = array("bufferViews");
        views.push(std::move(v));
        return views.items.size() - 1;
    }

    size_t accessor(size_t viewIndex, int componentType, size_t count, const char* type, bool normalized = false, JsonValue min = {},
                    JsonValue max = {}) {
        JsonValue a = jsonObject();
        a.set("bufferView", jsonInteger(int64_t(viewIndex)));
        a.set("componentType", jsonInteger(componentType));
        if (normalized) a.set("normalized", jsonBool(true));
        a.set("count", jsonInteger(int64_t(count)));
        a.set("type", jsonString(type));
        if (!min.isNull()) a.set("min", std::move(min));
        if (!max.isNull()) a.set("max", std::move(max));
        JsonValue& accessors = array("accessors");
        accessors.push(std::move(a));
        return accessors.items.size() - 1;
    }

    size_t floats(const std::vector<float>& values, uint32_t components, const char* type, int target, bool withBounds = false) {
        align();
        const size_t offset = bin.size();
        bin.resize(offset + values.size() * 4);
        std::memcpy(bin.data() + offset, values.data(), values.size() * 4);
        JsonValue mn, mx;
        if (withBounds && !values.empty()) {
            mn = jsonArray();
            mx = jsonArray();
            for (uint32_t c = 0; c < components; ++c) {
                float lo = values[c], hi = values[c];
                for (size_t k = c; k < values.size(); k += components) {
                    lo = std::min(lo, values[k]);
                    hi = std::max(hi, values[k]);
                }
                mn.push(jsonNumber(lo));
                mx.push(jsonNumber(hi));
            }
        }
        const size_t v = view(offset, values.size() * 4, target);
        return accessor(v, 5126, values.size() / components, type, false, std::move(mn), std::move(mx));
    }

    size_t bytes(const std::vector<uint8_t>& values, const char* type, bool normalized, int target) {
        align();
        const size_t offset = bin.size();
        bin.insert(bin.end(), values.begin(), values.end());
        const size_t v = view(offset, values.size(), target);
        return accessor(v, 5121, values.size() / 4, type, normalized);
    }

    // Triangle lists, written with each triangle's second and third index swapped: the z mirror reverses the winding test
    // (cross(b - a, c - a) . n changes sign under a reflection), and glTF's front faces are counter-clockwise - kept as stored,
    // 5,497 of npc_redshirt_f_electric_pistol_mech_v01's 5,500 triangles faced away from their normals and Blender culled the
    // outside (user report 2026-10-17). The import swaps them back, so a round trip keeps the game's order.
    size_t indices(const std::vector<uint32_t>& triangles) {
        align();
        const size_t offset = bin.size();
        std::vector<uint32_t> values = triangles;
        for (size_t k = 0; k + 2 < values.size(); k += 3) std::swap(values[k + 1], values[k + 2]);
        const bool wide = !values.empty() && *std::max_element(values.begin(), values.end()) > 0xFFFF;
        for (uint32_t i : values) {
            if (wide) {
                for (int b = 0; b < 4; ++b) bin.push_back(uint8_t(i >> (8 * b)));
            } else {
                bin.push_back(uint8_t(i));
                bin.push_back(uint8_t(i >> 8));
            }
        }
        const size_t v = view(offset, bin.size() - offset, 34963);
        return accessor(v, wide ? 5125 : 5123, values.size(), "SCALAR");
    }

    size_t node(JsonValue n) {
        JsonValue& nodes = array("nodes");
        nodes.push(std::move(n));
        return nodes.items.size() - 1;
    }

    void child(size_t parent, size_t childIndex) {
        JsonValue& p = array("nodes").items[parent];
        if (!p.member("children")) p.set("children", jsonArray());
        const_cast<JsonValue*>(p.member("children"))->push(jsonInteger(int64_t(childIndex)));
    }
};

JsonValue parameterJson(const tr::GeoFileParameter& p) {
    JsonValue a = jsonArray();
    a.push(jsonString(p.key));
    a.push(jsonInteger(p.type));
    switch (p.type) {
        case 1: a.push(jsonBool(p.boolValue != 0)); break;
        case 2: a.push(jsonInteger(p.intValue)); break;
        case 3: {
            JsonValue f = jsonArray();
            for (float x : p.floats) f.push(jsonNumber(x));
            a.push(std::move(f));
            break;
        }
        case 4: a.push(jsonString(p.text)); break;
        case 5: case 8: a.push(jsonString(p.asset.text)); break;
        default: a.push(jsonString(base64(p.raw))); break;
    }
    return a;
}

const tr::GeoFileParameter* findParameter(const tr::GeoFilePiece& piece, const char* key) {
    for (const tr::GeoFileParameter& p : piece.parameters)
        if (p.key == key) return &p;
    return nullptr;
}

int wrapMode(const tr::GeoFilePiece& piece, const char* key) {
    const tr::GeoFileParameter* p = findParameter(piece, key);
    const int d3d = p && p->type == 2 ? p->intValue : 1;
    return d3d == 2 ? 33648 : d3d == 3 ? 33071 : 10497;     // D3DTADDRESS_MIRROR / CLAMP / WRAP
}

bool effectIsTwoSided(const std::string& effect) {
    // The four material effects whose passes set CULLMODE NONE (CLAUDE.md "Back-face culling").
    const std::string e = lower(effect);
    return e == "paldiffmap.fx" || e == "trdiffmapnolight.fx" || e == "trlava.fx" || e == "trbeam.fx" || e == "paldiffmap" || e == "trdiffmapnolight" ||
           e == "trlava" || e == "trbeam";
}

}  // namespace

std::vector<std::string> modelOwnAnimations(tr::AssetLibrary& assets, const std::string& geoName) {
    std::vector<std::string> out;
    try {
        const tr::GeoModel model = tr::readGeo(assets.read(geoName));
        for (const tr::GeoProperty& p : model.properties)
            if (lower(p.key) == "anim" && !p.value.empty() && assets.exists(p.value)) out.push_back(p.value);
    } catch (const std::exception&) {
    }
    const std::string own = baseName(geoName) + ".anm";
    if (assets.exists(own)) {
        bool listed = false;
        for (const std::string& a : out) listed = listed || lower(a) == lower(own);
        if (!listed) out.push_back(own);
    }
    return out;
}

ModelExchangeReport exportModelGltf(tr::AssetLibrary& assets, const std::string& geoName, const std::vector<std::string>& animations,
                                    const std::string& outPath, const ModelExportOptions& options) {
    namespace fs = std::filesystem;
    ModelExchangeReport report;
    const tr::Bytes original = assets.read(geoName);
    const tr::GeoFile file = tr::readGeoFile(original);
    const tr::GeoModel model = tr::readGeo(original);
    const fs::path gltfPath(outPath);
    const fs::path folder = gltfPath.parent_path();
    const std::string binName = gltfPath.stem().string() + ".bin";
    fs::create_directories(folder.empty() ? fs::path(".") : folder);

    Gltf g;
    {
        JsonValue asset = jsonObject();
        asset.set("version", jsonString("2.0"));
        asset.set("generator", jsonString("RasaEdit"));
        g.root.set("asset", std::move(asset));
        // Every top-level array exists from the start (references into the root stay valid); empty ones are dropped at the end.
        for (const char* name : {"scenes", "nodes", "meshes", "materials", "textures", "images", "samplers", "skins", "animations", "accessors",
                                 "bufferViews", "buffers"})
            g.root.set(name, jsonArray());
    }

    // Root node: the model and what glTF has no place for (rasaModel, written as JSON text at the end - Blender keeps a string
    // custom property as it is but turns lists of mixed types into Python text).
    JsonValue rasaModel = jsonObject();
    const size_t rootNode = [&] {
        JsonValue n = jsonObject();
        n.set("name", jsonString(baseName(geoName)));
        JsonValue& m = rasaModel;
        m.set("source", jsonString(geoName));
        m.set("gbodVersion", jsonInteger(file.gbodHeader.version));
        m.set("gbodValue", jsonNumber(file.gbodValue));
        JsonValue groups = jsonArray();
        for (const tr::GeoPropertyGroup& group : file.properties) {
            JsonValue gj = jsonObject();
            gj.set("id", jsonInteger(group.id));
            JsonValue pairs = jsonArray();
            for (const tr::GeoParameter& p : group.pairs) {
                JsonValue pair = jsonArray();
                pair.push(jsonString(p.key));
                pair.push(jsonString(p.value));
                pairs.push(std::move(pair));
            }
            gj.set("pairs", std::move(pairs));
            groups.push(std::move(gj));
        }
        m.set("properties", std::move(groups));
        m.set("animations", jsonArray());
        return g.node(std::move(n));
    }();
    g.root.set("scene", jsonInteger(0));
    {
        JsonValue scene = jsonObject();
        JsonValue nodes = jsonArray();
        nodes.push(jsonInteger(int64_t(rootNode)));
        scene.set("nodes", std::move(nodes));
        g.array("scenes").push(std::move(scene));
    }

    // Bones. A joint's node transform is its REST pose - the one its inverse bind matrix holds (the pose the mesh is bound in), which
    // is what Blender builds its rest pose from - not the skeleton's bind (PSKE) values, which can differ (npc_eloh_hologram_large's
    // rays bone by 12.9 m, the obelisk's root by 90 degrees). The bind values travel in rasaBone.bind; animations carry every
    // component (the bind where a track leaves one out), so Blender poses the bones as the game does.
    const std::vector<modelmath::M4> bindWorld = modelmath::bindWorlds(file.bones);
    const std::vector<modelmath::M4> restWorld = modelmath::restWorlds(file.bones);
    std::vector<size_t> boneNode(file.bones.size());
    std::map<std::string, size_t> boneByName;            // lower-case name -> bone index
    for (size_t b = 0; b < file.bones.size(); ++b) {
        const tr::GeoFileBone& bone = file.bones[b];
        JsonValue n = jsonObject();
        n.set("name", jsonString(bone.name));
        const int32_t parent = bone.parent;
        const modelmath::M4 parentRest = parent >= 0 && size_t(parent) < b ? restWorld[size_t(parent)] : modelmath::M4{};
        const modelmath::Trs rest = modelmath::decompose(modelmath::inverse(parentRest) * restWorld[b]);
        n.set("translation", vec({rest.translation.x, rest.translation.y, rest.translation.z}));
        n.set("rotation", vec({rest.rotation.x, rest.rotation.y, rest.rotation.z, rest.rotation.w}));
        n.set("scale", vec({rest.scale.x, rest.scale.y, rest.scale.z}));
        JsonValue rb = jsonObject();
        rb.set("pbon", vec({double(bone.unknownA), double(bone.unknownB), double(bone.unknownC)}));
        rb.set("bdatVersion", jsonInteger(bone.bdatHeader.version));
        rb.set("hasInverseBind", jsonBool(bone.hasInverseBind));
        rb.set("bind", vec({bone.translation.x, bone.translation.y, bone.translation.z, bone.rotation.x, bone.rotation.y, bone.rotation.z,
                            bone.rotation.w, bone.scale.x, bone.scale.y, bone.scale.z}));
        if (!bone.boundingVolume.empty()) rb.set("boundingVolume", jsonString(base64(bone.boundingVolume)));
        JsonValue extras = jsonObject();
        extras.set("rasaBone", jsonString(writeJson(rb)));
        n.set("extras", std::move(extras));
        boneNode[b] = g.node(std::move(n));
        boneByName.emplace(lower(bone.name), b);
    }
    for (size_t b = 0; b < file.bones.size(); ++b) {
        const int32_t parent = file.bones[b].parent;
        g.child(parent >= 0 && size_t(parent) < file.bones.size() ? boneNode[size_t(parent)] : rootNode, boneNode[b]);
    }
    // A node under a bone that should sit at `local` in the bone's BIND frame (where the engine draws static pieces and finds contact
    // points): its matrix relative to the joint node's rest frame.
    auto underBone = [&](size_t bone, const modelmath::M4& local) {
        const modelmath::M4 m = modelmath::inverse(restWorld[bone]) * bindWorld[bone] * local;
        JsonValue a = jsonArray();
        for (double v : m.m) a.push(jsonNumber(v));
        return a;
    };

    // Skin: every bone a joint, inverse binds as stored (or the inverse of the bind world when a bone has none).
    std::optional<size_t> skinIndex;
    if (!file.bones.empty()) {
        std::vector<float> matrices;
        for (size_t b = 0; b < file.bones.size(); ++b) {
            const tr::GeoFileBone& bone = file.bones[b];
            if (bone.hasInverseBind) {
                for (double v : mirrorMatrix(bone.inverseBind)) matrices.push_back(float(v));
            } else {
                const modelmath::M4 inv = modelmath::inverse(restWorld[b]);
                for (double v : inv.m) matrices.push_back(float(v));
            }
        }
        const size_t ibm = g.floats(matrices, 16, "MAT4", 0);
        JsonValue skin = jsonObject();
        skin.set("inverseBindMatrices", jsonInteger(int64_t(ibm)));
        JsonValue joints = jsonArray();
        for (size_t node : boneNode) joints.push(jsonInteger(int64_t(node)));
        skin.set("joints", std::move(joints));
        skin.set("skeleton", jsonInteger(int64_t(rootNode)));
        g.array("skins").push(std::move(skin));
        skinIndex = 0;
    }

    // Contact points: empties under their bone.
    for (const tr::GeoFileContactPoint& cp : file.contactPoints) {
        JsonValue n = jsonObject();
        std::string name = "cp" + std::to_string(cp.point.id);
        for (const tr::GeoParameter& v : cp.point.values)
            if (lower(v.key) == "cpname" && !v.value.empty()) name = v.value;
        n.set("name", jsonString(name));
        const int32_t bone = cp.point.bone;
        const modelmath::M4 local = modelmath::gltfTrs(cp.point.position, cp.point.rotation, tr::Vec3{1, 1, 1});
        if (bone >= 0 && size_t(bone) < boneNode.size()) {
            n.set("matrix", underBone(size_t(bone), local));
        } else {
            const tr::Vec3 t = mirror(cp.point.position);
            const tr::Quat q = mirror(cp.point.rotation);
            n.set("translation", vec({t.x, t.y, t.z}));
            n.set("rotation", vec({q.x, q.y, q.z, q.w}));
        }
        JsonValue c = jsonObject();
        c.set("id", jsonInteger(cp.point.id));
        JsonValue values = jsonArray();
        for (const tr::GeoParameter& v : cp.point.values) {
            JsonValue pair = jsonArray();
            pair.push(jsonString(v.key));
            pair.push(jsonString(v.value));
            values.push(std::move(pair));
        }
        c.set("values", std::move(values));
        JsonValue extras = jsonObject();
        extras.set("rasaContactPoint", jsonString(writeJson(c)));
        n.set("extras", std::move(extras));
        const size_t node = g.node(std::move(n));
        g.child(bone >= 0 && size_t(bone) < boneNode.size() ? boneNode[size_t(bone)] : rootNode, node);
    }

    // Textures: every texture parameter's file as PNG, one glTF image each.
    std::map<std::string, std::optional<size_t>> textureIndex;      // lower-case file -> glTF texture
    auto texture = [&](const std::string& fileName, int wrapS, int wrapT) -> std::optional<size_t> {
        if (fileName.empty() || !options.textures) return std::nullopt;
        const std::string key = lower(fileName) + "|" + std::to_string(wrapS) + "|" + std::to_string(wrapT);
        if (auto it = textureIndex.find(key); it != textureIndex.end()) return it->second;
        std::optional<size_t> result;
        const std::string png = "textures/" + baseName(fileName) + ".png";
        // The image (shared by texture slots with other wrap modes).
        size_t imageIndex = SIZE_MAX;
        JsonValue& images = g.array("images");
        for (size_t k = 0; k < images.items.size(); ++k)
            if (const JsonValue* uri = images.items[k].member("uri"); uri && uri->text == png) imageIndex = k;
        if (imageIndex == SIZE_MAX) {
            tr::Bytes dds;
            if (!assets.tryRead(fileName, dds)) {
                report.warnings.push_back("texture " + fileName + " is not in the archives");
            } else {
                try {
                    const tr::Bytes pngData = tr::writePng(tr::ddsImage(dds));
                    fs::create_directories(folder / "textures");
                    std::ofstream out(folder / png, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(pngData.data()), std::streamsize(pngData.size()));
                    report.written.push_back((folder / png).string());
                    JsonValue image = jsonObject();
                    image.set("uri", jsonString(png));
                    image.set("name", jsonString(fileName));
                    images.push(std::move(image));
                    imageIndex = images.items.size() - 1;
                } catch (const std::exception& e) {
                    report.warnings.push_back("texture " + fileName + ": " + e.what());
                }
            }
        }
        if (imageIndex != SIZE_MAX) {
            JsonValue sampler = jsonObject();
            sampler.set("wrapS", jsonInteger(wrapS));
            sampler.set("wrapT", jsonInteger(wrapT));
            JsonValue& samplers = g.array("samplers");
            samplers.push(std::move(sampler));
            JsonValue t = jsonObject();
            t.set("source", jsonInteger(int64_t(imageIndex)));
            t.set("sampler", jsonInteger(int64_t(samplers.items.size() - 1)));
            JsonValue& textures = g.array("textures");
            textures.push(std::move(t));
            result = textures.items.size() - 1;
        }
        textureIndex[key] = result;
        return result;
    };

    // A further glTF scene with one root node: Blender imports it as an excluded (hidden) collection of that name.
    auto hiddenScene = [&](const char* name) {
        JsonValue n = jsonObject();
        n.set("name", jsonString(name));
        const size_t root = g.node(std::move(n));
        JsonValue scene = jsonObject();
        scene.set("name", jsonString(name));
        JsonValue nodes = jsonArray();
        nodes.push(jsonInteger(int64_t(root)));
        scene.set("nodes", std::move(nodes));
        g.array("scenes").push(std::move(scene));
        g.array("scenes").items[0].set("name", jsonString(baseName(geoName)));
        return root;
    };

    // Pieces: a mesh and a material each. Low-detail pieces (unless the model has nothing else) go into a second glTF scene by
    // default: Blender's importer ("Import Scene as Collection", on by default) makes each scene a collection and excludes those of
    // the scenes other than the default one, so they arrive in a hidden "Low detail" collection. They are listed in rasaModel.omitted
    // either way, so an import restores them from the source when the file does not carry them (Blender's export leaves excluded
    // collections out).
    std::vector<bool> exported(file.pieces.size(), true);
    std::vector<bool> hidden(file.pieces.size(), false);
    std::optional<size_t> lowDetailRoot;
    {
        auto lowDetail = [&](size_t k) { return model.pieces[k].lod > 0 || model.pieces[k].swapId == 99; };
        bool anyFull = false;
        for (size_t k = 0; k < file.pieces.size(); ++k) anyFull = anyFull || !lowDetail(k);
        JsonValue omitted = jsonArray();
        for (size_t k = 0; k < file.pieces.size(); ++k)
            if (options.lowDetail != ModelExportOptions::LowDetail::Visible && anyFull && lowDetail(k)) {
                if (options.lowDetail == ModelExportOptions::LowDetail::Omitted) exported[k] = false;
                else hidden[k] = true;
                omitted.push(jsonInteger(int64_t(k)));
            }
        rasaModel.set("omitted", std::move(omitted));
        if (std::find(hidden.begin(), hidden.end(), true) != hidden.end()) lowDetailRoot = hiddenScene("Low detail");
    }
    for (size_t k = 0; k < file.pieces.size(); ++k) {
        if (!exported[k]) continue;
        const tr::GeoFilePiece& piece = file.pieces[k];
        const tr::GeoPiece& decoded = model.pieces[k];

        // Material.
        JsonValue material = jsonObject();
        material.set("name", jsonString(baseName(geoName) + " " + std::to_string(k) + " " + piece.effect.text));
        JsonValue pbr = jsonObject();
        pbr.set("metallicFactor", jsonNumber(0));
        pbr.set("roughnessFactor", jsonNumber(1));
        if (const tr::GeoFileParameter* d = findParameter(piece, "DiffuseTexture"))
            if (auto t = texture(d->asset.text, wrapMode(piece, "DiffuseAddressU"), wrapMode(piece, "DiffuseAddressV"))) {
                JsonValue info = jsonObject();
                info.set("index", jsonInteger(int64_t(*t)));
                pbr.set("baseColorTexture", std::move(info));
            }
        material.set("pbrMetallicRoughness", std::move(pbr));
        if (const tr::GeoFileParameter* n = findParameter(piece, "NormalMapTexture"))
            if (auto t = texture(n->asset.text, 10497, 10497)) {
                JsonValue info = jsonObject();
                info.set("index", jsonInteger(int64_t(*t)));
                material.set("normalTexture", std::move(info));
            }
        // Blending: additive / translucent effects blend, alpha-tested ones cut out.
        const std::string effect = lower(piece.effect.text);
        const tr::GeoFileParameter* alphaTest = findParameter(piece, "AlphaTestEnable");
        const tr::GeoFileParameter* phase = findParameter(piece, "Phase");
        const bool blended = effect.rfind("tradditive", 0) == 0 || (phase && (lower(phase->text) == "additive" || lower(phase->text) == "translucent"));
        if (blended) {
            material.set("alphaMode", jsonString("BLEND"));
        } else if ((alphaTest && alphaTest->boolValue) || effect.rfind("paldiffalpha", 0) == 0) {
            material.set("alphaMode", jsonString("MASK"));
            material.set("alphaCutoff", jsonNumber(0.5));
        }
        if (effectIsTwoSided(piece.effect.text)) material.set("doubleSided", jsonBool(true));
        {
            JsonValue rm = jsonObject();
            rm.set("effect", jsonString(piece.effect.text));
            JsonValue params = jsonArray();
            for (const tr::GeoFileParameter& p : piece.parameters) params.push(parameterJson(p));
            rm.set("parameters", std::move(params));
            JsonValue extras = jsonObject();
            extras.set("rasaMaterial", jsonString(writeJson(rm)));
            material.set("extras", std::move(extras));
        }
        // Textures the other slots name are written too (they are not glTF material slots).
        for (const tr::GeoFileParameter& p : piece.parameters)
            if ((p.type == 5 || p.type == 8) && p.key != "DiffuseTexture" && p.key != "NormalMapTexture") texture(p.asset.text, 10497, 10497);
        JsonValue& materials = g.array("materials");
        materials.push(std::move(material));
        const size_t materialIndex = materials.items.size() - 1;

        // Piece settings.
        JsonValue rp = jsonObject();
        rp.set("index", jsonInteger(int64_t(k)));
        rp.set("gpceVersion", jsonInteger(piece.header.version));
        rp.set("skinned", jsonBool(piece.skinned));
        rp.set("bone", jsonString(piece.boneName));
        rp.set("group", jsonString(piece.group));
        rp.set("name", jsonString(piece.name));
        rp.set("swapId", jsonInteger(piece.swapId));
        rp.set("unknown0", jsonInteger(piece.unknown0));
        rp.set("hasMaterial", jsonInteger(piece.hasMaterial));
        rp.set("bonesPerVertex", jsonInteger(piece.bonesPerVertex));
        rp.set("sortedIndexSets", jsonInteger(int64_t(piece.sortedIndices.size())));
        {
            JsonValue ud = jsonArray();
            for (const tr::GeoParameter& p : piece.userData) {
                JsonValue pair = jsonArray();
                pair.push(jsonString(p.key));
                pair.push(jsonString(p.value));
                ud.push(std::move(pair));
            }
            rp.set("userData", std::move(ud));
            JsonValue layout = jsonArray();
            for (const tr::VertexElement& e : piece.vertices.layout)
                layout.push(vec({double(e.type), double(e.method), double(e.usage), double(e.usageIndex)}));
            rp.set("layout", std::move(layout));
            JsonValue palette = jsonArray();
            for (const std::string& b : piece.skinBones) palette.push(jsonString(b));
            rp.set("skinBones", std::move(palette));
        }

        JsonValue node = jsonObject();
        node.set("name", jsonString(piece.name.empty() ? "piece " + std::to_string(k) : piece.name));
        if (!decoded.vertices.empty() && !decoded.indices.empty()) {
            // Attributes the piece's layout has.
            bool hasNormal = false, hasColour = false, hasUv[4] = {false, false, false, false};
            for (const tr::VertexElement& e : piece.vertices.layout) {
                hasNormal = hasNormal || (e.usage == 3 && e.usageIndex == 0);
                hasColour = hasColour || (e.usage == 10 && e.usageIndex == 0);
                if (e.usage == 5 && e.usageIndex < 4) hasUv[e.usageIndex] = true;
            }
            std::vector<float> positions, normals, uvs[4];
            std::vector<uint8_t> colours, joints;
            std::vector<float> weights;
            for (const tr::GeoVertex& v : decoded.vertices) {
                const tr::Vec3 p = mirror(v.position);
                positions.insert(positions.end(), {p.x, p.y, p.z});
                if (hasNormal) {
                    tr::Vec3 n = mirror(v.normal);
                    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    n = len > 1e-6f ? tr::Vec3{n.x / len, n.y / len, n.z / len} : tr::Vec3{0, 1, 0};
                    normals.insert(normals.end(), {n.x, n.y, n.z});
                }
                const tr::Vec2 set[4] = {v.uv, v.uv1, v.uv2, v.uv3};
                for (int s = 0; s < 4; ++s)
                    if (hasUv[s]) uvs[s].insert(uvs[s].end(), {set[s].x, set[s].y});
                if (hasColour) {
                    colours.push_back(uint8_t((v.colour >> 16) & 0xFF));
                    colours.push_back(uint8_t((v.colour >> 8) & 0xFF));
                    colours.push_back(uint8_t(v.colour & 0xFF));
                    colours.push_back(uint8_t(v.colour >> 24));
                }
                if (piece.skinned) {
                    // The game stores two weights and derives the third as 1 - their sum, which is often a rounding leftover
                    // (1e-7) on the same bone as another slot. glTF tools read a joint listed twice as one assignment overwriting the
                    // other (Blender's vertex groups), so influences of one bone are merged and leftovers below 1e-4 dropped.
                    std::vector<std::pair<size_t, float>> influences;
                    for (uint32_t i = 0; i < 3; ++i) {
                        const float w = tr::blendWeight(v, i);
                        if (w < 1e-4f) continue;
                        const uint32_t slot = tr::blendIndex(v, i);
                        size_t joint = 0;
                        if (slot < piece.skinBones.size()) {
                            if (auto it = boneByName.find(lower(piece.skinBones[slot])); it != boneByName.end()) joint = it->second;
                            else report.warnings.push_back("piece " + std::to_string(k) + ": palette bone " + piece.skinBones[slot] + " is not in the skeleton");
                        }
                        bool merged = false;
                        for (auto& inf : influences)
                            if (inf.first == joint) {
                                inf.second += w;
                                merged = true;
                            }
                        if (!merged) influences.emplace_back(joint, w);
                    }
                    float sum = 0;
                    for (const auto& inf : influences) sum += inf.second;
                    if (influences.empty() || sum <= 0) influences = {{0, 1.0f}}, sum = 1;
                    for (uint32_t i = 0; i < 4; ++i) {
                        joints.push_back(uint8_t(std::min<size_t>(i < influences.size() ? influences[i].first : 0, 255)));
                        weights.push_back(i < influences.size() ? influences[i].second / sum : 0.0f);   // glTF wants a sum of 1
                    }
                }
            }
            if (piece.skinned && file.bones.size() > 256) report.warnings.push_back("more than 256 bones: joint indices clipped");
            JsonValue attributes = jsonObject();
            attributes.set("POSITION", jsonInteger(int64_t(g.floats(positions, 3, "VEC3", 34962, true))));
            if (hasNormal) attributes.set("NORMAL", jsonInteger(int64_t(g.floats(normals, 3, "VEC3", 34962))));
            for (int s = 0; s < 4; ++s)
                if (hasUv[s]) attributes.set("TEXCOORD_" + std::to_string(s), jsonInteger(int64_t(g.floats(uvs[s], 2, "VEC2", 34962))));
            if (hasColour) attributes.set("COLOR_0", jsonInteger(int64_t(g.bytes(colours, "VEC4", true, 34962))));
            if (piece.skinned) {
                attributes.set("JOINTS_0", jsonInteger(int64_t(g.bytes(joints, "VEC4", false, 34962))));
                attributes.set("WEIGHTS_0", jsonInteger(int64_t(g.floats(weights, 4, "VEC4", 34962))));
            }
            JsonValue primitive = jsonObject();
            primitive.set("attributes", std::move(attributes));
            primitive.set("indices", jsonInteger(int64_t(g.indices(decoded.indices))));
            primitive.set("material", jsonInteger(int64_t(materialIndex)));
            JsonValue mesh = jsonObject();
            mesh.set("name", jsonString(piece.name.empty() ? "piece " + std::to_string(k) : piece.name));
            JsonValue primitives = jsonArray();
            primitives.push(std::move(primitive));
            mesh.set("primitives", std::move(primitives));
            JsonValue extras = jsonObject();
            extras.set("rasaPiece", jsonString(writeJson(rp)));
            mesh.set("extras", std::move(extras));
            JsonValue& meshes = g.array("meshes");
            meshes.push(std::move(mesh));
            node.set("mesh", jsonInteger(int64_t(meshes.items.size() - 1)));
            if (piece.skinned && skinIndex) node.set("skin", jsonInteger(int64_t(*skinIndex)));
        } else {
            // An empty piece keeps its settings on the node.
            JsonValue extras = jsonObject();
            extras.set("rasaPiece", jsonString(writeJson(rp)));
            node.set("extras", std::move(extras));
            report.warnings.push_back("piece " + std::to_string(k) + " has no triangles (kept as an empty node)");
        }
        // A static piece on a bone is drawn with the bone's (bind) world transform: a child of the bone, placed so it sits in the
        // bone's bind frame. Skinned pieces at the root.
        // Hidden low-detail pieces hang under the "Low detail" root (a node is in one hierarchy only): a static one at its bone's
        // bind world, not following the bone in Blender (the import finds the bone by rasaPiece.bone).
        size_t parent = hidden[k] ? *lowDetailRoot : rootNode;
        if (!piece.skinned && !piece.boneName.empty())
            if (auto it = boneByName.find(lower(piece.boneName)); it != boneByName.end()) {
                if (hidden[k]) {
                    JsonValue a = jsonArray();
                    for (double v : bindWorld[it->second].m) a.push(jsonNumber(v));
                    node.set("matrix", std::move(a));
                } else {
                    parent = boneNode[it->second];
                    node.set("matrix", underBone(it->second, modelmath::M4{}));
                }
            }
        const size_t n = g.node(std::move(node));
        g.child(parent, n);
    }

    // Collision: every bone's volumes (what the client walks on and collides with - see geofile.h GeoBoneVolume) as meshes in a
    // hidden "Collision" collection, each at its bone's bind world (the volumes' frame). Boxes and spheres become box / sphere meshes
    // and are read back as such (rasaCollision.kind).
    if (options.collision) {
        std::optional<size_t> collisionRoot;
        for (size_t b = 0; b < file.bones.size(); ++b) {
            if (file.bones[b].boundingVolume.empty()) continue;
            std::vector<tr::GeoBoneVolume> volumes;
            try {
                volumes = tr::readBoneVolumes(file.bones[b].boundingVolume);
            } catch (const std::exception& e) {
                report.warnings.push_back("bone " + file.bones[b].name + ": collision volume not read (" + e.what() + ")");
                continue;
            }
            for (size_t k = 0; k < volumes.size(); ++k) {
                const tr::GeoBoneVolume& v = volumes[k];
                std::vector<tr::Vec3> points;
                std::vector<uint32_t> indices;
                std::string kind;
                if (v.kind == tr::fourcc("BVWS")) {
                    kind = "surface";
                    points = v.vertices;
                    for (uint16_t i : v.indices) indices.push_back(i);
                } else if (v.kind == tr::fourcc("BVBX")) {
                    kind = "box";
                    for (int c = 0; c < 8; ++c)
                        points.push_back(tr::Vec3{v.centre.x + ((c & 4) ? v.halfExtents.x : -v.halfExtents.x), v.centre.y + ((c & 2) ? v.halfExtents.y : -v.halfExtents.y),
                                                  v.centre.z + ((c & 1) ? v.halfExtents.z : -v.halfExtents.z)});
                    static const uint32_t faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
                    for (const auto& f : faces) indices.insert(indices.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
                } else if (v.kind == tr::fourcc("BVSP")) {
                    kind = "sphere";
                    const int rings = 6, segments = 12;
                    for (int r = 0; r <= rings; ++r)
                        for (int sgm = 0; sgm < segments; ++sgm) {
                            const double th = 3.14159265358979 * r / rings, ph = 2 * 3.14159265358979 * sgm / segments;
                            points.push_back(tr::Vec3{float(v.centre.x + v.sphereRadius * std::sin(th) * std::cos(ph)), float(v.centre.y + v.sphereRadius * std::cos(th)),
                                                      float(v.centre.z + v.sphereRadius * std::sin(th) * std::sin(ph))});
                        }
                    for (int r = 0; r < rings; ++r)
                        for (int sgm = 0; sgm < segments; ++sgm) {
                            const uint32_t a0 = uint32_t(r * segments + sgm), a1 = uint32_t(r * segments + (sgm + 1) % segments);
                            const uint32_t b0 = a0 + uint32_t(segments), b1 = a1 + uint32_t(segments);
                            indices.insert(indices.end(), {a0, a1, b0, a1, b1, b0});      // facing outward once written
                        }
                } else {
                    report.warnings.push_back("bone " + file.bones[b].name + ": collision volume " + tr::tagName(v.kind) + " is not exported");
                    continue;
                }
                if (points.empty() || indices.empty()) continue;
                std::vector<float> positions;
                for (const tr::Vec3& p : points) {
                    const tr::Vec3 m = mirror(p);
                    positions.insert(positions.end(), {m.x, m.y, m.z});
                }
                JsonValue attributes = jsonObject();
                attributes.set("POSITION", jsonInteger(int64_t(g.floats(positions, 3, "VEC3", 34962, true))));
                JsonValue primitive = jsonObject();
                primitive.set("attributes", std::move(attributes));
                primitive.set("indices", jsonInteger(int64_t(g.indices(indices))));
                JsonValue primitives = jsonArray();
                primitives.push(std::move(primitive));
                const std::string name = "collision " + file.bones[b].name + (volumes.size() > 1 ? " " + std::to_string(k) : "");
                JsonValue rc = jsonObject();
                rc.set("bone", jsonString(file.bones[b].name));
                rc.set("kind", jsonString(kind));
                const std::string rcText = writeJson(rc);
                JsonValue mesh = jsonObject();
                mesh.set("name", jsonString(name));
                mesh.set("primitives", std::move(primitives));
                JsonValue extras = jsonObject();
                extras.set("rasaCollision", jsonString(rcText));
                mesh.set("extras", std::move(extras));
                JsonValue& meshes = g.array("meshes");
                meshes.push(std::move(mesh));
                JsonValue node = jsonObject();
                node.set("name", jsonString(name));
                node.set("mesh", jsonInteger(int64_t(meshes.items.size() - 1)));
                JsonValue nodeExtras = jsonObject();
                nodeExtras.set("rasaCollision", jsonString(rcText));
                node.set("extras", std::move(nodeExtras));
                JsonValue matrix = jsonArray();
                for (double x : bindWorld[b].m) matrix.push(jsonNumber(x));
                node.set("matrix", std::move(matrix));
                if (!collisionRoot) collisionRoot = hiddenScene("Collision");
                const size_t n = g.node(std::move(node));
                g.child(*collisionRoot, n);
            }
        }
        rasaModel.set("collisionExported", jsonBool(true));
    }

    // Animations. Every bone gets translation, rotation and scale channels: a track's keys with the bind value in each component
    // the track leaves out (the engine keeps the bind there), and a bone without a track holds its bind. So Blender poses the
    // skeleton as the game does (its rest pose is the inverse binds', see Bones); the import leaves a component out again when it
    // reads back as the bind and the source track did not store it.
    for (const std::string& animName : animations) {
        tr::Bytes data;
        if (!assets.tryRead(animName, data)) {
            report.warnings.push_back("animation " + animName + " is not in the archives");
            continue;
        }
        tr::AnimFile anim;
        try {
            anim = tr::readAnimFile(data);
        } catch (const std::exception& e) {
            report.warnings.push_back("animation " + animName + ": " + e.what());
            continue;
        }
        JsonValue samplers = jsonArray(), channels = jsonArray(), trackInfo = jsonArray(), materialTracks = jsonArray(), otherTracks = jsonArray();
        auto channel = [&](size_t node, const char* path, size_t input, size_t output) {
            JsonValue s = jsonObject();
            s.set("input", jsonInteger(int64_t(input)));
            s.set("output", jsonInteger(int64_t(output)));
            s.set("interpolation", jsonString("LINEAR"));
            samplers.push(std::move(s));
            JsonValue c = jsonObject();
            c.set("sampler", jsonInteger(int64_t(samplers.items.size() - 1)));
            JsonValue target = jsonObject();
            target.set("node", jsonInteger(int64_t(node)));
            target.set("path", jsonString(path));
            c.set("target", std::move(target));
            channels.push(std::move(c));
        };
        auto timeAccessor = [&](const std::vector<float>& times) {
            const size_t input = g.floats(times, 1, "SCALAR", 0);
            JsonValue& acc = g.array("accessors").items[input];
            acc.set("min", vec({times.front()}));
            acc.set("max", vec({times.back()}));
            return input;
        };
        size_t unmatched = 0;
        std::vector<bool> animated(file.bones.size(), false);
        for (const tr::AnimFileTrack& t : anim.tracks) {
            if (t.target == tr::fourcc("EFCT")) {
                JsonValue m = jsonObject();
                m.set("group", jsonString(t.name));
                m.set("parameter", jsonString(t.parameter));
                JsonValue keys = jsonArray();
                for (const tr::MaterialKey& key : tr::decodeMaterialKeys(t))
                    keys.push(vec({key.scaleU, key.scaleV, key.rotation.x, key.rotation.y, key.rotation.z, key.translateU, key.translateV, key.time}));
                m.set("keys", std::move(keys));
                materialTracks.push(std::move(m));
                continue;
            }
            if (t.target != tr::fourcc("SKEL")) {
                JsonValue o = jsonObject();
                o.set("preTag", jsonInteger(t.preTag));
                o.set("target", jsonInteger(t.target));
                o.set("version", jsonInteger(t.header.version));
                o.set("byte0", jsonInteger(t.byte0));
                o.set("raw", jsonString(base64(t.raw)));
                otherTracks.push(std::move(o));
                continue;
            }
            JsonValue info = jsonObject();
            info.set("bone", jsonString(t.name));
            info.set("flags", jsonInteger(t.flags));
            info.set("keys", jsonString(tr::tagName(t.keyTag)));
            trackInfo.push(std::move(info));
            const auto boneIt = boneByName.find(lower(t.name));
            if (boneIt == boneByName.end()) {
                ++unmatched;
                continue;
            }
            const size_t b = boneIt->second;
            const tr::GeoFileBone& bone = file.bones[b];
            const std::vector<tr::AnimKey> keys = tr::decodeAnimKeys(t);
            if (keys.empty()) continue;
            animated[b] = true;
            // glTF needs strictly increasing times: drop keys that do not advance (none expected).
            std::vector<tr::AnimKey> kept;
            for (const tr::AnimKey& key : keys)
                if (kept.empty() || key.time > kept.back().time) kept.push_back(key);
            const std::string kind = tr::tagName(t.keyTag);
            const bool scale = kind == "KEYS" || kind == "KEST" || kind == "KESR" || kind == "KSRT" || kind == "XKSR" || kind == "XSRT";
            const bool rotation = kind == "KEYR" || kind == "KERT" || kind == "KESR" || kind == "KSRT" || kind == "XKER" || kind == "XKRT" || kind == "XKSR" ||
                                  kind == "XSRT";
            const bool translation = kind == "KEYT" || kind == "KERT" || kind == "KEST" || kind == "KSRT" || kind == "XKRT" || kind == "XSRT";
            std::vector<float> times, tv, rv, sv;
            for (const tr::AnimKey& key : kept) {
                times.push_back(key.time);
                const tr::Vec3 p = mirror(translation ? key.translation : bone.translation);
                const tr::Quat q = mirror(rotation ? key.rotation : bone.rotation);
                const tr::Vec3 s = scale ? key.scale : bone.scale;
                tv.insert(tv.end(), {p.x, p.y, p.z});
                rv.insert(rv.end(), {q.x, q.y, q.z, q.w});
                sv.insert(sv.end(), {s.x, s.y, s.z});
            }
            const size_t input = timeAccessor(times);
            channel(boneNode[b], "translation", input, g.floats(tv, 3, "VEC3", 0));
            channel(boneNode[b], "rotation", input, g.floats(rv, 4, "VEC4", 0));
            channel(boneNode[b], "scale", input, g.floats(sv, 3, "VEC3", 0));
        }
        // Bones without a track hold their bind.
        for (size_t b = 0; b < file.bones.size(); ++b) {
            if (animated[b]) continue;
            const tr::GeoFileBone& bone = file.bones[b];
            const size_t input = timeAccessor({0.0f});
            const tr::Vec3 p = mirror(bone.translation);
            const tr::Quat q = mirror(bone.rotation);
            channel(boneNode[b], "translation", input, g.floats({p.x, p.y, p.z}, 3, "VEC3", 0));
            channel(boneNode[b], "rotation", input, g.floats({q.x, q.y, q.z, q.w}, 4, "VEC4", 0));
            channel(boneNode[b], "scale", input, g.floats({bone.scale.x, bone.scale.y, bone.scale.z}, 3, "VEC3", 0));
        }
        if (unmatched) report.warnings.push_back(animName + ": " + std::to_string(unmatched) + " track(s) name bones this model does not have");
        if (channels.items.empty()) {
            report.warnings.push_back(animName + ": the model has no bones - left out");
            continue;
        }
        JsonValue a = jsonObject();
        a.set("name", jsonString(baseName(animName)));
        a.set("samplers", std::move(samplers));
        a.set("channels", std::move(channels));
        JsonValue ra = jsonObject();
        ra.set("name", jsonString(baseName(animName)));
        ra.set("source", jsonString(animName));
        ra.set("animVersion", jsonInteger(anim.animHeader.version));
        ra.set("tracks", std::move(trackInfo));
        ra.set("materialTracks", std::move(materialTracks));
        ra.set("otherTracks", std::move(otherTracks));
        JsonValue extras = jsonObject();
        extras.set("rasaAnimation", jsonString(writeJson(ra)));
        a.set("extras", std::move(extras));
        // Blender drops animation extras: the root's rasaModel keeps a copy, found by the animation's name.
        const_cast<JsonValue*>(rasaModel.member("animations"))->push(std::move(ra));
        g.array("animations").push(std::move(a));
    }
    {
        JsonValue extras = jsonObject();
        extras.set("rasaModel", jsonString(writeJson(rasaModel)));
        g.array("nodes").items[rootNode].set("extras", std::move(extras));
    }
    // Buffer and files.
    g.align();
    {
        JsonValue buffer = jsonObject();
        buffer.set("uri", jsonString(binName));
        buffer.set("byteLength", jsonInteger(int64_t(g.bin.size())));
        g.array("buffers").push(std::move(buffer));
    }
    {
        std::ofstream out(folder / binName, std::ios::binary);
        out.write(reinterpret_cast<const char*>(g.bin.data()), std::streamsize(g.bin.size()));
        if (!out) throw tr::FormatError("cannot write " + (folder / binName).string());
        report.written.push_back((folder / binName).string());
    }
    {
        g.root.members.erase(std::remove_if(g.root.members.begin(), g.root.members.end(),
                                            [](const auto& m) { return m.second.isArray() && m.second.items.empty(); }),
                             g.root.members.end());
        const std::string text = writeJson(g.root);
        std::ofstream out(gltfPath, std::ios::binary);
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) throw tr::FormatError("cannot write " + gltfPath.string());
        report.written.push_back(gltfPath.string());
    }
    return report;
}
