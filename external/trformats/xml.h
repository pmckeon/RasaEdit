// xml.h - a minimal XML reader for the game's XML data (sky / light property files, the UI export).
//
// Elements, attributes, self-closing tags, comments, processing instructions and the five predefined entities. No DTDs,
// CDATA or namespaces (an xmlns:… attribute is kept as an ordinary attribute). An element's text is the concatenation of
// its character data, entity-decoded, leading and trailing whitespace removed (e.g. <id>10Frame_Vertical</id>).
#pragma once

#include "trformats.h"

#include <string>
#include <utility>
#include <vector>

namespace tr {

struct XmlNode {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attributes;     // in file order
    std::vector<XmlNode> children;
    std::string text;
    // Where the element sits in the parsed source (byte offsets): its '<', just past its start tag ('>' or "/>"), just past its end
    // tag (= sourceTagEnd when self-closing). Lets an editor splice the source text and leave everything else byte-identical.
    size_t sourceStart = 0, sourceTagEnd = 0, sourceEnd = 0;
    bool selfClosing = false;
    std::string attribute(const std::string& name) const;            // "" if absent
    bool hasAttribute(const std::string& name) const;
    const XmlNode* child(const std::string& name) const;              // the first child with that tag
};

// The document's root element; throws FormatError on malformed input or content after the root.
XmlNode parseXml(const Bytes& data);

}  // namespace tr
