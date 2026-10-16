#include "image.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tr {

// ---- PNG ----

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void putBe32(Bytes& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

Bytes inflateAll(const Bytes& in, size_t expected) {
    Bytes out(expected);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) throw FormatError("PNG: zlib init failed");
    z.next_in = const_cast<Bytes::value_type*>(in.data());
    z.avail_in = uInt(in.size());
    z.next_out = out.data();
    z.avail_out = uInt(out.size());
    const int result = inflate(&z, Z_FINISH);
    const size_t produced = out.size() - z.avail_out;
    inflateEnd(&z);
    if ((result != Z_STREAM_END && result != Z_BUF_ERROR) || produced != expected) throw FormatError("PNG: image data does not inflate to its size");
    return out;
}

uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

}  // namespace

RgbaImage readPng(const Bytes& data) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (data.size() < 8 || std::memcmp(data.data(), signature, 8) != 0) throw FormatError("not a PNG file");
    size_t p = 8;
    uint32_t width = 0, height = 0;
    uint8_t depth = 0, colourType = 0, interlace = 0;
    Bytes idat, palette, transparency;
    while (p + 12 <= data.size()) {
        const uint32_t length = be32(&data[p]);
        const std::string type(reinterpret_cast<const char*>(&data[p + 4]), 4);
        if (p + 12 + size_t(length) > data.size()) throw FormatError("PNG: chunk " + type + " runs past the end");
        const uint8_t* body = &data[p + 8];
        if (type == "IHDR") {
            width = be32(body);
            height = be32(body + 4);
            depth = body[8];
            colourType = body[9];
            interlace = body[12];
        } else if (type == "PLTE") {
            palette.assign(body, body + length);
        } else if (type == "tRNS") {
            transparency.assign(body, body + length);
        } else if (type == "IDAT") {
            idat.insert(idat.end(), body, body + length);
        } else if (type == "IEND") {
            break;
        }
        p += 12 + size_t(length);
    }
    if (width == 0 || height == 0) throw FormatError("PNG: no image header");
    if (interlace != 0) throw FormatError("PNG: interlaced images are not supported (save without interlacing)");
    if (depth != 8 && depth != 16 && !(colourType == 3 && (depth == 1 || depth == 2 || depth == 4)) && !(colourType == 0 && depth < 8))
        throw FormatError("PNG: bit depth " + std::to_string(depth) + " is not supported");
    uint32_t channels = 0;
    switch (colourType) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: throw FormatError("PNG: colour type " + std::to_string(colourType) + " is not supported");
    }
    const size_t bitsPerPixel = size_t(channels) * depth;
    const size_t rowBytes = (size_t(width) * bitsPerPixel + 7) / 8;
    const size_t bpp = std::max<size_t>(1, bitsPerPixel / 8);
    Bytes raw = inflateAll(idat, (rowBytes + 1) * height);
    Bytes rows(rowBytes * height);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t filter = raw[y * (rowBytes + 1)];
        const uint8_t* in = &raw[y * (rowBytes + 1) + 1];
        uint8_t* out = &rows[y * rowBytes];
        const uint8_t* up = y ? &rows[(y - 1) * rowBytes] : nullptr;
        for (size_t x = 0; x < rowBytes; ++x) {
            const int a = x >= bpp ? out[x - bpp] : 0, b = up ? up[x] : 0, c = (up && x >= bpp) ? up[x - bpp] : 0;
            switch (filter) {
                case 0: out[x] = in[x]; break;
                case 1: out[x] = uint8_t(in[x] + a); break;
                case 2: out[x] = uint8_t(in[x] + b); break;
                case 3: out[x] = uint8_t(in[x] + ((a + b) >> 1)); break;
                case 4: out[x] = uint8_t(in[x] + paeth(a, b, c)); break;
                default: throw FormatError("PNG: unknown row filter " + std::to_string(filter));
            }
        }
    }
    RgbaImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(size_t(width) * height);
    auto sample = [&](const uint8_t* row, uint32_t x, uint32_t channel) -> uint32_t {
        if (depth == 16) return row[(size_t(x) * channels + channel) * 2];         // the high byte
        if (depth == 8) return row[size_t(x) * channels + channel];
        const size_t bit = size_t(x) * depth;
        return (row[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
    };
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = &rows[y * rowBytes];
        for (uint32_t x = 0; x < width; ++x) {
            Rgba8& px = image.pixels[size_t(y) * width + x];
            switch (colourType) {
                case 0: {
                    uint32_t g = sample(row, x, 0);
                    if (depth < 8) g = g * 255 / ((1u << depth) - 1);
                    px = Rgba8{uint8_t(g), uint8_t(g), uint8_t(g), 255};
                    break;
                }
                case 2: px = Rgba8{uint8_t(sample(row, x, 0)), uint8_t(sample(row, x, 1)), uint8_t(sample(row, x, 2)), 255}; break;
                case 3: {
                    const uint32_t i = sample(row, x, 0);
                    if (size_t(i) * 3 + 2 >= palette.size()) throw FormatError("PNG: palette index out of range");
                    px = Rgba8{palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2], uint8_t(i < transparency.size() ? transparency[i] : 255)};
                    break;
                }
                case 4: {
                    const uint8_t g = uint8_t(sample(row, x, 0));
                    px = Rgba8{g, g, g, uint8_t(sample(row, x, 1))};
                    break;
                }
                default:
                    px = Rgba8{uint8_t(sample(row, x, 0)), uint8_t(sample(row, x, 1)), uint8_t(sample(row, x, 2)), uint8_t(sample(row, x, 3))};
                    break;
            }
        }
    }
    return image;
}

