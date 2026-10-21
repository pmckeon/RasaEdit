#include "TerrainOverview.h"
#include "GpuTexture.h"

#include <algorithm>
#include <cmath>

namespace {

struct Colour {
    float red;
    float green;
    float blue;
};

Colour lerp(const Colour& a, const Colour& b, float t) {
    return Colour{a.red + (b.red - a.red) * t, a.green + (b.green - a.green) * t, a.blue + (b.blue - a.blue) * t};
}

// Low ground dark green, mid slopes olive, high ground pale rock.
Colour heightTint(float t) {
    const Colour low{60, 85, 45}, mid{125, 115, 80}, high{205, 195, 175};
    return t < 0.5f ? lerp(low, mid, t * 2.0f) : lerp(mid, high, (t - 0.5f) * 2.0f);
}

}  // namespace

std::vector<uint8_t> TerrainOverview::buildImage(const tr::Terrain& terrain, uint32_t maxSize, uint32_t& width, uint32_t& height,
                                                 uint32_t& step) {
    const uint32_t rows = terrain.boundedRows();
    const uint32_t columns = terrain.boundedColumns();
    step = std::max<uint32_t>(1, (std::max(rows, columns) + maxSize - 1) / maxSize);
    width = (columns - 1) / step + 1;
    height = (rows - 1) / step + 1;

    float lowest = 1e30f, highest = -1e30f;
    for (uint32_t r = 0; r < rows; r += step)
        for (uint32_t c = 0; c < columns; c += step)
            if (terrain.hasData(r, c)) {
                const float h = terrain.worldHeightAt(r, c);
                lowest = std::min(lowest, h);
                highest = std::max(highest, h);
            }
    const float range = highest > lowest ? highest - lowest : 1.0f;

    auto heightOr = [&](int64_t r, int64_t c, float fallback) {
        if (r < 0 || c < 0 || r >= int64_t(rows) || c >= int64_t(columns)) return fallback;
        return terrain.hasData(uint32_t(r), uint32_t(c)) ? terrain.worldHeightAt(uint32_t(r), uint32_t(c)) : fallback;
    };

    // Light from -x, +z (upper left of the overview), raised.
    const float lx = -0.55f, ly = 0.7f, lz = 0.45f;
    const float spacing = terrain.header.spacing * float(step);
    std::vector<uint8_t> pixels(size_t(width) * height * 4, 0);
    for (uint32_t i = 0; i < height; ++i) {
        for (uint32_t j = 0; j < width; ++j) {
            const uint32_t r = i * step, c = j * step;
            uint8_t* p = &pixels[(size_t(i) * width + j) * 4];
            if (!terrain.hasData(r, c)) continue;                       // transparent where there is no terrain
            const float h = terrain.worldHeightAt(r, c);
            const float dx = (heightOr(r, int64_t(c) + step, h) - heightOr(r, int64_t(c) - step, h)) / (2.0f * spacing);
            const float dz = (heightOr(int64_t(r) + step, c, h) - heightOr(int64_t(r) - step, c, h)) / (2.0f * spacing);
            const float nl = std::sqrt(dx * dx + 1.0f + dz * dz);
            const float shade = std::clamp((-dx * lx + ly - dz * lz) / nl, 0.18f, 1.0f);
            const Colour tint = heightTint((h - lowest) / range);
            p[0] = uint8_t(std::min(255.0f, tint.red * shade * 1.25f));
            p[1] = uint8_t(std::min(255.0f, tint.green * shade * 1.25f));
            p[2] = uint8_t(std::min(255.0f, tint.blue * shade * 1.25f));
            p[3] = 255;
        }
    }
    return pixels;
}

TerrainOverview::TerrainOverview(SDL_GPUDevice* device, const tr::Terrain& terrain, uint32_t maxSize) : device_(device) {
    uint32_t step = 1;
    const std::vector<uint8_t> pixels = buildImage(terrain, maxSize, width_, height_, step);
    minX_ = terrain.worldX(0);
    maxX_ = terrain.worldX((width_ - 1) * step);
    minZ_ = terrain.worldZ(0);
    maxZ_ = terrain.worldZ((height_ - 1) * step);

    texture_ = uploadRgbaTexture(device_, pixels.data(), width_, height_);
}

TerrainOverview::~TerrainOverview() {
    if (texture_) SDL_ReleaseGPUTexture(device_, texture_);
}
