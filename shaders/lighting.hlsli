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
// The slot buffer holds the shadow views (SceneLighting::maxLocalShadows), then one entry per placed light (view.rgb = its back colour x
// intensity - plane lights).
#define MAX_LOCAL_SHADOWS 72

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

// Plane light box fade (TRPlaneLight pixel shader, effects.glm, disassembled 2026-10-12): p = the pixel in the light's frame;
// outside the box |p| < dims nothing; per-axis fade 1 - (clamp(|p|, falloff, dims) - falloff) / (dims - falloff), the smallest
// of the three. The light's direction is fixed (-its local z axis); see localLights / localSpecular for the rest.
float planeFade(float3 world, float4 position, float4 dims, float4 falloff, float4 q)
{
    const float3 p = abs(rotateInverse(q, world - position.xyz));
    if (any(p > dims.xyz))
        return 0.0;
    const float3 f = float3(falloff.x, falloff.z, falloff.w);
    const float3 fade = 1.0 - (clamp(p, f, dims.xyz) - f) / max(dims.xyz - f, 0.0001);
    return min(fade.x, min(fade.y, fade.z));
}

// One placed-light shadow view, read from a fragment storage buffer (Renderer's localShadowBuffer_): a spot light has one, a plane
// light one, a point light six (cube faces +x, -x, +y, -y, +z, -z). Up to 72 views do not fit the 4 KiB a uniform push may hold on
// SDL GPU's Vulkan backend.
struct LocalShadowSlot
{
    row_major float4x4 matrix;   // world -> the view's clip space
    float4 view;                 // xyz the view's axis, w -dot(axis, eye): linear depth = dot(view.xyz, world) + view.w
    float4 rect;                 // x, y first texel in the atlas, z size, w the shadow light's index
    float4 params;               // x near, y far, z opacity, w softness
    float4 bias;                 // x scale, y min, z max of the perspective bias; w 1 = orthographic (the camera-depth bias)
    float4 camera;               // orthographic views: the camera's forward, w -dot(forward, eye)
};

// Placed-light shadows - the game's light-pass shadow variants (effects.glm, disassembled 2026-10-17: TRSpotLight ps3SpotLight[1/3/5/7],
// TRPlaneLight ps3[1/3/5/7], TRPointLight ps3PointLight[1/3/5/7]; the spot constants read from an apitrace capture at the Bootcamp
// firing range): the maps hold the view's linear depth; an empty texel is far.
// Spot and plane (2D maps): reference = depth - clamp(0.05 depth, 0.035, 3) for perspective maps, depth - clamp(0.02 z, 0.065, 3) for
// orthographic ones (z = the camera's view depth, as the sun's); five 2 x 2 bilinear PCF taps - the centre and the four diagonals half
// a texel x g_ShadowSoftness away, 0.2 each - at uv + half a texel; lit outside the map's square and its depth range.
// Point (cube): the lookup vector v = the pixel relative to the light, m = max(|v.x|, |v.y|, |v.z|) = the cube face's depth;
// reference = max(m - clamp(0.05 m, 0.8, 5), 0); eight single-texel taps at v + (-h, -h, -h), (-k, 0, 0), (0, k, 0), (k, 0, 0),
// (-h, h, -h), (h, h, -h), (h, -h, -h), (0, 0, 0) (k = g_ShadowSoftness / size, h = k / 2), each lit when the stored depth >= the
// reference, averaged. All: visibility = 1 - ShadowOpacity x occlusion. The editor's atlas stores the hardware depth of each view's
// projection, turned back into linear depth here.
float shadowTexel(Texture2D<float> atlas, float4 rect, float4 params, float4 bias, int2 t)
{
    const int last = int(rect.z) - 1;
    t = clamp(t, int2(0, 0), int2(last, last));
    const float d = atlas.Load(int3(int2(rect.xy) + t, 0));
    if (bias.w > 0.5)
        return params.x + d * (params.y - params.x);                     // orthographic: depth is linear
    return params.x * params.y / (params.y - d * (params.y - params.x));
}

