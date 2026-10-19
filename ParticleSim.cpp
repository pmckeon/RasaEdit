#include "ParticleSim.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float twoPi = 6.2831853f;

tr::ParticleValue lerpValue(const tr::ParticleValue& a, const tr::ParticleValue& b, float t) {
    return tr::ParticleValue{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

tr::ParticleValue sampleKeys(const std::vector<tr::ParticleKey>& keys, float t) {
    if (keys.empty()) return tr::ParticleValue{1, 1, 1, 1};
    if (t <= keys.front().time) return keys.front().value;
    for (size_t k = 1; k < keys.size(); ++k) {
        if (t <= keys[k].time) {
            const float span = keys[k].time - keys[k - 1].time;
            return lerpValue(keys[k - 1].value, keys[k].value, span > 0 ? (t - keys[k - 1].time) / span : 1.0f);
        }
    }
    return keys.back().value;
}

tr::Vec3 point(const tr::ParticleValue& v) { return tr::Vec3{v.x, v.y, v.z}; }

tr::Vec3 transformDirection(const Mat4& m, const tr::Vec3& d) {
    return tr::Vec3{m.row0.x * d.x + m.row0.y * d.y + m.row0.z * d.z, m.row1.x * d.x + m.row1.y * d.y + m.row1.z * d.z,
                    m.row2.x * d.x + m.row2.y * d.y + m.row2.z * d.z};
}

uint8_t unitByte(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// Inverse of an affine matrix (upper 3 x 3 and translation).
Mat4 affineInverse(const Mat4& m) {
    const Mat4 it = inverseTranspose3(m);
    Mat4 r;
    r.row0 = {it.row0.x, it.row1.x, it.row2.x, 0};
    r.row1 = {it.row0.y, it.row1.y, it.row2.y, 0};
    r.row2 = {it.row0.z, it.row1.z, it.row2.z, 0};
    r.row3 = {0, 0, 0, 1};
    const tr::Vec3 t{m.row0.w, m.row1.w, m.row2.w};
    const tr::Vec3 moved = transformDirection(r, t);
    r.row0.w = -moved.x;
    r.row1.w = -moved.y;
    r.row2.w = -moved.z;
    return r;
}

// D3DXQuaternionRotationYawPitchRoll (radians), as Wine's d3dx9 computes it.
tr::Quat yawPitchRoll(float yaw, float pitch, float roll) {
    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f), cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    const float cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);
    tr::Quat q;
    q.x = cy * sp * cr + sy * cp * sr;
    q.y = sy * cp * cr - cy * sp * sr;
    q.z = cy * cp * sr - sy * sp * cr;
    q.w = cy * cp * cr + sy * sp * sr;
    return q;
}

// A particle domain (pfxBox / pfxHalfSpace / pfxSphere, traced 2026-10-20) placed in the particles' space: its own frame (box: centre
// and rotation, sphere: centre) times `world` when given - pfxBounce::Apply (FUN_009ca910) sets the system's world transform each
// frame for World / Hybrid groups and none for Object groups; pfxTerminateOutside (FUN_00999060) never sets one, so its domain stays
// as loaded (no transform).
struct PlacedDomain {
    const tr::ParticleDomain* domain = nullptr;
    Mat4 frame, inverse;
    tr::Vec3 normal;              // half space, in the particles' space
    float offset = 0;             // half space: inside where normal . p + offset >= 0
};

PlacedDomain placeDomain(const tr::ParticleDomain& d, const Mat4* world) {
    PlacedDomain pd;
    pd.domain = &d;
    using Kind = tr::ParticleDomain::Kind;
    if (d.kind == Kind::HalfSpace) {
        // FUN_009d5270: without a transform the stored normal and distance; with one, the normal through the matrix and the offset
        // -(n' . p0') of a point of the plane (p0 = -distance n).
        pd.normal = d.normal;
        pd.offset = d.distance;
        if (world) {
            pd.normal = transformDirection(*world, d.normal);
            const tr::Vec3 p0 = transformPoint(*world, scaled(d.normal, -d.distance));
            pd.offset = -dot(pd.normal, p0);
        }
        return pd;
    }
    const float degrees = 0.0174532925f;
    const tr::Quat q = d.kind == Kind::Box ? yawPitchRoll(d.rotation.y * degrees, d.rotation.x * degrees, d.rotation.z * degrees) : tr::Quat{};
    pd.frame = placementMatrix(d.centre, q, 1.0f);
    if (world) pd.frame = multiply(*world, pd.frame);
    pd.inverse = affineInverse(pd.frame);
    return pd;
}

// Inside test (domain vtable slot 29; TOUT): box within +-|dimension| / 2 of its frame, sphere within the radius, half space
// n . p + offset >= 0. The invert flag is not read here.
bool domainInside(const PlacedDomain& pd, const tr::Vec3& p) {
    const tr::ParticleDomain& d = *pd.domain;
    using Kind = tr::ParticleDomain::Kind;
    if (d.kind == Kind::HalfSpace) return dot(pd.normal, p) + pd.offset >= 0.0f;
    const tr::Vec3 q = transformPoint(pd.inverse, p);
    if (d.kind == Kind::Sphere) return dot(q, q) <= d.radius * d.radius;
    return std::fabs(q.x) <= std::fabs(d.dimension.x) * 0.5f && std::fabs(q.y) <= std::fabs(d.dimension.y) * 0.5f &&
           std::fabs(q.z) <= std::fabs(d.dimension.z) * 0.5f;
}

// Hit test of the step from `from` to `to` (domain vtable slot 30; BONC): the surface normal there, pointing along the crossing.
// Half space (FUN_009d5380): the ends on opposite sides; the plane normal. Sphere (FUN_009bc7b0): one end inside, the other not; the
// normal at the crossing point (negated when inverted), kept only when the step runs along it. Box (FUN_009c7230): a particle leaving
// (or, inverted, entering) - every face whose plane it crosses, within 0.1 m (0x00c2b4d0 / 0x00c2bd40), moving outward (inward),
// adds its outward (inward) axis; the sum normalised. Normals are turned into the particles' space by the frame's rotation.
bool domainHit(const PlacedDomain& pd, const tr::Vec3& from, const tr::Vec3& to, tr::Vec3& normal) {
    const tr::ParticleDomain& d = *pd.domain;
    using Kind = tr::ParticleDomain::Kind;
    if (d.kind == Kind::HalfSpace) {
        if ((dot(pd.normal, to) + pd.offset) * (dot(pd.normal, from) + pd.offset) >= 0.0f) return false;
        normal = pd.normal;
        return true;
    }
    const tr::Vec3 a = transformPoint(pd.inverse, from), b = transformPoint(pd.inverse, to);
    tr::Vec3 n{0, 0, 0};
    if (d.kind == Kind::Sphere) {
        const float r2 = d.radius * d.radius;
        if ((dot(a, a) <= r2) == (dot(b, b) <= r2)) return false;
        const tr::Vec3 step = subtract(b, a);
        const float qa = dot(step, step), qb = 2.0f * dot(a, step), qc = dot(a, a) - r2;
        const float disc = qb * qb - 4.0f * qa * qc;
        if (qa <= 0.0f || disc < 0.0f) return false;
        const float root = std::sqrt(disc);
        float t = (-qb - root) / (2.0f * qa);
        if (t < 0.0f || t > 1.0f) t = (-qb + root) / (2.0f * qa);
        const tr::Vec3 at = add(a, scaled(step, std::clamp(t, 0.0f, 1.0f)));
        if (dot(at, at) <= 0.0f) return false;
        tr::Vec3 c = normalized(at);
        if (d.invert) c = scaled(c, -1.0f);
        if (dot(subtract(a, b), c) <= 0.0f) n = c;
    } else {
        const tr::Vec3 h{std::fabs(d.dimension.x) * 0.5f, std::fabs(d.dimension.y) * 0.5f, std::fabs(d.dimension.z) * 0.5f};
        auto in = [&](const tr::Vec3& v) { return std::fabs(v.x) <= h.x && std::fabs(v.y) <= h.y && std::fabs(v.z) <= h.z; };
        const bool inA = in(a), inB = in(b);
        const float e = 0.1f;
        const float da[3] = {a.x, a.y, a.z}, db[3] = {b.x, b.y, b.z}, hh[3] = {h.x, h.y, h.z};
        float out[3] = {0, 0, 0};
        if (d.invert ? (!inA && inB) : (inA && !inB)) {
            for (int k = 0; k < 3; ++k) {
                const float back = da[k] - db[k];          // (from - to) along the axis
                if (!d.invert) {
                    if (db[k] + hh[k] <= e && da[k] + hh[k] >= -e && -back <= 0.0f) out[k] -= 1.0f;   // out through the - face
                    if (db[k] - hh[k] >= -e && da[k] - hh[k] <= e && back <= 0.0f) out[k] += 1.0f;    // out through the + face
                } else {
                    if (db[k] + hh[k] >= -e && da[k] + hh[k] <= e && back <= 0.0f) out[k] += 1.0f;    // in through the - face
                    if (db[k] - hh[k] <= e && da[k] - hh[k] >= -e && -back <= 0.0f) out[k] -= 1.0f;   // in through the + face
                }
            }
        }
        n = tr::Vec3{out[0], out[1], out[2]};
    }
    if (dot(n, n) <= 0.0f) return false;
    normal = normalized(transformDirection(pd.frame, normalized(n)));
    return true;
}

}  // namespace

