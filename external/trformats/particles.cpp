#include "particles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace tr {

namespace {

struct Chunk {
    uint32_t tag = 0;
    uint32_t version = 0;
    size_t start = 0;
    size_t end = 0;
};

Chunk readChunk(ByteReader& r) {
    Chunk c;
    c.start = r.tell();
    c.tag = r.u32();
    const uint32_t size = r.u32();
    c.version = r.u32();
    r.u32();                                   // reserved, 0
    c.end = r.tell() + size;
    if (c.end > r.end()) throw FormatError("chunk " + tagName(c.tag) + " runs past the end of its data");
    return c;
}

void finish(ByteReader& r, const Chunk& c) {
    if (r.tell() != c.end)
        throw FormatError(tagName(c.tag) + " at offset " + std::to_string(c.start) + ": " + std::to_string(long(c.end) - long(r.tell())) +
                          " bytes not understood");
}

ParticleValue readValue(ByteReader& r, uint32_t components) {
    ParticleValue v;
    v.x = r.f32();
    if (components > 1) v.y = r.f32();
    if (components > 2) v.z = r.f32();
    if (components > 3) v.w = r.f32();
    return v;
}

std::string readIstr(ByteReader& r) {
    const Chunk c = readChunk(r);
    if (c.tag != fourcc("ISTR")) throw FormatError("expected ISTR but found " + tagName(c.tag));
    std::string text = r.cstr();
    finish(r, c);
    return text;
}

// An asset reference (assId unserializer FUN_005ac5c0): ISTR name, INON none, ITNI numeric id.
std::string readAsset(ByteReader& r) {
    const Chunk c = readChunk(r);
    std::string value;
    if (c.tag == fourcc("ISTR")) value = r.cstr();
    else if (c.tag == fourcc("ITNI")) value = "#" + std::to_string(r.u32());
    else if (c.tag != fourcc("INON")) throw FormatError("asset: unexpected " + tagName(c.tag));
    r.seek(c.end);
    return value;
}

class Reader {
public:
    explicit Reader(ByteReader& r) : r_(r) {}
    uint32_t slots = 0;

