#pragma once

#include "GpuMath.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

// Vertex used by every viewport mesh. colour is RGBA8 with red in the low byte (UBYTE4_NORM).
struct MeshVertex {
    float positionX = 0, positionY = 0, positionZ = 0;
    float normalX = 0, normalY = 0, normalZ = 0;
    uint32_t colour = 0xFFFFFFFF;
    float texU = 0, texV = 0;
    float texU1 = 0, texV1 = 0;        // second UV set (glow maps)
};

inline uint32_t packColour(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    return uint32_t(red) | (uint32_t(green) << 8) | (uint32_t(blue) << 16) | (uint32_t(alpha) << 24);
}

// Material of the game's TRAdditiveAnisoSpecMapDiffMap[DetMapDecMap]VertColor effects (shader in effects.glm, verified by disassembly):
//   albedo = lerp(diffuse + detail - 0.5, decal, decal.a);  alpha = diffuse.a * vertex.a * MatDiffuse.a
//   colour = 4 * alpha * albedo * (albedo * vertex.rgb + LightTexture(0.5, max(N.V, 0)).a * MatSpecular)
struct AnisoMaterial {
    SDL_GPUTexture* diffuse = nullptr;     // UV set 0
    SDL_GPUTexture* detail = nullptr;      // null: no detail layer (diffuse used as is)
    SDL_GPUTexture* decal = nullptr;       // null: no decal
    SDL_GPUTexture* light = nullptr;       // LightTexture: alpha ramp indexed by N.V
    bool detailUv1 = false;                // DetailTexCoord = 1
    bool decalUv1 = false;                 // DecalTexCoord = 1
    Vec4f specular = Vec4f{0, 0, 0, 0};    // MatSpecular
    Vec4f diffuseAlpha = Vec4f{1, 1, 1, 1};  // MatDiffuse
};

struct GpuMesh {
    SDL_GPUBuffer* vertices = nullptr;
    SDL_GPUBuffer* indices = nullptr;
    uint32_t indexCount = 0;
    bool lines = false;                 // line list (two indices per segment) instead of triangles
};

// Inputs of the terrain shader (see shaders/terrain.hlsl).
struct TerrainMaterial {
    SDL_GPUTexture* layers = nullptr;           // 2D array, one RGBA layer per texture layer, mipmapped
    SDL_GPUTexture* layerIndices = nullptr;     // one texel per terrain sample: its four layer indices
    SDL_GPUTexture* layerWeights = nullptr;     // one texel per terrain sample: its four weights
    float originX = 0;                          // world x of sample column 0
    float originZ = 0;                          // world z of sample row 0
    float inverseSpacing = 1;
    uint32_t columns = 1;
    uint32_t rows = 1;
    std::vector<float> tilings;                 // metres per repeat, per layer (up to 128)
};

// Frame lighting (see shaders/lighting.hlsli): hemispheric light + one directional light, linear fog.
// The defaults are the plain editor lighting used when sky lighting is off.
struct SceneLighting {
    tr::Vec3 towardsLight{-0.35f, 0.85f, -0.4f};
    Vec4f sunFront{0.65f, 0.65f, 0.65f, 1};
    Vec4f sunBack{0, 0, 0, 1};
    Vec4f hemiTop{0.4f, 0.4f, 0.4f, 1};
    Vec4f hemiBottom{0.3f, 0.3f, 0.3f, 1};
    bool fog = false;
    Vec4f fogColour{0.55f, 0.66f, 0.78f, 1};
    float fogStart = 0;
    float fogDensity = 0;                        // exp-squared fog, see shaders/lighting.hlsli
    bool showLight = false;                      // debug view like the game's /dl_showlight: 0.2 x light, no albedo
    // Sun shadows: a shadow map from the sun over the area around the camera. The game's shadows let part of the sun
    // through: shadowOpacity 0.64 fitted at Alia Das, 12:00 (shadowed ground 1.02 = ambient 0.60 + 0.36 x sun 1.16).
    bool sunShadows = false;
    float shadowOpacity = 0.64f;
    SDL_FColor clearColour{0.55f, 0.66f, 0.78f, 1.0f};   // background (the sky colour with sky lighting)
    // Placed lights, laid out as the shaders' arrays (shaders/lighting.hlsli): position xyz + range; colour rgb *
    // intensity + fade share; spot direction + cos(outer half angle) (-2 = point); x = cos(inner half angle).
    static constexpr int maxLocalLights = 16;
    int localCount = 0;
    Vec4f localPosition[maxLocalLights] = {};
    Vec4f localColour[maxLocalLights] = {};
    Vec4f localSpot[maxLocalLights] = {};
    Vec4f localSpotInner[maxLocalLights] = {};
};

// Creates the GPU device, preferring Vulkan (the same SPIR-V path on Windows, Linux and macOS via MoltenVK); falls back
// to SDL's own choice (Direct3D 12 on Windows) if Vulkan is unavailable. SDL_GPU_DRIVER, when set, still wins.
SDL_GPUDevice* createGpuDevice(SDL_GPUShaderFormat formats, bool debug);

