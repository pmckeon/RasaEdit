// scene.hlsl - textured lit meshes (models, placeholders) and unlit lines for the 3D viewport.
//
// Registers follow SDL_gpu's rules (SDL 3.4): vertex uniforms in space1; fragment textures and
// samplers in space2 (combined image samplers in SPIR-V); fragment uniforms in space3. DXC maps
// register spaces to SPIR-V descriptor sets, so one source serves DXIL and SPIR-V.
// Vertex attributes use TEXCOORD<location> semantics as SDL's Direct3D 12 backend requires.
// Matrices are row_major, used as mul(matrix, columnVector); world space is the game's
// left-handed space (+x right, +y up, +z forward).

cbuffer VertexUniforms : register(b0, space1)
{
    row_major float4x4 viewProjection;
    row_major float4x4 model;
};

cbuffer FragmentUniforms : register(b0, space3)
{
    float4 tint;              // multiplied with the vertex colour and the texture
    float4 lightDirection;    // xyz: towards the light (world space); w: 1 = lit, 0 = unlit
    float4 options;           // x: 1 = alpha test (discard below 0.5)
};

[[vk::combinedImageSampler]] Texture2D diffuseTexture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState diffuseSampler : register(s0, space2);

struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 colour   : TEXCOORD2;
    float2 uv       : TEXCOORD3;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal   : TEXCOORD0;
    float4 colour   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
};

VertexOutput vsMain(VertexInput input)
{
    VertexOutput output;
    const float4 world = mul(model, float4(input.position, 1.0));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)model, input.normal);
    output.colour = input.colour;
    output.uv = input.uv;
    return output;
}

float4 psMain(VertexOutput input) : SV_Target0
{
    const float4 texel = diffuseTexture.Sample(diffuseSampler, input.uv);
    if (options.x > 0.5 && texel.a < 0.5)
        discard;
    const float4 base = input.colour * tint * float4(texel.rgb, 1.0);
    if (lightDirection.w < 0.5)
        return base;
    const float3 n = normalize(input.normal);
    // Two-sided: mesh winding and thin geometry are not relied on.
    const float diffuse = abs(dot(n, normalize(lightDirection.xyz)));
    return float4(base.rgb * (0.35 + 0.65 * diffuse), base.a);
}
