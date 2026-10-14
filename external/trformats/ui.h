// ui.h - the game's UI layout export, data\xml\Tabula_Rasa_UI_EXPORT.xml (a loose file; 34,123 lines in client 1.16.5.0).
//
// <TabulaRasaUI><Definitions>…</Definitions><Widgets>…</Widgets></TabulaRasaUI> (verified 2026-10-15 on the shipped file):
//   Definitions/LogicalSize   <width>1280</width><height>960</height>: the coordinate space of every widget
//   Definitions/Colors        <Color id R G B A/>                                      (159)
//   Definitions/Fonts         <LoadedFontFile>f1.lff</LoadedFontFile> (TrueType files in data\misc: f1 = "Conduit ITC Light",
//                             f2 = "EurostileTRegExt"); <MultiFont id="Fonts.<name>"> of <FontRange RangeBegin RangeEnd
//                             faceName pointSize [shadow bold italic]/>                   (56)
//   Definitions/Textures      <textureDef id name regionL regionT regionR regionB modeU modeV/> - a pixel region of a .dds,
//                             stretched or tiled ("stretch" / "tile")                      (1,512)
//   Definitions/Animations    <Animation><id/><playbackType/><animType/> + uv / colour tracks  (146; not interpreted here)
//   Definitions/TemplateWidgets   widget trees the scripts instantiate at runtime          (273)
//   Widgets                   the screen's widget tree: Window / WidgetComposite / Bitmap / Text / PushButton / Frame …
//                             (20,985 elements), kept as XML nodes - the widget kinds are interpreted by the editor.
#pragma once

#include "trformats.h"
#include "xml.h"

#include <map>
#include <string>
#include <vector>

namespace tr {

struct UiColour {
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

struct UiFontRange {
    uint32_t begin = 0, end = 0;       // character codes RangeBegin..RangeEnd inclusive
    std::string face;                  // faceName: a TrueType family name
    int32_t pointSize = 10;
    bool shadow = false;
    bool bold = false;
    bool italic = false;
};

struct UiFont {
    std::string id;                    // as in the file, e.g. "Fonts.A_Conduit_10"
    std::vector<UiFontRange> ranges;
    const UiFontRange* rangeFor(uint32_t code) const;     // the first range containing the code, else the first range
};

struct UiTextureDef {
    std::string id;
    std::string name;                  // the .dds
    int32_t left = 0, top = 0, right = 0, bottom = 0;     // pixel region of the texture
    bool tileU = false, tileV = false; // modeU / modeV "tile" (else "stretch")
};

struct UiExport {
    int32_t logicalWidth = 1280, logicalHeight = 960;
    std::map<std::string, UiColour> colours;
    std::vector<std::string> loadedFontFiles;
    std::map<std::string, UiFont> fonts;
    std::map<std::string, UiTextureDef> textures;
    XmlNode templates;                 // Definitions/TemplateWidgets
    XmlNode animations;                // Definitions/Animations
    XmlNode widgets;                   // Widgets

    const UiFont* font(const std::string& fontId) const;            // "A_Conduit_10" or "Fonts.A_Conduit_10"
    const UiTextureDef* texture(const std::string& id) const;
};

UiExport readUiExport(const Bytes& xml);

}  // namespace tr
