#include "UiCanvas.h"

#include "dds.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <tuple>

// FreeType (vcpkg "freetype"): hinted glyphs like the game's GDI ones, on every platform.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_DRIVER_H
#include FT_MODULE_H
#include FT_TRUETYPE_TABLES_H

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int32_t intAttribute(const tr::XmlNode& n, const char* name, int32_t fallback = 0) {
    const std::string v = n.attribute(name);
    return v.empty() ? fallback : int32_t(std::strtol(v.c_str(), nullptr, 10));
}

bool attributeIsTrue(const tr::XmlNode& n, const char* name) { return n.attribute(name) == "true"; }

// The engine's anchor point of a w x h rectangle (tabula_rasa.exe FUN_008b7060; integer halves).
void anchorPoint(int32_t anchor, int32_t w, int32_t h, int32_t& x, int32_t& y) {
    switch (anchor) {
        case 1: x = w / 2; y = 0; break;
        case 2: x = w; y = 0; break;
        case 3: x = w; y = h / 2; break;
        case 4: x = w; y = h; break;
        case 5: x = w / 2; y = h; break;
        case 6: x = 0; y = h; break;
        case 7: x = 0; y = h / 2; break;
        case 8: x = w / 2; y = h / 2; break;
        default: x = 0; y = 0; break;
    }
}

