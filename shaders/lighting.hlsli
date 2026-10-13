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
    float sunVisibility;              // 1 = fully sunlit (sun shadows)
};

#define MAX_EXTRA_DIRECTIONAL 3

// Further directional sky lights (TRDirLight each: sat(N.L) x (LightColor - LightBackColor), no shadow map). forwardPiece:
// the forward gDirLight array has Color only.
float3 extraSkyLight(float3 n, float4 towards[MAX_EXTRA_DIRECTIONAL], float4 front[MAX_EXTRA_DIRECTIONAL],
                     float4 back[MAX_EXTRA_DIRECTIONAL], bool forwardPiece)
{
    float3 sum = float3(0, 0, 0);
    [unroll] for (int k = 0; k < MAX_EXTRA_DIRECTIONAL; ++k)
        if (towards[k].w > 0.5)
            sum += saturate(dot(n, towards[k].xyz)) * (front[k].rgb - (forwardPiece ? float3(0, 0, 0) : back[k].rgb));
    return sum;
}

float3 skyLight(float3 n, SkyLighting s)
{
    const float ndl = dot(n, s.towardsLight);
    const float3 hemi = lerp(s.hemiBottom, s.hemiTop, n.y * 0.5 + 0.5);
    // TRDirLight (deferred, with and without shadows): sat(N.L) x (LightColor x visibility - LightBackColor) - the back colour
    // is subtracted on the lit side. The forward gDirLight has no back colour (callers pass sunBack = 0 for forward pieces).
    return hemi + saturate(ndl) * (s.sunFront * s.sunVisibility - s.sunBack);
}

// Placed lights (the map's lights.xml), up to 32 per frame, nearest the camera first. Formulas from the game's
// deferred passes (effects.glm trpointlight / trspotlight, verified 2026-10-10):
//   point: colour * intensity * saturate(N.L) * (1 - clamp((d - start) / (end - start), 0, 1)),
//          start = min(LightFalloff, 0.970225 range), end = 0.985 range
//   spot:  the same fade measured along the spot's axis (its local +z) instead of the distance, times a cone factor:
//          1 inside cos(cone), linear to 0 at cos(penumbra)
// position: xyz, w range; colour: rgb * intensity, w fade start distance; spot: xyz axis, w cos(penumbra) or -2 for a
// point light; spotInner: x cos(cone).
#define MAX_LOCAL_LIGHTS 32

// Sun shadow: 3x3 filtered comparisons in the sun's depth map, offset along the normal against acne; outside the map
// (beyond the area around the camera) everything is lit. params: x on, y opacity (the game lets part of the sun through),
// z 1 / map size.
// The game's TRDirLight shadow test (ps3dir[1], effects.glm, disassembled 2026-10-14), orthographic sun map:
//   reference = shadow-view z - clamp(0.02 x camera view depth, 0.065, 3) (metres, no normal offset); an empty texel counts
//   as far; five manual 2 x 2 bilinear PCF taps - the centre and (+-s, +-s), s = 1/2 texel x g_ShadowSoftness - weighted 0.2;
//   within 15 % of the map's edge the result fades to lit (x 6.66666); outside the map or its depth range: lit;
//   visibility = 1 - g_ShadowOpacity x (1 - lit). The comparison sampler is LINEAR, so each SampleCmp is that 2 x 2 tap.
float sunShadow(Texture2D<float> map, SamplerComparisonState cmp, float4x4 m, float4 params, float4 extra, float3 eye, float3 world, float3 n)
{
    if (params.x < 0.5)
        return 1.0;
    const float4 p = mul(m, float4(world, 1.0));
    const float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z < 0.0 || p.z > 1.0)
        return 1.0;
    const float viewDepth = dot(world - eye, extra.xyz);
    const float reference = max(p.z - clamp(0.02 * viewDepth, 0.065, 3.0) * extra.w, 0.0);
    const float s = 0.5 * params.z * params.w;
    float lit = map.SampleCmpLevelZero(cmp, uv + float2(-s, s), reference) + map.SampleCmpLevelZero(cmp, uv + float2(s, s), reference) +
                map.SampleCmpLevelZero(cmp, uv + float2(s, -s), reference) + map.SampleCmpLevelZero(cmp, uv + float2(-s, -s), reference) +
                map.SampleCmpLevelZero(cmp, uv, reference);
    lit *= 0.2;
    const float edge = min(min(uv.x, uv.y), 1.0 - max(uv.x, uv.y));
    if (edge < 0.15)
        lit = saturate(lerp(1.0, lit, edge * 6.66665983));
    return 1.0 - params.y * (1.0 - lit);
}