    // kind: 'f' float, 'p' point, 'c' colour - decides the width of constants and keys.
    ParticleGenerator generator(char kind) {
        const Chunk c = readChunk(r_);
        ParticleGenerator g;
        g.tag = c.tag;
        g.stateSlot = slots++;
        const uint32_t components = kind == 'f' ? 1 : kind == 'p' ? 3 : 4;
        const uint32_t t = c.tag;
        if (t == fourcc("CONF") || t == fourcc("CONP") || t == fourcc("CONC")) {
            g.kind = ParticleGeneratorKind::Constant;
            g.value = readValue(r_, components);
        } else if (t == fourcc("UNIF")) {
            g.kind = ParticleGeneratorKind::Uniform;
            g.minimum = r_.f32();
            g.maximum = r_.f32();
        } else if (t == fourcc("UUNI")) {
            g.kind = ParticleGeneratorKind::UnitRandom;
        } else if (t == fourcc("GAUS")) {
            g.kind = ParticleGeneratorKind::Gaussian;
            g.mean = r_.f32();
            g.deviation = r_.f32();
        } else if (t == fourcc("SINE")) {
            g.kind = ParticleGeneratorKind::Sine;
            g.amplitude = r_.f32();
            g.frequency = r_.f32();
            g.offset = r_.f32();
        } else if (t == fourcc("SEQF") || t == fourcc("SEQP") || t == fourcc("SEQC") || t == fourcc("RNDF") || t == fourcc("RNDP") ||
                   t == fourcc("RNDC")) {
            g.kind = ((t >> 24) & 0xFF) == 'S' ? ParticleGeneratorKind::Sequence : ParticleGeneratorKind::Random;
            const uint32_t n = r_.u32();
            for (uint32_t k = 0; k < n; ++k) g.children.push_back(generator(kind));
        } else if (t == fourcc("HLDF") || t == fourcc("HLDP") || t == fourcc("HLDC") || t == fourcc("LBDF") || t == fourcc("LBDP") ||
                   t == fourcc("LBDC")) {
            g.kind = ((t >> 24) & 0xFF) == 'H' ? ParticleGeneratorKind::Hold : ParticleGeneratorKind::Blend;
            if (c.version == 1 && t == fourcc("HLDC")) {         // one file: a plain count (UNVERIFIED reading)
                ParticleGenerator count;
                count.tag = fourcc("CONF");
                count.stateSlot = slots++;
                count.value.x = r_.f32();
                g.children.push_back(count);
            } else {
                g.children.push_back(generator('f'));
            }
            g.children.push_back(generator(kind));
        } else if (t == fourcc("ADDF") || t == fourcc("ADDP") || t == fourcc("ADDC")) {
            g.kind = ParticleGeneratorKind::Add;
            g.children.push_back(generator(kind));
            g.children.push_back(generator(kind));
        } else if (t == fourcc("KEYF") || t == fourcc("KEYP") || t == fourcc("KEYC")) {
            g.kind = ParticleGeneratorKind::Keys;
            const uint32_t n = r_.u32();
            for (uint32_t k = 0; k < n; ++k) {
                ParticleKey key;
                key.time = r_.f32();
                key.value = readValue(r_, components);
                g.keys.push_back(key);
            }
            r_.seek(c.end);                    // one KEYP carries a stray trailing chunk
        } else if (t == fourcc("SPEP")) {
            g.kind = ParticleGeneratorKind::Sphere;
            g.centre = r_.vec3();
            for (int k = 0; k < 3; ++k) g.children.push_back(generator('f'));
        } else if (t == fourcc("BOXP")) {
            g.kind = ParticleGeneratorKind::Box;
            for (int k = 0; k < 3; ++k) g.children.push_back(generator('f'));
        } else if (t == fourcc("CYLP")) {
            g.kind = ParticleGeneratorKind::Cylinder;
            g.centre = r_.vec3();
            g.axis = r_.vec3();
            g.axisU = r_.vec3();
            g.axisV = r_.vec3();
            for (int k = 0; k < 3; ++k) g.children.push_back(generator('f'));
        } else if (t == fourcc("RNGP")) {
            g.kind = ParticleGeneratorKind::Range;
            g.children.push_back(generator('f'));
            g.centre = r_.vec3();
            g.axis = r_.vec3();
        } else if (t == fourcc("RNGC")) {
            g.kind = ParticleGeneratorKind::Range;
            g.children.push_back(generator('f'));
            g.colour1 = readValue(r_, 4);
            g.colour2 = readValue(r_, 4);
        } else if (t == fourcc("PTCL")) {
            g.kind = ParticleGeneratorKind::PointColour;
            g.children.push_back(generator('p'));
            g.children.push_back(generator('f'));
        } else if (t == fourcc("GEOG")) {
            g.kind = ParticleGeneratorKind::Geo;
            g.geo = readIstr(r_);
        } else {
            throw FormatError("unknown particle generator " + tagName(t) + " at offset " + std::to_string(c.start));
        }
        finish(r_, c);
        return g;
    }

    ParticleDomain domain() {
        const Chunk c = readChunk(r_);
        ParticleDomain d;
        // Layouts from the unserializers (traced 2026-10-20): pfxBox FUN_009c7950 reads the invert byte FIRST, then centre (+ 8),
        // dimension (+ 0x14) and rotation (+ 0x20, degrees: x pitch, y yaw, z roll); pfxSphere (v1 / v2) stores the radius SQUARED
        // (the reader keeps its square root, rcpss(rsqrtss)) and v2 adds the invert byte; pfxHalfSpace normal then distance.
        if (c.tag == fourcc("BOXS")) {
            d.kind = ParticleDomain::Kind::Box;
            d.invert = r_.u8() != 0;
            d.centre = r_.vec3();
            d.dimension = r_.vec3();
            d.rotation = r_.vec3();
        } else if (c.tag == fourcc("HALF")) {
            d.kind = ParticleDomain::Kind::HalfSpace;
            d.normal = r_.vec3();
            d.distance = r_.f32();
        } else if (c.tag == fourcc("SPHE")) {
            d.kind = ParticleDomain::Kind::Sphere;
            d.centre = r_.vec3();
            d.radius = std::sqrt(std::max(r_.f32(), 0.0f));
            if (c.version >= 2) d.invert = r_.u8() != 0;
        } else {
            throw FormatError("unknown particle domain " + tagName(c.tag));
        }
        finish(r_, c);
        return d;
    }

