// terrain.hlsl - blended terrain layers for the 3D viewport.
//
// Every terrain sample has four (layer index, coverage) pairs. They arrive as two RGBA8 textures with
// one texel per sample (read with a point sampler); coverage is the client's alpha-mask value
// (tr::slotCoverage). For each pixel the four surrounding samples are summed by coverage - the game
// adds one pass per layer, texture x mask, and leaves what no layer covers black - then interpolated
// bilinearly (the game's masks are filtered textures at one texel per sample). Layers live in one texture array
// and are projected tri-planar with the game's hard switch by normal (TRTerrain shader:
// |n.x| > 0.5 -> X-facing, else |n.z| > 0.5 -> Z-facing, else top-down), scaled by each layer's
// tiling (taken as metres per repeat). Registers as in scene.hlsl; lighting and fog as in lighting.hlsli.

#include "lighting.hlsli"

cbuffer VertexUniforms : register(b0, space1)
{
    row_major float4x4 viewProjection;
    row_major float4x4 model;
};

cbuffer FragmentUniforms : register(b0, space3)
{
    float4 grid;              // x: world x of sample column 0, y: world z of sample row 0, z: 1 / spacing
    float4 gridSize;          // x: columns, y: rows, z: 1 / columns, w: 1 / rows
    float4 lightDirection;    // xyz: towards the light
    float4 sunFront;
    float4 sunBack;
    float4 hemiTop;
    float4 hemiBottom;
    float4 fogColour;         // rgb; w: 1 = fog on
    float4 fogRange;          // x: start distance, y: density, z: 1 = light-buffer debug view
    float4 cameraPosition;    // xyz
    float4 localCount;        // x: number of placed lights
    float4 localPosition[MAX_LOCAL_LIGHTS];
    float4 localColour[MAX_LOCAL_LIGHTS];
    float4 localSpot[MAX_LOCAL_LIGHTS];
    float4 localSpotInner[MAX_LOCAL_LIGHTS];
    float4 localRotation[MAX_LOCAL_LIGHTS];
    row_major float4x4 shadowMatrix;   // world -> sun shadow map clip space
    float4 shadowParams;              // x: 1 = sun shadows on, y: opacity, z: 1 / map size, w: g_ShadowSoftness
    float4 shadowExtra;               // xyz: camera forward (view depth for the bias), w: shadow-map depth units per metre
    float4 extraTowards[MAX_EXTRA_DIRECTIONAL];   // further directional sky lights: towards the light, w = 1 in use
    float4 extraFront[MAX_EXTRA_DIRECTIONAL];
    float4 extraBack[MAX_EXTRA_DIRECTIONAL];
    float4 localShadowBase[MAX_LOCAL_LIGHTS];   // x first shadow view + 1, y view count, z shadow-light index
    float4 tilings[32];       // metres per repeat for layers 0..127 (four per entry)
};

[[vk::combinedImageSampler]] Texture2DArray layerTextures : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState layerSampler : register(s0, space2);
[[vk::combinedImageSampler]] Texture2D layerIndices : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState indexSampler : register(s1, space2);
[[vk::combinedImageSampler]] Texture2D layerWeights : register(t2, space2);
[[vk::combinedImageSampler]] SamplerState weightSampler : register(s2, space2);
[[vk::combinedImageSampler]] Texture2D<float> shadowMap : register(t3, space2);
[[vk::combinedImageSampler]] TextureCubeArray globeMaps : register(t4, space2);
[[vk::combinedImageSampler]] SamplerState globeSampler : register(s4, space2);
[[vk::combinedImageSampler]] SamplerComparisonState shadowSampler : register(s3, space2);
[[vk::combinedImageSampler]] Texture2D<float> shadowAtlas : register(t5, space2);      // placed-light shadow maps (point, clamp)
[[vk::combinedImageSampler]] SamplerState shadowAtlasSampler : register(s5, space2);
[[vk::combinedImageSampler]] Texture2DArray cookieMaps : register(t6, space2);         // spot lights' 2D globe maps
[[vk::combinedImageSampler]] SamplerState cookieSampler : register(s6, space2);
StructuredBuffer<LocalShadowSlot> localShadowSlots : register(t7, space2);    // placed-light shadow slots (after the 7 samplers)

struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 colour   : TEXCOORD2;
    float2 uv       : TEXCOORD3;
    float2 uv1      : TEXCOORD4;      // unused (shared vertex layout)
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 world    : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float  solid    : TEXCOORD2;      // the sample's flag 0 (1 = terrain, 0 = hole), interpolated
};

