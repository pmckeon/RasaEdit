// trformats.cpp - see trformats.h for the format notes.
#include "trformats.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace tr {

// ------------------------------------------------------------------------------------------
// Basic I/O
// ------------------------------------------------------------------------------------------
Bytes readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw FormatError("cannot open " + path);
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0);
    Bytes data(static_cast<size_t>(size));
    if (size > 0) file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

static uint32_t loadU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

ByteReader::ByteReader(const Bytes& data, size_t start, size_t end)
    : data_(&data), pos_(start), end_(end == SIZE_MAX ? data.size() : end) {
    if (end_ > data.size() || pos_ > end_) throw FormatError("reader window out of range");
}

void ByteReader::need(size_t count) const {
    if (count > end_ - pos_) throw FormatError("unexpected end of data at offset " + std::to_string(pos_));
}

void ByteReader::seek(size_t pos) {
    if (pos > end_) throw FormatError("seek past end (" + std::to_string(pos) + ")");
    pos_ = pos;
}

uint8_t ByteReader::u8() {
    need(1);
    return (*data_)[pos_++];
}

uint16_t ByteReader::u16() {
    need(2);
    const uint8_t* p = data_->data() + pos_;
    pos_ += 2;
    return uint16_t(p[0] | (p[1] << 8));
}

uint32_t ByteReader::u32() {
    need(4);
    const uint32_t value = loadU32(data_->data() + pos_);
    pos_ += 4;
    return value;
}

int32_t ByteReader::i32() { return static_cast<int32_t>(u32()); }

uint64_t ByteReader::u64() {
    const uint64_t low = u32();
    const uint64_t high = u32();
    return low | (high << 32);
}

float ByteReader::f32() {
    const uint32_t bits = u32();
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

Vec3 ByteReader::vec3() {
    Vec3 v;
    v.x = f32();
    v.y = f32();
    v.z = f32();
    return v;
}

std::string ByteReader::cstr() {
    const size_t start = pos_;
    while (pos_ < end_ && (*data_)[pos_] != 0) ++pos_;
    if (pos_ >= end_) throw FormatError("unterminated string at offset " + std::to_string(start));
    std::string text(reinterpret_cast<const char*>(data_->data() + start), pos_ - start);
    ++pos_;
    return text;
}

Bytes ByteReader::bytes(size_t count) {
    need(count);
    Bytes out(data_->begin() + pos_, data_->begin() + pos_ + count);
    pos_ += count;
    return out;
}

uint32_t ByteReader::peekU32() const {
    if (end_ - pos_ < 4) return 0;
    return loadU32(data_->data() + pos_);
}

const uint8_t* ByteReader::at(size_t pos) const {
    if (pos > end_) throw FormatError("position out of range");
    return data_->data() + pos;
}

// ------------------------------------------------------------------------------------------
// .glm archive
// ------------------------------------------------------------------------------------------
std::string toLowerAscii(std::string text) {
    for (char& ch : text)
        if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    return text;
}

GlmArchive::GlmArchive(const std::string& path) : path_(path), file_(path, std::ios::binary) {
    if (!file_) throw FormatError("cannot open " + path);
    file_.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(file_.tellg());
    if (size < 8) throw FormatError(path + ": file too small");
    auto readAt = [&](uint64_t offset, size_t count) {
        if (offset + count > size) throw FormatError(path + ": read past end of archive");
        Bytes buffer(count);
        file_.seekg(std::streamoff(offset));
        if (count && !file_.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(count)))
            throw FormatError(path + ": read failed");
        return buffer;
    };
    const Bytes footer = readAt(size - 4, 4);
    const uint32_t directoryOffset = loadU32(footer.data());
    const Bytes header = readAt(directoryOffset, 20);
    if (std::memcmp(header.data(), "CHNKBLXX", 8) != 0) throw FormatError(path + ": no CHNKBLXX directory");
    const uint32_t nameTableOffset = loadU32(header.data() + 8);
    const uint32_t nameTableLength = loadU32(header.data() + 12);
    const uint32_t fileCount = loadU32(header.data() + 16);
    const Bytes directory = readAt(uint64_t(directoryOffset) + 20, size_t(fileCount) * 22);
    const Bytes names = readAt(nameTableOffset, nameTableLength);
    ByteReader r(directory);
    entries_.reserve(fileCount);
    index_.reserve(fileCount);
    for (uint32_t k = 0; k < fileCount; ++k) {
        GlmEntry entry;
        entry.offset = r.u32();
        entry.storedSize = r.u32();
        entry.rawSize = r.u32();
        entry.nameOffset = r.u32();
        entry.flags = r.u16();
        entry.timestamp = r.u32();
        if (entry.nameOffset >= names.size()) throw FormatError(path + ": bad name offset");
        const char* text = reinterpret_cast<const char*>(names.data() + entry.nameOffset);
        entry.name.assign(text, strnlen(text, names.size() - entry.nameOffset));
        if (uint64_t(entry.offset) + entry.storedSize > size) throw FormatError(path + ": entry " + entry.name + " out of range");
        index_.emplace(toLowerAscii(entry.name), entries_.size());
        entries_.push_back(std::move(entry));
    }
}

const GlmEntry* GlmArchive::find(const std::string& name) const {
    const auto it = index_.find(toLowerAscii(name));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

Bytes GlmArchive::read(const GlmEntry& entry) const {
    Bytes stored(entry.storedSize);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.clear();
        file_.seekg(std::streamoff(entry.offset));
        if (entry.storedSize && !file_.read(reinterpret_cast<char*>(stored.data()), std::streamsize(entry.storedSize)))
            throw FormatError(path_ + ": cannot read " + entry.name);
    }
    if (entry.flags == 0) return stored;
    Bytes out(entry.rawSize);
    if (entry.rawSize == 0) return out;
    uLongf outLength = entry.rawSize;
    const int rc = uncompress(out.data(), &outLength, stored.data(), entry.storedSize);
    if (rc != Z_OK || outLength != entry.rawSize) throw FormatError(entry.name + ": zlib error " + std::to_string(rc));
    return out;
}

Bytes GlmArchive::read(const std::string& name) const {
    const GlmEntry* entry = find(name);
    if (!entry) throw FormatError(path_ + " has no file named " + name);
    return read(*entry);
}

Bytes buildGlmArchive(std::vector<GlmSource> files) {
    std::sort(files.begin(), files.end(), [](const GlmSource& a, const GlmSource& b) { return toLowerAscii(a.name) < toLowerAscii(b.name); });
    for (size_t k = 0; k < files.size(); ++k) {
        if (files[k].name.empty() || files[k].name.find('\0') != std::string::npos) throw FormatError("glm: an entry needs a name");
        if (k && toLowerAscii(files[k].name) == toLowerAscii(files[k - 1].name)) throw FormatError("glm: two entries named " + files[k].name);
    }
    const uint32_t now = uint32_t(std::time(nullptr));
    Bytes out;
    const auto putU32 = [&](uint32_t v) { for (int b = 0; b < 4; ++b) out.push_back(uint8_t(v >> (8 * b))); };
    const auto putU16 = [&](uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); };
    const auto pad = [&] { while (out.size() % 1024) out.push_back(0); };
    std::vector<GlmEntry> entries;
    for (const GlmSource& f : files) {
        pad();
        GlmEntry e;
        e.name = f.name;
        e.offset = uint32_t(out.size());
        e.rawSize = uint32_t(f.data.size());
        e.timestamp = f.timestamp ? f.timestamp : now;
        Bytes packed;
        if (f.compress && !f.data.empty()) {
            uLongf length = compressBound(uLong(f.data.size()));
            packed.resize(length);
            if (compress2(packed.data(), &length, f.data.data(), uLong(f.data.size()), 1) != Z_OK) throw FormatError("glm: zlib failed on " + f.name);
            packed.resize(length);
        }
        if (!packed.empty() && packed.size() < f.data.size()) {
            e.flags = 1;
            e.storedSize = uint32_t(packed.size());
            out.insert(out.end(), packed.begin(), packed.end());
        } else {
            e.storedSize = e.rawSize;
            out.insert(out.end(), f.data.begin(), f.data.end());
        }
        if (out.size() > 0xFFFFFFFFull) throw FormatError("glm: archive larger than 4 GB");
        entries.push_back(std::move(e));
    }
    pad();
    const uint32_t nameTableOffset = uint32_t(out.size());
    for (GlmEntry& e : entries) {
        e.nameOffset = uint32_t(out.size()) - nameTableOffset;
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.push_back(0);
    }
    const uint32_t nameTableLength = uint32_t(out.size()) - nameTableOffset;
    const uint32_t directoryOffset = uint32_t(out.size());
    const char magic[8] = {'C', 'H', 'N', 'K', 'B', 'L', 'X', 'X'};
    out.insert(out.end(), magic, magic + 8);
    putU32(nameTableOffset);
    putU32(nameTableLength);
    putU32(uint32_t(entries.size()));
    for (const GlmEntry& e : entries) {
        putU32(e.offset);
        putU32(e.storedSize);
        putU32(e.rawSize);
        putU32(e.nameOffset);
        putU16(e.flags);
        putU32(e.timestamp);
    }
    putU32(0);
    putU32(directoryOffset);
    return out;
}

void writeGlmArchive(const std::string& path, std::vector<GlmSource> files) {
    const Bytes archive = buildGlmArchive(std::move(files));
    const std::string temporary = path + ".writing";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw FormatError("cannot write " + temporary);
        out.write(reinterpret_cast<const char*>(archive.data()), std::streamsize(archive.size()));
        if (!out) throw FormatError("cannot write " + temporary);
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temporary, path, ec);
        if (ec) throw FormatError("cannot replace " + path + ": " + ec.message());
    }
}