    ParticleOperator op() {
        const Chunk c = readChunk(r_);
        ParticleOperator o;
        o.tag = c.tag;
        o.stateSlot = slots++;
        const uint32_t t = c.tag;
        using K = ParticleOperatorKind;
        struct Named { uint32_t tag; K kind; };
        static const Named kinds[] = {
            {fourcc("COMP"), K::Composite}, {fourcc("SEQ "), K::Sequence}, {fourcc("APAF"), K::ApplyAfter}, {fourcc("EMIT"), K::Emit}, {fourcc("PEMT"), K::EmitFromParticle},
            {fourcc("MOVE"), K::Move}, {fourcc("DRAG"), K::Drag}, {fourcc("ACCL"), K::Accelerate}, {fourcc("ACCS"), K::AccelerateSpin}, {fourcc("GRAV"), K::Gravity},
            {fourcc("VORT"), K::Vortex}, {fourcc("TCOL"), K::TargetColour}, {fourcc("TALP"), K::TargetAlpha}, {fourcc("KCLR"), K::KeyColour}, {fourcc("SETC"), K::SetColour},
            {fourcc("SETG"), K::SetGrowth}, {fourcc("SETP"), K::SetPosition}, {fourcc("SETR"), K::SetRotation}, {fourcc("SETS"), K::SetSize}, {fourcc("SETV"), K::SetVelocity},
            {fourcc("SEST"), K::SetStretch}, {fourcc("SESP"), K::SetSpin}, {fourcc("ALTV"), K::AlignToVelocity}, {fourcc("TOUT"), K::TerminateOutside},
            {fourcc("BONC"), K::Bounce}, {fourcc("DUMM"), K::Dummy}};
        bool known = false;
        for (const Named& n : kinds)
            if (n.tag == t) {
                o.kind = n.kind;
                known = true;
            }
        if (!known) throw FormatError("unknown particle operator " + tagName(t) + " at offset " + std::to_string(c.start));
        // Emitters, dummies and the version-1 forms have no enabled byte; vortex gained it in version 3.
        const bool hasEnabled = o.kind != K::Emit && o.kind != K::EmitFromParticle && o.kind != K::Dummy &&
                                c.version >= (o.kind == K::Vortex ? 3u : 2u);
        if (hasEnabled) o.enabled = r_.u8() != 0;
        switch (o.kind) {
            case K::Emit:
            case K::EmitFromParticle: {
                ParticleEmitter& e = o.emitter;
                e.rate = generator('f');
                if (o.kind == K::Emit) e.point = generator('p');
                e.velocity = generator('p');
                e.colour = generator('c');
                e.size = generator('f');
                e.growth = generator('f');
                e.rotation = generator('f');
                e.spin = generator('f');
                e.stretch = generator('f');
                e.lifetime = generator('f');
                e.roundingMode = r_.u32();
                break;
            }
            case K::Composite: {
                const uint32_t n = r_.u32();
                for (uint32_t k = 0; k < n; ++k) o.children.push_back(op());
                break;
            }
            case K::Sequence: {
                const uint32_t n = r_.u32();
                for (uint32_t k = 0; k < n; ++k) {
                    ParticleStage stage;
                    stage.time = r_.f32();
                    if (r_.u8() != 0) {
                        stage.child = int32_t(o.children.size());
                        o.children.push_back(op());
                    }
                    o.stages.push_back(stage);
                }
                break;
            }
            case K::ApplyAfter:
                o.children.push_back(op());
                o.time = r_.f32();
                o.unknown = r_.f32();
                break;
            case K::Drag:
            case K::AccelerateSpin: o.coefficient = r_.f32(); break;
            case K::Accelerate: o.vector = r_.vec3(); break;
            case K::Gravity:
                o.vector = r_.vec3();
                o.coefficient = r_.f32();
                break;
            case K::Vortex:
                o.vector = r_.vec3();
                o.axis = r_.vec3();
                o.coefficient = r_.f32();
                break;
            case K::TargetColour:
                o.time = r_.f32();
                o.target = readValue(r_, 4);
                break;
            case K::TargetAlpha:
                o.target.w = r_.f32();
                o.time = r_.f32();
                break;
            case K::KeyColour: {
                const uint32_t n = r_.u32();
                for (uint32_t k = 0; k < n; ++k) {
                    ParticleKey key;
                    key.time = r_.f32();
                    key.value = readValue(r_, 4);
                    o.keys.push_back(key);
                }
                break;
            }
            case K::SetColour: o.generator = generator('c'); break;
            case K::SetPosition:
            case K::SetVelocity: o.generator = generator('p'); break;
            case K::SetGrowth:
            case K::SetRotation:
            case K::SetSize:
            case K::SetStretch:
            case K::SetSpin: o.generator = generator('f'); break;
            case K::TerminateOutside: o.domain = domain(); break;
            case K::Bounce:
                o.friction = r_.f32();
                o.resilience = r_.f32();
                o.cutoff = r_.f32();
                o.domain = domain();
                break;
            case K::Move:
            case K::AlignToVelocity:
            case K::Dummy: break;
        }
        finish(r_, c);
        return o;
    }

