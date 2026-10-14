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
        case K::Geo:
            ++unsupported_;
            return tr::ParticleValue{};
    }
    return tr::ParticleValue{};
}

void ParticleInstance::emit(const tr::ParticleOperator& op, size_t group, float dt) {
    // pfxEmit::Apply (FUN_009e2a50).
    SlotState& s = slots_[op.stateSlot];
    s.time += dt;
    const tr::ParticleGroup& g = system_->groups[group];
    if (!g.enabled) return;
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
        case K::EmitFromParticle:
        case K::Bounce: ++unsupported_; break;
        case K::Move:
            for (size_t k = 0; k < list.size();) {
                Particle& p = list[k];
                p.position = add(p.position, scaled(p.velocity, dt));
                p.age += p.inverseLife * dt;
                p.size += p.growth * dt;
                p.rotation += p.spin * dt;
                if (p.age < 1.0f) {
                    ++k;
                } else {
                    p = list.back();             // the engine removes by moving the last particle into the slot
                    list.pop_back();
                }
            }
            break;
        case K::Drag:
            for (Particle& p : list) p.velocity = add(p.velocity, scaled(p.velocity, length(p.velocity) * op.coefficient * dt));
            break;
        case K::Accelerate:
            for (Particle& p : list) p.velocity = add(p.velocity, scaled(op.vector, dt));
            break;
        case K::AccelerateSpin:
            for (Particle& p : list) p.spin += op.coefficient * dt;      // UNVERIFIED (not decompiled)
            break;
        case K::Gravity:
            for (Particle& p : list) {
                const tr::Vec3 d = subtract(op.vector, p.position);
                p.velocity = add(p.velocity, scaled(d, op.coefficient / (dot(d, d) + 0.001f) * dt));
            }
            break;
        case K::Vortex: {
            // UNVERIFIED (not decompiled): swirl about the axis through the centre.
            const tr::Vec3 axis = normalized(op.axis);
            for (Particle& p : list) p.velocity = add(p.velocity, scaled(cross(axis, subtract(p.position, op.vector)), op.coefficient * dt));
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
            // UNVERIFIED domain conventions (13 uses): box axis-aligned with full dimensions, half space n.p >= distance.
            const tr::ParticleDomain& d = op.domain;
            auto inside = [&](const tr::Vec3& p) {
                switch (d.kind) {
                    case tr::ParticleDomain::Kind::Box: {
                        const tr::Vec3 q = subtract(p, d.centre);
                        const bool in = std::fabs(q.x) <= d.dimension.x * 0.5f && std::fabs(q.y) <= d.dimension.y * 0.5f && std::fabs(q.z) <= d.dimension.z * 0.5f;
                        return in != d.invert;
                    }
                    case tr::ParticleDomain::Kind::HalfSpace: return dot(d.normal, p) >= d.distance;
                    case tr::ParticleDomain::Kind::Sphere: return (length(subtract(p, d.centre)) <= d.radius) != d.invert;
                }
                return true;
            };
            list.erase(std::remove_if(list.begin(), list.end(), [&](const Particle& p) { return !inside(p.position); }), list.end());
            break;
        }
        case K::Dummy: break;
    }
}

void ParticleInstance::update(float dt) {
    if (!system_->enabled || dt <= 0.0f) return;
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
    for (const Particle& p : particles_[group]) {
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
