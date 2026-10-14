#include "xml.h"

#include <cctype>

namespace tr {

std::string XmlNode::attribute(const std::string& name) const {
    for (const auto& a : attributes)
        if (a.first == name) return a.second;
    return "";
}

bool XmlNode::hasAttribute(const std::string& name) const {
    for (const auto& a : attributes)
        if (a.first == name) return true;
    return false;
}

const XmlNode* XmlNode::child(const std::string& name) const {
    for (const XmlNode& c : children)
        if (c.tag == name) return &c;
    return nullptr;
}

namespace {

class XmlParser {
public:
    explicit XmlParser(const Bytes& data) : text_(reinterpret_cast<const char*>(data.data()), data.size()) {}
    XmlNode document() {
        skipMisc();
        XmlNode root;
        element(root);
        skipMisc();
        if (pos_ < text_.size()) throw FormatError("XML: content after the root element");
        return root;
    }

private:
    [[noreturn]] void error(const std::string& what) const { throw FormatError("XML: " + what + " at offset " + std::to_string(pos_)); }
    bool startsWith(const char* s) const { return text_.compare(pos_, std::char_traits<char>::length(s), s) == 0; }
    void skipSpace() { while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
    void skipPast(const char* end) {
        const size_t found = text_.find(end, pos_);
        if (found == std::string::npos) error(std::string("missing ") + end);
        pos_ = found + std::char_traits<char>::length(end);
    }
    void skipMisc() {                                  // whitespace, <?…?>, <!--…-->, <!…>
        for (;;) {
            skipSpace();
            if (startsWith("<?")) skipPast("?>");
            else if (startsWith("<!--")) skipPast("-->");
            else if (startsWith("<!")) skipPast(">");
            else return;
        }
    }
    std::string name() {
        const size_t start = pos_;
        while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_' || text_[pos_] == ':' ||
                                       text_[pos_] == '-' || text_[pos_] == '.'))
            ++pos_;
        if (pos_ == start) error("name expected");
        return text_.substr(start, pos_ - start);
    }
    static std::string decode(const char* s, size_t n) {
        std::string out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (s[i] != '&') { out += s[i]; continue; }
            size_t semi = i + 1;
            while (semi < n && s[semi] != ';') ++semi;
            const std::string entity = semi < n ? std::string(s + i + 1, semi - i - 1) : "";
            const char c = entity == "amp" ? '&' : entity == "lt" ? '<' : entity == "gt" ? '>' : entity == "quot" ? '"' : entity == "apos" ? '\'' : 0;
            if (c) { out += c; i = semi; }
            else out += '&';
        }
        return out;
    }
    static std::string trimmed(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }
    void element(XmlNode& node) {
        if (pos_ >= text_.size() || text_[pos_] != '<') error("'<' expected");
        ++pos_;
        node.tag = name();
        for (;;) {
            skipSpace();
            if (startsWith("/>")) { pos_ += 2; return; }
            if (startsWith(">")) { ++pos_; break; }
            std::string key = name();
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '=') error("'=' expected");
            ++pos_;
            skipSpace();
            const char quote = pos_ < text_.size() ? text_[pos_] : 0;
            if (quote != '"' && quote != '\'') error("quoted value expected");
            const size_t end = text_.find(quote, pos_ + 1);
            if (end == std::string::npos) error("unterminated value");
            node.attributes.emplace_back(std::move(key), decode(text_.data() + pos_ + 1, end - pos_ - 1));
            pos_ = end + 1;
        }
        std::string text;
        for (;;) {
            const size_t lt = text_.find('<', pos_);
            if (lt == std::string::npos) error("unterminated element " + node.tag);
            text += decode(text_.data() + pos_, lt - pos_);
            pos_ = lt;
            if (startsWith("</")) {
                pos_ += 2;
                if (name() != node.tag) error("mismatched end tag for " + node.tag);
                skipSpace();
                if (pos_ >= text_.size() || text_[pos_] != '>') error("'>' expected");
                ++pos_;
                node.text = trimmed(text);
                return;
            }
            if (startsWith("<!--")) { skipPast("-->"); continue; }
            node.children.emplace_back();
            element(node.children.back());
        }
    }
    std::string text_;
    size_t pos_ = 0;
};

}  // namespace

XmlNode parseXml(const Bytes& data) { return XmlParser(data).document(); }

}  // namespace tr