Bytes writePng(const RgbaImage& image, bool withAlpha) {
    const uint32_t channels = withAlpha ? 4 : 3;
    const size_t rowBytes = size_t(image.width) * channels;
    Bytes raw;
    raw.reserve((rowBytes + 1) * image.height);
    for (uint32_t y = 0; y < image.height; ++y) {
        // Filter "up" for every row but the first (a fixed choice that compresses textures well enough).
        const uint8_t filter = y ? 2 : 0;
        raw.push_back(filter);
        for (uint32_t x = 0; x < image.width; ++x) {
            const Rgba8& px = image.pixels[size_t(y) * image.width + x];
            const uint8_t v[4] = {px.red, px.green, px.blue, px.alpha};
            for (uint32_t c = 0; c < channels; ++c) {
                uint8_t value = v[c];
                if (filter == 2) {
                    const Rgba8& above = image.pixels[size_t(y - 1) * image.width + x];
                    const uint8_t u[4] = {above.red, above.green, above.blue, above.alpha};
                    value = uint8_t(value - u[c]);
                }
                raw.push_back(value);
            }
        }
    }
    uLongf packedSize = compressBound(uLong(raw.size()));
    Bytes packed(packedSize);
    if (compress2(packed.data(), &packedSize, raw.data(), uLong(raw.size()), 6) != Z_OK) throw FormatError("PNG: zlib compression failed");
    packed.resize(packedSize);

    Bytes out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* type, const Bytes& body) {
        putBe32(out, uint32_t(body.size()));
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), body.begin(), body.end());
        putBe32(out, uint32_t(crc32(0, &out[start], uInt(out.size() - start))));
    };
    Bytes header;
    putBe32(header, image.width);
    putBe32(header, image.height);
    header.push_back(8);
    header.push_back(withAlpha ? 6 : 2);
    header.push_back(0);
    header.push_back(0);
    header.push_back(0);
    chunk("IHDR", header);
    chunk("IDAT", packed);
    chunk("IEND", Bytes());
    return out;
}

// ---- DDS ----

