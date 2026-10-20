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

// Colour modulation (FUN_008b7280, per byte): 0 if either is 0, the other if one is 255, else round(a x b / 255).
uint8_t multiplyColour(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    if (a == 255) return b;
    if (b == 255) return a;
    return uint8_t(std::lround(double(a) * double(b) / 255.0));
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
        width_ = 1280;
        height_ = 960;
    }
    assets_ = assets;
    game_ = game;
    gameRoot_ = gameRoot;
    images_.clear();
    faces_.clear();
    fontFiles_.clear();
    loadAnimations();
    fontFilesListed_ = false;
    drawn_.clear();
    editLayouts_.clear();
    missingTextures_.clear();
    missingFonts_.clear();
    missingTextIds_.clear();
}

void UiCanvas::setExport(const tr::UiExport* ui) {
    ui_ = ui;
    loadAnimations();
}

void UiCanvas::loadAnimations() {
    animations_.clear();
    if (ui_) {
        for (const tr::XmlNode& a : ui_->animations.children) {
            if (a.tag != "Animation") continue;
            const tr::XmlNode* id = a.child("id");
            if (!id || id->text.empty()) continue;
            Animation anim;
            const tr::XmlNode* type = a.child("animType");
            anim.bitmap = type && type->text == "Bitmap";
            const tr::XmlNode* playback = a.child("playbackType");
            if (playback) anim.playback = playback->text == "pingpong" ? 2 : playback->text == "repeat" ? 1 : 0;
            auto readTrack = [&](const char* tag, AnimTrack& track, std::initializer_list<const char*> names) {
                const tr::XmlNode* t = a.child(tag);
                if (!t) return;
                track.discrete = t->attribute("discrete") == "true";
                for (const tr::XmlNode& f : t->children) {
                    AnimKey key;
                    key.time = uint32_t(std::strtoul(f.attribute("time").c_str(), nullptr, 10));
                    int k = 0;
                    for (const char* n : names) key.v[k++] = std::strtof(f.attribute(n).c_str(), nullptr);
                    track.keys.push_back(key);
                }
                std::stable_sort(track.keys.begin(), track.keys.end(), [](const AnimKey& x, const AnimKey& y) { return x.time < y.time; });
                if (!track.keys.empty()) anim.length = std::max(anim.length, track.keys.back().time);
            };
            if (anim.bitmap) {
                readTrack("uvTrack", anim.uv, {"X", "Y", "Z", "W"});
                readTrack("rotTrack", anim.rot, {"rot"});
                if (const tr::XmlNode* r = a.child("rotTrack")) {
                    if (r->hasAttribute("anchorX")) anim.anchorX = std::strtof(r->attribute("anchorX").c_str(), nullptr);
                    if (r->hasAttribute("anchorY")) anim.anchorY = std::strtof(r->attribute("anchorY").c_str(), nullptr);
                }
            } else {
                readTrack("scaleTrack", anim.scale, {"X", "Y"});
                readTrack("transTrack", anim.trans, {"X", "Y"});
                readTrack("colorTrack", anim.colour, {"R", "G", "B", "A"});
            }
            animations_[id->text] = std::move(anim);
        }
    }
}

const tr::XmlNode* UiCanvas::node(const std::vector<int32_t>& path) const {
    const tr::XmlNode* n = detachedRoot_ ? detachedRoot_ : root();
    if (!n) return nullptr;
    for (int32_t i : path) {
        if (i < 0 || size_t(i) >= n->children.size()) return nullptr;
        n = &n->children[size_t(i)];
    }
    return n;
}

// Animations (tabula_rasa.exe client\tr\ui\anim, decompiled 2026-10-15). Loaders: AnimData FUN_008bf0a0 (id; playbackType pingpong
// 2, repeat 1, else - "stall" - 0), WidgetAnimData FUN_008ba880 (scaleTrack / transTrack: (time, X, Y); colorTrack: (time, R, G, B, A)),
// BitmapAnimData FUN_008c0250 (uvTrack (time, X, Y, Z, W), rotTrack (time, rot) with anchorX / anchorY, default 0.5; textureTrack -
// empty in the export). Widgets name theirs with widgetAnimName (Widget loader FUN_0089c630 -> slot 88), bitmaps with bitmapAnimName
// (FUN_008f4070 -> slot 135); setting one starts its player at once (slot 91: AnimManager FUN_0088d100 makes a WidgetAnimPlayer /
// BitmapAnimPlayer, FUN_0088d3a0 starts it at time 0). Per frame (FUN_008bf780) the player's time t (ms, x speed 1.0) is wrapped
// (FUN_008bf440): length = the longest track's last key (FUN_008bba90); length 0 = nothing applied; repeat t mod length; pingpong t mod
// 2 length, mirrored past length; stall unwrapped. Each track is sampled (FUN_008bc1c0 / FUN_008bbdc0 / FUN_008c2190; keys sorted by
// time on first use): no keys = not applied; one key, or t at or before the first: the first; at or after the last: the last; else
// between the keys around t with f = (t - t0) / (t1 - t0): linear (vectors FUN_008bd310 / FUN_008c3fd0; colours per channel, rounded
// and clamped, FUN_004445c0 / FUN_00444390), or for a discrete track the earlier key while f < 1. UNVERIFIED: no in-game animation was
// captured; the clock here is one shared time (the game starts each widget's at load, and autoRestartAnims - Widget + 0x98 - restarts
// it, presumably when shown; that call was not traced).
bool UiCanvas::sampleAnimation(const std::string& name, bool bitmap, AnimFrame& frame) const {
    if (options_.animTime < 0 || name.empty()) return false;
    const auto it = animations_.find(name);
    if (it == animations_.end() || it->second.bitmap != bitmap || it->second.length == 0) return false;
    const Animation& a = it->second;
    uint32_t t = uint32_t(options_.animTime);
    if (a.playback != 0 || t < a.length) animating_ = true;
    if (a.playback == 1) t %= a.length;
    else if (a.playback == 2) {
        t %= a.length * 2;
        if (t > a.length) t = a.length * 2 - t;
    }
    auto sampleTrack = [&](const AnimTrack& track, int components, bool colour, float out[4]) {
        if (track.keys.empty()) return false;
        const std::vector<AnimKey>& k = track.keys;
        if (k.size() == 1 || t <= k.front().time) {
            for (int c = 0; c < components; ++c) out[c] = k.front().v[c];
            return true;
        }
        if (t >= k.back().time) {
            for (int c = 0; c < components; ++c) out[c] = k.back().v[c];
            return true;
        }
        size_t i = 1;
        while (i + 1 < k.size() && k[i].time < t) ++i;      // k[i - 1].time < t <= k[i].time
        const AnimKey& p = k[i - 1];
        const AnimKey& n = k[i];
        const float f = n.time != p.time ? float(t - p.time) / float(n.time - p.time) : 0.0f;
        for (int c = 0; c < components; ++c) {
            if (colour) {
                const float v = f <= 0 ? p.v[c] : f >= 1 ? n.v[c] : float(std::lround(p.v[c] + f * (n.v[c] - p.v[c])));
                out[c] = std::clamp(v, 0.0f, 255.0f);
            } else if (track.discrete) {
                out[c] = f < 1 ? p.v[c] : n.v[c];
            } else {
                out[c] = f <= 0 ? p.v[c] : f >= 1 ? n.v[c] : p.v[c] + f * (n.v[c] - p.v[c]);
            }
        }
        return true;
    };
    float v[4];
    if (bitmap) {
        if ((frame.uv = sampleTrack(a.uv, 4, false, v))) std::copy(v, v + 4, frame.uvRect);
        if ((frame.rot = sampleTrack(a.rot, 1, false, v))) {
            frame.rotation = v[0];
            frame.anchorX = a.anchorX;
            frame.anchorY = a.anchorY;
        }
        return frame.uv || frame.rot;
    }
    if ((frame.scale = sampleTrack(a.scale, 2, false, v))) {
        frame.scaleX = v[0];
        frame.scaleY = v[1];
    }
    if ((frame.trans = sampleTrack(a.trans, 2, false, v))) {
        frame.transX = v[0];
        frame.transY = v[1];
    }
    if ((frame.colour = sampleTrack(a.colour, 4, true, v)))
        for (int c = 0; c < 4; ++c) frame.rgba[c] = uint8_t(v[c]);
    return frame.scale || frame.trans || frame.colour;
}