float shadowTap(Texture2D<float> atlas, float4 rect, float4 params, float4 bias, float2 uv, float reference)
{
    const float2 p = uv * rect.z;
    const int2 i = int2(floor(p));
    const float2 f = frac(p);
    const float a = shadowTexel(atlas, rect, params, bias, i) >= reference ? 1.0 : 0.0;
    const float b = shadowTexel(atlas, rect, params, bias, i + int2(1, 0)) >= reference ? 1.0 : 0.0;
    const float c = shadowTexel(atlas, rect, params, bias, i + int2(0, 1)) >= reference ? 1.0 : 0.0;
    const float e = shadowTexel(atlas, rect, params, bias, i + int2(1, 1)) >= reference ? 1.0 : 0.0;
    return lerp(lerp(a, b, f.x), lerp(c, e, f.x), f.y);
}

float localShadow(float3 world, LocalShadowSlot s, Texture2D<float> atlas)
{
    const float4 clip = mul(s.matrix, float4(world, 1.0));
    if (clip.w <= 0.0)
        return 1.0;
    const float3 ndc = clip.xyz / clip.w;
    const float texel = 1.0 / s.rect.z;
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) + 0.5 * texel;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0)
        return 1.0;
    const float depth = dot(s.view.xyz, world) + s.view.w;
    float reference;
    if (s.bias.w > 0.5)
        reference = depth - clamp(0.02 * (dot(s.camera.xyz, world) + s.camera.w), 0.065, 3.0);
    else
        reference = depth - clamp(s.bias.x * depth, s.bias.y, s.bias.z);
    reference = max(reference, 0.0);
    const float h = 0.5 * texel * s.params.w;
    float lit = shadowTap(atlas, s.rect, s.params, s.bias, uv, reference);
    lit += shadowTap(atlas, s.rect, s.params, s.bias, uv + float2(-h, h), reference);
    lit += shadowTap(atlas, s.rect, s.params, s.bias, uv + float2(h, h), reference);
    lit += shadowTap(atlas, s.rect, s.params, s.bias, uv + float2(h, -h), reference);
    lit += shadowTap(atlas, s.rect, s.params, s.bias, uv + float2(-h, -h), reference);
    return 1.0 - s.params.z * (1.0 - 0.2 * lit);
}

// One cube tap: the face the vector points into (its major axis), its single texel's linear depth >= reference.
float cubeTap(float3 lightPosition, float3 v, int first, float reference, StructuredBuffer<LocalShadowSlot> slots, Texture2D<float> atlas)
{
    const float3 a = abs(v);
    int face = a.x >= a.y && a.x >= a.z ? (v.x >= 0.0 ? 0 : 1) : (a.y >= a.z ? (v.y >= 0.0 ? 2 : 3) : (v.z >= 0.0 ? 4 : 5));
    const LocalShadowSlot s = slots[first + face];
    const float4 clip = mul(s.matrix, float4(lightPosition + v, 1.0));
    if (clip.w <= 0.0)
        return 1.0;
    const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    const int2 t = int2(floor(uv * s.rect.z));
    return shadowTexel(atlas, s.rect, s.params, s.bias, t) >= reference ? 1.0 : 0.0;
}

float cubeShadow(float3 world, float3 lightPosition, int first, StructuredBuffer<LocalShadowSlot> slots, Texture2D<float> atlas)
{
    const float3 v = world - lightPosition;
    const float m = max(abs(v.x), max(abs(v.y), abs(v.z)));
    const LocalShadowSlot s0 = slots[first];
    const float reference = max(m - clamp(s0.bias.x * m, s0.bias.y, s0.bias.z), 0.0);
    const float k = s0.params.w / s0.rect.z;
    const float h = 0.5 * k;
    float lit = cubeTap(lightPosition, v + float3(-h, -h, -h), first, reference, slots, atlas);
    lit += cubeTap(lightPosition, v + float3(-k, 0, 0), first, reference, slots, atlas);
    lit += cubeTap(lightPosition, v + float3(0, k, 0), first, reference, slots, atlas);
    lit += cubeTap(lightPosition, v + float3(k, 0, 0), first, reference, slots, atlas);
    lit += cubeTap(lightPosition, v + float3(-h, h, -h), first, reference, slots, atlas);
    lit += cubeTap(lightPosition, v + float3(h, h, -h), first, reference, slots, atlas);
    lit += cubeTap(lightPosition, v + float3(h, -h, -h), first, reference, slots, atlas);
    lit += cubeTap(lightPosition, v, first, reference, slots, atlas);
    return 1.0 - s0.params.z * (1.0 - 0.125 * lit);
}