UiCanvas::Rect intersect(const UiCanvas::Rect& a, const UiCanvas::Rect& b) {
    return UiCanvas::Rect{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

std::vector<uint32_t> codepoints(const std::string& s) {     // UTF-8 (invalid bytes taken as Latin-1)
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        uint32_t cp = extra == 3 ? c & 0x7 : extra == 2 ? c & 0xF : extra == 1 ? c & 0x1F : c;
        bool ok = true;
        for (int k = 1; k <= extra && ok; ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        if (!ok) { cp = c; extra = 0; }
        out.push_back(cp);
        i += size_t(extra) + 1;
    }
    return out;
}

}  // namespace

// ------------------------------------------------------------------------------------------
// Fonts
// ------------------------------------------------------------------------------------------
// The game's text (tabula_rasa.exe gfxFontImpl.cpp, decompiled 2026-10-15): FUN_006ae9e0 creates a GDI font (CreateFontIndirectA, weight
// 400 or 700 for bold - a synthetic bold on these regular faces -, quality 4 = ANTIALIASED_QUALITY: hinted grayscale) and a 32-bit DIB;
// FUN_006af200 draws a glyph white on transparent: the shadow (flag 4) in 0x00FFFF at (x + 1, 1), the outline (flag 8, no shipped font)
// four times in 0x00FFC0, the glyph in 0xFF00FF at (x, 0), x = -abcA when negative, and packs each pixel into an A4R4G4B4 texel:
// alpha = blue's high nibble (every layer), grey = table[red's high nibble][alpha], table[m][a] = a ? min(15, round(15 m / a)) : m
// (FUN_006ae5a0; 15.0 at 0x00c2b770) - the shadow comes out black, alpha has 16 levels. Cell abcB + max(abcA, 0) + max(abcC, 0) (+ 1
// shadow) by tmHeight; advance abcA + abcB + abcC; kerning pairs from the font.
// Here the glyphs come from FreeType with its TrueType bytecode interpreter in version 35 (the classic, GDI-like hinting), the layers
// combined as GDI blends them into the DIB (blue = main + (1 - main) x shadow, red = main) and packed the game's way.
struct UiCanvas::FontLibrary {
    FT_Library library = nullptr;
    FontLibrary() {
        if (FT_Init_FreeType(&library) != 0) {
            library = nullptr;
            return;
        }
        FT_UInt interpreter = TT_INTERPRETER_VERSION_35;
        FT_Property_Set(library, "truetype", "interpreter-version", &interpreter);
    }
    ~FontLibrary() {
        if (library) FT_Done_FreeType(library);
    }
};

struct UiCanvas::Face {
    FT_Face face = nullptr;
    std::shared_ptr<std::vector<uint8_t>> data;    // FreeType reads the font from this memory
    float winAscent = 0, winDescent = 0;            // OS/2 usWinAscent / usWinDescent in font units (GDI's tmAscent / tmDescent)
    int32_t currentEm = -1;
    struct Glyph {
        int32_t width = 0, height = 0;
        int32_t originX = 0;                        // texel column 0 relative to the pen
        int32_t top = 0;                            // texel row 0 relative to the cell's top
        int32_t advance = 0;
        std::vector<uint8_t> alpha, grey;           // 4-bit values 0..15 (the game's A4R4G4B4 texel)
    };
    std::map<std::tuple<int32_t, uint32_t, bool, bool>, Glyph> glyphs;     // (em, code point, bold, shadow)
    ~Face() {
        if (face) FT_Done_Face(face);
    }
    float units(int32_t em) const { return float(em) / float(face->units_per_EM); }
    // GDI's TEXTMETRIC at an em (integers): tmAscent, tmDescent, tmHeight = their sum, tmInternalLeading = tmHeight - em.
    int32_t ascent(int32_t em) const { return int32_t(std::lround(winAscent * units(em))); }
    int32_t descent(int32_t em) const { return int32_t(std::lround(winDescent * units(em))); }
    int32_t cellHeight(int32_t em) const { return ascent(em) + descent(em); }
    int32_t internalLeading(int32_t em) const { return cellHeight(em) - em; }
    void setSize(int32_t em) {
        if (em == currentEm) return;
        FT_Set_Pixel_Sizes(face, 0, FT_UInt(em));
        currentEm = em;
    }
    const Glyph& glyph(int32_t em, uint32_t cp, bool bold, bool shadow) {
        const auto key = std::make_tuple(em, cp, bold, shadow);
        auto it = glyphs.find(key);
        if (it != glyphs.end()) return it->second;
        static uint8_t table[16][16];
        static bool tableBuilt = false;
        if (!tableBuilt) {
            for (int m = 0; m < 16; ++m)
                for (int a = 0; a < 16; ++a) table[m][a] = uint8_t(a < 1 ? m : std::clamp(int(std::lround(double(m) * 15.0 / double(a))), 0, 15));
            tableBuilt = true;
        }
        Glyph g;
        setSize(em);
        std::vector<uint8_t> cover;                 // the main glyph's coverage
        int32_t bw = 0, bh = 0, left = 0, top = 0;
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) == 0) {
            const FT_GlyphSlot slot = face->glyph;
            const FT_Bitmap& bm = slot->bitmap;
            bw = int32_t(bm.width);
            bh = int32_t(bm.rows);
            left = slot->bitmap_left;
            top = slot->bitmap_top;
            g.advance = int32_t((slot->advance.x + 32) >> 6);
            cover.assign(size_t(bw) * size_t(bh), 0);
            for (int32_t y = 0; y < bh; ++y)
                for (int32_t x = 0; x < bw; ++x) cover[size_t(y) * size_t(bw) + size_t(x)] = bm.buffer[y * bm.pitch + x];
        }
        if (bold) {                                 // synthetic bold: the coverage spread one pixel right, one pixel wider advance
            std::vector<uint8_t> wide(size_t(bw + 1) * size_t(bh), 0);
            for (int32_t y = 0; y < bh; ++y)
                for (int32_t x = 0; x <= bw; ++x) {
                    const uint8_t here = x < bw ? cover[size_t(y) * size_t(bw) + size_t(x)] : 0;
                    const uint8_t prev = x > 0 ? cover[size_t(y) * size_t(bw) + size_t(x - 1)] : 0;
                    wide[size_t(y) * size_t(bw + 1) + size_t(x)] = std::max(here, prev);
                }
            cover.swap(wide);
            bw += 1;
            g.advance += 1;
        }
        // The cell: the glyph bitmap plus the shadow one pixel right and down.
        const int32_t pad = shadow ? 1 : 0;
        g.width = bw + pad;
        g.height = bh + pad;
        g.originX = left;
        g.top = ascent(em) - top;
        g.alpha.assign(size_t(g.width) * size_t(g.height), 0);
        g.grey.assign(g.alpha.size(), 0);
        auto at = [&](int32_t x, int32_t y) -> float {
            return x >= 0 && y >= 0 && x < bw && y < bh ? float(cover[size_t(y) * size_t(bw) + size_t(x)]) / 255.0f : 0.0f;
        };
        for (int32_t y = 0; y < g.height; ++y)
            for (int32_t x = 0; x < g.width; ++x) {
                const float main = at(x, y), shade = shadow ? at(x - 1, y - 1) : 0.0f;
                const uint32_t blue = uint32_t(std::lround((main + (1.0f - main) * shade) * 255.0f)), red = uint32_t(std::lround(main * 255.0f));
                const uint32_t alpha = blue >> 4, mainNibble = red >> 4;
                g.alpha[size_t(y) * size_t(g.width) + size_t(x)] = uint8_t(alpha);
                g.grey[size_t(y) * size_t(g.width) + size_t(x)] = table[mainNibble][alpha];
            }
        return glyphs.emplace(key, std::move(g)).first->second;
    }
    int32_t kern(int32_t em, uint32_t first, uint32_t second) {
        if (!FT_HAS_KERNING(face)) return 0;
        setSize(em);
        FT_Vector delta{};
        FT_Get_Kerning(face, FT_Get_Char_Index(face, first), FT_Get_Char_Index(face, second), FT_KERNING_DEFAULT, &delta);
        return int32_t(delta.x >> 6);
    }
};