    ParticleGroup group() {
        const Chunk c = readChunk(r_);
        if (c.tag == fourcc("PGRS")) {
            ParticleGroup g = group();
            g.sorted = true;
            g.sortOrder = r_.u8();
            finish(r_, c);
            return g;
        }
        if (c.tag != fourcc("PGRP")) throw FormatError("expected a particle group but found " + tagName(c.tag));
        ParticleGroup g;
        g.version = c.version;
        if (c.version < 4 || c.version > 8) throw FormatError("PGRP version " + std::to_string(c.version) + " is not supported");
        if (c.version >= 7) g.enabled = r_.u8() != 0;
        if (c.version >= 6) g.name = r_.cstr();
        if (c.version >= 5) g.zBias = r_.u32();
        g.maxParticles = r_.u32();
        g.primitive = r_.u8();
        g.referenceFrame = r_.u8();
        g.sourceBlend = r_.u32();
        g.destinationBlend = r_.u32();
        g.blendOperation = r_.u32();
        if (c.version == 8) g.unknownFlag = r_.u8();
        if (g.primitive >= 2 && g.primitive <= 4) {
            g.texture = readIstr(r_);
            g.billboard = r_.u32();
        }
        finish(r_, c);
        return g;
    }

private:
    ByteReader& r_;
};

const ParticleGenerator& defaultGenerator() {
    static const ParticleGenerator one = [] {
        ParticleGenerator g;
        g.value = ParticleValue{1, 1, 1, 1};
        return g;
    }();
    return one;
}

}  // namespace

const ParticleGenerator& generatorChild(const ParticleGenerator& generator, size_t index) {
    return index < generator.children.size() ? generator.children[index] : defaultGenerator();
}

uint32_t particleBlendType(const ParticleGroup& group) {
    if (group.sourceBlend == 5) return group.destinationBlend == 2 ? (group.blendOperation == 1 ? 0u : 3u) : 2u;
    if (group.sourceBlend == 1) return 1;
    return 2;
}

ParticleSystem readParticleSystem(const Bytes& data) {
    if (data.size() < 8 || std::memcmp(data.data(), "CHNKBLXX", 8) != 0) throw FormatError("not a binary chunk file");
    ByteReader r(data, 8);
    const Chunk c = readChunk(r);
    if (c.tag != fourcc("PSYS")) throw FormatError("expected PSYS but found " + tagName(c.tag));
    ParticleSystem s;
    s.version = c.version;
    if (c.version >= 4) s.enabled = r.u8() != 0;
    Reader reader(r);
    const uint32_t n = r.u32();
    for (uint32_t k = 0; k < n; ++k) {
        ParticleGroup g = reader.group();
        g.op = reader.op();
        s.groups.push_back(std::move(g));
    }
    if (c.version >= 2) {
        const Chunk bounds = readChunk(r);
        if (bounds.tag != fourcc("BBOX")) throw FormatError("expected BBOX but found " + tagName(bounds.tag));
        r.seek(bounds.end);
        s.warmupInterval = r.f32();
        s.warmupCount = r.u32();
    }
    finish(r, c);
    s.stateCount = reader.slots;
    return s;
}

namespace {
std::vector<ParticleAttachment> attachments(const GeoModel& model, const char* wanted) {
    std::vector<ParticleAttachment> out;
    auto add = [&](const std::string& key, const std::string& value) {
        if (toLowerAscii(key) != wanted) return;
        const size_t comma = value.find(',');
        if (comma == std::string::npos) return;
        ParticleAttachment a;
        a.contactPoint = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
        a.file = value.substr(comma + 1);
        for (const ParticleAttachment& b : out)
            if (b.contactPoint == a.contactPoint && toLowerAscii(b.file) == toLowerAscii(a.file)) return;
        out.push_back(std::move(a));
    };
    for (const GeoContactPoint& cp : model.contactPoints)
        for (const GeoParameter& p : cp.values) add(p.key, p.value);
    for (const GeoProperty& p : model.properties) add(p.key, p.value);
    for (const GeoPiece& piece : model.pieces)
        for (const GeoParameter& p : piece.userData) add(p.key, p.value);
    return out;
}
}  // namespace

