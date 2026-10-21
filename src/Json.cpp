#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

const JsonValue* JsonValue::member(const std::string& name) const {
    if (type != Type::Object) return nullptr;
    for (const auto& m : members)
        if (m.first == name) return &m.second;
    return nullptr;
}

double JsonValue::number() const { return type == Type::Number ? std::strtod(text.c_str(), nullptr) : 0.0; }

bool JsonValue::integer(int64_t& out) const {
    if (type != Type::Number) return false;
    if (text.find_first_of(".eE") == std::string::npos) {
        char* end = nullptr;
        const long long v = std::strtoll(text.c_str(), &end, 10);
        if (end && *end == 0) {
            out = v;
            return true;
        }
        return false;
    }
    const double d = number();
    if (std::floor(d) != d) return false;
    out = int64_t(d);
    return true;
}

namespace {

struct Parser {
    const std::string& s;
    size_t p = 0;
    std::string error;

    void skip() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    }
    bool fail(const char* what) {
        if (error.empty()) error = std::string(what) + " at offset " + std::to_string(p);
        return false;
    }
    bool literal(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (s.compare(p, n, word) != 0) return fail("unknown literal");
        p += n;
        return true;
    }
    static void appendUtf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    bool hex4(uint32_t& v) {
        if (p + 4 > s.size()) return fail("short \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        ++p;                                       // opening quote
        while (p < s.size()) {
            const char c = s[p++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= s.size()) break;
            const char e = s[p++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t c1 = 0;
                    if (!hex4(c1)) return false;
                    if (c1 >= 0xD800 && c1 < 0xDC00 && p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                        p += 2;
                        uint32_t c2 = 0;
                        if (!hex4(c2)) return false;
                        c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    }
                    appendUtf8(out, c1);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(JsonValue& v) {
        const size_t start = p;
        if (s[p] == '-') ++p;
        if (p >= s.size() || !(s[p] >= '0' && s[p] <= '9')) return fail("bad number");
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        if (p < s.size() && s[p] == '.') {
            ++p;
            if (p >= s.size() || !(s[p] >= '0' && s[p] <= '9')) return fail("bad number");
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            ++p;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
            if (p >= s.size() || !(s[p] >= '0' && s[p] <= '9')) return fail("bad number");
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        }
        v.type = JsonValue::Type::Number;
        v.text = s.substr(start, p - start);
        return true;
    }
    bool value(JsonValue& v, int depth) {
        if (depth > 200) return fail("nested too deeply");
        skip();
        if (p >= s.size()) return fail("unexpected end");
        const char c = s[p];
        if (c == '{') {
            ++p;
            v.type = JsonValue::Type::Object;
            skip();
            if (p < s.size() && s[p] == '}') { ++p; return true; }
            while (true) {
                skip();
                if (p >= s.size() || s[p] != '"') return fail("expected a member name");
                std::string name;
                if (!string(name)) return false;
                skip();
                if (p >= s.size() || s[p] != ':') return fail("expected ':'");
                ++p;
                JsonValue member;
                if (!value(member, depth + 1)) return false;
                v.members.emplace_back(std::move(name), std::move(member));
                skip();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            v.type = JsonValue::Type::Array;
            skip();
            if (p < s.size() && s[p] == ']') { ++p; return true; }
            while (true) {
                JsonValue item;
                if (!value(item, depth + 1)) return false;
                v.items.push_back(std::move(item));
                skip();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = JsonValue::Type::String;
            return string(v.text);
        }
        if (c == 't') { v.type = JsonValue::Type::Bool; v.boolean = true; return literal("true"); }
        if (c == 'f') { v.type = JsonValue::Type::Bool; v.boolean = false; return literal("false"); }
        if (c == 'n') { v.type = JsonValue::Type::Null; return literal("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return fail("unexpected character");
    }
};

}  // namespace

bool parseJson(const std::string& textIn, JsonValue& out, std::string& error) {
    Parser parser{textIn};
    out = JsonValue{};
    if (!parser.value(out, 0)) {
        error = parser.error;
        return false;
    }
    parser.skip();
    if (parser.p != textIn.size()) {
        error = "text after the document at offset " + std::to_string(parser.p);
        return false;
    }
    return true;
}

JsonValue& JsonValue::push(JsonValue value) {
    type = Type::Array;
    items.push_back(std::move(value));
    return items.back();
}

JsonValue& JsonValue::set(const std::string& name, JsonValue value) {
    type = Type::Object;
    for (auto& m : members)
        if (m.first == name) return m.second = std::move(value);
    members.emplace_back(name, std::move(value));
    return members.back().second;
}

JsonValue jsonBool(bool value) {
    JsonValue v;
    v.type = JsonValue::Type::Bool;
    v.boolean = value;
    return v;
}

JsonValue jsonNumber(double value) {
    JsonValue v;
    v.type = JsonValue::Type::Number;
    if (!std::isfinite(value)) value = 0;
    char text[32];
    std::snprintf(text, sizeof text, "%.9g", value);
    v.text = text;
    return v;
}

JsonValue jsonInteger(int64_t value) {
    JsonValue v;
    v.type = JsonValue::Type::Number;
    v.text = std::to_string(value);
    return v;
}

JsonValue jsonString(std::string value) {
    JsonValue v;
    v.type = JsonValue::Type::String;
    v.text = std::move(value);
    return v;
}

JsonValue jsonArray() {
    JsonValue v;
    v.type = JsonValue::Type::Array;
    return v;
}

JsonValue jsonObject() {
    JsonValue v;
    v.type = JsonValue::Type::Object;
    return v;
}

namespace {

void writeString(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char text[8];
                    std::snprintf(text, sizeof text, "\\u%04x", c);
                    out += text;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
}

void writeValue(const JsonValue& v, std::string& out) {
    switch (v.type) {
        case JsonValue::Type::Null: out += "null"; break;
        case JsonValue::Type::Bool: out += v.boolean ? "true" : "false"; break;
        case JsonValue::Type::Number: out += v.text.empty() ? "0" : v.text; break;
        case JsonValue::Type::String: writeString(v.text, out); break;
        case JsonValue::Type::Array:
            out += '[';
            for (size_t k = 0; k < v.items.size(); ++k) {
                if (k) out += ',';
                writeValue(v.items[k], out);
            }
            out += ']';
            break;
        case JsonValue::Type::Object:
            out += '{';
            for (size_t k = 0; k < v.members.size(); ++k) {
                if (k) out += ',';
                writeString(v.members[k].first, out);
                out += ':';
                writeValue(v.members[k].second, out);
            }
            out += '}';
            break;
    }
}

}  // namespace

std::string writeJson(const JsonValue& value) {
    std::string out;
    writeValue(value, out);
    return out;
}