// Offscreen scene renderer for the viewport: colour + depth targets; textured-mesh, line and terrain pipelines.
class Renderer {
public:
    explicit Renderer(SDL_GPUDevice* device);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    SDL_GPUDevice* device() const { return device_; }
    // (Re)creates the render targets when the size changes. Call before the frame's draw lists
    // reference colourTarget().
    void resize(uint32_t width, uint32_t height);
    SDL_GPUTexture* colourTarget() const { return colour_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    GpuMesh createMesh(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices, bool lines);
    void destroyMesh(GpuMesh& mesh);

    // Frame: begin, queue draws, then record everything into a command buffer.
    void beginFrame(const Mat4& viewProjection, const tr::Vec3& cameraPosition, const SceneLighting& lighting);
    // texture nullptr draws untextured (white); alphaTest discards texels with alpha below 0.5.
    // hue (w = 1) adds hue.rgb weighted by the texture alpha, as the game's "Hue" effects do (avatar skin, hair, clothing).
    // glow (sampled with the second UV set) is added after lighting, as the game's glow buffer is.
    void draw(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, bool lit, SDL_GPUTexture* texture = nullptr, bool alphaTest = false,
              const Vec4f& hue = Vec4f{0, 0, 0, 0}, SDL_GPUTexture* glow = nullptr);
    // Alpha-blended (tint.w is the opacity), drawn after all opaque draws, without depth writes.
    void drawTranslucent(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint);
    // Additive (one, one), unlit, faded by fog, drawn last without depth writes: the game's TRAdditive* effects.
    void drawAdditive(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, SDL_GPUTexture* texture);
    // The game's TRAdditiveAnisoSpec* effects (holograms): additive, with detail/decal layers and a view-angle light ramp.
    void drawAnisoAdditive(const GpuMesh& mesh, const Mat4& model, const AnisoMaterial& material);
    void drawTerrain(const GpuMesh& mesh, const TerrainMaterial& material);
    void record(SDL_GPUCommandBuffer* commands);
    size_t queuedDraws() const { return draws_.size() + translucentDraws_.size() + additiveDraws_.size(); }

private:
    struct DrawItem {
        const GpuMesh* mesh;
        Mat4 model;
        Vec4f tint;
        bool lit;
        SDL_GPUTexture* texture;
        bool alphaTest;
        const TerrainMaterial* terrain;          // non-null: terrain draw
        bool translucent;
        Vec4f hue;
        SDL_GPUTexture* glow;                    // never null for scene draws (black_ when none)
        bool additive = false;
        SDL_GPUTexture* detail = nullptr;        // aniso additive layers (null: defaults bound)
        SDL_GPUTexture* decal = nullptr;
        SDL_GPUTexture* light = nullptr;
        Vec4f specular = Vec4f{0, 0, 0, 0};
        Vec4f layers = Vec4f{0, 0, 0, 0};
        bool aniso = false;
    };
    SDL_GPUShader* createShader(const unsigned char* dxil, size_t dxilSize, const unsigned char* spirv, size_t spirvSize,
                                const char* entryPoint, SDL_GPUShaderStage stage, uint32_t samplers);
    SDL_GPUGraphicsPipeline* createPipeline(SDL_GPUShader* vertexShader, SDL_GPUShader* fragmentShader, SDL_GPUPrimitiveType primitive,
                                            int blend = 0);
    void recordItem(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const DrawItem& item, SDL_GPUGraphicsPipeline*& bound);

    SDL_GPUDevice* device_;
    SDL_GPUShader* sceneVertex_ = nullptr;
    SDL_GPUShader* sceneFragment_ = nullptr;
    SDL_GPUShader* terrainVertex_ = nullptr;
    SDL_GPUShader* terrainFragment_ = nullptr;
    SDL_GPUGraphicsPipeline* trianglePipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* linePipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* terrainPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* translucentPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* additivePipeline_ = nullptr;
    // Sun shadow map (depth from the sun, orthographic, centred on the camera, texel-snapped).
    static constexpr uint32_t shadowSize = 2048;
    static constexpr float shadowExtent = 120.0f;   // half width in metres
    static constexpr float shadowDepth = 1200.0f;
    SDL_GPUShader* shadowFragment_ = nullptr;
    SDL_GPUGraphicsPipeline* shadowPipeline_ = nullptr;
    SDL_GPUTexture* shadowMap_ = nullptr;
    SDL_GPUSampler* shadowSampler_ = nullptr;     // comparison sampler
    Mat4 shadowMatrix_;
    bool shadowsActive_ = false;
    void recordShadows(SDL_GPUCommandBuffer* commands);
    SDL_GPUSampler* linearSampler_ = nullptr;   // trilinear, repeat
    SDL_GPUSampler* pointSampler_ = nullptr;    // nearest, clamp (terrain data textures)
    SDL_GPUSampler* clampSampler_ = nullptr;    // trilinear, clamp (lookup ramps)
    SDL_GPUTexture* white_ = nullptr;
    SDL_GPUTexture* black_ = nullptr;          // default glow
    SDL_GPUTextureFormat colourFormat_ = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUTextureFormat depthFormat_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    SDL_GPUTexture* colour_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    Mat4 viewProjection_;
    Vec4f towardsLight_;
    SDL_FColor clearColour_ = {0, 0, 0, 1};
    SceneLighting lighting_;
    Vec4f cameraPosition_;
    std::vector<DrawItem> draws_;
    std::vector<DrawItem> translucentDraws_;
    std::vector<DrawItem> additiveDraws_;
};
