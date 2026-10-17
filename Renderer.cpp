#include "Renderer.h"
#include "GpuTexture.h"
#include "ShaderBytecode.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

struct RefractUniforms {   // shaders/refract.hlsl RefractUniforms
    Vec4f matDiffuse;
    Vec4f params;
    Vec4f cameraRight, cameraUp, cameraForward;
    Vec4f transform[4];
};

struct VertexUniforms {
    Mat4 viewProjection;
    Mat4 model;
    uint32_t skinned = 0, boneBase = 0, skinNormals = 0, aniso = 0;     // scene.hlsl `skin`
    Mat4 anisoNormal = identityMatrix();
    Vec4f eyePosition = Vec4f{0, 0, 0, 1};
    Vec4f anisoFog = Vec4f{0, 0, 0, 0};
};

// The lighting fields shared by both fragment shaders (shaders/lighting.hlsli), in cbuffer order.
struct LightingUniforms {
    Vec4f sunFront;
    Vec4f sunBack;
    Vec4f hemiTop;
    Vec4f hemiBottom;
    Vec4f fogColour;               // w: 1 = fog on
    Vec4f fogRange;                // x: start distance, y: density
    Vec4f cameraPosition;
    Vec4f localCount;              // x: number of placed lights
    Vec4f localPosition[SceneLighting::maxLocalLights];
    Vec4f localColour[SceneLighting::maxLocalLights];
    Vec4f localSpot[SceneLighting::maxLocalLights];
    Vec4f localSpotInner[SceneLighting::maxLocalLights];
    Vec4f localRotation[SceneLighting::maxLocalLights];
    Mat4 shadowMatrix;             // world -> sun shadow map clip space
    Vec4f shadowParams;            // x: 1 = sun shadows on, y: opacity, z: 1 / map size, w: softness
    Vec4f shadowExtra;             // xyz: camera forward, w: shadow depth units per metre
    Vec4f extraTowards[SceneLighting::maxExtraDirectional];   // xyz towards the light; w: 1 = in use
    Vec4f extraFront[SceneLighting::maxExtraDirectional];
    Vec4f extraBack[SceneLighting::maxExtraDirectional];
    Vec4f localShadowBase[SceneLighting::maxLocalLights];   // x first shadow view + 1, y view count, z shadow-light index
};

// A placed-light shadow slot as lighting.hlsli's LocalShadowSlot reads it from the fragment storage buffer (localShadowBuffer_). Not
// in the uniforms: SDL GPU's Vulkan backend takes at most 4 KiB per uniform push (MAX_UBO_SECTION_SIZE) and silently drops the rest -
// with 16 slots the fragment uniforms were ~4.8 KiB and every placed-light shadow vanished.
struct LocalShadowSlotGpu {
    Mat4 matrix;
    Vec4f view;
    Vec4f rect;
    Vec4f params;
    Vec4f bias;
    Vec4f camera;
};

struct SceneFragmentUniforms {
    Vec4f tint;
    Vec4f lightDirection;          // w: 1 = lit, 0 = unlit
    Vec4f options;                 // x: alpha test, z: additive, w: aniso additive
    Vec4f hue;                     // rgb added, weighted by the texture alpha; w: 1 = on
    LightingUniforms lighting;
    Vec4f specular;                // aniso additive: MatSpecular
    Vec4f layers;                  // aniso additive: x detail uses UV set 1, y decal uses UV set 1, z 1 = has detail, w 1 = has decal
    Vec4f material;                // x: 1 = normal map, y: 1 = gloss map, z: 1 = environment reflection (gloss.b), w: 1 = cut-out
                                   // by normal-map alpha (TRHair)
    Vec4f trFlags;                 // TrMaterial::flags, w bit 1 = active
    Vec4f trSpecular;
    Vec4f trEmissive;
    Vec4f trTransform0u, trTransform0v, trTransform1u, trTransform1v, trTransform2u, trTransform2v;
};

struct TerrainFragmentUniforms {
    Vec4f grid;                    // originX, originZ, 1 / spacing
    Vec4f gridSize;                // columns, rows, 1 / columns, 1 / rows
    Vec4f lightDirection;
    LightingUniforms lighting;
    Vec4f tilings[32];             // 128 layers
};

// SDL GPU's Vulkan backend accepts at most 4 KiB per uniform push (MAX_UBO_SECTION_SIZE); anything beyond is lost without an error.
static_assert(sizeof(SceneFragmentUniforms) <= 4096, "scene fragment uniforms exceed one uniform push");
static_assert(sizeof(TerrainFragmentUniforms) <= 4096, "terrain fragment uniforms exceed one uniform push");

LightingUniforms lightingUniforms(const SceneLighting& l, const Vec4f& cameraPosition, const Mat4& shadowMatrix, bool shadows, float mapSize) {
    LightingUniforms u;
    u.shadowMatrix = shadowMatrix;
    u.shadowParams = Vec4f{shadows ? 1.0f : 0.0f, l.shadowOpacity, 1.0f / mapSize, l.shadowSoftness};
    u.shadowExtra = Vec4f{l.cameraForward.x, l.cameraForward.y, l.cameraForward.z, 1.0f / Renderer::shadowDepth};
    u.sunFront = l.sunFront;
    u.sunBack = l.sunBack;
    for (int k = 0; k < SceneLighting::maxExtraDirectional; ++k) {
        const bool used = k < l.extraDirectionalCount;
        const tr::Vec3 t = used ? normalized(l.extraTowards[k]) : tr::Vec3{0, 1, 0};
        u.extraTowards[k] = Vec4f{t.x, t.y, t.z, used ? 1.0f : 0.0f};
        u.extraFront[k] = used ? l.extraFront[k] : Vec4f{0, 0, 0, 0};
        u.extraBack[k] = used ? l.extraBack[k] : Vec4f{0, 0, 0, 0};
    }
    for (int k = 0; k < SceneLighting::maxLocalLights; ++k) u.localShadowBase[k] = l.localShadowBase[k];
    u.hemiTop = l.hemiTop;
    u.hemiBottom = l.hemiBottom;
    u.fogColour = Vec4f{l.fogColour.x, l.fogColour.y, l.fogColour.z, l.fog ? 1.0f : 0.0f};
    u.fogRange = Vec4f{l.fogStart, l.fogDensity, l.showLight ? 1.0f : 0.0f, float(l.debugView)};
    u.cameraPosition = cameraPosition;
    u.localCount = Vec4f{float(l.localCount), 0, 0, 0};
    for (int i = 0; i < SceneLighting::maxLocalLights; ++i) {
        u.localPosition[i] = l.localPosition[i];
        u.localColour[i] = l.localColour[i];
        u.localSpot[i] = l.localSpot[i];
        u.localSpotInner[i] = l.localSpotInner[i];
        u.localRotation[i] = l.localRotation[i];
    }
    return u;
}

[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string(what) + ": " + SDL_GetError()); }

SDL_GPUSampler* createSampler(SDL_GPUDevice* device, bool linear, bool repeat) {
    SDL_GPUSamplerCreateInfo info = {};
    info.min_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    info.mag_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    info.mipmap_mode = linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    const SDL_GPUSamplerAddressMode mode = repeat ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_u = mode;
    info.address_mode_v = mode;
    info.address_mode_w = mode;
    info.max_lod = linear ? 1000.0f : 0.0f;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device, &info);
    if (!sampler) fail("SDL_CreateGPUSampler");
    return sampler;
}

}  // namespace

SDL_GPUDevice* createGpuDevice(SDL_GPUShaderFormat formats, bool debug) {
    // SDL_HINT_GPU_DRIVER takes precedence over the requested name inside SDL.
    if (SDL_GPUDevice* device = SDL_CreateGPUDevice(formats, debug, "vulkan")) return device;
    if (SDL_GetHint(SDL_HINT_GPU_DRIVER)) return nullptr;     // the user's choice failed; don't override it
    SDL_Log("Vulkan unavailable (%s); using SDL's default GPU driver", SDL_GetError());
    return SDL_CreateGPUDevice(formats, debug, nullptr);
}