// q^-1 v for a unit quaternion q (x y z w).
float3 rotateInverse(float4 q, float3 v)
{
    const float3 u = -q.xyz;
    return 2.0 * dot(u, v) * u + (q.w * q.w - dot(u, u)) * v + 2.0 * q.w * cross(u, v);
}

// q v for a unit quaternion q (x y z w).
float3 rotateBy(float4 q, float3 v)
{
    const float3 u = q.xyz;
    return 2.0 * dot(u, v) * u + (q.w * q.w - dot(u, u)) * v + 2.0 * q.w * cross(u, v);
}

// Plane light (TRPlaneLight pixel shader, effects.glm, disassembled 2026-10-12): p = the pixel in the light's frame; outside
// the box |p| < dims nothing; per-axis fade 1 - (clamp(|p|, falloff, dims) - falloff) / (dims - falloff), the smallest of
// the three; direction fixed: sat(N . -(light's local z axis)). The caller multiplies by colour x intensity. BackColor is
// not applied (0 in the shipped lights checked); the shadowed variant is not reproduced.
float planeLight(float3 world, float3 n, float4 position, float4 dims, float4 falloff, float4 q)
{
    const float3 p = abs(rotateInverse(q, world - position.xyz));
    if (any(p > dims.xyz))
        return 0.0;
    const float3 f = float3(falloff.x, falloff.z, falloff.w);
    const float3 fade = 1.0 - (clamp(p, f, dims.xyz) - f) / max(dims.xyz - f, 0.0001);
    const float3 axis = rotateBy(q, float3(0, 0, 1));
    return min(fade.x, min(fade.y, fade.z)) * saturate(dot(n, -axis));
}

// Globe maps (GlobeMapFile): the game's trpointlight globe variant samples a cube map with the pixel's position in the
// light's own frame (Proj2LightView) and multiplies the light colour by it (disassembled from effects.glm).
float3 localLights(float3 world, float3 n, int count, float4 position[MAX_LOCAL_LIGHTS], float4 colour[MAX_LOCAL_LIGHTS],
                   float4 spot[MAX_LOCAL_LIGHTS], float4 spotInner[MAX_LOCAL_LIGHTS], float4 rotation[MAX_LOCAL_LIGHTS],
                   TextureCubeArray globes, SamplerState globeSampler)
{
    float3 sum = float3(0, 0, 0);
    for (int i = 0; i < count; ++i)
    {
        const float3 toLight = position[i].xyz - world;
        const float d = length(toLight);
        const float range = position[i].w;
        if (d >= range)
            continue;
        if (spot[i].w < -2.5)
        {
            sum += colour[i].rgb * planeLight(world, n, position[i], spot[i], spotInner[i], rotation[i]);
            continue;
        }
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
        float3 globe = float3(1, 1, 1);
        if (spotInner[i].y > 0.5)
            globe = globes.SampleLevel(globeSampler, float4(rotateInverse(rotation[i], -toLight), spotInner[i].y), 0).rgb;
        sum += colour[i].rgb * globe * saturate(dot(n, l)) * fade * cone;
    }
    return sum;
}

