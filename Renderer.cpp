#include "Renderer.h"
#include "GpuTexture.h"
#include "ShaderBytecode.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

struct VertexUniforms {
    Mat4 viewProjection;
    Mat4 model;
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
    Vec4f shadowParams;            // x: 1 = sun shadows on, y: opacity, z: 1 / map size
};

struct SceneFragmentUniforms {
    Vec4f tint;
    Vec4f lightDirection;          // w: 1 = lit, 0 = unlit
    Vec4f options;                 // x: alpha test, z: additive, w: aniso additive
    Vec4f hue;                     // rgb added, weighted by the texture alpha; w: 1 = on
    LightingUniforms lighting;
    Vec4f specular;                // aniso additive: MatSpecular
    Vec4f layers;                  // aniso additive: x detail uses UV set 1, y decal uses UV set 1, z 1 = has detail, w 1 = has decal
    Vec4f material;                // x: 1 = normal map, y: 1 = gloss map
};

struct TerrainFragmentUniforms {
    Vec4f grid;                    // originX, originZ, 1 / spacing
    Vec4f gridSize;                // columns, rows, 1 / columns, 1 / rows
    Vec4f lightDirection;
    LightingUniforms lighting;
    Vec4f tilings[32];             // 128 layers
};

LightingUniforms lightingUniforms(const SceneLighting& l, const Vec4f& cameraPosition, const Mat4& shadowMatrix, bool shadows, float mapSize) {
    LightingUniforms u;
    u.shadowMatrix = shadowMatrix;
    u.shadowParams = Vec4f{shadows ? 1.0f : 0.0f, l.shadowOpacity, 1.0f / mapSize, 0};
    u.sunFront = l.sunFront;
    u.sunBack = l.sunBack;
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
    sceneVertex_ = createShader(sceneVertexDxil, sizeof sceneVertexDxil, sceneVertexSpirv, sizeof sceneVertexSpirv, "vsMain",
                                SDL_GPU_SHADERSTAGE_VERTEX, 0);
    sceneFragment_ = createShader(sceneFragmentDxil, sizeof sceneFragmentDxil, sceneFragmentSpirv, sizeof sceneFragmentSpirv, "psMain",
                                  SDL_GPU_SHADERSTAGE_FRAGMENT, 9);
    terrainVertex_ = createShader(terrainVertexDxil, sizeof terrainVertexDxil, terrainVertexSpirv, sizeof terrainVertexSpirv, "vsMain",
                                  SDL_GPU_SHADERSTAGE_VERTEX, 0);
    terrainFragment_ = createShader(terrainFragmentDxil, sizeof terrainFragmentDxil, terrainFragmentSpirv, sizeof terrainFragmentSpirv,
                                    "psMain", SDL_GPU_SHADERSTAGE_FRAGMENT, 5);
    shadowFragment_ = createShader(shadowFragmentDxil, sizeof shadowFragmentDxil, shadowFragmentSpirv, sizeof shadowFragmentSpirv, "psShadow",
                                   SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    if (!SDL_GPUTextureSupportsFormat(device_, depthFormat_, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        depthFormat_ = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    trianglePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    culledTrianglePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 0, true);
    linePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_LINELIST);
    terrainPipeline_ = createPipeline(terrainVertex_, terrainFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    translucentPipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 1);
    additivePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 2);
    subtractivePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 3);
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
        SDL_GPUVertexAttribute attributes[7] = {};
        const uint32_t offsets[7] = {uint32_t(offsetof(MeshVertex, positionX)), uint32_t(offsetof(MeshVertex, normalX)), uint32_t(offsetof(MeshVertex, colour)),
                                     uint32_t(offsetof(MeshVertex, texU)), uint32_t(offsetof(MeshVertex, texU1)), uint32_t(offsetof(MeshVertex, tangentX)),
                                     uint32_t(offsetof(MeshVertex, binormalX))};
        const SDL_GPUVertexElementFormat formats[7] = {SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
                                                       SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
                                                       SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3};
        for (uint32_t k = 0; k < 7; ++k) {
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
        info.vertex_input_state.num_vertex_attributes = 7;
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
    }
    setGlobeMaps({}, 4);
    const uint32_t white = 0xFFFFFFFF;
    white_ = uploadRgbaTexture(device_, &white, 1, 1);
    const uint32_t black = 0xFF000000;
    black_ = uploadRgbaTexture(device_, &black, 1, 1);
    viewProjection_ = identityMatrix();
}

