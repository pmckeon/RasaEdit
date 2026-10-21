#pragma once

#include "trformats.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

// Top-down hill-shaded image of a map's terrain, uploaded as an SDL GPU texture
// (usable directly as an ImGui texture: ImTextureID is SDL_GPUTexture* in imgui 1.92.2+).
// Image row 0 is the terrain's minimum z; draw it with z pointing up the screen (flip V).
class TerrainOverview {
public:
    TerrainOverview(SDL_GPUDevice* device, const tr::Terrain& terrain, uint32_t maxSize = 1024);
    ~TerrainOverview();
    TerrainOverview(const TerrainOverview&) = delete;
    TerrainOverview& operator=(const TerrainOverview&) = delete;

    SDL_GPUTexture* texture() const { return texture_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    // World-space rectangle the image covers (x across, z up).
    float minX() const { return minX_; }
    float maxX() const { return maxX_; }
    float minZ() const { return minZ_; }
    float maxZ() const { return maxZ_; }

    // CPU part, exposed for testing: RGBA8 pixels, width x height, row 0 = minimum z.
    static std::vector<uint8_t> buildImage(const tr::Terrain& terrain, uint32_t maxSize, uint32_t& width, uint32_t& height,
                                           uint32_t& step);

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    float minX_ = 0, maxX_ = 0, minZ_ = 0, maxZ_ = 0;
};