namespace {

struct Colour {
    float r = 0, g = 0, b = 0;
};

uint16_t pack565(const Colour& c) {
    auto q = [](float v, int bits) { return uint32_t(std::lround(std::min(std::max(v, 0.0f), 1.0f) * float((1 << bits) - 1))); };
    return uint16_t((q(c.r, 5) << 11) | (q(c.g, 6) << 5) | q(c.b, 5));
}

Colour unpack565(uint16_t v) {
    return Colour{float((v >> 11) & 0x1F) / 31.0f, float((v >> 5) & 0x3F) / 63.0f, float(v & 0x1F) / 31.0f};
}

float distance2(const Colour& a, const Colour& b) {
    const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

// A BC1 colour block (8 bytes). `transparent`: pixels with alpha below 128 (3-colour mode, index 3 = transparent black).
void encodeColourBlock(const Rgba8 (&px)[16], bool allowTransparent, uint8_t* out) {
    Colour c[16];
    bool transparent[16];
    bool anyTransparent = false;
    int opaque = 0;
    for (int i = 0; i < 16; ++i) {
        c[i] = Colour{px[i].red / 255.0f, px[i].green / 255.0f, px[i].blue / 255.0f};
        transparent[i] = allowTransparent && px[i].alpha < 128;
        anyTransparent = anyTransparent || transparent[i];
        if (!transparent[i]) ++opaque;
    }
    // Endpoints along the principal axis of the opaque colours, refined by least squares.
    Colour mean;
    for (int i = 0; i < 16; ++i)
        if (!transparent[i]) {
            mean.r += c[i].r;
            mean.g += c[i].g;
            mean.b += c[i].b;
        }
    if (opaque > 0) {
        mean.r /= float(opaque);
        mean.g /= float(opaque);
        mean.b /= float(opaque);
    }
    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        if (transparent[i]) continue;
        const float r = c[i].r - mean.r, g = c[i].g - mean.g, b = c[i].b - mean.b;
        cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
    }
    Colour axis{1, 1, 1};
    for (int it = 0; it < 8; ++it) {
        const Colour next{cov[0] * axis.r + cov[1] * axis.g + cov[2] * axis.b, cov[1] * axis.r + cov[3] * axis.g + cov[4] * axis.b,
                          cov[2] * axis.r + cov[4] * axis.g + cov[5] * axis.b};
        const float n = std::sqrt(next.r * next.r + next.g * next.g + next.b * next.b);
        if (n < 1e-12f) break;
        axis = Colour{next.r / n, next.g / n, next.b / n};
    }
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 16; ++i) {
        if (transparent[i]) continue;
        const float t = (c[i].r - mean.r) * axis.r + (c[i].g - mean.g) * axis.g + (c[i].b - mean.b) * axis.b;
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    if (opaque == 0) lo = hi = 0;
    Colour e0{mean.r + axis.r * hi, mean.g + axis.g * hi, mean.b + axis.b * hi};
    Colour e1{mean.r + axis.r * lo, mean.g + axis.g * lo, mean.b + axis.b * lo};
    const bool threeColour = anyTransparent;
    uint32_t indices = 0;
    uint16_t a = 0, b = 0;
    for (int pass = 0; pass < 3; ++pass) {
        a = pack565(e0);
        b = pack565(e1);
        if (threeColour ? a > b : a < b) std::swap(a, b);
        const Colour ca = unpack565(a), cb = unpack565(b);
        Colour palette[4];
        palette[0] = ca;
        palette[1] = cb;
        if (!threeColour && a != b) {
            palette[2] = Colour{(2 * ca.r + cb.r) / 3, (2 * ca.g + cb.g) / 3, (2 * ca.b + cb.b) / 3};
            palette[3] = Colour{(ca.r + 2 * cb.r) / 3, (ca.g + 2 * cb.g) / 3, (ca.b + 2 * cb.b) / 3};
        } else {
            palette[2] = Colour{(ca.r + cb.r) / 2, (ca.g + cb.g) / 2, (ca.b + cb.b) / 2};
            palette[3] = Colour{};
        }
        const int choices = (!threeColour && a != b) ? 4 : 3;
        indices = 0;
        // Least-squares refit of the two endpoints from the chosen indices (weights 1, 0, 2/3, 1/3 or 1, 0, 1/2).
        float aa = 0, bb = 0, ab = 0;
        Colour ax, bx;
        for (int i = 0; i < 16; ++i) {
            uint32_t best = 0;
            if (transparent[i]) {
                best = 3;
            } else {
                float bestD = 1e30f;
                for (int k = 0; k < choices; ++k) {
                    const float d = distance2(c[i], palette[k]);
                    if (d < bestD) { bestD = d; best = uint32_t(k); }
                }
                const float w = best == 0 ? 1.0f : best == 1 ? 0.0f : choices == 4 ? (best == 2 ? 2.0f / 3 : 1.0f / 3) : 0.5f;
                aa += w * w; bb += (1 - w) * (1 - w); ab += w * (1 - w);
                ax.r += w * c[i].r; ax.g += w * c[i].g; ax.b += w * c[i].b;
                bx.r += (1 - w) * c[i].r; bx.g += (1 - w) * c[i].g; bx.b += (1 - w) * c[i].b;
            }
            indices |= best << (2 * i);
        }
        const float det = aa * bb - ab * ab;
        if (pass == 2 || std::fabs(det) < 1e-8f) break;
        e0 = Colour{(ax.r * bb - bx.r * ab) / det, (ax.g * bb - bx.g * ab) / det, (ax.b * bb - bx.b * ab) / det};
        e1 = Colour{(bx.r * aa - ax.r * ab) / det, (bx.g * aa - ax.g * ab) / det, (bx.b * aa - ax.b * ab) / det};
        // The refit endpoints are for palette[0] / palette[1] as ordered; keep e0 = the first.
        const Colour p0 = unpack565(a);
        if (distance2(p0, unpack565(pack565(e0))) > distance2(p0, unpack565(pack565(e1)))) std::swap(e0, e1);
    }
    out[0] = uint8_t(a);
    out[1] = uint8_t(a >> 8);
    out[2] = uint8_t(b);
    out[3] = uint8_t(b >> 8);
    for (int k = 0; k < 4; ++k) out[4 + k] = uint8_t(indices >> (8 * k));
}

// A BC3 alpha block (8 bytes): 8-value mode, nearest of the interpolated levels.
void encodeAlphaBlock(const Rgba8 (&px)[16], uint8_t* out) {
    uint8_t lo = 255, hi = 0;
    for (const Rgba8& p : px) {
        lo = std::min(lo, p.alpha);
        hi = std::max(hi, p.alpha);
    }
    out[0] = hi;
    out[1] = lo;
    float levels[8] = {float(hi), float(lo)};
    for (int k = 1; k < 7; ++k) levels[k + 1] = (float(7 - k) * hi + float(k) * lo) / 7.0f;
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        uint64_t best = 0;
        float bestD = 1e30f;
        for (int k = 0; k < (hi == lo ? 1 : 8); ++k) {
            const float d = std::fabs(levels[k] - px[i].alpha);
            if (d < bestD) { bestD = d; best = uint64_t(k); }
        }
        bits |= best << (3 * i);
    }
    for (int k = 0; k < 6; ++k) out[2 + k] = uint8_t(bits >> (8 * k));
}

