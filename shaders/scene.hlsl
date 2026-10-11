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
    float4 options;           // x: > 0 = alpha test, discard texture alpha below x; y: 1 = alpha-blended particle, 2 = translucent (lit, texture alpha);
                              // z: 1 = additive, 2 = lit additive (forward-phase pieces); w: 1 = aniso additive
    float4 hue;               // rgb added to the texel weighted by its alpha (the game's "Hue" effects); w: 1 = on
    float4 sunFront;          // rgb: directional light on surfaces facing it
    float4 sunBack;           // rgb: directional light's back colour (surfaces facing away)
    float4 hemiTop;           // rgb: hemispheric light from above
    float4 hemiBottom;        // rgb: hemispheric light from below
    float4 fogColour;         // rgb; w: 1 = fog on
    float4 fogRange;          // x: start distance, y: density, z: 1 = light-buffer debug view, w: 2 normals / 3 spec intensity view
    float4 cameraPosition;    // xyz
    float4 localCount;        // x: number of placed lights
    float4 localPosition[MAX_LOCAL_LIGHTS];
    float4 localColour[MAX_LOCAL_LIGHTS];
    float4 localSpot[MAX_LOCAL_LIGHTS];
    float4 localSpotInner[MAX_LOCAL_LIGHTS];
    float4 localRotation[MAX_LOCAL_LIGHTS];
    row_major float4x4 shadowMatrix;   // world -> sun shadow map clip space
    float4 shadowParams;              // x: 1 = sun shadows on, y: opacity, z: 1 / map size
    float4 specular;          // aniso additive: MatSpecular
    float4 layers;            // x detail on UV set 1, y decal on UV set 1, z has detail, w has decal (aniso additive and
                              // forward-phase pieces)
    float4 material;          // x: 1 = normal map (t7, tangent frame), y: 1 = gloss map (t8: r spec power, g spec intensity)
};

[[vk::combinedImageSampler]] Texture2D diffuseTexture : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState diffuseSampler : register(s0, space2);
[[vk::combinedImageSampler]] Texture2D glowTexture : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState glowSampler : register(s1, space2);
[[vk::combinedImageSampler]] Texture2D detailTexture : register(t2, space2);
[[vk::combinedImageSampler]] SamplerState detailSampler : register(s2, space2);
[[vk::combinedImageSampler]] Texture2D decalTexture : register(t3, space2);
[[vk::combinedImageSampler]] SamplerState decalSampler : register(s3, space2);
[[vk::combinedImageSampler]] Texture2D lightTexture : register(t4, space2);
[[vk::combinedImageSampler]] SamplerState lightSampler : register(s4, space2);
[[vk::combinedImageSampler]] Texture2D<float> shadowMap : register(t5, space2);
[[vk::combinedImageSampler]] TextureCubeArray globeMaps : register(t6, space2);
[[vk::combinedImageSampler]] SamplerState globeSampler : register(s6, space2);
[[vk::combinedImageSampler]] SamplerComparisonState shadowSampler : register(s5, space2);
[[vk::combinedImageSampler]] Texture2D normalMapTexture : register(t7, space2);
[[vk::combinedImageSampler]] SamplerState normalMapSampler : register(s7, space2);
[[vk::combinedImageSampler]] Texture2D glossTexture : register(t8, space2);
[[vk::combinedImageSampler]] SamplerState glossSampler : register(s8, space2);

struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 colour   : TEXCOORD2;
    float2 uv       : TEXCOORD3;
    float2 uv1      : TEXCOORD4;
    float3 tangent  : TEXCOORD5;
    float3 binormal : TEXCOORD6;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal   : TEXCOORD0;
    float4 colour   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
    float3 world    : TEXCOORD3;
    float2 uv1      : TEXCOORD4;
    float3 tangent  : TEXCOORD5;
    float3 binormal : TEXCOORD6;
};

VertexOutput vsMain(VertexInput input)
{
    VertexOutput output;
    const float4 world = mul(model, float4(input.position, 1.0));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)model, input.normal);
    output.tangent = mul((float3x3)model, input.tangent);
    output.binormal = mul((float3x3)model, input.binormal);
    output.colour = input.colour;
    output.uv = input.uv;
    output.world = world.xyz;
    output.uv1 = input.uv1;
    return output;
}

