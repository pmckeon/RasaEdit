#include "ui.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <set>

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
                ui.colourDefs = std::move(d);
            } else if (d.tag == "Fonts") {
                ui.fontDefs = std::move(d);
            } else if (d.tag == "Textures") {
                ui.textureDefs = std::move(d);
            } else if (d.tag == "Animations") {
                ui.animations = std::move(d);
            } else if (d.tag == "TemplateWidgets") {
                ui.templates = std::move(d);
            }
        }
    }
    ui.indexDefinitions();
    return ui;
}

void UiExport::indexDefinitions() {
    colours.clear();
    loadedFontFiles.clear();
    fonts.clear();
    textures.clear();
    for (const XmlNode& c : colourDefs.children) {
        UiColour colour;
        colour.r = uint8_t(intAttribute(c, "R", 255));
        colour.g = uint8_t(intAttribute(c, "G", 255));
        colour.b = uint8_t(intAttribute(c, "B", 255));
        colour.a = uint8_t(intAttribute(c, "A", 255));
        colours[c.attribute("id")] = colour;
    }
    for (const XmlNode& f : fontDefs.children) {
        if (f.tag == "LoadedFontFile") {
            loadedFontFiles.push_back(f.text);
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
        fonts[font.id] = font;
    }
    for (const XmlNode& t : textureDefs.children) {
        if (t.tag != "textureDef") continue;
        UiTextureDef def;
        def.id = t.attribute("id");
        def.name = t.attribute("name");
        def.left = intAttribute(t, "regionL", -1);
        def.top = intAttribute(t, "regionT", -1);
        def.right = intAttribute(t, "regionR", -1);
        def.bottom = intAttribute(t, "regionB", -1);
        def.tileU = t.attribute("modeU") == "tile";
        def.tileV = t.attribute("modeV") == "tile";
        textures[def.id] = def;
    }
}

}  // namespace tr