UiCanvas::UiCanvas() = default;
UiCanvas::~UiCanvas() {
    faces_.clear();                                  // before the library
    fontLibrary_.reset();
}

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::filesystem::path> systemFontDirectories() {
    std::vector<std::filesystem::path> dirs;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    dirs.emplace_back(std::filesystem::path(windir ? windir : "C:\\Windows") / "Fonts");
#elif defined(__APPLE__)
    dirs.emplace_back("/System/Library/Fonts");
    dirs.emplace_back("/Library/Fonts");
    if (const char* home = std::getenv("HOME")) dirs.emplace_back(std::filesystem::path(home) / "Library" / "Fonts");
#else
    dirs.emplace_back("/usr/share/fonts");
    dirs.emplace_back("/usr/local/share/fonts");
    if (const char* home = std::getenv("HOME")) {
        dirs.emplace_back(std::filesystem::path(home) / ".fonts");
        dirs.emplace_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
    }
#endif
    return dirs;
}

}  // namespace

UiCanvas::Face* UiCanvas::face(const std::string& family) {
    auto it = faces_.find(family);
    if (it != faces_.end()) return it->second.get();
    if (!fontLibrary_) fontLibrary_ = std::make_unique<FontLibrary>();
    if (!fontLibrary_->library) return (faces_[family] = nullptr).get();
    if (!fontFilesListed_) {
        fontFilesListed_ = true;
        // The game's own fonts first (Definitions/Fonts LoadedFontFile: TrueType files in data\misc), then the system's (read on demand).
        if (ui_)
            for (const std::string& file : ui_->loadedFontFiles) {
                tr::Bytes bytes;
                auto data = std::make_shared<std::vector<uint8_t>>();
                if (assets_ && assets_->tryRead(file, bytes)) data->assign(bytes.begin(), bytes.end());
                else {
                    std::ifstream in(std::filesystem::path(gameRoot_) / "data" / "misc" / file, std::ios::binary);
                    if (in) data->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }
                if (!data->empty()) fontFiles_.emplace_back(file, data);
            }
        for (const auto& dir : systemFontDirectories()) {
            std::error_code ec;
            for (auto entry = std::filesystem::recursive_directory_iterator(dir, ec); !ec && entry != std::filesystem::recursive_directory_iterator();
                 entry.increment(ec)) {
                const std::string ext = lowerAscii(entry->path().extension().string());
                if (ext == ".ttf" || ext == ".ttc" || ext == ".otf") fontFiles_.emplace_back(entry->path().string(), nullptr);
            }
        }
    }
    const std::string wanted = lowerAscii(family);
    std::unique_ptr<Face> found;
    for (auto& file : fontFiles_) {
        std::shared_ptr<std::vector<uint8_t>> data = file.second;
        if (!data) {
            data = std::make_shared<std::vector<uint8_t>>();
            std::ifstream in(file.first, std::ios::binary);
            if (in) data->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (data->empty()) continue;
        // Every face of the file (collections hold several); a regular face of the family.
        FT_Long faces = 1;
        for (FT_Long index = 0; index < faces && !found; ++index) {
            FT_Face ftFace = nullptr;
            if (FT_New_Memory_Face(fontLibrary_->library, data->data(), FT_Long(data->size()), index, &ftFace) != 0) break;
            faces = ftFace->num_faces;
            const bool match = ftFace->family_name && lowerAscii(ftFace->family_name) == wanted &&
                               (ftFace->style_flags & (FT_STYLE_FLAG_BOLD | FT_STYLE_FLAG_ITALIC)) == 0;
            if (!match) {
                FT_Done_Face(ftFace);
                continue;
            }
            found = std::make_unique<Face>();
            found->face = ftFace;
            found->data = data;
            const TT_OS2* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(ftFace, FT_SFNT_OS2));
            found->winAscent = os2 ? float(os2->usWinAscent) : float(ftFace->ascender);
            found->winDescent = os2 ? float(os2->usWinDescent) : float(-ftFace->descender);
        }
        if (found) {
            file.second = data;                    // keep files that supplied a face
            break;
        }
    }
    if (!found) missingFonts_.insert(family);
    return (faces_[family] = std::move(found)).get();
}

