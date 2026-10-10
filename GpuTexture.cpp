#include "GpuTexture.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

struct Region {
    uint32_t mipLevel = 0;
    uint32_t layer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t offset = 0;                 // into the staging blob
};

[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string(what) + ": " + SDL_GetError()); }

SDL_GPUTexture* createTexture(SDL_GPUDevice* device, SDL_GPUTextureFormat format, SDL_GPUTextureType type, uint32_t width,
                              uint32_t height, uint32_t layers, uint32_t levels) {
    SDL_GPUTextureCreateInfo info = {};
    info.type = type;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = layers;
    info.num_levels = levels;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    if (!texture) fail("SDL_CreateGPUTexture");
    return texture;
}

// Uploads every region from one staging blob in a single copy pass.
void uploadRegions(SDL_GPUDevice* device, SDL_GPUTexture* texture, const std::vector<uint8_t>& blob, const std::vector<Region>& regions) {
    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = uint32_t(blob.size());
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (!transfer) fail("SDL_CreateGPUTransferBuffer");
    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    std::memcpy(mapped, blob.data(), blob.size());
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    for (const Region& r : regions) {
        SDL_GPUTextureTransferInfo source = {};
        source.transfer_buffer = transfer;
        source.offset = uint32_t(r.offset);          // pixels_per_row / rows_per_layer 0 = tightly packed
        SDL_GPUTextureRegion destination = {};
        destination.texture = texture;
        destination.mip_level = r.mipLevel;
        destination.layer = r.layer;
        destination.w = r.width;
        destination.h = r.height;
        destination.d = 1;
        SDL_UploadToGPUTexture(copy, &source, &destination, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
}

void appendBytes(std::vector<uint8_t>& blob, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    blob.insert(blob.end(), bytes, bytes + size);
}

uint32_t mipCount(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    while (width > 1 || height > 1) {
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        ++levels;
    }
    return levels;
}

// Appends a full mip chain of an RGBA image to the blob as regions of one layer.
void appendMipChain(std::vector<uint8_t>& blob, std::vector<Region>& regions, std::vector<tr::Rgba8> level, uint32_t width,
                    uint32_t height, uint32_t layer) {
    for (uint32_t mip = 0;; ++mip) {
        regions.push_back(Region{mip, layer, width, height, blob.size()});
        appendBytes(blob, level.data(), level.size() * sizeof(tr::Rgba8));
        if (width == 1 && height == 1) break;
        level = halveRgba(level, width, height);
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
}

}  // namespace

std::vector<tr::Rgba8> halveRgba(const std::vector<tr::Rgba8>& source, uint32_t width, uint32_t height) {
    const uint32_t w = width > 1 ? width / 2 : 1, h = height > 1 ? height / 2 : 1;
    std::vector<tr::Rgba8> out(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t x0 = std::min(2 * x, width - 1), x1 = std::min(2 * x + 1, width - 1);
            const uint32_t y0 = std::min(2 * y, height - 1), y1 = std::min(2 * y + 1, height - 1);
            const tr::Rgba8& a = source[size_t(y0) * width + x0];
            const tr::Rgba8& b = source[size_t(y0) * width + x1];
            const tr::Rgba8& c = source[size_t(y1) * width + x0];
            const tr::Rgba8& d = source[size_t(y1) * width + x1];
            out[size_t(y) * w + x] = tr::Rgba8{uint8_t((a.red + b.red + c.red + d.red + 2) / 4), uint8_t((a.green + b.green + c.green + d.green + 2) / 4),
                                               uint8_t((a.blue + b.blue + c.blue + d.blue + 2) / 4), uint8_t((a.alpha + b.alpha + c.alpha + d.alpha + 2) / 4)};
        }
    return out;
}

std::vector<tr::Rgba8> resizeRgba(const std::vector<tr::Rgba8>& source, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t width,
                                  uint32_t height) {
    // Halve first while the source is more than twice the target (keeps the bilinear step sharp and alias-free).
    std::vector<tr::Rgba8> current = source;
    uint32_t cw = sourceWidth, ch = sourceHeight;
    while (cw >= width * 2 && ch >= height * 2) {
        current = halveRgba(current, cw, ch);
        cw /= 2;
        ch /= 2;
    }
    std::vector<tr::Rgba8> out(size_t(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
        const float sy = (float(y) + 0.5f) * float(ch) / float(height) - 0.5f;
        const int y0 = std::clamp(int(sy), 0, int(ch) - 1), y1 = std::min(y0 + 1, int(ch) - 1);
        const float fy = std::clamp(sy - float(y0), 0.0f, 1.0f);
        for (uint32_t x = 0; x < width; ++x) {
            const float sx = (float(x) + 0.5f) * float(cw) / float(width) - 0.5f;
            const int x0 = std::clamp(int(sx), 0, int(cw) - 1), x1 = std::min(x0 + 1, int(cw) - 1);
            const float fx = std::clamp(sx - float(x0), 0.0f, 1.0f);
            const tr::Rgba8& a = current[size_t(y0) * cw + x0];
            const tr::Rgba8& b = current[size_t(y0) * cw + x1];
            const tr::Rgba8& c = current[size_t(y1) * cw + x0];
            const tr::Rgba8& d = current[size_t(y1) * cw + x1];
            auto mixChannel = [&](uint8_t pa, uint8_t pb, uint8_t pc, uint8_t pd) {
                const float top = pa + (pb - pa) * fx, bottom = pc + (pd - pc) * fx;
                return uint8_t(std::clamp(top + (bottom - top) * fy + 0.5f, 0.0f, 255.0f));
            };
            out[size_t(y) * width + x] = tr::Rgba8{mixChannel(a.red, b.red, c.red, d.red), mixChannel(a.green, b.green, c.green, d.green),
                                                   mixChannel(a.blue, b.blue, c.blue, d.blue), mixChannel(a.alpha, b.alpha, c.alpha, d.alpha)};
        }
    }
    return out;
}

SDL_GPUTexture* uploadRgbaTexture(SDL_GPUDevice* device, const void* pixels, uint32_t width, uint32_t height) {
    SDL_GPUTexture* texture = createTexture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, width, height, 1, 1);
    std::vector<uint8_t> blob;
    appendBytes(blob, pixels, size_t(width) * height * 4);
    uploadRegions(device, texture, blob, {Region{0, 0, width, height, 0}});
    return texture;
}

SDL_GPUTexture* uploadRgbaTextureMipmapped(SDL_GPUDevice* device, const std::vector<tr::Rgba8>& pixels, uint32_t width, uint32_t height) {
    SDL_GPUTexture* texture = createTexture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, width, height, 1,
                                            mipCount(width, height));
    std::vector<uint8_t> blob;
    std::vector<Region> regions;
    appendMipChain(blob, regions, pixels, width, height, 0);
    uploadRegions(device, texture, blob, regions);
    return texture;
}

SDL_GPUTexture* uploadDdsTexture(SDL_GPUDevice* device, const tr::Bytes& dds) {
    const tr::DdsHeader header = tr::readDdsHeader(dds);
    const SDL_GPUTextureFormat format = tr::ddsIsDxt1(header)   ? SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM
                                        : tr::ddsIsDxt3(header) ? SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM
                                        : tr::ddsIsDxt5(header) ? SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM
                                                                : SDL_GPU_TEXTUREFORMAT_INVALID;
    const bool blockAligned = (header.width & 0x3) == 0 && (header.height & 0x3) == 0;
    if (format != SDL_GPU_TEXTUREFORMAT_INVALID && blockAligned &&
        SDL_GPUTextureSupportsFormat(device, format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        const std::vector<tr::DdsLevel> levels = tr::ddsCompressedLevels(dds, header);
        if (!levels.empty()) {
            SDL_GPUTexture* texture = createTexture(device, format, SDL_GPU_TEXTURETYPE_2D, header.width, header.height, 1, uint32_t(levels.size()));
            std::vector<uint8_t> blob;
            std::vector<Region> regions;
            for (uint32_t k = 0; k < levels.size(); ++k) {
                regions.push_back(Region{k, 0, levels[k].width, levels[k].height, blob.size()});
                appendBytes(blob, dds.data() + levels[k].offset, levels[k].size);
            }
            uploadRegions(device, texture, blob, regions);
            return texture;
        }
    }
    tr::DdsHeader decoded;
    const std::vector<tr::Rgba8> pixels = tr::decodeDds(dds, decoded);
    return uploadRgbaTextureMipmapped(device, pixels, decoded.width, decoded.height);
}

SDL_GPUTexture* uploadRgbaCubeArray(SDL_GPUDevice* device, const std::vector<std::vector<tr::Rgba8>>& faces, uint32_t size) {
    const uint32_t count = uint32_t(faces.size());
    SDL_GPUTexture* texture = createTexture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_CUBE_ARRAY, size, size, count,
                                            mipCount(size, size));
    std::vector<uint8_t> blob;
    std::vector<Region> regions;
    for (uint32_t layer = 0; layer < count; ++layer) appendMipChain(blob, regions, faces[layer], size, size, layer);
    uploadRegions(device, texture, blob, regions);
    return texture;
}

SDL_GPUTexture* uploadRgbaArray(SDL_GPUDevice* device, const std::vector<std::vector<tr::Rgba8>>& layers, uint32_t size) {
    const uint32_t count = std::max<uint32_t>(1, uint32_t(layers.size()));
    SDL_GPUTexture* texture = createTexture(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D_ARRAY, size, size, count,
                                            mipCount(size, size));
    std::vector<uint8_t> blob;
    std::vector<Region> regions;
    for (uint32_t layer = 0; layer < layers.size(); ++layer) appendMipChain(blob, regions, layers[layer], size, size, layer);
    if (layers.empty()) appendMipChain(blob, regions, std::vector<tr::Rgba8>(size_t(size) * size, tr::Rgba8{128, 128, 128, 255}), size, size, 0);
    uploadRegions(device, texture, blob, regions);
    return texture;
}
