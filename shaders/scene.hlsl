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
    uint4 skin;               // x: 1 = skinned (GPU skinning), y: first bone row in boneRows, z: 1 = skin the normal too (tangents always),
                              // w: 1 = TRAdditiveAnisoSpec* piece: facing term and fog per vertex as vs2x_AnisoDetDec[_Skinned]
    // vs2x_AnisoDetDec[_Skinned] (verified 2026-10-14 against an apitrace capture of the game): oT0.y = max(N . V, 0) per vertex with
    // N = (the mesh normal x gWorldMatrix) x gInvWorldScale, NOT normalised (skinned pieces: gWorldMatrix = palette entry 0's world
    // skin matrix, gInvWorldScale = 1 / the entity's scale, so |N| = that bone's scale, e.g. 5.228 for the hologram's body; static
    // pieces: the node's matrix and 1 / its scale, |N| = 1), V = normalize(eye - position); oFog = exp(-(density x max(d - start,
    // 0))^2), and the runtime state has FOGENABLE on with FOGCOLOR 0.
    row_major float4x4 anisoNormal;   // the 3 x 3 applied to the vertex normal
    float4 eyePosition;               // xyz
    float4 anisoFog;                  // x start, y density, z 1 = fog on; w 1 = sky layer: put at the far plane (z = w)
};

// GPU skinning (Renderer::setSkin): per vertex up to 4 bones of the model's skeleton and their weights (SV_VertexID indexes it;
// weight 0 = unused), and the frame's bone matrices as 3 rows each (the affine part of skin = posed world x inverse bind).
// Vertex storage buffers are t[n] in space0 (SDL_gpu).
struct SkinVertex
{
    uint4 bones;
    float4 weights;
};
StructuredBuffer<SkinVertex> skinVertices : register(t0, space0);
StructuredBuffer<float4> boneRows : register(t1, space0);

cbuffer FragmentUniforms : register(b0, space3)
{
    float4 tint;              // multiplied with the vertex colour and the texture
    float4 lightDirection;    // xyz: towards the light (world space); w: 1 = lit, 0 = unlit
    float4 options;           // x: > 0 = alpha test, discard texture alpha below x; y: 1 = alpha-blended particle, 2 = translucent (lit, texture alpha),
                              // 3 = sky layer (TREmissive*, unlit, alpha x texture alpha);
                              // z: 1 = additive, 2 = lit additive (forward-phase pieces); w: 1 = aniso additive
    float4 hue;               // rgb - 0.5 added to the texel weighted by its alpha (the game's "Hue" effects); w: 1 = on, 2 = on and not
                              // weighted (alpha-tested Pal*Hue effects: their alpha is the cut-out)
    float4 sunFront;          // rgb: directional light on surfaces facing it
    float4 sunBack;           // rgb: directional light's back colour (surfaces facing away)
    float4 hemiTop;           // rgb: hemispheric light from above
    float4 hemiBottom;        // rgb: hemispheric light from below
    float4 fogColour;         // rgb; w: 1 = fog on
    float4 fogRange;          // x: start distance, y: density, z: 1 = light-buffer debug view, w: 2 normals / 3 spec intensity / 4 albedo view
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
    float4 specular;          // aniso additive: MatSpecular
    float4 layers;            // x detail on UV set 1, y decal on UV set 1, z has detail, w has decal (aniso additive and
                              // forward-phase pieces; w = 2: a Pal*Dec* decal map in t3 on uv2, lerped into the deferred albedo)
                              // forward-phase pieces)
    float4 material;          // x: 1 = normal map (t7, tangent frame), y: 1 = gloss map (t8: r spec power, g spec intensity,
                              // b environment reflection), z: 1 = environment cube reflection (t9), w: 1 = cut-out by normal-map alpha
    // TR* deferred materials (TrMaterial in Renderer.h): slot 1 map in t2 on uv1, slot 2 map in t3 on uv2.
    float4 trFlags;           // x slot 1 kind, y slot 2 kind (1 modifier, 2 detail, 3 decal), z environment mode, w bits (1 active,
                              // 2 albedo x (2 vcol + emissive), 4 specular, 8 emissive glow, 0x10 fading forward,
                              // 0x20 forward piece: UV transforms only)
    float4 trSpecular;        // MatSpecular
    float4 trEmissive;        // MatEmissive x 0.5
    float4 trTransform[6];    // TexCoordTransform0..2: u row, v row
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
[[vk::combinedImageSampler]] TextureCube environmentTexture : register(t9, space2);
[[vk::combinedImageSampler]] SamplerState environmentSampler : register(s9, space2);
[[vk::combinedImageSampler]] Texture2D<float> shadowAtlas : register(t10, space2);     // placed-light shadow maps (point, clamp)
[[vk::combinedImageSampler]] SamplerState shadowAtlasSampler : register(s10, space2);
[[vk::combinedImageSampler]] Texture2DArray cookieMaps : register(t11, space2);        // spot lights' 2D globe maps
[[vk::combinedImageSampler]] SamplerState cookieSampler : register(s11, space2);
StructuredBuffer<LocalShadowSlot> localShadowSlots : register(t12, space2);   // placed-light shadow slots (after the 12 samplers)

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
    float2 uv2      : TEXCOORD7;
    float2 aniso    : TEXCOORD8;      // skin.w = 1: x facing (N . V, clamped at 0), y fog visibility
};