std::vector<ParticleAttachment> particleAttachments(const GeoModel& model) { return attachments(model, "psys"); }

const GeoContactPoint* findContactPoint(const GeoModel& model, uint32_t id) {
    for (const GeoContactPoint& cp : model.contactPoints)
        if (cp.id == id) return &cp;
    return nullptr;
}

}  // namespace tr

namespace tr {

float sfxCurveValue(const SfxCurve& curve, float seconds) {
    if (curve.keys.empty()) return 1.0f;
    if (curve.keys.size() == 1) return curve.keys.front().second;
    const float last = curve.keys.back().first;
    float t = seconds;
    if (curve.loop && last > 0.0f) t = std::fmod(std::max(t, 0.0f), last);
    t = std::clamp(t, 0.0f, last);
    for (size_t k = 0; k + 1 < curve.keys.size(); ++k) {
        const auto& a = curve.keys[k];
        const auto& b = curve.keys[k + 1];
        if (t >= a.first && t <= b.first && b.first > a.first) return a.second + (b.second - a.second) * (t - a.first) / (b.first - a.first);
    }
    return curve.keys.back().second;
}

const SfxCurve& sfxLightCurve(const SfxLight& light, int index) {
    switch (index) {
        case 0: return light.colourR;
        case 1: return light.colourG;
        case 2: return light.colourB;
        case 3: return light.colourA;
        case 4: return light.range;
        case 5: return light.falloffPercent;
        case 6: return light.intensity;
        case 7: return light.penumbraAngle;
        default: return light.coneAngle;
    }
}

SfxPackage readSfxPackage(const Bytes& data) {
    ByteReader r(data);
    if (data.size() < 8 || std::memcmp(data.data(), "CHNKBLXX", 8) != 0) throw FormatError("pkg: not a chunk file");
    r.seek(8);
    const Chunk root = readChunk(r);
    if (root.tag != fourcc("SFXP")) throw FormatError("pkg: expected SFXP but found " + tagName(root.tag));
    SfxPackage package;
    while (r.tell() < root.end) {
        const Chunk event = readChunk(r);
        package.events.push_back(event.tag);
        if (event.tag == fourcc("ACCY")) {
            SfxAccessory a;
            a.version = event.version;
            r.u8();
            a.mesh = readAsset(r);
            a.animation = readAsset(r);
            r.u8();
            if (event.version > 1) {
                a.animation2 = readAsset(r);
                readAsset(r);
                r.u8();
            }
            if (event.version > 2) {
                r.u8();
                a.package = readAsset(r);
            }
            if (event.version > 3) for (int k = 0; k < 3; ++k) readAsset(r);
            if (event.version > 4) r.u8();
            if (event.version > 6) r.u8();
            if (event.version > 7) r.f32();
            finish(r, event);
            package.accessories.push_back(a);
        } else if (event.tag == fourcc("PFXA")) {
            SfxParticleAttach p;
            p.version = event.version;
            p.unknown0 = r.u8();
            p.file = readAsset(r);
            p.target = r.u8();
            p.contactPoint = r.u8();
            p.unknown2 = r.u8();
            if (event.version == 2) p.flag = r.u8() != 0;
            finish(r, event);
            package.particles.push_back(p);
        } else if (event.tag == fourcc("SLIT")) {
            SfxLight l;
            l.version = event.version;
            l.unknown0 = r.u8();
            l.target = r.u8();
            l.contactPoint = r.u8();
            l.unknown3 = r.u8();
            l.durationMs = r.u32();
            l.priority = r.f32();
            l.castsShadows = r.u8() != 0;
            l.shadowQuality = r.u32();
            l.shadowBiasPercent = r.f32();
            l.position = Vec3{r.f32(), r.f32(), r.f32()};
            l.offset = Vec3{r.f32(), r.f32(), r.f32()};
            l.globeMap = event.version < 4 ? r.cstr() : readAsset(r);
            if (event.version < 3) {
                l.rotation = Quat{r.f32(), r.f32(), r.f32(), r.f32()};
            } else {
                // D3DXQuaternionRotationYawPitchRoll(yaw = rotation[1], pitch = rotation[0], roll = rotation[2]), degrees.
                const float d = 3.14159265f / 180.0f;
                const float pitch = r.f32() * d, yaw = r.f32() * d, roll = r.f32() * d;
                const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f), cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
                const float cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);
                l.rotation = Quat{cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr, cy * cp * sr - sy * sp * cr, cy * cp * cr + sy * sp * sr};
            }
            auto curve = [&r]() {
                SfxCurve c;
                c.loop = r.u8() != 0;
                c.uiMax = r.f32();
                const uint32_t n = r.u32();
                for (uint32_t k = 0; k < n; ++k) {
                    const float t = r.f32();
                    c.keys.emplace_back(t, r.f32());
                }
                return c;
            };
            l.colourR = curve(); l.colourG = curve(); l.colourB = curve(); l.colourA = curve();
            l.range = curve(); l.falloffPercent = curve(); l.intensity = curve();
            if (event.version >= 2) {
                l.penumbraAngle = curve(); l.coneAngle = curve();
                l.spot = r.u8() == 1;
            }
            if (event.version >= 5) l.renderMode = r.u8();
            finish(r, event);
            package.lights.push_back(l);
        } else if (event.tag == fourcc("ACBM")) {
            SfxBeam b;
            b.version = event.version;
            b.marker = r.u8();
            b.texture = readAsset(r);
            b.durationMs = r.u32();
            b.target1 = r.u8();
            b.contactPoint1 = r.u8();
            b.target2 = r.u8();
            b.contactPoint2 = r.u8();
            if (event.version > 1) {
                b.tileRate = r.f32();
                b.animRate = r.f32();
            }
            b.noiseSpeed = r.f32();
            b.noiseAmplitude = r.f32();
            b.width = r.f32();
            if (event.version > 2) b.unknownC = r.u8();
            if (event.version > 3) {
                b.tileOncePerSegment = r.u32();
                b.maxSubdivisions = r.u32();
                b.branchChance = float(r.u8()) / 100.0f;
            }
            if (event.version > 4) {
                b.branchLength = float(r.u8()) / 100.0f;
                b.branchDecay = float(r.u8()) / 100.0f;
                b.branchPull = float(r.u8()) / 100.0f;
            }
            finish(r, event);
            package.beams.push_back(b);
        } else if (event.tag == fourcc("AOPC")) {
            SfxOpacity o;
            o.version = event.version;
            o.unknown0 = r.u8();
            o.target = r.u8();
            o.loop = r.u8() != 0;
            const uint32_t n = r.u32();
            for (uint32_t k = 0; k < n; ++k) {
                const float t = r.f32();
                o.keys.emplace_back(t, r.f32());
            }
            if (event.version >= 2) o.durationMs = r.u32();
            else if (!o.keys.empty()) o.durationMs = uint32_t(std::lround(o.keys.back().first * 1000.0f));
            if (event.version >= 3) o.unknown10 = r.u8();
            finish(r, event);
            package.opacities.push_back(o);
        } else if (event.tag == fourcc("SND ")) {
            SfxSound a;
            a.version = event.version;
            a.marker = r.u8();
            if (event.version < 12) {
                const uint32_t n = r.u8();
                for (uint32_t k = 0; k < n; ++k) a.files.push_back(readAsset(r));
            }
            a.target = r.u8();
            if (event.version < 12) r.f32();
            if (event.version > 2) a.flag10 = r.u8() != 0;
            if (event.version > 3) a.contactPoint = r.u8();
            if (event.version > 4) {
                a.hasPair = r.u8() != 0;
                if (a.hasPair) {
                    a.pair0 = r.u32();
                    a.pair1 = r.u32();
                }
            }
            if (event.version > 5) a.audioSet = r.u32();
            if (event.version > 6) {
                a.unknown24 = r.u8();
                a.unknown28 = r.u8();
            }
            if (event.version > 7) {
                a.flag2c = r.u8() != 0;
                a.distance = r.f32();
            }
            if (event.version > 9) {
                a.flag34 = r.u8() != 0;
                a.offset = Vec3{r.f32(), r.f32(), r.f32()};
            }
            finish(r, event);
            package.sounds.push_back(a);
        }
        r.seek(event.end);
    }
    return package;
}

}  // namespace tr