VertexOutput vsMain(VertexInput input)
{
    VertexOutput output;
    const float4 world = mul(model, float4(input.position, 1.0));
    output.position = mul(viewProjection, world);
    output.world = world.xyz;
    output.normal = mul((float3x3)model, input.normal);
    output.solid = input.colour.a;
    return output;
}

float layerTiling(uint layer)
{
    const float4 entry = tilings[layer >> 2];
    const uint lane = layer & 3;
    const float t = lane == 0 ? entry.x : lane == 1 ? entry.y : lane == 2 ? entry.z : entry.w;
    return max(t, 0.01);
}

float4 psMain(VertexOutput input) : SV_Target0
{
    // Terrain holes: TRTerrainClient's SetZBufferAndFog pass texkills where the base mask (flag 0) is under 0.5.
    if (input.solid < 0.5)
        discard;
    const float3 n = normalize(input.normal);
    const float3 an = abs(n);
    // Tri-planar projection chosen once per pixel; gradients taken outside the loops.
    // The game's layer coordinates are position x gLayMat = scale(1/t, 1/t, -1/t) (FUN_007423f0; TRTerrainClient vertex
    // shader: oT3 = v0 x gLayMat), i.e. z mirrored, sampled as .xz (top), .zy (X-facing), .xy (Z-facing).
    const float3 layerPosition = float3(input.world.x, input.world.y, -input.world.z);
    const float2 projected = an.x > 0.5 ? layerPosition.zy : (an.z > 0.5 ? layerPosition.xy : layerPosition.xz);
    const float2 dx = ddx(projected), dy = ddy(projected);

    const float2 g = float2((input.world.x - grid.x) * grid.z, (input.world.z - grid.y) * grid.z);
    const float2 cell = floor(g);
    const float2 f = g - cell;
    float3 colour = float3(0, 0, 0);
    [unroll] for (uint corner = 0; corner < 4; ++corner)
    {
        const float2 offset = float2(corner & 1, corner >> 1);
        const float2 sampleUv = (clamp(cell + offset, float2(0, 0), gridSize.xy - 1) + 0.5) * gridSize.zw;
        // round(), not + 0.5 and a uint cast: DXC folded float(uint(x + 0.5)) to x + 0.5, and the array slice selection
        // rounds that (index 13 drew slice 14, measured on Vulkan and D3D12).
        const float4 indices = round(layerIndices.SampleLevel(indexSampler, sampleUv, 0) * 255.0);
        const float4 weights = layerWeights.SampleLevel(weightSampler, sampleUv, 0);
        const float bilinear = (offset.x > 0.5 ? f.x : 1.0 - f.x) * (offset.y > 0.5 ? f.y : 1.0 - f.y);
        [unroll] for (uint slot = 0; slot < 4; ++slot)
        {
            const uint layer = (uint)indices[slot];
            const float scale = 1.0 / layerTiling(layer);
            const float3 texel = layerTextures.SampleGrad(layerSampler, float3(projected * scale, layer), dx * scale, dy * scale).rgb;
            colour += texel * (weights[slot] * bilinear);
        }
    }
    SkyLighting s;
    s.towardsLight = normalize(lightDirection.xyz);
    s.sunFront = sunFront.rgb;
    s.sunBack = sunBack.rgb;
    s.hemiTop = hemiTop.rgb;
    s.hemiBottom = hemiBottom.rgb;
    s.sunVisibility = sunShadow(shadowMap, shadowSampler, shadowMatrix, shadowParams, shadowExtra, cameraPosition.xyz, input.world, n);
    const float3 light = skyLight(n, s) + extraSkyLight(n, extraTowards, extraFront, extraBack, false) + localLights(input.world, n, (int)localCount.x, localPosition, localColour, localSpot, localSpotInner, localRotation, globeMaps, globeSampler, cookieMaps, cookieSampler, shadowAtlas, localShadowSlots, localShadowBase, 0.0);
    if (fogRange.w > 1.5)
        return float4(fogRange.w < 2.5 ? n * 0.5 + 0.5 : (fogRange.w > 3.5 && fogRange.w < 4.5) ? saturate(colour) : float3(0, 0, 0), 1.0);   // normals / spec / albedo views (terrain has no spec: 0; spec power view 5 too)
    if (fogRange.z > 0.5)
        return float4(light * 0.2, 1.0);       // the game's light buffer (x OO_LIGHT_MULTIPLIER), as /dl_showlight shows it
    const float3 lit = colour * light;
    return float4(applyFog(lit, length(cameraPosition.xyz - input.world), fogColour, fogRange), 1.0);
}