// ------------------------------------------------------------------------------------------
// Sources and layout
// ------------------------------------------------------------------------------------------
void UiCanvas::setSources(const tr::UiExport* ui, const tr::AssetLibrary* assets, tr::GameData* game, const std::string& gameRoot) {
    ui_ = ui;
    if (ui_) {
        width_ = ui_->logicalWidth;
        height_ = ui_->logicalHeight;
    }
    assets_ = assets;
    game_ = game;
    gameRoot_ = gameRoot;
    images_.clear();
    faces_.clear();
    fontFiles_.clear();
    fontFilesListed_ = false;
    drawn_.clear();
    missingTextures_.clear();
    missingFonts_.clear();
    missingTextIds_.clear();
}

const tr::XmlNode* UiCanvas::node(const tr::UiExport& ui, const std::vector<int32_t>& path) {
    const tr::XmlNode* n = &ui.widgets;
    for (int32_t i : path) {
        if (i < 0 || size_t(i) >= n->children.size()) return nullptr;
        n = &n->children[size_t(i)];
    }
    return n;
}

UiCanvas::Rect UiCanvas::childRect(const tr::XmlNode& widget, const Rect& parent) const {
    int32_t w = intAttribute(widget, "sizeW"), h = intAttribute(widget, "sizeH");
    if (attributeIsTrue(widget, "resizeToParent")) {
        w = parent.width();
        h = parent.height();
    }
    // Full-screen windows (declared at the export's logical size) take a wider screen's width (see fitToScreen).
    if (ui_ && width_ != ui_->logicalWidth && w == ui_->logicalWidth && h == ui_->logicalHeight && parent.width() == width_) w = width_;
    int32_t px = 0, py = 0, cx = 0, cy = 0;
    anchorPoint(intAttribute(widget, "parentAnchor"), parent.width(), parent.height(), px, py);
    anchorPoint(intAttribute(widget, "childAnchor"), w, h, cx, cy);
    const int32_t x = parent.x0 + px + intAttribute(widget, "anchorOffsetX") - cx;
    const int32_t y = parent.y0 + py + intAttribute(widget, "anchorOffsetY") - cy;
    return Rect{x, y, x + w, y + h};
}

UiCanvas::Rect UiCanvas::layout(const std::vector<int32_t>& path) const {
    if (!ui_) return Rect{};
    Rect rect{0, 0, width_, height_};
    const tr::XmlNode* n = &ui_->widgets;
    for (int32_t i : path) {
        if (i < 0 || size_t(i) >= n->children.size()) break;
        n = &n->children[size_t(i)];
        rect = childRect(*n, rect);
    }
    return rect;
}

// ------------------------------------------------------------------------------------------
// Drawing
// ------------------------------------------------------------------------------------------
struct UiCanvas::Target {
    int32_t width = 0, height = 0;             // pixels
    float sx = 1, sy = 1;                      // pixels per logical unit
    std::vector<float> rgb;                    // 0..1 as the 8-bit frame buffer holds it
    enum class Blend : uint8_t { Alpha, Additive, One };
    // A logical rectangle in pixels (edges rounded).
    Rect pixels(const Rect& r) const {
        return Rect{int32_t(std::lround(float(r.x0) * sx)), int32_t(std::lround(float(r.y0) * sy)), int32_t(std::lround(float(r.x1) * sx)),
                    int32_t(std::lround(float(r.y1) * sy))};
    }
    void blend(int32_t x, int32_t y, float r, float g, float b, float a, Blend mode) {
        float* p = &rgb[(size_t(y) * size_t(width) + size_t(x)) * 3];
        switch (mode) {
            case Blend::Alpha:                 // SRCALPHA / INVSRCALPHA
                p[0] = r * a + p[0] * (1 - a); p[1] = g * a + p[1] * (1 - a); p[2] = b * a + p[2] * (1 - a);
                break;
            case Blend::Additive:              // SRCALPHA / ONE
                p[0] = std::min(1.0f, p[0] + r * a); p[1] = std::min(1.0f, p[1] + g * a); p[2] = std::min(1.0f, p[2] + b * a);
                break;
            case Blend::One:                   // ONE / ONE
                p[0] = std::min(1.0f, p[0] + r); p[1] = std::min(1.0f, p[1] + g); p[2] = std::min(1.0f, p[2] + b);
                break;
        }
    }
};