// Layout (Widget slot 105, FUN_0089d740, read in the assembly 2026-10-15): size w x h (sizeW / sizeH, or the parent's size with
// resizeToParent); a widget animation's scale makes it trunc(w x sx) x trunc(h x sy), centred: offset ((w - w') / 2, (h - h') / 2)
// (integer halves towards 0); its translation adds (trunc(tx), trunc(ty)). With a parent: left = parent left + trunc((anchor position +
// offset) x S), width trunc(w' x S), S = the parent's scale (slot 107: its own animation scale x its parent's, so children shrink with
// it); the anchor position (FUN_008b6fb0) uses the unscaled sizes. Without animations this is the plain anchor layout.
UiCanvas::Placed UiCanvas::place(const tr::XmlNode& widget, const Placed& parent) const {
    int32_t w = intAttribute(widget, "sizeW"), h = intAttribute(widget, "sizeH");
    if (attributeIsTrue(widget, "resizeToParent")) {
        w = parent.w;
        h = parent.h;
    }
    if (const auto so = buttonOverrides_.find(&widget); so != buttonOverrides_.end() && so->second.size) {
        w = so->second.width;
        h = so->second.height;
    }
    if (&widget == options_.dragNode) {
        w += options_.dragW;
        h += options_.dragH;
    }
    // Full-screen windows (declared at the export's logical size) take a wider screen's width (see fitToScreen).
    if (width_ != 1280 && w == 1280 && h == 960 && parent.w == width_) w = width_;
    int32_t px = 0, py = 0, cx = 0, cy = 0;
    anchorPoint(intAttribute(widget, "parentAnchor"), parent.w, parent.h, px, py);
    anchorPoint(intAttribute(widget, "childAnchor"), w, h, cx, cy);
    int32_t offsetX = intAttribute(widget, "anchorOffsetX"), offsetY = intAttribute(widget, "anchorOffsetY");
    if (&widget == options_.dragNode) {
        offsetX += options_.dragX;
        offsetY += options_.dragY;
    }
    const auto o = buttonOverrides_.find(&widget);
    if (o != buttonOverrides_.end() && o->second.offset) {
        offsetX = o->second.offsetX;
        offsetY = o->second.offsetY;
    }
    int32_t dx = 0, dy = 0, drawnW = w, drawnH = h;
    Placed out;
    out.w = w;
    out.h = h;
    out.sx = parent.sx;
    out.sy = parent.sy;
    AnimFrame frame;
    if (sampleAnimation(widget.attribute("widgetAnimName"), false, frame)) {
        if (frame.scale) {
            drawnW = int32_t(float(w) * frame.scaleX);
            drawnH = int32_t(float(h) * frame.scaleY);
            dx = (w - drawnW) / 2;
            dy = (h - drawnH) / 2;
            out.sx *= frame.scaleX;
            out.sy *= frame.scaleY;
        }
        if (frame.trans) {
            dx += int32_t(frame.transX);
            dy += int32_t(frame.transY);
        }
    }
    const int32_t x = parent.rect.x0 + int32_t(float(px + offsetX - cx + dx) * parent.sx);
    const int32_t y = parent.rect.y0 + int32_t(float(py + offsetY - cy + dy) * parent.sy);
    out.rect = Rect{x, y, x + int32_t(float(drawnW) * parent.sx), y + int32_t(float(drawnH) * parent.sy)};
    // The render context colour the widget's children draw with: its colour x its parent's context colour (FUN_008aee70).
    uint8_t own[4];
    ownColour(widget, own);
    for (int c = 0; c < 4; ++c) out.colour[c] = multiplyColour(own[c], parent.colour[c]);
    return out;
}

UiCanvas::Placed UiCanvas::placedAt(const std::vector<int32_t>& path) const {
    Placed placed;
    placed.rect = Rect{0, 0, width_, height_};
    placed.w = width_;
    placed.h = height_;
    if (!ui_) return placed;
    const tr::XmlNode* n = root();
    for (int32_t i : path) {
        if (i < 0 || size_t(i) >= n->children.size()) break;
        n = &n->children[size_t(i)];
        placed = place(*n, placed);
    }
    return placed;
}

UiCanvas::Rect UiCanvas::layout(const std::vector<int32_t>& path) const { return placedAt(path).rect; }

