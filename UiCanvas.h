#pragma once

#include "assetlibrary.h"
#include "gamedata.h"
#include "ui.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Lays out and draws a branch of the game's UI export (tr::UiExport, data\xml\Tabula_Rasa_UI_EXPORT.xml) into an RGBA image of
// the export's logical size (1280 x 960), on the CPU.
//
// Layout (tabula_rasa.exe, verified 2026-10-15): a widget's position is relative to its parent: the parent's anchor point
// (parentAnchor) + anchorOffset - its own anchor point (childAnchor) (FUN_008b6fb0); anchor points (FUN_008b7060): 0 top left,
// 1 top (w/2, 0), 2 top right, 3 right (w, h/2), 4 bottom right, 5 bottom (w/2, h), 6 bottom left, 7 left (0, h/2), 8 centre
// (w/2, h/2), halves by integer division - the uianchor module's eWidgetAnchor_TopLeft .. Center (FUN_0051f2f0); both anchors
// default to 0 (Widget constructor FUN_0089c280); resizeToParent takes the parent's size (Widget layout FUN_0089d740). The
// screen's root widgets sit in the logical frame.
//
// Drawing: widgets with a textureDef (Bitmap, Filler, frame pieces, caret, …) draw that texture region, ColoredRect a colour,
// Text / RichText their textId's string; every other kind only holds children, which draw after their parent in file order.
// Colour = colorR/G/B/A (default 255) x texel. Blending per effect (effects.glm UI effects, pass states and pixel shaders read
// 2026-10-15): PalUIDefault (texture x colour), PalUIColoredRect (colour) and PalUIMultiTex blend SRCALPHA / INVSRCALPHA;
// TRUIAdditiveAlpha and PalUIMultiTexAdd SRCALPHA / ONE; PalUIMultiTex* and TRUIRadarMask multiply by a second map
// (maskTextureName). UNVERIFIED: the effect of widgets that name none (drawn like PalUIDefault), the mask's coordinates (spread
// over the widget), TRUIBackgroundMask (the "wire" lines: texture x a runtime mouse mask, ONE / ONE - not drawn unless asked),
// scaleToFit (text: see drawText - the game's GDI glyphs reproduced with FreeType).
class UiCanvas {
public:
    struct Rect {
        int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int32_t width() const { return x1 - x0; }
        int32_t height() const { return y1 - y0; }
        bool contains(int32_t x, int32_t y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    };
    struct Options {
        bool showHidden = false;            // draw widgets with visible="false" below the chosen branch
        bool backgroundMaskWires = false;   // draw TRUIBackgroundMask widgets at full strength (their mouse mask is runtime)
        int background = 0;                 // 0 black, 1 grey, 2 checkerboard
        // The screen the UI is drawn for, in pixels (0: the export's logical size, 1280 x 960, at one pixel per unit). Wider
        // than 1.5 the root is 1706 x 960 logical, as in the game; widgets are laid out in logical units and drawn at pixels.
        int32_t screenWidth = 0, screenHeight = 0;
        // Draw over this image instead of the background colour (screen-sized RGBA; for comparisons with game frames and for
        // drawing several branches in turn).
        const std::vector<uint8_t>* backgroundRgba = nullptr;
    };
    int32_t width() const { return width_; }              // the last render's root, logical units
    int32_t height() const { return height_; }
    int32_t pixelWidth() const { return pixelWidth_; }    // the last render's image
    int32_t pixelHeight() const { return pixelHeight_; }
    // A widget as drawn: its path (child indices from the export's <Widgets>) and screen rectangle.
    struct DrawnWidget {
        std::vector<int32_t> path;
        Rect rect;
        bool visual = false;                // drew a texture, colour or text
    };

    UiCanvas();
    ~UiCanvas();
    UiCanvas(const UiCanvas&) = delete;
    UiCanvas& operator=(const UiCanvas&) = delete;

    void setSources(const tr::UiExport* ui, const tr::AssetLibrary* assets, tr::GameData* game, const std::string& gameRoot);

    // The node at a path from <Widgets> (null if the path is invalid) and its screen rectangle.
    static const tr::XmlNode* node(const tr::UiExport& ui, const std::vector<int32_t>& path);
    Rect layout(const std::vector<int32_t>& path) const;

    // Draws the branch at path (the branch's root is drawn even when hidden; ancestors are laid out, not drawn) into rgba
    // (pixelWidth() x pixelHeight() x 4 bytes). Returns the widgets drawn (logical rectangles), in drawing order.
    std::vector<DrawnWidget> render(const std::vector<int32_t>& path, const Options& options, std::vector<uint8_t>& rgba);
    // The widget under a point of the last render: the last drawn visual widget containing it, else the deepest container.
    std::vector<int32_t> pick(int32_t x, int32_t y) const;

    std::string text(const tr::XmlNode& widget);                    // a Text widget's string ("" if none)
    const std::set<std::string>& missingTextures() const { return missingTextures_; }
    const std::set<std::string>& missingFonts() const { return missingFonts_; }
    const std::set<std::string>& missingTextIds() const { return missingTextIds_; }

private:
    struct Image {
        int32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    struct Face;
    struct FontLibrary;
    struct Target;
    const Image* image(const std::string& name);
    Face* face(const std::string& family);
    Rect childRect(const tr::XmlNode& widget, const Rect& parent) const;
    void drawWidget(const tr::XmlNode& widget, std::vector<int32_t>& path, const Rect& parent, const Rect& clip, bool branchRoot, Target& target);
    void drawTexture(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target);
    void drawColour(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target);
    void drawText(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target);

    const tr::UiExport* ui_ = nullptr;
    const tr::AssetLibrary* assets_ = nullptr;
    tr::GameData* game_ = nullptr;
    std::string gameRoot_;
    Options options_;
    int32_t width_ = 1280, height_ = 960;
    int32_t pixelWidth_ = 1280, pixelHeight_ = 960;
    std::map<std::string, std::unique_ptr<Image>> images_;          // by lower-case name; null = failed
    std::map<std::string, std::unique_ptr<Face>> faces_;            // by family name; null = not found
    std::unique_ptr<FontLibrary> fontLibrary_;                      // FreeType
    std::vector<std::pair<std::string, std::shared_ptr<std::vector<uint8_t>>>> fontFiles_;   // (path, data), loaded on demand
    bool fontFilesListed_ = false;
    std::vector<DrawnWidget> drawn_;
    std::set<std::string> missingTextures_, missingFonts_, missingTextIds_;
};
