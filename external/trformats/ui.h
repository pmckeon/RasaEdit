// ui.h - the game's UI layout export, data\xml\Tabula_Rasa_UI_EXPORT.xml (a loose file; 34,123 lines in client 1.16.5.0).
//
// <TabulaRasaUI><Definitions>…</Definitions><Widgets>…</Widgets></TabulaRasaUI> (verified 2026-10-15 on the shipped file):
//   Definitions/LogicalSize   <width>1280</width><height>960</height> (not read by the game, whose root is 1280 x 960 / 1706 x 960)
//   Definitions/Colors        <Color id R G B A/>                                      (159)
//   Definitions/Fonts         <LoadedFontFile>f1.lff</LoadedFontFile> (TrueType files in data\misc: f1 = "Conduit ITC Light",
//                             f2 = "EurostileTRegExt"); <MultiFont id="Fonts.<name>"> of <FontRange RangeBegin RangeEnd
//                             faceName pointSize [shadow bold italic]/>                   (56)
//   Definitions/Textures      <textureDef id name regionL regionT regionR regionB modeU modeV/> - a pixel region of a .dds
//                             (-1 or absent: the whole texture), stretched or tiled ("stretch" / "tile")  (1,512)
//   Definitions/Animations    <Animation><id/><playbackType/><animType/> + uv / colour tracks  (146; not interpreted here)
//   Definitions/TemplateWidgets   widget trees the scripts instantiate at runtime          (273)
//   Widgets                   the screen's widget tree: Window / WidgetComposite / Bitmap / Text / PushButton / Frame …
//                             (20,985 elements), kept as XML nodes - the widget kinds are interpreted by the editor.
#pragma once

#include "trformats.h"
#include "xml.h"

#include <map>
#include <memory>
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
    int32_t left = -1, top = -1, right = -1, bottom = -1; // pixel region of the texture (the game's default -1: the whole texture)
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
    XmlNode colourDefs, fontDefs, textureDefs;   // Definitions/Colors, Fonts, Textures as written (for validation)

    const UiFont* font(const std::string& fontId) const;            // "A_Conduit_10" or "Fonts.A_Conduit_10"
    const UiTextureDef* texture(const std::string& id) const;
    void indexDefinitions();           // colours, loadedFontFiles, fonts, textures again from colourDefs / fontDefs / textureDefs
};

// The trees a UiDocument path starts from: <Widgets>, or Definitions/TemplateWidgets, Colors, Fonts, Textures, Animations.
enum class UiSection : uint8_t { Widgets, Templates, Colours, Fonts, Textures, Animations };

UiExport readUiExport(const Bytes& xml);

// The export open for editing: its source text and the parsed export, kept in step. Every edit is one splice of the text (all other
// bytes stay as they were - formatting, attribute order, line ends), after which the text is parsed again; each splice is recorded for
// undo / redo. Paths are child indices from the section's element (<Widgets>, Definitions/TemplateWidgets, Colors, ...; the bool forms:
// templates or Widgets). Edits throw FormatError on an invalid path.
class UiDocument {
public:
    explicit UiDocument(const Bytes& source);
    const std::string& text() const { return text_; }
    const UiExport& ui() const { return *ui_; }
    const XmlNode* node(UiSection section, const std::vector<int32_t>& path) const;