void UiCanvas::offsetsFor(const std::vector<int32_t>& path, const Rect& rect, int32_t& offsetX, int32_t& offsetY) const {
    offsetX = offsetY = 0;
    const tr::XmlNode* n = node(path);
    if (!n || path.empty()) return;
    const Placed parent = placedAt(std::vector<int32_t>(path.begin(), path.end() - 1));
    int32_t px = 0, py = 0, cx = 0, cy = 0;
    anchorPoint(intAttribute(*n, "parentAnchor"), parent.w, parent.h, px, py);
    anchorPoint(intAttribute(*n, "childAnchor"), rect.width(), rect.height(), cx, cy);
    offsetX = rect.x0 - parent.rect.x0 - px + cx;
    offsetY = rect.y0 - parent.rect.y0 - py + cy;
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
    const auto o = buttonOverrides_.find(&widget);
    const tr::UiTextureDef* def = ui_->texture(o != buttonOverrides_.end() && o->second.texture ? o->second.textureDef : widget.attribute("textureDef"));
    if (!def) return;
    const std::string effect = lower(widget.attribute("effect"));
    Target::Blend blend = Target::Blend::Alpha;
    bool useColour = true;
    if (effect == "truiadditivealpha.fx" || effect == "paluimultitexadd.fx") blend = Target::Blend::Additive;
    // TRUIBackgroundMask (traced 2026-10-15 in the fountain trace, call 37335934): pixel shader rgb = texture x UIMouseMaskSampler, alpha
    // 1, blended ONE / ONE, no vertex colour; the vertex shader's mask coordinate = MouseUVXForm x the vertex's screen position (NDC) -
    // (3.332, -1.875) at 1920 x 1080 = 1/288 per pixel on both axes, i.e. the 256 x 256 ui_mouse_mask.dds (the UI manager loads it,
    // FUN_00895430; a radial white spot, 1/255 at the edges) spread over 256 logical units centred on the mouse, address CLAMP.
    const Image* mouseMask = nullptr;
    if (effect == "truibackgroundmask.fx") {
        if (!options_.mouseHighlight) return;  // the clamped edge: 1/255, nothing visible
        mouseMask = image("ui_mouse_mask.dds");
        if (!mouseMask) return;
        blend = Target::Blend::One;
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
    colourOf(widget, colour);
    if (!useColour) colour[0] = colour[1] = colour[2] = colour[3] = 1.0f;
    const Rect px = target.pixels(rect);
    const Rect area = intersect(px, target.pixels(clip));
    // A bitmap animation (Bitmap render FUN_008f5300 -> FUN_008f5550, read in the assembly 2026-10-15): a uv key replaces the texture region
    // by (trunc(width x X), trunc(height x Y), trunc(width x Z), trunc(height x W)) of the whole texture (an empty one widened by a pixel,
    // both axes against the width - as the code does); a rotation key gives the quad's texture coordinates the matrix translate(-anchor)
    // x RotationZ(rot) x translate(anchor) (D3DX, row vectors; rot in radians). The draw (FUN_005adc60) takes a region with right <= left
    // or bottom <= top as the whole texture. UNVERIFIED: that the rotation applies to the quad's 0..1 coordinates before they are mapped
    // into the region (the draw's matrix chain decompiles poorly), and the sampler's wrap addressing for animated regions outside the
    // texture (16Frame_512x256's first row is at Y 1..1.25).
    int32_t left = def->left, top = def->top, right = def->right, bottom = def->bottom;
    AnimFrame anim;
    const bool animated = sampleAnimation(widget.attribute("bitmapAnimName"), true, anim);
    if (animated && anim.uv) {
        left = int32_t(float(img->width) * anim.uvRect[0]);
        top = int32_t(float(img->height) * anim.uvRect[1]);
        right = int32_t(float(img->width) * anim.uvRect[2]);
        bottom = int32_t(float(img->height) * anim.uvRect[3]);
        if (left == right) {
            if (right < img->width) ++right;
            else --left;
        }
        if (top == bottom) {
            if (bottom < img->width) ++bottom;
            else --top;
        }
    }
    if (right <= left || bottom <= top) {
        left = 0;
        top = 0;
        right = img->width;
        bottom = img->height;
    }
    const bool rotate = animated && anim.rot;
    const float rc = std::cos(anim.rotation), rs = std::sin(anim.rotation);
    const float regionW = float(right - left), regionH = float(bottom - top);
    const float w = float(std::max(1, px.width())), h = float(std::max(1, px.height()));
    for (int32_t y = std::max(area.y0, 0); y < std::min(area.y1, target.height); ++y) {
        const float dy = float(y - px.y0) + 0.5f;
        // Tiled: the region repeats at its own size in logical units.
        float v = def->tileV && regionH > 0 ? float(top) + std::fmod(dy / target.sy, regionH) : float(top) + dy / h * regionH;
        for (int32_t x = std::max(area.x0, 0); x < std::min(area.x1, target.width); ++x) {
            const float dx = float(x - px.x0) + 0.5f;
            float u = def->tileU && regionW > 0 ? float(left) + std::fmod(dx / target.sx, regionW) : float(left) + dx / w * regionW;
            if (rotate) {
                const float qx = dx / w - anim.anchorX, qy = dy / h - anim.anchorY;
                u = float(left) + (qx * rc - qy * rs + anim.anchorX) * regionW;
                v = float(top) + (qx * rs + qy * rc + anim.anchorY) * regionH;
            }
            float t[4];
            if (animated) {
                const float fw = float(img->width), fh = float(img->height);
                sample(img->rgba, img->width, img->height, u - std::floor(u / fw) * fw, v - std::floor(v / fh) * fh, t);
            } else {
                sample(img->rgba, img->width, img->height, u, v, t);
            }
            if (mask) {
                float m[4];
                sample(mask->rgba, mask->width, mask->height, dx / w * float(mask->width), dy / h * float(mask->height), m);
                for (int c = 0; c < 4; ++c) t[c] *= m[c];
            }
            if (mouseMask) {
                const float size = 256.0f * target.sy;     // pixels
                const float mu = (float(x) + 0.5f - options_.mouseX * target.sx) / size + 0.5f;
                const float mv = (float(y) + 0.5f - options_.mouseY * target.sy) / size + 0.5f;
                float m[4];
                sample(mouseMask->rgba, mouseMask->width, mouseMask->height, std::clamp(mu, 0.0f, 1.0f) * float(mouseMask->width),
                       std::clamp(mv, 0.0f, 1.0f) * float(mouseMask->height), m);
                for (int c = 0; c < 3; ++c) t[c] *= m[c];
            }
            target.blend(x, y, t[0] * colour[0], t[1] * colour[1], t[2] * colour[2], blend == Target::Blend::One ? 1.0f : t[3] * colour[3], blend);
        }
    }
}

void UiCanvas::drawColour(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target) {
    float colour[4];
    colourOf(widget, colour);
    const Target::Blend blend = lower(widget.attribute("effect")) == "truiadditivealpha.fx" ? Target::Blend::Additive : Target::Blend::Alpha;
    const Rect area = intersect(target.pixels(rect), target.pixels(clip));
    for (int32_t y = std::max(area.y0, 0); y < std::min(area.y1, target.height); ++y)
        for (int32_t x = std::max(area.x0, 0); x < std::min(area.x1, target.width); ++x) target.blend(x, y, colour[0], colour[1], colour[2], colour[3], blend);
}

std::string UiCanvas::text(const tr::XmlNode& widget) {
    if (&widget == sampleNode_) return sampleText_;          // a Definitions preview's sample
    if (const auto o = textOverrides_.find(&widget); o != textOverrides_.end()) return o->second;   // an edit box's typed text
    const std::string id = widget.attribute("textId");
    if (id.empty() || !game_) return "";
    std::string s;
    if (!game_->uiTextExists(id, 1, &s)) missingTextIds_.insert(id);   // an id whose text is "" (ID_DEFAULT_TEXT) is not missing
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
    struct Item {
        uint32_t cp;
        Face* face;
        int32_t em;
        bool bold, shadow;
    };
    std::vector<Item> items;
    const tr::UiFontRange* basePtr = nullptr;
    Face* baseFace = nullptr;
    int32_t baseEm = 0;
    const std::vector<uint32_t> cps = codepoints(s);
    auto setFont = [&](const tr::UiFont& f) {
        basePtr = &f.ranges.front();
        baseFace = face(basePtr->face);
        baseEm = emOf(*basePtr);
        items.clear();
        if (!baseFace) return;
        for (uint32_t cp : cps) {
            const tr::UiFontRange* range = f.rangeFor(cp);
            Face* rf = range ? face(range->face) : nullptr;
            if (!rf) { rf = baseFace; range = basePtr; }
            items.push_back(Item{cp, rf, emOf(*range), range->bold, range->shadow});
        }
    };
    setFont(*font);
    if (!baseFace) return;
    const Rect px = target.pixels(rect);
    auto advance = [&](size_t i) {
        int32_t a = items[i].face->glyph(items[i].em, items[i].cp, items[i].bold, items[i].shadow).advance;
        if (i + 1 < items.size() && items[i + 1].face == items[i].face) a += items[i].face->kern(items[i].em, items[i].cp, items[i + 1].cp);
        return a;
    };
    const bool wrap = attributeIsTrue(widget, "wrapText");
    // Text that does not fit an unwrapped widget (uitext.cpp FUN_008b1c00, decompiled 2026-10-15):
    // - scaleToFit: another of the UI's fonts - FUN_00896a20 -> FUN_008ea910 -> FUN_006ab090 takes the width ratio (widget / text),
    //   s = trunc(-em x ratio) (GDI lfHeight = -em) and walks s towards 0 until a created font of the same face has that size (first
    //   with the same style flags, then plain); the defined font owning it is used, else "DefaultSmallest". Fonts are created for
    //   every defined range, so: the largest defined font of the same face no bigger than trunc(em x ratio).
    // - otherwise FUN_008fa210 cuts it to fit and appends "..." (three periods).
    if (!wrap && !items.empty()) {
        int32_t total = 0;
        for (size_t i = 0; i < items.size(); ++i) total += advance(i);
        if (total > px.width() && px.width() > 0) {
            if (attributeIsTrue(widget, "scaleToFit")) {
                const int32_t targetEm = int32_t(float(baseEm) * float(px.width()) / float(total));
                const tr::UiFont* best = nullptr;
                int32_t bestEm = 0;
                for (int pass = 0; pass < 2 && !best; ++pass)
                    for (const auto& entry : ui_->fonts) {
                        const tr::UiFontRange& r = entry.second.ranges.empty() ? *basePtr : entry.second.ranges.front();
                        if (entry.second.ranges.empty() || r.face != basePtr->face) continue;
                        if (pass == 0 && (r.bold != basePtr->bold || r.italic != basePtr->italic || r.shadow != basePtr->shadow)) continue;
                        if (pass == 1 && (r.bold || r.italic || r.shadow)) continue;
                        const int32_t em = emOf(r);
                        if (em <= targetEm && em > bestEm) {
                            best = &entry.second;
                            bestEm = em;
                        }
                    }
                if (!best) best = ui_->font("DefaultSmallest");
                if (best && !best->ranges.empty()) {
                    setFont(*best);
                    if (!baseFace) return;
                }
            } else if (scrollTail_) {
                // An edit box keeps its caret (at the end) in view: the leading characters scroll out (UNVERIFIED rule - the game
                // moves its visible window, + 0x184 / + 0x188, only as far as the caret needs; FUN_008d2710).
                int32_t used = total;
                size_t drop = 0;
                while (drop < items.size() && used > px.width()) used -= advance(drop++);
                items.erase(items.begin(), items.begin() + std::ptrdiff_t(drop));
            } else {
                // Keep the characters that fit, then drop more until "..." fits too.
                int32_t used = 0;
                size_t keep = 0;
                while (keep < items.size() && used + advance(keep) <= px.width()) used += advance(keep++);
                const Face::Glyph& dot = baseFace->glyph(baseEm, '.', basePtr->bold, basePtr->shadow);
                const int32_t dots = dot.advance * 3;
                while (keep > 0 && used + dots > px.width()) used -= advance(--keep);
                items.resize(keep);
                for (int k = 0; k < 3; ++k) items.push_back(Item{'.', baseFace, baseEm, basePtr->bold, basePtr->shadow});
            }
        }
    }
    std::vector<std::pair<size_t, size_t>> lines;
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
    colourOf(widget, colour);
    const Target::Blend blend = lower(widget.attribute("effect")) == "truiadditivealpha.fx" ? Target::Blend::Additive : Target::Blend::Alpha;
    const std::string horiz = widget.attribute("horizAlign"), vert = widget.attribute("vertAlign");
    const int32_t lineHeight = baseFace->cellHeight(baseEm), leading = baseFace->internalLeading(baseEm);
    const int32_t blockHeight = int32_t(lines.size()) * lineHeight - leading;
    int32_t y = px.y0;
    if (vert == "center") y += (px.height() - blockHeight) / 2;
    else if (vert == "bottom") y += px.height() - blockHeight;
    const Rect area = intersect(px, target.pixels(clip));
    measuredEm_ = baseEm;
    measuredWidth_ = 0;
    for (const auto& line : lines) {
        int32_t lineWidth = 0;
        for (size_t i = line.first; i < line.second; ++i) lineWidth += advance(i);
        measuredWidth_ = std::max(measuredWidth_, lineWidth);
        if (measureOnly_) continue;
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

// Widget slot 73 (FUN_0089d3c0): the colour property, x the widget animation's colour when it has one (FUN_008b7280).
void UiCanvas::ownColour(const tr::XmlNode& widget, uint8_t rgba[4]) const {
    float f[4];
    const auto o = buttonOverrides_.find(&widget);
    if (o != buttonOverrides_.end() && o->second.colour) std::copy(o->second.rgba, o->second.rgba + 4, f);
    else widgetColour(widget, f);
    for (int c = 0; c < 4; ++c) rgba[c] = uint8_t(std::lround(std::clamp(f[c], 0.0f, 1.0f) * 255.0f));
    AnimFrame frame;
    if (sampleAnimation(widget.attribute("widgetAnimName"), false, frame) && frame.colour)
        for (int c = 0; c < 4; ++c) rgba[c] = multiplyColour(frame.rgba[c], rgba[c]);
}

// What a widget draws with (Bitmap render FUN_008f5300, the composite's FUN_008aee70): its colour x the render context colour, which
// every composite multiplies its own colour into for its children - so a container's colour (alpha 0 on some HUD groups) reaches them.
void UiCanvas::colourOf(const tr::XmlNode& widget, float rgba[4]) const {
    uint8_t own[4];
    ownColour(widget, own);
    for (int c = 0; c < 4; ++c) rgba[c] = float(multiplyColour(own[c], context_[c])) / 255.0f;
}

// Button states (tabula_rasa.exe, decompiled 2026-10-15). A push button (PushButton; ScrollUpBtn / ScrollDownBtn, Thumb, Track and Entry
// load the same attributes; loader FUN_008e2fc0) owns a Bitmap and a Text child (the export's children are those, dumped in the
// state they had: Up for all but 27 Disabled ones) and applies its state to them (FUN_008e42f0): texture (FUN_008e4460), text
// colour (FUN_008e4360, SetColor) and text anchor offset (FUN_008e43e0, SetAnchorOffset), each chosen as: not enabled -> Disabled;
// pushed (+0x139) -> Down; highlighted (+0x138) -> Highlighted; else Up (texture attributes Up / Highlighted / Down /
// DisabledTextureDef; colour and offset Up / Highlight / Down / DisabledTextColor / TextAnchorOffset). Defaults (constructor
// FUN_008e2b20): textures "", colours 0xFFFFFFFF, offsets (0, 0) - so a state without an attribute still sets white / (0, 0).
// A toggle button (Button, ToggleButton; FUN_008e7c30) owns a Bitmap and chooses its texture (FUN_008e8be0): not enabled ->
// DownDisabled if on, else UpDisabled; pressed (+0x18b) -> Pressed; highlighted (+0x18a) -> DownHighlighted / UpHighlighted; else
// Down / Up.
void UiCanvas::applyButtonState(const tr::XmlNode& button) {
    if ((options_.buttonState == 0 && !options_.interactiveButtons) || !isButton(button)) return;
    const tr::XmlNode* bitmap = nullptr;
    const tr::XmlNode* textNode = nullptr;
    for (const tr::XmlNode& c : button.children) {
        if (!bitmap && c.tag == "Bitmap") bitmap = &c;
        if (!textNode && c.tag == "Text") textNode = &c;
    }
    int state = options_.buttonState;                       // 1 up, 2 highlighted, 3 down, 4 disabled, 5 pressed
    const bool toggle = button.tag == "Button" || button.tag == "ToggleButton";
    bool on = options_.toggledOn;
    if (options_.interactiveButtons) {
        // enabled defaults to false: the Widget constructor (FUN_0089c280) zeroes the flag (+ 0x56, SetEnabled / IsEnabled = slots 114 / 115)
        // and the loader sets it only when the attribute is present (the export's 22 push buttons without it are exactly its Disabled ones).
        const bool hovered = options_.hoveredButton == &button;
        state = button.attribute("enabled") != "true" ? 4 : hovered && options_.pushedButton == &button ? (toggle ? 5 : 3) : hovered ? 2 : 1;
        on = options_.toggledButtons && options_.toggledButtons->count(&button) > 0;
    }
    if (toggle) {
        std::string def;
        if (state == 4) def = button.attribute(on ? "DownDisabledTextureDef" : "UpDisabledTextureDef");
        else if (state == 5) def = button.attribute("PressedTextureDef");
        else if (state == 2) def = button.attribute(on ? "DownHighlightedTextureDef" : "UpHighlightedTextureDef");
        else def = button.attribute(on ? "DownTextureDef" : "UpTextureDef");
        if (bitmap) {
            ButtonOverride& o = buttonOverrides_[bitmap];
            o.texture = true;
            o.textureDef = def;
        }
        return;
    }
    const char* texture = state == 4 ? "DisabledTextureDef" : state == 3 || state == 5 ? "DownTextureDef" : state == 2 ? "HighlightedTextureDef" : "UpTextureDef";
    const char* prefix = state == 4 ? "Disabled" : state == 3 || state == 5 ? "Down" : state == 2 ? "Highlight" : "Up";
    if (bitmap) {
        ButtonOverride& o = buttonOverrides_[bitmap];
        o.texture = true;
        o.textureDef = button.attribute(texture);
    }
    if (textNode) {
        ButtonOverride& o = buttonOverrides_[textNode];
        o.colour = true;
        const std::string p = prefix;
        if (button.hasAttribute(p + "TextColorR"))
            for (int c = 0; c < 4; ++c) o.rgba[c] = float(intAttribute(button, (p + "TextColor" + "RGBA"[c]).c_str(), 255)) / 255.0f;
        o.offset = true;
        o.offsetX = intAttribute(button, (p + "TextAnchorOffsetX").c_str());
        o.offsetY = intAttribute(button, (p + "TextAnchorOffsetY").c_str());
    }
}

// Progress bars (tabula_rasa.exe uidrawables, decompiled 2026-10-15): the loader FUN_008b3ee0 reads orientation (leftToRight 1, rightToLeft
// 2, bottomToTop 4, topToBottom 8; default 1), style (reveal 0, stretch 1, anything else - "slide" - 2; default 1) and percentComplete
// (float, default 0), owns a Bitmap (the export's Filler child) and lays it out at load and on every change (FUN_008b4440), f = trunc(w x p)
// or trunc(h x p):
// - reveal (FUN_008b44b0): the filler at the bar's size and (0, 0); the bar clips its children to the filled part (right = left + f,
//   left = right - f, top = bottom - f, bottom = top + f).
// - stretch (FUN_008b45b0): the filler sized (f, h) / (w, f), at (0, 0), or (w - f, 0) / (0, h - f) for rightToLeft / bottomToTop.
// - slide (FUN_008b4710): the filler at the bar's size, offset (f - w, 0), (w - f, 0), (0, h - f), (0, f - h).
// The other two clip to the whole bar. The export's Filler children are this layout at the exported percentComplete.
void UiCanvas::applyProgress(const tr::XmlNode& bar) {
    if (bar.tag != "ProgressBar") return;
    const tr::XmlNode* filler = nullptr;
    for (const tr::XmlNode& c : bar.children)
        if (c.tag == "Filler" || c.tag == "Bitmap") { filler = &c; break; }
    const std::string o = bar.attribute("orientation"), st = bar.attribute("style");
    const int orientation = o.empty() || o == "leftToRight" ? 1 : o == "rightToLeft" ? 2 : o == "bottomToTop" ? 4 : 8;
    const int style = st.empty() || st == "stretch" ? 1 : st == "reveal" ? 0 : 2;
    float p = options_.progress >= 0 ? options_.progress : 0.0f;
    if (options_.progress < 0 && bar.hasAttribute("percentComplete")) p = std::strtof(bar.attribute("percentComplete").c_str(), nullptr);
    const int32_t w = intAttribute(bar, "sizeW"), h = intAttribute(bar, "sizeH");
    const int32_t fw = int32_t(float(w) * p), fh = int32_t(float(h) * p);     // truncated, as _ftol
    Rect clipRect{0, 0, w, h};
    if (!filler) {
        childClips_[&bar] = clipRect;
        return;
    }
    ButtonOverride& f = buttonOverrides_[filler];
    f.size = true;
    f.offset = true;
    f.width = w;
    f.height = h;
    f.offsetX = 0;
    f.offsetY = 0;
    if (style == 0) {
        if (orientation == 1) clipRect.x1 = fw;
        else if (orientation == 2) clipRect.x0 = w - fw;
        else if (orientation == 4) clipRect.y0 = h - fh;
        else clipRect.y1 = fh;
    } else if (style == 1) {
        if (orientation == 1 || orientation == 2) f.width = fw;
        else f.height = fh;
        if (orientation == 2) f.offsetX = w - fw;
        if (orientation == 4) f.offsetY = h - fh;
    } else {
        if (orientation == 1) f.offsetX = fw - w;
        else if (orientation == 2) f.offsetX = w - fw;
        else if (orientation == 4) f.offsetY = h - fh;
        else f.offsetY = fh - h;
    }
    childClips_[&bar] = clipRect;
}

// Scroll bars (scrollbar.cpp; loader FUN_008d6ce0, decompiled 2026-10-15): own two arrow PushButtons, a Track and a Thumb (the export's
// children). Range min..max (FUN_008d7950), value clamped to min..max - pageSize + 1 (FUN_008d79e0); the thumb (FUN_008d84f0, anchors
// top / left, FUN_008d8410): ratio = (value - min) / ((max - min + 1) - pageSize) (0 when the range is smaller than the page; 0 / 0 when
// equal - NaN, _ftol 0x80000000, as one exported scroll bar shows), vertical (the default) offset (0, top button height +
// trunc((track height - thumb height) x ratio)), horizontal (left button width + trunc((track width - thumb width) x ratio), 0). All 63
// exported thumbs match. ("slide" stores the thumb's centre in a Track field, +0xb4 - its use is not traced.)
// Sliders (Slider, Selector, RangeTarget; loader FUN_008da640 not traced in detail): offset (trunc((track width - thumb width) x
// (value - min) / (max - min)), (track height - thumb height) / 2) fits all 11 exported thumbs exactly.
void UiCanvas::applyScroll(const tr::XmlNode& bar) {
    float ratio = options_.scroll;
    if (options_.scrollValues) {
        const auto own = options_.scrollValues->find(&bar);
        if (own != options_.scrollValues->end()) ratio = own->second;
    }
    if (ratio < 0 || !bar.hasAttribute("maxRange")) return;
    const tr::XmlNode* thumb = nullptr;
    const tr::XmlNode* track = nullptr;
    const tr::XmlNode* up = nullptr;
    const tr::XmlNode* down = nullptr;
    for (const tr::XmlNode& c : bar.children) {
        if (c.tag == "Thumb") thumb = &c;
        else if (c.tag == "Track") track = &c;
        else if (c.tag == "ScrollUpBtn") up = &c;
        else if (c.tag == "ScrollDownBtn") down = &c;
    }
    if (!thumb) return;
    const int32_t trackW = track ? intAttribute(*track, "sizeW") : intAttribute(bar, "sizeW");
    const int32_t trackH = track ? intAttribute(*track, "sizeH") : intAttribute(bar, "sizeH");
    const int32_t thumbW = intAttribute(*thumb, "sizeW"), thumbH = intAttribute(*thumb, "sizeH");
    const float r = std::clamp(ratio, 0.0f, 1.0f);
    ButtonOverride& o = buttonOverrides_[thumb];
    o.offset = true;
    if (bar.tag == "ScrollBar") {
        // The chosen position stands for value = min + r x (range - page), i.e. the same ratio.
        if (bar.attribute("style") == "horizontal") {
            o.offsetX = (down ? intAttribute(*down, "sizeW") : 0) + int32_t(float(trackW - thumbW) * r);
            o.offsetY = 0;
        } else {
            o.offsetX = 0;
            o.offsetY = (up ? intAttribute(*up, "sizeH") : 0) + int32_t(float(trackH - thumbH) * r);
        }
    } else {
        o.offsetX = int32_t(float(trackW - thumbW) * r);
        o.offsetY = (trackH - thumbH) / 2;
    }
}

void UiCanvas::drawWidget(const tr::XmlNode& widget, std::vector<int32_t>& path, const Placed& parent, const Rect& clip, bool branchRoot,
                          Target& target) {
    if (!branchRoot && !options_.showHidden && widget.attribute("visible") == "false") {
        const auto shown = buttonOverrides_.find(&widget);
        if (shown == buttonOverrides_.end() || !shown->second.show) return;
    }
    applyButtonState(widget);
    applyProgress(widget);
    applyScroll(widget);
    const Placed placed = place(widget, parent);
    const Rect rect = placed.rect;
    std::copy(parent.colour, parent.colour + 4, context_);
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
    if (widget.tag == "EditBox" || widget.tag == "PasswordBox") layoutEdit(widget, rect, clip, target);
    drawn_.push_back(DrawnWidget{path, rect, visual});
    // clipChildren defaults to true: the WidgetComposite constructor (FUN_008ae750) sets + 0xB0 = 1 and its loader (FUN_008ae9c0) only
    // overwrites it when the attribute is present (the export writes only "false"); the composite render (FUN_008aee70) clips its
    // children to its own rectangle when it is set. Only composites have children.
    Rect childClip = widget.attribute("clipChildren") != "false" ? intersect(clip, rect) : clip;
    if (const auto cc = childClips_.find(&widget); cc != childClips_.end())
        childClip = intersect(childClip, Rect{rect.x0 + cc->second.x0, rect.y0 + cc->second.y0, rect.x0 + cc->second.x1, rect.y0 + cc->second.y1});
    for (size_t k = 0; k < widget.children.size(); ++k) {
        path.push_back(int32_t(k));
        drawWidget(widget.children[k], path, placed, childClip, false, target);
        path.pop_back();
    }
    if (widget.tag == "EditBox" || widget.tag == "PasswordBox") drawEditText(widget, rect, childClip, target);
}

// Edit boxes (tabula_rasa.exe editbox.cpp, traced 2026-10-20): EditBox (constructor FUN_008ce7a0, loader FUN_008cebb0) owns a
// Caret (+ 0xf0) and a Highlight (+ 0xf4, selection) Bitmap; the loader sizes the caret 3 x the font's height and hides it; while the
// box has the keyboard (+ 0x18c) its update (FUN_008cffd0) toggles the caret every 500 ms (focus and typing show it, the timer at 0). Its render
// (vtable + 0x1b8, FUN_008cfb70; WidgetComposite's is FUN_008aee70) draws the composite (children) first, then the text: the prepared
// runs (FUN_008d2710) at x = the width of a prefix string (+ 0x11c, set by scripts - empty here) and y = (height - font height) / 2,
// in TextColor (+ 0xdc; default 0xFFFFFFFF, DAT_00d08b54 - no exported box sets it) and TextColorHighlighted for selected runs; the
// caret is placed (Widget slot + 0x148) at (width of the text before it + the prefix, (height - font height) / 2). A PasswordBox draws
// drawableChar for every character. UNVERIFIED: the font height is taken as the em (the game reads the font's + 0x10 x the UI scale),
// the text colour is multiplied by the render context like a Text's, the scrolling rule.
bool UiCanvas::editText(const tr::XmlNode& box, std::string& text) const {
    text.clear();
    if (options_.editTexts) {
        const auto it = options_.editTexts->find(&box);
        if (it != options_.editTexts->end()) text = it->second;
    }
    if (box.tag == "PasswordBox" && !text.empty()) {
        const uint32_t cp = uint32_t(std::strtoul(box.attribute("drawableChar").c_str(), nullptr, 10));
        std::string glyph;
        auto put = [&glyph](uint32_t c) {
            if (c < 0x80) glyph += char(c);
            else if (c < 0x800) { glyph += char(0xC0 | (c >> 6)); glyph += char(0x80 | (c & 0x3F)); }
            else { glyph += char(0xE0 | (c >> 12)); glyph += char(0x80 | ((c >> 6) & 0x3F)); glyph += char(0x80 | (c & 0x3F)); }
        };
        put(cp ? cp : '*');
        const size_t n = codepoints(text).size();
        text.clear();
        for (size_t k = 0; k < n; ++k) text += glyph;
    }
    return !text.empty() || options_.focusedEdit == &box;
}

// The width of a string in the box's font, pixels (the game measures each run apart, FUN_008f9d70).
int32_t UiCanvas::measureEdit(const tr::XmlNode& box, const Rect& rect, const Rect& clip, Target& target, const std::string& text) {
    if (text.empty()) return 0;
    tr::XmlNode line;
    line.tag = "Text";
    line.attributes = {{"fontId", box.attribute("fontId")}, {"vertAlign", "center"}};
    textOverrides_[&line] = text;
    measureOnly_ = scrollTail_ = true;
    drawText(line, Rect{rect.x0, rect.y0, rect.x0 + 100000, rect.y1}, clip, target);
    measureOnly_ = scrollTail_ = false;
    textOverrides_.erase(&line);
    return measuredWidth_;
}

namespace {
std::string utf8Of(const std::vector<uint32_t>& cps, size_t from, size_t to) {
    std::string s;
    for (size_t k = from; k < to && k < cps.size(); ++k) {
        const uint32_t c = cps[k];
        if (c < 0x80) s += char(c);
        else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
        else { s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    }
    return s;
}
}  // namespace

// EditBox layout (FUN_008d2710, traced 2026-10-20). The visible window [+ 0x184, + 0x188) follows the caret (+ 0x144): a caret before the
// window's start becomes the start and the end is worked out from it (vtable + 0x20c); one past the end (or an end past the text)
// becomes the end and the start is worked out back from it (+ 0x208) - both NOT read: the editor fits as many whole characters as the
// box's width holds. The window's text is laid out as runs (+ 0x134, 0x30 bytes: string, highlighted + 0x1c, x / y): without a
// selection (+ 0x13c == + 0x140) one run and the Highlight (+ 0xf4) hidden; with one, the text before it, the selection clipped to the
// window (highlighted), then the rest; the Highlight is shown, placed at the selection's run (Widget + 0x148) and sized to it (+ 0x128:
// the run's width x the font height, from FUN_008f9d70). The caret goes to (width of the window's text before it + the prefix,
// (height - font height) / 2).
void UiCanvas::layoutEdit(const tr::XmlNode& box, const Rect& rect, const Rect& clip, Target& target) {
    const tr::XmlNode* caret = nullptr;
    const tr::XmlNode* highlight = nullptr;
    for (const tr::XmlNode& c : box.children) {
        if (c.tag == "Caret") caret = &c;
        if (c.tag == "Highlight") highlight = &c;
    }
    std::string shown;
    editText(box, shown);
    const std::vector<uint32_t> cps = codepoints(shown);
    const size_t n = cps.size();
    const bool focused = options_.focusedEdit == &box;
    const size_t caretAt = focused ? std::min(options_.caretIndex, n) : n;
    size_t selLo = focused ? std::min(std::min(options_.selectionStart, options_.selectionEnd), n) : 0;
    size_t selHi = focused ? std::min(std::max(options_.selectionStart, options_.selectionEnd), n) : 0;
    const float sx = std::max(target.sx, 0.01f);
    const int32_t boxWidth = int32_t(float(rect.width()) * sx);
    // Prefix widths from a start character (pixels).
    auto widthBetween = [&](size_t from, size_t to) { return measureEdit(box, rect, clip, target, utf8Of(cps, from, to)); };
    auto endFrom = [&](size_t start) {
        size_t end = start;
        while (end < n && widthBetween(start, end + 1) <= boxWidth) ++end;
        return end;
    };
    auto startBack = [&](size_t end) {
        size_t start = end;
        while (start > 0 && widthBetween(start - 1, end) <= boxWidth) --start;
        return start;
    };
    size_t first = 0;
    if (options_.editWindows) {
        const auto it = options_.editWindows->find(&box);
        if (it != options_.editWindows->end()) first = std::min(it->second, n);
    }
    size_t last = endFrom(first);
    if (caretAt < first) {
        first = caretAt;
        last = endFrom(first);
    } else if (caretAt > last) {
        last = caretAt;
        first = startBack(last);
    } else if (last < n && first > 0 && widthBetween(first - 1, last) <= boxWidth) {
        first = startBack(last);               // room again (text removed): show more of what is before
    }
    if (options_.editWindows) (*options_.editWindows)[&box] = first;
    EditLayout& layout = editLayouts_[&box];
    layout = EditLayout{};
    layout.left = rect.x0;
    layout.first = first;
    for (size_t k = first; k <= last; ++k) layout.edges.push_back(rect.x0 + int32_t(float(widthBetween(first, k)) / sx));
    auto logicalX = [&](size_t k) { return layout.edges[std::clamp(k, first, last) - first] - rect.x0; };
    selLo = std::clamp(selLo, first, last);
    selHi = std::clamp(selHi, first, last);
    if (selLo >= selHi) {
        if (first < last) layout.runs.push_back(EditRun{utf8Of(cps, first, last), 0, false});
    } else {
        if (first < selLo) layout.runs.push_back(EditRun{utf8Of(cps, first, selLo), 0, false});
        layout.runs.push_back(EditRun{utf8Of(cps, selLo, selHi), logicalX(selLo), true});
        if (selHi < last) layout.runs.push_back(EditRun{utf8Of(cps, selHi, last), logicalX(selHi), false});
    }
    measureEdit(box, rect, clip, target, " ");
    const int32_t emLogical = std::max(1, int32_t(std::lround(float(measuredEm_) / std::max(target.sy, 0.01f))));
    if (highlight) {
        ButtonOverride& o = buttonOverrides_[highlight];
        o = ButtonOverride{};
        if (focused && selLo < selHi) {
            o.show = true;
            o.offset = true;
            o.offsetX = logicalX(selLo);
            o.offsetY = (rect.height() - emLogical) / 2;
            o.size = true;
            o.width = std::max(1, logicalX(selHi) - logicalX(selLo));
            o.height = emLogical;
        }
    }
    if (!caret || !focused || !options_.caretVisible) return;
    ButtonOverride& o = buttonOverrides_[caret];
    o.show = true;
    o.offset = true;
    o.offsetX = logicalX(caretAt);
    o.offsetY = (rect.height() - emLogical) / 2;
    o.size = true;
    o.width = 3;
    o.height = emLogical;
}

// The runs, in TextColor (+ 0xdc) or, for the selection, TextColorHighlighted (+ 0xe0), each x the render context's colour
// (FUN_008cfb70 multiplies both by the context with FUN_008b7280 and passes the highlighted one for runs with + 0x1c set).
void UiCanvas::drawEditText(const tr::XmlNode& box, const Rect& rect, const Rect& clip, Target& target) {
    const auto it = editLayouts_.find(&box);
    if (it == editLayouts_.end()) return;
    for (const EditRun& run : it->second.runs) {
        tr::XmlNode line;
        line.tag = "Text";
        line.attributes = {{"fontId", box.attribute("fontId")}, {"vertAlign", "center"}};
        const std::string colour = run.highlighted ? "TextColorHighlighted" : "TextColor";
        for (const char* c : {"R", "G", "B", "A"})
            if (box.hasAttribute(colour + c)) line.attributes.emplace_back(std::string("color") + c, box.attribute(colour + c));
        textOverrides_[&line] = run.text;
        scrollTail_ = true;
        drawText(line, Rect{rect.x0 + run.x, rect.y0, rect.x1 + 100000, rect.y1}, intersect(clip, rect), target);
        scrollTail_ = false;
        textOverrides_.erase(&line);
    }
}

size_t UiCanvas::editIndexAt(const tr::XmlNode* box, int32_t x) const {
    const auto it = editLayouts_.find(box);
    if (it == editLayouts_.end() || it->second.edges.empty()) return 0;
    const std::vector<int32_t>& edges = it->second.edges;
    size_t best = 0;
    for (size_t k = 1; k < edges.size(); ++k)
        if (std::abs(edges[k] - x) < std::abs(edges[best] - x)) best = k;
    return it->second.first + best;
}

void UiCanvas::fillBackground(Target& target) const {
    const int32_t screenW = target.width, screenH = target.height;
    const Options& options = options_;
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
}

void UiCanvas::finish(const Target& target, std::vector<uint8_t>& rgba) const {
    const int32_t screenW = target.width, screenH = target.height;
    const size_t count = size_t(screenW) * size_t(screenH);
    rgba.resize(count * 4);
    for (size_t k = 0; k < count; ++k) {
        for (int c = 0; c < 3; ++c) rgba[k * 4 + size_t(c)] = uint8_t(std::lround(std::clamp(target.rgb[k * 3 + size_t(c)], 0.0f, 1.0f) * 255.0f));
        rgba[k * 4 + 3] = 255;
    }
}

void UiCanvas::renderDetached(const tr::XmlNode& widget, int32_t pixelWidth, int32_t pixelHeight, float scale, const Options& options,
                              const std::string& sampleText, std::vector<uint8_t>& rgba) {
    detachedRoot_ = &widget;                // paths of this render (and the queries after it) start at the widget
    drawn_.clear();
    editLayouts_.clear();
    options_ = options;
    options_.screenWidth = options_.screenHeight = 0;
    options_.backgroundRgba = nullptr;
    animating_ = false;
    buttonOverrides_.clear();
    childClips_.clear();
    if (scale <= 0) scale = 1;
    pixelWidth_ = std::max(pixelWidth, 1);
    pixelHeight_ = std::max(pixelHeight, 1);
    width_ = std::max(int32_t(std::lround(float(pixelWidth_) / scale)), 1);
    height_ = std::max(int32_t(std::lround(float(pixelHeight_) / scale)), 1);
    Target target;
    target.width = pixelWidth_;
    target.height = pixelHeight_;
    target.sx = target.sy = scale;
    fillBackground(target);
    Placed frame;
    frame.rect = Rect{0, 0, width_, height_};
    frame.w = width_;
    frame.h = height_;
    sampleNode_ = &widget;
    sampleText_ = sampleText;
    std::vector<int32_t> walk;
    drawWidget(widget, walk, frame, frame.rect, true, target);
    sampleNode_ = nullptr;
    finish(target, rgba);
}

uint32_t UiCanvas::animationLength(const std::string& name) const {
    const auto it = animations_.find(name);
    return it == animations_.end() ? 0 : it->second.length;
}

std::vector<UiCanvas::DrawnWidget> UiCanvas::render(const std::vector<int32_t>& path, const Options& options, std::vector<uint8_t>& rgba) {
    detachedRoot_ = nullptr;
    drawn_.clear();
    editLayouts_.clear();
    options_ = options;
    animating_ = false;
    buttonOverrides_.clear();
    childClips_.clear();
    // The logical size is the exe's (FUN_004cd8c0: 1280 x 960); the export's Definitions/LogicalSize is not read by the game.
    const int32_t logicalW = 1280, logicalH = 960;
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
    fillBackground(target);
    const Rect screen{0, 0, width_, height_};
    const tr::XmlNode* n = node(path);
    if (n && !path.empty()) {
        std::vector<int32_t> parentPath(path.begin(), path.end() - 1);
        std::vector<int32_t> walk = path;
        drawWidget(*n, walk, placedAt(parentPath), screen, true, target);
    } else if (n) {                            // the <Widgets> element itself: every root widget
        const Placed root = placedAt({});
        for (size_t k = 0; k < n->children.size(); ++k) {
            std::vector<int32_t> walk{int32_t(k)};
            drawWidget(n->children[k], walk, root, screen, false, target);
        }
    }
    finish(target, rgba);
    return drawn_;
}

const tr::XmlNode* UiCanvas::thumbAt(int32_t x, int32_t y) const {
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it) {
        const tr::XmlNode* n = node(it->path);
        if (n && n->tag == "Thumb" && it->rect.contains(x, y)) return n;
    }
    return nullptr;
}

bool UiCanvas::drawnRect(const tr::XmlNode* widget, Rect& rect) const {
    for (const DrawnWidget& d : drawn_)
        if (node(d.path) == widget) {
            rect = d.rect;
            return true;
        }
    return false;
}

const tr::XmlNode* UiCanvas::parentOf(const tr::XmlNode* child) const {
    for (const DrawnWidget& d : drawn_)
        if (node(d.path) == child && !d.path.empty()) {
            std::vector<int32_t> up = d.path;
            up.pop_back();
            return up.empty() ? nullptr : node(up);
        }
    return nullptr;
}

float UiCanvas::thumbRatioAt(const tr::XmlNode& bar, int32_t x, int32_t y) const {
    // The bar's drawn rectangle and the same sizes applyScroll uses: thumb offset = lead (+ arrow button) + trunc(travel x r).
    Rect barRect{0, 0, 0, 0};
    bool found = false;
    for (const DrawnWidget& d : drawn_)
        if (node(d.path) == &bar) {
            barRect = d.rect;
            found = true;
            break;
        }
    if (!found) return 0.0f;
    const tr::XmlNode *thumb = nullptr, *track = nullptr, *up = nullptr, *down = nullptr;
    for (const tr::XmlNode& c : bar.children) {
        if (c.tag == "Thumb") thumb = &c;
        else if (c.tag == "Track") track = &c;
        else if (c.tag == "ScrollUpBtn") up = &c;
        else if (c.tag == "ScrollDownBtn") down = &c;
    }
    if (!thumb) return 0.0f;
    const int32_t trackW = track ? intAttribute(*track, "sizeW") : intAttribute(bar, "sizeW");
    const int32_t trackH = track ? intAttribute(*track, "sizeH") : intAttribute(bar, "sizeH");
    const int32_t thumbW = intAttribute(*thumb, "sizeW"), thumbH = intAttribute(*thumb, "sizeH");
    const bool vertical = bar.tag == "ScrollBar" && bar.attribute("style") != "horizontal";
    if (vertical) {
        const int32_t lead = barRect.y0 + (up ? intAttribute(*up, "sizeH") : 0);
        return trackH > thumbH ? std::clamp(float(y - lead) / float(trackH - thumbH), 0.0f, 1.0f) : 0.0f;
    }
    const int32_t lead = barRect.x0 + (bar.tag == "ScrollBar" && down ? intAttribute(*down, "sizeW") : 0);
    return trackW > thumbW ? std::clamp(float(x - lead) / float(trackW - thumbW), 0.0f, 1.0f) : 0.0f;
}

// Scroll bar presses (tabula_rasa.exe scrollbar.cpp, traced 2026-10-20). The loader (FUN_008d6ce0) keeps its parts at + 0xcc
// LesserButton / ScrollDownBtn, + 0xc8 GreaterButton / ScrollUpBtn, + 0xd0 Track, + 0xd4 Thumb; the press handler (0x008d7cd0)
// on ScrollDownBtn sets value + 1, on ScrollUpBtn max(min, value - 1), on the Track (FUN_008d8690) value - 1 when the mouse is
// before the thumb along the bar (x for horizontal, style 0; y for vertical) and value + 1 when after it, nothing on the thumb;
// setValue (FUN_008d79e0) clamps to min .. max - pageSize + 1 (min when that is not above min). The control held keeps firing
// (FUN_008d7c00): elapsed ms summed, past 599 it fires again and 100 is taken off - repeats at 600, 700, 800 ... ms, the track
// re-testing the mouse against the thumb each time. Sliders are not handled (their loader is not read).
bool UiCanvas::scrollStepAt(int32_t x, int32_t y, const tr::XmlNode*& bar, int& step) const {
    bar = nullptr;
    step = 0;
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it) {
        if (!it->rect.contains(x, y) || it->path.empty()) continue;
        const tr::XmlNode* n = node(it->path);
        if (!n || (n->tag != "ScrollUpBtn" && n->tag != "ScrollDownBtn" && n->tag != "Track" && n->tag != "Thumb")) continue;
        std::vector<int32_t> up = it->path;
        up.pop_back();
        const tr::XmlNode* parent = node(up);   // {} = the root (a detached render's widget)
        if (!parent || parent->tag != "ScrollBar") continue;
        bar = parent;
        if (n->tag == "ScrollDownBtn") step = 1;
        else if (n->tag == "ScrollUpBtn") step = -1;
        else if (n->tag == "Track") {
            const tr::XmlNode* thumb = nullptr;
            for (const tr::XmlNode& c : parent->children)
                if (c.tag == "Thumb") thumb = &c;
            Rect t{0, 0, 0, 0};
            if (!thumb || !drawnRect(thumb, t)) return true;
            const bool horizontal = parent->attribute("style") == "horizontal";
            const int32_t at = horizontal ? x : y, low = horizontal ? t.x0 : t.y0, high = horizontal ? t.x1 : t.y1;
            step = at < low ? -1 : at >= high ? 1 : 0;
        }
        return true;
    }
    return false;
}

float UiCanvas::scrollRatioStepped(const tr::XmlNode& bar, float ratio, int step) {
    const int32_t minimum = intAttribute(bar, "minRange"), maximum = intAttribute(bar, "maxRange"), page = intAttribute(bar, "pageSize");
    const int32_t top = maximum - page + 1;                        // setValue's upper limit
    if (top <= minimum) return 0.0f;
    const float span = float(top - minimum);                        // (max - min + 1) - pageSize, the thumb ratio's denominator
    if (ratio < 0) ratio = std::clamp(float(intAttribute(bar, "currentValue") - minimum) / span, 0.0f, 1.0f);   // as exported
    int32_t value = minimum + int32_t(std::lround(std::clamp(ratio, 0.0f, 1.0f) * span)) + step;
    value = std::clamp(value, minimum, top);
    return float(value - minimum) / span;
}

// Sliders (slider.cpp, traced 2026-10-20): the loader (FUN_008da640) keeps + 0xc4 Track and + 0xc8 Thumb (no arrow buttons); style
// "horizontal" = 0 (+ 0xc0), min + 0xe0, max + 0xe4, value + 0xec. A press on the Track (FUN_008db250) sets value = min + trunc((max - min)
// x f + 0.5) with f = (mouse - track start) / track length along the slider's axis, clamped to min .. max by setValue (FUN_008db030); the
// thumb ratio is (value - min) / (max - min) (FUN_008db630). The mouse wheel (both kinds: scroll bar FUN_008d7ca0, slider FUN_008db220)
// adds the event's delta to the value; the UI makes it -(WM_MOUSEWHEEL delta / 120) (FUN_00902870: one notch up = -1).
bool UiCanvas::sliderPressAt(int32_t x, int32_t y, const tr::XmlNode*& slider, float& ratio) const {
    slider = nullptr;
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it) {
        if (!it->rect.contains(x, y) || it->path.empty()) continue;
        const tr::XmlNode* n = node(it->path);
        if (!n || n->tag != "Track") continue;
        std::vector<int32_t> up = it->path;
        up.pop_back();
        const tr::XmlNode* parent = node(up);   // {} = the root (a detached render's widget)
        if (!parent || parent->tag == "ScrollBar" || !parent->hasAttribute("maxRange")) continue;
        const int32_t minimum = intAttribute(*parent, "minRange"), maximum = intAttribute(*parent, "maxRange");
        const bool vertical = parent->attribute("style") == "vertical";
        const int32_t start = vertical ? it->rect.y0 : it->rect.x0, length = vertical ? it->rect.height() : it->rect.width();
        if (length <= 0 || maximum <= minimum) return false;
        const float f = float((vertical ? y : x) - start) / float(length);
        const int32_t value = std::clamp(minimum + int32_t(float(maximum - minimum) * f + 0.5f), minimum, maximum);
        slider = parent;
        ratio = float(value - minimum) / float(maximum - minimum);
        return true;
    }
    return false;
}

const tr::XmlNode* UiCanvas::wheelTargetAt(int32_t x, int32_t y) const {
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it) {
        if (!it->rect.contains(x, y)) continue;
        for (std::vector<int32_t> p = it->path;; p.pop_back()) {
            const tr::XmlNode* n = node(p);
            if (n && n->hasAttribute("maxRange") && (n->tag == "ScrollBar" || n->tag == "Slider" || n->tag == "Selector" || n->tag == "RangeTarget"))
                return n;
            if (p.empty()) break;
        }
    }
    return nullptr;
}