RgbaImage halve(const RgbaImage& in) {
    RgbaImage out;
    out.width = std::max(1u, in.width / 2);
    out.height = std::max(1u, in.height / 2);
    out.pixels.resize(size_t(out.width) * out.height);
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x) {
            uint32_t sum[4] = {0, 0, 0, 0}, n = 0;
            for (uint32_t dy = 0; dy < 2; ++dy)
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const uint32_t sx = std::min(in.width - 1, x * 2 + dx), sy = std::min(in.height - 1, y * 2 + dy);
                    const Rgba8& p = in.pixels[size_t(sy) * in.width + sx];
                    sum[0] += p.red; sum[1] += p.green; sum[2] += p.blue; sum[3] += p.alpha;
                    ++n;
                }
            out.pixels[size_t(y) * out.width + x] = Rgba8{uint8_t((sum[0] + n / 2) / n), uint8_t((sum[1] + n / 2) / n), uint8_t((sum[2] + n / 2) / n),
                                                          uint8_t((sum[3] + n / 2) / n)};
        }
    return out;
}

void putLe32(Bytes& out, uint32_t v) {
    for (int b = 0; b < 4; ++b) out.push_back(uint8_t(v >> (8 * b)));
}

void encodeLevel(const RgbaImage& image, DdsEncoding encoding, Bytes& out) {
    if (encoding == DdsEncoding::Argb8888) {
        for (const Rgba8& p : image.pixels) {
            out.push_back(p.blue);
            out.push_back(p.green);
            out.push_back(p.red);
            out.push_back(p.alpha);
        }
        return;
    }
    bool anyTransparent = false;
    if (encoding == DdsEncoding::Dxt1)
        for (const Rgba8& p : image.pixels) anyTransparent = anyTransparent || p.alpha < 128;
    for (uint32_t by = 0; by < (image.height + 3) / 4; ++by)
        for (uint32_t bx = 0; bx < (image.width + 3) / 4; ++bx) {
            Rgba8 px[16];
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t x = std::min(image.width - 1, bx * 4 + i % 4), y = std::min(image.height - 1, by * 4 + i / 4);
                px[i] = image.pixels[size_t(y) * image.width + x];
            }
            uint8_t block[16];
            if (encoding == DdsEncoding::Dxt1) {
                encodeColourBlock(px, anyTransparent, block);
                out.insert(out.end(), block, block + 8);
                continue;
            }
            if (encoding == DdsEncoding::Dxt3) {
                auto nibble = [](uint8_t alpha) { return uint8_t((uint32_t(alpha) * 15 + 127) / 255); };
                for (int k = 0; k < 8; ++k) block[k] = uint8_t(nibble(px[2 * k].alpha) | (nibble(px[2 * k + 1].alpha) << 4));
            } else {
                encodeAlphaBlock(px, block);
            }
            encodeColourBlock(px, false, block + 8);
            out.insert(out.end(), block, block + 16);
        }
}

}  // namespace