ParticleInstance::ParticleInstance(const tr::ParticleSystem& system, const Mat4& world, uint32_t seed)
    : system_(&system), world_(world), random_(seed ? seed : 0x9E3779B9u), slots_(system.stateCount), particles_(system.groups.size()) {
    for (size_t g = 0; g < system.groups.size(); ++g) particles_[g].reserve(std::min<uint32_t>(system.groups[g].maxParticles, 4096));
}

float ParticleInstance::random() {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return float(random_ >> 8) * (1.0f / 16777216.0f);
}

float ParticleInstance::gaussian() {
    const float u = std::max(random(), 1e-7f);
    return std::sqrt(-2.0f * std::log(u)) * std::cos(twoPi * random());
}

size_t ParticleInstance::particleCount() const {
    size_t n = 0;
    for (const auto& p : particles_) n += p.size();
    return n;
}

tr::ParticleValue ParticleInstance::evaluate(const tr::ParticleGenerator& g, float t) {
    using K = tr::ParticleGeneratorKind;
    SlotState& s = slots_[g.stateSlot];
    switch (g.kind) {
        case K::Constant: return g.value;
        case K::Uniform: {
            const float v = g.minimum + (g.maximum - g.minimum) * random();
            return tr::ParticleValue{v, v, v, v};
        }
        case K::UnitRandom: {
            const float v = random();
            return tr::ParticleValue{v, v, v, v};
        }
        case K::Gaussian: {
            const float v = g.mean + g.deviation * gaussian();
            return tr::ParticleValue{v, v, v, v};
        }
        case K::Sine: {
            const float v = g.amplitude * std::sin((t - g.offset) * twoPi * g.frequency);
            return tr::ParticleValue{v, v, v, v};
        }
        case K::Sequence:
            if (g.children.empty()) return tr::ParticleValue{1, 1, 1, 1};
            return evaluate(g.children[s.index++ % g.children.size()], t);
        case K::Random:
            if (g.children.empty()) return tr::ParticleValue{1, 1, 1, 1};
            return evaluate(g.children[std::min(size_t(random() * float(g.children.size())), g.children.size() - 1)], t);
        case K::Hold:
            // pfxHoldGenerator: the base value is kept for (int) count calls.
            if (--s.count < 1) {
                s.count = int32_t(evaluate(tr::generatorChild(g, 0), t).x);
                s.held = evaluate(tr::generatorChild(g, 1), t);
            }
            return s.held;
        case K::Blend: {
            // pfxLinearBlendGenerator: a new segment every (int) count calls, from the last target to a new base value
            // (the count generator is evaluated at time 0, as the engine does).
            if (s.count <= s.step) {
                s.count = int32_t(evaluate(tr::generatorChild(g, 0), 0.0f).x);
                s.from = s.held;
                s.held = evaluate(tr::generatorChild(g, 1), t);
                s.step = 0;
            }
            const float f = s.count == 0 ? 0.0f : float(s.step++) / float(s.count);
            return lerpValue(s.from, s.held, f);
        }
        case K::Add: {
            const tr::ParticleValue a = evaluate(tr::generatorChild(g, 0), t), b = evaluate(tr::generatorChild(g, 1), t);
            return tr::ParticleValue{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
        }
        case K::Keys: return sampleKeys(g.keys, t);
        case K::Sphere: {
            const float r = evaluate(tr::generatorChild(g, 0), t).x;
            const float theta = evaluate(tr::generatorChild(g, 1), t).x * twoPi;
            const float phi = evaluate(tr::generatorChild(g, 2), t).x * (twoPi * 0.5f);
            const float sinPhi = std::sin(phi);
            return tr::ParticleValue{g.centre.x + std::cos(theta) * r * sinPhi, g.centre.y + std::cos(phi) * r, g.centre.z + std::sin(theta) * r * sinPhi, 1};
        }
        case K::Box:
            return tr::ParticleValue{evaluate(tr::generatorChild(g, 0), t).x, evaluate(tr::generatorChild(g, 1), t).x, evaluate(tr::generatorChild(g, 2), t).x, 1};
        case K::Cylinder: {
            const float d = evaluate(tr::generatorChild(g, 0), t).x;
            const float a = evaluate(tr::generatorChild(g, 1), t).x * twoPi;
            const float r = evaluate(tr::generatorChild(g, 2), t).x;
            const tr::Vec3 p = add(add(g.centre, scaled(g.axis, d)), scaled(add(scaled(g.axisU, std::cos(a)), scaled(g.axisV, std::sin(a))), r));
            return tr::ParticleValue{p.x, p.y, p.z, 1};
        }
        case K::Range: {
            const float f = evaluate(tr::generatorChild(g, 0), t).x;
            if (g.tag == tr::fourcc("RNGC")) return lerpValue(g.colour1, g.colour2, f);
            const tr::Vec3 p = add(g.centre, scaled(g.axis, f));
            return tr::ParticleValue{p.x, p.y, p.z, 1};
        }
        case K::PointColour: {
            tr::ParticleValue v = evaluate(tr::generatorChild(g, 0), t);
            v.w = evaluate(tr::generatorChild(g, 1), t).x;
            return v;
        }
        case K::Geo: {
            // pfxGeoGenerator (FUN_009dd080 / FUN_009dced0, traced 2026-10-20): a random piece of the model (its static and skinned
            // pieces, GPCE / GSKN, FUN_009dcd00), then a random vertex of it - (random32 x count) >> 32 each - and that vertex's
            // POSITION as stored (no bone or entity transform); the origin when the model or piece has nothing.
            const std::vector<std::vector<tr::Vec3>>* pieces = geoPoints_ ? geoPoints_(g.geo) : nullptr;
            if (!pieces || pieces->empty()) {
                ++unsupported_;
                return tr::ParticleValue{0, 0, 0, 1};
            }
            const std::vector<tr::Vec3>& piece = (*pieces)[std::min(size_t(random() * float(pieces->size())), pieces->size() - 1)];
            if (piece.empty()) return tr::ParticleValue{0, 0, 0, 1};
            const tr::Vec3& v = piece[std::min(size_t(random() * float(piece.size())), piece.size() - 1)];
            return tr::ParticleValue{v.x, v.y, v.z, 1};
        }
    }
    return tr::ParticleValue{};
}

void ParticleInstance::remove(size_t group, size_t index) {
    std::vector<Particle>& list = particles_[group];
    if (system_->groups[group].sorted) {
        list.erase(list.begin() + std::ptrdiff_t(index));
    } else {
        list[index] = list.back();             // the engine's array moves the last particle into the slot
        list.pop_back();
    }
}

// pfxEmitFromParticle::Apply (FUN_009aae30, traced 2026-10-20): new particles at the particles of the PREVIOUS group of the system
// (the system numbers its groups, group + 0x138, FUN_00583920; the first group emits nothing). Skipped when this group is off, or
// when it is in Object space and the source is not. rate x dt per source particle: the whole part each, the fraction by chance per
// source (Randomize) or accumulated once a call (Accumulate); at most up to MaxParticles. A new particle starts at its source's
// position (taken into world space when the source is in Object space and this group is not), with the velocity, colour, size,
// growth, rotation, spin, stretch and lifetime generators at the emitter's time (velocity turned by the system for Hybrid) - the
// source's own velocity is not inherited - and has already lived a random part of dt, as pfxEmit's.
void ParticleInstance::emitFromParticles(const tr::ParticleOperator& op, size_t group, float dt) {
    SlotState& s = slots_[op.stateSlot];
    s.time += dt;
    const tr::ParticleGroup& g = system_->groups[group];
    if (group == 0) return;                     // Enabled: set by the system's reset (see emit)
    const size_t source = group - 1;
    const tr::ParticleGroup& sg = system_->groups[source];
    if (g.referenceFrame == 0 && sg.referenceFrame != 0) return;
    const tr::ParticleEmitter& e = op.emitter;
    const float wanted = evaluate(e.rate, s.time).x * dt;
    int32_t whole = int32_t(std::floor(wanted));
    const float fraction = wanted - float(whole);
    if (e.roundingMode == 1) {
        s.accumulator += fraction;
        if (s.accumulator > 1.0f) {
            ++whole;
            s.accumulator -= 1.0f;
        }
    }
    const bool toWorld = sg.referenceFrame == 0 && (g.referenceFrame == 1 || g.referenceFrame == 2);
    std::vector<Particle>& list = particles_[group];
    const size_t sources = particles_[source].size();
    for (size_t k = 0; k < sources; ++k) {
        int32_t n = whole;
        if (e.roundingMode == 0 && random() < fraction) ++n;
        n = std::min<int32_t>(n, int32_t(g.maxParticles) - int32_t(list.size()));
        const tr::Vec3 at = particles_[source][k].position;
        for (int32_t m = 0; m < n; ++m) {
            Particle p;
            p.position = toWorld ? transformPoint(world_, at) : at;
            p.previous = p.position;
            p.velocity = point(evaluate(e.velocity, s.time));
            if (g.referenceFrame == 2) p.velocity = transformDirection(world_, p.velocity);
            p.colour = evaluate(e.colour, s.time);
            const float life = evaluate(e.lifetime, s.time).x;
            p.inverseLife = life < 100000.0f && life != 0.0f ? 1.0f / life : 0.0f;
            p.size = evaluate(e.size, s.time).x;
            p.growth = evaluate(e.growth, s.time).x;
            p.rotation = evaluate(e.rotation, s.time).x;
            p.spin = evaluate(e.spin, s.time).x;
            p.stretch = evaluate(e.stretch, s.time).x;
            const float f = random() * dt;
            p.position = add(p.position, scaled(p.velocity, f));
            p.age += p.inverseLife * f;
            p.size += p.growth * f;
            p.rotation += p.spin * f;
            list.push_back(p);
        }
    }
}

// pfxBounce::Apply (FUN_009ca910, traced 2026-10-20) - the Particle System API's bounce: a particle whose next step (position +
// velocity x dt) hits the domain gets its velocity split along the surface normal into vn and vt, then v = vt x friction -
// vn x resilience when |vt|^2 > cutoff, else vt - vn x resilience (the three floats as stored, in that order). Positions are left
// to Move. The domain takes the system's world transform for World / Hybrid groups.
void ParticleInstance::bounce(const tr::ParticleOperator& op, size_t group, float dt) {
    const tr::ParticleGroup& g = system_->groups[group];
    const PlacedDomain pd = placeDomain(op.domain, g.referenceFrame == 0 ? nullptr : &world_);
    for (Particle& p : particles_[group]) {
        tr::Vec3 n;
        if (!domainHit(pd, p.position, add(p.position, scaled(p.velocity, dt)), n)) continue;
        const tr::Vec3 vn = scaled(n, dot(n, p.velocity));
        const tr::Vec3 vt = subtract(p.velocity, vn);
        p.velocity = dot(vt, vt) > op.cutoff ? subtract(scaled(vt, op.friction), scaled(vn, op.resilience))
                                             : subtract(vt, scaled(vn, op.resilience));
    }
}

void ParticleInstance::emit(const tr::ParticleOperator& op, size_t group, float dt) {
    // pfxEmit::Apply (FUN_009e2a50).
    SlotState& s = slots_[op.stateSlot];
    s.time += dt;
    const tr::ParticleGroup& g = system_->groups[group];
    // The group's Enabled flag (group + 0x154) is set by the system's reset when it starts (FUN_005834b0 -> each group's
    // FUN_0066cb30), whatever the file stores (0 in 2,544 of 4,788 shipped groups) - so every group emits.
    const tr::ParticleEmitter& e = op.emitter;
    const float wanted = evaluate(e.rate, s.time).x * dt;
    int32_t n = int32_t(std::floor(wanted));
    const float fraction = wanted - float(n);
    if (e.roundingMode == 0) {
        if (random() < fraction) ++n;
    } else if (e.roundingMode == 1) {
        s.accumulator += fraction;
        if (s.accumulator > 1.0f) {
            ++n;
            s.accumulator -= 1.0f;
        }
    }
    std::vector<Particle>& list = particles_[group];
    n = std::min<int32_t>(n, int32_t(g.maxParticles) - int32_t(list.size()));
    for (int32_t k = 0; k < n; ++k) {
        Particle p;
        p.position = point(evaluate(e.point, s.time));
        if (g.referenceFrame == 1 || g.referenceFrame == 2) p.position = transformPoint(world_, p.position);
        p.previous = p.position;
        p.velocity = point(evaluate(e.velocity, s.time));
        if (g.referenceFrame == 2) p.velocity = transformDirection(world_, p.velocity);
        p.colour = evaluate(e.colour, s.time);
        const float life = evaluate(e.lifetime, s.time).x;
        p.inverseLife = life < 100000.0f && life != 0.0f ? 1.0f / life : 0.0f;
        p.size = evaluate(e.size, s.time).x;
        p.growth = evaluate(e.growth, s.time).x;
        p.rotation = evaluate(e.rotation, s.time).x;
        p.spin = evaluate(e.spin, s.time).x;
        p.stretch = evaluate(e.stretch, s.time).x;
        // Spread over the frame: each new particle has already lived a random part of dt.
        const float f = random() * dt;
        p.position = add(p.position, scaled(p.velocity, f));
        p.age += p.inverseLife * f;
        p.size += p.growth * f;
        p.rotation += p.spin * f;
        list.push_back(p);
    }
}

void ParticleInstance::apply(const tr::ParticleOperator& op, size_t group, float dt) {
    using K = tr::ParticleOperatorKind;
    if (!op.enabled) return;
    std::vector<Particle>& list = particles_[group];
    SlotState& s = slots_[op.stateSlot];
    switch (op.kind) {
        case K::Composite:
            for (const tr::ParticleOperator& child : op.children) apply(child, group, dt);
            break;
        case K::Sequence: {
            // pfxSequence::Apply (FUN_009af8b0): each stage gets the part of [time, time + dt] inside its window.
            const float start = s.time, end = s.time + dt;
            s.time = end;
            if (!s.active) break;
            for (size_t k = s.index; k < op.stages.size(); ++k) {
                const float stageStart = op.stages[k].time;
                if (end < stageStart) break;
                if (op.stages[k].child < 0) continue;
                const float stageEnd = k + 1 < op.stages.size() ? op.stages[k + 1].time : 100000.0f;
                const float slice = std::min(end, stageEnd) - std::max(start, stageStart);
                if (slice <= 0.0f) s.index = uint32_t(k);
                else apply(op.children[size_t(op.stages[k].child)], group, slice);
            }
            if (s.index >= op.stages.size()) s.active = false;
            break;
        }
        case K::ApplyAfter:
            s.time += dt;
            if (s.time > op.time && !op.children.empty()) apply(op.children.front(), group, dt);
            break;
        case K::Emit: emit(op, group, dt); break;
        case K::EmitFromParticle: emitFromParticles(op, group, dt); break;
        case K::Bounce: bounce(op, group, dt); break;
        case K::Move:
            for (size_t k = 0; k < list.size();) {
                Particle& p = list[k];
                p.position = add(p.position, scaled(p.velocity, dt));
                p.age += p.inverseLife * dt;
                p.size += p.growth * dt;
                p.rotation += p.spin * dt;
                if (p.age < 1.0f) ++k;
                else remove(group, k);
            }
            break;
        case K::Drag:
            for (Particle& p : list) p.velocity = add(p.velocity, scaled(p.velocity, length(p.velocity) * op.coefficient * dt));
            break;
        case K::Accelerate:
            for (Particle& p : list) p.velocity = add(p.velocity, scaled(op.vector, dt));
            break;
        case K::AccelerateSpin:
            for (Particle& p : list) p.spin += op.coefficient * dt;      // pfxAccelerateSpin::Apply (FUN_00971640, particle + 0x48)
            break;
        case K::Gravity:
            for (Particle& p : list) {
                const tr::Vec3 d = subtract(op.vector, p.position);
                p.velocity = add(p.velocity, scaled(d, op.coefficient / (dot(d, d) + 0.001f) * dt));
            }
            break;
        case K::Vortex: {
            // pfxVortex::Apply (FUN_009b7e70, traced 2026-10-20) - the Particle System API's vortex: each particle's POSITION turns
            // about the axis through the centre (the centre taken into world space for World / Hybrid groups; the axis as stored)
            // by magnitude / (r^2 + 0.001) - per call, not x dt. offset = p - centre, n = offset / r, w = (n . axis) axis,
            // u = n - w, v = axis x u; p = centre + (u cos a + v sin a + w) r, sin taken as +-sqrt(1 - cos^2), negative below 0
            // and above pi.
            const tr::Vec3 centre = system_->groups[group].referenceFrame == 0 ? op.vector : transformPoint(world_, op.vector);
            for (Particle& p : list) {
                const tr::Vec3 offset = subtract(p.position, centre);
                const float r2 = dot(offset, offset);
                if (r2 <= 0.0f) continue;
                const float r = std::sqrt(r2);
                const tr::Vec3 n = scaled(offset, 1.0f / r);
                const tr::Vec3 w = scaled(op.axis, dot(n, op.axis));
                const tr::Vec3 u = subtract(n, w);
                const tr::Vec3 v = cross(op.axis, u);
                const float angle = op.coefficient / (r2 + 0.001f);
                const float c = std::cos(angle);
                float sn = std::sqrt(std::max(0.0f, 1.0f - c * c));
                if (angle < 0.0f || angle > 3.14159265f) sn = -sn;
                p.position = add(centre, scaled(add(add(scaled(u, c), scaled(v, sn)), w), r));
            }
            break;
        }
        case K::TargetColour: {
            const float f = std::clamp(op.time * dt, 0.0f, 1.0f);
            for (Particle& p : list) p.colour = lerpValue(p.colour, op.target, f);
            break;
        }
        case K::TargetAlpha:
            for (Particle& p : list) p.colour.w += op.time * dt * (op.target.w - p.colour.w);
            break;
        case K::KeyColour:
            if (!op.keys.empty())
                for (Particle& p : list) p.colour = sampleKeys(op.keys, p.age);
            break;
        case K::SetColour:
            for (Particle& p : list) p.colour = evaluate(op.generator, p.age);
            break;
        case K::SetGrowth:
            for (Particle& p : list) p.growth = evaluate(op.generator, p.age).x;
            break;
        case K::SetPosition:
            for (Particle& p : list) p.position = point(evaluate(op.generator, p.age));
            break;
        case K::SetRotation:
            for (Particle& p : list) p.rotation = evaluate(op.generator, p.age).x;
            break;
        case K::SetSize:
            for (Particle& p : list) p.size = evaluate(op.generator, p.age).x;
            break;
        case K::SetVelocity:
            for (Particle& p : list) p.velocity = point(evaluate(op.generator, p.age));
            break;
        case K::SetStretch:
            for (Particle& p : list) p.stretch = evaluate(op.generator, p.age).x;
            break;
        case K::SetSpin:
            for (Particle& p : list) p.spin = evaluate(op.generator, p.age).x;
            break;
        case K::AlignToVelocity:
            for (Particle& p : list) p.previous = add(p.position, p.velocity);
            break;
        case K::TerminateOutside: {
            // pfxTerminateOutside::Apply (FUN_00999060): removes the particles the domain's inside test rejects; the domain is never
            // transformed (compared with the positions as they are) and its invert flag is not read.
            const PlacedDomain pd = placeDomain(op.domain, nullptr);
            for (size_t k = 0; k < list.size();) {
                if (domainInside(pd, list[k].position)) ++k;
                else remove(group, k);
            }
            break;
        }
        case K::Dummy: break;
    }
}

void ParticleInstance::update(float dt) {
    // The system's Enabled flag, like its groups', is set by its reset at start (FUN_005834b0 -> FUN_005845d0(system, 1)).
    if (dt <= 0.0f) return;
    for (size_t g = 0; g < system_->groups.size(); ++g) apply(system_->groups[g].op, g, dt);
}

bool ParticleInstance::worldBounds(tr::Vec3& centre, float& radius) const {
    tr::Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    float largest = 0;
    bool any = false;
    for (size_t g = 0; g < particles_.size() && g < system_->groups.size(); ++g) {
        const bool objectFrame = system_->groups[g].referenceFrame == 0;
        for (const Particle& p : particles_[g]) {
            const tr::Vec3 c = objectFrame ? transformPoint(world_, p.position) : p.position;
            lo = tr::Vec3{std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
            hi = tr::Vec3{std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
            largest = std::max(largest, std::fabs(p.size));
            any = true;
        }
    }
    if (!any) return false;
    centre = tr::Vec3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    const tr::Vec3 half{(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f};
    radius = std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z) + largest;
    return true;
}

void ParticleInstance::buildQuads(size_t group, const tr::Vec3& cameraRight, const tr::Vec3& cameraUp, std::vector<MeshVertex>& out) const {
    const tr::ParticleGroup& g = system_->groups[group];
    if (g.primitive < 2 || g.primitive > 4) return;
    const bool objectFrame = g.referenceFrame == 0;
    const bool camera = g.billboard == 3;
    // Sprite axes in the system's frame: billboard X -> (y, z), Y -> (x, z), Z -> (x, y).
    const tr::Vec3 axisA = g.billboard == 0 ? tr::Vec3{0, 1, 0} : tr::Vec3{1, 0, 0};
    const tr::Vec3 axisB = g.billboard == 2 ? tr::Vec3{0, 1, 0} : tr::Vec3{0, 0, 1};
    auto toWorld = [&](const tr::Vec3& v) { return objectFrame ? transformDirection(world_, v) : v; };
    static const float cornerX[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
    static const float cornerY[4] = {0.5f, -0.5f, 0.5f, -0.5f};
    static const float cornerU[4] = {0, 0, 1, 1};
    static const float cornerV[4] = {0, 1, 0, 1};
    static const int order[6] = {0, 1, 2, 2, 1, 3};
    // Sorted groups: emission order, newest first for sort order 0 (new particles at the list's head), oldest first for 1.
    const std::vector<Particle>& list = particles_[group];
    const bool newestFirst = g.sorted && g.sortOrder == 0;
    for (size_t index = 0; index < list.size(); ++index) {
        const Particle& p = list[newestFirst ? list.size() - 1 - index : index];
        const tr::Vec3 centre = objectFrame ? transformPoint(world_, p.position) : p.position;
        tr::Vec3 a, b;                      // world-space directions of u and v
        if (g.primitive == 4) {
            tr::Vec3 n{0, 1, 0}, t{1, 0, 0};
            if (dot(p.velocity, p.velocity) >= 1e-4f) {
                n = normalized(p.velocity);
                const tr::Vec3 c = cross(n, tr::Vec3{0, 1, 0});
                t = dot(c, c) < 1e-4f ? tr::Vec3{1, 0, 0} : normalized(c);
            }
            const tr::Vec3 nt = cross(n, t);
            switch (g.billboard) {
                case 0: a = toWorld(n); b = toWorld(nt); break;
                case 1: a = toWorld(t); b = toWorld(n); break;
                case 2: a = toWorld(t); b = toWorld(nt); break;
                default: {
                    // The shader adds u T.xy + v N.xy in view space.
                    a = add(scaled(cameraRight, t.x), scaled(cameraUp, t.y));
                    b = add(scaled(cameraRight, n.x), scaled(cameraUp, n.y));
                    break;
                }
            }
        } else if (camera) {
            a = cameraRight;
            b = cameraUp;
        } else {
            a = toWorld(axisA);
            b = toWorld(axisB);
        }
        float alignX = 1, alignY = 0;       // aligned sprites: (u, v) turned towards the previous position
        if (g.primitive == 3) {
            const tr::Vec3 delta = objectFrame ? transformDirection(world_, subtract(p.previous, p.position)) : subtract(p.previous, p.position);
            const float da = camera ? dot(delta, cameraRight) : dot(delta, a), db = camera ? dot(delta, cameraUp) : dot(delta, b);
            const float len = std::sqrt(da * da + db * db);
            if (len > 1e-6f) {
                alignX = da / len;
                alignY = db / len;
            }
        }
        const float size = std::max(p.size, 0.0f);
        const float s = std::exp2(p.stretch);
        const float angle = p.rotation * twoPi;
        const float cosA = std::cos(angle), sinA = std::sin(angle);
        const uint32_t colour = packColour(unitByte(p.colour.x), unitByte(p.colour.y), unitByte(p.colour.z), unitByte(p.colour.w));
        MeshVertex corners[4];
        for (int k = 0; k < 4; ++k) {
            const float cx = cornerX[k] * size, cy = cornerY[k] * size;
            float u = s * cx * cosA - (cy / s) * sinA;
            float v = s * cx * sinA + (cy / s) * cosA;
            const float ru = u * alignX - v * alignY, rv = u * alignY + v * alignX;
            u = ru;
            v = rv;
            const tr::Vec3 w = add(centre, add(scaled(a, u), scaled(b, v)));
            MeshVertex& m = corners[k];
            m.positionX = w.x; m.positionY = w.y; m.positionZ = w.z;
            m.normalY = 1;
            m.colour = colour;
            m.texU = cornerU[k];
            m.texV = cornerV[k];
        }
        for (int k : order) out.push_back(corners[k]);
    }
}