Renderer::Renderer(SDL_GPUDevice* device) : device_(device) {
    const char* driver = SDL_GetGPUDeviceDriver(device_);
    splitCommandBuffers_ = driver && std::strcmp(driver, "direct3d12") == 0;
    sceneVertex_ = createShader(sceneVertexDxil, sizeof sceneVertexDxil, sceneVertexSpirv, sizeof sceneVertexSpirv, "vsMain",
                                SDL_GPU_SHADERSTAGE_VERTEX, 0, 2);       // storage buffers: skin vertices, bone rows
    sceneFragment_ = createShader(sceneFragmentDxil, sizeof sceneFragmentDxil, sceneFragmentSpirv, sizeof sceneFragmentSpirv, "psMain",
                                  SDL_GPU_SHADERSTAGE_FRAGMENT, 12, 1);   // storage buffer: placed-light shadow slots
    terrainVertex_ = createShader(terrainVertexDxil, sizeof terrainVertexDxil, terrainVertexSpirv, sizeof terrainVertexSpirv, "vsMain",
                                  SDL_GPU_SHADERSTAGE_VERTEX, 0);
    terrainFragment_ = createShader(terrainFragmentDxil, sizeof terrainFragmentDxil, terrainFragmentSpirv, sizeof terrainFragmentSpirv,
                                    "psMain", SDL_GPU_SHADERSTAGE_FRAGMENT, 7, 1);
    shadowFragment_ = createShader(shadowFragmentDxil, sizeof shadowFragmentDxil, shadowFragmentSpirv, sizeof shadowFragmentSpirv, "psShadow",
                                   SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    refractFragment_ = createShader(refractFragmentDxil, sizeof refractFragmentDxil, refractFragmentSpirv, sizeof refractFragmentSpirv, "psRefract",
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, 5);
    if (!SDL_GPUTextureSupportsFormat(device_, depthFormat_, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        depthFormat_ = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    trianglePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    culledTrianglePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 0, true);
    linePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_LINELIST);
    terrainPipeline_ = createPipeline(terrainVertex_, terrainFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    culledTerrainPipeline_ = createPipeline(terrainVertex_, terrainFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 0, true);
    translucentPipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 1);
    additivePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 2);
    // Diagnostic: RASA_FLIPBLENDCULL=1 culls the other faces of blended pieces (winding checks against in-game shots).
    const char* flipBlend = SDL_getenv("RASA_FLIPBLENDCULL");
    const bool flip = flipBlend && *flipBlend && *flipBlend != '0';
    culledAdditivePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 2, true, flip);
    culledTranslucentPipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 1, true, flip);
    subtractivePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 3);
    refractPipeline_ = createPipeline(sceneVertex_, refractFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 4, false, false, false);   // no depth target
    linearSampler_ = createSampler(device_, true, true);
    pointSampler_ = createSampler(device_, false, false);
    clampSampler_ = createSampler(device_, true, false);
    {
        SDL_GPUSamplerCreateInfo s = {};
        s.min_filter = SDL_GPU_FILTER_LINEAR;
        s.mag_filter = SDL_GPU_FILTER_LINEAR;
        s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        s.enable_compare = true;
        s.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        shadowSampler_ = SDL_CreateGPUSampler(device_, &s);
        if (!shadowSampler_) fail("SDL_CreateGPUSampler (shadow)");
        SDL_GPUTextureCreateInfo t = {};
        t.type = SDL_GPU_TEXTURETYPE_2D;
        t.width = t.height = shadowSize;
        t.layer_count_or_depth = 1;
        t.num_levels = 1;
        t.sample_count = SDL_GPU_SAMPLECOUNT_1;
        t.format = depthFormat_;
        t.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        shadowMap_ = SDL_CreateGPUTexture(device_, &t);
        if (!shadowMap_) fail("SDL_CreateGPUTexture (shadow map)");
        // Depth only, no colour target; alpha-tested foliage casts dappled shadows; slope-scaled bias against acne.
        SDL_GPUVertexBufferDescription buffer = {};
        buffer.slot = 0;
        buffer.pitch = sizeof(MeshVertex);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attributes[8] = {};
        const uint32_t offsets[8] = {uint32_t(offsetof(MeshVertex, positionX)), uint32_t(offsetof(MeshVertex, normalX)), uint32_t(offsetof(MeshVertex, colour)),
                                     uint32_t(offsetof(MeshVertex, texU)), uint32_t(offsetof(MeshVertex, texU1)), uint32_t(offsetof(MeshVertex, tangentX)),
                                     uint32_t(offsetof(MeshVertex, binormalX)), uint32_t(offsetof(MeshVertex, texU2))};
        const SDL_GPUVertexElementFormat formats[8] = {SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
                                                       SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
                                                       SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2};
        for (uint32_t k = 0; k < 8; ++k) {
            attributes[k].location = k;
            attributes[k].format = formats[k];
            attributes[k].offset = offsets[k];
        }
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = sceneVertex_;
        info.fragment_shader = shadowFragment_;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 8;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.rasterizer_state.enable_depth_bias = true;
        info.rasterizer_state.depth_bias_constant_factor = 2.0f;
        info.rasterizer_state.depth_bias_slope_factor = 2.0f;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.target_info.num_color_targets = 0;
        info.target_info.depth_stencil_format = depthFormat_;
        info.target_info.has_depth_stencil_target = true;
        shadowPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (!shadowPipeline_) fail("SDL_CreateGPUGraphicsPipeline (shadow)");
        // Placed-light shadow maps: the same layout without the depth bias (the game's TRSpotLight shadow test biases in the shader),
        // and with depth CLIPPING as Direct3D 9 always does - SDL GPU's default (enable_depth_clip false) clamps instead, which put
        // geometry behind a light's near plane at depth 0: Bootcamp's Light_29 (orthographic) was shadowed wholesale by the glowstick
        // pole 0.4 m behind it.
        info.rasterizer_state.enable_depth_bias = false;
        info.rasterizer_state.depth_bias_constant_factor = 0.0f;
        info.rasterizer_state.depth_bias_slope_factor = 0.0f;
        info.rasterizer_state.enable_depth_clip = true;
        localShadowTwoSidedPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (!localShadowTwoSidedPipeline_) fail("SDL_CreateGPUGraphicsPipeline (local shadow)");
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
        localShadowPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (!localShadowPipeline_) fail("SDL_CreateGPUGraphicsPipeline (local shadow, culled)");
        t.width = t.height = shadowAtlasSize;
        shadowAtlas_ = SDL_CreateGPUTexture(device_, &t);
        if (!shadowAtlas_) fail("SDL_CreateGPUTexture (shadow atlas)");
        setCookieMaps({}, 4);
    }
    {
        // Water (shaders/water.hlsl): its own vertex shader with UV set 3; opaque, no culling (TRWaterReflectRefract sets
        // CULLMODE NONE), no depth target - the fragment shader tests against the sampled scene depth.
        waterVertex_ = createShader(waterVertexDxil, sizeof waterVertexDxil, waterVertexSpirv, sizeof waterVertexSpirv, "vsWater",
                                    SDL_GPU_SHADERSTAGE_VERTEX, 0);
        waterFragment_ = createShader(waterFragmentDxil, sizeof waterFragmentDxil, waterFragmentSpirv, sizeof waterFragmentSpirv, "psWater",
                                      SDL_GPU_SHADERSTAGE_FRAGMENT, 7);
        SDL_GPUVertexBufferDescription buffer = {};
        buffer.slot = 0;
        buffer.pitch = sizeof(MeshVertex);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attributes[9] = {};
        const uint32_t offsets[9] = {uint32_t(offsetof(MeshVertex, positionX)), uint32_t(offsetof(MeshVertex, normalX)), uint32_t(offsetof(MeshVertex, colour)),
                                     uint32_t(offsetof(MeshVertex, texU)), uint32_t(offsetof(MeshVertex, texU1)), uint32_t(offsetof(MeshVertex, tangentX)),
                                     uint32_t(offsetof(MeshVertex, binormalX)), uint32_t(offsetof(MeshVertex, texU2)), uint32_t(offsetof(MeshVertex, texU3))};
        const SDL_GPUVertexElementFormat formats[9] = {SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
                                                       SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
                                                       SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2};
        for (uint32_t k = 0; k < 9; ++k) {
            attributes[k].location = k;
            attributes[k].format = formats[k];
            attributes[k].offset = offsets[k];
        }
        SDL_GPUColorTargetDescription target = {};
        target.format = colourFormat_;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = waterVertex_;
        info.fragment_shader = waterFragment_;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 9;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = false;
        waterPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (!waterPipeline_) fail("SDL_CreateGPUGraphicsPipeline (water)");
        SDL_GPUSamplerCreateInfo s = {};
        s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
        s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        s.address_mode_u = s.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        s.max_lod = 1000.0f;
        volumeSampler_ = SDL_CreateGPUSampler(device_, &s);
        if (!volumeSampler_) fail("SDL_CreateGPUSampler (volume)");
    }
    setGlobeMaps({}, 4);
    const uint32_t white = 0xFFFFFFFF;
    white_ = uploadRgbaTexture(device_, &white, 1, 1);
    const uint32_t black = 0xFF000000;
    black_ = uploadRgbaTexture(device_, &black, 1, 1);
    {
        SDL_GPUTextureCreateInfo t = {};
        t.type = SDL_GPU_TEXTURETYPE_3D;
        t.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        t.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        t.width = t.height = 1;
        t.layer_count_or_depth = 1;
        t.num_levels = 1;
        whiteVolume_ = SDL_CreateGPUTexture(device_, &t);
        if (!whiteVolume_) fail("SDL_CreateGPUTexture (white volume)");
        SDL_GPUTransferBufferCreateInfo ti = {};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = 4;
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &ti);
        void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
        std::memcpy(mapped, &white, 4);
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureTransferInfo source = {};
        source.transfer_buffer = transfer;
        SDL_GPUTextureRegion destination = {};
        destination.texture = whiteVolume_;
        destination.w = destination.h = destination.d = 1;
        SDL_UploadToGPUTexture(copy, &source, &destination, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(commands);
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
    }
    blackCube_ = uploadRgbaCube(device_, std::vector<std::vector<tr::Rgba8>>(6, std::vector<tr::Rgba8>(1, tr::Rgba8{0, 0, 0, 255})), 1);
    viewProjection_ = identityMatrix();
}

Renderer::~Renderer() {
    if (colour_) SDL_ReleaseGPUTexture(device_, colour_);
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
    if (white_) SDL_ReleaseGPUTexture(device_, white_);
    if (black_) SDL_ReleaseGPUTexture(device_, black_);
    if (blackCube_) SDL_ReleaseGPUTexture(device_, blackCube_);
    if (whiteVolume_) SDL_ReleaseGPUTexture(device_, whiteVolume_);
    if (linearSampler_) SDL_ReleaseGPUSampler(device_, linearSampler_);
    if (pointSampler_) SDL_ReleaseGPUSampler(device_, pointSampler_);
    if (clampSampler_) SDL_ReleaseGPUSampler(device_, clampSampler_);
    if (shadowSampler_) SDL_ReleaseGPUSampler(device_, shadowSampler_);
    if (volumeSampler_) SDL_ReleaseGPUSampler(device_, volumeSampler_);
    if (waterPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, waterPipeline_);
    if (waterVertex_) SDL_ReleaseGPUShader(device_, waterVertex_);
    if (waterFragment_) SDL_ReleaseGPUShader(device_, waterFragment_);
    if (shadowMap_) SDL_ReleaseGPUTexture(device_, shadowMap_);
    if (globes_) SDL_ReleaseGPUTexture(device_, globes_);
    if (shadowPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, shadowPipeline_);
    if (shadowFragment_) SDL_ReleaseGPUShader(device_, shadowFragment_);
    if (localShadowBuffer_) SDL_ReleaseGPUBuffer(device_, localShadowBuffer_);
    if (localShadowTransfer_) SDL_ReleaseGPUTransferBuffer(device_, localShadowTransfer_);
    if (boneBuffer_) SDL_ReleaseGPUBuffer(device_, boneBuffer_);
    if (boneTransfer_) SDL_ReleaseGPUTransferBuffer(device_, boneTransfer_);
    if (skinDummy_) SDL_ReleaseGPUBuffer(device_, skinDummy_);
    if (particleBuffer_) SDL_ReleaseGPUBuffer(device_, particleBuffer_);
    if (particleTransfer_) SDL_ReleaseGPUTransferBuffer(device_, particleTransfer_);
    for (SDL_GPUGraphicsPipeline* p : {trianglePipeline_, culledTrianglePipeline_, linePipeline_, terrainPipeline_, culledTerrainPipeline_, translucentPipeline_, additivePipeline_, culledAdditivePipeline_, culledTranslucentPipeline_, subtractivePipeline_, refractPipeline_})
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    if (frameCopy_) SDL_ReleaseGPUTexture(device_, frameCopy_);
    for (SDL_GPUShader* s : {sceneVertex_, sceneFragment_, terrainVertex_, terrainFragment_, refractFragment_})
        if (s) SDL_ReleaseGPUShader(device_, s);
    if (shadowAtlas_) SDL_ReleaseGPUTexture(device_, shadowAtlas_);
    if (cookies_) SDL_ReleaseGPUTexture(device_, cookies_);
    if (localShadowPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, localShadowPipeline_);
    if (localShadowTwoSidedPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, localShadowTwoSidedPipeline_);
    for (SDL_GPUGraphicsPipeline* p : {pvsDepthPipeline_, pvsDepthTwoSidedPipeline_})
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    for (SDL_GPUComputePipeline* p : {pvsCountPipeline_, pvsMarkPipeline_})
        if (p) SDL_ReleaseGPUComputePipeline(device_, p);
    for (SDL_GPUBuffer* b : {pvsNodes_, pvsCounts_, pvsBits_})
        if (b) SDL_ReleaseGPUBuffer(device_, b);
    if (pvsDownload_) SDL_ReleaseGPUTransferBuffer(device_, pvsDownload_);
}

