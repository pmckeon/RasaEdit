// dds.h - DirectDraw Surface reader: header parsing and decoding of the top mip level to RGBA8.
//
// Supports DXT1 (BC1, with 1-bit alpha), DXT3 (BC2), DXT5 (BC3); uncompressed RGB/RGBA described by
// bit masks (16, 24 or 32 bits per pixel); luminance (L8, A4L4, A8L8, L16; flag 0x20000); alpha only
// (A8; flag 0x2); and 8-bit palettised (flag 0x20: a 256-entry R,G,B,flags palette follows the
// header). DX10 headers are not supported. Layout: "DDS ", DDS_HEADER (124 bytes; pixel format at
// offset 76), then the data.
#pragma once

#include "trformats.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tr {

struct DdsHeader {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 1;
    uint32_t pixelFormatFlags = 0;      // 0x1 has alpha, 0x2 alpha only, 0x4 FourCC, 0x20 palette, 0x40 RGB, 0x20000 luminance
    uint32_t fourCC = 0;                // as stored, e.g. 'DXT1' = bytes "DXT1"
    uint32_t rgbBitCount = 0;
    uint32_t redMask = 0;
    uint32_t greenMask = 0;
    uint32_t blueMask = 0;
    uint32_t alphaMask = 0;
};

struct Rgba8 {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint8_t alpha = 0;
};

inline bool ddsIsFourCC(const DdsHeader& h) { return (h.pixelFormatFlags & 0x4) != 0; }
inline bool ddsIsDxt1(const DdsHeader& h) { return ddsIsFourCC(h) && h.fourCC == 0x31545844; }    // "DXT1"
inline bool ddsIsDxt3(const DdsHeader& h) { return ddsIsFourCC(h) && h.fourCC == 0x33545844; }    // "DXT3"
inline bool ddsIsDxt5(const DdsHeader& h) { return ddsIsFourCC(h) && h.fourCC == 0x35545844; }    // "DXT5"
inline bool ddsIsPalettised(const DdsHeader& h) { return (h.pixelFormatFlags & 0x20) != 0; }
inline bool ddsIsLuminance(const DdsHeader& h) { return (h.pixelFormatFlags & 0x20000) != 0; }
inline bool ddsIsAlphaOnly(const DdsHeader& h) { return (h.pixelFormatFlags & 0x2) != 0 && (h.pixelFormatFlags & 0x40) == 0; }
// Short description of the pixel format, e.g. "DXT1", "RGB 32-bit", "luminance 8-bit", "palette 8-bit".
std::string ddsFormatName(const DdsHeader& h);


DdsHeader readDdsHeader(const Bytes& file);

// Byte layout of each mip level of a block-compressed (DXT1/3/5) file, for uploading the compressed
// data directly to a GPU (BC1/BC2/BC3). Levels that would run past the end of the file are dropped.
struct DdsLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t offset = 0;            // from the start of the file
    size_t size = 0;
};
inline uint32_t ddsBlockBytes(const DdsHeader& h) { return ddsIsDxt1(h) ? 8 : 16; }
std::vector<DdsLevel> ddsCompressedLevels(const Bytes& file, const DdsHeader& header);
// Top mip level as RGBA8, row 0 = top of the image. Throws FormatError for unsupported formats.
std::vector<Rgba8> decodeDds(const Bytes& file, DdsHeader& header);

}  // namespace tr
