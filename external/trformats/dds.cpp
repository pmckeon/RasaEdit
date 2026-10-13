// dds.cpp - see dds.h.
#include "dds.h"

#include <cstdio>
#include <cstring>

namespace tr {

namespace {

uint16_t read16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t read32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

// RGB565 accessors, expanded to 8 bits by replicating the top bits.
uint8_t red565(uint16_t c) { const uint8_t r = uint8_t((c >> 11) & 0x1F); return uint8_t((r << 3) | (r >> 2)); }
uint8_t green565(uint16_t c) { const uint8_t g = uint8_t((c >> 5) & 0x3F); return uint8_t((g << 2) | (g >> 4)); }
uint8_t blue565(uint16_t c) { const uint8_t b = uint8_t(c & 0x1F); return uint8_t((b << 3) | (b >> 2)); }

Rgba8 colour565(uint16_t c) { return Rgba8{red565(c), green565(c), blue565(c), 255}; }

Rgba8 mix(const Rgba8& a, const Rgba8& b, int weightA, int weightB) {
    const int total = weightA + weightB;
    return Rgba8{uint8_t((a.red * weightA + b.red * weightB) / total), uint8_t((a.green * weightA + b.green * weightB) / total),
                 uint8_t((a.blue * weightA + b.blue * weightB) / total), 255};
}

// Colour part of a BC1/BC2/BC3 block. allowTransparent: BC1's 3-colour mode (colour0 <= colour1).
void decodeColourBlock(const uint8_t* block, bool allowTransparent, Rgba8 (&out)[16]) {
    const uint16_t c0 = read16(block);
    const uint16_t c1 = read16(block + 2);
    const uint32_t indices = read32(block + 4);
    Rgba8 palette0 = colour565(c0), palette1 = colour565(c1), palette2, palette3;
    if (c0 > c1 || !allowTransparent) {
        palette2 = mix(palette0, palette1, 2, 1);
        palette3 = mix(palette0, palette1, 1, 2);
    } else {
        palette2 = mix(palette0, palette1, 1, 1);
        palette3 = Rgba8{0, 0, 0, 0};
    }
    for (int k = 0; k < 16; ++k) {
        switch ((indices >> (2 * k)) & 0x3) {
            case 0: out[k] = palette0; break;
            case 1: out[k] = palette1; break;
            case 2: out[k] = palette2; break;
            default: out[k] = palette3; break;
        }
    }
}

// BC2 explicit alpha: 4 bits per pixel.
void decodeExplicitAlpha(const uint8_t* block, Rgba8 (&out)[16]) {
    for (int k = 0; k < 16; ++k) {
        const uint8_t nibble = uint8_t((block[k >> 1] >> ((k & 0x1) * 4)) & 0xF);
        out[k].alpha = uint8_t(nibble * 17);
    }
}

// BC3 interpolated alpha: two endpoints and 3-bit indices.
void decodeInterpolatedAlpha(const uint8_t* block, Rgba8 (&out)[16]) {
    const int a0 = block[0], a1 = block[1];
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= uint64_t(block[2 + k]) << (8 * k);
    for (int k = 0; k < 16; ++k) {
        const int index = int((bits >> (3 * k)) & 0x7);
        int alpha;
        if (index == 0) alpha = a0;
        else if (index == 1) alpha = a1;
        else if (a0 > a1) alpha = ((8 - index) * a0 + (index - 1) * a1) / 7;
        else if (index == 6) alpha = 0;
        else if (index == 7) alpha = 255;
        else alpha = ((6 - index) * a0 + (index - 1) * a1) / 5;
        out[k].alpha = uint8_t(alpha);
    }
}

// Mask helpers for uncompressed formats.
uint32_t maskShift(uint32_t mask) {
    uint32_t shift = 0;
    while (mask != 0 && (mask & 0x1) == 0) { mask >>= 1; ++shift; }
    return shift;
}

uint8_t maskedChannel(uint32_t pixel, uint32_t mask, uint8_t fallback) {
    if (mask == 0) return fallback;
    const uint32_t shift = maskShift(mask);
    const uint32_t maximum = mask >> shift;
    return uint8_t(((pixel & mask) >> shift) * 255 / maximum);
}

}  // namespace

DdsHeader readDdsHeader(const Bytes& file) {
    if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0) throw FormatError("not a DDS file");
    const uint8_t* h = file.data() + 4;
    DdsHeader header;
    header.height = read32(h + 8);
    header.width = read32(h + 12);
    header.mipCount = read32(h + 24) == 0 ? 1 : read32(h + 24);
    header.pixelFormatFlags = read32(h + 76);
    header.fourCC = read32(h + 80);
    header.rgbBitCount = read32(h + 84);
    header.redMask = read32(h + 88);
    header.greenMask = read32(h + 92);
    header.blueMask = read32(h + 96);
    header.alphaMask = read32(h + 100);
    if ((read32(h + 108) & 0x200000) != 0 && read32(h + 20) > 1) header.depth = read32(h + 20);   // caps2 DDSCAPS2_VOLUME, dwDepth
    if (header.width == 0 || header.height == 0 || header.width > 16384 || header.height > 16384) throw FormatError("DDS: bad size");
    return header;
}