DdsEncoding ddsEncodingLike(const DdsHeader& original) {
    if (ddsIsDxt1(original)) return DdsEncoding::Dxt1;
    if (ddsIsDxt3(original)) return DdsEncoding::Dxt3;
    if (ddsIsDxt5(original)) return DdsEncoding::Dxt5;
    return DdsEncoding::Argb8888;
}

const char* ddsEncodingName(DdsEncoding encoding) {
    switch (encoding) {
        case DdsEncoding::Dxt1: return "DXT1";
        case DdsEncoding::Dxt3: return "DXT3";
        case DdsEncoding::Dxt5: return "DXT5";
        default: return "ARGB 32-bit";
    }
}

Bytes encodeDds(const RgbaImage& image, DdsEncoding encoding, bool mipmaps) {
    if (image.width == 0 || image.height == 0 || image.pixels.size() != size_t(image.width) * image.height) throw FormatError("DDS: empty image");
    uint32_t levels = 1;
    if (mipmaps)
        for (uint32_t w = image.width, h = image.height; w > 1 || h > 1; w = std::max(1u, w / 2), h = std::max(1u, h / 2)) ++levels;
    const bool block = encoding != DdsEncoding::Argb8888;
    const uint32_t blockBytes = encoding == DdsEncoding::Dxt1 ? 8 : 16;
    const uint32_t topSize = block ? ((image.width + 3) / 4) * ((image.height + 3) / 4) * blockBytes : image.width * 4;
    Bytes out = {'D', 'D', 'S', ' '};
    putLe32(out, 124);
    putLe32(out, 0x1 | 0x2 | 0x4 | 0x1000 | (levels > 1 ? 0x20000 : 0) | (block ? 0x80000 : 0x8));   // caps, height, width, pixel format, mips, linear size / pitch
    putLe32(out, image.height);
    putLe32(out, image.width);
    putLe32(out, topSize);
    putLe32(out, 0);                                       // depth
    putLe32(out, levels);
    for (int k = 0; k < 11; ++k) putLe32(out, 0);
    putLe32(out, 32);                                      // pixel format
    if (block) {
        putLe32(out, 0x4);
        putLe32(out, encoding == DdsEncoding::Dxt1 ? 0x31545844 : encoding == DdsEncoding::Dxt3 ? 0x33545844 : 0x35545844);
        for (int k = 0; k < 5; ++k) putLe32(out, 0);
    } else {
        putLe32(out, 0x41);                                // RGB + alpha
        putLe32(out, 0);
        putLe32(out, 32);
        putLe32(out, 0x00FF0000);
        putLe32(out, 0x0000FF00);
        putLe32(out, 0x000000FF);
        putLe32(out, 0xFF000000);
    }
    putLe32(out, 0x1000 | (levels > 1 ? 0x400008 : 0));    // TEXTURE, COMPLEX + MIPMAP
    for (int k = 0; k < 4; ++k) putLe32(out, 0);
    RgbaImage level = image;
    for (uint32_t k = 0; k < levels; ++k) {
        encodeLevel(level, encoding, out);
        if (k + 1 < levels) level = halve(level);
    }
    return out;
}

RgbaImage ddsImage(const Bytes& dds) {
    DdsHeader header;
    RgbaImage image;
    image.pixels = decodeDds(dds, header);
    image.width = header.width;
    image.height = header.height;
    return image;
}

}  // namespace tr