VertexOutput vsMain(VertexInput input, uint vertexId : SV_VertexID)
{
    VertexOutput output;
    float3 position = input.position, normal = input.normal, tangent = input.tangent, binormal = input.binormal;
    if (skin.x != 0)
    {
        // Linear blend as SceneView's CPU skinning did: the position averaged by the total weight, directions summed.
        const SkinVertex sv = skinVertices[vertexId];
        float3 sp = 0, sn = 0, st = 0, sb = 0;
        float total = 0;
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            const float w = sv.weights[k];
            if (w <= 0.0) continue;
            const uint row = skin.y + sv.bones[k] * 3;
            const float4 r0 = boneRows[row], r1 = boneRows[row + 1], r2 = boneRows[row + 2];
            const float4 p = float4(input.position, 1.0);
            sp += w * float3(dot(r0, p), dot(r1, p), dot(r2, p));
            sn += w * float3(dot(r0.xyz, input.normal), dot(r1.xyz, input.normal), dot(r2.xyz, input.normal));
            st += w * float3(dot(r0.xyz, input.tangent), dot(r1.xyz, input.tangent), dot(r2.xyz, input.tangent));
            sb += w * float3(dot(r0.xyz, input.binormal), dot(r1.xyz, input.binormal), dot(r2.xyz, input.binormal));
            total += w;
        }
        if (total > 0.0)
        {
            position = sp / total;
            if (skin.z != 0) normal = sn;
            tangent = st;
            binormal = sb;
        }
    }
    const float4 world = mul(model, float4(position, 1.0));
    output.position = mul(viewProjection, world);
    if (anisoFog.w > 0.5)
        output.position.z = output.position.w;      // sky layer: passes the LESS_OR_EQUAL test only on background pixels
    output.normal = mul((float3x3)model, normal);
    output.tangent = mul((float3x3)model, tangent);
    output.binormal = mul((float3x3)model, binormal);
    output.colour = input.colour;
    output.uv = input.uv;
    output.world = world.xyz;
    output.uv1 = input.uv1;
    output.uv2 = input.uv2;
    output.aniso = float2(0.0, 1.0);
    if (skin.w != 0)
    {
        const float3 n = mul((float3x3)anisoNormal, normal);
        const float3 toEye = eyePosition.xyz - world.xyz;
        const float d = length(toEye);
        const float f = max(d - anisoFog.x, 0.0) * anisoFog.y;
        output.aniso = float2(max(dot(n, toEye / max(d, 1e-6)), 0.0), anisoFog.z > 0.5 ? exp(-f * f) : 1.0);
    }
    return output;
}