std::vector<Rgba8> decodeDdsVolume(const Bytes& file, DdsHeader& header) {
    header = readDdsHeader(file);
    const uint32_t depth = header.depth;
    if (depth > 1 && (ddsIsDxt1(header) || ddsIsDxt3(header) || ddsIsDxt5(header)) && header.height % 4 != 0)
        throw FormatError("DDS volume: slice height not a multiple of 4");
    if (size_t(header.height) * depth > 16384) throw FormatError("DDS volume: too many slices");
    Bytes stacked = file;                                    // the same bytes read as one image depth x as tall
    const uint32_t height = header.height * depth;
    stacked[4 + 8] = uint8_t(height);
    stacked[4 + 9] = uint8_t(height >> 8);
    stacked[4 + 10] = uint8_t(height >> 16);
    stacked[4 + 11] = uint8_t(height >> 24);
    DdsHeader flat;
    std::vector<Rgba8> pixels = decodeDds(stacked, flat);
    return pixels;
}

std::vector<Rgba8> decodeDds(const Bytes& file, DdsHeader& header) {
    header = readDdsHeader(file);
    const uint32_t width = header.width, height = header.height;
    std::vector<Rgba8> pixels(size_t(width) * height);
    const uint8_t* data = file.data() + 128;
    const size_t available = file.size() - 128;

    if (ddsIsDxt1(header) || ddsIsDxt3(header) || ddsIsDxt5(header)) {
        const size_t blockBytes = ddsIsDxt1(header) ? 8 : 16;
        const uint32_t blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
        if (size_t(blocksX) * blocksY * blockBytes > available) throw FormatError("DDS: data shorter than the image");
        Rgba8 texels[16];
        for (uint32_t by = 0; by < blocksY; ++by) {
            for (uint32_t bx = 0; bx < blocksX; ++bx) {
                const uint8_t* block = data + (size_t(by) * blocksX + bx) * blockBytes;
                if (ddsIsDxt1(header)) {
                    decodeColourBlock(block, true, texels);
                } else {
                    decodeColourBlock(block + 8, false, texels);
                    if (ddsIsDxt3(header)) decodeExplicitAlpha(block, texels);
                    else decodeInterpolatedAlpha(block, texels);
                }
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint32_t px = bx * 4 + x, py = by * 4 + y;
                        if (px < width && py < height) pixels[size_t(py) * width + px] = texels[y * 4 + x];
                    }
            }
        }
        return pixels;
    }

    if (ddsIsPalettised(header)) {
        // 256 palette entries (red, green, blue, flags) after the header, then one index byte per pixel.
        if (available < 1024 + size_t(width) * height) throw FormatError("DDS: data shorter than the image");
        const uint8_t* palette = data;
        const uint8_t* indices = data + 1024;
        const bool hasAlpha = (header.pixelFormatFlags & 0x1) != 0;
        for (size_t k = 0; k < size_t(width) * height; ++k) {
            const uint8_t* entry = palette + size_t(indices[k]) * 4;
            pixels[k] = Rgba8{entry[0], entry[1], entry[2], hasAlpha ? entry[3] : uint8_t(255)};
        }
        return pixels;
    }
    if (ddsIsLuminance(header) || ddsIsAlphaOnly(header)) {
        const uint32_t bytesPerPixel = header.rgbBitCount / 8;
        if (bytesPerPixel < 1 || bytesPerPixel > 2) throw FormatError("DDS: unsupported luminance/alpha bit count " + std::to_string(header.rgbBitCount));
        if (size_t(width) * height * bytesPerPixel > available) throw FormatError("DDS: data shorter than the image");
        const bool hasAlpha = (header.pixelFormatFlags & 0x3) != 0;
        for (size_t k = 0; k < size_t(width) * height; ++k) {
            const uint8_t* p = data + k * bytesPerPixel;
            const uint32_t pixel = bytesPerPixel == 2 ? uint32_t(p[0] | (p[1] << 8)) : p[0];
            if (ddsIsAlphaOnly(header)) {
                pixels[k] = Rgba8{255, 255, 255, maskedChannel(pixel, header.alphaMask ? header.alphaMask : 0xFF, 255)};
            } else {
                const uint8_t l = maskedChannel(pixel, header.redMask ? header.redMask : (bytesPerPixel == 2 ? 0xFFFF : 0xFF), 0);
                pixels[k] = Rgba8{l, l, l, hasAlpha ? maskedChannel(pixel, header.alphaMask, 255) : uint8_t(255)};
            }
        }
        return pixels;
    }
    if (ddsIsFourCC(header)) throw FormatError("DDS: unsupported format " + ddsFormatName(header));
    const uint32_t bytesPerPixel = header.rgbBitCount / 8;
    if ((header.pixelFormatFlags & 0x40) == 0 || bytesPerPixel < 2 || bytesPerPixel > 4) throw FormatError("DDS: unsupported format " + ddsFormatName(header));
    if (size_t(width) * height * bytesPerPixel > available) throw FormatError("DDS: data shorter than the image");
    const bool hasAlpha = (header.pixelFormatFlags & 0x1) != 0;
    for (size_t k = 0; k < size_t(width) * height; ++k) {
        const uint8_t* p = data + k * bytesPerPixel;
        uint32_t pixel = 0;
        for (uint32_t b = 0; b < bytesPerPixel; ++b) pixel |= uint32_t(p[b]) << (8 * b);
        pixels[k] = Rgba8{maskedChannel(pixel, header.redMask, 0), maskedChannel(pixel, header.greenMask, 0),
                          maskedChannel(pixel, header.blueMask, 0), hasAlpha ? maskedChannel(pixel, header.alphaMask, 255) : uint8_t(255)};
    }
    return pixels;
}

