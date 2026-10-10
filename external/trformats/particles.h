// particles.h - particle systems (.pfx, 2,097 files in the archives): the engine's "palantir" pfx library.
//
// Chunk file (CHNKBLXX, chunks as in .geo). Layouts from tabula_rasa.exe's unserializers (pfx*::Unserialize, names from
// the RTTI and Property_pfx* strings) and verified 2026-10-10: every byte of all 2,097 files is consumed.
//   PSYS v4 { u8 enabled, u32 n, n x (group, operator), BBOX, f32, u32 }  (v2: no enabled byte; v1: no BBOX / tail)
//   group: PGRP (pfxParticleGroup) or PGRS { PGRP, u8 sortOrder } (pfxParticleGroupSorted)
//   PGRP v8 { u8 enabled, cstr name, u32 zBias, u32 maxParticles, u8 primitive, u8 referenceFrame,
//             u32 D3D SrcBlend, u32 DestBlend, u32 BlendOp, u8 UNKNOWN, [primitive 2..4: ISTR texture, u32 billboard] }
//         v7 without the UNKNOWN byte, v6 without enabled, v5 without name, v4 { u32 maxParticles, u8 primitive,
//         u8 referenceFrame, 3 x u32 blend, [ISTR, u32] }.
//   Operators (pfxParticleOperator) and generators (pfxFloat/Point/ColorGenerator) are chunks; see the kinds below.
//   Operators of chunk version >= 2 (VORT >= 3) start with u8 enabled.
// Semantics (decompiled; the exe function is named where it matters):
//   - Per frame each group's operator is applied with the time step dt (seconds).
//   - Particle: position, previous position, velocity, colour rgba, age (0..1), 1 / lifetime, size, growth, rotation
//     (turns), spin, stretch. MOVE integrates position += velocity dt, age += dt / lifetime, size += growth dt,
//     rotation += spin dt, and kills particles whose age reaches 1 (no MOVE: particles never age).
//   - EMIT (pfxEmit::Apply, FUN_009e2a50): emitter time t += dt; n = rate(t) * dt, the fraction rounded by
//     roundingMode (0 randomly, 1 accumulated), capped at maxParticles; each new particle takes point(t), velocity(t),
//     colour(t), size(t), growth(t), rotation(t), spin(t), stretch(t), 1 / lifetime(t), and is advanced by a random
//     fraction of dt. Reference frame World / Hybrid: position (and for Hybrid the velocity) taken to world space.
//   - SET* operators evaluate their generator at the particle's age, so keyframe times are fractions of a life.
//   - Generators keep state between calls: SEQx cycles its children per call, HLDx holds a value for round(count)
//     calls, LBDx moves linearly from the previous base value to the next over round(count) calls.
//   - SEQ (pfxSequence): stage k runs from its time until stage k + 1's (the last one forever); each frame its
//     operator gets the part of [t, t + dt] inside its window.
#pragma once

#include "trformats.h"

#include <string>
#include <vector>