// TexCoordTransform<slot> as the TR vertex shaders apply it: dp3 of (u, v, 1) with the matrix's first two columns (the
// shader constant is MATRIX_COLUMNS; SceneView passes (e0, e4, e8) and (e1, e5, e9) of the stored 16 floats).
float2 trTransformUv(float2 uv, uint slot)
{
    const float3 h = float3(uv, 1.0);
    return float2(dot(h, trTransform[slot * 2].xyz), dot(h, trTransform[slot * 2 + 1].xyz));
}

// The game's TRAdditiveAnisoSpecMapDiffMap[DetMapDecMap]VertColor pixel shader (effects.glm, disassembled):
// a detail layer added around 0.5 grey, a decal blended by its alpha, and a specular term from LightTexture,
// an alpha ramp looked up by how directly the surface faces the camera (N.V, not two-sided).
float3 anisoAdditive(VertexOutput input)
{
    // vs2x_AnisoDetDec: diffuse = UV set 0 x TexCoordTransform0, detail = its UV set (uv1) x TexCoordTransform1, decal = its UV
    // set (uv2) x TexCoordTransform2.
    const float4 diffuse = diffuseTexture.Sample(diffuseSampler, trTransformUv(input.uv, 0));
    float3 albedo = diffuse.rgb;
    if (layers.z > 0.5)
        albedo += detailTexture.Sample(detailSampler, trTransformUv(input.uv1, 1)).rgb - 0.5;
    if (options.w > 1.5 && hue.w > 0.5)
        albedo += hue.rgb - 0.5;                // PSAdditiveDetDetHueCompiled: + Hue (stored as colour - 0.5), not weighted
    if (layers.w > 0.5)
    {
        const float4 decal = decalTexture.Sample(decalSampler, trTransformUv(input.uv2, 2));
        albedo = lerp(albedo, decal.rgb, decal.a);
    }
    // Facing from the vertex shader (per vertex, unnormalised normal - see VertexUniforms); LightTexture is sampled clamped.
    const float alpha = diffuse.a * input.colour.a * tint.a;
    // options.w = 2: the plain TRAdditiveDiffMap[DetMapDecMap]VertColor[Hue] effects (PSAdditiveCompiled /
    // PSAdditiveDetDet[Hue]Compiled, disassembled 2026-10-14 and checked against an apitrace capture of a glowstick):
    // albedo x (2 x vertex colour)^2 x alpha, written with that alpha and blended SRCALPHA / ONE (alpha squared overall),
    // fogged to black per vertex like the aniso shader (vsAdditiveDiff / vsAdditive_Array).
    if (options.w > 1.5)
    {
        const float3 v2 = 2.0 * input.colour.rgb;
        return max(albedo * v2 * v2, 0.0) * alpha * alpha * input.aniso.y;
    }
    const float ramp = lightTexture.Sample(lightSampler, float2(0.5, input.aniso.x)).a;
    // The shader writes this alpha to oC0.w and its pass blends SRCALPHA / ONE, so the frame gets the colour x alpha
    // again (alpha squared overall); the editor's additive pipeline is ONE / ONE, so it is applied here.
    // Fog: FOGCOLOR 0, so the colour is scaled by the per-vertex visibility before the SRCALPHA / ONE blend.
    return max(4.0 * alpha * albedo * (albedo * input.colour.rgb + ramp * specular.rgb), 0.0) * alpha * input.aniso.y;
}

// One TR map slot applied to the albedo: 1 modifier (second diffuse, lerp by vertex alpha), 2 detail (+ map - 0.5),
// 3 decal (lerp by the map's alpha).
float3 trApplySlot(float3 albedo, uint kind, float4 map, float vertexAlpha)
{
    if (kind == 1) return lerp(albedo, map.rgb, vertexAlpha);
    if (kind == 2) return albedo + map.rgb - 0.5;
    if (kind == 3) return lerp(albedo, map.rgb, map.a);
    return albedo;
}