SDL_GPUShader* Renderer::createShader(const unsigned char* dxil, size_t dxilSize, const unsigned char* spirv, size_t spirvSize,
                                      const char* entryPoint, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t storageBuffers) {
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
    SDL_GPUShaderCreateInfo info = {};
    if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        info.code = dxil;
        info.code_size = dxilSize;
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
    } else if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.code = spirv;
        info.code_size = spirvSize;
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    } else {
        throw std::runtime_error("the GPU device accepts neither DXIL nor SPIR-V shaders");
    }
    info.entrypoint = entryPoint;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_storage_buffers = storageBuffers;
    info.num_uniform_buffers = 1;
    SDL_GPUShader* shader = SDL_CreateGPUShader(device_, &info);
    if (!shader) fail("SDL_CreateGPUShader");
    return shader;
}

SDL_GPUGraphicsPipeline* Renderer::createPipeline(SDL_GPUShader* vertexShader, SDL_GPUShader* fragmentShader, SDL_GPUPrimitiveType primitive,
                                                  int blend, bool cullBack, bool cullFront, bool depthTarget) {
    SDL_GPUVertexBufferDescription buffer = {};
    buffer.slot = 0;
    buffer.pitch = sizeof(MeshVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[8] = {};
    attributes[0].location = 0;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[0].offset = offsetof(MeshVertex, positionX);
    attributes[1].location = 1;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[1].offset = offsetof(MeshVertex, normalX);
    attributes[2].location = 2;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    attributes[2].offset = offsetof(MeshVertex, colour);
    attributes[3].location = 3;
    attributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[3].offset = offsetof(MeshVertex, texU);
    attributes[4].location = 4;
    attributes[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[4].offset = offsetof(MeshVertex, texU1);
    attributes[5].location = 5;
    attributes[5].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[5].offset = offsetof(MeshVertex, tangentX);
    attributes[6].location = 6;
    attributes[6].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[6].offset = offsetof(MeshVertex, binormalX);

    SDL_GPUColorTargetDescription target = {};
    target.format = colourFormat_;
    if (blend != 0) {                                  // 1: alpha blend, 2: additive (one, one), 3: reverse subtract (one, one),
                                                       // 4: alpha blend with depth writes (refraction)
        target.blend_state.enable_blend = true;
        const bool added = blend == 2 || blend == 3;
        target.blend_state.src_color_blendfactor = added ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor = added ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.color_blend_op = blend == 3 ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vertexShader;
    info.fragment_shader = fragmentShader;
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    attributes[7].location = 7;
    attributes[7].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[7].offset = offsetof(MeshVertex, texU2);
    info.vertex_input_state.num_vertex_attributes = 8;
    info.primitive_type = primitive;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = cullFront ? SDL_GPU_CULLMODE_FRONT : cullBack ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.enable_depth_test = depthTarget;
    info.depth_stencil_state.enable_depth_write = depthTarget && (blend == 0 || blend == 4);
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = depthFormat_;
    info.target_info.has_depth_stencil_target = depthTarget;
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    if (!pipeline) fail("SDL_CreateGPUGraphicsPipeline");
    return pipeline;
}

void Renderer::resize(uint32_t width, uint32_t height) {
    width = width < 1 ? 1 : width;
    height = height < 1 ? 1 : height;
    if (colour_ && width == width_ && height == height_) return;
    if (colour_) SDL_ReleaseGPUTexture(device_, colour_);       // SDL defers destruction until the GPU is done with it
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
    if (frameCopy_) SDL_ReleaseGPUTexture(device_, frameCopy_);
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.format = colourFormat_;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    colour_ = SDL_CreateGPUTexture(device_, &info);
    if (!colour_) fail("SDL_CreateGPUTexture (colour target)");
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    frameCopy_ = SDL_CreateGPUTexture(device_, &info);
    if (!frameCopy_) fail("SDL_CreateGPUTexture (frame copy)");
    info.format = depthFormat_;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;     // sampled by the water pass
    depth_ = SDL_CreateGPUTexture(device_, &info);
    if (!depth_) fail("SDL_CreateGPUTexture (depth target)");
    width_ = width;
    height_ = height;
}

GpuMesh Renderer::createMesh(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices, bool lines) {
    GpuMesh mesh;
    mesh.lines = lines;
    mesh.indexCount = uint32_t(indices.size());
    if (vertices.empty() || indices.empty()) return mesh;
    const uint32_t vertexBytes = uint32_t(vertices.size() * sizeof(MeshVertex));
    const uint32_t indexBytes = uint32_t(indices.size() * sizeof(uint32_t));

    SDL_GPUBufferCreateInfo bufferInfo = {};
    bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bufferInfo.size = vertexBytes;
    mesh.vertices = SDL_CreateGPUBuffer(device_, &bufferInfo);
    bufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    bufferInfo.size = indexBytes;
    mesh.indices = SDL_CreateGPUBuffer(device_, &bufferInfo);
    if (!mesh.vertices || !mesh.indices) {
        destroyMesh(mesh);
        fail("SDL_CreateGPUBuffer");
    }

    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = vertexBytes + indexBytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!transfer) {
        destroyMesh(mesh);
        fail("SDL_CreateGPUTransferBuffer");
    }
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    std::memcpy(mapped, vertices.data(), vertexBytes);
    std::memcpy(mapped + vertexBytes, indices.data(), indexBytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source = {};
    source.transfer_buffer = transfer;
    SDL_GPUBufferRegion destination = {};
    destination.buffer = mesh.vertices;
    destination.size = vertexBytes;
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    source.offset = vertexBytes;
    destination.buffer = mesh.indices;
    destination.size = indexBytes;
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return mesh;
}

void Renderer::destroyMesh(GpuMesh& mesh) {
    if (mesh.vertices) SDL_ReleaseGPUBuffer(device_, mesh.vertices);
    if (mesh.indices) SDL_ReleaseGPUBuffer(device_, mesh.indices);
    mesh = GpuMesh();
}

void Renderer::beginFrame(const Mat4& viewProjection, const tr::Vec3& cameraPosition, const SceneLighting& lighting) {
    viewProjection_ = viewProjection;
    localCasters_.clear();
    towardsLight_ = Vec4f{lighting.towardsLight.x, lighting.towardsLight.y, lighting.towardsLight.z, 1.0f};
    clearColour_ = lighting.clearColour;
    lighting_ = lighting;
    // Sun shadow map: orthographic from the sun, centred ahead of the camera as the game's (see shadowExtent).
    shadowsActive_ = lighting.sunShadows && lighting.towardsLight.y > 0.02f;
    if (shadowsActive_) {
        const tr::Vec3 toward = normalized(lighting.towardsLight);
        const tr::Vec3 forward = scaled(toward, -1.0f);
        const tr::Vec3 right = normalized(cross(std::fabs(forward.y) > 0.99f ? tr::Vec3{0, 0, 1} : tr::Vec3{0, 1, 0}, forward));
        const tr::Vec3 up = cross(forward, right);
        const tr::Vec3 level{lighting.cameraForward.x, 0, lighting.cameraForward.z};
        const float levelLength = std::sqrt(level.x * level.x + level.z * level.z);
        const tr::Vec3 ahead = levelLength > 1e-4f ? add(cameraPosition, scaled(level, shadowAhead / levelLength)) : cameraPosition;
        const float cx = dot(ahead, right), cy = dot(ahead, up);
        const float cz = dot(cameraPosition, forward);
        const tr::Vec3 centre = add(add(scaled(right, cx), scaled(up, cy)), scaled(forward, cz));
        const Mat4 view = viewLH(subtract(centre, scaled(forward, shadowDepth * 0.5f)), right, up, forward);
        Mat4 ortho = identityMatrix();
        ortho.row0 = {1.0f / shadowExtent, 0, 0, 0};
        ortho.row1 = {0, 1.0f / shadowExtent, 0, 0};
        ortho.row2 = {0, 0, 1.0f / shadowDepth, 0};
        shadowMatrix_ = multiply(ortho, view);
    } else {
        shadowMatrix_ = identityMatrix();
    }
    cameraPosition_ = Vec4f{cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f};
    draws_.clear();
    translucentDraws_.clear();
    additiveDraws_.clear();
    skyDraws_.clear();
    refractDraws_.clear();
    waterDraws_.clear();
    bones_.clear();
    currentSkin_ = nullptr;
    particleVertices_.clear();
    particleBatches_.clear();
}

void Renderer::draw(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, bool lit, SDL_GPUTexture* texture, bool alphaTest, const Vec4f& hue,
                    SDL_GPUTexture* glow, bool twoSided, float alphaReference, SDL_GPUTexture* normalMap, SDL_GPUTexture* glossMap,
                    bool environment, const TrMaterial* trMaterial, bool hairCutout, SDL_GPUTexture* palDecal) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, tint, lit, texture ? texture : white_, alphaTest, nullptr, false, hue, glow ? glow : black_};
    item.twoSided = twoSided;
    item.alphaReference = alphaReference;
    item.normalMap = normalMap;
    item.glossMap = glossMap;
    item.environment = environment;
    item.trHairCutout = hairCutout;
    item.castsShadow = shadowCasting_;
    if (trMaterial && trMaterial->active) {
        item.tr = *trMaterial;
        item.detail = trMaterial->slot1;         // slot 1 / 2 maps use the detail (t2) and decal (t3) bindings
        item.decal = trMaterial->slot2;
    } else if (palDecal) {
        item.decal = palDecal;                   // Pal*Dec* decal map (t3, UV set 2): layers.w = 2
        item.layers.w = 2.0f;
    }
    item.skinVertices = currentSkin_;
    item.boneBase = currentBoneBase_;
    item.skinNormals = currentSkinNormals_;
    draws_.push_back(item);
}

void Renderer::drawRefract(const GpuMesh& mesh, const Mat4& model, const RefractMaterial& material) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    refractDraws_.push_back(RefractItem{&mesh, model, material, currentSkin_, currentBoneBase_, currentSkinNormals_});
}

