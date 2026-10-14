#include "ui.h"

#include <cstdlib>

namespace tr {

namespace {

int32_t intAttribute(const XmlNode& n, const char* name, int32_t fallback = 0) {
    const std::string v = n.attribute(name);
    return v.empty() ? fallback : int32_t(std::strtol(v.c_str(), nullptr, 10));
}

bool boolAttribute(const XmlNode& n, const char* name) { return n.attribute(name) == "true"; }

}  // namespace

const UiFontRange* UiFont::rangeFor(uint32_t code) const {
    for (const UiFontRange& r : ranges)
        if (code >= r.begin && code <= r.end) return &r;
    return ranges.empty() ? nullptr : &ranges.front();
}

const UiFont* UiExport::font(const std::string& fontId) const {
    auto it = fonts.find(fontId);
    if (it == fonts.end()) it = fonts.find("Fonts." + fontId);
    return it == fonts.end() ? nullptr : &it->second;
}

const UiTextureDef* UiExport::texture(const std::string& id) const {
    const auto it = textures.find(id);
    return it == textures.end() ? nullptr : &it->second;
}

UiExport readUiExport(const Bytes& xml) {
    XmlNode root = parseXml(xml);
    if (root.tag != "TabulaRasaUI") throw FormatError("UI export: root element " + root.tag + " (expected <TabulaRasaUI>)");
    UiExport ui;
    for (XmlNode& section : root.children) {
        if (section.tag == "Widgets") {
            ui.widgets = std::move(section);
            continue;
        }
        if (section.tag != "Definitions") continue;
        for (XmlNode& d : section.children) {
            if (d.tag == "LogicalSize") {
                if (const XmlNode* w = d.child("width")) ui.logicalWidth = int32_t(std::strtol(w->text.c_str(), nullptr, 10));
                if (const XmlNode* h = d.child("height")) ui.logicalHeight = int32_t(std::strtol(h->text.c_str(), nullptr, 10));
            } else if (d.tag == "Colors") {
                for (const XmlNode& c : d.children) {
                    UiColour colour;
                    colour.r = uint8_t(intAttribute(c, "R", 255));
                    colour.g = uint8_t(intAttribute(c, "G", 255));
                    colour.b = uint8_t(intAttribute(c, "B", 255));
                    colour.a = uint8_t(intAttribute(c, "A", 255));
                    ui.colours[c.attribute("id")] = colour;
                }
            } else if (d.tag == "Fonts") {
                for (const XmlNode& f : d.children) {
                    if (f.tag == "LoadedFontFile") {
                        ui.loadedFontFiles.push_back(f.text);
                        continue;
                    }
                    if (f.tag != "MultiFont") continue;
                    UiFont font;
                    font.id = f.attribute("id");
                    for (const XmlNode& r : f.children) {
                        if (r.tag != "FontRange") continue;
                        UiFontRange range;
                        range.begin = uint32_t(intAttribute(r, "RangeBegin"));
                        range.end = uint32_t(intAttribute(r, "RangeEnd"));
                        range.face = r.attribute("faceName");
                        range.pointSize = intAttribute(r, "pointSize", 10);
                        range.shadow = boolAttribute(r, "shadow");
                        range.bold = boolAttribute(r, "bold");
                        range.italic = boolAttribute(r, "italic");
                        font.ranges.push_back(range);
                    }
                    ui.fonts[font.id] = font;
                }
            } else if (d.tag == "Textures") {
                for (const XmlNode& t : d.children) {
                    if (t.tag != "textureDef") continue;
                    UiTextureDef def;
                    def.id = t.attribute("id");
                    def.name = t.attribute("name");
                    def.left = intAttribute(t, "regionL");
                    def.top = intAttribute(t, "regionT");
                    def.right = intAttribute(t, "regionR");
                    def.bottom = intAttribute(t, "regionB");
                    def.tileU = t.attribute("modeU") == "tile";
                    def.tileV = t.attribute("modeV") == "tile";
                    ui.textures[def.id] = def;
                }
            } else if (d.tag == "Animations") {
                ui.animations = std::move(d);
            } else if (d.tag == "TemplateWidgets") {
                ui.templates = std::move(d);
            }
        }
    }
    return ui;
}

}  // namespace tr
