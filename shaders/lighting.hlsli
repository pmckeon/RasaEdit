// lighting.hlsli - sky lighting and fog shared by scene.hlsl and terrain.hlsl.
//
// From the map region's .sky at the chosen time of day (see SceneView::skyLighting): a hemispheric light (sky colour
// above, ground colour below, blended by the normal's y) plus one directional light (the sun or moon layer) with a
// front colour towards the light and a back colour on the far side; then linear distance fog in the sky colour.
// With sky lighting off the editor passes a plain grey hemisphere and a white sun.

struct SkyLighting
{
    float3 towardsLight;
    float3 sunFront;
    float3 sunBack;
    float3 hemiTop;
    float3 hemiBottom;
};

float3 skyLight(float3 n, SkyLighting s)
{
    const float ndl = dot(n, s.towardsLight);
    const float3 hemi = lerp(s.hemiBottom, s.hemiTop, n.y * 0.5 + 0.5);
    return hemi + s.sunFront * saturate(ndl) + s.sunBack * saturate(-ndl);
}

// Placed lights (the map's lights.xml), up to 16 per frame, nearest the camera first. Formulas from the game's
// deferred passes (effects.glm trpointlight / trspotlight, verified 2026-10-10):
//   point: colour * intensity * saturate(N.L) * (1 - clamp((d - start) / (end - start), 0, 1)),
//          start = min(LightFalloff, 0.970225 range), end = 0.985 range
//   spot:  the same fade measured along the spot's axis (its local +z) instead of the distance, times a cone factor:
//          1 inside cos(cone), linear to 0 at cos(penumbra)
// position: xyz, w range; colour: rgb * intensity, w fade start distance; spot: xyz axis, w cos(penumbra) or -2 for a
// point light; spotInner: x cos(cone).
#define MAX_LOCAL_LIGHTS 16

float3 localLights(float3 world, float3 n, int count, float4 position[MAX_LOCAL_LIGHTS], float4 colour[MAX_LOCAL_LIGHTS],
                   float4 spot[MAX_LOCAL_LIGHTS], float4 spotInner[MAX_LOCAL_LIGHTS])
{
    float3 sum = float3(0, 0, 0);
    for (int i = 0; i < count; ++i)
    {
        const float3 toLight = position[i].xyz - world;
        const float d = length(toLight);
        const float range = position[i].w;
        if (d >= range)
            continue;
        const float3 l = toLight / max(d, 0.0001);
        const bool isSpot = spot[i].w > -1.5;
        const float along = isSpot ? dot(-toLight, spot[i].xyz) : d;       // spot: depth along its axis
        if (isSpot && along <= 0.0)
            continue;
        const float start = min(colour[i].w, 0.970225 * range);
        const float end = 0.985 * range;
        const float fade = 1.0 - saturate((along - start) / max(end - start, 0.000001));
        float cone = 1.0;
        if (isSpot)
        {
            const float c = along / max(d, 0.0001);                         // cosine of the angle from the axis
            const float cosCone = spotInner[i].x, cosPenumbra = spot[i].w;
            cone = c >= cosCone ? 1.0 : (c >= cosPenumbra ? 1.0 - (cosCone - c) / max(cosCone - cosPenumbra, 0.000001) : 0.0);
        }
        sum += colour[i].rgb * saturate(dot(n, l)) * fade * cone;
    }
    return sum;
}

// The game's fog (trhdrcomposite): visibility = exp(-(density * max(d - start, 0))^2), lerp to the fog colour.
// fogColour.w: 1 = on; fogRange: x start distance, y density.
float3 applyFog(float3 rgb, float distance, float4 fogColour, float4 fogRange)
{
    if (fogColour.w < 0.5)
        return rgb;
    const float x = max(distance - fogRange.x, 0.0) * fogRange.y;
    return lerp(fogColour.rgb, rgb, exp(-x * x));
}