// The game's TRAdditiveAnisoSpecMapDiffMap[DetMapDecMap]VertColor pixel shader (effects.glm, disassembled):
// a detail layer added around 0.5 grey, a decal blended by its alpha, and a specular term from LightTexture,
// an alpha ramp looked up by how directly the surface faces the camera (N.V, not two-sided).
float3 anisoAdditive(VertexOutput input)
{
    const float4 diffuse = diffuseTexture.Sample(diffuseSampler, input.uv);
    float3 albedo = diffuse.rgb;
    if (layers.z > 0.5)
        albedo += detailTexture.Sample(detailSampler, layers.x > 0.5 ? input.uv1 : input.uv).rgb - 0.5;
    if (layers.w > 0.5)
    {
        const float4 decal = decalTexture.Sample(decalSampler, layers.y > 0.5 ? input.uv1 : input.uv);
        albedo = lerp(albedo, decal.rgb, decal.a);
    }
    const float facing = max(dot(normalize(input.normal), normalize(cameraPosition.xyz - input.world)), 0.0);
    const float ramp = lightTexture.Sample(lightSampler, float2(0.5, facing)).a;
    const float alpha = diffuse.a * input.colour.a * tint.a;
    return max(4.0 * alpha * albedo * (albedo * input.colour.rgb + ramp * specular.rgb), 0.0);
}

