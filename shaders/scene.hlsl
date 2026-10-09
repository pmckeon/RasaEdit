// scene.hlsl - textured lit meshes (models, placeholders) and unlit lines for the 3D viewport.
//
// Registers follow SDL_gpu's rules (SDL 3.4): vertex uniforms in space1; fragment textures and
// samplers in space2 (combined image samplers in SPIR-V); fragment uniforms in space3. DXC maps
// register spaces to SPIR-V descriptor sets, so one source serves DXIL and SPIR-V.
// Vertex attributes use TEXCOORD<location> semantics as SDL's Direct3D 12 backend requires.
// Matrices are row_major, used as mul(matrix, columnVector); world space is the game's
// left-handed space (+x right, +y up, +z forward).

#include "lighting.hlsli"

cbuffer VertexUniforms : register(b0, space1)
{
    row_major float4x4 viewProjection;
    row_major float4x4 model;
};

cbuffer FragmentUniforms : register(b0, space3)
{
    float4 tint;              // multiplied with the vertex colour and the texture
    float4 lightDirection;    // xyz: towards the light (world space); w: 1 = lit, 0 = unlit
    float4 options;           // x: 1 = alpha test (discard below 0.5); z: 1 = additive
    float4 hue;               // rgb added to the texel weighted by its alpha (the game's "Hue" effects); w: 1 = on
    float4 sunFront;          // rgb: directional light on surfaces facing it
    float4 sunBack;           // rgb: directional light's back colour (surfaces facing away)
    float4 hemiTop;           // rgb: hemispheric light from above
    float4 hemiBottom;        // rgb: hemispheric light from below
    float4 fogColour;         // rgb; w: 1 = fog on
    float4 fogRange;          // x: start distance, y: density
    float4 cameraPosition;    // xyz
    float4 localCount;        // x: number of placed lights
    float4 localPosition[MAX_LOCAL_LIGHTS];
    float4 localColour[MAX_LOCAL_LIGHTS];
    float4 localSpot[MAX_LOCAL_LIGHTS];
    float4 localSpotInner[MAX_LOCAL_LIGHTS];
};

[[vk::combinedImageSampler]] Texture2D diffuseTexture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState diffuseSampler : register(s0, space2);
[[vk::combinedImageSampler]] Texture2D glowTexture : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState glowSampler : register(s1, space2);

struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 colour   : TEXCOORD2;
    float2 uv       : TEXCOORD3;
    float2 uv1      : TEXCOORD4;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal   : TEXCOORD0;
    float4 colour   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float3 world    : TEXCOORD3;
    float2 uv1      : TEXCOORD4;
};

VertexOutput vsMain(VertexInput input)
{
    VertexOutput output;
    const float4 world = mul(model, float4(input.position, 1.0));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)model, input.normal);
    output.colour = input.colour;
    output.uv = input.uv;
    output.world = world.xyz;
    output.uv1 = input.uv1;
    return output;
}

float4 psMain(VertexOutput input) : SV_Target0
{
    const float4 texel = diffuseTexture.Sample(diffuseSampler, input.uv);
    if (options.x > 0.5 && texel.a < 0.5)
        discard;
    // Hued textures (avatar skin, hair, clothing) are grey with the hue mask in alpha; the game's fallback
    // shader computes diffuse.rgb + Hue.rgb * diffuse.a (decal maps not applied here).
    const float3 albedo = hue.w > 0.5 ? saturate(texel.rgb + hue.rgb * texel.a) : texel.rgb;
    const float4 base = input.colour * tint * float4(albedo, 1.0);
    if (options.z > 0.5)
    {
        // Additive (TRAdditive* effects: glows, holograms, light cones): unlit, blended one/one; fog fades it out.
        const float3 black = applyFog(base.rgb, length(cameraPosition.xyz - input.world), float4(0, 0, 0, fogColour.w), fogRange);
        return float4(black, 1.0);
    }
    if (lightDirection.w < 0.5)
        return base;
    // Two-sided: the normal is turned towards the camera (mesh winding and thin geometry are not relied on).
    const float3 view = cameraPosition.xyz - input.world;
    float3 n = normalize(input.normal);
    if (dot(n, view) < 0.0)
        n = -n;
    SkyLighting s;
    s.towardsLight = normalize(lightDirection.xyz);
    s.sunFront = sunFront.rgb;
    s.sunBack = sunBack.rgb;
    s.hemiTop = hemiTop.rgb;
    s.hemiBottom = hemiBottom.rgb;
    // Glow maps add after lighting (the game's glow buffer, added in trhdrcomposite before fog).
    const float3 glow = glowTexture.Sample(glowSampler, input.uv1).rgb;
    const float3 lit = base.rgb * (skyLight(n, s) + localLights(input.world, n, (int)localCount.x, localPosition, localColour, localSpot, localSpotInner)) + glow;
    return float4(applyFog(lit, length(view), fogColour, fogRange), base.a);
}
