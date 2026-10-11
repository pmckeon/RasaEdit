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

struct SkyLayer {
    bool enabled = true;
    std::string geoFilename;
    Vec3 rotationAxis{0, 1, 0};
    float rotationsPerDay = 0;
    float initialPitch = 0, initialYaw = 0, initialRoll = 0;     // radians
    bool useFogColour = false;
    std::vector<SkyAlphaKey> alphaKeys;
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
    float width = 0;                  // plane: Width, Length (the box's full x / y extents), FalloffPercentVec (x y z)
    float length = 0;
    Vec3 falloffPercentVec;
    Vec3 direction() const;           // spot: the direction it shines
};

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

}  // namespace tr