// Specular, from the game's TRDirLight / TRPointLight / TRSpotLight "WithSpec" passes (effects.glm, disassembled
// 2026-10-11). sp = specular power, i = specular intensity from the G-buffer (Pal* materials: gloss map r and g):
//   a = sat(20 N.L) i;  scale = min(a (0.865019 sp^2 + 0.35) + a sat(1 - N.V)^2, 1.25);
//   exponent = 8 + 245 sp^2 (1 - sat(1 - N.V));  spec = scale (N.H)^exponent, only where N.L > 0, N.H > 0 and sp > 0.
// The pass multiplies it by the light colour (and a point/spot light's fade x intensity, the sun's shadow factor) and
// writes it to the specular buffer, which trhdrcomposite adds untinted.
float specularFactor(float3 n, float3 v, float3 l, float sp, float i)
{
    const float ndl = dot(n, l);
    const float sp2 = sp * sp;
    if (ndl <= 0.0 || sp2 <= 0.0)
        return 0.0;
    const float a = saturate(20.0 * ndl) * i;
    const float edge = saturate(1.0 - dot(n, v));
    const float scale = min(a * (0.865018964 * sp2 + 0.35) + a * edge * edge, 1.25);
    const float exponent = 8.0 + 245.0 * sp2 * (1.0 - edge);
    const float ndh = dot(n, normalize(v + l));
    return ndh > 0.0 ? scale * pow(ndh, exponent) : 0.0;
}

// The hemi light's specular (TRHemiLight pass, disassembled from an apitrace capture 2026-10-14; Use_Specular was 1): a sky
// reflection lobe of TopColor, written to the specular buffer:
//   scale = min(i (0.865019 sp^2 + 0.35) + i sat(1 - N.V)^2, 1.25);  e = 8 + 245 sp^2 (1 - sat(1 - N.V));
//   spec = TopColor scale ((R.y / |R| + 1) / 2)^(0.2 e + 5), R = reflect(-V, N); nothing where sp = 0.
float3 hemiSpecular(float3 n, float3 v, float sp, float i, float3 top)
{
    const float sp2 = sp * sp;
    if (sp2 <= 0.0)
        return float3(0, 0, 0);
    const float edge = saturate(1.0 - dot(n, v));
    const float scale = min(i * (0.865018964 * sp2 + 0.35) + i * edge * edge, 1.25);
    const float exponent = 8.0 + 245.0 * sp2 * (1.0 - edge);
    const float3 r = reflect(-v, n);
    return top * scale * pow((r.y / max(length(r), 1e-6) + 1.0) * 0.5, exponent * 0.2 + 5.0);
}

// Placed lights' specular: the same fade and cone as localLights (globe maps not applied to the specular - UNVERIFIED).
float3 localSpecular(float3 world, float3 n, float3 v, float sp, float i, int count, float4 position[MAX_LOCAL_LIGHTS],
                     float4 colour[MAX_LOCAL_LIGHTS], float4 spot[MAX_LOCAL_LIGHTS], float4 spotInner[MAX_LOCAL_LIGHTS])
{
    float3 sum = float3(0, 0, 0);
    for (int k = 0; k < count; ++k)
    {
        const float3 toLight = position[k].xyz - world;
        const float d = length(toLight);
        const float range = position[k].w;
        if (d >= range)
            continue;
        if (spot[k].w < -2.5)
            continue;                           // plane lights: specular not decoded
        const bool isSpot = spot[k].w > -1.5;
        const float along = isSpot ? dot(-toLight, spot[k].xyz) : d;
        if (isSpot && along <= 0.0)
            continue;
        const float start = min(colour[k].w, 0.970225 * range);
        const float end = 0.985 * range;
        const float fade = 1.0 - saturate((along - start) / max(end - start, 0.000001));
        float cone = 1.0;
        if (isSpot)
        {
            const float c = along / max(d, 0.0001);
            const float cosCone = spotInner[k].x, cosPenumbra = spot[k].w;
            cone = c >= cosCone ? 1.0 : (c >= cosPenumbra ? 1.0 - (cosCone - c) / max(cosCone - cosPenumbra, 0.000001) : 0.0);
        }
        sum += colour[k].rgb * specularFactor(n, v, toLight / max(d, 0.0001), sp, i) * fade * cone;
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