namespace tr {

struct ParticleValue { float x = 0, y = 0, z = 0, w = 0; };   // float: x; point: xyz; colour: rgba (0..1)

enum class ParticleGeneratorKind : uint8_t {
    Constant,       // CONF / CONP / CONC: value
    Uniform,        // UNIF: minimum + (maximum - minimum) * random
    UnitRandom,     // UUNI: random 0..1
    Gaussian,       // GAUS: mean, deviation (normal distribution - UNVERIFIED shape)
    Sine,           // SINE: amplitude * sin(2 pi frequency (t - offset))
    Sequence,       // SEQF / SEQP / SEQC: children in turn, one per call
    Random,         // RNDF / RNDP / RNDC: a random child per call (UNVERIFIED: not decompiled)
    Hold,           // HLDF / HLDP / HLDC: children count, base; base value held for round(count) calls
    Blend,          // LBDF / LBDP: children count, base; linear walk from the last base value to the next
    Add,            // ADDF / ADDP / ADDC: children first + second
    Keys,           // KEYF / KEYP / KEYC: keys (time, value), linear, clamped at the ends
    Sphere,         // SPEP: centre + r (sin phi cos theta, cos phi, sin phi sin theta); children radius, theta (turns),
                    //       phi (half turns)
    Box,            // BOXP: children x, y, z
    Cylinder,       // CYLP: centre + distance * axis + radius (cos a axisU + sin a axisV); children distance, angle
                    //       (turns), radius
    Range,          // RNGP: centre + child(t) * axis;  RNGC: lerp(colour1, colour2, child(t))
    PointColour,    // PTCL: children point (rgb), alpha
    Geo,            // GEOG: points on a model (geo); not evaluated
};

struct ParticleKey {
    float time = 0;
    ParticleValue value;
};

struct ParticleGenerator {
    ParticleGeneratorKind kind = ParticleGeneratorKind::Constant;
    uint32_t tag = 0;                       // chunk tag (fourcc), e.g. CONF
    ParticleValue value;                    // Constant
    float minimum = 0, maximum = 1;         // Uniform
    float mean = 0, deviation = 0;          // Gaussian
    float amplitude = 0, frequency = 0, offset = 0;   // Sine
    Vec3 centre;                            // Sphere, Cylinder, Range (point)
    Vec3 axis;                              // Cylinder, Range (point): direction
    Vec3 axisU, axisV;                      // Cylinder
    ParticleValue colour1, colour2;         // Range (colour)
    std::vector<ParticleKey> keys;          // Keys
    std::vector<ParticleGenerator> children;    // roles per kind, see above
    std::string geo;                        // Geo
    uint32_t stateSlot = 0;                 // index of this generator's run-time state (ParticleSystem::stateCount)
};

// The i-th child, or a constant 1 if there is none (the engine's default for a missing generator).
const ParticleGenerator& generatorChild(const ParticleGenerator& generator, size_t index);

// pfxEmit (EMIT) / pfxEmitFromParticle (PEMT, no point).
struct ParticleEmitter {
    ParticleGenerator rate;                 // particles per second
    ParticleGenerator point;
    ParticleGenerator velocity;
    ParticleGenerator colour;
    ParticleGenerator size;
    ParticleGenerator growth;               // size per second
    ParticleGenerator rotation;             // turns
    ParticleGenerator spin;                 // turns per second
    ParticleGenerator stretch;              // the sprite is 2^stretch wider and 2^-stretch taller
    ParticleGenerator lifetime;             // seconds
    uint32_t roundingMode = 0;              // 0 Randomize, 1 Accumulate
};

// Domain of TOUT / BONC: BOXS box (centre, rotation, dimension, invert), HALF half space (normal, distance), SPHE
// sphere (centre, radius, invert).
struct ParticleDomain {
    enum class Kind : uint8_t { Box, HalfSpace, Sphere };
    Kind kind = Kind::Box;
    Vec3 centre;
    Vec3 rotation;
    Vec3 dimension;
    Vec3 normal;
    float distance = 0;
    float radius = 0;
    bool invert = false;
};

enum class ParticleOperatorKind : uint8_t {
    Composite,          // COMP: children in order
    Sequence,           // "SEQ ": stages (time, optional operator in children)
    ApplyAfter,         // APAF: the child, once more than time seconds have passed
    Emit,               // EMIT
    EmitFromParticle,   // PEMT (not simulated)
    Move,               // MOVE
    Drag,               // DRAG: velocity += |velocity| * coefficient * dt * velocity
    Accelerate,         // ACCL: velocity += vector * dt
    AccelerateSpin,     // ACCS: spin += coefficient * dt
    Gravity,            // GRAV: velocity += (vector - position) * coefficient / (|vector - position|^2 + 0.001) * dt
    Vortex,             // VORT: vector (centre), axis, coefficient (magnitude) - approximated
    TargetColour,       // TCOL: colour moves towards target, rate time per second
    TargetAlpha,        // TALP: alpha += time * dt * (target.w - alpha)
    KeyColour,          // KCLR: colour from keys at the particle's age
    SetColour, SetGrowth, SetPosition, SetRotation, SetSize, SetVelocity, SetStretch, SetSpin,   // SETC SETG SETP SETR SETS SETV SEST SESP
    AlignToVelocity,    // ALTV: previous position = position + velocity (aligned sprites point along the velocity)
    TerminateOutside,   // TOUT: kills particles outside the domain
    Bounce,             // BONC (not simulated)
    Dummy,              // DUMM
};

struct ParticleStage {
    float time = 0;
    int32_t child = -1;                     // index into ParticleOperator::children, -1 = no operator
};

struct ParticleOperator {
    ParticleOperatorKind kind = ParticleOperatorKind::Dummy;
    uint32_t tag = 0;
    bool enabled = true;
    ParticleEmitter emitter;                // Emit, EmitFromParticle
    ParticleGenerator generator;            // Set*
    std::vector<ParticleOperator> children; // Composite, Sequence (stage operators), ApplyAfter (one)
    std::vector<ParticleStage> stages;      // Sequence
    float coefficient = 0;                  // Drag, AccelerateSpin, Gravity, Vortex
    Vec3 vector;                            // Accelerate, Gravity / Vortex centre
    Vec3 axis;                              // Vortex
    float time = 0;                         // ApplyAfter, TargetColour, TargetAlpha
    float unknown = 0;                      // ApplyAfter: second float (UNKNOWN)
    ParticleValue target;                   // TargetColour; TargetAlpha in w
    std::vector<ParticleKey> keys;          // KeyColour
    float friction = 0, resilience = 0, cutoff = 0;   // Bounce
    ParticleDomain domain;                  // TerminateOutside, Bounce
    uint32_t stateSlot = 0;
};

struct ParticleGroup {
    uint32_t version = 0;
    bool enabled = true;
    std::string name;
    uint32_t zBias = 0;                     // reading UNVERIFIED (the field the reader stores before MaxParticles)
    uint32_t maxParticles = 0;
    uint8_t primitive = 2;                  // 0 Point, 1 Line, 2 Sprite, 3 Aligned Sprite, 4 Oriented Sprite
    uint8_t referenceFrame = 0;             // 0 Object, 1 World, 2 Hybrid
    uint32_t sourceBlend = 5;               // D3DBLEND (5 SRCALPHA)
    uint32_t destinationBlend = 2;          // D3DBLEND (2 ONE, 6 INVSRCALPHA)
    uint32_t blendOperation = 1;            // D3DBLENDOP (1 ADD, 3 REVSUBTRACT)
    uint8_t unknownFlag = 0;                // v8 only; UNKNOWN (perhaps FogEnabled)
    std::string texture;
    uint32_t billboard = 3;                 // X 0, Y 1, Z 2, Camera 3: the sprite's plane is across that axis
    bool sorted = false;                    // PGRS
    uint8_t sortOrder = 0;
    ParticleOperator op;
};

// BlendType as the engine derives it from the D3D states (pfxParticleGroup, FUN_0066f600):
// 0 Additive (SRCALPHA, ONE, ADD), 3 Subtractive (SRCALPHA, ONE, other op), 1 Multiply (SrcBlend ZERO), 2 Diffuse.
uint32_t particleBlendType(const ParticleGroup& group);

struct ParticleSystem {
    uint32_t version = 0;
    bool enabled = true;
    std::vector<ParticleGroup> groups;
    float warmupInterval = 0;               // PSYS tail f32 (reading UNVERIFIED; 0 in 1,973 files)
    uint32_t warmupCount = 0;               // PSYS tail u32 (reading UNVERIFIED)
    uint32_t stateCount = 0;                // generators + operators (their stateSlot values are 0 .. stateCount - 1)
};

ParticleSystem readParticleSystem(const Bytes& data);

// Particle systems a model carries: "psys" values "<contact point id>,<file>.pfx" from its contact points, its GBOD
// property table and its pieces' user data (the same value often appears in several), without duplicates.
// 2,147 of 2,203 references in the shipped models name an existing contact point.
struct ParticleAttachment {
    uint32_t contactPoint = 0;
    std::string file;
};
std::vector<ParticleAttachment> particleAttachments(const GeoModel& model);
const GeoContactPoint* findContactPoint(const GeoModel& model, uint32_t id);

}  // namespace tr
