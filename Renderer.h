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
};

inline uint32_t packColour(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    return uint32_t(red) | (uint32_t(green) << 8) | (uint32_t(blue) << 16) | (uint32_t(alpha) << 24);
}

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
    void beginFrame(const Mat4& viewProjection, const tr::Vec3& towardsLight, SDL_FColor clearColour);
    // texture nullptr draws untextured (white); alphaTest discards texels with alpha below 0.5.
    void draw(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint, bool lit, SDL_GPUTexture* texture = nullptr, bool alphaTest = false);
    // Alpha-blended (tint.w is the opacity), drawn after all opaque draws, without depth writes.
    void drawTranslucent(const GpuMesh& mesh, const Mat4& model, const Vec4f& tint);
    void drawTerrain(const GpuMesh& mesh, const TerrainMaterial& material);
    void record(SDL_GPUCommandBuffer* commands);
    size_t queuedDraws() const { return draws_.size() + translucentDraws_.size(); }

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
    };
    SDL_GPUShader* createShader(const unsigned char* dxil, size_t dxilSize, const unsigned char* spirv, size_t spirvSize,
                                const char* entryPoint, SDL_GPUShaderStage stage, uint32_t samplers);
    SDL_GPUGraphicsPipeline* createPipeline(SDL_GPUShader* vertexShader, SDL_GPUShader* fragmentShader, SDL_GPUPrimitiveType primitive,
                                            bool blended = false);
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
    SDL_GPUSampler* linearSampler_ = nullptr;   // trilinear, repeat
    SDL_GPUSampler* pointSampler_ = nullptr;    // nearest, clamp (terrain data textures)
    SDL_GPUTexture* white_ = nullptr;
    SDL_GPUTextureFormat colourFormat_ = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUTextureFormat depthFormat_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    SDL_GPUTexture* colour_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    Mat4 viewProjection_;
    Vec4f towardsLight_;
    SDL_FColor clearColour_ = {0, 0, 0, 1};
    std::vector<DrawItem> draws_;
    std::vector<DrawItem> translucentDraws_;
};