float4 psMain(VertexOutput input) : SV_Target0
{
    const bool particle = options.y > 0.5 && options.y < 1.5;
    if ((fogRange.z > 0.5 || fogRange.w > 1.5) && (options.w > 0.5 || options.z > 0.5 || particle || options.y > 2.5))
        return float4(0, 0, 0, options.y > 0.5 ? 0 : 1);   // debug views: additive / forward effects are not in the G-buffer
    if (options.w > 0.5)
    {
        // Fog per vertex (anisoAdditive): the passes set FOGENABLE 0, but the device state at the game's draw has it on (apitrace
        // replay, 2026-10-14) and the vertex shader writes oFog.
        return float4(anisoAdditive(input), 1.0);
    }
    const uint trBits = (uint)trFlags.w;
    const float2 uv0 = (trBits & 0x21) ? trTransformUv(input.uv, 0) : input.uv;
    const float4 texel = diffuseTexture.Sample(diffuseSampler, uv0);
    if (options.x > 0.0 && texel.a < options.x)
        discard;
    if (material.w > 0.5 && normalMapTexture.Sample(normalMapSampler, input.uv).a < 0.5)
        discard;                                // TRHair (ps2x_BaseDeferredAlphaTestNoParallaxHue: texkill(normal.a - 0.5))
    // Hued textures (avatar skin, hair, clothing) are grey with the hue mask in alpha; the game's fallback
    // shader computes diffuse.rgb + Hue.rgb * diffuse.a (decal maps not applied here).
    // The engine stores a HueColor-annotated parameter as colour - 0.5 (effect parameter setter FUN_005bd040), so grey 0.5
    // is neutral.
    float3 albedo = hue.w > 1.5 ? saturate(texel.rgb + hue.rgb - 0.5) : hue.w > 0.5 ? saturate(texel.rgb + (hue.rgb - 0.5) * texel.a) : texel.rgb;
    // Detail layer, added around 0.5 grey (the forward TRDiffMapDetMapVertColor pixel shader psPixelShader:
    // (diffuse + detail - 0.5) x lit vertex colour x 2). Set only for forward-phase pieces.
    if (layers.z > 0.5)
    {
        // layers.x: the detail map's UV set; trBits 0x20: x TexCoordTransform<set> (vsBasicStatic2TexVertColor).
        const uint detailSet = (uint)(layers.x + 0.5);
        const float2 detailUv = detailSet == 2 ? input.uv2 : detailSet == 1 ? input.uv1 : input.uv;
        albedo += detailTexture.Sample(detailSampler, (trBits & 0x20) ? trTransformUv(detailUv, detailSet) : detailUv).rgb - 0.5;
    }
    float4 base = input.colour * tint * float4(albedo, 1.0);
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
            sf.sunBack = float3(0, 0, 0);       // forward gDirLight: Color only
            sf.hemiTop = hemiTop.rgb;
            sf.hemiBottom = hemiBottom.rgb;
            sf.sunVisibility = 1.0;
            rgb *= skyLight(nf, sf) + extraSkyLight(nf, extraTowards, extraFront, extraBack, true) + localLights(input.world, nf, (int)localCount.x, localPosition, localColour, localSpot, localSpotInner, localRotation, globeMaps, globeSampler, cookieMaps, cookieSampler, shadowAtlas, localShadowSlots, localShadowBase);
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
    if (options.y > 2.5)
        return float4(base.rgb, base.a * texel.a);   // sky layer, TREmissive* psNormal: texture x vertex colour x MatDiffuse x 4 (tint)
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
    // Environment reflection (Pal*Env* materials, ps2x_BaseDeferred* / ps2x_DeferredDec*, disassembled 2026-10-11): the
    // G-buffer albedo is lerp(2 x (diffuse + Hue) x vertex colour, envCube(reflect(-V, N)) with LOD bias 3 - 4 sqrt(gloss.r),
    // gloss.b), V = normalize(gWorldEyePos - world position), world space; the cube is gGlobalEnvTexture (the region's).
    if (material.z > 0.5 && material.y > 0.5)
    {
        const float4 gloss = glossTexture.Sample(glossSampler, input.uv);
        const float3 r = reflect(-normalize(view), n);
        const float3 env = environmentTexture.SampleBias(environmentSampler, r, 3.0 - 4.0 * sqrt(gloss.r)).rgb;
        base.rgb = lerp(base.rgb, env, gloss.b);
    }
    // Decal map (every Pal*Dec* deferred pixel shader - ps2x_DeferredDec*, ps2x_DeferredNoEnvDecAlphaTestHue, disassembled
    // 2026-10-14): albedo = lerp(albedo after the reflection, decal.rgb, decal.a); the decal is on UV set DecalTexCoord (the
    // vertex shader array element is DecalTexCoord itself), untransformed, and is not scaled by the vertex colour. tint carries
    // the x2 of the Pal albedo, so the decal gets tint / 2 (the selection highlight only).
    if (layers.w > 1.5)
    {
        const float4 decal = decalTexture.Sample(decalSampler, input.uv2);
        base.rgb = lerp(base.rgb, decal.rgb * tint.rgb * 0.5, decal.a);
    }
    // TR* deferred materials (effects.glm G-buffer pixel shaders): diffuse, then a modifier (slot 1), detail and decal maps in
    // slot order, the environment cube (EnvMapSampler = gGlobalEnvTexture, reflect(-V, N) per vertex, no bias) by
    // detail.a or diffuse.a x MatSpecular.z, then x vertex colour x 2 (+ MatEmissive x 0.5 for DetMapDecMap).
    float trGloss = 1.0;
    float fadeAlpha = 1.0;               // trBits 0x10 (fading forward TRDiffMapDiffMap*): lerp(diffuse.a, modifier.a, vertex alpha)
    if (trBits & 0x1)
    {
        const float4 map1 = detailTexture.Sample(detailSampler, trTransformUv(input.uv1, 1));
        fadeAlpha = trFlags.x == 1 ? lerp(texel.a, map1.a, input.colour.a) : texel.a;
        const float4 map2 = decalTexture.Sample(decalSampler, trTransformUv(input.uv2, 2));
        const uint kind1 = (uint)trFlags.x, kind2 = (uint)trFlags.y, envMode = (uint)trFlags.z;
        const float4 detail = kind1 == 2 ? map1 : map2;
        float3 a = trApplySlot(albedo, kind1 == 3 ? 0 : kind1, map1, input.colour.a);
        a = trApplySlot(a, kind2 == 3 ? 0 : kind2, map2, input.colour.a);
        const float3 env = envMode != 0 ? environmentTexture.Sample(environmentSampler, reflect(-normalize(view), n)).rgb : float3(0, 0, 0);
        if (envMode == 1) a = lerp(a, env, detail.a * trSpecular.z);
        if (kind1 == 3) a = trApplySlot(a, 3, map1, input.colour.a);
        if (kind2 == 3) a = trApplySlot(a, 3, map2, input.colour.a);
        if (envMode == 2) a = lerp(a, env, detail.a * trSpecular.z);
        if (envMode == 3) a = lerp(a, env, texel.a * trSpecular.z);
        trGloss = envMode == 1 || envMode == 2 ? detail.a : envMode == 3 ? texel.a : 1.0;
        const float3 scale = (trBits & 0x2) ? input.colour.rgb * tint.rgb + trEmissive.rgb * tint.rgb * 0.5 : input.colour.rgb * tint.rgb;
        base.rgb = a * scale;
    }
    SkyLighting s;
    s.towardsLight = normalize(lightDirection.xyz);
    s.sunFront = sunFront.rgb;
    s.sunBack = options.y > 1.5 ? float3(0, 0, 0) : sunBack.rgb;   // translucent = forward pieces: no back colour
    s.hemiTop = hemiTop.rgb;
    s.hemiBottom = hemiBottom.rgb;
    // Glow maps add after lighting (the game's glow buffer, added in trhdrcomposite before fog).
    const float3 glow = glowTexture.Sample(glowSampler, input.uv1).rgb + ((trBits & 0x8) && !(trBits & 0x10) ? trEmissive.rgb : float3(0, 0, 0));
    // Translucent draws are forward pieces: the game's forward vertex shader has no shadow term.
    s.sunVisibility = options.y > 1.5 ? 1.0 : sunShadow(shadowMap, shadowSampler, shadowMatrix, shadowParams, shadowExtra, cameraPosition.xyz, input.world, n);
    // Fading forward pieces (trBits 0x10): the effect's forward vertex shader lights with the hemi and directional sky light only.
    const int localLightCount = (trBits & 0x10) ? 0 : (int)localCount.x;
    const float3 light = skyLight(n, s) + extraSkyLight(n, extraTowards, extraFront, extraBack, options.y > 1.5) + localLights(input.world, n, localLightCount, localPosition, localColour, localSpot, localSpotInner, localRotation, globeMaps, globeSampler, cookieMaps, cookieSampler, shadowAtlas, localShadowSlots, localShadowBase);
    if (fogRange.z > 0.5)
        return float4(light * 0.2, 1.0);       // the game's light buffer (x OO_LIGHT_MULTIPLIER), as /dl_showlight shows it
    if (fogRange.w > 1.5)
    {
        if (options.y > 1.5)
            return float4(0, 0, 0, 0);         // translucent (forward) pieces are not in the G-buffer
        if (fogRange.w < 2.5)
            return float4(n * 0.5 + 0.5, 1.0);
        if (fogRange.w > 3.5)
            return float4(saturate(base.rgb), 1.0);   // G-buffer albedo (8-bit target: clamped - UNVERIFIED format)
        const float specIntensity = material.y > 0.5 ? glossTexture.Sample(glossSampler, input.uv).g : (trBits & 0x4) ? trSpecular.y * trGloss : 0.0;
        return float4(specIntensity.xxx, 1.0);
    }
    float3 lit = base.rgb * light + glow;
    // Specular (gloss map r = power, g = intensity), added untinted as trhdrcomposite adds its specular buffer.
    // TR materials: spec power = saturate(MatSpecular.x), intensity = MatSpecular.y (x detail.a / diffuse.a for the gloss
    // variants), from their vertex shaders (oT5 / oT7 = MatSpecular.x x 255, MatSpecular.y; the pixel shader clamps and / 255).
    if (material.y > 0.5 || (trBits & 0x4))
    {
        const float4 gloss = material.y > 0.5 ? glossTexture.Sample(glossSampler, input.uv)
                                              : float4(saturate(trSpecular.x), trSpecular.y * trGloss, 0, 0);
        const float3 v = normalize(view);
        [unroll] for (int k = 0; k < MAX_EXTRA_DIRECTIONAL; ++k)
            if (extraTowards[k].w > 0.5)
                lit += extraFront[k].rgb * specularFactor(n, v, extraTowards[k].xyz, gloss.r, gloss.g);
        lit += hemiSpecular(n, v, gloss.r, gloss.g, hemiTop.rgb);
        lit += sunFront.rgb * specularFactor(n, v, s.towardsLight, gloss.r, gloss.g) * s.sunVisibility +
               localSpecular(input.world, n, v, gloss.r, gloss.g, (int)localCount.x, localPosition, localColour, localSpot, localSpotInner, shadowAtlas, localShadowSlots, localShadowBase);
    }
    // Alpha: only translucent draws blend with it. Opaque draws keep it 1 - the viewport image is composited with alpha.
    const float blendAlpha = (trBits & 0x10) ? fadeAlpha : base.a * texel.a;
    return float4(applyFog(lit, length(view), fogColour, fogRange), options.y > 1.5 ? blendAlpha : 1.0);
}

// Shadow pass: depth only; alpha-tested pieces (foliage) discard like the colour pass.
void psShadow(VertexOutput input)
{
    if (options.x > 0.0 && diffuseTexture.Sample(diffuseSampler, input.uv).a < options.x)
        discard;
    if (options.y > 0.5 && input.colour.a < 0.5)
        discard;                           // terrain holes (vertex alpha = flag 0) cast no shadow (UNVERIFIED)
}
