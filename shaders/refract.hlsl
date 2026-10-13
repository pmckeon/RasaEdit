// refract.hlsl - the game's TRRefract pixel shader (effects.glm trrefractsp_2.fxc, technique Refract, pass
// WaterReflectRefract, psTable[0]; disassembled 2026-10-12): screen-space distortion shells (e.g. the Bootcamp
// hologram's square-pattern field). Drawn with scene.hlsl's vsMain (the same vertex layout and outputs); the frame copy
// is the colour target after opaque geometry. Pass states: SRCALPHA / INVSRCALPHA, depth test and write, no culling,
// no fog. Registers follow scene.hlsl's notes (SDL_gpu spaces).
// 2026-10-14: matched to the shaders bound at the Bootcamp hologram's refraction draw in an apitrace capture (the Shader Model 3
// pair psTable[1] / vsTableSkinned3Bone[1]): the skinned vertex shader passes the normal, tangent and binormal in WORLD space
// (gWorldMatrixArray), normalised, while the static one (vsTable[1]) passes them in view space (gWorldViewMatrix), not
// normalised - params.w selects. The pixel shader samples the scene depth (MRT_Sampler_Depth): where the scene at the refracted
// point is nearer than the shell it shows the unrefracted frame, and at its own pixel it is depth-tested against the scene - the
// editor's pass has no depth attachment and does both tests here (refraction is drawn last, its depth writes are not needed).

cbuffer RefractUniforms : register(b0, space3)
{
    float4 matDiffuse;        // MatDiffuse as the shader sees it (x gModulateScale 0.5)
    float4 params;            // x: RefractionDisplacement, y: 1 / target width, z: 1 / target height, w: 1 = skinned piece (world space)
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
[[vk::combinedImageSampler]] Texture2D<float> depthTexture : register(t4, space2);  // the scene depth (point, clamp)
[[vk::combinedImageSampler]] SamplerState depthSampler : register(s4, space2);

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
    // Own pixel: hidden behind the scene (the game writes the frame back unchanged).
    if (input.position.z > depthTexture.Load(int3(int2(input.position.xy), 0)))
        discard;
    // t0 / t1 / t2: the normal, tangent and binormal - view space, not normalised (static pieces), or world space, normalised
    // (skinned pieces), as the game's two vertex shaders pass them.
    const bool skinned = params.w > 0.5;
    const float3 t0 = skinned ? normalize(input.normal) : toView(input.normal);
    const float3 t1 = skinned ? normalize(input.tangent) : toView(input.tangent);
    const float3 t2 = skinned ? normalize(input.binormal) : toView(input.binormal);
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
    // The scene at the refracted point nearer than the shell: the unrefracted frame, opaque (= the pixel unchanged).
    if (depthTexture.SampleLevel(depthSampler, uvR, 0) < input.position.z)
        return float4(frameTexture.Sample(frameSampler, screen).rgb, 1.0);
    const float4 diff = diffuseTexture.Sample(diffuseSampler, uv0 + (uvR - screen));
    const float3 frame = frameTexture.Sample(frameSampler, uvR).rgb;
    const float3 rgb = frame * matDiffuse.rgb * 2.0 + (diff.rgb - 0.5) * 2.0;
    return float4(rgb, fade * diff.a * input.colour.a);
}
