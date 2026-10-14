// sky.h - sky files (.sky, data/sky.glm): day-cycle lighting, fog and sky layers.
//
// XML <SkyData Version="3"> made of <PropertyObject class=…><property name value type/>… (verified 2026-10-10 on
// all 144 files; 143 are this format, sky_foreas_cave_room.sky is an older <FogData> format - not supported):
//   GlobalLights/Lights/Light: LightData (Name, Hemi, Orientation, RenderMode, …) + LightKeyFrames
//   SkyLayers/SkyLayer: SkyLayerData (GeoFilename, RotationAxis, RotsPerDay, InitialPitch/Yaw/Roll, UseFogColor, …),
//     AlphaKeyFrames, OffsetKeyFrames, Lights (directional lights carried by the layer: the sun, the moon)
//   FogFrames/FogFrame: FogKeyFrame (Time, Color, StartDist, EndDist, Used; Density in 3 frames)
// Key times are fractions of the day, 0..1, not sorted in the files. Colours "r, g, b, a" are 0..255.
// Light key "Type": 1/2 on hemispheric lights, 3/4 on directional layer lights. The older format names hemi keys
// "Front"; read here as 1 = front (sky / towards the light), 2 = back (ground / away) and 3/4 the same for
// directional lights (UNVERIFIED reading).
#pragma once

#include "trformats.h"

#include <map>
#include <string>
#include <vector>