const UiCanvas::Image* UiCanvas::image(const std::string& name) {
    const std::string key = lower(name);
    auto it = images_.find(key);
    if (it != images_.end()) return it->second.get();
    std::unique_ptr<Image> img;
    tr::Bytes bytes;
    if (assets_ && assets_->tryRead(name, bytes)) {
        try {
            tr::DdsHeader header;
            const std::vector<tr::Rgba8> pixels = tr::decodeDds(bytes, header);
            img = std::make_unique<Image>();
            img->width = int32_t(header.width);
            img->height = int32_t(header.height);
            img->rgba.resize(pixels.size() * 4);
            for (size_t k = 0; k < pixels.size(); ++k) {
                img->rgba[k * 4 + 0] = pixels[k].red;
                img->rgba[k * 4 + 1] = pixels[k].green;
                img->rgba[k * 4 + 2] = pixels[k].blue;
                img->rgba[k * 4 + 3] = pixels[k].alpha;
            }
        } catch (const std::exception&) {
            img.reset();
        }
    }
    if (!img) missingTextures_.insert(name);
    return (images_[key] = std::move(img)).get();
}

namespace {

// Bilinear sample at texel coordinates (u, v) (texel centres at .5), clamped to the image; out = rgba 0..1.
void sample(const std::vector<uint8_t>& rgba, int32_t w, int32_t h, float u, float v, float out[4]) {
    const float fx = u - 0.5f, fy = v - 0.5f;
    const int32_t x0 = int32_t(std::floor(fx)), y0 = int32_t(std::floor(fy));
    const float tx = fx - float(x0), ty = fy - float(y0);
    auto texel = [&](int32_t x, int32_t y, int c) {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return float(rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)]) / 255.0f;
    };
    for (int c = 0; c < 4; ++c) {
        const float a = texel(x0, y0, c) * (1 - tx) + texel(x0 + 1, y0, c) * tx;
        const float b = texel(x0, y0 + 1, c) * (1 - tx) + texel(x0 + 1, y0 + 1, c) * tx;
        out[c] = a * (1 - ty) + b * ty;
    }
}

void widgetColour(const tr::XmlNode& w, float c[4]) {
    c[0] = float(intAttribute(w, "colorR", 255)) / 255.0f;
    c[1] = float(intAttribute(w, "colorG", 255)) / 255.0f;
    c[2] = float(intAttribute(w, "colorB", 255)) / 255.0f;
    c[3] = float(intAttribute(w, "colorA", 255)) / 255.0f;
}

}  // namespace

