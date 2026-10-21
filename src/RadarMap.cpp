#include "RadarMap.h"
#include "GpuTexture.h"

std::vector<tr::Rgba8> RadarMap::buildImage(const std::string& radarArchivePath, int32_t mapTemplateId, int32_t regionId,
                                            const tr::RadarInfo& info, uint32_t maxSize, uint32_t& width, uint32_t& height,
                                            int32_t& tilesPresent) {
    const int32_t columns = tr::radarColumns(info), rows = tr::radarRows(info);
    const uint32_t tileSize = uint32_t(tr::radarMetresPerTile());
    width = uint32_t(columns) * tileSize;
    height = uint32_t(rows) * tileSize;
    std::vector<tr::Rgba8> image(size_t(width) * height);       // transparent where tiles are missing
    tilesPresent = 0;
    const tr::GlmArchive archive(radarArchivePath);
    for (int32_t n = 0; n < columns * rows; ++n) {
        const tr::GlmEntry* entry = archive.find(tr::radarTileName(mapTemplateId, regionId, n));
        if (!entry) continue;
        tr::DdsHeader header;
        const std::vector<tr::Rgba8> tile = tr::decodeDds(archive.read(*entry), header);
        const uint32_t x0 = uint32_t(n % columns) * tileSize, y0 = uint32_t(n / columns) * tileSize;
        for (uint32_t y = 0; y < header.height && y < tileSize; ++y)
            for (uint32_t x = 0; x < header.width && x < tileSize; ++x)
                image[size_t(y0 + y) * width + x0 + x] = tile[size_t(y) * header.width + x];
        ++tilesPresent;
    }
    while (width > maxSize || height > maxSize) {                // 2x2 box filter
        const uint32_t w = width / 2, h = height / 2;
        std::vector<tr::Rgba8> half(size_t(w) * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const tr::Rgba8& a = image[size_t(2 * y) * width + 2 * x];
                const tr::Rgba8& b = image[size_t(2 * y) * width + 2 * x + 1];
                const tr::Rgba8& c = image[size_t(2 * y + 1) * width + 2 * x];
                const tr::Rgba8& d = image[size_t(2 * y + 1) * width + 2 * x + 1];
                half[size_t(y) * w + x] = tr::Rgba8{uint8_t((a.red + b.red + c.red + d.red) / 4), uint8_t((a.green + b.green + c.green + d.green) / 4),
                                                    uint8_t((a.blue + b.blue + c.blue + d.blue) / 4), uint8_t((a.alpha + b.alpha + c.alpha + d.alpha) / 4)};
            }
        image.swap(half);
        width = w;
        height = h;
    }
    return image;
}

bool RadarMap::hasTiles(const std::string& radarArchivePath, int32_t mapTemplateId, int32_t regionId) {
    const tr::GlmArchive archive(radarArchivePath);
    return archive.find(tr::radarTileName(mapTemplateId, regionId, 0)) != nullptr;
}

RadarMap::RadarMap(SDL_GPUDevice* device, const std::string& radarArchivePath, int32_t mapTemplateId, int32_t regionId,
                   const tr::RadarInfo& info) : device_(device) {
    uint32_t width = 0, height = 0;
    const std::vector<tr::Rgba8> image = buildImage(radarArchivePath, mapTemplateId, regionId, info, 8192, width, height, tilesPresent_);
    tilesTotal_ = tr::radarColumns(info) * tr::radarRows(info);
    leftX_ = tr::radarLeftX(info);
    rightX_ = leftX_ + float(tr::radarColumns(info) * tr::radarMetresPerTile());
    topZ_ = tr::radarTopZ(info);
    bottomZ_ = topZ_ - float(tr::radarRows(info) * tr::radarMetresPerTile());
    texture_ = uploadRgbaTexture(device_, image.data(), width, height);
}

RadarMap::~RadarMap() {
    if (texture_) SDL_ReleaseGPUTexture(device_, texture_);
}