namespace tr {

struct SkyLightKey {
    float time = 0;
    uint32_t type = 0;                // 1/2 hemi front/back, 3/4 directional front/back (see above)
    Vec3 colour;                      // "Diffuse" (written b, g, r, a; = "DiffuseColor" r, g, b)
    float diffuseIntensity = 1;       // "DIntensity"
    float ambientIntensity = 0;       // "AIntensity" (UNKNOWN use)
    float specularIntensity = 0;      // "SIntensity" (UNKNOWN use)
    float brightness = 1;             // "DBrightness" (UNKNOWN use)
    float shadowOpacity = 1;          // "ShadowOpacity": type-3 keys form the light's shadow-opacity track (FUN_00841470)
};

struct SkyLight {
    std::string name;
    bool hemi = false;
    bool active = true;
    bool castsShadows = false;
    Vec3 orientation{0, 1, 0};        // directional: direction the light travels, before the layer's rotation
    uint32_t renderMode = 0;          // UNKNOWN meaning (1, 0, 2, 99 occur)
    std::vector<SkyLightKey> keys;
};

struct SkyFogKey {
    float time = 0;
    Vec3 colour;                      // "Color" (written b, g, r, a); also the sky colour: dome layers with UseFogColor take it
    float startDistance = 0;
    float endDistance = 0;
    bool used = false;                // false in every shipped sky (fog is enabled elsewhere)
};

struct SkyAlphaKey {
    float time = 0;
    float alpha = 1;
};

// A layer's keyed colour (the AlphaKeyFrames list of ColorKeyFrame objects: Color r, g, b, a - a = Alpha in every shipped key):
// the engine adds rgb to the layer's base colour and uses a as its alpha (TRasa::SkyLayer, see sampleLayerColour).
struct SkyColourKey {
    float time = 0;
    Vec3 colour;
    float alpha = 1;
};

// OffsetKeyFrames: X, Y, Z added to the layer's position (the camera's) - FUN_00885af0 / FUN_00885f90.
struct SkyOffsetKey {
    float time = 0;
    Vec3 offset;
};

struct SkyLayer {
    bool enabled = true;
    std::string geoFilename;
    Vec3 rotationAxis{0, 1, 0};
    float rotationsPerDay = 0;
    float initialPitch = 0, initialYaw = 0, initialRoll = 0;     // radians
    bool useFogColour = false;
    // DiffuseColor (packed, written b, g, r, a like the light keys' Diffuse; = Diffuse r, g, b, a): the base colour
    // (FUN_00823470 reads bytes 2, 1, 0, 3 x 1/255 into the layer's + 0x64 .. + 0x70).
    Vec3 baseColour{1, 1, 1};
    std::vector<SkyAlphaKey> alphaKeys;
    std::vector<SkyColourKey> colourKeys;
    std::vector<SkyOffsetKey> offsetKeys;
    std::vector<SkyLight> lights;
};

struct Sky {
    bool weatherActive = false;
    std::vector<SkyLight> globalLights;
    std::vector<SkyLayer> layers;
    std::vector<SkyFogKey> fog;
};

Sky readSky(const Bytes& data);

// Placed lights: a map folder's lights.xml and light templates (.ltmp, lights.glm), XML <LightRoot><Lights><Light
// Type=…><PropertyObject class="LightImplPropsPoint|Spot|Plane">…. Type fourccs 'LIPT' point, 'LISP' spot, 'LIPL'
// plane. <Mods> (animated intensity/colour/position/rotation modifiers) are not read.
// The game's spot pass shines along the light's local +z (verified in trspotlight); WorldOrientation is read as
// x, y, z, w (UNVERIFIED). UNVERIFIED readings: Cone/PenumbraAngle are full angles in degrees; FalloffPercent × Range
// is the distance where the light starts to fade (the shaders' LightFalloff).
struct PlacedLight {
    enum class Kind : uint8_t { Point, Spot, Plane };
    Kind kind = Kind::Point;
    std::string name;
    bool enabled = true;
    bool castsShadows = false;
    Vec3 position;
    Vec3 offset;                      // Offset: in a light template (.ltmp), its place relative to the contact point
    Vec3 colour{1, 1, 1};
    float intensity = 1;
    float range = 10;
    float falloffPercent = 1;
    Quat worldOrientation;            // WorldOrientation (spot / plane), else LocalOrientation (point lights), x y z w
    std::string globeMap;             // GlobeMapFile: a cube map the light projects around itself ("" = none)
    float coneAngle = 45;             // spot, degrees
    float penumbraAngle = 60;         // spot, degrees
    uint64_t id = 0;                  // "ID": what a lights.xml <Mod LightID="..."> refers to
    float width = 0;                  // plane: Width, Length (the box's full x / y extents), FalloffPercentVec (x y z)
    float length = 0;
    Vec3 falloffPercentVec;
    Vec3 direction() const;           // spot: the direction it shines
};

// Light modifiers (lights.xml <Mods>; tabula_rasa.exe, decompiled 2026-10-12). A <Mod LightID> holds tracks; each track is
// one modifier and (time, weight) keys. Per frame (FUN_00852c30 -> FUN_00853c80) the elapsed time (speed 1.0,
// FUN_007cb070) is wrapped by fmod with the track's last key time (the list's loop flag is set, FUN_007caf20,
// FUN_00853d50), the weight is interpolated linearly between the surrounding keys (FUN_00848c40 / FUN_00849d80), and the
// modifier sets a light property, replacing it (slot 28 of each modifier class):
//   Intensity = Intensity1 + w (Intensity2 - Intensity1); Color = lerp(Color1, Color2, w) (endpoint order by analogy);
//   LocalOrientation = rotation about RotationAxis by clamp(w 2 pi, 0, 2 pi); Offset = lerp(Offset1, Offset2, w)
//   (UNVERIFIED endpoint order). LightID 0 is skipped (FUN_007df550). Range / cone / penumbra / back-colour modifiers
//   exist in the exe but are not read here.
struct LightModifierTrack {
    enum class Kind : uint8_t { Intensity, Color, Rotation, Position, Other };
    Kind kind = Kind::Other;
    bool enabled = true;
    float intensity1 = 0, intensity2 = 0;
    Vec3 colour1, colour2;
    Vec3 axis{0, 1, 0};
    Vec3 offset1, offset2;
    std::vector<std::pair<float, float>> keys;    // (time in seconds, weight), in file order
};
struct LightModifier {
    uint64_t lightId = 0;
    std::vector<LightModifierTrack> tracks;
};
std::vector<LightModifier> readLightModifiers(const Bytes& data);

// A map's lights.xml edited as text, so everything the editor does not model (property order, formatting, unknown properties,
// the <Mods> keys) stays as it was. Light n is the n-th <Light> element of <Lights> - the same order readLights returns them in.
// Changing a property rewrites only that property's value="..."; duplicating copies the <Light> block (and the light's <Mod>
// blocks, by ID) to the end of its section; removing deletes the block and its <Mod> blocks. Numbers are written like the
// shipped files ("%.7f", vectors ", "-separated).
class LightsDocument {
public:
    explicit LightsDocument(const Bytes& xml);
    size_t count() const { return lights_.size(); }
    std::string property(size_t light, const std::string& name) const;              // "" if absent
    // Replaces the property's value, or adds <property name value type> before the light's </PropertyObject>. false if the light
    // has no PropertyObject.
    bool setProperty(size_t light, const std::string& name, const std::string& value, const std::string& type = "string");
    size_t duplicate(size_t light, uint64_t newId, const std::string& newName);       // returns the copy's index
    void remove(size_t light);
    Bytes bytes() const { return Bytes(text_.begin(), text_.end()); }
    static std::string formatNumber(float v);
    static std::string formatVector(const float* v, size_t n);

private:
    struct Span {
        size_t begin = 0, end = 0;                   // whole lines: indentation to the newline after </Light>
    };
    void scan();
    std::vector<Span> modsOf(uint64_t id) const;     // <Mod LightID="id"> blocks (none for id 0)
    std::string text_;
    std::vector<Span> lights_;
};
float lightModifierWeight(const LightModifierTrack& track, float elapsedSeconds);   // looping, as above

// overrides: property name -> value, applied over every light's own properties (as the engine sets a contact point's
// "beginlight" ... "endlight" pairs by property name on an attached template, FUN_00816bf0 / FUN_00884cf0).
std::vector<PlacedLight> readLights(const Bytes& data, const std::map<std::string, std::string>& overrides = {});

// Day-cycle sampling: keys wrap around (time 1 = time 0), linear interpolation.
Vec3 skyLightColour(const SkyLight& light, uint32_t keyType, float time);   // colour * DIntensity; 0 if no such keys
// A directional sky light's ShadowOpacity at a time: the type-3 keys' ShadowOpacity, interpolated linearly with wrap-around
// (TRasa::SkyLight: FUN_00840ba0 puts type-3 keys in a separate track, FUN_00841470 samples it, FUN_00840990 sets the
// light's ShadowOpacity, which the shadowed TRDirLight pass applies as visibility = 1 - opacity x occlusion). 1 without keys.
float skyLightShadowOpacity(const SkyLight& light, float time);
SkyFogKey sampleSkyFog(const Sky& sky, float time);                           // all zero if the sky has no fog frames
float sampleLayerAlpha(const SkyLayer& layer, float time);                    // 1 if no alpha keys
// A layer-space direction rotated into the world at time: initial yaw/pitch/roll, then RotsPerDay turns about the
// rotation axis per day (order UNVERIFIED; the sun's light points straight down at time 0.5 with these rules).
Vec3 skyLayerDirection(const SkyLayer& layer, const Vec3& direction, float time);
// The keyed colour at a time of day (cyclic linear interpolation like the engine's FUN_00885af0; no keys: (0, 0, 0) alpha 1).
SkyColourKey sampleLayerColour(const SkyLayer& layer, float time);
Vec3 sampleLayerOffset(const SkyLayer& layer, float time);     // no keys: 0

}  // namespace tr