void UiCanvas::drawTexture(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target) {
    const tr::UiTextureDef* def = ui_->texture(widget.attribute("textureDef"));
    if (!def) return;
    const std::string effect = lower(widget.attribute("effect"));
    Target::Blend blend = Target::Blend::Alpha;
    bool useColour = true;
    if (effect == "truiadditivealpha.fx" || effect == "paluimultitexadd.fx") blend = Target::Blend::Additive;
    if (effect == "truibackgroundmask.fx") {
        if (!options_.backgroundMaskWires) return;
        blend = Target::Blend::One;            // texture x mouse mask, alpha 1, ONE / ONE; no vertex colour
        useColour = false;
    }
    const Image* img = image(def->name);
    if (!img || img->width <= 0 || img->height <= 0) return;
    const Image* mask = nullptr;
    if (effect == "paluimultitex.fx" || effect == "paluimultitexadd.fx" || effect == "truiradarmask.fx") {
        const std::string maskName = widget.attribute("maskTextureName");
        if (!maskName.empty()) mask = image(maskName);
    }
    float colour[4];
    widgetColour(widget, colour);
    if (!useColour) colour[0] = colour[1] = colour[2] = colour[3] = 1.0f;
    const Rect px = target.pixels(rect);
    const Rect area = intersect(px, target.pixels(clip));
    const float regionW = float(def->right - def->left), regionH = float(def->bottom - def->top);
    const float w = float(std::max(1, px.width())), h = float(std::max(1, px.height()));
    for (int32_t y = std::max(area.y0, 0); y < std::min(area.y1, target.height); ++y) {
        const float dy = float(y - px.y0) + 0.5f;
        // Tiled: the region repeats at its own size in logical units.
        const float v = def->tileV && regionH > 0 ? float(def->top) + std::fmod(dy / target.sy, regionH) : float(def->top) + dy / h * regionH;
        for (int32_t x = std::max(area.x0, 0); x < std::min(area.x1, target.width); ++x) {
            const float dx = float(x - px.x0) + 0.5f;
            const float u = def->tileU && regionW > 0 ? float(def->left) + std::fmod(dx / target.sx, regionW) : float(def->left) + dx / w * regionW;
            float t[4];
            sample(img->rgba, img->width, img->height, u, v, t);
            if (mask) {
                float m[4];
                sample(mask->rgba, mask->width, mask->height, dx / w * float(mask->width), dy / h * float(mask->height), m);
                for (int c = 0; c < 4; ++c) t[c] *= m[c];
            }
            target.blend(x, y, t[0] * colour[0], t[1] * colour[1], t[2] * colour[2], blend == Target::Blend::One ? 1.0f : t[3] * colour[3], blend);
        }
    }
}

void UiCanvas::drawColour(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target) {
    float colour[4];
    widgetColour(widget, colour);
    const Target::Blend blend = lower(widget.attribute("effect")) == "truiadditivealpha.fx" ? Target::Blend::Additive : Target::Blend::Alpha;
    const Rect area = intersect(target.pixels(rect), target.pixels(clip));
    for (int32_t y = std::max(area.y0, 0); y < std::min(area.y1, target.height); ++y)
        for (int32_t x = std::max(area.x0, 0); x < std::min(area.x1, target.width); ++x) target.blend(x, y, colour[0], colour[1], colour[2], colour[3], blend);
}

std::string UiCanvas::text(const tr::XmlNode& widget) {
    const std::string id = widget.attribute("textId");
    if (id.empty() || !game_) return "";
    const std::string s = game_->uiText(id);
    if (s.empty()) missingTextIds_.insert(id);
    return s;
}