static const GlmEntry* findSuffix(const GlmArchive& glm, const std::string& suffix) {
    for (const GlmEntry& entry : glm.entries()) {
        const std::string& n = entry.name;
        if (n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
            return &entry;
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------------
// Terrain
// ------------------------------------------------------------------------------------------
float worldHeight(const TerrainHeader& header, uint16_t word) {
    const float half = header.bounds.max.y;
    return float(heightFixed13(word)) * (2.0f * half) / 8192.0f - half;
}

const LayerBlend& sampleLayer(const TerrainSample& sample, int index) {
    switch (index) {
        case 0: return sample.layer0;
        case 1: return sample.layer1;
        case 2: return sample.layer2;
        default: return sample.layer3;
    }
}

uint8_t slotCoverage(const TerrainSample& sample, int index) {
    const uint8_t layer = sampleLayer(sample, index).layerIndex;
    // The weight of the layer is that of its first slot (FUN_0072aa20).
    float weight = 0.0f;
    for (int k = 0; k < 4; ++k)
        if (sampleLayer(sample, k).layerIndex == layer) {
            weight = sampleLayer(sample, k).weight * (1.0f / 255.0f);
            break;
        }
    if (layer == 0 || weight <= 0.001f) return 0;     // slots naming layer 0 only feed the base mask
    for (int k = 0; k < 4; ++k)
        if (sampleLayer(sample, k).layerIndex > layer) weight *= 1.0f - sampleLayer(sample, k).weight * (1.0f / 255.0f);
    if (weight <= 0.001f) return 0;
    return static_cast<uint8_t>(std::min(255, static_cast<int>(weight * 255.0f + 0.5f)));   // truncating conversion
}

uint32_t Terrain::boundedRows() const {
    const auto n = uint32_t(std::lround((header.bounds.max.z - header.bounds.min.z) / header.spacing)) + 1;
    return std::min(n, rows);
}

uint32_t Terrain::boundedColumns() const {
    const auto n = uint32_t(std::lround((header.bounds.max.x - header.bounds.min.x) / header.spacing)) + 1;
    return std::min(n, columns);
}

bool Terrain::heightAtWorld(float x, float z, float& height) const {
    const float fc = (x - header.bounds.min.x) / header.spacing;
    const float fr = (z - header.bounds.min.z) / header.spacing;
    if (!(fc >= 0.0f && fr >= 0.0f)) return false;
    const auto c0 = static_cast<uint32_t>(fc);
    const auto r0 = static_cast<uint32_t>(fr);
    if (c0 + 1 >= columns || r0 + 1 >= rows) return false;
    if (!hasData(r0, c0) || !hasData(r0 + 1, c0) || !hasData(r0, c0 + 1) || !hasData(r0 + 1, c0 + 1)) return false;
    const float tc = fc - float(c0);
    const float tr = fr - float(r0);
    const float top = worldHeightAt(r0, c0) * (1.0f - tc) + worldHeightAt(r0, c0 + 1) * tc;
    const float bottom = worldHeightAt(r0 + 1, c0) * (1.0f - tc) + worldHeightAt(r0 + 1, c0 + 1) * tc;
    height = top * (1.0f - tr) + bottom * tr;
    return true;
}

Terrain readTerrain(const GlmArchive& glm) {
    const GlmEntry* thd = findSuffix(glm, ".thd");
    if (!thd) throw FormatError("archive has no .thd terrain header");
    const size_t underscore = thd->name.rfind('_');
    const size_t dot = thd->name.rfind('.');
    if (underscore == std::string::npos || dot == std::string::npos || dot <= underscore + 1)
        throw FormatError("unexpected terrain header name " + thd->name);
    const std::string prefix = thd->name.substr(0, underscore);

    Terrain terrain;
    terrain.terrainId = uint32_t(std::stoul(thd->name.substr(underscore + 1, dot - underscore - 1)));
    const Bytes headerBytes = glm.read(*thd);
    ByteReader r(headerBytes);
    TerrainHeader& h = terrain.header;
    h.version = r.u32();
    h.terrainFormat = r.u32();
    h.unknownGridX = r.u32();
    h.unknownGridZ = r.u32();
    h.bounds.min = r.vec3();
    h.bounds.max = r.vec3();
    h.spacing = r.f32();
    h.samplesPerPatchX = r.u32();
    h.samplesPerPatchZ = r.u32();
    h.patchesX = r.u32();
    h.patchesZ = r.u32();
    if (h.samplesPerPatchX < 2 || h.samplesPerPatchZ < 2 || h.patchesX == 0 || h.patchesZ == 0 ||
        h.patchesX > 1024 || h.patchesZ > 1024 || h.spacing <= 0.0f)
        throw FormatError("implausible terrain header");

    const uint32_t sx = h.samplesPerPatchX;
    const uint32_t sz = h.samplesPerPatchZ;
    // Patches do not share edge samples: patch n starts samplesPerPatch samples after its neighbour
    // (confirmed by in-game heights: 11 of 12 Bootcamp readings within 0.03 m).
    terrain.columns = h.patchesX * sx;
    terrain.rows = h.patchesZ * sz;
    const size_t count = size_t(terrain.rows) * terrain.columns;
    terrain.samples.resize(count);
    terrain.sampleHasData.assign(count, 0);
    terrain.patchPresent.assign(size_t(h.patchesX) * h.patchesZ, 0);

    char name[512];
    for (uint32_t n = 0; n < h.patchesX * h.patchesZ; ++n) {
        std::snprintf(name, sizeof name, "%s_%x_%u.tpc", prefix.c_str(), n, terrain.terrainId);
        const GlmEntry* entry = glm.find(name);
        if (!entry) continue;                                  // absent patches carry no data
        const Bytes patch = glm.read(*entry);
        if (patch.size() < 4 + size_t(sx) * sz * 12) throw FormatError(std::string(name) + ": too small");
        terrain.patchPresent[n] = 1;
        const uint32_t row0 = (n / h.patchesX) * sz;
        const uint32_t column0 = (n % h.patchesX) * sx;
        const uint8_t* p = patch.data() + 4;
        for (uint32_t i = 0; i < sz; ++i) {
            for (uint32_t j = 0; j < sx; ++j, p += 12) {
                const size_t index = size_t(row0 + i) * terrain.columns + column0 + j;
                TerrainSample& s = terrain.samples[index];
                s.word = uint16_t(p[0] | (p[1] << 8));
                s.layer0 = {p[2], p[3]};
                s.layer1 = {p[4], p[5]};
                s.layer2 = {p[6], p[7]};
                s.layer3 = {p[8], p[9]};
                terrain.sampleHasData[index] = 1;
            }
        }
    }
    return terrain;
}

void writeTerrainObj(const Terrain& terrain, const std::string& path, uint32_t step, bool rightHanded) {
    if (step == 0) step = 1;
    const uint32_t boundedRows = terrain.boundedRows();
    const uint32_t boundedColumns = terrain.boundedColumns();
    const uint32_t rows = (boundedRows - 1) / step + 1;
    const uint32_t columns = (boundedColumns - 1) / step + 1;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) throw FormatError("cannot write " + path);
    const float zSign = rightHanded ? -1.0f : 1.0f;
    std::fprintf(f, "# Tabula Rasa terrain, step %u, %ux%u vertices, %s-handed\n", step, rows, columns, rightHanded ? "right" : "left");
    for (uint32_t i = 0; i < rows; ++i)
        for (uint32_t j = 0; j < columns; ++j)
            std::fprintf(f, "v %.3f %.3f %.3f\n", terrain.worldX(j * step), terrain.worldHeightAt(i * step, j * step),
                         zSign * terrain.worldZ(i * step));
    for (uint32_t i = 0; i < rows; ++i)
        for (uint32_t j = 0; j < columns; ++j)
            std::fprintf(f, "vt %.5f %.5f\n", double(j * step) / (boundedColumns - 1), double(i * step) / (boundedRows - 1));
    for (uint32_t i = 0; i + 1 < rows; ++i) {
        for (uint32_t j = 0; j + 1 < columns; ++j) {
            if (!terrain.hasData(i * step, j * step) || !terrain.hasData((i + 1) * step, j * step) ||
                !terrain.hasData(i * step, (j + 1) * step) || !terrain.hasData((i + 1) * step, (j + 1) * step))
                continue;                                      // no geometry where patches are absent
            const uint32_t a = i * columns + j + 1;            // OBJ indices are 1-based
            const uint32_t b = a + 1;
            const uint32_t c = a + columns;
            const uint32_t d = c + 1;
            if (rightHanded) {
                std::fprintf(f, "f %u/%u %u/%u %u/%u\n", a, a, d, d, c, c);
                std::fprintf(f, "f %u/%u %u/%u %u/%u\n", a, a, b, b, d, d);
            } else {
                std::fprintf(f, "f %u/%u %u/%u %u/%u\n", a, a, c, c, d, d);
                std::fprintf(f, "f %u/%u %u/%u %u/%u\n", a, a, d, d, b, b);
            }
        }
    }
    std::fclose(f);
}

std::vector<TextureLayer> readTextureLayers(const GlmArchive& glm) {
    const GlmEntry* entry = findSuffix(glm, "_texlayers.tab");
    if (!entry) throw FormatError("archive has no _texlayers.tab");
    const Bytes data = glm.read(*entry);
    ByteReader r(data);
    const uint32_t version = r.u32();
    if (version < 3 || version > 4) throw FormatError("texlayers.tab: version " + std::to_string(version));
    const uint32_t count = r.u32();
    std::vector<TextureLayer> layers;
    for (uint32_t k = 0; k < count; ++k) {
        TextureLayer layer;
        layer.name = r.cstr();
        layer.texture = r.cstr();
        if (version >= 4) {
            layer.normalMap = r.cstr();
            layer.extraMap = r.cstr();
        }
        layer.tiling = r.f32();
        const uint32_t sets = r.u32();
        for (uint32_t s = 0; s < sets; ++s) layer.tribbleSets.push_back(r.cstr());
        layers.push_back(std::move(layer));
    }
    if (r.remaining() != 0) throw FormatError("texlayers.tab: trailing bytes");
    return layers;
}

std::vector<TribbleSet> readTribbleSets(const GlmArchive& glm) {
    const GlmEntry* entry = findSuffix(glm, "_tribbleset.tab");
    if (!entry) return {};                     // optional: 16 of the 66 shipped terrains have none
    const Bytes data = glm.read(*entry);
    ByteReader r(data);
    r.u32();                                   // version (1)
    const uint32_t count = r.u32();
    std::vector<TribbleSet> sets;
    for (uint32_t k = 0; k < count; ++k) {
        TribbleSet set;
        set.name = r.cstr();
        set.format = r.u32();
        if (set.format < 1 || set.format > 5) throw FormatError("tribbleset.tab: unknown set format " + std::to_string(set.format));
        const uint32_t models = r.u32();
        for (uint32_t m = 0; m < models; ++m) {
            TribbleModel model;
            model.model = r.cstr();
            const float density = r.f32();
            model.density = density < 0.001f ? 0.001f : density > 100.0f ? 100.0f : density;
            model.drawDistance = std::max(r.f32(), 1.0f);
            model.orientation = r.u32();
            model.randomYaw = r.u32();
            model.heightMin = r.f32();
            model.heightMax = r.f32();
            // FUN_009125f0: each slope limit clamped to 0 .. pi/2.
            auto slope = [](float s) { return s < 0.0f ? 0.0f : double(s) > 1.5707963705062866 ? 1.5707963705062866f : s; };
            model.slopeMin = slope(r.f32());
            model.slopeMax = slope(r.f32());
            if (set.format >= 2) {
                model.scaleMin = r.f32();
                model.scaleMax = r.f32();
            }
            model.flags = r.u32() != 0 ? 1u : 0u;
            model.seed = r.u32();
            if (set.format == 3) model.unknown54 = r.u32() != 0 ? 1 : 0;
            else if (set.format >= 4) model.unknown54 = r.u8() != 0 ? 1 : 0;
            if (set.format == 5) {
                model.unknown24 = r.f32();
                model.unknown28 = r.f32();
            }
            set.models.push_back(std::move(model));
        }
        sets.push_back(std::move(set));
    }
    if (r.remaining() != 0) throw FormatError("tribbleset.tab: trailing bytes");
    return sets;
}

namespace {

// TRasa::BasicRandomGenerator: Park-Miller "minimal standard" with multiplier 48271 (Schrage's method,
// FUN_00957260), value = state * 2^-31 (FUN_0074b8f0; the scale 0x30000000 at 0x00c2b764).
struct TribbleRandom {
    int32_t state;
    explicit TribbleRandom(int32_t seed) : state(seed == 0 ? 0x156 : seed) {}
    double next() {
        const int32_t hi = state / 44488, lo = state % 44488;
        int32_t t = int32_t(int64_t(48271) * lo - int64_t(3399) * hi);
        if (t < 0) t += 0x7fffffff;
        state = t;
        return double(t % 0x7fffffff) * (1.0 / 2147483648.0);
    }
};

// The 28-byte cell record (FUN_00565170 / FUN_00565010) and the normal the block keeps beside it (FUN_0090b520).
struct TribbleRecord {
    Vec3 position;
    Vec3 normal;
    bool clutter = false;                      // flag 2 (record + 0xE)
    const TerrainSample* sample = nullptr;
};

Vec3 tribbleSamplePosition(const Terrain& terrain, uint32_t row, uint32_t column) {
    return Vec3{terrain.worldX(column), terrain.worldHeightAt(row, column), terrain.worldZ(row)};
}

Vec3 cross3(const Vec3& a, const Vec3& b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize3(const Vec3& v) {                // D3DXVec3Normalize: zero stays zero
    const float l = std::sqrt(dot3(v, v));
    return l > 0.0f ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{};
}

// The terrain object's grid size (FUN_0090bc70, .thd parser): W = trunc((max.x - min.x) / spacing + 0.1), H likewise
// in z - e.g. 2048 on Concordia, whose patches give 2176 columns. Sample lookups outside it fail (FUN_0056aa20).
int64_t tribbleGridColumns(const Terrain& terrain) {
    const double inv = 1.0 / double(terrain.header.spacing);
    return std::min<int64_t>(int64_t(double(terrain.header.bounds.max.x - terrain.header.bounds.min.x) * inv + 0.1), terrain.columns);
}
int64_t tribbleGridRows(const Terrain& terrain) {
    const double inv = 1.0 / double(terrain.header.spacing);
    return std::min<int64_t>(int64_t(double(terrain.header.bounds.max.z - terrain.header.bounds.min.z) * inv + 0.1), terrain.rows);
}

// FUN_0090b520: sum over the 8 neighbours (in this order) of v x d, d = neighbour - centre, v = (-d.z, 0, d.x),
// normalised. A neighbour without data leaves the previously fetched position in place (FUN_0056aa20 writes nothing).
Vec3 tribbleNormal(const Terrain& terrain, uint32_t row, uint32_t column) {
    static const int offsets[8][2] = {{-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}};   // (dx, dz)
    Vec3 last;
    if (terrain.hasData(row, column)) last = tribbleSamplePosition(terrain, row, column);
    const Vec3 centre = last;
    Vec3 sum;
    for (const auto& o : offsets) {
        const int64_t x = int64_t(column) + o[0], z = int64_t(row) + o[1];
        if (x < 0 || z < 0 || x >= tribbleGridColumns(terrain) || z >= tribbleGridRows(terrain)) continue;
        if (terrain.hasData(uint32_t(z), uint32_t(x))) last = tribbleSamplePosition(terrain, uint32_t(z), uint32_t(x));
        const Vec3 d{last.x - centre.x, last.y - centre.y, last.z - centre.z};
        const Vec3 c = cross3(Vec3{-d.z, 0.0f, d.x}, d);
        sum = Vec3{sum.x + c.x, sum.y + c.y, sum.z + c.z};
    }
    return normalize3(sum);
}

// FUN_0074d010: the four (layer, weight / 255) pairs sorted by layer, then weight (insertion sort FUN_0074e8c0);
// the first pair of `layer` times (1 - weight) of every pair after it.
float tribbleLayerWeight(const TerrainSample& sample, uint8_t layer) {
    struct Pair { uint8_t layer; float weight; };
    Pair pairs[4];
    bool found = false;
    for (int k = 0; k < 4; ++k) {
        pairs[k] = Pair{sampleLayer(sample, k).layerIndex, float(sampleLayer(sample, k).weight * (1.0 / 255.0))};
        if (pairs[k].layer == layer) found = true;
    }
    if (!found) return 0.0f;
    std::stable_sort(pairs, pairs + 4, [](const Pair& a, const Pair& b) { return a.layer < b.layer || (a.layer == b.layer && a.weight < b.weight); });
    int i = 0;
    while (pairs[i].layer != layer) ++i;
    float w = pairs[i].weight;
    for (int k = i + 1; k < 4; ++k) w *= 1.0f - pairs[k].weight;
    return w;
}

// The terrain coverage test FUN_0074bc20 uses to decide which layers a cell has (FUN_0072aa20 + FUN_0093fdd0, as
// slotCoverage without the rounding).
bool tribbleLayerPresent(const TerrainSample& sample, int index) {
    const uint8_t layer = sampleLayer(sample, index).layerIndex;
    if (layer == 0) return false;
    float weight = 0.0f;
    for (int k = 0; k < 4; ++k)
        if (sampleLayer(sample, k).layerIndex == layer) {
            weight = sampleLayer(sample, k).weight * (1.0f / 255.0f);
            break;
        }
    if (weight <= 0.001f) return false;
    for (int k = 0; k < 4; ++k)
        if (sampleLayer(sample, k).layerIndex > layer) weight *= 1.0f - sampleLayer(sample, k).weight * (1.0f / 255.0f);
    return weight > 0.001f;
}

// 10 % fade at each end of a band (the same in the height and slope tests of FUN_0074c3a0).
float tribbleBandFade(float t, bool fadeLow) {
    if (fadeLow && 0.1 > double(t)) return float(double(t) * 10.0);
    if (double(t) > 0.9) return float((1.0 - double(t)) * 10.0);
    return 1.0f;
}

Quat d3dxRotationAxis(const Vec3& axis, float angle) {
    const Vec3 a = normalize3(axis);
    const float s = std::sin(angle * 0.5f);
    return Quat{a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
}

Quat d3dxMultiply(const Quat& q1, const Quat& q2) {       // D3DXQuaternionMultiply: rotation q1 followed by q2
    return Quat{q2.w * q1.x + q2.x * q1.w + q2.y * q1.z - q2.z * q1.y,
                q2.w * q1.y - q2.x * q1.z + q2.y * q1.w + q2.z * q1.x,
                q2.w * q1.z + q2.x * q1.y - q2.y * q1.x + q2.z * q1.w,
                q2.w * q1.w - q2.x * q1.x - q2.y * q1.y - q2.z * q1.z};
}

}  // namespace

uint32_t tribbleCellsX(const Terrain& terrain) {
    // FUN_0073b2d0: the column of the bounds' far edge (rounded), clamped to W - 1, / 32, + 1.
    const auto column = int64_t(double(terrain.header.bounds.max.x - terrain.header.bounds.min.x) / terrain.header.spacing + 0.5);
    return uint32_t(std::clamp<int64_t>(column, 0, std::max<int64_t>(tribbleGridColumns(terrain) - 1, 0)) >> 5) + 1;
}

uint32_t tribbleCellsZ(const Terrain& terrain) {
    const auto row = int64_t(double(terrain.header.bounds.max.z - terrain.header.bounds.min.z) / terrain.header.spacing + 0.5);
    return uint32_t(std::clamp<int64_t>(row, 0, std::max<int64_t>(tribbleGridRows(terrain) - 1, 0)) >> 5) + 1;
}

std::vector<TribbleInstance> placeTribbleCell(const Terrain& terrain, const std::vector<TextureLayer>& layers,
                                              const std::vector<TribbleSet>& sets, uint32_t cellIndex) {
    std::vector<TribbleInstance> out;
    if (terrain.columns == 0 || terrain.rows == 0 || sets.empty()) return out;
    struct Container {
        std::string mesh;
        uint32_t seed;
        float drawDistance;
        std::vector<TribbleInstance> instances;
    };
    std::vector<Container> containers;
    // FUN_0073c830: the cell's rectangle, 33 x 33 samples (edges shared with the neighbours), clamped to the grid
    // (FUN_0094ae30).
    const uint32_t cellsX = tribbleCellsX(terrain);
    const int64_t gridW = tribbleGridColumns(terrain), gridH = tribbleGridRows(terrain);
    if (gridW <= 0 || gridH <= 0) return out;
    const int64_t x0 = std::min<int64_t>(32 * int64_t(cellIndex % cellsX), gridW - 1);
    const int64_t z0 = std::min<int64_t>(32 * int64_t(cellIndex / cellsX), gridH - 1);
    const int64_t x1 = std::min<int64_t>(x0 + 32, gridW - 1);
    const int64_t z1 = std::min<int64_t>(z0 + 32, gridH - 1);
    const int64_t width = x1 - x0 + 1;
    std::vector<TribbleRecord> records(size_t(width * (z1 - z0 + 1)));
    for (int64_t z = z0; z <= z1; ++z)
        for (int64_t x = x0; x <= x1; ++x) {
            TribbleRecord& rec = records[size_t((z - z0) * width + (x - x0))];
            rec.sample = &terrain.sampleAt(uint32_t(z), uint32_t(x));
            rec.position = tribbleSamplePosition(terrain, uint32_t(z), uint32_t(x));
            rec.normal = tribbleNormal(terrain, uint32_t(z), uint32_t(x));
            rec.clutter = terrain.hasData(uint32_t(z), uint32_t(x)) && terrainFlag(rec.sample->word, 2);
        }
    auto record = [&](int64_t x, int64_t z) -> const TribbleRecord& { return records[size_t((z - z0) * width + (x - x0))]; };

    // FUN_0074bc20: the layers present anywhere in the rectangle, ascending; per layer its texlayers entry's sets.
    bool present[256] = {};
    for (const TribbleRecord& rec : records)
        for (int k = 0; k < 4; ++k)
            if (tribbleLayerPresent(*rec.sample, k)) present[sampleLayer(*rec.sample, k).layerIndex] = true;
    const float spacing = terrain.header.spacing;
    for (int layer = 1; layer < 256; ++layer) {
        if (!present[layer] || size_t(layer) > layers.size()) continue;
        for (const std::string& setName : layers[size_t(layer - 1)].tribbleSets) {
            size_t setIndex = 0;
            while (setIndex < sets.size() && sets[setIndex].name != setName) ++setIndex;
            if (setIndex == sets.size()) continue;
            const TribbleSet& set = sets[setIndex];
            for (size_t m = 0; m < set.models.size(); ++m) {
                const TribbleModel& model = set.models[m];
                TribbleRandom random(int32_t(cellIndex + model.seed));              // FUN_0074c2c0
                for (int64_t z = z0; z <= z1; ++z)
                    for (int64_t x = x0; x <= x1; ++x) {
                        // FUN_0074c3a0: how many instances this sample gets.
                        const TribbleRecord& rec = record(x, z);
                        if (!rec.clutter) continue;
                        const float weight = tribbleLayerWeight(*rec.sample, uint8_t(layer));
                        if (0.001f > weight) continue;
                        float heightFade = 1.0f;
                        if (1e-5f < model.heightMin) {
                            const float above = rec.position.y - model.heightMin, band = model.heightMax - model.heightMin;
                            if (above < 0.0f || band <= above) continue;
                            heightFade = tribbleBandFade(above / band, true);
                            if (heightFade == 0.0f) continue;
                        }
                        const float ny = std::clamp(rec.normal.y, 0.0f, 1.0f);
                        const float angle = std::acos(ny) - model.slopeMin, slopeBand = model.slopeMax - model.slopeMin;
                        if (angle < 0.0f || slopeBand <= angle) continue;
                        const float slopeFade = tribbleBandFade(angle / slopeBand, model.slopeMin != 0.0f);
                        if (slopeFade == 0.0f) continue;
                        const float expected = float(double(model.density) * weight * heightFade * slopeFade * double(spacing * spacing));
                        int count = int(expected);
                        const double r = random.next();
                        if (double(float(expected - float(count))) >= r) ++count;

                        // FUN_0074c690: place them. Offsets reach a whole sample each way; a draw towards a missing
                        // neighbour (the rectangle's edge) is dropped.
                        const bool hasLeft = x != x0, hasRight = x != x1, hasBack = z != z0, hasFront = z != z1;
                        auto neighbour = [&](int index) -> const TribbleRecord& { return record(x + index % 3 - 1, z + index / 3 - 1); };
                        for (; count > 0; --count) {
                            float rx = float(random.next() - 0.5);
                            float rz = float(random.next() - 0.5);
                            if ((!hasLeft && rx < 0.0f) || (!hasRight && rx > 0.0f) || (!hasBack && rz < 0.0f) || (!hasFront && rz > 0.0f)) continue;
                            // 3 x 3 neighbourhood index = 3 (dz + 1) + (dx + 1); a = lerp(i0, i1, rz'), b = lerp(i2, i3, rz'),
                            // result = lerp(a, b, rx'). In the negative quadrants rx' = -2 rx starts at the far sample.
                            int i0, i1, i2, i3;
                            float tx, tz;
                            if (0.0f > rx) {
                                if (0.0f > rz) { i0 = 0; i1 = 3; i2 = 1; i3 = 4; tz = float(rz * -2.0); }
                                else { i0 = 3; i1 = 6; i2 = 4; i3 = 7; tz = rz + rz; }
                                tx = float(rx * -2.0);
                            } else {
                                if (0.0f > rz) { i0 = 1; i1 = 4; i2 = 2; i3 = 5; tz = float(rz * -2.0); }
                                else { i0 = 4; i1 = 7; i2 = 5; i3 = 8; tz = rz + rz; }
                                tx = rx + rx;
                            }
                            auto bilinear = [&](Vec3 TribbleRecord::*field) {
                                const Vec3& p0 = neighbour(i0).*field; const Vec3& p1 = neighbour(i1).*field;
                                const Vec3& p2 = neighbour(i2).*field; const Vec3& p3 = neighbour(i3).*field;
                                const Vec3 a{tz * (p1.x - p0.x) + p0.x, (p1.y - p0.y) * tz + p0.y, p0.z + (p1.z - p0.z) * tz};
                                const Vec3 b{(p3.x - p2.x) * tz + p2.x, p2.y + (p3.y - p2.y) * tz, p2.z + (p3.z - p2.z) * tz};
                                return Vec3{tx * (b.x - a.x) + a.x, a.y + (b.y - a.y) * tx, a.z + tx * (b.z - a.z)};
                            };
                            TribbleInstance instance;
                            instance.set = uint32_t(setIndex);
                            instance.model = uint32_t(m);
                            instance.layer = uint32_t(layer);
                            instance.position = bilinear(&TribbleRecord::position);
                            const Vec3 up{0.0f, 1.0f, 0.0f};
                            Vec3 normal = model.orientation == 0 || model.orientation == 1 ? up : normalize3(bilinear(&TribbleRecord::normal));
                            if (model.orientation == 1) {
                                const float tiltX = float((random.next() - 0.5) * 0.2);
                                const float tiltZ = float((random.next() - 0.5) * 0.2);
                                normal = normalize3(Vec3{tiltX + normal.x, normal.y + 0.0f, tiltZ + normal.z});
                            } else if (model.orientation == 2 || model.orientation == 4) {
                                if (model.orientation == 2) normal = normalize3(Vec3{normal.x, float(normal.y + 1.0), normal.z});
                                const Vec3 v{float(random.next() - 0.5), 0.0f, float(random.next() - 0.5)};
                                Vec3 c = cross3(v, normal);
                                const double length = random.next() * 0.15;
                                const float squared = float(double(c.x) * c.x + double(c.y) * c.y + double(c.z) * c.z);
                                const float s = squared > 0.0f ? float(length / std::sqrt(squared)) : 0.0f;
                                c = Vec3{s * c.x, c.y * s, s * c.z};
                                normal = normalize3(Vec3{c.x + normal.x, c.y + normal.y, c.z + normal.z});
                            }
                            float yaw = 0.0f;
                            if (model.randomYaw == 1) {
                                const double turn = random.next() * 3.1415927410125732;
                                yaw = float(turn + turn);
                            }
                            Quat rotation = d3dxRotationAxis(up, yaw);
                            // FUN_0043cf30 / FUN_0043ce70: each component equal within 0.01, absolute or relative.
                            auto close = [](float a, float b) {
                                const float d = std::fabs(a - b);
                                return d <= 0.0099999998f || d <= std::max(std::fabs(a), std::fabs(b)) * 0.0099999998f;
                            };
                            if (!(close(normal.x, up.x) && close(normal.y, up.y) && close(normal.z, up.z))) {
                                const Vec3 axis = normalize3(cross3(normal, up));
                                const float tilt = std::acos(up.x * normal.x + up.y * normal.y + up.z * normal.z);
                                rotation = d3dxMultiply(rotation, d3dxRotationAxis(axis, -tilt));
                            }
                            instance.rotation = rotation;
                            instance.scale = float(random.next() * (double(model.scaleMax) - model.scaleMin) + model.scaleMin);
                            // The container for this mesh (FUN_0074c140), created by the first entry using it.
                            size_t c = 0;
                            while (c < containers.size() && containers[c].mesh != model.model) ++c;
                            if (c == containers.size()) containers.push_back(Container{model.model, model.seed, model.drawDistance, {}});
                            instance.container = uint32_t(c);
                            instance.containerDrawDistance = containers[c].drawDistance;
                            containers[c].instances.push_back(instance);
                        }
                    }
            }
        }
    }
    // FUN_00751050: each container shuffles its instances once.
    for (Container& c : containers) {
        std::vector<TribbleInstance>& v = c.instances;
        const uint32_t n = uint32_t(v.size());
        TribbleRandom random(int32_t(cellIndex + c.seed));
        const float last = float(int32_t(n) - 1);
        for (uint32_t k = 0; k < n; ++k) {
            const float r = float(random.next());
            const auto other = uint32_t(int64_t(double(r) * double(last)));     // FISTP with truncation
            if (other != k && other < n) std::swap(v[k], v[other]);
        }
        for (uint32_t k = 0; k < n; ++k) {
            v[k].rank = k;
            v[k].containerSize = n;
            out.push_back(v[k]);
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------
// .map
// ------------------------------------------------------------------------------------------
namespace {

int32_t readCount(ByteReader& r, size_t minBytesEach, const char* what) {
    const int32_t count = r.i32();
    if (count < 0 || (minBytesEach != 0 && size_t(count) > r.remaining() / minBytesEach))
        throw FormatError(std::string("map: implausible ") + what + " count " + std::to_string(count));
    return count;
}

Quat readQuatWxyz(ByteReader& r) {
    Quat q;
    q.w = r.f32();
    q.x = r.f32();
    q.y = r.f32();
    q.z = r.f32();
    return q;
}

HueColour readHue(ByteReader& r) {
    HueColour c;
    c.red = r.u8();
    c.green = r.u8();
    c.blue = r.u8();
    c.alpha = r.u8();
    return c;
}

void readParticles(ByteReader& r, int32_t count, MapEntity& entity) {
    if (count < 0 || size_t(count) > r.remaining() / 21) throw FormatError("map: implausible particle count");
    for (int32_t k = 0; k < count; ++k) {
        MapParticle p;
        p.assetId = r.i32();
        p.connectionPoint = r.i32();
        p.direction = r.vec3();
        p.type = r.u8();
        entity.particles.push_back(p);
    }
}

}  // namespace

MapFile readMap(const Bytes& data) {
    ByteReader r(data);
    MapFile m;
    m.fileSize = data.size();
    m.minorVersion = r.u16();
    m.majorVersion = r.u16();
    if (m.majorVersion != 2 || m.minorVersion < 43 || m.minorVersion > 45)
        throw FormatError("map format " + std::to_string(m.majorVersion) + "." + std::to_string(m.minorVersion) +
                          " not supported (2.43 - 2.45 implemented; client/gamemap.py documents older ones)");
    m.templateVersion = r.i32();
    m.status = r.i32();                                                   // MapDesc

    const int32_t terrains = readCount(r, 4, "terrain");                  // Terrain
    for (int32_t k = 0; k < terrains; ++k) m.terrainIds.push_back(r.i32());

    m.terrainVis.detailLevel = r.i32();                                   // TerrainVisSettings
    m.terrainVis.algorithm = r.i32();
    m.terrainVis.maxDistance = r.f32();
    m.terrainVis.fadeDistance = r.f32();

    if (m.minorVersion >= 45) {                                           // Entities, loader 11
        const int32_t count = readCount(r, 88, "entity");
        m.entities.reserve(size_t(count));
        for (int32_t k = 0; k < count; ++k) {
            MapEntity e;
            e.classId = r.i32();
            const uint32_t low = r.u32();
            const uint32_t high = r.u32();
            e.entityId = makeEntityId(low, high);
            e.location = r.vec3();
            e.orientation = readQuatWxyz(r);
            e.hue1 = readHue(r);
            e.hue2 = readHue(r);
            e.scale = r.f32();
            e.flags = r.i32();
            e.cullLayer = r.i32();
            const int32_t particles = r.i32();
            readParticles(r, particles, e);
            e.hasBounds = true;
            e.bounds.min = r.vec3();
            e.bounds.max = r.vec3();
            m.entities.push_back(std::move(e));
        }
    } else {                                                              // Entities, loader 10
        const int32_t entitySize = r.i32();
        if (entitySize < 60) throw FormatError("map: entity record smaller than 60 bytes");
        m.entityRecordSize = entitySize;
        const int32_t count = readCount(r, size_t(entitySize), "entity");
        m.entities.reserve(size_t(count));
        for (int32_t k = 0; k < count; ++k) {
            const size_t start = r.tell();
            MapEntity e;
            e.classId = r.i32();
            const uint32_t low = r.u32();
            const uint32_t high = r.u32();
            e.entityId = makeEntityId(low, high);
            e.location = r.vec3();
            e.orientation = readQuatWxyz(r);
            e.hue1 = readHue(r);
            e.scale = r.f32();
            e.flags = r.i32();
            e.cullLayer = r.i32();
            const int32_t particles = r.i32();
            e.recordTail = r.bytes(size_t(entitySize) - (r.tell() - start));
            r.seek(start + size_t(entitySize));
            readParticles(r, particles, e);
            if (r.i32() != 0) {                                           // isWater (2.37+)
                e.hasWater = true;
                e.waterVersion = r.i32();
                e.waterData = r.bytes(e.waterVersion == 1 ? 172 : e.waterVersion == 2 ? 44 : 0);
            }
            m.entities.push_back(std::move(e));
        }
    }

    const int32_t abvs = readCount(r, 21, "generated ABV");               // GeneratedABVs
    for (int32_t k = 0; k < abvs; ++k) {
        MapGeneratedAbv a;
        const uint32_t low = r.u32();
        const uint32_t high = r.u32();
        a.entityId = makeEntityId(low, high);
        a.shape = r.u8();
        a.extents = r.vec3();
        m.generatedAbvs.push_back(a);
    }

    m.dayLengthSeconds = r.i32();                                         // TimeSettings
    m.startingSkyTime = r.i32();

    const int32_t scripts = readCount(r, 12, "camera script");            // CameraScripts (version 2)
    for (int32_t k = 0; k < scripts; ++k) {
        MapCameraScript s;
        s.id = r.i32();
        s.returnTimeMs = r.i32();
        const int32_t frames = readCount(r, 44, "camera keyframe");
        for (int32_t f = 0; f < frames; ++f) {
            MapCameraKeyframe kf;
            kf.frameNumber = r.i32();
            kf.position = r.vec3();
            kf.rotation = readQuatWxyz(r);
            kf.focalLength = r.f32();
            kf.arriveTimeMs = r.i32();
            kf.holdTimeMs = r.i32();
            s.keyframes.push_back(kf);
        }
        m.cameraScripts.push_back(std::move(s));
    }

    const bool regionV3 = m.minorVersion >= 44;                           // RegionSettings
    const int32_t regions = readCount(r, regionV3 ? 32 : 40, "region");
    for (int32_t k = 0; k < regions; ++k) {
        MapRegion g;
        g.regionId = r.i32();
        g.priority = r.i32();
        g.enabledFlags = r.i32();
        g.ambient = r.i32();
        g.music = r.i32();
        if (!regionV3) {
            g.music2 = r.i32();
            g.music3 = r.i32();
        }
        g.environmentMap = r.i32();
        g.sky = r.i32();
        g.displayName = r.i32();
        m.regions.push_back(g);
    }
    // PointLights and AudioEmitters read nothing (lights.xml / audioemitters.xml).
    m.bytesConsumed = r.tell();
    if (m.bytesConsumed < data.size()) m.trailing.assign(data.begin() + std::ptrdiff_t(m.bytesConsumed), data.end());
    return m;
}

MapFile readMap(const std::string& path) { return readMap(readFile(path)); }

namespace {

struct ByteWriter {
    Bytes out;
    void raw(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }        // little-endian (x86), as ByteReader reads
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }           // the bits as read, so unchanged values round-trip exactly
    void vec3(const Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
    void quatWxyz(const Quat& q) { f32(q.w); f32(q.x); f32(q.y); f32(q.z); }
    void hue(const HueColour& c) { u8(c.red); u8(c.green); u8(c.blue); u8(c.alpha); }
    void bytes(const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); }
};

void writeParticles(ByteWriter& w, const MapEntity& e) {
    for (const MapParticle& p : e.particles) {
        w.i32(p.assetId);
        w.i32(p.connectionPoint);
        w.vec3(p.direction);
        w.u8(p.type);
    }
}

}  // namespace

Bytes writeMap(const MapFile& m) {
    if (m.majorVersion != 2 || m.minorVersion < 43 || m.minorVersion > 45)
        throw FormatError("writeMap: map format " + std::to_string(m.majorVersion) + "." + std::to_string(m.minorVersion) + " not supported");
    ByteWriter w;
    w.u16(m.minorVersion);
    w.u16(m.majorVersion);
    w.i32(m.templateVersion);
    w.i32(m.status);
    w.i32(int32_t(m.terrainIds.size()));
    for (int32_t id : m.terrainIds) w.i32(id);
    w.i32(m.terrainVis.detailLevel);
    w.i32(m.terrainVis.algorithm);
    w.f32(m.terrainVis.maxDistance);
    w.f32(m.terrainVis.fadeDistance);
    if (m.minorVersion >= 45) {
        w.i32(int32_t(m.entities.size()));
        for (const MapEntity& e : m.entities) {
            w.i32(e.classId);
            w.u32(entityIdLow(e.entityId));
            w.u32(entityIdHigh(e.entityId));
            w.vec3(e.location);
            w.quatWxyz(e.orientation);
            w.hue(e.hue1);
            w.hue(e.hue2);
            w.f32(e.scale);
            w.i32(e.flags);
            w.i32(e.cullLayer);
            w.i32(int32_t(e.particles.size()));
            writeParticles(w, e);
            w.vec3(e.bounds.min);
            w.vec3(e.bounds.max);
        }
    } else {
        // Loader 10: i32 record size, then the count.
        const int32_t recordSize = m.entityRecordSize >= 60 ? m.entityRecordSize : 60;
        w.i32(recordSize);
        w.i32(int32_t(m.entities.size()));
        for (const MapEntity& e : m.entities) {
            const size_t start = w.out.size();
            w.i32(e.classId);
            w.u32(entityIdLow(e.entityId));
            w.u32(entityIdHigh(e.entityId));
            w.vec3(e.location);
            w.quatWxyz(e.orientation);
            w.hue(e.hue1);
            w.f32(e.scale);
            w.i32(e.flags);
            w.i32(e.cullLayer);
            w.i32(int32_t(e.particles.size()));
            Bytes tail = e.recordTail;
            tail.resize(size_t(recordSize) - (w.out.size() - start), 0);
            w.bytes(tail);
            writeParticles(w, e);
            w.i32(e.hasWater ? 1 : 0);
            if (e.hasWater) {
                w.i32(e.waterVersion);
                w.bytes(e.waterData);
            }
        }
    }
    w.i32(int32_t(m.generatedAbvs.size()));
    for (const MapGeneratedAbv& a : m.generatedAbvs) {
        w.u32(entityIdLow(a.entityId));
        w.u32(entityIdHigh(a.entityId));
        w.u8(a.shape);
        w.vec3(a.extents);
    }
    w.i32(m.dayLengthSeconds);
    w.i32(m.startingSkyTime);
    w.i32(int32_t(m.cameraScripts.size()));
    for (const MapCameraScript& s : m.cameraScripts) {
        w.i32(s.id);
        w.i32(s.returnTimeMs);
        w.i32(int32_t(s.keyframes.size()));
        for (const MapCameraKeyframe& kf : s.keyframes) {
            w.i32(kf.frameNumber);
            w.vec3(kf.position);
            w.quatWxyz(kf.rotation);
            w.f32(kf.focalLength);
            w.i32(kf.arriveTimeMs);
            w.i32(kf.holdTimeMs);
        }
    }
    const bool regionV3 = m.minorVersion >= 44;
    w.i32(int32_t(m.regions.size()));
    for (const MapRegion& g : m.regions) {
        w.i32(g.regionId);
        w.i32(g.priority);
        w.i32(g.enabledFlags);
        w.i32(g.ambient);
        w.i32(g.music);
        if (!regionV3) {
            w.i32(g.music2);
            w.i32(g.music3);
        }
        w.i32(g.environmentMap);
        w.i32(g.sky);
        w.i32(g.displayName);
    }
    w.bytes(m.trailing);
    return w.out;
}

// ------------------------------------------------------------------------------------------
// .vis
// ------------------------------------------------------------------------------------------
namespace {

// Inflate one PVS row. Tolerates streams that end early (zero-padded and flagged as damaged).
std::vector<uint32_t> inflatePvsRow(const uint8_t* src, size_t size, size_t wordCount, bool& damaged) {
    std::vector<uint8_t> out(wordCount * 4, 0);
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) throw FormatError("zlib init failed");
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = uInt(size);
    zs.next_out = out.data();
    zs.avail_out = uInt(out.size());
    inflate(&zs, Z_FINISH);
    const size_t produced = out.size() - zs.avail_out;
    inflateEnd(&zs);
    damaged = produced != out.size();
    std::vector<uint32_t> words(wordCount);
    for (size_t k = 0; k < wordCount; ++k) words[k] = loadU32(out.data() + k * 4);
    return words;
}

}  // namespace

VisFile readVis(const Bytes& data) {
    ByteReader r(data);
    VisFile v;
    v.fileSize = data.size();
    v.streamFlags = r.u8();
    v.containerVersion = r.u32();
    if (v.containerVersion != 1) throw FormatError("vis: container version " + std::to_string(v.containerVersion));
    v.hasTree = r.u8() != 0;
    if (v.hasTree) {
        v.treeVersion = r.u32();
        v.nodeFormatVersion = r.u32();
        if (v.treeVersion < 1 || v.treeVersion > 3) throw FormatError("vis: tree version " + std::to_string(v.treeVersion));
        if (v.treeVersion < 3) throw FormatError("vis: tree versions below 3 (extra build parameters) not supported");
        if (v.nodeFormatVersion != 1) throw FormatError("vis: node format " + std::to_string(v.nodeFormatVersion));
        const uint32_t count = r.u32();
        if (size_t(count) > r.remaining() / 52) throw FormatError("vis: node count larger than file");
        v.nodes.reserve(count);
        for (uint32_t k = 0; k < count; ++k) {
            VisNode n;
            n.index = r.u32();
            n.parent = r.i32();
            n.child0 = r.i32();
            n.child1 = r.i32();
            n.leafIndex = r.u32();
            n.bounds.min = r.vec3();
            n.bounds.max = r.vec3();
            n.splitAxis = r.u32();
            n.planeD = r.f32();
            v.nodes.push_back(n);
        }
    }
    if (r.remaining() >= 1) v.hasPvs = r.u8() != 0;
    if (v.hasPvs) {
        v.pvsVersion = r.u32();
        v.leafCount = r.u32();
        if (v.pvsVersion < 1 || v.pvsVersion > 2) throw FormatError("vis: PVS version " + std::to_string(v.pvsVersion));
        if (size_t(v.leafCount) > r.remaining() / 12) throw FormatError("vis: leaf count larger than file");
        const size_t wordCount = (size_t(v.leafCount) + 31) / 32;
        v.rows.resize(v.leafCount);
        for (uint32_t k = 0; k < v.leafCount; ++k) {
            VisPvsRow& row = v.rows[k];
            row.dataSize = r.u32();
            row.timestamp = r.u64();
            row.compressed = v.pvsVersion == 2 ? r.u32() != 0 : true;
            if (row.dataSize == 0) continue;
            const size_t start = r.tell();
            r.skip(row.dataSize);
            const uint8_t* src = data.data() + start;
            if (row.compressed) {
                row.words = inflatePvsRow(src, row.dataSize, wordCount, row.damaged);
            } else {
                row.words.assign(wordCount, 0);
                for (size_t w = 0; w < wordCount && w * 4 + 4 <= row.dataSize; ++w) row.words[w] = loadU32(src + w * 4);
                row.damaged = row.dataSize < wordCount * 4;
            }
            if (row.damaged) ++v.damagedRows;
        }
        if (r.remaining() >= 1) {
            v.hasTrailingFlag = true;
            v.trailingFlag = r.u8();
        }
    }
    v.bytesConsumed = r.tell();                // shipped files have one more byte the client never reads
    return v;
}

VisFile readVis(const std::string& path) { return readVis(readFile(path)); }

int32_t findLeafNode(const VisFile& vis, const Vec3& point) {
    if (vis.nodes.empty()) return -1;
    const Bounds& root = vis.nodes[0].bounds;
    if (point.x < root.min.x || point.y < root.min.y || point.z < root.min.z ||
        point.x > root.max.x || point.y > root.max.y || point.z > root.max.z)
        return -1;
    int32_t index = 0;
    for (size_t guard = 0; guard <= vis.nodes.size(); ++guard) {
        const VisNode& node = vis.nodes[size_t(index)];
        if (node.isLeaf()) return index;
        if (node.child0 < 0 || node.child1 < 0 || size_t(node.child0) >= vis.nodes.size() || size_t(node.child1) >= vis.nodes.size())
            return -1;
        const float split = node.splitPosition();
        const bool child0IsLow = component(vis.nodes[size_t(node.child0)].bounds.max, node.splitAxis) <= split + 1e-3f;
        const bool pointIsLow = component(point, node.splitAxis) <= split;
        index = (pointIsLow == child0IsLow) ? node.child0 : node.child1;
    }
    return -1;
}
// ------------------------------------------------------------------------------------------
// .geo chunk files
// ------------------------------------------------------------------------------------------
std::string tagName(uint32_t tag) {
    const char text[5] = {char(tag >> 24), char(tag >> 16), char(tag >> 8), char(tag), 0};
    return text;
}

namespace {

struct Chunk {
    uint32_t tag = 0;
    uint32_t size = 0;
    uint32_t version = 0;
    size_t start = 0;
    size_t end = 0;
};

Chunk readChunk(ByteReader& r) {
    Chunk c;
    c.start = r.tell();
    c.tag = r.u32();
    c.size = r.u32();
    c.version = r.u32();
    r.u32();                                   // reserved, 0
    c.end = r.tell() + c.size;
    if (c.end > r.end()) throw FormatError("chunk " + tagName(c.tag) + " runs past the end of its data");
    return c;
}

Chunk expectChunk(ByteReader& r, uint32_t tag) {
    const Chunk c = readChunk(r);
    if (c.tag != tag)
        throw FormatError("expected " + tagName(tag) + " but found " + tagName(c.tag) +
                          " at offset " + std::to_string(c.start));
    return c;
}

void skipChunk(ByteReader& r) {
    const Chunk c = readChunk(r);
    r.seek(c.end);
}

struct Value4 {
    float x = 0, y = 0, z = 0, w = 0;
};

uint32_t elementSize(uint8_t type) {
    switch (type) {
        case 0: case 4: case 5: case 6: case 8: case 9: case 11: case 13: case 14: case 15: return 4;
        case 1: case 7: case 10: case 12: case 16: return 8;
        case 2: return 12;
        case 3: return 16;
        default: return 0;
    }
}

float halfToFloat(uint16_t half) {
    const uint32_t sign = uint32_t(half & 0x8000) << 16;
    uint32_t exponent = (half >> 10) & 0x1F;
    uint32_t mantissa = half & 0x3FF;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {                               // subnormal: normalise
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400) == 0) { mantissa <<= 1; --exponent; }
            bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000 | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

int16_t loadI16(const uint8_t* p) { return int16_t(uint16_t(p[0] | (p[1] << 8))); }
uint16_t loadU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
float loadF32(const uint8_t* p) {
    const uint32_t bits = loadU32(p);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// Decode one D3D9 vertex element (D3DDECLTYPE) into up to four floats.
Value4 decodeElement(uint8_t type, const uint8_t* p) {
    Value4 v;
    switch (type) {
        case 0: v.x = loadF32(p); break;
        case 1: v.x = loadF32(p); v.y = loadF32(p + 4); break;
        case 2: v.x = loadF32(p); v.y = loadF32(p + 4); v.z = loadF32(p + 8); break;
        case 3: v.x = loadF32(p); v.y = loadF32(p + 4); v.z = loadF32(p + 8); v.w = loadF32(p + 12); break;
        case 4:                                // D3DCOLOR: bytes B, G, R, A
            v.x = p[2] / 255.0f; v.y = p[1] / 255.0f; v.z = p[0] / 255.0f; v.w = p[3] / 255.0f; break;
        case 5: v.x = p[0]; v.y = p[1]; v.z = p[2]; v.w = p[3]; break;
        case 6: v.x = loadI16(p); v.y = loadI16(p + 2); break;
        case 7: v.x = loadI16(p); v.y = loadI16(p + 2); v.z = loadI16(p + 4); v.w = loadI16(p + 6); break;
        case 8: v.x = p[0] / 255.0f; v.y = p[1] / 255.0f; v.z = p[2] / 255.0f; v.w = p[3] / 255.0f; break;
        case 9: v.x = loadI16(p) / 32767.0f; v.y = loadI16(p + 2) / 32767.0f; break;
        case 10:
            v.x = loadI16(p) / 32767.0f; v.y = loadI16(p + 2) / 32767.0f;
            v.z = loadI16(p + 4) / 32767.0f; v.w = loadI16(p + 6) / 32767.0f; break;
        case 11: v.x = loadU16(p) / 65535.0f; v.y = loadU16(p + 2) / 65535.0f; break;
        case 12:
            v.x = loadU16(p) / 65535.0f; v.y = loadU16(p + 2) / 65535.0f;
            v.z = loadU16(p + 4) / 65535.0f; v.w = loadU16(p + 6) / 65535.0f; break;
        case 13: {                             // UDEC3: unsigned 10.10.10
            const uint32_t w = loadU32(p);
            v.x = float(w & 0x3FF); v.y = float((w >> 10) & 0x3FF); v.z = float((w >> 20) & 0x3FF); v.w = 1;
            break;
        }
        case 14: {                             // DEC3N: signed normalised 10.10.10
            const uint32_t w = loadU32(p);
            auto s10 = [](uint32_t x) { return int32_t(x << 22) >> 22; };
            v.x = float(s10(w & 0x3FF)) / 511.0f; v.y = float(s10((w >> 10) & 0x3FF)) / 511.0f;
            v.z = float(s10((w >> 20) & 0x3FF)) / 511.0f;
            v.w = 1;
            break;
        }
        case 15: v.x = halfToFloat(loadU16(p)); v.y = halfToFloat(loadU16(p + 2)); break;
        case 16:
            v.x = halfToFloat(loadU16(p)); v.y = halfToFloat(loadU16(p + 2));
            v.z = halfToFloat(loadU16(p + 4)); v.w = halfToFloat(loadU16(p + 6)); break;
        default: break;
    }
    return v;
}

uint32_t packColour(const Value4& c) {
    auto channel = [](float f) { return uint32_t(std::lround(std::min(std::max(f, 0.0f), 1.0f) * 255.0f)); };
    return (channel(c.w) << 24) | (channel(c.x) << 16) | (channel(c.y) << 8) | channel(c.z);
}

std::vector<VertexElement> readDecl(ByteReader& r) {
    const Chunk c = expectChunk(r, fourcc("DECL"));
    static const uint8_t usageLookupV1[16] = {0, 1, 2, 3, 4, 0xA, 0xA, 5, 6, 7, 8, 9, 0xA, 0xB, 0xC, 0xD};
    std::vector<VertexElement> layout;
    const uint32_t count = r.u32();
    uint32_t offset = 0;
    for (uint32_t k = 0; k < count; ++k) {
        VertexElement e;
        if (c.version == 1) {
            e.type = uint8_t(r.u16());
            e.method = uint8_t(r.u16());
            const uint16_t usage = r.u16();
            e.usage = usage < 16 ? usageLookupV1[usage] : 0xFF;
            e.usageIndex = uint8_t(r.u16());
        } else {
            e.type = r.u8();
            e.method = r.u8();
            e.usage = r.u8();
            e.usageIndex = r.u8();
        }
        e.offset = offset;
        offset += elementSize(e.type);
        layout.push_back(e);
    }
    r.seek(c.end);
    return layout;
}

void readVert(ByteReader& r, GeoPiece& piece) {
    const Chunk c = expectChunk(r, fourcc("VERT"));
    if (c.version == 3) r.u64();               // CRC (Auto Assault files)
    piece.layout = readDecl(r);
    const int32_t count = r.i32();
    uint32_t declared = 0;
    for (const VertexElement& e : piece.layout) declared += elementSize(e.type);
    const size_t available = c.end - r.tell();
    uint32_t stride = declared;
    if (count > 0 && size_t(count) * declared != available && available % size_t(count) == 0)
        stride = uint32_t(available / size_t(count));      // trust the chunk size over the layout
    if (count < 0 || size_t(count) * stride > available) throw FormatError("VERT: vertex data larger than its chunk");
    piece.stride = stride;
    piece.vertices.resize(size_t(count));
    const uint8_t* base = r.at(r.tell());
    for (int32_t i = 0; i < count; ++i) {
        GeoVertex& v = piece.vertices[size_t(i)];
        const uint8_t* p = base + size_t(i) * stride;
        for (const VertexElement& e : piece.layout) {
            if (e.offset + elementSize(e.type) > stride) continue;
            if (e.usageIndex != 0) {
                const Value4 value = decodeElement(e.type, p + e.offset);
                if (e.usage == 0 && e.usageIndex == 1) v.position1 = {value.x, value.y, value.z}; // POSITION1 (billboard centre)
                if (e.usage == 5 && e.usageIndex == 1) v.uv1 = {value.x, value.y};               // TEXCOORD1 (glow / decal maps)
                if (e.usage == 5 && e.usageIndex == 2) v.uv2 = {value.x, value.y};               // TEXCOORD2 (decal maps)
                if (e.usage == 5 && e.usageIndex == 3) v.uv3 = {value.x, value.y};               // TEXCOORD3 (water volume map)
                if (e.usage == 6 && e.usageIndex == 1) v.binormal = {value.x, value.y, value.z}; // TANGENT1 (binormal)
                continue;
            }
            const Value4 value = decodeElement(e.type, p + e.offset);
            switch (e.usage) {
                case 0: v.position = {value.x, value.y, value.z}; break;           // POSITION
                case 1: {                                                          // BLENDWEIGHT (FLOAT1..3)
                    const uint32_t explicitWeights = e.type <= 2 ? uint32_t(e.type) + 1 : 0;
                    if (explicitWeights == 0) break;
                    v.blendWeight0 = value.x;
                    v.blendWeight1 = explicitWeights >= 2 ? value.y : 1 - value.x;
                    v.blendWeight2 = explicitWeights >= 3 ? value.z : explicitWeights == 2 ? 1 - value.x - value.y : 0;
                    v.blendWeight3 = explicitWeights == 3 ? 1 - value.x - value.y - value.z : 0;
                    break;
                }
                case 2: v.blendIndices = loadU32(p + e.offset); break;            // BLENDINDICES (D3DCOLOR / UBYTE4)
                case 3: v.normal = {value.x, value.y, value.z}; break;             // NORMAL
                case 6: v.tangent = {value.x, value.y, value.z}; break;            // TANGENT0
                case 5: v.uv = {value.x, value.y}; break;                          // TEXCOORD0
                case 10: v.colour = e.type == 4 ? loadU32(p + e.offset) : packColour(value); break;  // COLOR0
                default: break;
            }
        }
    }
    r.seek(c.end);
}

std::vector<uint32_t> readIndx(ByteReader& r) {
    const Chunk c = expectChunk(r, fourcc("INDX"));
    if (c.version == 2) r.u64();               // CRC (Auto Assault files)
    const int32_t count = r.i32();
    const size_t available = c.end - r.tell();
    std::vector<uint32_t> indices;
    if (count > 0) {
        const bool wide = size_t(count) * 4 == available;   // 32-bit indices if the size says so
        if (!wide && size_t(count) * 2 > available) throw FormatError("INDX: index data larger than its chunk");
        indices.resize(size_t(count));
        for (int32_t k = 0; k < count; ++k) indices[size_t(k)] = wide ? r.u32() : r.u16();
    }
    r.seek(c.end);
    return indices;
}

Bounds readBbox(ByteReader& r) {
    const Chunk c = expectChunk(r, fourcc("BBOX"));
    if (c.version == 2) r.u8();                // isEmpty
    Bounds b;
    b.min = r.vec3();
    b.max = r.vec3();
    r.seek(c.end);                             // centre (vec3) and radius (f32) follow
    return b;
}

std::string readAssetId(ByteReader& r) {
    const Chunk c = readChunk(r);
    std::string id;
    if (c.tag == fourcc("ISTR")) id = r.cstr();
    else if (c.tag == fourcc("IINT")) id = std::to_string(r.i32());
    r.seek(c.end);                             // INON (no id) or an unknown kind
    return id;
}

std::string formatFloat(float f) {
    char text[32];
    std::snprintf(text, sizeof text, "%g", f);
    return text;
}

}  // namespace

bool isHuedEffect(const std::string& effectName) {
    // TRHair has no "Hue" in its name but is hued like the Pal*Hue effects (verified 2026-10-14, trhairsp_2.fxc): its Hue
    // parameter has the semantic HueColor (Hue2: HueColor2), and the deferred pixel shader ps2x_BaseDeferredAlphaTestNoParallaxHue
    // computes diffuse + Hue x diffuse.a before x vertex colour x 2 (cut-out: texkill(normal map alpha - 0.5)).
    return effectName.find("Hue") != std::string::npos || tr::toLowerAscii(effectName).rfind("trhair", 0) == 0;
}

namespace {

void readEfct(ByteReader& r, GeoPiece& piece) {
    const Chunk c = expectChunk(r, fourcc("EFCT"));
    if (c.version == 2) piece.effectName = r.cstr();
    else if (c.version >= 3) piece.effectName = readAssetId(r);
    while (r.tell() + 16 <= c.end && r.peekU32() == fourcc("PARM")) {
        const Chunk pc = readChunk(r);
        GeoParameter parameter;
        parameter.key = r.cstr();
        const int32_t type = r.i32();
        if (pc.version == 2) {
            switch (type) {
                case 1: parameter.value = r.u32() == 1 ? "true" : "false"; break;
                case 2: parameter.value = std::to_string(r.i32()); break;
                case 3: {
                    const int32_t n = r.i32();
                    for (int32_t k = 0; k < n; ++k) {
                        if (k) parameter.value += ' ';
                        parameter.value += formatFloat(r.f32());
                    }
                    break;
                }
                case 4: parameter.value = r.cstr(); break;
                case 5: case 8: parameter.value = readAssetId(r); break;
                default: break;
            }
        }
        r.seek(pc.end);
        if (parameter.key == "DiffuseTexture") piece.diffuseTexture = parameter.value;
        piece.parameters.push_back(std::move(parameter));
    }
    r.seek(c.end);
}

// USDA: u32 count, then count pairs of (cstr key, cstr value).
void readUserData(ByteReader& r, GeoPiece& piece) {
    const Chunk c = expectChunk(r, fourcc("USDA"));
    const uint32_t pairs = r.u32();
    for (uint32_t k = 0; k < pairs && r.tell() < c.end; ++k) {
        GeoParameter parameter;
        parameter.key = r.cstr();
        parameter.value = r.cstr();
        std::string key = parameter.key;
        for (char& ch : key) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (key == "lod") piece.lod = uint32_t(std::strtoul(parameter.value.c_str(), nullptr, 10));
        piece.userData.push_back(std::move(parameter));
    }
    r.seek(c.end);
}

GeoPiece readGpce(ByteReader& r) {
    const Chunk c = expectChunk(r, fourcc("GPCE"));
    GeoPiece piece;
    piece.version = c.version;
    if (c.version >= 9) r.u8();                // hasMaterial
    readEfct(r, piece);
    piece.indices = readIndx(r);
    if (c.version >= 10) {
        readVert(r, piece);
    } else {
        const int32_t streams = r.i32();
        readVert(r, piece);
        if (streams == 2) skipChunk(r);        // second VERT (purpose UNKNOWN); the first is kept
    }
    piece.bounds = readBbox(r);
    piece.boneName = r.cstr();
    if (c.version >= 8 && r.tell() + 8 <= c.end) {
        r.u32();                               // UNKNOWN, always 0
        piece.swapId = r.u32();
        piece.swapGroup = r.cstr();
        piece.pieceName = r.cstr();
        if (r.tell() + 16 <= c.end && r.peekU32() == fourcc("USDA")) readUserData(r, piece);
    }
    r.seek(c.end);                             // remaining fields, extra index buffers: skipped
    return piece;
}

GeoPiece readGskn(ByteReader& r) {
    const Chunk c = expectChunk(r, fourcc("GSKN"));
    GeoPiece piece = readGpce(r);
    piece.skinned = true;
    const uint32_t bones = r.u32();
    for (uint32_t k = 0; k < bones; ++k) piece.skinBones.push_back(r.cstr());
    r.seek(c.end);
    return piece;
}

std::vector<GeoBone> readSkeleton(ByteReader& r) {
    const Chunk c = expectChunk(r, fourcc("PSKE"));
    std::vector<GeoBone> bones;
    const uint32_t count = r.u32();
    for (uint32_t k = 0; k < count; ++k) {
        const Chunk bone = expectChunk(r, fourcc("PBON"));
        GeoBone b;
        b.parent = r.i32();
        r.i32();                               // UNKNOWN
        r.i32();                               // UNKNOWN
        r.f32();                               // UNKNOWN
        const Chunk data = expectChunk(r, fourcc("BDAT"));
        b.name = r.cstr();
        b.rotation.x = r.f32();
        b.rotation.y = r.f32();
        b.rotation.z = r.f32();
        b.rotation.w = r.f32();
        b.translation = r.vec3();
        if (data.version == 2) b.scale = r.vec3();
        if (r.tell() + 64 <= data.end) {       // inverse bind matrix, rows of 4 floats (4th column 0 0 0 1)
            b.hasInverseBind = true;
            b.inverseBindX = r.vec3(); r.f32();
            b.inverseBindY = r.vec3(); r.f32();
            b.inverseBindZ = r.vec3(); r.f32();
            b.inverseBindT = r.vec3(); r.f32();
        }
        r.seek(data.end);                      // optional bounding volume follows
        r.seek(bone.end);
        bones.push_back(std::move(b));
    }
    r.seek(c.end);
    return bones;
}

GeoModel readGbod(ByteReader& r) {
    const Chunk c = expectChunk(r, fourcc("GBOD"));
    GeoModel model;
    model.version = c.version;
    if (c.version == 3) r.f32();               // UNKNOWN
    if (r.peekU32() != fourcc("BBOX")) {       // shader name list (seen in Auto Assault files)
        const uint32_t n = r.u32();
        for (uint32_t k = 0; k < n; ++k) r.cstr();
    }
    model.bounds = readBbox(r);
    {
        const Chunk points = expectChunk(r, fourcc("CPDG"));     // contact points
        const uint32_t count = r.u32();
        for (uint32_t k = 0; k < count; ++k) {
            const Chunk point = expectChunk(r, fourcc("CPDF"));
            GeoContactPoint cp;
            cp.id = r.u32();
            cp.bone = r.i32();
            cp.rotation.x = r.f32();
            cp.rotation.y = r.f32();
            cp.rotation.z = r.f32();
            cp.rotation.w = r.f32();
            cp.position = r.vec3();
            const uint32_t pairs = r.u32();
            for (uint32_t p = 0; p < pairs; ++p) {
                GeoParameter value;
                value.key = r.cstr();
                value.value = r.cstr();
                cp.values.push_back(std::move(value));
            }
            r.seek(point.end);
            model.contactPoints.push_back(std::move(cp));
        }
        r.seek(points.end);
    }
    const uint32_t entries = r.u32();
    for (uint32_t k = 0; k < entries; ++k) {
        const int32_t id = r.i32();
        const uint32_t pairs = r.u32();
        for (uint32_t p = 0; p < pairs; ++p) {
            GeoProperty property;
            property.id = id;
            property.key = r.cstr();
            property.value = r.cstr();
            model.properties.push_back(std::move(property));
        }
    }
    model.bones = readSkeleton(r);
    const int32_t morphs = r.i32();
    if (morphs > 0) throw FormatError("GBOD: morph targets are not supported");
    const int32_t pieces = r.i32();
    for (int32_t k = 0; k < pieces; ++k) {
        const uint32_t tag = r.peekU32();
        if (tag == fourcc("GPCE")) model.pieces.push_back(readGpce(r));
        else if (tag == fourcc("GSKN")) model.pieces.push_back(readGskn(r));
        else skipChunk(r);
    }
    const int32_t shadows = r.i32();
    for (int32_t k = 0; k < shadows; ++k) skipChunk(r);
    model.shadowCount = uint32_t(std::max(shadows, 0));
    r.seek(c.end);                             // version >= 2: LOD handler flag and chunk
    return model;
}

Vec3 rotate(const Quat& q, const Vec3& v) {
    // v' = v + 2w (q x v) + 2 q x (q x v)
    const Vec3 t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y),
            v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

}  // namespace

GeoModel readGeo(const Bytes& data) {
    if (data.size() < 8 || std::memcmp(data.data(), "CHNK", 4) != 0) throw FormatError("not a chunk file (no CHNK header)");
    if (data[4] != 'B') throw FormatError("text chunk files are not supported");
    ByteReader r(data, 8);
    while (r.remaining() >= 16) {
        if (r.peekU32() == fourcc("GBOD")) return readGbod(r);
        skipChunk(r);
    }
    throw FormatError("no GBOD chunk found");
}

namespace {

// Which components a key chunk stores, and whether it is the compressed (half / int16) form.
struct KeyLayout {
    bool known = false;
    bool compressed = false;
    bool scale = false;
    bool rotation = false;
    bool translation = false;
    uint32_t size() const {
        return compressed ? (scale ? 6 : 0) + (rotation ? 8 : 0) + (translation ? 6 : 0) + 2
                          : (scale ? 12 : 0) + (rotation ? 16 : 0) + (translation ? 12 : 0) + 4;
    }
};

KeyLayout keyLayout(uint32_t tag) {
    struct Entry { uint32_t tag; bool compressed, scale, rotation, translation; };
    static const Entry table[] = {
        {fourcc("KEYT"), false, false, false, true},  {fourcc("KEYR"), false, false, true, false},
        {fourcc("KEYS"), false, true, false, false},  {fourcc("KERT"), false, false, true, true},
        {fourcc("KEST"), false, true, false, true},   {fourcc("KESR"), false, true, true, false},
        {fourcc("KSRT"), false, true, true, true},    {fourcc("XKER"), true, false, true, false},
        {fourcc("XKRT"), true, false, true, true},    {fourcc("XKSR"), true, true, true, false},
        {fourcc("XSRT"), true, true, true, true},
    };
    for (const Entry& e : table)
        if (e.tag == tag) return KeyLayout{true, e.compressed, e.scale, e.rotation, e.translation};
    return KeyLayout{};
}

Quat normalizedQuat(const Quat& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 0 ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
}

float readHalf(ByteReader& r) { return halfToFloat(r.u16()); }
float readSnorm16(ByteReader& r) { return float(int16_t(r.u16())) / 32767.0f; }

AnimKey readKey(ByteReader& r, const KeyLayout& layout) {
    AnimKey key;
    if (layout.scale) {
        key.scale.x = layout.compressed ? readHalf(r) : r.f32();
        key.scale.y = layout.compressed ? readHalf(r) : r.f32();
        key.scale.z = layout.compressed ? readHalf(r) : r.f32();
    }
    if (layout.rotation) {
        key.rotation.x = layout.compressed ? readSnorm16(r) : r.f32();
        key.rotation.y = layout.compressed ? readSnorm16(r) : r.f32();
        key.rotation.z = layout.compressed ? readSnorm16(r) : r.f32();
        key.rotation.w = layout.compressed ? readSnorm16(r) : r.f32();
    }
    if (layout.translation) {
        key.translation.x = layout.compressed ? readHalf(r) : r.f32();
        key.translation.y = layout.compressed ? readHalf(r) : r.f32();
        key.translation.z = layout.compressed ? readHalf(r) : r.f32();
    }
    key.time = layout.compressed ? readHalf(r) : r.f32();
    // 382 of 13.8 million keys are > 1% off unit length (int16 rounding of small rotations): normalise.
    if (layout.rotation) key.rotation = normalizedQuat(key.rotation);
    return key;
}

}  // namespace

namespace {
// tabula_rasa.exe 0x00d02b10: (name, id) pairs; 60 is not used.
const std::pair<uint32_t, const char*> kConnectionPointNames[] = {
    {0, "InvalidCP"}, {1, "BoneTracker1"}, {2, "BoneTracker2"}, {3, "BoneTracker3"}, {4, "BoneTracker4"}, {5, "Head"}, {6, "Mouth"},
    {7, "Left_Shoulder"}, {8, "Right_Shoulder"}, {9, "Right_Bicep"}, {10, "Right_Forearm"}, {11, "Right_Hand"}, {12, "Left_Bicep"},
    {13, "Left_Forearm"}, {14, "Left_Hand"}, {15, "Upper_Torso"}, {16, "Lower_Torso"}, {17, "Butt"}, {18, "Left_Thigh"},
    {19, "Right_Thigh"}, {20, "Left_Calf"}, {21, "Right_Calf"}, {22, "Left_Foot"}, {23, "Right_Foot"}, {24, "Face"}, {25, "Neck"},
    {26, "Terra"}, {27, "Arch"}, {28, "VFX1"}, {29, "VFX2"}, {30, "VFX3"}, {31, "VFX4"}, {32, "VFX5"}, {33, "Projectile"},
    {34, "Float"}, {35, "Access1"}, {36, "Power1"}, {37, "Switch1"}, {38, "Damage1"}, {39, "Damage2"}, {40, "Ground_Small"},
    {41, "Ground_Medium"}, {42, "Ground_Large"}, {43, "Floor_Small"}, {44, "Floor_Medium"}, {45, "Floor_Large"},
    {46, "Wall_Small"}, {47, "Wall_Medium"}, {48, "Wall_Large"}, {49, "Window_Drapery"}, {50, "Player_Vendor"},
    {51, "Player_Crafting"}, {52, "Tabletop_Small"}, {53, "Tabletop_Medium"}, {54, "Tabletop_Large"},
    {55, "Creature_Head_Small"}, {56, "Creature_Head_Medium"}, {57, "Creature_Head_Large"}, {58, "Lightbulb"}, {59, "Helmet"},
    {61, "Backpack"}, {62, "PackHolster"}, {63, "Left_Wrist_Object"}, {64, "Satchel"}, {65, "Sword"}, {66, "Pistol_Holster"},
    {67, "Thigh_Pack"}, {68, "LIT1"}, {69, "LIT2"}, {70, "LIT3"}, {71, "LIT4"}, {72, "LIT5"}, {73, "LIT6"}, {74, "LIT7"},
    {75, "LIT8"}, {76, "LIT9"}, {77, "LIT10"}, {78, "USABLE_SMALL"}, {79, "USABLE_MED"}, {80, "USABLE_LARGE"}, {81, "USABLE_TOP"},
    {82, "USABLE_BOTTOM"}, {83, "USABLE_FRONT"}, {84, "USABLE_BACK"}, {85, "USABLE_LEFT"}, {86, "USABLE_RIGHT"}, {87, "COG"},
    {88, "Camera_Proxy"}};
}  // namespace

ConnectionPointName parseConnectionPointName(const GeoContactPoint& cp) {
    ConnectionPointName out;
    const GeoParameter* name = nullptr;
    const GeoParameter* nameId = nullptr;
    for (const GeoParameter& p : cp.values) {      // the first cpNameId or cpName key, as the exe scans
        const std::string key = toLowerAscii(p.key);
        if (key == "cpnameid") { nameId = &p; break; }
        if (key == "cpname") { name = &p; break; }
    }
    if (nameId) {
        const std::string& v = nameId->value;
        if (v.size() < 3) return out;
        const char t = char(std::tolower(static_cast<unsigned char>(v[0]))), u = char(std::tolower(static_cast<unsigned char>(v[1])));
        out.type = t == 'h' ? 1 : t == 'p' ? 2 : t == 's' ? 3 : t == 'd' ? 4 : 0;
        out.usage = u == 's' ? 1 : u == 'p' ? 2 : 0;
        if (out.type == 0 || out.usage == 0) return out;
        out.id = uint32_t(std::strtol(v.c_str() + 2, nullptr, 10));
        out.valid = true;
        return out;
    }
    if (!name || name->value.size() < 5) return out;
    static const std::pair<const char*, std::pair<uint32_t, uint32_t>> prefixes[] = {
        {"hpp_", {1, 2}}, {"hps_", {1, 1}}, {"ppp_", {2, 2}}, {"pps_", {2, 1}}, {"spp_", {3, 2}}, {"sps_", {3, 1}}, {"dpp_", {4, 2}}, {"dps_", {4, 1}}};
    const std::string lower = toLowerAscii(name->value);
    for (const auto& prefix : prefixes) {
        if (lower.compare(0, 4, prefix.first) != 0) continue;
        const std::string rest = lower.substr(4);
        for (const auto& entry : kConnectionPointNames)
            if (toLowerAscii(entry.second) == rest) {
                out.type = prefix.second.first;
                out.usage = prefix.second.second;
                out.id = entry.first;
                out.valid = true;
                return out;
            }
        return out;
    }
    return out;
}

std::string connectionPointIdName(uint32_t id) {
    for (const auto& entry : kConnectionPointNames)
        if (entry.first == id) return entry.second;
    return "";
}

AnimationMarkers readAnimationMarkers(const Bytes& data) {
    if (data.size() < 8 || std::memcmp(data.data(), "CHNK", 4) != 0) throw FormatError("not a chunk file (no CHNK header)");
    if (data[4] != 'B') throw FormatError("text chunk files are not supported");
    ByteReader r(data, 8);
    const Chunk amrk = expectChunk(r, fourcc("AMRK"));
    AnimationMarkers result;
    result.version = amrk.version;
    const uint32_t count = r.u32();
    if (size_t(count) * 5 != amrk.end - r.tell())
        throw FormatError("AMRK holds " + std::to_string(amrk.end - r.tell()) + " bytes for " + std::to_string(count) + " markers");
    result.markers.reserve(count);
    for (uint32_t k = 0; k < count; ++k) {
        AnimMarker m;
        m.type = r.u8();
        m.timeMs = r.u32();
        result.markers.push_back(m);
    }
    return result;
}

const char* animMarkerTypeName(int type) {
    // tabula_rasa.exe 0x00820e00: switch over 0..60 (jump table 0x00821238), each case one name.
    static const char* const names[61] = {
        "Start",    "Strike",   "Footstep1", "WeaponSwing", "WeaponSwing2", "SubStrike", "Windup1", "Windup2", "Windup3", "Windup4",
        "Resolve1", "Resolve2", "Resolve3",  "Resolve4",    "ProjectileArrived", "Tick", "Usable1", "Usable2", "Usable3", "Usable4",
        "Script1",  "Script2",  "Script3",   "Script4",     "Stop", "ScaleBegin", "ScaleEnd", "PauseHeadTracking", "UnpauseHeadTracking",
        "PreStrike", "Footstep2", "Footstep3", "Footstep4", "Footstep5", "Footstep6", "Footstep7", "Footstep8", "Audio1", "Audio2", "Audio3",
        "Audio4", "Audio5", "Audio6", "Audio7", "Audio8", "Windup5", "Windup6", "Windup7", "Windup8", "Resolve5", "Resolve6", "Resolve7",
        "Resolve8", "Timer1", "Timer2", "Timer3", "Timer4", "Timer5", "Timer6", "Timer7", "Timer8"};
    return type >= 0 && type <= 60 ? names[type] : "unknown";
}

Animation readAnimation(const Bytes& data) {
    if (data.size() < 8 || std::memcmp(data.data(), "CHNK", 4) != 0) throw FormatError("not a chunk file (no CHNK header)");
    if (data[4] != 'B') throw FormatError("text chunk files are not supported");
    ByteReader r(data, 8);
    const Chunk anim = expectChunk(r, fourcc("ANIM"));
    Animation animation;
    animation.version = anim.version;
    const uint32_t tracks = r.u32();
    r.u8();                                    // UNKNOWN, always 0
    for (uint32_t t = 0; t < tracks; ++t) {
        r.u32();                               // UNKNOWN fourcc before each track
        const Chunk trak = expectChunk(r, fourcc("TRAK"));
        AnimTrack track;
        r.u8();                                // UNKNOWN, always 0
        track.target = r.u32();
        if (track.target == fourcc("EFCT")) {
            MaterialTrack m;
            m.group = r.cstr();
            m.parameter = r.cstr();
            r.u32();                           // UNKNOWN, always 0
            const uint32_t keyCount = r.u32();
            const Chunk keys = readChunk(r);
            if (keys.tag == fourcc("KEUV") && size_t(keyCount) * 32 == keys.size) {
                for (uint32_t k = 0; k < keyCount; ++k) {
                    MaterialKey key;
                    key.scaleU = r.f32();
                    key.scaleV = r.f32();
                    key.rotation.x = r.f32();
                    key.rotation.y = r.f32();
                    key.rotation.z = r.f32();
                    key.translateU = r.f32();
                    key.translateV = r.f32();
                    key.time = r.f32();
                    m.keys.push_back(key);
                }
                if (!m.keys.empty()) animation.duration = std::max(animation.duration, m.keys.back().time);
                animation.materialTracks.push_back(std::move(m));
            }
            r.seek(trak.end);
            continue;
        }
        if (track.target != fourcc("SKEL")) {
            r.seek(trak.end);
            continue;
        }
        track.name = r.cstr();
        r.u8();                                // UNKNOWN, always 0
        track.flags = r.u32();
        const uint32_t keyCount = r.u32();
        const Chunk keys = readChunk(r);
        const KeyLayout layout = keyLayout(keys.tag);
        if (!layout.known) throw FormatError("TRAK " + track.name + ": unknown key chunk " + tagName(keys.tag));
        if (size_t(keyCount) * layout.size() != keys.size)
            throw FormatError("TRAK " + track.name + ": " + tagName(keys.tag) + " size does not match its key count");
        track.keyKind = keys.tag;
        track.hasScale = layout.scale;
        track.hasRotation = layout.rotation;
        track.hasTranslation = layout.translation;
        track.keys.reserve(keyCount);
        for (uint32_t k = 0; k < keyCount; ++k) track.keys.push_back(readKey(r, layout));
        if (!track.keys.empty()) animation.duration = std::max(animation.duration, track.keys.back().time);
        animation.tracks.push_back(std::move(track));
        r.seek(trak.end);
    }
    return animation;
}

const AnimTrack* findTrack(const Animation& animation, const std::string& boneName) {
    for (const AnimTrack& t : animation.tracks)
        if (t.name == boneName) return &t;
    const std::string wanted = toLowerAscii(boneName);  // e.g. npc_eloh_hologram_large: CTRL_world (.anm) vs CTRL_World (.geo)
    for (const AnimTrack& t : animation.tracks)
        if (toLowerAscii(t.name) == wanted) return &t;
    return nullptr;
}

MaterialKey sampleMaterialTrack(const MaterialTrack& track, float time) {
    if (track.keys.empty()) return MaterialKey{};
    if (time <= track.keys.front().time) return track.keys.front();
    if (time >= track.keys.back().time) return track.keys.back();
    const auto next = std::upper_bound(track.keys.begin(), track.keys.end(), time,
                                       [](float t, const MaterialKey& k) { return t < k.time; });
    const MaterialKey& b = *next;
    const MaterialKey& a = *(next - 1);
    const float span = b.time - a.time;
    const float u = span > 0 ? (time - a.time) / span : 0.0f;
    auto mix = [u](float p, float q) { return p + (q - p) * u; };
    MaterialKey out;
    out.scaleU = mix(a.scaleU, b.scaleU);
    out.scaleV = mix(a.scaleV, b.scaleV);
    out.rotation = Vec3{mix(a.rotation.x, b.rotation.x), mix(a.rotation.y, b.rotation.y), mix(a.rotation.z, b.rotation.z)};
    out.translateU = mix(a.translateU, b.translateU);
    out.translateV = mix(a.translateV, b.translateV);
    out.time = time;
    return out;
}

void materialKeyTransform(const MaterialKey& key, Vec3& rowU, Vec3& rowV) {
    const float c = std::cos(key.rotation.z), s = std::sin(key.rotation.z);
    // u' = scaleU (c (u - 0.5) - s (v - 0.5)) + 0.5 + translateU; v' = scaleV (s (u - 0.5) + c (v - 0.5)) + 0.5 + translateV
    rowU = Vec3{key.scaleU * c, -key.scaleU * s, key.scaleU * (-0.5f * c + 0.5f * s) + 0.5f + key.translateU};
    rowV = Vec3{key.scaleV * s, key.scaleV * c, key.scaleV * (-0.5f * s - 0.5f * c) + 0.5f + key.translateV};
}

AnimKey sampleTrack(const AnimTrack& track, float time) {
    if (track.keys.empty()) return AnimKey{};
    if (time <= track.keys.front().time) return track.keys.front();
    if (time >= track.keys.back().time) return track.keys.back();
    const auto next = std::upper_bound(track.keys.begin(), track.keys.end(), time,
                                       [](float t, const AnimKey& k) { return t < k.time; });
    const AnimKey& b = *next;
    const AnimKey& a = *(next - 1);
    const float span = b.time - a.time;
    const float u = span > 0 ? (time - a.time) / span : 0.0f;
    auto mix = [u](const Vec3& p, const Vec3& q) { return Vec3{p.x + (q.x - p.x) * u, p.y + (q.y - p.y) * u, p.z + (q.z - p.z) * u}; };
    AnimKey out;
    out.time = time;
    out.scale = mix(a.scale, b.scale);
    out.translation = mix(a.translation, b.translation);
    const float sign = a.rotation.x * b.rotation.x + a.rotation.y * b.rotation.y + a.rotation.z * b.rotation.z + a.rotation.w * b.rotation.w < 0 ? -1.0f : 1.0f;
    out.rotation = normalizedQuat(Quat{a.rotation.x + (sign * b.rotation.x - a.rotation.x) * u, a.rotation.y + (sign * b.rotation.y - a.rotation.y) * u,
                                       a.rotation.z + (sign * b.rotation.z - a.rotation.z) * u, a.rotation.w + (sign * b.rotation.w - a.rotation.w) * u});
    return out;
}

void writeGeoObj(const GeoModel& model, const std::string& objPath, bool applyBoneTransforms, bool rightHanded) {
    const float zSign = rightHanded ? -1.0f : 1.0f;
    std::string mtlPath = objPath;
    const size_t dot = mtlPath.rfind('.');
    mtlPath = (dot == std::string::npos ? mtlPath : mtlPath.substr(0, dot)) + ".mtl";
    const size_t slash = mtlPath.find_last_of("/\\");
    const std::string mtlName = slash == std::string::npos ? mtlPath : mtlPath.substr(slash + 1);

    FILE* mtl = std::fopen(mtlPath.c_str(), "w");
    if (!mtl) throw FormatError("cannot write " + mtlPath);
    for (size_t i = 0; i < model.pieces.size(); ++i) {
        const GeoPiece& piece = model.pieces[i];
        std::fprintf(mtl, "newmtl piece_%zu\nKd 1 1 1\n", i);
        if (!piece.effectName.empty()) std::fprintf(mtl, "# effect %s\n", piece.effectName.c_str());
        if (!piece.diffuseTexture.empty()) std::fprintf(mtl, "map_Kd %s\n", piece.diffuseTexture.c_str());
        std::fprintf(mtl, "\n");
    }
    std::fclose(mtl);

    FILE* f = std::fopen(objPath.c_str(), "w");
    if (!f) throw FormatError("cannot write " + objPath);
    std::fprintf(f, "mtllib %s\n", mtlName.c_str());
    size_t base = 1;
    for (size_t i = 0; i < model.pieces.size(); ++i) {
        const GeoPiece& piece = model.pieces[i];
        const GeoBone* bone = nullptr;
        if (applyBoneTransforms && !piece.boneName.empty())
            for (const GeoBone& b : model.bones)
                if (b.name == piece.boneName) { bone = &b; break; }
        std::fprintf(f, "o piece_%zu\nusemtl piece_%zu\n", i, i);
        for (const GeoVertex& v : piece.vertices) {
            Vec3 p = v.position;
            if (bone) {
                p = rotate(bone->rotation, p);
                p = {p.x * bone->scale.x, p.y * bone->scale.y, p.z * bone->scale.z};
                p = {p.x + bone->translation.x, p.y + bone->translation.y, p.z + bone->translation.z};
            }
            std::fprintf(f, "v %g %g %g\n", p.x, p.y, zSign * p.z);
        }
        for (const GeoVertex& v : piece.vertices) std::fprintf(f, "vt %g %g\n", v.uv.x, 1.0f - v.uv.y);
        for (const GeoVertex& v : piece.vertices) {
            const Vec3 n = bone ? rotate(bone->rotation, v.normal) : v.normal;
            std::fprintf(f, "vn %g %g %g\n", n.x, n.y, zSign * n.z);
        }
        for (size_t k = 0; k + 2 < piece.indices.size(); k += 3) {
            const size_t a = base + piece.indices[k];
            const size_t b = base + piece.indices[k + 1];
            const size_t c = base + piece.indices[k + 2];
            std::fprintf(f, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, a, a, b, b, b, c, c, c);
        }
        base += piece.vertices.size();
    }
    std::fclose(f);
}

}  // namespace tr