std::vector<DdsLevel> ddsCompressedLevels(const Bytes& file, const DdsHeader& header) {
    std::vector<DdsLevel> levels;
    if (!ddsIsDxt1(header) && !ddsIsDxt3(header) && !ddsIsDxt5(header)) return levels;
    size_t offset = 128;
    uint32_t width = header.width, height = header.height;
    for (uint32_t level = 0; level < header.mipCount; ++level) {
        const size_t size = size_t((width + 3) / 4) * ((height + 3) / 4) * ddsBlockBytes(header);
        if (offset + size > file.size()) break;
        levels.push_back(DdsLevel{width, height, offset, size});
        offset += size;
        if (width == 1 && height == 1) break;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    return levels;
}

std::string ddsFormatName(const DdsHeader& h) {
    if (ddsIsFourCC(h)) {
        const char text[5] = {char(h.fourCC & 0xFF), char((h.fourCC >> 8) & 0xFF), char((h.fourCC >> 16) & 0xFF), char((h.fourCC >> 24) & 0xFF), 0};
        bool printable = true;
        for (int k = 0; k < 4; ++k) printable = printable && text[k] >= 32 && text[k] < 127;
        return printable ? std::string(text) : "D3DFORMAT " + std::to_string(h.fourCC);
    }
    const std::string bits = std::to_string(h.rgbBitCount) + "-bit";
    if (ddsIsPalettised(h)) return "palette " + bits;
    if (ddsIsLuminance(h)) return std::string((h.pixelFormatFlags & 0x1) ? "luminance+alpha " : "luminance ") + bits;
    if (ddsIsAlphaOnly(h)) return "alpha " + bits;
    if (h.pixelFormatFlags & 0x40) return std::string((h.pixelFormatFlags & 0x1) ? "RGBA " : "RGB ") + bits;
    return "unknown (flags 0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%X", h.pixelFormatFlags); return std::string(b); }() + ")";
}

}  // namespace tr
