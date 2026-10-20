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
// (maskTextureName); widgets that name no effect draw as PalUIDefault and TRUIBackgroundMask (the "wire" lines) as texture x the
// mouse mask, ONE / ONE (both verified in the fountain trace, see drawTexture). A composite's colour multiplies into
// everything under it (the render context, see place). Animations, progress bars, scroll bars, button states: see the .cpp.
// UNVERIFIED: the maskTextureName masks' coordinates (spread over the widget); text: see drawText - the game's GDI glyphs reproduced
// with FreeType.
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
        // TRUIBackgroundMask widgets (the window "wires") glow only around the mouse: texture x ui_mouse_mask.dds over a 256-unit square
        // centred on it (see drawTexture). Off: no mouse over the UI - the mask's clamped edge (1/255) leaves them invisible.
        bool mouseHighlight = false;
        float mouseX = 0, mouseY = 0;       // logical units
        // Button state (see drawWidget): 0 as exported (the export's Bitmap / Text children: Up, or Disabled for a few), 1 up,
        // 2 highlighted, 3 down (pushed), 4 disabled, 5 pressed (toggle buttons; push buttons show down). toggledOn: toggle buttons'
        // Down variants.
        int buttonState = 0;
        bool toggledOn = false;
        // Interactive buttons (replaces buttonState / toggledOn): each button's state from the mouse - disabled when not enabled,
        // pushed (down / pressed) while the mouse is down on the hovered button, highlighted when hovered, else up; toggle buttons
        // are on when in toggledButtons.
        bool interactiveButtons = false;
        const tr::XmlNode* hoveredButton = nullptr;
        const tr::XmlNode* pushedButton = nullptr;
        const std::set<const tr::XmlNode*>* toggledButtons = nullptr;
        // Progress bars' fill, 0..1; below 0: each bar's own percentComplete (as the engine applies it at load).
        float progress = -1.0f;
        // Scroll bars' and sliders' thumbs, 0..1 of their travel; below 0: as exported (each one's currentValue).
        float scroll = -1.0f;
        // Interactive mode: each scroll bar's / slider's own thumb position (0..1 of its travel), taking precedence over `scroll`.
        const std::map<const tr::XmlNode*, float>* scrollValues = nullptr;
        // Animations (Definitions/Animations, see sampleAnimation): milliseconds since every widget's animation started; below 0: none
        // applied (the export's own state).
        float animTime = -1.0f;
        // Edit mode: the widget being dragged and by how much (logical units), drawn moved before the edit is written.
        const tr::XmlNode* dragNode = nullptr;
        int32_t dragX = 0, dragY = 0;          // added to its anchorOffset
        int32_t dragW = 0, dragH = 0;          // added to its size (resizing)
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
    // The same game, an edited export (keeps the loaded textures and fonts).
    void setExport(const tr::UiExport* ui);

    // Which tree paths start from: <Widgets> (the screen), or Definitions/TemplateWidgets (unparented trees the scripts instantiate;
    // drawn where they would sit on the screen's root).
    void setTemplates(bool templates) { templates_ = templates; }
    bool templates() const { return templates_; }
    const tr::XmlNode* root() const { return ui_ ? (templates_ ? &ui_->templates : &ui_->widgets) : nullptr; }
    // The node at a path from the root (null if the path is invalid) and its screen rectangle.
    const tr::XmlNode* node(const std::vector<int32_t>& path) const;
    Rect layout(const std::vector<int32_t>& path) const;
    // The anchorOffset that puts the element at path at rect (logical), given its anchors and the rect's size (the layout rule
    // backwards: offset = rect corner - parent position - parent anchor point + own anchor point).
    void offsetsFor(const std::vector<int32_t>& path, const Rect& rect, int32_t& offsetX, int32_t& offsetY) const;

    // Draws the branch at path (the branch's root is drawn even when hidden; ancestors are laid out, not drawn) into rgba
    // (pixelWidth() x pixelHeight() x 4 bytes). Returns the widgets drawn (logical rectangles), in drawing order.
    std::vector<DrawnWidget> render(const std::vector<int32_t>& path, const Options& options, std::vector<uint8_t>& rgba);
    // Definitions previews: draws `widget` - a node the caller built, not part of the export (a Text with a fontId, a copy of an
    // element that uses an animation, ...) - on its own in a pixelWidth x pixelHeight image at `scale` pixels per logical unit (the
    // game's screen scale: 1.125 on a 1080-line screen), laid out in that frame as a root widget. A Text widget shows sampleText
    // instead of its textId. Options as for render (animTime, background, buttonState, ...; screen size and backgroundRgba ignored).
    void renderDetached(const tr::XmlNode& widget, int32_t pixelWidth, int32_t pixelHeight, float scale, const Options& options,
                        const std::string& sampleText, std::vector<uint8_t>& rgba);
    uint32_t animationLength(const std::string& name) const;       // milliseconds (the longest track's last key); 0 if none
    // The widget under a point of the last render: the last drawn visual widget containing it, else the deepest container.
    std::vector<int32_t> pick(int32_t x, int32_t y) const;
    // The button (a widget with button textures) under a point of the last render: the picked widget or its nearest such ancestor.
    const tr::XmlNode* buttonAt(int32_t x, int32_t y) const;
    // Interactive scroll bars / sliders: the thumb drawn under (x, y) (or nullptr), its bar, and the thumb position (0..1) that puts
    // the thumb's top left at (x - grabX, y - grabY) - applyScroll's layout backwards.
    const tr::XmlNode* thumbAt(int32_t x, int32_t y) const;
    const tr::XmlNode* parentOf(const tr::XmlNode* child) const;
    bool drawnRect(const tr::XmlNode* widget, Rect& rect) const;     // where the last draw put it
    float thumbRatioAt(const tr::XmlNode& bar, int32_t x, int32_t y) const;
    static bool isButton(const tr::XmlNode& n) { return n.hasAttribute("UpTextureDef"); }
    // Whether the last render sampled an animation that has not come to rest (repeat / pingpong, or before its end).
    bool animating() const { return animating_; }

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
    // A laid-out widget as its children see it (Widget layout FUN_0089d740): its logical rectangle, its size property (unscaled; the
    // anchor points use it), the animation scale it passes down (Widget slot 107) and the render context colour (WidgetComposite render
    // FUN_008aee70).
    struct Placed {
        Rect rect;
        int32_t w = 0, h = 0;
        float sx = 1, sy = 1;
        uint8_t colour[4] = {255, 255, 255, 255};
    };
    Placed place(const tr::XmlNode& widget, const Placed& parent) const;
    Placed placedAt(const std::vector<int32_t>& path) const;   // the widget at path (the <Widgets> root for an empty path)
    void drawWidget(const tr::XmlNode& widget, std::vector<int32_t>& path, const Placed& parent, const Rect& clip, bool branchRoot, Target& target);
    void drawTexture(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target);
    void drawColour(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target);
    void drawText(const tr::XmlNode& widget, const Rect& rect, const Rect& clip, Target& target);
    void fillBackground(Target& target) const;                      // options_.background / backgroundRgba
    void finish(const Target& target, std::vector<uint8_t>& rgba) const;
    const tr::XmlNode* sampleNode_ = nullptr;                       // renderDetached's widget, whose text is sampleText_
    std::string sampleText_;

    const tr::UiExport* ui_ = nullptr;
    bool templates_ = false;
    const tr::AssetLibrary* assets_ = nullptr;
    tr::GameData* game_ = nullptr;
    std::string gameRoot_;
    Options options_;
    // A button's state applied to its Bitmap / Text children (drawWidget): texture, text colour, text offset.
    struct ButtonOverride {
        bool texture = false, colour = false, offset = false, size = false;
        std::string textureDef;
        float rgba[4] = {1, 1, 1, 1};
        int32_t offsetX = 0, offsetY = 0;
        int32_t width = 0, height = 0;
    };
    std::map<const tr::XmlNode*, Rect> childClips_;      // a widget's clip for its children (progress bars), relative to it
    void applyProgress(const tr::XmlNode& bar);
    void applyScroll(const tr::XmlNode& bar);
    std::map<const tr::XmlNode*, ButtonOverride> buttonOverrides_;
    void applyButtonState(const tr::XmlNode& button);
    void colourOf(const tr::XmlNode& widget, float rgba[4]) const;      // the widget's colour x the context colour, 0..1
    void ownColour(const tr::XmlNode& widget, uint8_t rgba[4]) const;   // Widget slot 73: own colour x the animation's colour
    uint8_t context_[4] = {255, 255, 255, 255};                         // the render context colour of the widget being drawn
    // Animations (Definitions/Animations; tabula_rasa.exe ui\anim, decompiled 2026-10-15, see sampleAnimation).
    struct AnimKey {
        uint32_t time = 0;                  // milliseconds
        float v[4] = {0, 0, 0, 0};          // scale / trans (x, y), uv (X, Y, Z, W), rot, colour (r, g, b, a bytes)
    };
    struct AnimTrack {
        bool discrete = false;
        std::vector<AnimKey> keys;          // sorted by time
    };
    struct Animation {
        bool bitmap = false;                // animType Bitmap (else Widget)
        int playback = 0;                   // 0 stall, 1 repeat, 2 pingpong
        AnimTrack scale, trans, colour;     // Widget
        AnimTrack uv, rot;                  // Bitmap
        float anchorX = 0.5f, anchorY = 0.5f;   // rotTrack's
        uint32_t length = 0;                // the longest track's last key
    };
    struct AnimFrame {
        bool scale = false, trans = false, colour = false, uv = false, rot = false;
        float scaleX = 1, scaleY = 1, transX = 0, transY = 0;
        uint8_t rgba[4] = {255, 255, 255, 255};
        float uvRect[4] = {0, 0, 1, 1};
        float rotation = 0, anchorX = 0.5f, anchorY = 0.5f;
    };
    std::map<std::string, Animation> animations_;
    mutable bool animating_ = false;
    bool sampleAnimation(const std::string& name, bool bitmap, AnimFrame& frame) const;
    void loadAnimations();
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