Renderer::~Renderer() {
    if (colour_) SDL_ReleaseGPUTexture(device_, colour_);
    if (depth_) SDL_ReleaseGPUTexture(device_, depth_);
    if (white_) SDL_ReleaseGPUTexture(device_, white_);
    if (black_) SDL_ReleaseGPUTexture(device_, black_);
    if (linearSampler_) SDL_ReleaseGPUSampler(device_, linearSampler_);
    if (pointSampler_) SDL_ReleaseGPUSampler(device_, pointSampler_);
    if (clampSampler_) SDL_ReleaseGPUSampler(device_, clampSampler_);
    if (shadowSampler_) SDL_ReleaseGPUSampler(device_, shadowSampler_);
    if (shadowMap_) SDL_ReleaseGPUTexture(device_, shadowMap_);
    if (globes_) SDL_ReleaseGPUTexture(device_, globes_);
    if (shadowPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, shadowPipeline_);
    if (shadowFragment_) SDL_ReleaseGPUShader(device_, shadowFragment_);
    if (particleBuffer_) SDL_ReleaseGPUBuffer(device_, particleBuffer_);
    if (particleTransfer_) SDL_ReleaseGPUTransferBuffer(device_, particleTransfer_);
    for (SDL_GPUGraphicsPipeline* p : {trianglePipeline_, culledTrianglePipeline_, linePipeline_, terrainPipeline_, translucentPipeline_, additivePipeline_, subtractivePipeline_})
        if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    for (SDL_GPUShader* s : {sceneVertex_, sceneFragment_, terrainVertex_, terrainFragment_})
        if (s) SDL_ReleaseGPUShader(device_, s);
}

SDL_GPUShader* Renderer::createShader(const unsigned char* dxil, size_t dxilSize, const unsigned char* spirv, size_t spirvSize,
                                      const char* entryPoint, SDL_GPUShaderStage stage, uint32_t samplers) {
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
    info.num_uniform_buffers = 1;
    SDL_GPUShader* shader = SDL_CreateGPUShader(device_, &info);
    if (!shader) fail("SDL_CreateGPUShader");
    return shader;
}

SDL_GPUGraphicsPipeline* Renderer::createPipeline(SDL_GPUShader* vertexShader, SDL_GPUShader* fragmentShader, SDL_GPUPrimitiveType primitive,
                                                  int blend, bool cullBack) {
    SDL_GPUVertexBufferDescription buffer = {};
    buffer.slot = 0;
    buffer.pitch = sizeof(MeshVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[7] = {};
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
    if (blend != 0) {                                  // 1: alpha blend, 2: additive (one, one), 3: reverse subtract (one, one)
        target.blend_state.enable_blend = true;
        target.blend_state.src_color_blendfactor = blend >= 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor = blend >= 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
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
    info.vertex_input_state.num_vertex_attributes = 7;
    info.primitive_type = primitive;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = cullBack ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = blend == 0;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = depthFormat_;
    info.target_info.has_depth_stencil_target = true;
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
    info.format = depthFormat_;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
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
    towardsLight_ = Vec4f{lighting.towardsLight.x, lighting.towardsLight.y, lighting.towardsLight.z, 1.0f};
    clearColour_ = lighting.clearColour;
    lighting_ = lighting;
    // Sun shadow map: orthographic from the sun around the camera, snapped to whole texels so it does not shimmer.
    shadowsActive_ = lighting.sunShadows && lighting.towardsLight.y > 0.02f;
    if (shadowsActive_) {
        const tr::Vec3 toward = normalized(lighting.towardsLight);
        const tr::Vec3 forward = scaled(toward, -1.0f);
        const tr::Vec3 right = normalized(cross(std::fabs(forward.y) > 0.99f ? tr::Vec3{0, 0, 1} : tr::Vec3{0, 1, 0}, forward));
        const tr::Vec3 up = cross(forward, right);
        const float texel = 2.0f * shadowExtent / float(shadowSize);
        float cx = dot(cameraPosition, right), cy = dot(cameraPosition, up);
        cx = std::floor(cx / texel) * texel;
        cy = std::floor(cy / texel) * texel;
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
    particleVertices_.clear();
    particleBatches_.clear();
}

void Renderer::draw(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, bool lit, SDL_GPUTexture* texture, bool alphaTest, const Vec4f& hue,
                    SDL_GPUTexture* glow, bool twoSided, float alphaReference, SDL_GPUTexture* normalMap, SDL_GPUTexture* glossMap) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, tint, lit, texture ? texture : white_, alphaTest, nullptr, false, hue, glow ? glow : black_};
    item.twoSided = twoSided;
    item.alphaReference = alphaReference;
    item.normalMap = normalMap;
    item.glossMap = glossMap;
    draws_.push_back(item);
}

void Renderer::drawTranslucent(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture, SDL_GPUTexture* detail,
                               bool detailUv1) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, tint, true, texture ? texture : white_, false, nullptr, true, Vec4f{0, 0, 0, 0}, black_};
    item.detail = detail;
    item.layers = Vec4f{detailUv1 ? 1.0f : 0.0f, 0, detail ? 1.0f : 0.0f, 0};
    translucentDraws_.push_back(item);
}

