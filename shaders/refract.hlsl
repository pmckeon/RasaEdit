// refract.hlsl - the game's TRRefract pixel shader (effects.glm trrefractsp_2.fxc, technique Refract, pass
// WaterReflectRefract, psTable[0]; disassembled 2026-10-12): screen-space distortion shells (e.g. the Bootcamp
// hologram's square-pattern field). Drawn with scene.hlsl's vsMain (the same vertex layout and outputs); the frame copy
// is the colour target after opaque geometry. Pass states: SRCALPHA / INVSRCALPHA, depth test and write, no culling,
// no fog. Registers follow scene.hlsl's notes (SDL_gpu spaces).

cbuffer RefractUniforms : register(b0, space3)
{
    float4 matDiffuse;        // MatDiffuse as the shader sees it (x gModulateScale 0.5)
    float4 params;            // x: RefractionDisplacement, y: 1 / target width, z: 1 / target height
    float4 cameraRight;       // view-space axes in world space (left-handed view: x right, y up, z forward)
    float4 cameraUp;
    float4 cameraForward;
    float4 transform[4];      // TexCoordTransform0 u row, v row (diffuse, UV set 0); TexCoordTransform1 (normal map, UV set 1)
};

[[vk::combinedImageSampler]] Texture2D diffuseTexture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState diffuseSampler : register(s0, space2);
[[vk::combinedImageSampler]] Texture2D normalMapTexture : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState normalMapSampler : register(s1, space2);
[[vk::combinedImageSampler]] Texture2D fadeTexture : register(t2, space2);         // ModifierTexture (RefractAlphaMapSampler)
[[vk::combinedImageSampler]] SamplerState fadeSampler : register(s2, space2);
[[vk::combinedImageSampler]] Texture2D frameTexture : register(t3, space2);        // RefractionSampler: the frame so far
[[vk::combinedImageSampler]] SamplerState frameSampler : register(s3, space2);

struct VertexOutput           // = scene.hlsl's VertexOutput
{
    float4 position : SV_Position;
    float3 normal   : TEXCOORD0;
    float4 colour   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float3 world    : TEXCOORD3;
    float2 uv1      : TEXCOORD4;
    float3 tangent  : TEXCOORD5;
    float3 binormal : TEXCOORD6;
    float2 uv2      : TEXCOORD7;
    float2 aniso    : TEXCOORD8;
};

float3 toView(float3 v) { return float3(dot(v, cameraRight.xyz), dot(v, cameraUp.xyz), dot(v, cameraForward.xyz)); }

float4 psRefract(VertexOutput input) : SV_Target0
{
    const float3 h0 = float3(input.uv, 1.0), h1 = float3(input.uv1, 1.0);
    const float2 uv0 = float2(dot(h0, transform[0].xyz), dot(h0, transform[1].xyz));
    const float2 uvN = float2(dot(h1, transform[2].xyz), dot(h1, transform[3].xyz));
    // t0 / t1 / t2: the view-space normal, tangent and binormal (gWorldViewMatrix, not normalised).
    const float3 t0 = toView(input.normal), t1 = toView(input.tangent), t2 = toView(input.binormal);
    const float4 nm = normalMapTexture.Sample(normalMapSampler, uvN);
    const float3 m = nm.xyz * 2.0 - 1.0;
    const float3 r = float3(dot(m, t1), dot(m, t2), dot(m, t0));
    const float inv = rsqrt(dot(r, r));
    // Edge fade by how much the perturbed normal faces the viewer: RefractAlphaMap at ((lerp(t0.z, n.z, nm.a) + 1) / 2).
    const float facing = (lerp(t0.z, r.z * inv, nm.a) + 1.0) * 0.5;
    const float fade = fadeTexture.Sample(fadeSampler, float2(facing, facing)).a;
    const float strength = diffuseTexture.Sample(diffuseSampler, uv0).a * params.x * fade;
    const float2 screen = input.position.xy * params.yz;
    float2 uvR = min(screen + r.xy * inv * strength, 0.997);
    uvR = max(uvR, 0.003);
    const float4 diff = diffuseTexture.Sample(diffuseSampler, uv0 + (uvR - screen));
    const float3 frame = frameTexture.Sample(frameSampler, uvR).rgb;
    const float3 rgb = frame * matDiffuse.rgb * 2.0 + (diff.rgb - 0.5) * 2.0;
    return float4(rgb, fade * diff.a * input.colour.a);
}
