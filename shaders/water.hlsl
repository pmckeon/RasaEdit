// water.hlsl - the game's TRWaterReflectRefract water (effects.glm trwaterreflectrefractsp_2.fxc; the pixel and vertex shaders
// as bound at the Bootcamp basin water's draw in an apitrace capture of the game, 2026-10-14, disassembled from the replayed
// device state). Opaque (no blending, no depth writes, no culling), drawn after the opaque scene over a copy of the frame, before
// the translucent and additive pieces. The scene depth is sampled (point, clamp) for the water's thickness at the pixel and at
// the refracted point; the depth test against the scene is done here (the pass has no depth target). Registers follow
// scene.hlsl's notes (SDL_gpu spaces).

cbuffer WaterVertexUniforms : register(b0, space1)
{
    row_major float4x4 viewProjection;
    row_major float4x4 model;
    row_major float4x4 normalMatrix;      // 3 x 3: model-space tangent frame -> world (inverse transpose of the model's 3 x 3)
    float4 eyePosition;
    float4 fog;                           // x: gFogStartDist, y: gFogDensity, z: 1 = fog on
    float4 transform[8];                  // TexCoordTransform0..3: u row, v row each (MATRIX_COLUMNS upload, as the TR shaders)
};

struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 colour   : TEXCOORD2;
    float2 uv       : TEXCOORD3;
    float2 uv1      : TEXCOORD4;
    float3 tangent  : TEXCOORD5;
    float3 binormal : TEXCOORD6;
    float2 uv2      : TEXCOORD7;
    float2 uv3      : TEXCOORD8;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 uv03     : TEXCOORD0;          // xy: UV0 x TexCoordTransform0 (diffuse), zw: UV3 x TexCoordTransform3 (volume)
    float4 uv12     : TEXCOORD1;          // xy: UV1 x TexCoordTransform1 (normal map), zw: UV2 x TexCoordTransform2 (normal map 1)
    float3 world    : TEXCOORD2;
    float  fog      : TEXCOORD3;          // visibility exp(-(density x max(d - start, 0))^2)
    float3 tangent  : TEXCOORD4;          // world space, not normalised (the pixel shader normalises the sum)
    float3 binormal : TEXCOORD5;
    float3 normal   : TEXCOORD6;
    float3 view     : TEXCOORD7;          // normalize(eye - vertex) PER VERTEX, interpolated and not renormalised (the game's o6)
};

float2 transformUv(float2 uv, int slot)
{
    const float3 p = float3(uv, 1.0);
    return float2(dot(p, transform[slot * 2].xyz), dot(p, transform[slot * 2 + 1].xyz));
}

VertexOutput vsWater(VertexInput input)
{
    VertexOutput o;
    const float4 world = mul(model, float4(input.position, 1.0));
    o.position = mul(viewProjection, world);
    o.world = world.xyz;
    o.uv03 = float4(transformUv(input.uv, 0), transformUv(input.uv3, 3));
    o.uv12 = float4(transformUv(input.uv1, 1), transformUv(input.uv2, 2));
    const float d = max(length(eyePosition.xyz - world.xyz) - fog.x, 0.0);
    o.fog = fog.z > 0.5 ? exp(-(fog.y * d) * (fog.y * d)) : 1.0;
    o.tangent = mul((float3x3)normalMatrix, input.tangent);
    o.binormal = mul((float3x3)normalMatrix, input.binormal);
    o.normal = mul((float3x3)normalMatrix, input.normal);
    o.view = normalize(eyePosition.xyz - world.xyz);
    return o;
}

cbuffer WaterFragmentUniforms : register(b0, space3)
{
    float4 hemiTop;                       // forward gHemiLight colorTop / colorBottom
    float4 hemiBottom;
    float4 dirColour;                     // forward gDirLight[0].Color
    float4 dirDirection;                  // gDirLight[0].Direction (direction of travel)
    float4 fogColour;                     // gFogColor
    float4 matSpecular;
    float4 matRefractColour;
    float4 params;                        // x: MatPower, y: 1 / ClampDepth, z: RefractionDisplacement, w: VolumeDisplacement
    float4 screen;                        // x, y: 1 / target size; z, w: projection row 2 (z_ndc = z + w / view z)
    float4 eye;
};