float UiCanvas::sliderRatioStepped(const tr::XmlNode& slider, float ratio, int step) {
    const int32_t minimum = intAttribute(slider, "minRange"), maximum = intAttribute(slider, "maxRange");
    if (maximum <= minimum) return 0.0f;
    const float span = float(maximum - minimum);
    if (ratio < 0) ratio = std::clamp(float(intAttribute(slider, "currentValue") - minimum) / span, 0.0f, 1.0f);
    const int32_t value = std::clamp(minimum + int32_t(std::lround(std::clamp(ratio, 0.0f, 1.0f) * span)) + step, minimum, maximum);
    return float(value - minimum) / span;
}

const tr::XmlNode* UiCanvas::buttonAt(int32_t x, int32_t y) const {
    // The mouse reaches the last drawn pickable widget under it (pickable defaults to false: Widget constructor FUN_0089c280 + 0x55,
    // set by the loader only when present). Composites take the mouse themselves only with mouseCapture (default false, + 0xB1 in
    // FUN_008ae750) - UNVERIFIED: the engine's hit test is not traced; without this an empty button group drawn over another
    // (LogOutWindow's QuitButtons / LogoutButtons) would hide its buttons.
    std::vector<int32_t> path;
    for (auto it = drawn_.rbegin(); it != drawn_.rend() && path.empty(); ++it) {
        const tr::XmlNode* n = node(it->path);
        if (!n || !it->rect.contains(x, y) || n->attribute("pickable") != "true") continue;
        if (!n->children.empty() && !isButton(*n) && n->attribute("mouseCapture") != "true") continue;
        path = it->path;
    }
    while (!path.empty()) {
        const tr::XmlNode* n = node(path);
        if (n && isButton(*n)) return n;
        path.pop_back();
    }
    return nullptr;
}

std::vector<int32_t> UiCanvas::pick(int32_t x, int32_t y) const {
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it)
        if (it->visual && it->rect.contains(x, y)) return it->path;
    const DrawnWidget* best = nullptr;
    for (const DrawnWidget& d : drawn_)
        if (d.rect.contains(x, y) && (!best || d.path.size() >= best->path.size())) best = &d;
    return best ? best->path : std::vector<int32_t>{};
}
