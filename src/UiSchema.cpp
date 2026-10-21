#include "UiSchema.h"

#include "assetlibrary.h"
#include "gamedata.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

namespace uischema {

namespace {

using AttributeList = std::vector<Attribute>;

void add(AttributeList& list, const std::string& group, const std::string& name, Type type, const std::string& def,
         std::vector<std::string> values = {}) {
    Attribute a;
    a.name = name;
    a.type = type;
    a.defaultValue = def;
    a.values = std::move(values);
    a.group = group;
    list.push_back(std::move(a));
}

// Widget (constructor FUN_0089c280, loader FUN_0089c630).
AttributeList widgetAttributes() {
    AttributeList l;
    add(l, "Layout", "uiid", Type::Text, "");
    add(l, "Layout", "anchorOffsetX", Type::Int, "0");
    add(l, "Layout", "anchorOffsetY", Type::Int, "0");
    add(l, "Layout", "childAnchor", Type::Anchor, "0");
    add(l, "Layout", "parentAnchor", Type::Anchor, "0");
    add(l, "Layout", "sizeW", Type::Int, "0");
    add(l, "Layout", "sizeH", Type::Int, "0");
    add(l, "Layout", "resizeToParent", Type::Bool, "false");
    add(l, "Layout", "positionX", Type::Int, "0");
    add(l, "Layout", "positionY", Type::Int, "0");
    add(l, "Appearance", "visible", Type::Bool, "true");
    for (const char* c : {"colorR", "colorG", "colorB", "colorA"}) add(l, "Appearance", c, Type::Byte, "255");
    add(l, "Appearance", "effect", Type::Effect, "");
    add(l, "Appearance", "widgetAnimName", Type::WidgetAnim, "");
    add(l, "Appearance", "autoRestartAnims", Type::Bool, "false");
    add(l, "Behaviour", "pickable", Type::Bool, "false");
    add(l, "Behaviour", "enabled", Type::Bool, "false");
    return l;
}

AttributeList compositeAttributes() {            // WidgetComposite (FUN_008ae750, loader FUN_008ae9c0)
    AttributeList l = widgetAttributes();
    add(l, "Behaviour", "clipChildren", Type::Bool, "true");
    add(l, "Behaviour", "mouseCapture", Type::Bool, "false");
    return l;
}

AttributeList editBoxAttributes() {              // EditBox (FUN_008ce7a0, loader FUN_008cebb0); defaults not traced
    AttributeList l = compositeAttributes();
    add(l, "Text", "fontId", Type::Font, "");
    for (const char* c : {"TextColorR", "TextColorG", "TextColorB", "TextColorA", "TextColorHighlightedR", "TextColorHighlightedG",
                          "TextColorHighlightedB", "TextColorHighlightedA"})
        add(l, "Text", c, Type::Byte, "");
    add(l, "Behaviour", "InputValidator", Type::Text, "");
    add(l, "Behaviour", "allowIME", Type::Flag, "");
    add(l, "Behaviour", "consumeAllKeyboardEvents", Type::Flag, "");
    add(l, "Behaviour", "lengthLimit", Type::Int, "");
    for (const char* c : {"AddCharSound", "DelCharSound", "CursorSound"}) add(l, "Sounds", c, Type::Sound, "");
    return l;
}

AttributeList rangeAttributes(const std::string& styleDefault, std::vector<std::string> styles) {
    AttributeList l = compositeAttributes();
    add(l, "Range", "style", Type::Enum, styleDefault, std::move(styles));
    for (const char* c : {"minRange", "maxRange", "pageSize", "currentValue"}) add(l, "Range", c, Type::Int, "0");
    return l;
}

const std::map<std::string, AttributeList>& table() {
    static const std::map<std::string, AttributeList> t = [] {
        std::map<std::string, AttributeList> m;
        m["ColoredRect"] = widgetAttributes();
        m["Movie"] = widgetAttributes();
        m["WidgetComposite"] = compositeAttributes();
        m["MovieComposite"] = compositeAttributes();
        m["Frame"] = compositeAttributes();
        m["CheckBox"] = compositeAttributes();
        m["ComboBox"] = compositeAttributes();
        {
            AttributeList l = compositeAttributes();
            add(l, "Behaviour", "script", Type::Text, "");
            m["Window"] = l;
        }
        {   // UIBitmap (FUN_008f3d70, loader FUN_008f4070)
            AttributeList l = widgetAttributes();
            add(l, "Bitmap", "textureDef", Type::TextureDef, "");
            add(l, "Bitmap", "maskTextureName", Type::TextureFile, "");
            add(l, "Bitmap", "bitmapAnimName", Type::BitmapAnim, "");
            add(l, "Behaviour", "opaqueClick", Type::Bool, "false");
            m["Bitmap"] = l;
        }
        {   // UIText (FUN_008b12b0, loader FUN_008b1510)
            AttributeList l = widgetAttributes();
            add(l, "Text", "textId", Type::TextId, "");
            add(l, "Text", "fontId", Type::Font, "");
            add(l, "Text", "horizAlign", Type::Enum, "left", {"left", "center", "right"});
            add(l, "Text", "vertAlign", Type::Enum, "top", {"top", "center", "bottom"});
            add(l, "Text", "wrapText", Type::Bool, "false");
            add(l, "Text", "scaleToFit", Type::Bool, "false");
            m["Text"] = l;
        }
        {   // RichUIText (FUN_008e4990, loader FUN_008e4b90)
            AttributeList l = widgetAttributes();
            add(l, "Text", "textId", Type::TextId, "");
            add(l, "Text", "fontId", Type::Font, "");
            m["RichText"] = l;
        }
        {   // ProgressBar (FUN_008b3e50, loader FUN_008b3ee0)
            AttributeList l = compositeAttributes();
            add(l, "Progress", "orientation", Type::Enum, "leftToRight", {"leftToRight", "rightToLeft", "bottomToTop", "topToBottom"});
            add(l, "Progress", "style", Type::Enum, "stretch", {"reveal", "stretch", "slide"});
            add(l, "Progress", "percentComplete", Type::Float, "0");
            m["ProgressBar"] = l;
        }
        {   // PushButton (FUN_008e2b20, loader FUN_008e2fc0)
            AttributeList l = compositeAttributes();
            for (const char* c : {"UpTextureDef", "HighlightedTextureDef", "DownTextureDef", "DisabledTextureDef"})
                add(l, "Button", c, Type::TextureDef, "");
            for (const char* p : {"Up", "Highlight", "Down", "Disabled"}) {
                for (const char* c : {"R", "G", "B", "A"}) add(l, "Button text", std::string(p) + "TextColor" + c, Type::Byte, "255");
                for (const char* c : {"X", "Y"}) add(l, "Button text", std::string(p) + "TextAnchorOffset" + c, Type::Int, "0");
            }
            for (const char* c : {"HighlightSound", "UnHighlightSound", "PressSound", "UnPressSound"}) add(l, "Sounds", c, Type::Sound, "");
            m["PushButton"] = l;
        }
        {   // ToggleButton (FUN_008e7a10, loader FUN_008e7c30)
            AttributeList l = compositeAttributes();
            for (const char* c : {"UpTextureDef", "UpHighlightedTextureDef", "UpDisabledTextureDef", "DownTextureDef",
                                  "DownHighlightedTextureDef", "DownDisabledTextureDef", "PressedTextureDef"})
                add(l, "Button", c, Type::TextureDef, "");
            for (const char* c : {"ToggleSound", "UnToggleSound", "HighlightSound", "UnHighlightSound", "PressSound", "UnPressSound"})
                add(l, "Sounds", c, Type::Sound, "");
            m["ToggleButton"] = l;
        }
        {   // ScrollBar (FUN_008d6c00, loader FUN_008d6ce0): "horizontal" -> 0, anything else 1 (vertical, the default)
            AttributeList l = rangeAttributes("vertical", {"horizontal", "vertical"});
            add(l, "Range", "slide", Type::Flag, "0");
            m["ScrollBar"] = l;
        }
        m["Slider"] = rangeAttributes("horizontal", {"horizontal", "vertical"});   // FUN_008da590 / FUN_008da640
        {   // Spinner (FUN_008f61e0, loader FUN_008f62a0): "vertical" -> 0, else 1; default not traced
            AttributeList l = rangeAttributes("", {"horizontal", "vertical"});
            add(l, "Range", "wrapRange", Type::Flag, "");
            m["Spinner"] = l;
        }
        m["EditBox"] = editBoxAttributes();
        m["MultiLineEditBox"] = editBoxAttributes();
        {
            AttributeList l = editBoxAttributes();
            add(l, "Text", "drawableChar", Type::Text, "");
            m["PasswordBox"] = l;
        }
        {   // SceneView (FUN_008b9070, loader FUN_008b9170); defaults not traced
            AttributeList l = widgetAttributes();
            add(l, "Scene", "buffered", Type::Bool, "");
            for (const char* c : {"clearColorR", "clearColorG", "clearColorB", "clearColorA"}) add(l, "Scene", c, Type::Byte, "");
            for (const char* c : {"fieldOfView", "nearPlane", "farPlane", "cameraYaw", "cameraPitch", "cameraRoll", "cameraPositionX",
                                  "cameraPositionY", "cameraPositionZ", "cameraOrientationX", "cameraOrientationY", "cameraOrientationZ",
                                  "cameraOrientationW"})
                add(l, "Scene", c, Type::Float, "");
            m["SceneView"] = l;
        }
        return m;
    }();
    return t;
}

const std::set<std::string>& framePieces() {
    static const std::set<std::string> s{"TopLeft", "TopLeftHoriz", "TopLeftVert", "Top", "TopRight", "TopRightHoriz", "TopRightVert",
                                         "Left", "Right", "BottomLeft", "BottomLeftHoriz", "BottomLeftVert", "Bottom", "BottomRight",
                                         "BottomRightHoriz", "BottomRightVert", "Center"};
    return s;
}

bool parseInt(const std::string& v, long& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    out = std::strtol(v.c_str(), &end, 10);
    return end && *end == 0;
}

bool parseFloat(const std::string& v) {
    if (v.empty()) return false;
    char* end = nullptr;
    std::strtod(v.c_str(), &end);
    return end && *end == 0;
}

bool hasAnimation(const tr::UiExport& ui, const std::string& id, const char* type) {
    for (const tr::XmlNode& a : ui.animations.children) {
        const tr::XmlNode* i = a.child("id");
        const tr::XmlNode* t = a.child("animType");
        if (i && i->text == id && t && t->text == type) return true;
    }
    return false;
}

}  // namespace

const std::vector<std::string>& factoryKinds() {   // the factory FUN_00897260 (tag or xsi:type -> creator)
    static const std::vector<std::string> k{"Bitmap", "Text", "RichText", "ColoredRect", "WidgetComposite", "Window", "Frame", "PushButton",
                                            "ToggleButton", "CheckBox", "ComboBox", "ProgressBar", "ScrollBar", "Slider", "Spinner",
                                            "EditBox", "MultiLineEditBox", "PasswordBox", "SceneView", "Movie", "MovieComposite"};
    return k;
}

bool isFactoryKind(const std::string& tag) {
    const auto& k = factoryKinds();
    return std::find(k.begin(), k.end(), tag) != k.end();
}

std::string elementClass(const std::string& tag) {
    if (isFactoryKind(tag)) return tag;
    if (tag == "Filler" || tag == "Caret" || tag == "Highlight" || framePieces().count(tag)) return "Bitmap";
    if (tag == "ScrollUpBtn" || tag == "ScrollDownBtn" || tag == "Track" || tag == "Thumb" || tag == "Entry") return "PushButton";
    if (tag == "DropDown") return "WidgetComposite";
    if (tag == "Button") return "ToggleButton";
    return "";
}

bool isComposite(const std::string& cls) {
    return !cls.empty() && cls != "Bitmap" && cls != "Text" && cls != "RichText" && cls != "ColoredRect" && cls != "SceneView" && cls != "Movie";
}

bool takesFreeChildren(const std::string& cls) { return cls == "WidgetComposite" || cls == "Window" || cls == "MovieComposite"; }

const std::vector<std::string>& ownedParts(const std::string& cls) {
    static const std::map<std::string, std::vector<std::string>> parts = [] {
        std::map<std::string, std::vector<std::string>> m;
        m["ProgressBar"] = {"Filler"};
        m["Frame"] = {"TopLeft", "TopLeftHoriz", "TopLeftVert", "Top", "TopRight", "TopRightHoriz", "TopRightVert", "Left", "Right",
                      "BottomLeft", "BottomLeftHoriz", "BottomLeftVert", "Bottom", "BottomRight", "BottomRightHoriz", "BottomRightVert", "Center"};
        m["ScrollBar"] = {"ScrollUpBtn", "ScrollDownBtn", "Track", "Thumb"};
        m["Slider"] = {"Track", "Thumb"};
        m["EditBox"] = m["MultiLineEditBox"] = m["PasswordBox"] = {"Caret", "Highlight"};
        m["CheckBox"] = {"Button", "Text"};
        m["ComboBox"] = {"Button", "Text", "DropDown", "Entry"};
        m["PushButton"] = {"Bitmap", "Text"};
        m["ToggleButton"] = {"Bitmap"};
        return m;
    }();
    static const std::vector<std::string> none;
    const auto it = parts.find(cls);
    return it == parts.end() ? none : it->second;
}

const std::vector<Attribute>& attributes(const std::string& cls) {
    static const AttributeList none;
    const auto it = table().find(cls);
    return it == table().end() ? none : it->second;
}

const Attribute* attribute(const std::string& cls, const std::string& name) {
    for (const Attribute& a : attributes(cls))
        if (a.name == name) return &a;
    return nullptr;
}

// Definitions loaders (uimanager.cpp; CLAUDE.md "Element and attribute reference", Mandatory / Definitions defaults): colours
// FUN_008ca160 (no default traced for a channel left out of a Color definition: always written), the texture loader FUN_008df350
// (region -1 = the whole texture, modes stretch), fonts (FontRange faceName / pointSize / RangeBegin / RangeEnd mandatory; bold /
// italic / shadow / outline false), animations (AnimData / BitmapAnimData FUN_008c0040: playbackType stall, uv / rot tracks not
// discrete, texture tracks discrete, rot anchor 0.5; frames need their time; widget tracks' discrete default not traced).
const std::vector<Attribute>& definitionAttributes(const std::string& tag) {
    static const std::map<std::string, AttributeList> table = [] {
        std::map<std::string, AttributeList> t;
        AttributeList& colour = t["Color"];
        add(colour, "Colour", "id", Type::Text, "");
        for (const char* c : {"R", "G", "B", "A"}) add(colour, "Colour", c, Type::Byte, "");
        AttributeList& texture = t["textureDef"];
        add(texture, "Texture", "id", Type::Text, "");
        add(texture, "Texture", "name", Type::TextureFile, "");
        for (const char* r : {"regionL", "regionT", "regionR", "regionB"}) add(texture, "Region", r, Type::Int, "-1");
        add(texture, "Mode", "modeU", Type::Enum, "stretch", {"stretch", "tile"});
        add(texture, "Mode", "modeV", Type::Enum, "stretch", {"stretch", "tile"});
        add(t["MultiFont"], "Font", "id", Type::Text, "");
        AttributeList& range = t["FontRange"];
        add(range, "Range", "RangeBegin", Type::Int, "");
        add(range, "Range", "RangeEnd", Type::Int, "");
        add(range, "Range", "faceName", Type::Text, "");
        add(range, "Range", "pointSize", Type::Int, "");
        for (const char* b : {"bold", "italic", "shadow", "outline"}) add(range, "Style", b, Type::Bool, "false");
        AttributeList& animation = t["Animation"];
        add(animation, "Animation", "id", Type::Text, "");
        add(animation, "Animation", "playbackType", Type::Enum, "stall", {"stall", "repeat", "pingpong"});
        add(animation, "Animation", "animType", Type::Enum, "", {"Widget", "Bitmap"});
        add(t["uvTrack"], "Track", "discrete", Type::Bool, "false");
        add(t["rotTrack"], "Track", "discrete", Type::Bool, "false");
        add(t["rotTrack"], "Track", "anchorX", Type::Float, "0.5");
        add(t["rotTrack"], "Track", "anchorY", Type::Float, "0.5");
        add(t["textureTrack"], "Track", "discrete", Type::Bool, "true");
        for (const char* w : {"scaleTrack", "transTrack", "colorTrack"}) add(t[w], "Track", "discrete", Type::Bool, "");
        const auto frame = [&](const char* tag, std::initializer_list<const char*> values, Type type) {
            AttributeList& f = t[tag];
            add(f, "Frame", "time", Type::Int, "");
            for (const char* v : values) add(f, "Frame", v, type, "");
        };
        frame("uvFrame", {"X", "Y", "Z", "W"}, Type::Float);
        frame("rotFrame", {"rot"}, Type::Float);
        frame("scaleFrame", {"X", "Y"}, Type::Float);
        frame("transFrame", {"X", "Y"}, Type::Float);
        frame("colorFrame", {"R", "G", "B", "A"}, Type::Byte);
        return t;
    }();
    static const AttributeList none;
    const auto it = table.find(tag);
    return it == table.end() ? none : it->second;
}

const Attribute* definitionAttribute(const std::string& tag, const std::string& name) {
    for (const Attribute& a : definitionAttributes(tag))
        if (a.name == name) return &a;
    return nullptr;
}

const std::vector<std::string>& animationTracks(const std::string& animType) {
    static const std::vector<std::string> bitmap{"uvTrack", "rotTrack", "textureTrack"}, widget{"scaleTrack", "transTrack", "colorTrack"}, none;
    return animType == "Bitmap" ? bitmap : animType == "Widget" ? widget : none;
}

std::string frameTag(const std::string& trackTag) {
    if (trackTag == "uvTrack") return "uvFrame";
    if (trackTag == "rotTrack") return "rotFrame";
    if (trackTag == "scaleTrack") return "scaleFrame";
    if (trackTag == "transTrack") return "transFrame";
    if (trackTag == "colorTrack") return "colorFrame";
    return "";
}

std::string checkValue(const Attribute& a, const std::string& v, const tr::UiExport& ui, const tr::AssetLibrary* assets, tr::GameData* game) {
    long n = 0;
    switch (a.type) {
        case Type::Text: return "";
        case Type::Int:
        case Type::Flag:
        case Type::Sound: return parseInt(v, n) ? "" : "not a whole number";
        case Type::Byte: return parseInt(v, n) && n >= 0 && n <= 255 ? "" : "not a whole number 0 - 255";
        case Type::Anchor: return parseInt(v, n) && n >= 0 && n <= 8 ? "" : "not an anchor 0 - 8";
        case Type::Bool: return v == "true" || v == "false" ? "" : "not \"true\" or \"false\" (the game reads it with boolalpha)";
        case Type::Float: return parseFloat(v) ? "" : "not a number";
        case Type::Enum:
            return std::find(a.values.begin(), a.values.end(), v) != a.values.end() ? "" : "not one of the values the game recognises";
        case Type::TextureDef:
            if (v.empty()) return "";
            return ui.texture(v) ? "" : "no textureDef \"" + v + "\" (the game would draw DEFAULT / fail the mask)";
        case Type::TextureFile:
            if (v.empty() || !assets) return "";
            return assets->exists(v) ? "" : "no texture file \"" + v + "\" in the game's archives";
        case Type::Font:
            if (v.empty()) return "";
            return ui.font(v) ? "" : "no font \"" + v + "\" (the text would not draw)";
        case Type::TextId:
            if (v.empty() || !game) return "";
            return game->uiTextExists(v) ? "" : "no text with id \"" + v + "\" in uielement / uielementlanguage";
        case Type::WidgetAnim:
            if (v.empty()) return "";
            return hasAnimation(ui, v, "Widget") ? "" : "no Widget animation \"" + v + "\"";
        case Type::BitmapAnim:
            if (v.empty()) return "";
            return hasAnimation(ui, v, "Bitmap") ? "" : "no Bitmap animation \"" + v + "\"";
        case Type::Effect: {
            static const std::set<std::string> known{"PalUIDefault.fx", "PalUIColoredRect.fx", "PalUIMultiTex.fx", "PalUIMultiTexAdd.fx",
                                                     "TRUIAdditiveAlpha.fx", "TRUIBackgroundMask.fx", "TRUIRadarMask.fx"};
            return v.empty() || known.count(v) ? "" : "not one of the UI effects (" + v + ")";
        }
    }
    return "";
}

namespace {

}  // namespace

bool isFormatType(Type t) {
    return t == Type::Int || t == Type::Byte || t == Type::Anchor || t == Type::Bool || t == Type::Flag || t == Type::Float || t == Type::Enum ||
           t == Type::Sound;
}

namespace {

bool isWarningType(Type t) { return t == Type::TextId || t == Type::Effect; }

void validateElement(const tr::XmlNode& n, const std::string& parentClass, bool parentIsRoot, bool templates, std::vector<int32_t>& path,
                     const tr::UiExport& ui, const tr::AssetLibrary* assets, tr::GameData* game, std::vector<Issue>& out) {
    const std::string cls = elementClass(n.tag);
    auto issue = [&](bool error, const std::string& attribute, const std::string& message) {
        Issue i;
        i.error = error;
        i.templates = templates;
        i.path = path;
        i.attribute = attribute;
        i.message = message;
        out.push_back(std::move(i));
    };
    if (parentIsRoot || takesFreeChildren(parentClass)) {
        if (!isFactoryKind(n.tag)) issue(true, "", "<" + n.tag + "> is not an element kind the game creates here (it is skipped)");
    } else {
        const auto& parts = ownedParts(parentClass);
        if (std::find(parts.begin(), parts.end(), n.tag) == parts.end())
            issue(true, "", "a " + parentClass + " has no part <" + n.tag + "> (its parts: " + [&] {
                std::string s;
                for (const auto& p : parts) s += (s.empty() ? "" : ", ") + p;
                return s.empty() ? std::string("none") : s;
            }() + ")");
    }
    if (cls.empty()) return;
    for (const auto& a : n.attributes) {
        const Attribute* spec = attribute(cls, a.first);
        if (!spec) {
            issue(false, a.first, "the game does not read " + a.first + " on a " + cls);
            continue;
        }
        const std::string problem = checkValue(*spec, a.second, ui, assets, game);
        if (!problem.empty()) {
            issue(!isWarningType(spec->type), a.first, a.first + "=\"" + a.second + "\": " + problem);
            out.back().resettable = isFormatType(spec->type);
        }
    }
    if (!n.children.empty() && !isComposite(cls)) issue(true, "", "a " + cls + " cannot hold children (the game ignores them)");
    std::map<std::string, int> partCount;
    for (size_t k = 0; k < n.children.size(); ++k) {
        path.push_back(int32_t(k));
        if (!takesFreeChildren(cls) && ++partCount[n.children[k].tag] == 2)
            issue(true, "", "a second <" + n.children[k].tag + "> part (the loader configures one)");
        validateElement(n.children[k], cls, false, templates, path, ui, assets, game, out);
        path.pop_back();
    }
}

}  // namespace

std::vector<Issue> validate(const tr::UiExport& ui, const tr::AssetLibrary* assets, tr::GameData* game) {
    std::vector<Issue> out;
    std::vector<int32_t> path;
    for (size_t k = 0; k < ui.widgets.children.size(); ++k) {
        path.assign(1, int32_t(k));
        validateElement(ui.widgets.children[k], "", true, false, path, ui, assets, game, out);
    }
    std::set<std::string> templateIds;
    for (size_t k = 0; k < ui.templates.children.size(); ++k) {
        path.assign(1, int32_t(k));
        const std::string id = ui.templates.children[k].attribute("uiid");
        if (id.empty() || !templateIds.insert(id).second) {
            Issue i;
            i.templates = true;
            i.path = path;
            i.attribute = "uiid";
            i.message = id.empty() ? "a template needs a uiid (it is registered by it; without one the game skips it)"
                                   : "a second template \"" + id + "\" (the game keeps the last)";
            out.push_back(i);
        }
        validateElement(ui.templates.children[k], "", true, true, path, ui, assets, game, out);
    }
    // Definitions (uimanager.cpp: a failing section aborts loading the whole UI).
    std::vector<int32_t> at;          // the entry being checked, from its section's element
    auto definition = [&](const std::string& section, const std::string& attribute, const std::string& message, bool error = true) {
        Issue i;
        i.error = error;
        i.section = section;
        i.path = at;
        i.attribute = attribute;
        i.message = message;
        out.push_back(i);
    };
    long n = 0;
    for (size_t k = 0; k < ui.colourDefs.children.size(); ++k) {
        const tr::XmlNode& c = ui.colourDefs.children[k];
        at.assign(1, int32_t(k));
        const std::string id = c.attribute("id");
        if (id.empty()) definition("Colors", "id", "a Color without id (\"missing color 'ID' data\": the UI fails to load)");
        bool any = false;
        for (const char* ch : {"R", "G", "B", "A"})
            if (c.hasAttribute(ch)) {
                any = true;
                if (!parseInt(c.attribute(ch), n) || n < 0 || n > 255) definition("Colors", ch, "Color " + id + ": " + ch + " is not 0 - 255");
            }
        if (!any && c.children.empty()) definition("Colors", "", "Color " + id + " has no R / G / B / A (the UI fails to load)");
    }
    for (size_t k = 0; k < ui.fontDefs.children.size(); ++k) {
        const tr::XmlNode& f = ui.fontDefs.children[k];
        if (f.tag != "MultiFont") continue;
        at.assign(1, int32_t(k));
        const std::string id = f.attribute("id");
        if (id.empty()) definition("Fonts", "id", "a MultiFont without id");
        for (size_t j = 0; j < f.children.size(); ++j) {
            const tr::XmlNode& r = f.children[j];
            at = {int32_t(k), int32_t(j)};
            for (const char* key : {"faceName", "pointSize", "RangeBegin", "RangeEnd"}) {
                if (!r.hasAttribute(key)) definition("Fonts", key, "font " + id + ": a FontRange without " + key + " (\"invalid XML font ...\")");
                else if (std::string(key) != "faceName" && !parseInt(r.attribute(key), n))
                    definition("Fonts", key, "font " + id + ": " + key + " is not a whole number");
            }
        }
    }
    std::set<std::string> textureIds;
    for (size_t k = 0; k < ui.textureDefs.children.size(); ++k) {
        const tr::XmlNode& t = ui.textureDefs.children[k];
        at.assign(1, int32_t(k));
        const std::string id = t.attribute("id");
        if (id.empty()) definition("Textures", "id", "a textureDef without id (the texture load fails and the UI with it)");
        else if (!textureIds.insert(id).second) definition("Textures", "id", "a second textureDef " + id + " (the game renames it " + id + ".RenameMe)");
        if (t.attribute("name").empty()) definition("Textures", "name", "textureDef " + id + " has no name (file): the UI fails to load");
        for (const char* key : {"regionL", "regionT", "regionR", "regionB"})
            if (t.hasAttribute(key) && !parseInt(t.attribute(key), n)) definition("Textures", key, "textureDef " + id + ": " + key + " is not a whole number");
        for (const char* key : {"modeU", "modeV"})
            if (t.hasAttribute(key) && t.attribute(key) != "tile" && t.attribute(key) != "stretch")
                definition("Textures", key, "textureDef " + id + ": " + key + " is neither tile nor stretch (read as stretch)", false);
    }
    std::set<std::string> animationIds;
    for (size_t k = 0; k < ui.animations.children.size(); ++k) {
        const tr::XmlNode& a = ui.animations.children[k];
        at.assign(1, int32_t(k));
        const tr::XmlNode* id = a.child("id");
        const std::string name = id ? id->text : "";
        if (name.empty()) definition("Animations", "id", "an Animation without id (\"AnimData loaded but doesn't have a valid name!\": the UI fails to load)");
        else if (!animationIds.insert(name).second) definition("Animations", "id", "a second Animation " + name + " (the UI fails to load)");
        const tr::XmlNode* type = a.child("animType");
        if (!type || (type->text != "Widget" && type->text != "Bitmap"))
            definition("Animations", "animType", "Animation " + name + ": animType must be Widget or Bitmap (the UI fails to load)");
        const tr::XmlNode* playback = a.child("playbackType");
        if (playback && playback->text.empty()) definition("Animations", "playbackType", "Animation " + name + ": empty playbackType (the UI fails to load)");
        for (const tr::XmlNode& track : a.children)
            for (const tr::XmlNode& frame : track.children)
                if (!frame.hasAttribute("time") && !frame.child("time"))
                    definition("Animations", "time", "Animation " + name + ": a " + frame.tag + " without time (the UI fails to load)");
    }
    return out;
}

}  // namespace uischema