void Renderer::drawTranslucent(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture, SDL_GPUTexture* detail,
                               int detailSet, bool cullBack, const TrMaterial* uvTransforms) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, tint, true, texture ? texture : white_, false, nullptr, true, Vec4f{0, 0, 0, 0}, black_};
    item.detail = detail;
    item.layers = Vec4f{float(detailSet), 0, detail ? 1.0f : 0.0f, 0};
    if (uvTransforms) {
        item.forwardUv = true;                   // tr.active stays false
        item.tr.transform0u = uvTransforms->transform0u; item.tr.transform0v = uvTransforms->transform0v;
        item.tr.transform1u = uvTransforms->transform1u; item.tr.transform1v = uvTransforms->transform1v;
        item.tr.transform2u = uvTransforms->transform2u; item.tr.transform2v = uvTransforms->transform2v;
    }
    item.cullBlended = cullBack;
    item.skinVertices = currentSkin_;
    item.boneBase = currentBoneBase_;
    item.skinNormals = currentSkinNormals_;
    translucentDraws_.push_back(item);
}

void Renderer::drawFadingForward(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture, const TrMaterial& material,
                                 bool cullBack) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, tint, true, texture ? texture : white_, false, nullptr, true, Vec4f{0, 0, 0, 0}, black_};
    item.tr = material;
    item.tr.flags.w = float(int(material.flags.w) | 0x10);
    item.detail = material.slot1;                // as draw(): slot 1 / 2 maps on the detail (t2) and decal (t3) bindings
    item.decal = material.slot2;
    item.cullBlended = cullBack;
    item.skinVertices = currentSkin_;
    item.boneBase = currentBoneBase_;
    item.skinNormals = currentSkinNormals_;
    item.castsShadow = false;
    translucentDraws_.push_back(item);
}

void Renderer::drawAdditive(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture, bool lit, SDL_GPUTexture* detail,
                            int detailSet, bool cullBack, const TrMaterial* uvTransforms) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, tint, false, texture ? texture : white_, false, nullptr, true, Vec4f{0, 0, 0, 0}, black_};
    item.additive = true;
    item.forwardLit = lit;
    item.cullBlended = cullBack;
    item.detail = detail;
    item.layers = Vec4f{float(detailSet), 0, detail ? 1.0f : 0.0f, 0};
    if (uvTransforms) {
        item.forwardUv = true;                   // tr.active stays false
        item.tr.transform0u = uvTransforms->transform0u; item.tr.transform0v = uvTransforms->transform0v;
        item.tr.transform1u = uvTransforms->transform1u; item.tr.transform1v = uvTransforms->transform1v;
        item.tr.transform2u = uvTransforms->transform2u; item.tr.transform2v = uvTransforms->transform2v;
    }
    item.skinVertices = currentSkin_;
    item.boneBase = currentBoneBase_;
    item.skinNormals = currentSkinNormals_;
    additiveDraws_.push_back(item);
}

void Renderer::drawSkyEmissive(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture, bool cullBack) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, tint, false, texture ? texture : white_, false, nullptr, true, Vec4f{0, 0, 0, 0}, black_};
    item.sky = true;                             // blended SRCALPHA / INVSRCALPHA, no depth writes (the translucent pipelines)
    item.cullBlended = cullBack;
    item.skinVertices = currentSkin_;            // skinned layers (e.g. the Bane dropships, animated)
    item.boneBase = currentBoneBase_;
    item.skinNormals = currentSkinNormals_;
    item.castsShadow = false;
    skyDraws_.push_back(item);
}

void Renderer::drawAnisoAdditive(const GpuMesh& mesh, const Mat4& model, const AnisoMaterial& material, bool cullBack, bool sky) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, material.diffuseAlpha, false, material.diffuse ? material.diffuse : white_, false, nullptr, true, Vec4f{0, 0, 0, 0},
                  black_};
    item.additive = true;
    item.detail = material.detail;
    item.decal = material.decal;
    item.light = material.light;
    item.specular = material.specular;
    item.aniso = true;
    item.anisoPlain = material.plain;
    item.hue = material.hue;
    item.cullBlended = cullBack;
    item.layers = Vec4f{0, 0, material.detail ? 1.0f : 0.0f, material.decal ? 1.0f : 0.0f};
    item.anisoNormal = material.normalMatrix;
    item.tr.transform0u = material.transform0u; item.tr.transform0v = material.transform0v;     // tr.active stays false
    item.tr.transform1u = material.transform1u; item.tr.transform1v = material.transform1v;
    item.tr.transform2u = material.transform2u; item.tr.transform2v = material.transform2v;
    item.skinVertices = currentSkin_;
    item.boneBase = currentBoneBase_;
    item.skinNormals = currentSkinNormals_;
    item.sky = sky;
    (sky ? skyDraws_ : additiveDraws_).push_back(item);
}

void Renderer::drawWater(const GpuMesh& mesh, const Mat4& model, const WaterMaterial& material) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    waterDraws_.push_back(WaterItem{&mesh, model, material});
}

void Renderer::drawParticles(const std::vector<MeshVertex>& vertices, SDL_GPUTexture* texture, uint32_t blendType, bool fog) {
    if (vertices.empty()) return;
    ParticleBatch batch;
    batch.first = uint32_t(particleVertices_.size());
    batch.count = uint32_t(vertices.size());
    batch.texture = texture ? texture : white_;
    batch.blendType = blendType;
    batch.fog = fog;
    // Consecutive quads of the same texture, blend and fog share one draw.
    if (!particleBatches_.empty() && particleBatches_.back().texture == batch.texture && particleBatches_.back().blendType == blendType &&
        particleBatches_.back().fog == fog)
        particleBatches_.back().count += batch.count;
    else
        particleBatches_.push_back(batch);
    particleVertices_.insert(particleVertices_.end(), vertices.begin(), vertices.end());
}

SDL_GPUBuffer* Renderer::createSkinBuffer(const std::vector<SkinVertex>& vertices) {
    if (vertices.empty()) return nullptr;
    const uint32_t bytes = uint32_t(vertices.size() * sizeof(SkinVertex));
    SDL_GPUBufferCreateInfo info = {};
    info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    info.size = bytes;
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &info);
    if (!buffer) fail("SDL_CreateGPUBuffer (skin)");
    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer (skin)");
    std::memcpy(SDL_MapGPUTransferBuffer(device_, transfer, false), vertices.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source = {};
    source.transfer_buffer = transfer;
    SDL_GPUBufferRegion destination = {};
    destination.buffer = buffer;
    destination.size = bytes;
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return buffer;
}

void Renderer::destroyBuffer(SDL_GPUBuffer*& buffer) {
    if (buffer) SDL_ReleaseGPUBuffer(device_, buffer);
    buffer = nullptr;
}

uint32_t Renderer::addBones(const std::vector<Mat4>& skinMatrices) {
    const uint32_t base = uint32_t(bones_.size());
    for (const Mat4& m : skinMatrices) {
        bones_.push_back(m.row0);
        bones_.push_back(m.row1);
        bones_.push_back(m.row2);
    }
    return base;
}

void Renderer::setSkin(SDL_GPUBuffer* skinVertices, uint32_t boneBase, bool skinNormals) {
    currentSkin_ = skinVertices;
    currentBoneBase_ = boneBase;
    currentSkinNormals_ = skinNormals;
}

