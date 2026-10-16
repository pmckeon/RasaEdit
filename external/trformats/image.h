#pragma once

// RGBA images in and out of the editor: PNG files (for Blender and other tools) and DDS textures (for the game).
// PNG: read 8 / 16-bit grey, grey + alpha, RGB, RGBA and palette images (non-interlaced; Blender writes these), written as
// 8-bit RGBA or RGB. DDS: written as DXT1 (with 1-bit alpha when needed), DXT3, DXT5 or 32-bit ARGB, with a full mip chain
// (box filter) - the formats of the shipped model textures.

#include "dds.h"

#include <cstdint>
#include <vector>

namespace tr {

struct RgbaImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<Rgba8> pixels;         // row 0 = top
};

RgbaImage readPng(const Bytes& data);
Bytes writePng(const RgbaImage& image, bool withAlpha = true);

enum class DdsEncoding : uint8_t { Dxt1, Dxt3, Dxt5, Argb8888 };
// The encoding to re-encode an existing texture in: its own block format, else 32-bit ARGB.
DdsEncoding ddsEncodingLike(const DdsHeader& original);
const char* ddsEncodingName(DdsEncoding encoding);
Bytes encodeDds(const RgbaImage& image, DdsEncoding encoding, bool mipmaps = true);

// The top level of a DDS texture as an image (decodeDds).
RgbaImage ddsImage(const Bytes& dds);

}  // namespace tr