// Text, as the game draws it (see "Fonts" above): glyph em = round(trunc(pointSize x 4 / 3) x screen scale) (the integer point step:
// Euro_9 14, A_Conduit_14 20, A_Conduit_16 24 pixels at 1920 x 1080 - measured on session image 270 and the fountain trace's final
// frame); lines GDI tmHeight apart, the first baseline tmAscent - tmInternalLeading below the rect's top (the em box at the top),
// vertAlign centre on the block's em height (fitted to the fountain trace's Quit dialog, FrontEnd/LogOutWindow). Alignment is in pixels
// (within 1 - 2 pixels of the game there; its exact rule is not traced).
void UiCanvas::drawText(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target) {
    const std::string s = text(widget);
    if (s.empty()) return;
    const tr::UiFont* font = ui_->font(widget.attribute("fontId"));
    if (!font) font = ui_->font("Default");
    if (!font && !ui_->fonts.empty()) font = &ui_->fonts.begin()->second;    // UNVERIFIED stand-in for unknown font ids
    if (!font || font->ranges.empty()) return;
    auto emOf = [&](const tr::UiFontRange& r) { return int32_t(std::lround(float(r.pointSize * 4 / 3) * target.sy)); };
    const tr::UiFontRange& base = font->ranges.front();
    Face* baseFace = face(base.face);
    if (!baseFace) return;
    const int32_t baseEm = emOf(base);
    struct Item {
        uint32_t cp;
        Face* face;
        int32_t em;
        bool bold, shadow;
    };
    std::vector<Item> items;
    for (uint32_t cp : codepoints(s)) {
        const tr::UiFontRange* range = font->rangeFor(cp);
        Face* f = range ? face(range->face) : nullptr;
        if (!f) { f = baseFace; range = &base; }
        items.push_back(Item{cp, f, emOf(*range), range->bold, range->shadow});
    }
    const Rect px = target.pixels(rect);
    auto advance = [&](size_t i) {
        int32_t a = items[i].face->glyph(items[i].em, items[i].cp, items[i].bold, items[i].shadow).advance;
        if (i + 1 < items.size() && items[i + 1].face == items[i].face) a += items[i].face->kern(items[i].em, items[i].cp, items[i + 1].cp);
        return a;
    };
    std::vector<std::pair<size_t, size_t>> lines;
    const bool wrap = attributeIsTrue(widget, "wrapText");
    size_t lineStart = 0, lastSpace = size_t(-1);
    int32_t width = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].cp == '\n') {
            lines.emplace_back(lineStart, i);
            lineStart = i + 1;
            width = 0;
            lastSpace = size_t(-1);
            continue;
        }
        if (items[i].cp == ' ') lastSpace = i;
        width += advance(i);
        if (wrap && width > px.width() && lastSpace != size_t(-1) && lastSpace > lineStart) {
            lines.emplace_back(lineStart, lastSpace);
            lineStart = lastSpace + 1;
            width = 0;
            for (size_t k = lineStart; k <= i; ++k) width += advance(k);
            lastSpace = size_t(-1);
        }
    }
    lines.emplace_back(lineStart, items.size());

    float colour[4];
    widgetColour(widget, colour);
    const Target::Blend blend = lower(widget.attribute("effect")) == "truiadditivealpha.fx" ? Target::Blend::Additive : Target::Blend::Alpha;
    const std::string horiz = widget.attribute("horizAlign"), vert = widget.attribute("vertAlign");
    const int32_t lineHeight = baseFace->cellHeight(baseEm), leading = baseFace->internalLeading(baseEm);
    const int32_t blockHeight = int32_t(lines.size()) * lineHeight - leading;
    int32_t y = px.y0;
    if (vert == "center") y += (px.height() - blockHeight) / 2;
    else if (vert == "bottom") y += px.height() - blockHeight;
    const Rect area = intersect(px, target.pixels(clip));
    for (const auto& line : lines) {
        int32_t lineWidth = 0;
        for (size_t i = line.first; i < line.second; ++i) lineWidth += advance(i);
        int32_t x = px.x0;
        if (horiz == "center") x += (px.width() - lineWidth) / 2;
        else if (horiz == "right") x += px.width() - lineWidth;
        const int32_t cellTop = y - leading;              // the glyph cell's top: the em box starts at the line's top
        for (size_t i = line.first; i < line.second; ++i) {
            const Face::Glyph& g = items[i].face->glyph(items[i].em, items[i].cp, items[i].bold, items[i].shadow);
            // Another range's face keeps the base line's baseline.
            const int32_t gx = x + g.originX, gy = cellTop + baseFace->ascent(baseEm) - items[i].face->ascent(items[i].em) + g.top;
            for (int32_t yy = 0; yy < g.height; ++yy) {
                const int32_t py = gy + yy;
                if (py < std::max(area.y0, 0) || py >= std::min(area.y1, target.height)) continue;
                for (int32_t xx = 0; xx < g.width; ++xx) {
                    const int32_t ppx = gx + xx;
                    if (ppx < std::max(area.x0, 0) || ppx >= std::min(area.x1, target.width)) continue;
                    const size_t k = size_t(yy) * size_t(g.width) + size_t(xx);
                    if (g.alpha[k] == 0) continue;
                    const float grey = float(g.grey[k]) / 15.0f, a = float(g.alpha[k]) / 15.0f;
                    target.blend(ppx, py, colour[0] * grey, colour[1] * grey, colour[2] * grey, colour[3] * a, blend);
                }
            }
            x += advance(i);
        }
        y += lineHeight;
    }
}

void UiCanvas::drawWidget(const tr::XmlNode& widget, std::vector<int32_t>& path, const Rect& parent, const Rect& clip, bool branchRoot,
                          Target& target) {
    if (!branchRoot && !options_.showHidden && widget.attribute("visible") == "false") return;
    const Rect rect = childRect(widget, parent);
    bool visual = false;
    if (widget.hasAttribute("textureDef")) {
        drawTexture(widget, rect, clip, target);
        visual = true;
    } else if (widget.tag == "ColoredRect") {
        drawColour(widget, rect, clip, target);
        visual = true;
    } else if (widget.tag == "Text" || widget.tag == "RichText") {
        drawText(widget, rect, clip, target);
        visual = !text(widget).empty();
    }
    drawn_.push_back(DrawnWidget{path, rect, visual});
    const Rect childClip = attributeIsTrue(widget, "clipChildren") ? intersect(clip, rect) : clip;
    for (size_t k = 0; k < widget.children.size(); ++k) {
        path.push_back(int32_t(k));
        drawWidget(widget.children[k], path, rect, childClip, false, target);
        path.pop_back();
    }
}

