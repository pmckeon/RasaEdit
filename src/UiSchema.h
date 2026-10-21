#pragma once

#include "ui.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tr {
class AssetLibrary;
class GameData;
}

// What the game's UI loader accepts in data\xml\Tabula_Rasa_UI_EXPORT.xml, from tabula_rasa.exe (decompiled 2026-10-15; see CLAUDE.md,
// "Element and attribute reference"): the element kinds, which attributes each kind's loader reads with their types and constructor
// defaults, which children a kind may hold, and the Definitions entries that are mandatory. Used by the UI workspace to offer only
// valid edits and to refuse saving a file the game would fail to load or would read differently from what the editor shows.
namespace uischema {

enum class Type : uint8_t {
    Text,          // free string
    Int,           // istream >> int
    Byte,          // 0..255 (colour channels)
    Anchor,        // 0..8 (uianchor)
    Bool,          // "true" / "false" (istream boolalpha)
    Flag,          // an integer, 0 = off (slide, allowIME, consumeAllKeyboardEvents)
    Float,
    Enum,          // one of values
    TextureDef,    // a Definitions/Textures id
    TextureFile,   // a .dds the archives hold (maskTextureName: the loader looks the file up, "Invalid textureID for MaskTexture")
    Font,          // a Definitions/Fonts MultiFont (with or without "Fonts.")
    TextId,        // a uielement string id
    WidgetAnim,    // a Definitions/Animations id with animType Widget
    BitmapAnim,    // ... with animType Bitmap
    Effect,        // an .fx file
    Sound,         // a sound id (integer)
};

struct Attribute {
    std::string name;
    Type type = Type::Text;
    std::string defaultValue;          // the constructor's value; "" = not traced (never removed as a default)
    std::vector<std::string> values;   // Enum
    std::string group;                 // for the inspector
};

// The class an element is created as: factory tags are themselves; owned parts by tag (Filler, Frame pieces, Caret / Highlight ->
// Bitmap; ScrollUpBtn / ScrollDownBtn / Track / Thumb / Entry -> PushButton; DropDown -> WidgetComposite; Button -> ToggleButton).
// "" for a tag the game does not know.
std::string elementClass(const std::string& tag);
bool isFactoryKind(const std::string& tag);
const std::vector<std::string>& factoryKinds();
bool isComposite(const std::string& cls);               // has children at all
bool takesFreeChildren(const std::string& cls);         // WidgetComposite / Window / MovieComposite: any factory kind
const std::vector<std::string>& ownedParts(const std::string& tag);   // fixed parts of a control (Frame's pieces, a button's Bitmap + Text, ...)
const std::vector<Attribute>& attributes(const std::string& cls);
const Attribute* attribute(const std::string& cls, const std::string& name);
// Definitions entries by tag: Color, textureDef, MultiFont, FontRange, the animation tracks (uvTrack, rotTrack, textureTrack,
// scaleTrack, transTrack, colorTrack) and their frames (uvFrame, rotFrame, scaleFrame, transFrame, colorFrame); "Animation" lists its
// text children (id, playbackType, animType), which are elements, not attributes. A default "" = mandatory or not traced (always
// written).
const std::vector<Attribute>& definitionAttributes(const std::string& tag);
const Attribute* definitionAttribute(const std::string& tag, const std::string& name);
// The tracks an animation of that animType holds (Bitmap: uv, rot, texture; Widget: scale, trans, colour) and a track's frame tag.
const std::vector<std::string>& animationTracks(const std::string& animType);
std::string frameTag(const std::string& trackTag);

// "" when the value is one the game reads as intended, else why not.
std::string checkValue(const Attribute& a, const std::string& value, const tr::UiExport& ui, const tr::AssetLibrary* assets, tr::GameData* game);

struct Issue {
    bool error = true;                 // errors block saving; warnings do not
    bool templates = false;            // the path is from TemplateWidgets (else Widgets)
    std::vector<int32_t> path;         // for Definitions issues: from the section's element (empty: the section as a whole)
    std::string section;               // "Colors" / "Fonts" / "Textures" / "Animations" for Definitions issues
    std::string attribute;
    std::string message;
    bool resettable = false;           // a value of the wrong form (number, anchor, boolean, enum): removing it gives the default
};
// The types whose values have a form to check (not references or free text): an invalid one is replaced by the default.
bool isFormatType(Type t);
std::vector<Issue> validate(const tr::UiExport& ui, const tr::AssetLibrary* assets, tr::GameData* game);

}  // namespace uischema