[[vk::combinedImageSampler]] Texture2D diffuseTexture : register(t0, space2);      // wrap, linear
[[vk::combinedImageSampler]] SamplerState diffuseSampler : register(s0, space2);
[[vk::combinedImageSampler]] Texture2D normalMapTexture : register(t1, space2);    // wrap
[[vk::combinedImageSampler]] SamplerState normalMapSampler : register(s1, space2);
[[vk::combinedImageSampler]] Texture2D modifierTexture : register(t2, space2);     // clamp
[[vk::combinedImageSampler]] SamplerState modifierSampler : register(s2, space2);
[[vk::combinedImageSampler]] Texture2D normalMap1Texture : register(t3, space2);   // wrap
[[vk::combinedImageSampler]] SamplerState normalMap1Sampler : register(s3, space2);
[[vk::combinedImageSampler]] Texture3D volumeTexture : register(t4, space2);       // wrap u / v, clamp w
[[vk::combinedImageSampler]] SamplerState volumeSampler : register(s4, space2);
[[vk::combinedImageSampler]] Texture2D<float> depthTexture : register(t5, space2); // the scene depth (point, clamp)
[[vk::combinedImageSampler]] SamplerState depthSampler : register(s5, space2);
[[vk::combinedImageSampler]] Texture2D frameTexture : register(t6, space2);        // the frame so far (linear, clamp)
[[vk::combinedImageSampler]] SamplerState frameSampler : register(s6, space2);

float viewDepth(float zNdc) { return screen.w / (zNdc - screen.z); }

float4 psWater(VertexOutput input) : SV_Target
{
    // Depth test against the scene (the game's pass: ZENABLE, no writes).
    const float sceneHere = depthTexture.Load(int3(int2(input.position.xy), 0));
    if (input.position.z > sceneHere)
        discard;
    const float2 uv = input.position.xy * screen.xy;
    const float waterZ = viewDepth(input.position.z);
    const float thickness0 = viewDepth(sceneHere) - waterZ;
    // Two scrolling normal maps, added: (a + b) x 2 - 2, through the tangent frame; normalised.
    const float3 m = (normalMapTexture.Sample(normalMapSampler, input.uv12.xy).rgb + normalMap1Texture.Sample(normalMap1Sampler, input.uv12.zw).rgb) * 2.0 - 2.0;
    const float3 n = normalize(m.x * input.tangent + m.y * input.binormal + m.z * input.normal);
    // Refraction: the frame at uv + n.xz x RefractionDisplacement, unless that point lies nearer than the water's own depth there.
    const float2 refractedUv = n.xz * params.z + uv;
    const float3 straight = frameTexture.Sample(frameSampler, uv).rgb;
    const float3 refracted = frameTexture.Sample(frameSampler, refractedUv).rgb;
    const float thickness1 = viewDepth(depthTexture.SampleLevel(depthSampler, refractedUv, 0)) - waterZ;
    const float d0 = saturate(thickness0 * params.y), d1 = saturate(thickness1 * params.y);
    const bool useStraight = d0 - d1 >= 0.0;
    const float deep = max(d0, d1);
    // The water's body: volume map (by thickness) x diffuse x the modifier's alpha (by thickness).
    const float2 volumeUv = n.xz * params.w + input.uv03.zw;
    const float4 volume = volumeTexture.Sample(volumeSampler, float3(volumeUv, deep));
    float4 body = diffuseTexture.Sample(diffuseSampler, n.xz * params.w + input.uv03.xy);
    body.a *= modifierTexture.Sample(modifierSampler, float2(deep, deep)).a;
    body *= volume;
    // Forward lighting: hemi by n.y, the first directional light by N.L.
    const float ndl = dot(n, -dirDirection.xyz);
    const float3 light = lerp(hemiBottom.rgb, hemiTop.rgb, (n.y + 1.0) * 0.5) + dirColour.rgb * max(ndl, 0.0);
    const float3 behind = (useStraight ? straight : refracted) * matRefractColour.rgb;
    float3 colour = lerp(behind, 2.0 * body.rgb * light, body.a);
    // Specular of the directional light: pow(N.H, MatPower), H = normalize(V - direction). V is the vertex shader's per-vertex
    // view direction, interpolated and used as it comes (traced 2026-10-20, tabula_rasa.7.trace call 31402441: vs o6 =
    // normalize(gWorldEyePos - world), ps "add r3, -c10, v4; nrm") - on a 9-vertex 64 m pond this is far from the per-pixel
    // direction and decides where the MatPower 500 sparkles fall.
    const float3 v = input.view;
    const float ndh = dot(n, normalize(v - dirDirection.xyz));
    if (params.x != 0.0 && ndh > 0.0 && ndl > 0.0)
        colour += pow(ndh, params.x) * dirColour.rgb * matSpecular.rgb;
    return float4(lerp(fogColour.rgb, colour, input.fog), 1.0);
}