void Renderer::drawAdditive(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture, bool lit, SDL_GPUTexture* detail,
                            bool detailUv1) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, tint, false, texture ? texture : white_, false, nullptr, true, Vec4f{0, 0, 0, 0}, black_};
    item.additive = true;
    item.forwardLit = lit;
    item.detail = detail;
    item.layers = Vec4f{detailUv1 ? 1.0f : 0.0f, 0, detail ? 1.0f : 0.0f, 0};
    additiveDraws_.push_back(item);
}

void Renderer::drawAnisoAdditive(const GpuMesh& mesh, const Mat4& model, const AnisoMaterial& material) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, material.diffuseAlpha, false, material.diffuse ? material.diffuse : white_, false, nullptr, true, Vec4f{0, 0, 0, 0},
                  black_};
    item.additive = true;
    item.detail = material.detail;
    item.decal = material.decal;
    item.light = material.light;
    item.specular = material.specular;
    item.aniso = true;
    item.layers = Vec4f{material.detailUv1 ? 1.0f : 0.0f, material.decalUv1 ? 1.0f : 0.0f, material.detail ? 1.0f : 0.0f, material.decal ? 1.0f : 0.0f};
    additiveDraws_.push_back(item);
}

void Renderer::drawParticles(const std::vector<MeshVertex>& vertices, SDL_GPUTexture* texture, uint32_t blendType) {
    if (vertices.empty()) return;
    ParticleBatch batch;
    batch.first = uint32_t(particleVertices_.size());
    batch.count = uint32_t(vertices.size());
    batch.texture = texture ? texture : white_;
    batch.blendType = blendType;
    // Consecutive quads of the same texture and blend share one draw.
    if (!particleBatches_.empty() && particleBatches_.back().texture == batch.texture && particleBatches_.back().blendType == blendType)
        particleBatches_.back().count += batch.count;
    else
        particleBatches_.push_back(batch);
    particleVertices_.insert(particleVertices_.end(), vertices.begin(), vertices.end());
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

void Renderer::recordParticles(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass) {
    if (particleBatches_.empty() || !particleBuffer_) return;
    SDL_GPUGraphicsPipeline* bound = nullptr;
    SDL_GPUBufferBinding vertexBinding = {particleBuffer_, 0};
    const VertexUniforms vertexUniforms{viewProjection_, identityMatrix()};
    for (const ParticleBatch& b : particleBatches_) {
        SDL_GPUGraphicsPipeline* pipeline = b.blendType == 0 ? additivePipeline_ : b.blendType == 3 ? subtractivePipeline_ : translucentPipeline_;
        if (pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
            bound = pipeline;
        }
        SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof vertexUniforms);
        const SDL_GPUTextureSamplerBinding samplers[9] = {{b.texture, linearSampler_}, {black_, linearSampler_}, {black_, linearSampler_},
                                                          {black_, linearSampler_}, {white_, clampSampler_}, {shadowMap_, shadowSampler_},
                                                          {globes_, clampSampler_}, {black_, linearSampler_}, {black_, linearSampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 9);
        // Additive and subtractive use the additive path (rgb x alpha, faded by fog); alpha-blended ones options.y.
        const bool added = b.blendType == 0 || b.blendType == 3;
        SceneFragmentUniforms u{Vec4f{1, 1, 1, 1}, towardsLight_, Vec4f{0, added ? 0.0f : 1.0f, added ? 1.0f : 0.0f, 0}, Vec4f{0, 0, 0, 0},
                                lightingUniforms(lighting_, cameraPosition_, shadowMatrix_, false, float(shadowSize))};
        u.lightDirection.w = 0;
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

void Renderer::drawTerrain(const GpuMesh& mesh, const TerrainMaterial& material) {
    if (!mesh.vertices || mesh.indexCount == 0 || !material.layers || !material.layerIndices || !material.layerWeights) return;
    draws_.push_back(DrawItem{&mesh, identityMatrix(), Vec4f{1, 1, 1, 1}, true, nullptr, false, &material, false, Vec4f{0, 0, 0, 0}, nullptr});
}

void Renderer::record(SDL_GPUCommandBuffer* commands) {
    if (!colour_) return;
    if (shadowsActive_) recordShadows(commands);
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
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colour, 1, &depth);

    SDL_GPUGraphicsPipeline* bound = nullptr;
    for (const DrawItem& item : draws_) recordItem(commands, pass, item, bound);
    for (const DrawItem& item : translucentDraws_) recordItem(commands, pass, item, bound);     // after all opaque geometry
    for (const DrawItem& item : additiveDraws_) recordItem(commands, pass, item, bound);        // glows, holograms, light cones
    recordParticles(commands, pass);
    SDL_EndGPURenderPass(pass);
}

// Shadow casters: the opaque lit draws (models and terrain) into the sun's depth map.
void Renderer::recordShadows(SDL_GPUCommandBuffer* commands) {
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = shadowMap_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
    SDL_BindGPUGraphicsPipeline(pass, shadowPipeline_);
    for (const DrawItem& item : draws_) {
        if (item.mesh->lines || !item.lit) continue;
        SDL_GPUBufferBinding vertexBinding = {item.mesh->vertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
        SDL_GPUBufferBinding indexBinding = {item.mesh->indices, 0};
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const VertexUniforms vertexUniforms{shadowMatrix_, item.model};
        SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof vertexUniforms);
        const bool alphaTest = !item.terrain && item.alphaTest;
        const SDL_GPUTextureSamplerBinding sampler = {alphaTest && item.texture ? item.texture : white_, linearSampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
        const Vec4f options{alphaTest ? item.alphaReference : 0.0f, 0, 0, 0};
        SceneFragmentUniforms u{Vec4f{1, 1, 1, 1}, towardsLight_, options, Vec4f{0, 0, 0, 0},
                                lightingUniforms(lighting_, cameraPosition_, shadowMatrix_, false, float(shadowSize))};
        SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
        SDL_DrawGPUIndexedPrimitives(pass, item.mesh->indexCount, 1, 0, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
}

void Renderer::recordItem(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const DrawItem& item, SDL_GPUGraphicsPipeline*& bound) {
    SDL_GPUGraphicsPipeline* pipeline = item.terrain ? terrainPipeline_ : item.additive ? additivePipeline_ : item.translucent ? translucentPipeline_
                                        : item.mesh->lines ? linePipeline_ : cullBackFaces && item.lit && !item.twoSided ? culledTrianglePipeline_ : trianglePipeline_;
    if (pipeline != bound) {
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        bound = pipeline;
    }
    SDL_GPUBufferBinding vertexBinding = {item.mesh->vertices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);
    SDL_GPUBufferBinding indexBinding = {item.mesh->indices, 0};
    SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    const VertexUniforms vertexUniforms{viewProjection_, item.model};
    SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof vertexUniforms);
    if (item.terrain) {
        const TerrainMaterial& m = *item.terrain;
        const SDL_GPUTextureSamplerBinding samplers[5] = {{m.layers, linearSampler_}, {m.layerIndices, pointSampler_}, {m.layerWeights, pointSampler_},
                                                          {shadowMap_, shadowSampler_}, {globes_, clampSampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
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
        const SDL_GPUTextureSamplerBinding samplers[9] = {{item.texture, linearSampler_}, {item.glow ? item.glow : black_, linearSampler_},
                                                          {item.detail ? item.detail : black_, linearSampler_},
                                                          {item.decal ? item.decal : black_, linearSampler_},
                                                          {item.light ? item.light : white_, clampSampler_},
                                                          {shadowMap_, shadowSampler_}, {globes_, clampSampler_},
                                                          {item.normalMap ? item.normalMap : black_, linearSampler_},
                                                          {item.glossMap ? item.glossMap : black_, linearSampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 9);
        SceneFragmentUniforms u{item.tint, towardsLight_, Vec4f{item.alphaTest ? item.alphaReference : 0.0f, item.translucent ? 2.0f : 0.0f, item.additive ? (item.forwardLit ? 2.0f : 1.0f) : 0.0f, item.aniso ? 1.0f : 0.0f},
                                item.hue, lightingUniforms(lighting_, cameraPosition_, shadowMatrix_, shadowsActive_, float(shadowSize)), item.specular, item.layers,
                                Vec4f{item.normalMap ? 1.0f : 0.0f, item.glossMap ? 1.0f : 0.0f, 0, 0}};
        u.lightDirection.w = item.lit ? 1.0f : 0.0f;
        SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
    }
    SDL_DrawGPUIndexedPrimitives(pass, item.mesh->indexCount, 1, 0, 0, 0);
}
