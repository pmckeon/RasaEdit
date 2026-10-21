#pragma once

// ParticleSim - runs the game's particle systems (trformats particles.h) and builds their sprite quads.
//
// The simulation follows the decompiled engine (pfxEmit::Apply, pfxMove, the generators; see particles.h). The
// sprite quads follow the PalParticleSystem*.fx vertex shaders (effects.glm, disassembled 2026-10-10): corner
// (cx, cy) = size x (+-0.5, +-0.5), uv (0,0) top-left; with s = 2^stretch and a = rotation x 2 pi
//   u = s cx cos a - (cy / s) sin a,   v = s cx sin a + (cy / s) cos a
// laid along two axes chosen by the group's primitive and billboard type:
//   Sprite:   X -> (y, z), Y -> (x, z), Z -> (x, y) of the system's frame; Camera -> the view's right and up.
//   Aligned:  as Sprite, (u, v) turned towards the previous position (ALTV sets it to position + velocity).
//   Oriented: N = the velocity's direction ((0, 1, 0) when |v|^2 < 1e-4), T = normalize(N x (0, 1, 0)) ((1, 0, 0)
//             when degenerate): X -> (N, N x T), Y -> (T, N), Z -> (T, N x T), Camera -> T, N used as view-space
//             vectors (as the shader does).
// Points and lines (primitives 0 and 1) are not drawn - no shipped .pfx uses them (viewport_test RASA_PFXSCAN).
// Sorted groups (PGRS, pfxParticleGroupSorted) keep their particles in emission order (a linked list: removal unlinks) and
// draw the newest first (sort order 0, new particles at the head) or last (1, at the tail); other groups keep an array where a
// removed particle is replaced by the last one (traced 2026-10-20, FUN_0067f730 / FUN_0067f7e0 / FUN_0067fa40).

#include "GpuMath.h"
#include "Renderer.h"
#include "particles.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct Particle {
    tr::Vec3 position;
    tr::Vec3 previous;
    tr::Vec3 velocity;
    tr::ParticleValue colour;
    float age = 0;              // 0..1
    float inverseLife = 0;      // 1 / lifetime (0: immortal)
    float size = 0;
    float growth = 0;
    float rotation = 0;         // turns
    float spin = 0;
    float stretch = 0;
};

// Points of GEOG generators (pfxGeoGenerator): each piece's stored vertex positions of a model, by .geo name (nullptr when missing).
using GeoPointSource = std::function<const std::vector<std::vector<tr::Vec3>>*(const std::string& geo)>;

class ParticleInstance {
public:
    // world: the system's frame (entity placement x contact point). seed: per-instance random sequence.
    ParticleInstance(const tr::ParticleSystem& system, const Mat4& world, uint32_t seed);

    void update(float dt);
    void setGeoPoints(GeoPointSource source) { geoPoints_ = std::move(source); }
    size_t particleCount() const;
    // World-space bounding sphere of the live particles (positions, grown by the largest size); false when there are none.
    bool worldBounds(tr::Vec3& centre, float& radius) const;
    const tr::ParticleSystem& system() const { return *system_; }
    const Mat4& world() const { return world_; }
    // Appends group g's quads as world-space triangles (6 vertices per particle).
    void buildQuads(size_t group, const tr::Vec3& cameraRight, const tr::Vec3& cameraUp, std::vector<MeshVertex>& out) const;
    // Generators the simulation could not evaluate (GEOG without a model) - for the status line.
    uint32_t unsupported() const { return unsupported_; }

private:
    struct SlotState {          // run-time state of one generator or operator (indexed by its stateSlot)
        uint32_t index = 0;     // Sequence generator: next child; Sequence operator: first live stage
        int32_t count = 0;      // Hold / Blend: calls per value
        int32_t step = 0;       // Blend: calls into the current segment
        tr::ParticleValue held; // Hold: the value; Blend: target
        tr::ParticleValue from; // Blend: start
        float time = 0;         // Emit: emitter time; Sequence / ApplyAfter: elapsed
        float accumulator = 0;  // Emit (roundingMode 1): fractional particles
        bool active = true;     // Sequence: false once past its last stage
    };
    float random();
    float gaussian();
    tr::ParticleValue evaluate(const tr::ParticleGenerator& g, float t);
    void apply(const tr::ParticleOperator& op, size_t group, float dt);
    void emit(const tr::ParticleOperator& op, size_t group, float dt);
    void emitFromParticles(const tr::ParticleOperator& op, size_t group, float dt);
    void bounce(const tr::ParticleOperator& op, size_t group, float dt);
    void remove(size_t group, size_t index);      // as the group's container does (sorted: in order; else the last moves in)

    const tr::ParticleSystem* system_;
    Mat4 world_;
    uint32_t random_;
    std::vector<SlotState> slots_;
    std::vector<std::vector<Particle>> particles_;      // per group
    uint32_t unsupported_ = 0;
    GeoPointSource geoPoints_;
};