// The frame's bone rows into boneBuffer_ (grown as needed; always at least one row so it can be bound).
void Renderer::uploadBones(SDL_GPUCommandBuffer* commands) {
    const uint32_t rows = std::max<uint32_t>(uint32_t(bones_.size()), 1);
    if (rows > boneCapacity_ || !boneBuffer_) {
        if (boneBuffer_) SDL_ReleaseGPUBuffer(device_, boneBuffer_);
        if (boneTransfer_) SDL_ReleaseGPUTransferBuffer(device_, boneTransfer_);
        boneCapacity_ = std::max<uint32_t>(rows + rows / 2, 3 * 256);
        SDL_GPUBufferCreateInfo info = {};
        info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        info.size = boneCapacity_ * uint32_t(sizeof(Vec4f));
        boneBuffer_ = SDL_CreateGPUBuffer(device_, &info);
        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = info.size;
        boneTransfer_ = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
        if (!boneBuffer_ || !boneTransfer_) fail("bone buffers");
    }
    if (!skinDummy_) {
        SDL_GPUBufferCreateInfo info = {};
        info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        info.size = uint32_t(sizeof(SkinVertex));
        skinDummy_ = SDL_CreateGPUBuffer(device_, &info);
        if (!skinDummy_) fail("skin dummy buffer");
    }
    if (bones_.empty()) return;
    const uint32_t bytes = uint32_t(bones_.size() * sizeof(Vec4f));
    std::memcpy(SDL_MapGPUTransferBuffer(device_, boneTransfer_, true), bones_.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, boneTransfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source = {};
    source.transfer_buffer = boneTransfer_;
    SDL_GPUBufferRegion destination = {};
    destination.buffer = boneBuffer_;
    destination.size = bytes;
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
}

// The frame's placed-light shadow slots into localShadowBuffer_ (lighting.hlsli LocalShadowSlot), read by the scene and terrain
// fragment shaders.
void Renderer::uploadLocalShadows(SDL_GPUCommandBuffer* commands) {
    const uint32_t bytes = uint32_t(SceneLighting::maxLocalShadows * sizeof(LocalShadowSlotGpu));
    if (!localShadowBuffer_) {
        SDL_GPUBufferCreateInfo info = {};
        info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        info.size = bytes;
        localShadowBuffer_ = SDL_CreateGPUBuffer(device_, &info);
        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = bytes;
        localShadowTransfer_ = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
        if (!localShadowBuffer_ || !localShadowTransfer_) fail("local shadow buffers");
    }
    LocalShadowSlotGpu* slots = static_cast<LocalShadowSlotGpu*>(SDL_MapGPUTransferBuffer(device_, localShadowTransfer_, true));
    for (int k = 0; k < SceneLighting::maxLocalShadows; ++k)
        slots[k] = LocalShadowSlotGpu{lighting_.localShadowMatrix[k], lighting_.localShadowView[k], lighting_.localShadowRect[k],
                                      lighting_.localShadowParams[k], lighting_.localShadowBias[k], lighting_.localShadowCamera[k]};
    SDL_UnmapGPUTransferBuffer(device_, localShadowTransfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source = {};
    source.transfer_buffer = localShadowTransfer_;
    SDL_GPUBufferRegion destination = {};
    destination.buffer = localShadowBuffer_;
    destination.size = bytes;
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
}

// Every draw through the scene vertex shader binds its two storage buffers (the dummy skin when not skinned).
void Renderer::bindSkin(SDL_GPURenderPass* pass, SDL_GPUBuffer* skinVertices) {
    SDL_GPUBuffer* buffers[2] = {skinVertices ? skinVertices : skinDummy_, boneBuffer_};
    SDL_BindGPUVertexStorageBuffers(pass, 0, buffers, 2);
}

void Renderer::uploadParticles(SDL_GPUCommandBuffer* commands) {
    if (particleVertices_.empty()) return;
    const uint32_t count = uint32_t(particleVertices_.size());
    if (count > particleCapacity_) {
        if (particleBuffer_) SDL_ReleaseGPUBuffer(device_, particleBuffer_);
        if (particleTransfer_) SDL_ReleaseGPUTransferBuffer(device_, particleTransfer_);
        particleCapacity_ = std::max<uint32_t>(count + count / 2, 6 * 4096);
        SDL_GPUBufferCreateInfo bufferInfo = {};
        bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bufferInfo.size = particleCapacity_ * uint32_t(sizeof(MeshVertex));
        particleBuffer_ = SDL_CreateGPUBuffer(device_, &bufferInfo);
        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = bufferInfo.size;
        particleTransfer_ = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
        if (!particleBuffer_ || !particleTransfer_) fail("particle buffers");
    }
    const uint32_t bytes = count * uint32_t(sizeof(MeshVertex));
    void* mapped = SDL_MapGPUTransferBuffer(device_, particleTransfer_, true);     // cycle: the previous upload may still be in flight
    std::memcpy(mapped, particleVertices_.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, particleTransfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source = {};
    source.transfer_buffer = particleTransfer_;
    SDL_GPUBufferRegion destination = {};
    destination.buffer = particleBuffer_;
    destination.size = bytes;
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
}

void Renderer::recordParticles(SDL_GPUCommandBuffer*& commands, SDL_GPURenderPass*& pass) {
    if (particleBatches_.empty() || !particleBuffer_) return;
    SDL_GPUGraphicsPipeline* bound = nullptr;
    SDL_GPUBufferBinding vertexBinding = {particleBuffer_, 0};
    const VertexUniforms vertexUniforms{viewProjection_, identityMatrix()};
    for (const ParticleBatch& b : particleBatches_) {
        if (reserveDescriptors(commands, pass, 12, 3)) bound = nullptr;
        SDL_GPUGraphicsPipeline* pipeline = b.blendType == 0 ? additivePipeline_ : b.blendType == 3 ? subtractivePipeline_ : translucentPipeline_;
        if (pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
            bindSkin(pass, nullptr);
            bound = pipeline;
        }
        SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof vertexUniforms);
        const SDL_GPUTextureSamplerBinding samplers[12] = {{b.texture, linearSampler_}, {black_, linearSampler_}, {black_, linearSampler_},
                                                           {black_, linearSampler_}, {white_, clampSampler_}, {shadowMap_, shadowSampler_},
                                                           {globes_, clampSampler_}, {black_, linearSampler_}, {black_, linearSampler_},
                                                           {blackCube_, clampSampler_}, {shadowAtlas_, pointSampler_}, {cookies_, clampSampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 12);
        SDL_BindGPUFragmentStorageBuffers(pass, 0, &localShadowBuffer_, 1);
        // Additive and subtractive use the additive path (rgb x alpha, faded by fog); alpha-blended ones options.y.
        const bool added = b.blendType == 0 || b.blendType == 3;
        SceneFragmentUniforms u{Vec4f{1, 1, 1, 1}, towardsLight_, Vec4f{0, added ? 0.0f : 1.0f, added ? 1.0f : 0.0f, 0}, Vec4f{0, 0, 0, 0},
                                lightingUniforms(lighting_, cameraPosition_, shadowMatrix_, false, float(shadowSize))};
        u.lightDirection.w = 0;
        if (!b.fog) u.lighting.fogColour.w = 0;
        SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
        SDL_DrawGPUPrimitives(pass, b.count, 1, b.first, 0);
    }
}

void Renderer::setGlobeMaps(const std::vector<std::vector<tr::Rgba8>>& faces, uint32_t size) {
    std::vector<std::vector<tr::Rgba8>> all(6, std::vector<tr::Rgba8>(size_t(size) * size, tr::Rgba8{255, 255, 255, 255}));
    all.insert(all.end(), faces.begin(), faces.end());
    if (globes_) SDL_ReleaseGPUTexture(device_, globes_);
    globes_ = uploadRgbaCubeArray(device_, all, size);
}

void Renderer::setCookieMaps(const std::vector<std::vector<tr::Rgba8>>& layers, uint32_t size) {
    std::vector<std::vector<tr::Rgba8>> all(1, std::vector<tr::Rgba8>(size_t(size) * size, tr::Rgba8{255, 255, 255, 255}));
    all.insert(all.end(), layers.begin(), layers.end());
    if (cookies_) SDL_ReleaseGPUTexture(device_, cookies_);
    cookies_ = uploadRgbaArray(device_, all, size);
}

void Renderer::drawLocalShadowCaster(const GpuMesh& mesh, const Mat4& model, SDL_GPUTexture* alphaTexture, float alphaReference, bool terrain,
                                     bool twoSided, uint32_t lightMask) {
    if (!mesh.vertices || mesh.indexCount == 0 || mesh.lines || lightMask == 0) return;
    localCasters_.push_back(LocalCaster{&mesh, model, alphaTexture, alphaReference, terrain, terrain || twoSided || !cullBackFaces, lightMask,
                                        terrain ? nullptr : currentSkin_, currentBoneBase_});
}

// The placed lights' shadow maps: each slot's square of the atlas, its casters drawn depth-only with the light's perspective view
// (psShadow: alpha cut-outs and terrain holes discard). The game renders a static light's map once at its creation (IsStatic); the
// editor redraws them every frame.
void Renderer::recordLocalShadows(SDL_GPUCommandBuffer*& commands) {
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = shadowAtlas_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = beginPass(commands, nullptr, &depth);
    SDL_GPUGraphicsPipeline* bound = nullptr;
    for (int slot = 0; slot < lighting_.localShadowCount && slot < SceneLighting::maxLocalShadows; ++slot) {
        const Vec4f& rect = lighting_.localShadowRect[slot];
        SDL_GPUViewport viewport = {rect.x, rect.y, rect.z, rect.z, 0.0f, 1.0f};
        SDL_SetGPUViewport(pass, &viewport);
        const SDL_Rect scissor = {int(rect.x), int(rect.y), int(rect.z), int(rect.z)};
        SDL_SetGPUScissor(pass, &scissor);
        for (const LocalCaster& c : localCasters_) {
            if (((c.lightMask >> uint32_t(rect.w)) & 0x1) == 0) continue;    // rect.w: the view's shadow light
            if (reserveDescriptors(commands, pass, 1, 2)) {
                bound = nullptr;
                SDL_SetGPUViewport(pass, &viewport);
                SDL_SetGPUScissor(pass, &scissor);
            }
            SDL_GPUGraphicsPipeline* pipeline = c.twoSided ? localShadowTwoSidedPipeline_ : localShadowPipeline_;
            if (pipeline != bound) {
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                bound = pipeline;
            }
            SDL_GPUBufferBinding vertexBinding = {c.mesh->vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
            SDL_GPUBufferBinding indexBinding = {c.mesh->indices, 0};
            SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            const VertexUniforms vertexUniforms{lighting_.localShadowMatrix[slot], c.model, c.skinVertices ? 1u : 0u, c.boneBase, 1u, 0};
            SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof vertexUniforms);
            bindSkin(pass, c.skinVertices);
            const SDL_GPUTextureSamplerBinding sampler = {c.alphaTexture ? c.alphaTexture : white_, linearSampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
            const Vec4f options{c.alphaTexture ? c.alphaReference : 0.0f, c.terrain ? 1.0f : 0.0f, 0, 0};
            SceneFragmentUniforms u{Vec4f{1, 1, 1, 1}, towardsLight_, options, Vec4f{0, 0, 0, 0},
                                    lightingUniforms(lighting_, cameraPosition_, shadowMatrix_, false, float(shadowSize))};
            SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
            SDL_DrawGPUIndexedPrimitives(pass, c.mesh->indexCount, 1, 0, 0, 0);
        }
    }
    SDL_EndGPURenderPass(pass);
}

void Renderer::drawTerrain(const GpuMesh& mesh, const TerrainMaterial& material) {
    if (!mesh.vertices || mesh.indexCount == 0 || !material.layers || !material.layerIndices || !material.layerWeights) return;
    draws_.push_back(DrawItem{&mesh, identityMatrix(), Vec4f{1, 1, 1, 1}, true, nullptr, false, &material, false, Vec4f{0, 0, 0, 0}, nullptr});
}

void Renderer::record(SDL_GPUCommandBuffer* /*caller's command buffer: the scene is recorded and submitted separately*/) {
    if (!colour_) return;
    // The scene goes into the renderer's own command buffer(s), submitted here - before the caller's (ImGui, downloads), so the
    // queue runs it first. On Direct3D 12 it may be split into several (reserveDescriptors).
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    if (!commands) fail("SDL_AcquireGPUCommandBuffer");
    samplerDescriptors_ = viewDescriptors_ = 0;
    uploadBones(commands);
    uploadLocalShadows(commands);
    if (shadowsActive_) recordShadows(commands);
    if (lighting_.localShadowCount > 0 && !localCasters_.empty()) recordLocalShadows(commands);
    uploadParticles(commands);
    SDL_GPUColorTargetInfo colour = {};
    colour.texture = colour_;
    colour.clear_color = clearColour_;
    colour.load_op = SDL_GPU_LOADOP_CLEAR;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = depth_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    const bool refraction = !refractDraws_.empty() && frameCopy_;
    const bool water = !waterDraws_.empty() && frameCopy_ && waterPipeline_;
    depth.store_op = refraction || water ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;
    SDL_GPURenderPass* pass = beginPass(commands, &colour, &depth);

    SDL_GPUGraphicsPipeline* bound = nullptr;
    for (const DrawItem& item : draws_) recordItem(commands, pass, item, bound);
    for (const DrawItem& item : skyDraws_) recordItem(commands, pass, item, bound);   // the Sky phase: after opaque, background only
    if (water) {
        // Water after the opaque geometry, before translucent / additive pieces and particles (the game's order in the
        // fountain trace: StretchRect of the frame after the sky phase, the water draws, then the glowstick's fins etc.).
        SDL_EndGPURenderPass(pass);
        recordWater(commands);
        colour.load_op = SDL_GPU_LOADOP_LOAD;
        depth.load_op = SDL_GPU_LOADOP_LOAD;
        depth.store_op = refraction ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
        depth.cycle = false;
        pass = beginPass(commands, &colour, &depth);
        bound = nullptr;
    }
    for (const DrawItem& item : translucentDraws_) recordItem(commands, pass, item, bound);     // after all opaque geometry
    for (const DrawItem& item : additiveDraws_) recordItem(commands, pass, item, bound);        // glows, holograms, light cones
    recordParticles(commands, pass);
    if (refraction) {
        // Refraction last, over a copy of the finished frame: in game the additive hologram shows through its refraction
        // shell (which writes depth), so it must already be in the frame (the phase registration order in FUN_00858480 -
        // Water, Refraction, Translucent, Flora, Additive - is not the draw order). Exact phase order NOT traced.
        SDL_EndGPURenderPass(pass);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureLocation from = {}, to = {};
        from.texture = colour_;
        to.texture = frameCopy_;
        SDL_CopyGPUTextureToTexture(copy, &from, &to, width_, height_, 1, false);
        SDL_EndGPUCopyPass(copy);
        colour.load_op = SDL_GPU_LOADOP_LOAD;
        colour.cycle = false;
        pass = beginPass(commands, &colour, nullptr);         // the shader tests against the sampled depth
        SDL_BindGPUGraphicsPipeline(pass, refractPipeline_);
        for (const RefractItem& item : refractDraws_) {
            if (reserveDescriptors(commands, pass, 5, 2)) SDL_BindGPUGraphicsPipeline(pass, refractPipeline_);
            SDL_GPUBufferBinding vertexBinding = {item.mesh->vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
            SDL_GPUBufferBinding indexBinding = {item.mesh->indices, 0};
            SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            const VertexUniforms vertexUniforms{viewProjection_, item.model, item.skinVertices ? 1u : 0u, item.boneBase, item.skinNormals ? 1u : 0u, 0};
            SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof vertexUniforms);
            bindSkin(pass, item.skinVertices);
            const RefractMaterial& m = item.material;
            const SDL_GPUTextureSamplerBinding samplers[5] = {{m.diffuse ? m.diffuse : white_, linearSampler_}, {m.normalMap ? m.normalMap : white_, linearSampler_},
                                                              {m.fade ? m.fade : white_, clampSampler_}, {frameCopy_, clampSampler_},
                                                              {depth_, pointSampler_}};
            SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
            RefractUniforms u{};
            u.matDiffuse = m.matDiffuse;
            u.params = Vec4f{m.displacement, 1.0f / float(width_), 1.0f / float(height_), item.skinVertices ? 1.0f : 0.0f};
            u.cameraRight = Vec4f{cameraRight_.x, cameraRight_.y, cameraRight_.z, 0};
            u.cameraUp = Vec4f{cameraUp_.x, cameraUp_.y, cameraUp_.z, 0};
            u.cameraForward = Vec4f{cameraForward_.x, cameraForward_.y, cameraForward_.z, 0};
            u.transform[0] = m.transform0u;
            u.transform[1] = m.transform0v;
            u.transform[2] = m.transform1u;
            u.transform[3] = m.transform1v;
            SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
            SDL_DrawGPUIndexedPrimitives(pass, item.mesh->indexCount, 1, 0, 0, 0);
        }
        bound = nullptr;
    }
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(commands);
}

// Begins a render pass and remembers its targets, so reserveDescriptors can continue it in a new command buffer. While command
// buffers may be split (Direct3D 12) the depth target is always stored.
SDL_GPURenderPass* Renderer::beginPass(SDL_GPUCommandBuffer* commands, const SDL_GPUColorTargetInfo* colour, SDL_GPUDepthStencilTargetInfo* depth) {
    if (depth && splitCommandBuffers_) depth->store_op = SDL_GPU_STOREOP_STORE;
    passHasColour_ = colour != nullptr;
    passHasDepth_ = depth != nullptr;
    if (colour) passColour_ = *colour;
    if (depth) passDepth_ = *depth;
    return SDL_BeginGPURenderPass(commands, colour, colour ? 1 : 0, depth);
}

// SDL 3.4.12's D3D12 backend writes each draw's descriptors into one shader-visible heap pair per command buffer (2,048 samplers,
// 65,536 CBV/SRV/UAV); when a draw's range does not fit it switches heaps too late (SDL_gpu_d3d12.c
// D3D12_INTERNAL_WriteGPUDescriptors tests only the first descriptor, and tables set earlier in the draw keep pointing into the old
// heap) - the device is removed (exit 0x87D). A new command buffer starts with fresh heaps, so the editor never lets one command
// buffer's draws exceed them. Counts are per draw, worst case: every sampler is a sampler plus a texture view.
bool Renderer::reserveDescriptors(SDL_GPUCommandBuffer*& commands, SDL_GPURenderPass*& pass, uint32_t samplers, uint32_t storageBuffers) {
    if (!splitCommandBuffers_) return false;
    const uint32_t views = samplers + storageBuffers;
    if (samplerDescriptors_ + samplers <= 2048 && viewDescriptors_ + views <= 65536) {
        samplerDescriptors_ += samplers;
        viewDescriptors_ += views;
        return false;
    }
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(commands);
    commands = SDL_AcquireGPUCommandBuffer(device_);
    if (!commands) fail("SDL_AcquireGPUCommandBuffer");
    samplerDescriptors_ = samplers;
    viewDescriptors_ = views;
    SDL_GPUColorTargetInfo colour = passColour_;
    colour.load_op = SDL_GPU_LOADOP_LOAD;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    colour.cycle = false;
    SDL_GPUDepthStencilTargetInfo depth = passDepth_;
    depth.load_op = SDL_GPU_LOADOP_LOAD;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = false;
    pass = SDL_BeginGPURenderPass(commands, passHasColour_ ? &colour : nullptr, passHasColour_ ? 1 : 0, passHasDepth_ ? &depth : nullptr);
    return true;
}

// Water: over a copy of the frame so far, sampling the scene depth (depth_ is not attached; the shader tests against it).
void Renderer::recordWater(SDL_GPUCommandBuffer*& commands) {
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureLocation from = {}, to = {};
    from.texture = colour_;
    to.texture = frameCopy_;
    SDL_CopyGPUTextureToTexture(copy, &from, &to, width_, height_, 1, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUColorTargetInfo colour = {};
    colour.texture = colour_;
    colour.load_op = SDL_GPU_LOADOP_LOAD;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = beginPass(commands, &colour, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, waterPipeline_);
    struct WaterVertexUniforms {
        Mat4 viewProjection;
        Mat4 model;
        Mat4 normalMatrix;
        Vec4f eyePosition;
        Vec4f fog;
        Vec4f transform[8];
    };
    struct WaterFragmentUniforms {
        Vec4f hemiTop, hemiBottom, dirColour, dirDirection, fogColour, matSpecular, matRefractColour, params, screen, eye;
    };
    const tr::Vec3 towards = normalized(lighting_.towardsLight);
    for (const WaterItem& item : waterDraws_) {
        if (reserveDescriptors(commands, pass, 7, 0)) SDL_BindGPUGraphicsPipeline(pass, waterPipeline_);
        SDL_GPUBufferBinding vertexBinding = {item.mesh->vertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
        SDL_GPUBufferBinding indexBinding = {item.mesh->indices, 0};
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const WaterMaterial& m = item.material;
        WaterVertexUniforms v{};
        v.viewProjection = viewProjection_;
        v.model = item.model;
        v.normalMatrix = inverseTranspose3(item.model);
        v.eyePosition = cameraPosition_;
        v.fog = Vec4f{lighting_.fogStart, lighting_.fogDensity, lighting_.fog ? 1.0f : 0.0f, 0};
        for (int k = 0; k < 8; ++k) v.transform[k] = m.transform[k];
        SDL_PushGPUVertexUniformData(commands, 0, &v, sizeof v);
        WaterFragmentUniforms u{};
        // The forward lights (gHemiLight, gDirLight[0]): the sky's hemi light and its main directional light (Color only).
        u.hemiTop = lighting_.hemiTop;
        u.hemiBottom = lighting_.hemiBottom;
        u.dirColour = lighting_.sunFront;
        u.dirDirection = Vec4f{-towards.x, -towards.y, -towards.z, 0};
        u.fogColour = Vec4f{lighting_.fogColour.x, lighting_.fogColour.y, lighting_.fogColour.z, 1};
        u.matSpecular = m.matSpecular;
        u.matRefractColour = m.matRefractColour;
        u.params = Vec4f{m.matPower, m.clampDepth != 0 ? 1.0f / m.clampDepth : 0.0f, m.refractionDisplacement, m.volumeDisplacement};
        u.screen = Vec4f{1.0f / float(width_), 1.0f / float(height_), projection_.row2.z, projection_.row2.w};
        u.eye = cameraPosition_;
        SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
        const SDL_GPUTextureSamplerBinding samplers[7] = {{m.diffuse ? m.diffuse : white_, linearSampler_},
                                                          {m.normalMap ? m.normalMap : white_, linearSampler_},
                                                          {m.modifier ? m.modifier : white_, clampSampler_},
                                                          {m.normalMap1 ? m.normalMap1 : white_, linearSampler_},
                                                          {m.volume ? m.volume : whiteVolume_, volumeSampler_},
                                                          {depth_, pointSampler_},
                                                          {frameCopy_, clampSampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 7);
        SDL_DrawGPUIndexedPrimitives(pass, item.mesh->indexCount, 1, 0, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
}

// Shadow casters: the opaque lit draws (models and terrain) into the sun's depth map.
void Renderer::recordShadows(SDL_GPUCommandBuffer*& commands) {
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = shadowMap_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = beginPass(commands, nullptr, &depth);
    SDL_BindGPUGraphicsPipeline(pass, shadowPipeline_);
    for (const DrawItem& item : draws_) {
        if (item.mesh->lines || !item.lit || !item.castsShadow) continue;
        if (reserveDescriptors(commands, pass, 1, 2)) SDL_BindGPUGraphicsPipeline(pass, shadowPipeline_);
        SDL_GPUBufferBinding vertexBinding = {item.mesh->vertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
        SDL_GPUBufferBinding indexBinding = {item.mesh->indices, 0};
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const VertexUniforms vertexUniforms{shadowMatrix_, item.model, item.skinVertices ? 1u : 0u, item.boneBase, item.skinNormals ? 1u : 0u, 0};
        SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof vertexUniforms);
        bindSkin(pass, item.skinVertices);
        const bool alphaTest = !item.terrain && item.alphaTest;
        const SDL_GPUTextureSamplerBinding sampler = {alphaTest && item.texture ? item.texture : white_, linearSampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
        const Vec4f options{alphaTest ? item.alphaReference : 0.0f, item.terrain ? 1.0f : 0.0f, 0, 0};
        SceneFragmentUniforms u{Vec4f{1, 1, 1, 1}, towardsLight_, options, Vec4f{0, 0, 0, 0},
                                lightingUniforms(lighting_, cameraPosition_, shadowMatrix_, false, float(shadowSize))};
        SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
        SDL_DrawGPUIndexedPrimitives(pass, item.mesh->indexCount, 1, 0, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
}

void Renderer::recordItem(SDL_GPUCommandBuffer*& commands, SDL_GPURenderPass*& pass, const DrawItem& item, SDL_GPUGraphicsPipeline*& bound) {
    if (reserveDescriptors(commands, pass, item.terrain ? 7 : 12, 3)) bound = nullptr;
    // Terrain: TRTerrainClient's deferred passes set no CULLMODE, so PalGlobalEffect's CCW baseline culls its back faces
    // (seen from below, e.g. from Manhattan's subway, the game shows no terrain).
    SDL_GPUGraphicsPipeline* pipeline = item.terrain ? (cullBackFaces ? culledTerrainPipeline_ : terrainPipeline_) : item.additive ? (cullBackFaces && item.cullBlended ? culledAdditivePipeline_ : additivePipeline_) : item.translucent ? (cullBackFaces && item.cullBlended ? culledTranslucentPipeline_ : translucentPipeline_)
                                        : item.mesh->lines ? linePipeline_ : cullBackFaces && (item.lit || item.sky) && !item.twoSided ? culledTrianglePipeline_ : trianglePipeline_;
    if (pipeline != bound) {
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        bound = pipeline;
    }
    SDL_GPUBufferBinding vertexBinding = {item.mesh->vertices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
    SDL_GPUBufferBinding indexBinding = {item.mesh->indices, 0};
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    VertexUniforms vertexUniforms{viewProjection_, item.model, item.skinVertices ? 1u : 0u, item.boneBase, item.skinNormals ? 1u : 0u, 0};
    if (item.aniso) {
        vertexUniforms.aniso = 1;
        vertexUniforms.anisoNormal = item.anisoNormal;
        vertexUniforms.eyePosition = cameraPosition_;
        vertexUniforms.anisoFog = Vec4f{lighting_.fogStart, lighting_.fogDensity, lighting_.fog ? 1.0f : 0.0f, 0};
    }
    if (item.sky) vertexUniforms.anisoFog.w = 1.0f;          // scene.hlsl: at the far plane
    SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof vertexUniforms);
    if (!item.terrain) bindSkin(pass, item.skinVertices);
    if (item.terrain) {
        const TerrainMaterial& m = *item.terrain;
        const SDL_GPUTextureSamplerBinding samplers[7] = {{m.layers, linearSampler_}, {m.layerIndices, pointSampler_}, {m.layerWeights, pointSampler_},
                                                          {shadowMap_, shadowSampler_}, {globes_, clampSampler_}, {shadowAtlas_, pointSampler_},
                                                          {cookies_, clampSampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 7);
        SDL_BindGPUFragmentStorageBuffers(pass, 0, &localShadowBuffer_, 1);
        TerrainFragmentUniforms u = {};
        u.grid = Vec4f{m.originX, m.originZ, m.inverseSpacing, 0};
        u.gridSize = Vec4f{float(m.columns), float(m.rows), 1.0f / float(m.columns), 1.0f / float(m.rows)};
        u.lightDirection = towardsLight_;
        u.lighting = lightingUniforms(lighting_, cameraPosition_, shadowMatrix_, shadowsActive_, float(shadowSize));
        for (size_t k = 0; k < m.tilings.size() && k < 128; ++k) {
            Vec4f& entry = u.tilings[k >> 2];
            switch (k & 0x3) {
                case 0: entry.x = m.tilings[k]; break;
                case 1: entry.y = m.tilings[k]; break;
                case 2: entry.z = m.tilings[k]; break;
                default: entry.w = m.tilings[k]; break;
            }
        }
        SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
    } else {
        // Unused detail/decal slots are switched off by layers.z/w; the light ramp defaults to opaque white.
        const SDL_GPUTextureSamplerBinding samplers[12] = {{item.texture, linearSampler_}, {item.glow ? item.glow : black_, linearSampler_},
                                                          {item.detail ? item.detail : black_, linearSampler_},
                                                          {item.decal ? item.decal : black_, linearSampler_},
                                                          {item.light ? item.light : white_, clampSampler_},
                                                          {shadowMap_, shadowSampler_}, {globes_, clampSampler_},
                                                          {item.normalMap ? item.normalMap : black_, linearSampler_},
                                                          {item.glossMap ? item.glossMap : black_, linearSampler_},
                                                          {environment_ ? environment_ : blackCube_, clampSampler_},
                                                          {shadowAtlas_, pointSampler_}, {cookies_, clampSampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 12);
        SDL_BindGPUFragmentStorageBuffers(pass, 0, &localShadowBuffer_, 1);
        SceneFragmentUniforms u{item.tint, towardsLight_, Vec4f{item.alphaTest ? item.alphaReference : 0.0f, item.sky && !item.aniso ? 3.0f : item.translucent ? 2.0f : 0.0f, item.additive ? (item.forwardLit ? 2.0f : 1.0f) : 0.0f, item.aniso ? (item.anisoPlain ? 2.0f : 1.0f) : 0.0f},
                                item.hue, lightingUniforms(lighting_, cameraPosition_, shadowMatrix_, shadowsActive_, float(shadowSize)), item.specular, item.layers,
                                Vec4f{item.normalMap ? 1.0f : 0.0f, item.glossMap ? 1.0f : 0.0f, item.environment && environment_ ? 1.0f : 0.0f,
                                      item.trHairCutout ? 1.0f : 0.0f},
                                Vec4f{item.tr.flags.x, item.tr.flags.y, environment_ ? item.tr.flags.z : 0.0f,
                                      float(item.tr.active ? (uint32_t(item.tr.flags.w) | 0x1) : item.forwardUv ? 0x20u : 0u)},
                                item.tr.specular, item.tr.emissive, item.tr.transform0u, item.tr.transform0v, item.tr.transform1u,
                                item.tr.transform1v, item.tr.transform2u, item.tr.transform2v};
        u.lightDirection.w = item.lit ? 1.0f : 0.0f;
        SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
    }
    SDL_DrawGPUIndexedPrimitives(pass, item.mesh->indexCount, 1, 0, 0, 0);
}

// ------------------------------------------------------------------------------------------
// PVS builder views (see Renderer.h / PvsBuilder.h)
// ------------------------------------------------------------------------------------------
SDL_GPUComputePipeline* Renderer::createComputePipeline(const unsigned char* dxil, size_t dxilSize, const unsigned char* spirv, size_t spirvSize,
                                                        const char* entryPoint, uint32_t samplers, uint32_t readOnlyBuffers, uint32_t readWriteBuffers,
                                                        uint32_t threadsX, uint32_t threadsY) {
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device_);
    SDL_GPUComputePipelineCreateInfo info = {};
    if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        info.code = dxil;
        info.code_size = dxilSize;
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
    } else {
        info.code = spirv;
        info.code_size = spirvSize;
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    }
    info.entrypoint = entryPoint;
    info.num_samplers = samplers;
    info.num_readonly_storage_buffers = readOnlyBuffers;
    info.num_readwrite_storage_buffers = readWriteBuffers;
    info.num_uniform_buffers = 1;
    info.threadcount_x = threadsX;
    info.threadcount_y = threadsY;
    info.threadcount_z = 1;
    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(device_, &info);
    if (!pipeline) fail("SDL_CreateGPUComputePipeline");
    return pipeline;
}

void Renderer::createPvsPipelines() {
    if (pvsDepthPipeline_) return;
    // The shadow pipeline's layout (depth only, psShadow: alpha cut-outs and terrain holes discard) without its depth bias; back
    // faces culled as PalGlobalEffect's CCW baseline does in the game (two-sided pieces: the four CULLMODE NONE effects).
    SDL_GPUVertexBufferDescription buffer = {};
    buffer.slot = 0;
    buffer.pitch = sizeof(MeshVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[8] = {};
    const uint32_t offsets[8] = {uint32_t(offsetof(MeshVertex, positionX)), uint32_t(offsetof(MeshVertex, normalX)), uint32_t(offsetof(MeshVertex, colour)),
                                 uint32_t(offsetof(MeshVertex, texU)), uint32_t(offsetof(MeshVertex, texU1)), uint32_t(offsetof(MeshVertex, tangentX)),
                                 uint32_t(offsetof(MeshVertex, binormalX)), uint32_t(offsetof(MeshVertex, texU2))};
    const SDL_GPUVertexElementFormat formats[8] = {SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
                                                   SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
                                                   SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2};
    for (uint32_t k = 0; k < 8; ++k) {
        attributes[k].location = k;
        attributes[k].format = formats[k];
        attributes[k].offset = offsets[k];
    }
    for (int culled = 0; culled < 2; ++culled) {
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = sceneVertex_;
        info.fragment_shader = shadowFragment_;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = 8;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = culled ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.target_info.num_color_targets = 0;
        info.target_info.depth_stencil_format = depthFormat_;
        info.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if (!pipeline) fail("SDL_CreateGPUGraphicsPipeline (PVS depth)");
        (culled ? pvsDepthPipeline_ : pvsDepthTwoSidedPipeline_) = pipeline;
    }
    pvsCountPipeline_ = createComputePipeline(pvsCountDxil, sizeof pvsCountDxil, pvsCountSpirv, sizeof pvsCountSpirv, "csCount", 1, 1, 2, 8, 8);
    pvsMarkPipeline_ = createComputePipeline(pvsMarkDxil, sizeof pvsMarkDxil, pvsMarkSpirv, sizeof pvsMarkSpirv, "csMark", 0, 0, 2, 64, 1);
}

void Renderer::uploadBuffer(SDL_GPUBuffer* buffer, const void* data, uint32_t size) {
    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer (PVS)");
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (data) std::memcpy(mapped, data, size);
    else std::memset(mapped, 0, size);
    SDL_UnmapGPUTransferBuffer(device_, transfer);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    if (!commands) fail("SDL_AcquireGPUCommandBuffer");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source = {};
    source.transfer_buffer = transfer;
    SDL_GPUBufferRegion destination = {};
    destination.buffer = buffer;
    destination.size = size;
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
}

void Renderer::setPvsTree(const std::vector<PvsNodeGpu>& nodes, uint32_t leafCount) {
    createPvsPipelines();
    for (SDL_GPUBuffer** b : {&pvsNodes_, &pvsCounts_, &pvsBits_})
        if (*b) { SDL_ReleaseGPUBuffer(device_, *b); *b = nullptr; }
    if (pvsDownload_) { SDL_ReleaseGPUTransferBuffer(device_, pvsDownload_); pvsDownload_ = nullptr; }
    pvsLeafCount_ = leafCount;
    const uint32_t nodeBytes = uint32_t(std::max<size_t>(nodes.size(), 1) * sizeof(PvsNodeGpu));
    const uint32_t countBytes = std::max<uint32_t>(leafCount, 1) * 4;
    const uint32_t wordBytes = ((std::max<uint32_t>(leafCount, 1) + 31) / 32) * 4;
    SDL_GPUBufferCreateInfo info = {};
    info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    info.size = nodeBytes;
    pvsNodes_ = SDL_CreateGPUBuffer(device_, &info);
    info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    info.size = countBytes;
    pvsCounts_ = SDL_CreateGPUBuffer(device_, &info);
    info.size = wordBytes;
    pvsBits_ = SDL_CreateGPUBuffer(device_, &info);
    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = wordBytes;
    pvsDownload_ = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if (!pvsNodes_ || !pvsCounts_ || !pvsBits_ || !pvsDownload_) fail("PVS buffers");
    if (!nodes.empty()) uploadBuffer(pvsNodes_, nodes.data(), nodeBytes);
    uploadBuffer(pvsCounts_, nullptr, countBytes);
    uploadBuffer(pvsBits_, nullptr, wordBytes);
}

void Renderer::clearPvsRow() {
    if (pvsBits_) uploadBuffer(pvsBits_, nullptr, ((std::max<uint32_t>(pvsLeafCount_, 1) + 31) / 32) * 4);
}

namespace {
struct PvsUniforms {
    Mat4 inverseViewProjection;
    Vec4f eye;
    uint32_t width, height, leafCount, threshold;
};
}

void Renderer::recordPvsView(const Mat4& inverseViewProjection, const tr::Vec3& eye, uint32_t threshold) {
    if (!depth_ || !pvsCountPipeline_ || !pvsNodes_) return;
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    if (!commands) fail("SDL_AcquireGPUCommandBuffer");
    samplerDescriptors_ = viewDescriptors_ = 0;
    uploadBones(commands);
    uploadLocalShadows(commands);
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = depth_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;
    SDL_GPURenderPass* pass = beginPass(commands, nullptr, &depth);
    SDL_GPUGraphicsPipeline* bound = nullptr;
    for (const DrawItem& item : draws_) {
        if (item.mesh->lines || !item.lit) continue;
        if (reserveDescriptors(commands, pass, 1, 2)) bound = nullptr;
        SDL_GPUGraphicsPipeline* pipeline = cullBackFaces && !item.twoSided ? pvsDepthPipeline_ : pvsDepthTwoSidedPipeline_;
        if (pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            bound = pipeline;
        }
        SDL_GPUBufferBinding vertexBinding = {item.mesh->vertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
        SDL_GPUBufferBinding indexBinding = {item.mesh->indices, 0};
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const VertexUniforms vertexUniforms{viewProjection_, item.model, item.skinVertices ? 1u : 0u, item.boneBase, item.skinNormals ? 1u : 0u, 0};
        SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof vertexUniforms);
        bindSkin(pass, item.skinVertices);
        const bool alphaTest = !item.terrain && item.alphaTest;
        const SDL_GPUTextureSamplerBinding sampler = {alphaTest && item.texture ? item.texture : white_, linearSampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
        const Vec4f options{alphaTest ? item.alphaReference : 0.0f, item.terrain ? 1.0f : 0.0f, 0, 0};
        SceneFragmentUniforms u{Vec4f{1, 1, 1, 1}, towardsLight_, options, Vec4f{0, 0, 0, 0},
                                lightingUniforms(lighting_, cameraPosition_, shadowMatrix_, false, float(shadowSize))};
        SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
        SDL_DrawGPUIndexedPrimitives(pass, item.mesh->indexCount, 1, 0, 0, 0);
    }
    SDL_EndGPURenderPass(pass);

    const PvsUniforms uniforms{inverseViewProjection, Vec4f{eye.x, eye.y, eye.z, 1}, width_, height_, pvsLeafCount_, threshold};
    SDL_GPUStorageBufferReadWriteBinding writable[2] = {};
    writable[0].buffer = pvsCounts_;
    writable[1].buffer = pvsBits_;
    SDL_GPUComputePass* compute = SDL_BeginGPUComputePass(commands, nullptr, 0, writable, 2);
    SDL_BindGPUComputePipeline(compute, pvsCountPipeline_);
    const SDL_GPUTextureSamplerBinding depthBinding = {depth_, pointSampler_};
    SDL_BindGPUComputeSamplers(compute, 0, &depthBinding, 1);
    SDL_BindGPUComputeStorageBuffers(compute, 0, &pvsNodes_, 1);
    SDL_PushGPUComputeUniformData(commands, 0, &uniforms, sizeof uniforms);
    SDL_DispatchGPUCompute(compute, (width_ + 7) / 8, (height_ + 7) / 8, 1);
    SDL_EndGPUComputePass(compute);
    // A second pass sees every count of the first (SDL synchronises read-write storage between compute passes).
    compute = SDL_BeginGPUComputePass(commands, nullptr, 0, writable, 2);
    SDL_BindGPUComputePipeline(compute, pvsMarkPipeline_);
    SDL_PushGPUComputeUniformData(commands, 0, &uniforms, sizeof uniforms);
    SDL_DispatchGPUCompute(compute, (pvsLeafCount_ + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(compute);
    SDL_SubmitGPUCommandBuffer(commands);
}

std::vector<uint32_t> Renderer::downloadPvsRow() {
    const uint32_t words = (std::max<uint32_t>(pvsLeafCount_, 1) + 31) / 32;
    std::vector<uint32_t> out(words, 0);
    if (!pvsBits_ || !pvsDownload_) return out;
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    if (!commands) fail("SDL_AcquireGPUCommandBuffer");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUBufferRegion source = {};
    source.buffer = pvsBits_;
    source.size = words * 4;
    SDL_GPUTransferBufferLocation destination = {};
    destination.transfer_buffer = pvsDownload_;
    SDL_DownloadFromGPUBuffer(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
    }
    const void* mapped = SDL_MapGPUTransferBuffer(device_, pvsDownload_, false);
    if (mapped) std::memcpy(out.data(), mapped, words * 4);
    SDL_UnmapGPUTransferBuffer(device_, pvsDownload_);
    return out;
}