// The shadow factor of placed light i (1 without a shadow: shadowBase.x = first view + 1, y = view count - 6 for a point light).
float localShadowOf(float3 world, float3 lightPosition, float4 shadowBase, StructuredBuffer<LocalShadowSlot> slots, Texture2D<float> atlas)
{
    if (shadowBase.x < 0.5)
        return 1.0;
    const int first = int(shadowBase.x + 0.5) - 1;
    if (shadowBase.y > 1.5)
        return cubeShadow(world, lightPosition, first, slots, atlas);
    return localShadow(world, slots[first], atlas);
}

// Globe maps (GlobeMapFile): the game's trpointlight globe variant samples a cube map with the pixel's position in the
// light's own frame (Proj2LightView) and multiplies the light colour by it (disassembled from effects.glm). A spot light's 2D globe
// (spotInner.w = layer + 1; TRSpotLight's globe variants): uv = (x / z, -y / z) x LightProjDistance + 0.5 in the light's frame, z
// along its axis, LightProjDistance = 0.5 / tan(PenumbraAngle / 2). Verified 2026-10-18 in the firing-range trace: ps3SpotLight[3]
// samples Tex_Sampler_Globe at (x / z, -y / z) x LightProjDistance + 0.5 of the pixel in the light's frame (Proj2LightView =
// inverse(Light2World) x the inverse view-projection, to 4e-6), and the flashlights' / lamps' Light2World axes equal the editor's.
// specPower: the pixel's specular power (the G-buffer's), which picks TRPlaneLight's branch (below).
float3 localLights(float3 world, float3 n, int count, float4 position[MAX_LOCAL_LIGHTS], float4 colour[MAX_LOCAL_LIGHTS],
                   float4 spot[MAX_LOCAL_LIGHTS], float4 spotInner[MAX_LOCAL_LIGHTS], float4 rotation[MAX_LOCAL_LIGHTS],
                   TextureCubeArray globes, SamplerState globeSampler, Texture2DArray cookies, SamplerState cookieSampler,
                   Texture2D<float> atlas, StructuredBuffer<LocalShadowSlot> slots, float4 shadowBase[MAX_LOCAL_LIGHTS], float specPower)
{
    float3 sum = float3(0, 0, 0);
    for (int i = 0; i < count; ++i)
    {
        const float3 toLight = position[i].xyz - world;
        const float d = length(toLight);
        const float range = position[i].w;
        // Spot lights are not cut at the range sphere: TRSpotLight fades along the axis only (its light volume is 1.4 x the range -
        // scale 14 for range 10 in the firing-range trace), so the fade ends at the range along the axis (see below).
        if (d >= range && !(spot[i].w > -1.5))
            continue;
        if (spot[i].w < -2.5)
        {
            // TRPlaneLight DeferredPlaneLightWithSpec ps3[0..7] (disassembled 2026-10-18): f = box fade x LightIntensity (nothing
            // where f <= 0.001), L = -(local z axis), vis = the shadow (ps3[1]: the orthographic branch of the spot test; applied
            // where N.L > 0). Pixels without specular power: f sat(N.L) (vis LightColor - LightBackColor); with it:
            // f (vis sat(N.L) LightColor + sat(-N.L) LightBackColor). colour = LightColor x intensity, back colour x intensity
            // (the slot buffer's entry after the shadow views). The pass writes an unsigned target, so it adds at least 0.
            const float f = planeFade(world, position[i], spot[i], spotInner[i], rotation[i]);
            if (f <= 0.001)
                continue;
            const float nl = dot(n, -rotateBy(rotation[i], float3(0, 0, 1)));
            const float vis = nl > 0.0 ? localShadowOf(world, position[i].xyz, shadowBase[i], slots, atlas) : 1.0;
            const float3 back = slots[MAX_LOCAL_SHADOWS + i].view.rgb;
            sum += max(specPower > 0.0 ? f * (vis * saturate(nl) * colour[i].rgb + saturate(-nl) * back)
                                       : f * saturate(nl) * (vis * colour[i].rgb - back), 0.0);
            continue;
        }
        const float3 l = toLight / max(d, 0.0001);
        const bool isSpot = spot[i].w > -1.5;
        const float along = isSpot ? dot(-toLight, spot[i].xyz) : d;       // spot: depth along its axis
        if (isSpot && along <= 0.0)
            continue;
        // Point lights (trpointlight): from min(LightFalloff, 0.970225 R) to 0.985 R. Spot lights (TRSpotLight ps3SpotLight[n], traced
        // 2026-10-17 with the firing-range lamps: LightFalloff 7.5, LightRange 10): 1 - (z - LightFalloff) / (LightRange - LightFalloff), z
        // the depth along the axis - from LightFalloff to the full range.
        const float start = isSpot ? colour[i].w : min(colour[i].w, 0.970225 * range);
        const float end = isSpot ? range : 0.985 * range;
        if (isSpot && along >= range)
            continue;
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
        else if (isSpot && spotInner[i].w > 0.5)
        {
            const float3 lv = rotateInverse(rotation[i], -toLight);
            const float c = spot[i].w;
            const float projDistance = 0.5 * c / max(sqrt(saturate(1.0 - c * c)), 0.0001);
            const float2 uv = float2(lv.x / lv.z, -lv.y / lv.z) * projDistance + 0.5;
            globe = cookies.SampleLevel(cookieSampler, float3(uv, spotInner[i].w), 0).rgb;
        }
        const float shadow = localShadowOf(world, position[i].xyz, shadowBase[i], slots, atlas);
        sum += colour[i].rgb * globe * saturate(dot(n, l)) * fade * cone * shadow;
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
                     float4 colour[MAX_LOCAL_LIGHTS], float4 spot[MAX_LOCAL_LIGHTS], float4 spotInner[MAX_LOCAL_LIGHTS],
                     float4 rotation[MAX_LOCAL_LIGHTS], Texture2D<float> atlas, StructuredBuffer<LocalShadowSlot> slots,
                     float4 shadowBase[MAX_LOCAL_LIGHTS])
{
    float3 sum = float3(0, 0, 0);
    for (int k = 0; k < count; ++k)
    {
        const float3 toLight = position[k].xyz - world;
        const float d = length(toLight);
        const float range = position[k].w;
        // Spot lights are not cut at the range sphere: TRSpotLight fades along the axis only (its light volume is 1.4 x the range -
        // scale 14 for range 10 in the firing-range trace), so the fade ends at the range along the axis (see below).
        if (d >= range && !(spot[k].w > -1.5))
            continue;
        if (spot[k].w < -2.5)
        {
            // TRPlaneLight (ps3[4..7]): f vis LightColor x the common specular factor, L = -(local z axis).
            const float f = planeFade(world, position[k], spot[k], spotInner[k], rotation[k]);
            if (f <= 0.001)
                continue;
            const float3 l = -rotateBy(rotation[k], float3(0, 0, 1));
            const float vis = dot(n, l) > 0.0 ? localShadowOf(world, position[k].xyz, shadowBase[k], slots, atlas) : 1.0;
            sum += colour[k].rgb * f * vis * specularFactor(n, v, l, sp, i);
            continue;
        }
        const bool isSpot = spot[k].w > -1.5;
        const float along = isSpot ? dot(-toLight, spot[k].xyz) : d;
        if (isSpot && along <= 0.0)
            continue;
        // Point lights (trpointlight): from min(LightFalloff, 0.970225 R) to 0.985 R. Spot lights (TRSpotLight ps3SpotLight[n], traced
        // 2026-10-17 with the firing-range lamps: LightFalloff 7.5, LightRange 10): 1 - (z - LightFalloff) / (LightRange - LightFalloff), z
        // the depth along the axis - from LightFalloff to the full range.
        const float start = isSpot ? colour[k].w : min(colour[k].w, 0.970225 * range);
        const float end = isSpot ? range : 0.985 * range;
        if (isSpot && along >= range)
            continue;
        const float fade = 1.0 - saturate((along - start) / max(end - start, 0.000001));
        float cone = 1.0;
        if (isSpot)
        {
            const float c = along / max(d, 0.0001);
            const float cosCone = spotInner[k].x, cosPenumbra = spot[k].w;
            cone = c >= cosCone ? 1.0 : (c >= cosPenumbra ? 1.0 - (cosCone - c) / max(cosCone - cosPenumbra, 0.000001) : 0.0);
        }
        const float shadow = localShadowOf(world, position[k].xyz, shadowBase[k], slots, atlas);
        sum += colour[k].rgb * specularFactor(n, v, toLight / max(d, 0.0001), sp, i) * fade * cone * shadow;
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