float4 psMain(VertexOutput input) : SV_Target0
{
    const bool particle = options.y > 0.5 && options.y < 1.5;
    if ((fogRange.z > 0.5 || fogRange.w > 1.5) && (options.w > 0.5 || options.z > 0.5 || particle))
        return float4(0, 0, 0, options.y > 0.5 ? 0 : 1);   // debug views: additive / forward effects are not in the G-buffer
    if (options.w > 0.5)
    {
        const float3 colour = applyFog(anisoAdditive(input), length(cameraPosition.xyz - input.world), float4(0, 0, 0, fogColour.w), fogRange);
        return float4(colour, 1.0);
    }
    const float4 texel = diffuseTexture.Sample(diffuseSampler, input.uv);
    if (options.x > 0.0 && texel.a < options.x)
        discard;
    // Hued textures (avatar skin, hair, clothing) are grey with the hue mask in alpha; the game's fallback
    // shader computes diffuse.rgb + Hue.rgb * diffuse.a (decal maps not applied here).
    float3 albedo = hue.w > 0.5 ? saturate(texel.rgb + hue.rgb * texel.a) : texel.rgb;
    // Detail layer, added around 0.5 grey (the forward TRDiffMapDetMapVertColor pixel shader psPixelShader:
    // (diffuse + detail - 0.5) x lit vertex colour x 2). Set only for forward-phase pieces.
    if (layers.z > 0.5)
        albedo += detailTexture.Sample(detailSampler, layers.x > 0.5 ? input.uv1 : input.uv).rgb - 0.5;
    const float4 base = input.colour * tint * float4(albedo, 1.0);
    if (options.z > 0.5)
    {
        // Additive (TRAdditive* effects: glows, light cones, mist): unlit, blended one/one, scaled by texture alpha x
        // vertex alpha (as the game's TRAdditiveAnisoSpec* shader does; the plain additive shader is not stored -
        // UNVERIFIED); fog fades it out. options.z = 2: a forward-phase piece in the Additive phase, lit per vertex like
        // the game's forward technique - vertex colour x (hemi + directional N.L + point lights), no shadow term -
        // alpha = diffuse.a x vertex.a x MatDiffuse.a (tint.a), blended SRCALPHA / ONE (PhaseAdditive.fx).
        float3 rgb = base.rgb;
        if (options.z > 1.5)
        {
            const float3 nf = normalize(input.normal);
            SkyLighting sf;
            sf.towardsLight = normalize(lightDirection.xyz);
            sf.sunFront = sunFront.rgb;
            sf.sunBack = sunBack.rgb;
            sf.hemiTop = hemiTop.rgb;
            sf.hemiBottom = hemiBottom.rgb;
            sf.sunVisibility = 1.0;
            rgb *= skyLight(nf, sf) + localLights(input.world, nf, (int)localCount.x, localPosition, localColour, localSpot, localSpotInner, localRotation,
                                                  globeMaps, globeSampler);
        }
        const float3 added = rgb * texel.a * input.colour.a * (options.z > 1.5 ? tint.a : 1.0);
        const float3 black = applyFog(added, length(cameraPosition.xyz - input.world), float4(0, 0, 0, fogColour.w), fogRange);
        return float4(black, 1.0);
    }
    if (particle)
    {
        // Alpha-blended particles (the game's PalParticleSystem pixel shader: texture x vertex colour, alpha x fog).
        const float visibility = applyFog(float3(1, 1, 1), length(cameraPosition.xyz - input.world), float4(0, 0, 0, fogColour.w), fogRange).x;
        return float4(base.rgb, base.a * texel.a * visibility);
    }
    if (lightDirection.w < 0.5)
        return base;
    // Two-sided: the normal is turned towards the camera (mesh winding and thin geometry are not relied on).
    const float3 view = cameraPosition.xyz - input.world;
    float3 n = normalize(input.normal);
    if (dot(n, view) < 0.0)
        n = -n;
    // Normal map (Pal* materials, ps2x_BaseDeferred*: N = TBN x (map x 2 - 1), the vertex shader's TBN from TANGENT0,
    // TANGENT1 and NORMAL through the world matrix).
    if (material.x > 0.5)
    {
        const float3 m = normalMapTexture.Sample(normalMapSampler, input.uv).rgb * 2.0 - 1.0;
        n = normalize(normalize(input.tangent) * m.x + normalize(input.binormal) * m.y + n * m.z);
    }
    SkyLighting s;
    s.towardsLight = normalize(lightDirection.xyz);
    s.sunFront = sunFront.rgb;
    s.sunBack = sunBack.rgb;
    s.hemiTop = hemiTop.rgb;
    s.hemiBottom = hemiBottom.rgb;
    // Glow maps add after lighting (the game's glow buffer, added in trhdrcomposite before fog).
    const float3 glow = glowTexture.Sample(glowSampler, input.uv1).rgb;
    // Translucent draws are forward pieces: the game's forward vertex shader has no shadow term.
    s.sunVisibility = options.y > 1.5 ? 1.0 : sunShadow(shadowMap, shadowSampler, shadowMatrix, shadowParams, input.world, n);
    const float3 light = skyLight(n, s) + localLights(input.world, n, (int)localCount.x, localPosition, localColour, localSpot, localSpotInner, localRotation,
                                                         globeMaps, globeSampler);
    if (fogRange.z > 0.5)
        return float4(light * 0.2, 1.0);       // the game's light buffer (x OO_LIGHT_MULTIPLIER), as /dl_showlight shows it
    if (fogRange.w > 1.5)
    {
        if (options.y > 1.5)
            return float4(0, 0, 0, 0);         // translucent (forward) pieces are not in the G-buffer
        if (fogRange.w < 2.5)
            return float4(n * 0.5 + 0.5, 1.0);
        return float4((material.y > 0.5 ? glossTexture.Sample(glossSampler, input.uv).g : 0.0).xxx, 1.0);
    }
    float3 lit = base.rgb * light + glow;
    // Specular (gloss map r = power, g = intensity), added untinted as trhdrcomposite adds its specular buffer.
    if (material.y > 0.5)
    {
        const float4 gloss = glossTexture.Sample(glossSampler, input.uv);
        const float3 v = normalize(view);
        lit += sunFront.rgb * specularFactor(n, v, s.towardsLight, gloss.r, gloss.g) * s.sunVisibility +
               localSpecular(input.world, n, v, gloss.r, gloss.g, (int)localCount.x, localPosition, localColour, localSpot, localSpotInner);
    }
    // Alpha: only translucent draws blend with it. Opaque draws keep it 1 - the viewport image is composited with alpha.
    return float4(applyFog(lit, length(view), fogColour, fogRange), options.y > 1.5 ? base.a * texel.a : base.a);
}

// Shadow pass: depth only; alpha-tested pieces (foliage) discard like the colour pass.
void psShadow(VertexOutput input)
{
    if (options.x > 0.0 && diffuseTexture.Sample(diffuseSampler, input.uv).a < options.x)
        discard;
}