namespace tr {

UiDocument::UiDocument(const Bytes& source) : text_(source.begin(), source.end()) {
    savedText_ = text_;
    if (text_.find("\r\n") != std::string::npos) newline_ = "\r\n";
    reparse();
    std::vector<std::vector<size_t>> sequences;
    std::function<void(const XmlNode&)> visit = [&](const XmlNode& n) {
        std::vector<size_t> seq;
        for (const auto& attribute : n.attributes) seq.push_back(attributeIndex_.emplace(attribute.first, attributeIndex_.size()).first->second);
        if (seq.size() > 1) sequences.push_back(std::move(seq));
        for (const XmlNode& c : n.children) visit(c);
    };
    for (const XmlNode* tree : {&ui_->widgets, &ui_->templates, &ui_->colourDefs, &ui_->fontDefs, &ui_->textureDefs, &ui_->animations}) visit(*tree);
    const size_t count = attributeIndex_.size();
    votes_.assign(count * count, 0);
    for (const auto& seq : sequences)
        for (size_t i = 0; i < seq.size(); ++i)
            for (size_t j = i + 1; j < seq.size(); ++j) ++votes_[seq[i] * count + seq[j]];
}

void UiDocument::reparse() { ui_ = std::make_unique<UiExport>(readUiExport(Bytes(text_.begin(), text_.end()))); }

const XmlNode* UiDocument::root(UiSection section) const {
    switch (section) {
        case UiSection::Templates: return &ui_->templates;
        case UiSection::Colours: return &ui_->colourDefs;
        case UiSection::Fonts: return &ui_->fontDefs;
        case UiSection::Textures: return &ui_->textureDefs;
        case UiSection::Animations: return &ui_->animations;
        default: return &ui_->widgets;
    }
}

const XmlNode* UiDocument::node(UiSection section, const std::vector<int32_t>& path) const {
    const XmlNode* n = root(section);
    if (n->sourceEnd == 0) return nullptr;                  // the file has no such section
    for (int32_t i : path) {
        if (i < 0 || size_t(i) >= n->children.size()) return nullptr;
        n = &n->children[size_t(i)];
    }
    return n;
}

const XmlNode& UiDocument::parentOf(UiSection section, const std::vector<int32_t>& path) const {
    if (path.empty()) throw FormatError("UI document: the root has no parent");
    const XmlNode* p = node(section, std::vector<int32_t>(path.begin(), path.end() - 1));
    if (!p || path.back() < 0 || size_t(path.back()) >= p->children.size()) throw FormatError("UI document: invalid path");
    return *p;
}

size_t UiDocument::lineStart(size_t pos) const {
    size_t i = pos;
    while (i > 0 && (text_[i - 1] == ' ' || text_[i - 1] == '\t')) --i;
    return i == 0 || text_[i - 1] == '\n' ? i : pos;
}

size_t UiDocument::lineAfter(size_t pos) const {
    size_t i = pos;
    while (i < text_.size() && (text_[i] == ' ' || text_[i] == '\t')) ++i;
    if (i < text_.size() && text_[i] == '\r') ++i;
    if (i < text_.size() && text_[i] == '\n') return i + 1;
    return i == text_.size() ? i : pos;
}

std::string UiDocument::indentOf(const XmlNode& n) const {
    const size_t a = lineStart(n.sourceStart);
    return text_.substr(a, n.sourceStart - a);
}

std::string UiDocument::escape(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

std::string UiDocument::startTag(const XmlNode& n, const std::vector<std::pair<std::string, std::string>>& attributes) const {
    std::string s = "<" + n.tag;
    for (const auto& a : attributes) s += " " + a.first + "=\"" + escape(a.second) + "\"";
    return s + (n.selfClosing ? " />" : ">");
}

std::string UiDocument::serialize(const XmlNode& e, const std::string& indent, const std::string& unit, const std::string& newline) {
    std::string s = indent + "<" + e.tag;
    for (const auto& a : e.attributes) s += " " + a.first + "=\"" + escape(a.second) + "\"";
    if (e.children.empty() && e.text.empty()) return s + " />" + newline;
    if (e.children.empty()) return s + ">" + escape(e.text) + "</" + e.tag + ">" + newline;
    s += ">" + newline;
    for (const XmlNode& c : e.children) s += serialize(c, indent + unit, unit, newline);
    return s + indent + "</" + e.tag + ">" + newline;
}

void UiDocument::apply(size_t offset, size_t length, const std::string& inserted) {
    // The innermost element of the trees that holds the whole replaced range.
    XmlNode* target = nullptr;
    bool definitions = false;
    std::function<void(XmlNode&)> descend = [&](XmlNode& n) {
        target = &n;
        for (XmlNode& c : n.children)
            if (c.sourceStart <= offset && offset + length <= c.sourceEnd) {
                descend(c);
                return;
            }
    };
    for (XmlNode* root : {&ui_->widgets, &ui_->templates, &ui_->colourDefs, &ui_->fontDefs, &ui_->textureDefs, &ui_->animations})
        if (!target && root->sourceEnd > 0 && root->sourceStart <= offset && offset + length <= root->sourceEnd) {
            descend(*root);
            definitions = root == &ui_->colourDefs || root == &ui_->fontDefs || root == &ui_->textureDefs;
        }
    const std::string removed = text_.substr(offset, length);
    text_.replace(offset, length, inserted);
    if (!target) {
        try {
            reparse();
        } catch (...) {
            text_.replace(offset, inserted.size(), removed);
            reparse();
            throw;
        }
        return;
    }
    const size_t start = target->sourceStart, oldEnd = target->sourceEnd;
    const int64_t delta = int64_t(inserted.size()) - int64_t(length);
    XmlNode fresh;
    try {
        fresh = parseXml(Bytes(text_.begin() + int64_t(start), text_.begin() + int64_t(oldEnd) + delta));
    } catch (...) {
        text_.replace(offset, inserted.size(), removed);     // never keep a text that does not parse
        throw;
    }
    std::function<void(XmlNode&)> place = [&](XmlNode& n) {
        n.sourceStart += start;
        n.sourceTagEnd += start;
        n.sourceEnd += start;
        for (XmlNode& c : n.children) place(c);
    };
    place(fresh);
    std::function<void(XmlNode&)> shift = [&](XmlNode& n) {
        for (size_t* f : {&n.sourceStart, &n.sourceTagEnd, &n.sourceEnd})
            if (*f >= oldEnd) *f = size_t(int64_t(*f) + delta);
        for (XmlNode& c : n.children) shift(c);
    };
    for (XmlNode* tree : {&ui_->widgets, &ui_->templates, &ui_->animations, &ui_->colourDefs, &ui_->fontDefs, &ui_->textureDefs}) shift(*tree);
    *target = std::move(fresh);
    if (definitions) ui_->indexDefinitions();
}

void UiDocument::splice(size_t offset, size_t length, const std::string& inserted) {
    Splice h;
    h.offset = offset;
    h.removed = text_.substr(offset, length);
    h.inserted = inserted;
    apply(offset, length, inserted);
    h.group = groupDepth_ > 0 ? currentGroup_ : nextGroup_++;
    history_.resize(undoCount_);
    history_.push_back(std::move(h));
    undoCount_ = history_.size();
    ++revision_;
}

void UiDocument::undo() {
    if (!canUndo()) return;
    const size_t group = history_[undoCount_ - 1].group;
    while (undoCount_ > 0 && history_[undoCount_ - 1].group == group) {
        const Splice& h = history_[--undoCount_];
        apply(h.offset, h.inserted.size(), h.removed);
    }
    ++revision_;
}

void UiDocument::redo() {
    if (!canRedo()) return;
    const size_t group = history_[undoCount_].group;
    while (undoCount_ < history_.size() && history_[undoCount_].group == group) {
        const Splice& h = history_[undoCount_++];
        apply(h.offset, h.removed.size(), h.inserted);
    }
    ++revision_;
}

void UiDocument::beginGroup() {
    if (groupDepth_++ == 0) currentGroup_ = nextGroup_++;
}

void UiDocument::endGroup() {
    if (groupDepth_ > 0) --groupDepth_;
}

std::string UiDocument::elementText(UiSection section, const std::vector<int32_t>& path) const {
    const XmlNode* n = node(section, path);
    if (!n || path.empty()) throw FormatError("UI document: invalid path");
    return serialize(*n, "", indentUnit_, newline_);
}

void UiDocument::setText(UiSection section, const std::vector<int32_t>& path, const std::string& value) {
    const XmlNode* n = node(section, path);
    if (!n || path.empty()) throw FormatError("UI document: invalid path");
    if (!n->children.empty()) throw FormatError("UI document: <" + n->tag + "> holds elements, not text");
    if (n->selfClosing) {
        if (value.empty()) return;
        size_t from = n->sourceTagEnd - 2;
        if (from > n->sourceStart && text_[from - 1] == ' ') --from;
        splice(from, n->sourceTagEnd - from, ">" + escape(value) + "</" + n->tag + ">");
        return;
    }
    const size_t endTag = text_.rfind("</", n->sourceEnd - 1);
    if (endTag == std::string::npos || endTag < n->sourceTagEnd) throw FormatError("UI document: no end tag");
    const std::string escaped = escape(value);
    if (text_.compare(n->sourceTagEnd, endTag - n->sourceTagEnd, escaped) == 0) return;
    splice(n->sourceTagEnd, endTag - n->sourceTagEnd, escaped);
}

XmlNode UiDocument::parseElement(const std::string& text) {
    XmlNode n = parseXml(Bytes(text.begin(), text.end()));
    std::function<void(XmlNode&)> clear = [&](XmlNode& e) {      // offsets of the clipboard text mean nothing in the document
        e.sourceStart = e.sourceTagEnd = e.sourceEnd = 0;
        for (XmlNode& c : e.children) clear(c);
    };
    clear(n);
    return n;
}

std::vector<int32_t> UiDocument::moveElementTo(UiSection section, const std::vector<int32_t>& path, const std::vector<int32_t>& parent, size_t index) {
    const XmlNode* n = node(section, path);
    if (!n || path.empty() || !node(section, parent)) throw FormatError("UI document: invalid path");
    if (parent.size() >= path.size() && std::equal(path.begin(), path.end(), parent.begin()))
        throw FormatError("UI document: an element cannot go inside itself");
    const XmlNode copy = *n;
    // Where the parent and index are once the element has left its place.
    std::vector<int32_t> target = parent;
    const std::vector<int32_t> oldParent(path.begin(), path.end() - 1);
    const size_t depth = oldParent.size();
    if (target.size() > depth && std::equal(oldParent.begin(), oldParent.end(), target.begin()) && target[depth] > path.back()) --target[depth];
    if (target == oldParent && index > size_t(path.back())) --index;
    beginGroup();
    try {
        removeElement(section, path);
        const std::vector<int32_t> moved = insertElement(section, target, index, copy);
        endGroup();
        return moved;
    } catch (...) {
        endGroup();
        throw;
    }
}

void UiDocument::setAttribute(UiSection section, const std::vector<int32_t>& path, const std::string& name, const std::string& value) {
    setAttributes(section, path, {{name, value}}, {});
}

void UiDocument::removeAttribute(UiSection section, const std::vector<int32_t>& path, const std::string& name) {
    setAttributes(section, path, {}, {name});
}

void UiDocument::setAttributes(UiSection section, const std::vector<int32_t>& path, const std::vector<std::pair<std::string, std::string>>& set,
                               const std::vector<std::string>& remove) {
    const XmlNode* n = node(section, path);
    if (!n || path.empty()) throw FormatError("UI document: invalid path");
    auto attributes = n->attributes;
    for (const std::string& name : remove)
        attributes.erase(std::remove_if(attributes.begin(), attributes.end(), [&](const auto& a) { return a.first == name; }), attributes.end());
    const size_t count = attributeIndex_.size();
    for (const auto& [name, value] : set) {
        const auto present = std::find_if(attributes.begin(), attributes.end(), [&](const auto& a) { return a.first == name; });
        if (present != attributes.end()) {
            present->second = value;
            continue;
        }
        // A new attribute goes before the first present one it precedes more often than not in the export.
        auto it = attributes.end();
        const auto x = attributeIndex_.find(name);
        if (x != attributeIndex_.end())
            for (it = attributes.begin(); it != attributes.end(); ++it) {
                const auto y = attributeIndex_.find(it->first);
                if (y != attributeIndex_.end() && votes_[x->second * count + y->second] > votes_[y->second * count + x->second]) break;
            }
        attributes.insert(it, {name, value});
    }
    if (attributes == n->attributes) return;
    splice(n->sourceStart, n->sourceTagEnd - n->sourceStart, startTag(*n, attributes));
}

std::vector<int32_t> UiDocument::insertElement(UiSection section, const std::vector<int32_t>& parentPath, size_t index, const XmlNode& element) {
    const XmlNode* p = node(section, parentPath);
    if (!p) throw FormatError("UI document: invalid path");
    index = std::min(index, p->children.size());
    const std::string parentIndent = indentOf(*p);
    const std::string body = serialize(element, parentIndent + indentUnit_, indentUnit_, newline_);
    std::vector<int32_t> path = parentPath;
    path.push_back(int32_t(index));
    if (p->selfClosing) {
        // <Tag ... /> becomes <Tag ...>, the child, </Tag>.
        size_t from = p->sourceTagEnd - 2;
        if (from > p->sourceStart && text_[from - 1] == ' ') --from;
        splice(from, p->sourceTagEnd - from, ">" + newline_ + body + parentIndent + "</" + p->tag + ">");
        return path;
    }
    size_t at;
    if (index < p->children.size()) at = lineStart(p->children[index].sourceStart);
    else {
        const size_t endTag = text_.rfind("</", p->sourceEnd - 1);
        at = lineStart(endTag);
        if (at == endTag) {                                  // the end tag shares a line: start one
            splice(at, 0, newline_ + body + parentIndent);
            return path;
        }
    }
    splice(at, 0, body);
    return path;
}

std::vector<int32_t> UiDocument::duplicateElement(UiSection section, const std::vector<int32_t>& path) {
    const XmlNode& p = parentOf(section, path);
    const XmlNode& n = p.children[size_t(path.back())];
    const size_t a = lineStart(n.sourceStart), b = lineAfter(n.sourceEnd);
    if (a == n.sourceStart || b == n.sourceEnd) splice(n.sourceEnd, 0, text_.substr(n.sourceStart, n.sourceEnd - n.sourceStart));
    else splice(b, 0, text_.substr(a, b - a));
    std::vector<int32_t> copy = path;
    ++copy.back();
    return copy;
}

void UiDocument::removeElement(UiSection section, const std::vector<int32_t>& path) {
    const XmlNode& p = parentOf(section, path);
    const XmlNode& n = p.children[size_t(path.back())];
    const size_t a = lineStart(n.sourceStart), b = lineAfter(n.sourceEnd);
    if (a != n.sourceStart && b != n.sourceEnd) splice(a, b - a, "");
    else splice(n.sourceStart, n.sourceEnd - n.sourceStart, "");
}

std::vector<int32_t> UiDocument::moveElement(UiSection section, const std::vector<int32_t>& path, int delta) {
    const XmlNode& p = parentOf(section, path);
    const int64_t i = path.back(), j = i + delta;
    if (delta == 0 || j < 0 || j >= int64_t(p.children.size())) return path;
    // Only neighbours are swapped (swapping across others would move them too).
    if (delta != 1 && delta != -1) throw FormatError("UI document: only neighbouring elements are swapped");
    const XmlNode& first = p.children[size_t(std::min(i, j))];
    const XmlNode& second = p.children[size_t(std::max(i, j))];
    const size_t a1 = lineStart(first.sourceStart), b1 = lineAfter(first.sourceEnd);
    const size_t a2 = lineStart(second.sourceStart), b2 = lineAfter(second.sourceEnd);
    if (b1 > a2) throw FormatError("UI document: elements share a line");
    const std::string swapped = text_.substr(a2, b2 - a2) + text_.substr(b1, a2 - b1) + text_.substr(a1, b1 - a1);
    splice(a1, b2 - a1, swapped);
    std::vector<int32_t> moved = path;
    moved.back() = int32_t(j);
    return moved;
}

}  // namespace tr