    // An attribute's value as written (escaped on writing). A new attribute goes where the export's own attribute order puts it.
    void setAttribute(UiSection section, const std::vector<int32_t>& path, const std::string& name, const std::string& value);
    void removeAttribute(UiSection section, const std::vector<int32_t>& path, const std::string& name);
    // Several at once, as one edit (one undo step): set these (name, value), remove those names.
    void setAttributes(UiSection section, const std::vector<int32_t>& path, const std::vector<std::pair<std::string, std::string>>& set,
                       const std::vector<std::string>& remove);
    // A new element (its attributes and children) as the index-th child of parent; returns its path.
    std::vector<int32_t> insertElement(UiSection section, const std::vector<int32_t>& parent, size_t index, const XmlNode& element);
    std::vector<int32_t> duplicateElement(UiSection section, const std::vector<int32_t>& path);    // the copy follows the original
    void removeElement(UiSection section, const std::vector<int32_t>& path);
    std::vector<int32_t> moveElement(UiSection section, const std::vector<int32_t>& path, int delta);  // swap with a sibling; new path
    // Moves an element (and everything in it) to be the index-th child of another parent (one undo step); returns its new path.
    // Throws when the parent is the element itself or inside it.
    std::vector<int32_t> moveElementTo(UiSection section, const std::vector<int32_t>& path, const std::vector<int32_t>& parent, size_t index);
    // An element as XML text (for the clipboard), and a parse of such text back to an element.
    std::string elementText(UiSection section, const std::vector<int32_t>& path) const;
    static XmlNode parseElement(const std::string& text);
    // The text of an element without children (<id>Name</id>; a self-closing one gains an end tag), escaped on writing.
    void setText(UiSection section, const std::vector<int32_t>& path, const std::string& text);
    const XmlNode* root(UiSection section) const;      // the section's element (empty when the file has none)
    // The same on the widget trees: templates = Definitions/TemplateWidgets, else <Widgets>.
    static UiSection treeOf(bool templates) { return templates ? UiSection::Templates : UiSection::Widgets; }
    const XmlNode* node(bool templates, const std::vector<int32_t>& path) const { return node(treeOf(templates), path); }
    void setAttribute(bool templates, const std::vector<int32_t>& path, const std::string& name, const std::string& value) {
        setAttribute(treeOf(templates), path, name, value);
    }
    void removeAttribute(bool templates, const std::vector<int32_t>& path, const std::string& name) { removeAttribute(treeOf(templates), path, name); }
    void setAttributes(bool templates, const std::vector<int32_t>& path, const std::vector<std::pair<std::string, std::string>>& set,
                       const std::vector<std::string>& remove) {
        setAttributes(treeOf(templates), path, set, remove);
    }
    std::vector<int32_t> insertElement(bool templates, const std::vector<int32_t>& parent, size_t index, const XmlNode& element) {
        return insertElement(treeOf(templates), parent, index, element);
    }
    std::vector<int32_t> duplicateElement(bool templates, const std::vector<int32_t>& path) { return duplicateElement(treeOf(templates), path); }
    void removeElement(bool templates, const std::vector<int32_t>& path) { removeElement(treeOf(templates), path); }
    std::vector<int32_t> moveElement(bool templates, const std::vector<int32_t>& path, int delta) { return moveElement(treeOf(templates), path, delta); }
    std::vector<int32_t> moveElementTo(bool templates, const std::vector<int32_t>& path, const std::vector<int32_t>& parent, size_t index) {
        return moveElementTo(treeOf(templates), path, parent, index);
    }
    std::string elementText(bool templates, const std::vector<int32_t>& path) const { return elementText(treeOf(templates), path); }
    // Edits between beginGroup and endGroup are one undo step (nesting allowed).
    void beginGroup();
    void endGroup();

    bool canUndo() const { return undoCount_ > 0; }
    bool canRedo() const { return undoCount_ < history_.size(); }
    void undo();
    void redo();
    void dropRedo() { history_.resize(undoCount_); }   // after undoing a refused edit: it cannot be redone
    size_t revision() const { return revision_; }     // changes with every edit, undo and redo
    bool modified() const { return text_ != savedText_; }
    void markSaved() { savedText_ = text_; }

    static std::string escape(const std::string& value);       // & < > " as entities
    static std::string serialize(const XmlNode& element, const std::string& indent, const std::string& unit, const std::string& newline);

private:
    struct Splice {
        size_t offset = 0;
        std::string removed, inserted;
        size_t group = 0;                               // splices of one group are undone / redone together
    };
    void splice(size_t offset, size_t length, const std::string& inserted);
    void apply(size_t offset, size_t length, const std::string& inserted);     // text + tree, re-parsing the innermost element touched
    void reparse();
    size_t lineStart(size_t pos) const;                 // start of pos's line when only whitespace precedes pos on it, else pos
    size_t lineAfter(size_t pos) const;                 // just past the line end following pos when only whitespace follows, else pos
    std::string indentOf(const XmlNode& n) const;
    std::string startTag(const XmlNode& n, const std::vector<std::pair<std::string, std::string>>& attributes) const;
    const XmlNode& parentOf(UiSection section, const std::vector<int32_t>& path) const;

    std::string text_, savedText_, newline_ = "\n", indentUnit_ = "    ";
    std::unique_ptr<UiExport> ui_;
    // How often attribute a is written before attribute b (votes_[a * n + b]), over every element: the export's order is not one
    // sequence for all kinds, so a new attribute goes before the first present one that it precedes more often than not.
    std::map<std::string, size_t> attributeIndex_;
    std::vector<uint32_t> votes_;
    std::vector<Splice> history_;
    size_t undoCount_ = 0, revision_ = 0;
    size_t groupDepth_ = 0, currentGroup_ = 0, nextGroup_ = 1;
};

}  // namespace tr
