#include "Renderer.h"
#include "GpuTexture.h"
#include "ShaderBytecode.h"

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
};

struct SceneFragmentUniforms {
    Vec4f tint;
    Vec4f lightDirection;          // w: 1 = lit, 0 = unlit
    Vec4f options;                 // x: alpha test, z: additive, w: aniso additive
    Vec4f hue;                     // rgb added, weighted by the texture alpha; w: 1 = on
    LightingUniforms lighting;
    Vec4f specular;                // aniso additive: MatSpecular
    Vec4f layers;                  // aniso additive: x detail uses UV set 1, y decal uses UV set 1, z 1 = has detail, w 1 = has decal
};

struct TerrainFragmentUniforms {
    Vec4f grid;                    // originX, originZ, 1 / spacing
    Vec4f gridSize;                // columns, rows, 1 / columns, 1 / rows
    Vec4f lightDirection;
    LightingUniforms lighting;
    Vec4f tilings[32];             // 128 layers
};

LightingUniforms lightingUniforms(const SceneLighting& l, const Vec4f& cameraPosition) {
    LightingUniforms u;
    u.sunFront = l.sunFront;
    u.sunBack = l.sunBack;
    u.hemiTop = l.hemiTop;
    u.hemiBottom = l.hemiBottom;
    u.fogColour = Vec4f{l.fogColour.x, l.fogColour.y, l.fogColour.z, l.fog ? 1.0f : 0.0f};
    u.fogRange = Vec4f{l.fogStart, l.fogDensity, l.showLight ? 1.0f : 0.0f, 0};
    u.cameraPosition = cameraPosition;
    u.localCount = Vec4f{float(l.localCount), 0, 0, 0};
    for (int i = 0; i < SceneLighting::maxLocalLights; ++i) {
        u.localPosition[i] = l.localPosition[i];
        u.localColour[i] = l.localColour[i];
        u.localSpot[i] = l.localSpot[i];
        u.localSpotInner[i] = l.localSpotInner[i];
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
                                  SDL_GPU_SHADERSTAGE_FRAGMENT, 5);
    terrainVertex_ = createShader(terrainVertexDxil, sizeof terrainVertexDxil, terrainVertexSpirv, sizeof terrainVertexSpirv, "vsMain",
                                  SDL_GPU_SHADERSTAGE_VERTEX, 0);
    terrainFragment_ = createShader(terrainFragmentDxil, sizeof terrainFragmentDxil, terrainFragmentSpirv, sizeof terrainFragmentSpirv,
                                    "psMain", SDL_GPU_SHADERSTAGE_FRAGMENT, 3);
    if (!SDL_GPUTextureSupportsFormat(device_, depthFormat_, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        depthFormat_ = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    trianglePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    linePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_LINELIST);
    terrainPipeline_ = createPipeline(terrainVertex_, terrainFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    translucentPipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 1);
    additivePipeline_ = createPipeline(sceneVertex_, sceneFragment_, SDL_GPU_PRIMITIVETYPE_TRIANGLELIST, 2);
    linearSampler_ = createSampler(device_, true, true);
    pointSampler_ = createSampler(device_, false, false);
    clampSampler_ = createSampler(device_, true, false);
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
    for (SDL_GPUGraphicsPipeline* p : {trianglePipeline_, linePipeline_, terrainPipeline_, translucentPipeline_, additivePipeline_})
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
                                                  int blend) {
    SDL_GPUVertexBufferDescription buffer = {};
    buffer.slot = 0;
    buffer.pitch = sizeof(MeshVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[5] = {};
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

    SDL_GPUColorTargetDescription target = {};
    target.format = colourFormat_;
    if (blend != 0) {                                  // 1: alpha blend, 2: additive (one, one)
        target.blend_state.enable_blend = true;
        target.blend_state.src_color_blendfactor = blend == 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor = blend == 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
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
    info.vertex_input_state.num_vertex_attributes = 5;
    info.primitive_type = primitive;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
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
    cameraPosition_ = Vec4f{cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f};
    draws_.clear();
    translucentDraws_.clear();
    additiveDraws_.clear();
}

void Renderer::draw(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, bool lit, SDL_GPUTexture* texture, bool alphaTest, const Vec4f& hue,
                    SDL_GPUTexture* glow) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    draws_.push_back(DrawItem{&mesh, model, tint, lit, texture ? texture : white_, alphaTest, nullptr, false, hue, glow ? glow : black_});
}

void Renderer::drawTranslucent(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    translucentDraws_.push_back(DrawItem{&mesh, model, tint, true, white_, false, nullptr, true, Vec4f{0, 0, 0, 0}, black_});
}

void Renderer::drawAdditive(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture) {
    if (!mesh.vertices || mesh.indexCount == 0) return;
    DrawItem item{&mesh, model, tint, false, texture ? texture : white_, false, nullptr, true, Vec4f{0, 0, 0, 0}, black_};
    item.additive = true;
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

void Renderer::drawTerrain(const GpuMesh& mesh, const TerrainMaterial& material) {
    if (!mesh.vertices || mesh.indexCount == 0 || !material.layers || !material.layerIndices || !material.layerWeights) return;
    draws_.push_back(DrawItem{&mesh, identityMatrix(), Vec4f{1, 1, 1, 1}, true, nullptr, false, &material, false, Vec4f{0, 0, 0, 0}, nullptr});
}

void Renderer::record(SDL_GPUCommandBuffer* commands) {
    if (!colour_) return;
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
    SDL_EndGPURenderPass(pass);
}

void Renderer::recordItem(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const DrawItem& item, SDL_GPUGraphicsPipeline*& bound) {
    SDL_GPUGraphicsPipeline* pipeline = item.terrain ? terrainPipeline_ : item.additive ? additivePipeline_ : item.translucent ? translucentPipeline_
                                        : item.mesh->lines ? linePipeline_ : trianglePipeline_;
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
        const SDL_GPUTextureSamplerBinding samplers[3] = {{m.layers, linearSampler_}, {m.layerIndices, pointSampler_}, {m.layerWeights, pointSampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);
        TerrainFragmentUniforms u = {};
        u.grid = Vec4f{m.originX, m.originZ, m.inverseSpacing, 0};
        u.gridSize = Vec4f{float(m.columns), float(m.rows), 1.0f / float(m.columns), 1.0f / float(m.rows)};
        u.lightDirection = towardsLight_;
        u.lighting = lightingUniforms(lighting_, cameraPosition_);
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
        const SDL_GPUTextureSamplerBinding samplers[5] = {{item.texture, linearSampler_}, {item.glow ? item.glow : black_, linearSampler_},
                                                          {item.detail ? item.detail : black_, linearSampler_},
                                                          {item.decal ? item.decal : black_, linearSampler_},
                                                          {item.light ? item.light : white_, clampSampler_}};
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
        SceneFragmentUniforms u{item.tint, towardsLight_, Vec4f{item.alphaTest ? 1.0f : 0.0f, 0, item.additive ? 1.0f : 0.0f, item.aniso ? 1.0f : 0.0f},
                                item.hue, lightingUniforms(lighting_, cameraPosition_), item.specular, item.layers};
        u.lightDirection.w = item.lit ? 1.0f : 0.0f;
        SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof u);
    }
    SDL_DrawGPUIndexedPrimitives(pass, item.mesh->indexCount, 1, 0, 0, 0);
}