std::vector<UiCanvas::DrawnWidget> UiCanvas::render(const std::vector<int32_t>& path, const Options& options, std::vector<uint8_t>& rgba) {
    drawn_.clear();
    options_ = options;
    const int32_t logicalW = ui_ ? ui_->logicalWidth : 1280, logicalH = ui_ ? ui_->logicalHeight : 960;
    // The root window's logical size (tabula_rasa.exe FUN_00896220, sizes from FUN_004cd8c0's call of the UI manager's init
    // FUN_00895430): 1706 x 960 when the screen's width / height exceeds 1.5 (double at 0x00c2b570), else 1280 x 960; the screen
    // scale is screen / logical per axis (FUN_00897190).
    const int32_t screenW = options.screenWidth > 0 ? options.screenWidth : logicalW;
    const int32_t screenH = options.screenHeight > 0 ? options.screenHeight : logicalH;
    width_ = options.screenWidth > 0 && double(screenW) / double(screenH) > 1.5 ? 1706 : logicalW;
    height_ = logicalH;
    pixelWidth_ = screenW;
    pixelHeight_ = screenH;
    Target target;
    target.width = screenW;
    target.height = screenH;
    target.sx = float(screenW) / float(width_);
    target.sy = float(screenH) / float(height_);
    target.rgb.assign(size_t(screenW) * size_t(screenH) * 3, 0.0f);
    const bool imageBackground = options.backgroundRgba && options.backgroundRgba->size() == size_t(screenW) * size_t(screenH) * 4;
    for (int32_t y = 0; y < screenH; ++y)
        for (int32_t x = 0; x < screenW; ++x) {
            float* p = &target.rgb[(size_t(y) * size_t(screenW) + size_t(x)) * 3];
            if (imageBackground) {
                const uint8_t* b = &(*options.backgroundRgba)[(size_t(y) * size_t(screenW) + size_t(x)) * 4];
                p[0] = float(b[0]) / 255.0f; p[1] = float(b[1]) / 255.0f; p[2] = float(b[2]) / 255.0f;
                continue;
            }
            float g = 0;
            if (options.background == 1) g = 0.25f;
            else if (options.background == 2) g = ((x / 16 + y / 16) & 1) ? 0.30f : 0.20f;
            p[0] = p[1] = p[2] = g;
        }
    const Rect screen{0, 0, width_, height_};
    const tr::XmlNode* n = ui_ ? node(*ui_, path) : nullptr;
    if (n && !path.empty()) {
        std::vector<int32_t> parentPath(path.begin(), path.end() - 1);
        std::vector<int32_t> walk = path;
        drawWidget(*n, walk, layout(parentPath), screen, true, target);
    } else if (n) {                            // the <Widgets> element itself: every root widget
        for (size_t k = 0; k < n->children.size(); ++k) {
            std::vector<int32_t> walk{int32_t(k)};
            drawWidget(n->children[k], walk, screen, screen, false, target);
        }
    }
    const size_t count = size_t(screenW) * size_t(screenH);
    rgba.resize(count * 4);
    for (size_t k = 0; k < count; ++k) {
        for (int c = 0; c < 3; ++c) rgba[k * 4 + size_t(c)] = uint8_t(std::lround(std::clamp(target.rgb[k * 3 + size_t(c)], 0.0f, 1.0f) * 255.0f));
        rgba[k * 4 + 3] = 255;
    }
    return drawn_;
}

std::vector<int32_t> UiCanvas::pick(int32_t x, int32_t y) const {
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it)
        if (it->visual && it->rect.contains(x, y)) return it->path;
    const DrawnWidget* best = nullptr;
    for (const DrawnWidget& d : drawn_)
        if (d.rect.contains(x, y) && (!best || d.path.size() >= best->path.size())) best = &d;
    return best ? best->path : std::vector<int32_t>{};
}
