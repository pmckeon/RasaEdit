// gamedata.cpp - see gamedata.h.
#include "gamedata.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>

namespace tr {

namespace py {

namespace {

Ref make(Kind kind) {
    auto o = std::make_shared<Object>();
    o->kind = kind;
    return o;
}

Ref makeString(std::string text, bool unicode = false) {
    Ref o = make(Kind::String);
    o->text = std::move(text);
    o->isUnicode = unicode;
    return o;
}

bool equals(const Object& a, const Object& b) {
    if (isInt(&a) && isInt(&b)) return asInt(&a) == asInt(&b);
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Kind::None: return true;
        case Kind::Float: return a.real == b.real;
        case Kind::String: return a.text == b.text;
        case Kind::Tuple:
            if (a.items.size() != b.items.size()) return false;
            for (size_t k = 0; k < a.items.size(); ++k)
                if (!equals(*a.items[k], *b.items[k])) return false;
            return true;
        default: return &a == &b;
    }
}

void appendInt(std::string& out, int64_t value) {
    out += 'i';
    for (int k = 0; k < 8; ++k) out += char((uint64_t(value) >> (8 * k)) & 0xFF);
}

// Canonical encoding of a hashable key (equal Python keys give equal strings); false if unhashable.
bool encodeKey(const Object& key, std::string& out) {
    switch (key.kind) {
        case Kind::None: out += 'n'; return true;
        case Kind::Bool: case Kind::Int: appendInt(out, asInt(&key)); return true;
        case Kind::Float:
            if (key.real == double(int64_t(key.real))) { appendInt(out, int64_t(key.real)); return true; }
            out += 'f';
            out.append(reinterpret_cast<const char*>(&key.real), sizeof key.real);
            return true;
        case Kind::String:
            out += 's';
            appendInt(out, int64_t(key.text.size()));
            out += key.text;
            return true;
        case Kind::Tuple:
            out += 't';
            appendInt(out, int64_t(key.items.size()));
            for (const Ref& item : key.items)
                if (!item || !encodeKey(*item, out)) return false;
            return true;
        default: return false;
    }
}

DictIndex& indexOf(Object& dict) {
    if (!dict.index) dict.index = std::make_unique<DictIndex>();
    return *dict.index;
}

// Position of key in dict, or SIZE_MAX.
size_t dictPosition(const Object& dict, const Object& key) {
    if (dict.index) {
        const DictIndex& index = *dict.index;
        if (isInt(&key)) {
            const auto it = index.intKeys.find(asInt(&key));
            return it == index.intKeys.end() ? SIZE_MAX : it->second;
        }
        if (key.kind == Kind::String) {
            const auto it = index.stringKeys.find(key.text);
            return it == index.stringKeys.end() ? SIZE_MAX : it->second;
        }
        std::string encoded;
        if (encodeKey(key, encoded)) {
            const auto it = index.otherKeys.find(encoded);
            return it == index.otherKeys.end() ? SIZE_MAX : it->second;
        }
    }
    for (size_t k = 0; k < dict.entries.size(); ++k)             // unhashable keys (never in shipped data)
        if (equals(*dict.entries[k].first, key)) return k;
    return SIZE_MAX;
}

Ref dictFindRef(const Object& dict, const Object& key) {
    const size_t position = dictPosition(dict, key);
    return position == SIZE_MAX ? nullptr : dict.entries[position].second;
}

void dictSet(Object& dict, const Ref& key, const Ref& value) {
    const size_t position = dictPosition(dict, *key);
    if (position != SIZE_MAX) {
        dict.entries[position].second = value;
        return;
    }
    DictIndex& index = indexOf(dict);
    const size_t next = dict.entries.size();
    if (isInt(key.get())) index.intKeys.emplace(asInt(key.get()), next);
    else if (key->kind == Kind::String) index.stringKeys.emplace(key->text, next);
    else {
        std::string encoded;
        if (encodeKey(*key, encoded)) index.otherKeys.emplace(std::move(encoded), next);
    }
    dict.entries.emplace_back(key, value);
}

}  // namespace

bool isNone(const Object* o) { return o == nullptr || o->kind == Kind::None; }
bool isInt(const Object* o) { return o != nullptr && (o->kind == Kind::Int || o->kind == Kind::Bool); }
bool isString(const Object* o) { return o != nullptr && o->kind == Kind::String; }

int64_t asInt(const Object* o, int64_t fallback) {
    if (o == nullptr) return fallback;
    if (o->kind == Kind::Int) return o->integer;
    if (o->kind == Kind::Bool) return o->boolean ? 1 : 0;
    return fallback;
}

double asFloat(const Object* o, double fallback) {
    if (o == nullptr) return fallback;
    if (o->kind == Kind::Float) return o->real;
    if (isInt(o)) return double(asInt(o));
    return fallback;
}

std::string asString(const Object* o, const std::string& fallback) { return isString(o) ? o->text : fallback; }

Ref makeInt(int64_t value) {
    Ref o = make(Kind::Int);
    o->integer = value;
    return o;
}
Ref makeText(std::string utf8) { return makeString(std::move(utf8), true); }
Ref makeTuple(std::vector<Ref> items) {
    Ref o = make(Kind::Tuple);
    o->items = std::move(items);
    return o;
}
Ref makeList(std::vector<Ref> items) {
    Ref o = make(Kind::List);
    o->items = std::move(items);
    return o;
}
Ref makeDict() { return make(Kind::Dict); }

void dictPut(Object& dict, const Ref& key, const Ref& value) { dictSet(dict, key, value); }
Ref dictGetRef(const Object& dict, const Object& key) { return dictFindRef(dict, key); }

bool dictRemove(Object& dict, const Object& key) {
    const size_t position = dictPosition(dict, key);
    if (position == SIZE_MAX) return false;
    std::vector<std::pair<Ref, Ref>> entries = std::move(dict.entries);
    entries.erase(entries.begin() + std::ptrdiff_t(position));
    dict.entries.clear();
    dict.index.reset();
    for (const auto& e : entries) dictSet(dict, e.first, e.second);
    return true;
}

Ref deepCopy(const Ref& o) {
    if (!o) return o;
    if (o->kind == Kind::Tuple || o->kind == Kind::List || o->kind == Kind::Set) {
        Ref c = make(o->kind);
        for (const Ref& item : o->items) c->items.push_back(deepCopy(item));
        return c;
    }
    if (o->kind == Kind::Dict) {
        Ref c = make(Kind::Dict);
        for (const auto& e : o->entries) dictSet(*c, deepCopy(e.first), deepCopy(e.second));
        return c;
    }
    return o;
}

namespace {
// Next code point of UTF-8 text (an invalid byte is taken as that Latin-1 character).
uint32_t nextCodePoint(const std::string& s, size_t& p) {
    const unsigned char c = static_cast<unsigned char>(s[p]);
    const auto continuation = [&](size_t k) { return p + k < s.size() && (static_cast<unsigned char>(s[p + k]) & 0xC0) == 0x80; };
    if (c >= 0xF0 && continuation(1) && continuation(2) && continuation(3)) {
        const uint32_t v = (uint32_t(c & 0x07) << 18) | (uint32_t(s[p + 1] & 0x3F) << 12) | (uint32_t(s[p + 2] & 0x3F) << 6) | uint32_t(s[p + 3] & 0x3F);
        p += 4;
        return v;
    }
    if (c >= 0xE0 && continuation(1) && continuation(2)) {
        const uint32_t v = (uint32_t(c & 0x0F) << 12) | (uint32_t(s[p + 1] & 0x3F) << 6) | uint32_t(s[p + 2] & 0x3F);
        p += 3;
        return v;
    }
    if (c >= 0xC0 && continuation(1)) {
        const uint32_t v = (uint32_t(c & 0x1F) << 6) | uint32_t(s[p + 1] & 0x3F);
        p += 2;
        return v;
    }
    ++p;
    return c;
}

void appendStringLiteral(std::string& out, const Object& o) {
    char buffer[16];
    if (o.isUnicode) out += 'u';
    out += '\'';
    for (size_t p = 0; p < o.text.size();) {
        const uint32_t c = o.isUnicode ? nextCodePoint(o.text, p) : static_cast<unsigned char>(o.text[p++]);
        if (c == '\\') out += "\\\\";
        else if (c == '\'') out += "\\'";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 0x20 && c < 0x7F) out += char(c);
        else {
            if (c < 0x100) std::snprintf(buffer, sizeof buffer, "\\x%02x", unsigned(c));
            else if (c < 0x10000) std::snprintf(buffer, sizeof buffer, "\\u%04x", unsigned(c));
            else std::snprintf(buffer, sizeof buffer, "\\U%08x", unsigned(c));
            out += buffer;
        }
    }
    out += '\'';
}

void appendLiteral(std::string& out, const Object& o) {
    switch (o.kind) {
        case Kind::None: out += "None"; return;
        case Kind::Bool: out += o.boolean ? "True" : "False"; return;
        case Kind::Int: out += std::to_string(o.integer); return;
        case Kind::Float: {
            char buffer[40];
            std::snprintf(buffer, sizeof buffer, "%.17g", o.real);
            std::string text = buffer;
            if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
            out += text;
            return;
        }
        case Kind::String: appendStringLiteral(out, o); return;
        case Kind::Tuple:
        case Kind::List: {
            out += o.kind == Kind::Tuple ? '(' : '[';
            for (size_t k = 0; k < o.items.size(); ++k) {
                if (k) out += ", ";
                appendLiteral(out, *o.items[k]);
            }
            if (o.kind == Kind::Tuple && o.items.size() == 1) out += ',';
            out += o.kind == Kind::Tuple ? ')' : ']';
            return;
        }
        case Kind::Dict: {
            out += '{';
            for (size_t k = 0; k < o.entries.size(); ++k) {
                if (k) out += ", ";
                appendLiteral(out, *o.entries[k].first);
                out += ": ";
                appendLiteral(out, *o.entries[k].second);
            }
            out += '}';
            return;
        }
        default: throw FormatError("sourceLiteral: no literal for " + repr(&o, 2));
    }
}
}  // namespace

std::string sourceLiteral(const Object& o) {
    std::string out;
    appendLiteral(out, o);
    return out;
}

std::string moduleSource(const Object& globals) {
    std::string out;
    for (const auto& g : globals.entries) {
        const std::string name = asString(g.first.get());
        if (name.empty() || name.compare(0, 2, "__") == 0) continue;
        out += name + " = ";
        const Object& value = *g.second;
        if (value.kind == Kind::Dict && !value.entries.empty()) {
            out += "{\n";
            for (const auto& e : value.entries) out += "    " + sourceLiteral(*e.first) + ": " + sourceLiteral(*e.second) + ",\n";
            out += "}\n";
        } else out += sourceLiteral(value) + "\n";
    }
    return out;
}

size_t length(const Object* o) {
    if (o == nullptr) return 0;
    if (o->kind == Kind::Tuple || o->kind == Kind::List || o->kind == Kind::Set) return o->items.size();
    if (o->kind == Kind::Dict) return o->entries.size();
    return 0;
}

const Object* item(const Object* sequence, size_t index) {
    if (sequence == nullptr || (sequence->kind != Kind::Tuple && sequence->kind != Kind::List)) return nullptr;
    return index < sequence->items.size() ? sequence->items[index].get() : nullptr;
}

const Object* dictGet(const Object* dict, int64_t key) {
    if (dict == nullptr || dict->kind != Kind::Dict || !dict->index) return nullptr;
    const auto it = dict->index->intKeys.find(key);
    return it == dict->index->intKeys.end() ? nullptr : dict->entries[it->second].second.get();
}

const Object* dictGet(const Object* dict, const std::string& key) {
    if (dict == nullptr || dict->kind != Kind::Dict || !dict->index) return nullptr;
    const auto it = dict->index->stringKeys.find(key);
    return it == dict->index->stringKeys.end() ? nullptr : dict->entries[it->second].second.get();
}

const Object* dictGetTuple(const Object* dict, std::initializer_list<int64_t> key) {
    if (dict == nullptr || dict->kind != Kind::Dict || !dict->index) return nullptr;
    std::string encoded = "t";
    appendInt(encoded, int64_t(key.size()));
    for (int64_t part : key) appendInt(encoded, part);
    const auto it = dict->index->otherKeys.find(encoded);
    return it == dict->index->otherKeys.end() ? nullptr : dict->entries[it->second].second.get();
}

const Object* attribute(const Object* object, const std::string& name) {
    // Instance attributes first, then (as in Python) attributes set on its class.
    for (const Object* owner = object; owner != nullptr;
         owner = owner->kind == Kind::Instance ? owner->instanceClass.get() : nullptr) {
        if (owner->kind != Kind::Instance && owner->kind != Kind::Class) return nullptr;
        for (const auto& a : owner->attributes)
            if (a.first == name) return a.second.get();
        if (owner->kind == Kind::Class) return nullptr;
    }
    return nullptr;
}

std::string repr(const Object* o, size_t maxItems) {
    if (o == nullptr) return "<null>";
    char number[64];
    auto sequence = [&](const std::vector<Ref>& items, const char* open, const char* close) {
        std::string s = open;
        for (size_t k = 0; k < items.size() && k < maxItems; ++k) s += (k ? ", " : "") + repr(items[k].get(), maxItems);
        if (items.size() > maxItems) s += ", ... (" + std::to_string(items.size()) + " items)";
        if (items.size() == 1 && o->kind == Kind::Tuple) s += ",";
        return s + close;
    };
    switch (o->kind) {
        case Kind::None: return "None";
        case Kind::Bool: return o->boolean ? "True" : "False";
        case Kind::Int: return std::to_string(o->integer);
        case Kind::Float: std::snprintf(number, sizeof number, "%.17g", o->real); return number;
        case Kind::String: return std::string(o->isUnicode ? "u'" : "'") + o->text + "'";
        case Kind::Tuple: return sequence(o->items, "(", ")");
        case Kind::List: return sequence(o->items, "[", "]");
        case Kind::Set: return sequence(o->items, "set([", "])");
        case Kind::Dict: {
            std::string s = "{";
            for (size_t k = 0; k < o->entries.size() && k < maxItems; ++k)
                s += (k ? ", " : "") + repr(o->entries[k].first.get(), maxItems) + ": " + repr(o->entries[k].second.get(), maxItems);
            if (o->entries.size() > maxItems) s += ", ... (" + std::to_string(o->entries.size()) + " entries)";
            return s + "}";
        }
        case Kind::Code: return "<code " + o->code->name + ">";
        case Kind::Function: return "<function " + o->code->name + ">";
        case Kind::ClassBody: return "<class body>";
        case Kind::Class: return "<class " + o->text + ">";
        case Kind::Instance: {
            std::string s = o->instanceClass ? o->instanceClass->text + "(" : "(";
            for (size_t k = 0; k < o->attributes.size(); ++k)
                s += (k ? ", " : "") + o->attributes[k].first + "=" + repr(o->attributes[k].second.get(), maxItems);
            return s + ")";
        }
        case Kind::Opaque: return "<" + o->text + ">";
    }
    return "?";
}

}  // namespace py

// ------------------------------------------------------------------------------------------
// Python 2.4 marshal reader
// ------------------------------------------------------------------------------------------
namespace {

using py::Kind;
using py::Object;
using py::Ref;

class Marshal {
public:
    Marshal(const Bytes& data, size_t start) : d_(data), p_(start) {}

    Ref read() {
        if (++depth_ > 256) throw FormatError("marshal: nesting too deep");
        Ref result = readInner();
        --depth_;
        return result;
    }

private:
    uint8_t u8() {
        need(1);
        return d_[p_++];
    }
    int32_t i32() {
        need(4);
        const uint32_t v = uint32_t(d_[p_]) | (uint32_t(d_[p_ + 1]) << 8) | (uint32_t(d_[p_ + 2]) << 16) | (uint32_t(d_[p_ + 3]) << 24);
        p_ += 4;
        return int32_t(v);
    }
    int64_t i64() {
        const uint64_t low = uint32_t(i32());
        const uint64_t high = uint32_t(i32());
        return int64_t(low | (high << 32));
    }
    double f64() {
        const uint64_t bits = uint64_t(i64());
        double value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
    std::string bytes(size_t count) {
        need(count);
        std::string s(reinterpret_cast<const char*>(d_.data() + p_), count);
        p_ += count;
        return s;
    }
    size_t count() {
        const int32_t n = i32();
        if (n < 0 || size_t(n) > d_.size() - p_ + 1) throw FormatError("marshal: bad length " + std::to_string(n));
        return size_t(n);
    }
    void need(size_t n) const {
        if (n > d_.size() - p_) throw FormatError("marshal: unexpected end of data");
    }
    std::vector<std::string> stringTuple() {
        const Ref t = read();
        std::vector<std::string> out;
        for (const Ref& item : t->items) out.push_back(item->text);
        return out;
    }

    Ref readInner() {
        const char type = char(u8());
        Ref o;
        switch (type) {
            case '0': return nullptr;                                   // TYPE_NULL (dict terminator)
            case 'N': return std::make_shared<Object>();
            case 'F': case 'T': o = std::make_shared<Object>(); o->kind = Kind::Bool; o->boolean = type == 'T'; return o;
            case 'S': case '.': o = std::make_shared<Object>(); o->kind = Kind::Opaque; o->text = type == 'S' ? "StopIteration" : "Ellipsis"; return o;
            case 'i': o = std::make_shared<Object>(); o->kind = Kind::Int; o->integer = i32(); return o;
            case 'I': o = std::make_shared<Object>(); o->kind = Kind::Int; o->integer = i64(); return o;
            case 'l': {                                                 // long: 15-bit digits, sign in the count
                const int32_t n = i32();
                const size_t digits = size_t(n < 0 ? -int64_t(n) : n);
                uint64_t value = 0;
                for (size_t k = 0; k < digits; ++k) {
                    need(2);
                    const uint64_t digit = uint64_t(d_[p_]) | (uint64_t(d_[p_ + 1]) << 8);
                    p_ += 2;
                    if (k < 5) value |= digit << (15 * k);
                }
                o = std::make_shared<Object>(); o->kind = Kind::Int; o->integer = n < 0 ? -int64_t(value) : int64_t(value);
                return o;
            }
            case 'f': { const size_t n = u8(); const std::string s = bytes(n); o = std::make_shared<Object>(); o->kind = Kind::Float; o->real = std::strtod(s.c_str(), nullptr); return o; }
            case 'g': o = std::make_shared<Object>(); o->kind = Kind::Float; o->real = f64(); return o;
            case 'x': { const size_t a = u8(); const std::string re = bytes(a); const size_t b = u8(); bytes(b);
                        o = std::make_shared<Object>(); o->kind = Kind::Opaque; o->text = "complex " + re; return o; }
            case 'y': f64(); f64(); o = std::make_shared<Object>(); o->kind = Kind::Opaque; o->text = "complex"; return o;
            case 's': case 't': {
                o = std::make_shared<Object>(); o->kind = Kind::String; o->text = bytes(count());
                if (type == 't') interned_.push_back(o);
                return o;
            }
            case 'R': {
                const int32_t index = i32();
                if (index < 0 || size_t(index) >= interned_.size()) throw FormatError("marshal: bad string reference");
                return interned_[size_t(index)];
            }
            case 'u': o = std::make_shared<Object>(); o->kind = Kind::String; o->isUnicode = true; o->text = bytes(count()); return o;
            case '(': case '[': case '<': case '>': {
                o = std::make_shared<Object>();
                o->kind = type == '(' ? Kind::Tuple : type == '[' ? Kind::List : Kind::Set;
                const size_t n = count();
                o->items.reserve(n);
                for (size_t k = 0; k < n; ++k) o->items.push_back(read());
                return o;
            }
            case '{': {
                o = std::make_shared<Object>(); o->kind = Kind::Dict;
                for (;;) {
                    Ref key = read();
                    if (!key) break;
                    py::dictSet(*o, key, read());
                }
                return o;
            }
            case 'c': {                                                 // code object (Python 2.3 - 2.7 layout)
                auto code = std::make_shared<py::Code>();
                code->argCount = i32();
                i32();                                                  // nlocals
                i32();                                                  // stacksize
                code->flags = i32();
                code->bytecode = read()->text;
                code->constants = read()->items;
                code->names = stringTuple();
                code->varNames = stringTuple();
                read();                                                 // freevars
                read();                                                 // cellvars
                read();                                                 // filename
                code->name = read()->text;
                i32();                                                  // firstlineno
                read();                                                 // lnotab
                o = std::make_shared<Object>(); o->kind = Kind::Code; o->code = code;
                return o;
            }
            default:
                throw FormatError(std::string("marshal: unsupported type '") + type + "' at offset " + std::to_string(p_ - 1));
        }
    }

    const Bytes& d_;
    size_t p_;
    std::vector<Ref> interned_;
    int depth_ = 0;
};

// ------------------------------------------------------------------------------------------
// Module evaluator (Python 2.4 opcode numbers)
// ------------------------------------------------------------------------------------------
Ref builtin(const std::string& name) {
    Ref o = std::make_shared<Object>();
    if (name == "None") return o;
    if (name == "True" || name == "False") { o->kind = Kind::Bool; o->boolean = name == "True"; return o; }
    o->kind = Kind::Opaque;
    o->text = "builtin " + name;
    return o;
}

Ref runModule(const py::Code& code, const std::string& moduleName) {
    Ref globals = std::make_shared<Object>();
    globals->kind = Kind::Dict;
    std::vector<Ref> stack;
    auto fail = [&](const std::string& what) -> FormatError { return FormatError(moduleName + ": " + what); };
    auto pop = [&]() {
        if (stack.empty()) throw fail("stack underflow");
        Ref top = std::move(stack.back());
        stack.pop_back();
        return top;
    };
    auto popN = [&](size_t n) {
        if (stack.size() < n) throw fail("stack underflow");
        std::vector<Ref> items(stack.end() - std::ptrdiff_t(n), stack.end());
        stack.resize(stack.size() - n);
        return items;
    };
    const std::string& bc = code.bytecode;
    size_t pc = 0;
    uint32_t extended = 0;
    while (pc < bc.size()) {
        const uint8_t op = uint8_t(bc[pc++]);
        uint32_t arg = 0;
        if (op >= 90) {                                                 // HAVE_ARGUMENT
            if (pc + 2 > bc.size()) throw fail("truncated bytecode");
            arg = (uint32_t(uint8_t(bc[pc])) | (uint32_t(uint8_t(bc[pc + 1])) << 8)) | extended;
            pc += 2;
            extended = 0;
        }
        switch (op) {
            case 1: pop(); break;                                       // POP_TOP
            case 2: {                                                   // ROT_TWO
                if (stack.size() < 2) throw fail("stack underflow");
                std::swap(stack[stack.size() - 1], stack[stack.size() - 2]);
                break;
            }
            case 3: {                                                   // ROT_THREE: top, second, third = second, third, top
                if (stack.size() < 3) throw fail("stack underflow");
                const size_t n = stack.size();
                Ref v = stack[n - 1], w = stack[n - 2], x = stack[n - 3];
                stack[n - 1] = w; stack[n - 2] = x; stack[n - 3] = v;
                break;
            }
            case 4:                                                     // DUP_TOP
                if (stack.empty()) throw fail("stack underflow");
                stack.push_back(stack.back());
                break;
            case 11: {                                                  // UNARY_NEGATIVE
                Ref v = pop();
                Ref r = std::make_shared<Object>();
                r->kind = v->kind;
                if (v->kind == Kind::Int) r->integer = -v->integer;
                else if (v->kind == Kind::Float) r->real = -v->real;
                else throw fail("negation of " + py::repr(v.get()));
                stack.push_back(r);
                break;
            }
            case 25: {                                                  // BINARY_SUBSCR: v[w]
                Ref w = pop(), v = pop();
                Ref found;
                if (v->kind == Kind::Dict) found = py::dictFindRef(*v, *w);
                else if ((v->kind == Kind::Tuple || v->kind == Kind::List) && py::isInt(w.get())) {
                    int64_t i = py::asInt(w.get());
                    if (i < 0) i += int64_t(v->items.size());
                    if (i >= 0 && size_t(i) < v->items.size()) found = v->items[size_t(i)];
                }
                if (!found) throw fail("subscript " + py::repr(w.get()) + " not found");
                stack.push_back(found);
                break;
            }
            case 60: {                                                  // STORE_SUBSCR: SECOND[TOP] = THIRD
                Ref w = pop(), v = pop(), u = pop();
                if (v->kind == Kind::Dict) py::dictSet(*v, w, u);
                else if (v->kind == Kind::List && py::isInt(w.get()) && size_t(py::asInt(w.get())) < v->items.size()) v->items[size_t(py::asInt(w.get()))] = u;
                else throw fail("item assignment on " + py::repr(v.get(), 2));
                break;
            }
            case 83: return globals;                                    // RETURN_VALUE (module end)
            case 89: {                                                  // BUILD_CLASS: name, bases, methods
                Ref methods = pop(), bases = pop(), name = pop();
                Ref cls = std::make_shared<Object>();
                cls->kind = Kind::Class;
                cls->text = name->text;
                if (methods->code)
                    for (const Ref& c : methods->code->constants)
                        if (c && c->kind == Kind::Code && c->code->name == "__init__")
                            for (int32_t k = 1; k < c->code->argCount && size_t(k) < c->code->varNames.size(); ++k)
                                cls->fieldNames.push_back(c->code->varNames[size_t(k)]);
                stack.push_back(cls);
                break;
            }
            case 90: {                                                  // STORE_NAME
                if (arg >= code.names.size()) throw fail("bad name index");
                Ref key = py::makeString(code.names[arg]);
                py::dictSet(*globals, key, pop());
                break;
            }
            case 95: {                                                  // STORE_ATTR: TOP.name = SECOND
                if (arg >= code.names.size()) throw fail("bad name index");
                Ref v = pop(), u = pop();
                if (v->kind != Kind::Instance && v->kind != Kind::Class) throw fail("attribute assignment on " + py::repr(v.get(), 2));
                bool replaced = false;
                for (auto& a : v->attributes)
                    if (a.first == code.names[arg]) { a.second = u; replaced = true; }
                if (!replaced) v->attributes.emplace_back(code.names[arg], u);
                break;
            }
            case 100:                                                   // LOAD_CONST
                if (arg >= code.constants.size()) throw fail("bad constant index");
                stack.push_back(code.constants[arg] ? code.constants[arg] : std::make_shared<Object>());
                break;
            case 101: case 116: {                                       // LOAD_NAME, LOAD_GLOBAL
                if (arg >= code.names.size()) throw fail("bad name index");
                const std::string& name = code.names[arg];
                Ref value;
                if (globals->index) {
                    const auto it = globals->index->stringKeys.find(name);
                    if (it != globals->index->stringKeys.end()) value = globals->entries[it->second].second;
                }
                stack.push_back(value ? value : builtin(name));
                break;
            }
            case 102: case 103: {                                       // BUILD_TUPLE, BUILD_LIST
                Ref o = std::make_shared<Object>();
                o->kind = op == 102 ? Kind::Tuple : Kind::List;
                o->items = popN(arg);
                stack.push_back(o);
                break;
            }
            case 104: {                                                 // BUILD_MAP
                Ref o = std::make_shared<Object>();
                o->kind = Kind::Dict;
                stack.push_back(o);
                break;
            }
            case 105: {                                                 // LOAD_ATTR
                if (arg >= code.names.size()) throw fail("bad name index");
                Ref v = pop();
                Ref found;
                for (const Object* owner = v.get(); owner && !found; owner = owner->kind == Kind::Instance ? owner->instanceClass.get() : nullptr)
                    if (owner->kind == Kind::Instance || owner->kind == Kind::Class)
                        for (const auto& a : owner->attributes)
                            if (a.first == code.names[arg]) found = a.second;
                if (!found) throw fail("attribute " + code.names[arg] + " not found");
                stack.push_back(found);
                break;
            }
            case 131: {                                                 // CALL_FUNCTION
                const size_t positional = arg & 0xFF;
                const size_t keywords = (arg >> 8) & 0xFF;
                std::vector<Ref> kw = popN(keywords * 2);
                std::vector<Ref> args = popN(positional);
                Ref callee = pop();
                Ref result = std::make_shared<Object>();
                if (callee->kind == Kind::Function) {                   // class body: not executed
                    result->kind = Kind::ClassBody;
                    result->code = callee->code;
                } else if (callee->kind == Kind::Class) {               // data class: __init__ stores its parameters
                    result->kind = Kind::Instance;
                    result->instanceClass = callee;
                    for (size_t k = 0; k < args.size(); ++k)
                        result->attributes.emplace_back(k < callee->fieldNames.size() ? callee->fieldNames[k] : "arg" + std::to_string(k), args[k]);
                    for (size_t k = 0; k + 1 < kw.size(); k += 2) result->attributes.emplace_back(kw[k]->text, kw[k + 1]);
                } else {
                    throw fail("call of " + py::repr(callee.get(), 2));
                }
                stack.push_back(result);
                break;
            }
            case 132: {                                                 // MAKE_FUNCTION (arg = default count)
                Ref codeObject = pop();
                popN(arg);
                Ref f = std::make_shared<Object>();
                f->kind = Kind::Function;
                f->code = codeObject->code;
                stack.push_back(f);
                break;
            }
            case 143: extended = arg << 16; break;                      // EXTENDED_ARG
            default:
                throw fail("unsupported opcode " + std::to_string(op) + " at " + std::to_string(pc - 1));
        }
    }
    return globals;
}

}  // namespace

py::Ref evaluateModule(const Bytes& pyo, const std::string& moduleName) {
    if (pyo.size() < 9 || pyo[2] != 0x0D || pyo[3] != 0x0A) throw FormatError(moduleName + ": not a compiled Python module");
    const uint16_t magic = uint16_t(pyo[0] | (pyo[1] << 8));
    if (magic < 62041 || magic > 62061) throw FormatError(moduleName + ": not Python 2.4 bytecode (magic " + std::to_string(magic) + ")");
    Marshal m(pyo, 8);
    const Ref codeObject = m.read();
    if (!codeObject || codeObject->kind != py::Kind::Code) throw FormatError(moduleName + ": no module code object");
    return runModule(*codeObject->code, moduleName);
}

// ------------------------------------------------------------------------------------------
// Python source modules (literal tables only)
// ------------------------------------------------------------------------------------------
namespace {

using py::Kind;
using py::Object;
using py::Ref;
using py::dictFindRef;
using py::dictSet;
using py::make;
using py::makeString;

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
}

class SourceParser {
public:
    SourceParser(const Bytes& source, const std::string& moduleName) : s_(source), module_(moduleName) {}

    Ref run() {
        Ref globals = make(Kind::Dict);
        for (;;) {
            skipSpace(true);
            if (p_ >= s_.size()) break;
            const std::string name = identifier();
            if (name.empty()) fail("expected a name");
            skipSpace(false);
            if (peek() != '=') fail("expected '=' after " + name);
            ++p_;
            globals_ = globals.get();
            Ref value = expression();
            dictSet(*globals, makeString(name), value);
            skipSpace(false);
            if (p_ < s_.size() && peek() != '\n' && peek() != ';' && peek() != '#') fail("unexpected text after " + name);
        }
        return globals;
    }

private:
    const Bytes& s_;
    std::string module_;
    size_t p_ = 0;
    int depth_ = 0;                                   // inside brackets: newlines are spaces
    Object* globals_ = nullptr;

    [[noreturn]] void fail(const std::string& what) const {
        size_t line = 1;
        for (size_t k = 0; k < p_ && k < s_.size(); ++k) line += s_[k] == '\n';
        throw FormatError(module_ + ": " + what + " (line " + std::to_string(line) + ")");
    }
    char peek() const { return p_ < s_.size() ? char(s_[p_]) : '\0'; }
    // Spaces, tabs, carriage returns, comments, backslash continuations; newlines too when inside brackets or asked.
    void skipSpace(bool newlines) {
        while (p_ < s_.size()) {
            const char c = char(s_[p_]);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f') ++p_;
            else if (c == '\n' && (newlines || depth_ > 0)) ++p_;
            else if (c == '\\' && p_ + 1 < s_.size() && (s_[p_ + 1] == '\n' || s_[p_ + 1] == '\r')) p_ += 2;
            else if (c == '#') { while (p_ < s_.size() && s_[p_] != '\n') ++p_; }
            else break;
        }
    }
    std::string identifier() {
        const size_t start = p_;
        while (p_ < s_.size() && (std::isalnum(s_[p_]) || s_[p_] == '_')) ++p_;
        if (start < p_ && std::isdigit(s_[start])) { p_ = start; return {}; }
        return std::string(reinterpret_cast<const char*>(s_.data()) + start, p_ - start);
    }
    Ref expression() {
        skipSpace(false);
        Ref first = atom();
        skipSpace(false);
        if (depth_ == 0 && peek() == ',') {               // a bare tuple: name = 1, 2
            Ref tuple = make(Kind::Tuple);
            tuple->items.push_back(first);
            while (peek() == ',') {
                ++p_;
                skipSpace(false);
                if (p_ >= s_.size() || peek() == '\n' || peek() == '#') break;
                tuple->items.push_back(atom());
                skipSpace(false);
            }
            return tuple;
        }
        return first;
    }
    // Items up to `close`, separated by commas (a trailing comma allowed).
    template <typename Item>
    bool items(char close, Item item) {
        ++depth_;
        bool comma = false;
        for (;;) {
            skipSpace(true);
            if (peek() == close) { ++p_; break; }
            item();
            skipSpace(true);
            if (peek() == ',') { ++p_; comma = true; continue; }
            if (peek() != close) fail(std::string("expected '") + close + "'");
            ++p_;
            break;
        }
        --depth_;
        return comma;
    }
    Ref atom() {
        skipSpace(false);
        const char c = peek();
        if (c == '(') {
            ++p_;
            Ref tuple = make(Kind::Tuple);
            const bool comma = items(')', [&] { tuple->items.push_back(atom()); });
            if (tuple->items.size() == 1 && !comma) return tuple->items[0];      // parentheses only
            return tuple;
        }
        if (c == '[') {
            ++p_;
            Ref list = make(Kind::List);
            items(']', [&] { list->items.push_back(atom()); });
            return list;
        }
        if (c == '{') {
            ++p_;
            Ref dict = make(Kind::Dict);
            items('}', [&] {
                Ref key = atom();
                skipSpace(true);
                if (peek() != ':') fail("expected ':' in a dict");
                ++p_;
                dictSet(*dict, key, atom());
            });
            return dict;
        }
        if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && std::isdigit(s_[p_ + 1]))) return number();
        if (c == '\'' || c == '"') return string(false);
        if ((c == 'u' || c == 'U' || c == 'r' || c == 'R') && p_ + 1 < s_.size()) {
            size_t q = p_;
            bool unicode = false, raw = false;
            while (q < s_.size() && std::strchr("uUrR", s_[q])) { unicode |= s_[q] == 'u' || s_[q] == 'U'; raw |= s_[q] == 'r' || s_[q] == 'R'; ++q; }
            if (q < s_.size() && (s_[q] == '\'' || s_[q] == '"') && q - p_ <= 2) {
                p_ = q;
                return string(unicode, raw);
            }
        }
        const std::string name = identifier();
        if (name == "None") return make(Kind::None);
        if (name == "True" || name == "False") {
            Ref b = make(Kind::Bool);
            b->boolean = name == "True";
            b->integer = b->boolean ? 1 : 0;
            return b;
        }
        if (!name.empty() && globals_) {
            Ref value = dictFindRef(*globals_, *makeString(name));
            if (value) return value;
            fail("unknown name " + name);
        }
        fail("unexpected character");
    }
    Ref number() {
        const size_t start = p_;
        if (peek() == '-' || peek() == '+') { ++p_; skipSpace(false); }
        bool real = false;
        while (p_ < s_.size() && (std::isalnum(s_[p_]) || s_[p_] == '.' ||
                                  ((s_[p_] == '-' || s_[p_] == '+') && (s_[p_ - 1] == 'e' || s_[p_ - 1] == 'E')))) {
            if (s_[p_] == '.' || ((s_[p_] == 'e' || s_[p_] == 'E') && !(p_ > start + 1 && (s_[start + 1] == 'x' || s_[start + 1] == 'X')))) real = true;
            ++p_;
        }
        std::string text(reinterpret_cast<const char*>(s_.data()) + start, p_ - start);
        text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
        while (!text.empty() && (text.back() == 'L' || text.back() == 'l')) text.pop_back();
        if (real) {
            Ref f = make(Kind::Float);
            f->real = std::stod(text);
            return f;
        }
        Ref i = make(Kind::Int);
        const bool negative = !text.empty() && text[0] == '-';
        std::string digits = text.substr(!text.empty() && (text[0] == '-' || text[0] == '+') ? 1 : 0);
        int base = 10;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) { base = 16; digits = digits.substr(2); }
        else if (digits.size() > 1 && digits[0] == '0') base = 8;
        i->integer = int64_t(std::stoull(digits, nullptr, base)) * (negative ? -1 : 1);
        return i;
    }
    // One string literal, then any adjacent literals concatenated (Python's implicit concatenation).
    Ref string(bool unicode, bool raw = false) {
        std::string text = literal(unicode, raw);
        for (;;) {
            const size_t save = p_;
            skipSpace(depth_ > 0);
            size_t q = p_;
            bool u = false, r = false;
            while (q < s_.size() && std::strchr("uUrR", s_[q]) && q - p_ < 2) { u |= s_[q] == 'u' || s_[q] == 'U'; r |= s_[q] == 'r' || s_[q] == 'R'; ++q; }
            if (q < s_.size() && (s_[q] == '\'' || s_[q] == '"')) {
                p_ = q;
                unicode |= u;
                text += literal(u || unicode, r);
            } else {
                p_ = save;
                break;
            }
        }
        return makeString(std::move(text), unicode);
    }
    std::string literal(bool unicode, bool raw) {
        const char quote = peek();
        const bool triple = p_ + 2 < s_.size() && s_[p_ + 1] == quote && s_[p_ + 2] == quote;
        p_ += triple ? 3 : 1;
        std::string out;
        for (;;) {
            if (p_ >= s_.size()) fail("unterminated string");
            const char c = char(s_[p_]);
            if (c == quote && (!triple || (p_ + 2 < s_.size() && s_[p_ + 1] == quote && s_[p_ + 2] == quote))) {
                p_ += triple ? 3 : 1;
                return out;
            }
            if (c == '\\' && !raw && p_ + 1 < s_.size()) {
                const char e = char(s_[p_ + 1]);
                p_ += 2;
                auto hex = [&](size_t n) {
                    if (p_ + n > s_.size()) fail("bad escape");
                    const uint32_t v = uint32_t(std::stoul(std::string(reinterpret_cast<const char*>(s_.data()) + p_, n), nullptr, 16));
                    p_ += n;
                    return v;
                };
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                        uint32_t v = uint32_t(e - '0');
                        for (int k = 0; k < 2 && p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '7'; ++k) v = v * 8 + uint32_t(s_[p_++] - '0');
                        if (unicode) appendUtf8(out, v); else out += char(v);
                        break;
                    }
                    case 'x': { const uint32_t v = hex(2); if (unicode) appendUtf8(out, v); else out += char(v); break; }
                    case 'u': if (unicode) { appendUtf8(out, hex(4)); break; } out += "\\u"; break;
                    case 'U': if (unicode) { appendUtf8(out, hex(8)); break; } out += "\\U"; break;
                    case 'a': out += '\a'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'v': out += '\v'; break;
                    case '\n': break;                                      // line continuation inside the string
                    case '\\': case '\'': case '"': out += e; break;
                    default: out += '\\'; out += e; break;
                }
                continue;
            }
            if (unicode && (unsigned char)c >= 0x80) appendUtf8(out, (unsigned char)c);      // Latin-1 source bytes
            else out += c;
            ++p_;
        }
    }
};

}  // namespace

py::Ref evaluateSource(const Bytes& source, const std::string& moduleName) { return SourceParser(source, moduleName).run(); }

// ------------------------------------------------------------------------------------------
// ZipArchive
// ------------------------------------------------------------------------------------------
namespace {
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
}

ZipArchive::ZipArchive(const std::string& path) : path_(path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw FormatError("cannot open " + path);
    file.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(file.tellg());
    const uint64_t tail = std::min<uint64_t>(size, 65557);
    Bytes end(tail);
    file.seekg(std::streamoff(size - tail));
    file.read(reinterpret_cast<char*>(end.data()), std::streamsize(tail));
    size_t eocd = SIZE_MAX;
    for (size_t k = tail >= 22 ? tail - 22 + 1 : 0; k-- > 0;)
        if (le32(end.data() + k) == 0x06054b50) { eocd = k; break; }
    if (eocd == SIZE_MAX) throw FormatError(path + ": not a zip file");
    const uint16_t count = le16(end.data() + eocd + 10);
    const uint32_t cdSize = le32(end.data() + eocd + 12);
    const uint32_t cdOffset = le32(end.data() + eocd + 16);
    if (uint64_t(cdOffset) + cdSize > size) throw FormatError(path + ": bad central directory");
    Bytes cd(cdSize);
    file.seekg(std::streamoff(cdOffset));
    file.read(reinterpret_cast<char*>(cd.data()), std::streamsize(cdSize));
    size_t p = 0;
    for (uint16_t k = 0; k < count; ++k) {
        if (p + 46 > cd.size() || le32(cd.data() + p) != 0x02014b50) throw FormatError(path + ": bad central directory entry");
        ZipEntry e;
        e.versionMadeBy = le16(cd.data() + p + 4);
        e.versionNeeded = le16(cd.data() + p + 6);
        e.method = le16(cd.data() + p + 10);
        e.modTime = le16(cd.data() + p + 12);
        e.modDate = le16(cd.data() + p + 14);
        e.crc = le32(cd.data() + p + 16);
        e.compressedSize = le32(cd.data() + p + 20);
        e.size = le32(cd.data() + p + 24);
        const uint16_t nameLength = le16(cd.data() + p + 28);
        const uint16_t extraLength = le16(cd.data() + p + 30);
        const uint16_t commentLength = le16(cd.data() + p + 32);
        e.externalAttributes = le32(cd.data() + p + 38);
        e.localHeaderOffset = le32(cd.data() + p + 42);
        if (p + 46 + nameLength > cd.size()) throw FormatError(path + ": bad entry name");
        e.name.assign(reinterpret_cast<const char*>(cd.data() + p + 46), nameLength);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        entries_.push_back(std::move(e));
        p += 46 + size_t(nameLength) + extraLength + commentLength;
    }
}

Bytes ZipArchive::readStored(const ZipEntry& entry) const {
    Bytes stored(entry.compressedSize);
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream file(path_, std::ios::binary);
    if (!file) throw FormatError("cannot open " + path_);
    uint8_t header[30];
    file.seekg(std::streamoff(entry.localHeaderOffset));
    if (!file.read(reinterpret_cast<char*>(header), 30) || le32(header) != 0x04034b50)
        throw FormatError(path_ + ": bad local header for " + entry.name);
    const uint64_t dataOffset = uint64_t(entry.localHeaderOffset) + 30 + le16(header + 26) + le16(header + 28);
    file.seekg(std::streamoff(dataOffset));
    if (entry.compressedSize && !file.read(reinterpret_cast<char*>(stored.data()), std::streamsize(entry.compressedSize)))
        throw FormatError(path_ + ": cannot read " + entry.name);
    return stored;
}

Bytes ZipArchive::read(const ZipEntry& entry) const {
    Bytes stored = readStored(entry);
    if (entry.method == 0) return stored;
    if (entry.method != 8) throw FormatError(entry.name + ": unsupported zip method " + std::to_string(entry.method));
    Bytes out(entry.size);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw FormatError("zlib init failed");
    zs.next_in = stored.data();
    zs.avail_in = uInt(stored.size());
    zs.next_out = out.data();
    zs.avail_out = uInt(out.size());
    const int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) throw FormatError(entry.name + ": inflate error " + std::to_string(rc));
    return out;
}

namespace {
void put16(Bytes& out, uint32_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}
void put32(Bytes& out, uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}
}  // namespace

void writeZip(const ZipArchive& source, const std::string& destination, const std::map<std::string, Bytes>& replace,
              const std::vector<std::string>& remove) {
    struct Written {
        ZipEntry entry;
        uint32_t offset = 0;
    };
    std::vector<Written> written;
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) throw FormatError("cannot write " + destination);
    uint64_t offset = 0;
    const auto write = [&](const Bytes& b) {
        out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
        offset += b.size();
    };
    // Now as a DOS time and date, for new and replaced entries.
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const uint16_t dosTime = uint16_t((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
    const uint16_t dosDate = uint16_t(((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
    const auto emit = [&](ZipEntry e, const Bytes& stored) {
        if (offset > 0xFFFFFFFFull) throw FormatError(destination + ": archive too large");
        Bytes header;
        put32(header, 0x04034b50);
        put16(header, e.versionNeeded);
        put16(header, 0);                                   // flags: no data descriptor, no encryption
        put16(header, e.method);
        put16(header, e.modTime);
        put16(header, e.modDate);
        put32(header, e.crc);
        put32(header, e.compressedSize);
        put32(header, e.size);
        put16(header, uint32_t(e.name.size()));
        put16(header, 0);
        header.insert(header.end(), e.name.begin(), e.name.end());
        written.push_back(Written{e, uint32_t(offset)});
        write(header);
        write(stored);
    };
    const auto deflated = [&](const std::string& name, const Bytes& raw, ZipEntry e) {
        e.name = name;
        e.size = uint32_t(raw.size());
        e.crc = uint32_t(crc32(0L, raw.data(), uInt(raw.size())));
        e.modTime = dosTime;
        e.modDate = dosDate;
        e.versionNeeded = 20;
        Bytes stored(compressBound(uLong(raw.size())) + 64);
        z_stream zs{};
        if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) throw FormatError("zlib init failed");
        zs.next_in = const_cast<Bytef*>(raw.data());
        zs.avail_in = uInt(raw.size());
        zs.next_out = stored.data();
        zs.avail_out = uInt(stored.size());
        const int rc = deflate(&zs, Z_FINISH);
        const uLong produced = zs.total_out;
        deflateEnd(&zs);
        if (rc != Z_STREAM_END) throw FormatError(name + ": deflate error " + std::to_string(rc));
        stored.resize(produced);
        e.method = 8;
        e.compressedSize = uint32_t(stored.size());
        emit(e, stored);
    };
    std::set<std::string> done;
    for (const ZipEntry& e : source.entries()) {
        if (std::find(remove.begin(), remove.end(), e.name) != remove.end()) continue;
        const auto r = replace.find(e.name);
        if (r != replace.end()) deflated(e.name, r->second, e);
        else emit(e, source.readStored(e));
        done.insert(e.name);
    }
    for (const auto& r : replace)
        if (!done.count(r.first)) deflated(r.first, r.second, ZipEntry{});
    const uint64_t directoryOffset = offset;
    Bytes directory;
    for (const Written& w : written) {
        const ZipEntry& e = w.entry;
        put32(directory, 0x02014b50);
        put16(directory, e.versionMadeBy);
        put16(directory, e.versionNeeded);
        put16(directory, 0);
        put16(directory, e.method);
        put16(directory, e.modTime);
        put16(directory, e.modDate);
        put32(directory, e.crc);
        put32(directory, e.compressedSize);
        put32(directory, e.size);
        put16(directory, uint32_t(e.name.size()));
        put16(directory, 0);                                // extra
        put16(directory, 0);                                // comment
        put16(directory, 0);                                // disk
        put16(directory, 0);                                // internal attributes
        put32(directory, e.externalAttributes);
        put32(directory, w.offset);
        directory.insert(directory.end(), e.name.begin(), e.name.end());
    }
    write(directory);
    if (written.size() > 0xFFFF || offset > 0xFFFFFFFFull) throw FormatError(destination + ": archive too large");
    Bytes end;
    put32(end, 0x06054b50);
    put16(end, 0);
    put16(end, 0);
    put16(end, uint32_t(written.size()));
    put16(end, uint32_t(written.size()));
    put32(end, uint32_t(directory.size()));
    put32(end, uint32_t(directoryOffset));
    put16(end, 0);
    write(end);
    out.close();
    if (!out) throw FormatError("cannot write " + destination);
}

// ------------------------------------------------------------------------------------------
// GameData
// ------------------------------------------------------------------------------------------
GameData::GameData(const std::string& gameZipPath) : zip_(std::make_unique<ZipArchive>(gameZipPath)) { indexModules(); }

void GameData::indexModules() {
    moduleEntries_.clear();
    // Compiled modules (.pyo / .pyc) first; a module shipped only as source (.py) is evaluated by evaluateSource.
    const auto& entries = zip_->entries();
    for (int pass = 0; pass < 2; ++pass)
        for (size_t k = 0; k < entries.size(); ++k) {
            const std::string name = toLowerAscii(entries[k].name);
            const bool compiled = name.size() > 4 && (name.compare(name.size() - 4, 4, ".pyo") == 0 || name.compare(name.size() - 4, 4, ".pyc") == 0);
            const bool source = name.size() > 3 && name.compare(name.size() - 3, 3, ".py") == 0;
            if (pass == 0 ? !compiled : !source) continue;
            size_t start = 0;
            for (size_t pos = name.find("generated/"); pos != std::string::npos; pos = name.find("generated/", pos + 1))
                if (pos == 0 || name[pos - 1] == '/') { start = pos; break; }
            std::string dotted = name.substr(start, name.size() - (compiled ? 4 : 3) - start);
            std::replace(dotted.begin(), dotted.end(), '/', '.');
            moduleEntries_.emplace(dotted, k);        // a compiled module of the same name stays
        }
}

std::vector<std::string> GameData::moduleNames() const {
    std::vector<std::string> names;
    for (const auto& m : moduleEntries_) names.push_back(m.first);
    std::sort(names.begin(), names.end());
    return names;
}

bool GameData::hasModule(const std::string& module) const { return moduleEntries_.count(toLowerAscii(module)) != 0; }

const py::Object& GameData::module(const std::string& module) {
    const std::string key = toLowerAscii(module);
    std::lock_guard<std::mutex> lock(cacheMutex_);
    const auto cached = cache_.find(key);
    if (cached != cache_.end()) return *cached->second;
    const auto it = moduleEntries_.find(key);
    if (it == moduleEntries_.end()) throw FormatError("game data has no module " + module);
    const ZipEntry& entry = zip_->entries()[it->second];
    const bool source = entry.name.size() > 3 && toLowerAscii(entry.name.substr(entry.name.size() - 3)) == ".py";
    py::Ref globals = source ? evaluateSource(zip_->read(entry), module) : evaluateModule(zip_->read(entry), module);
    return *cache_.emplace(key, globals).first->second;
}

std::string GameData::moduleEntryName(const std::string& module) const {
    const auto it = moduleEntries_.find(toLowerAscii(module));
    return it == moduleEntries_.end() ? std::string() : zip_->entries()[it->second].name;
}

void GameData::reloadArchive(const std::map<std::string, py::Ref>& updated) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    zip_ = std::make_unique<ZipArchive>(zip_->path());
    indexModules();
    for (const auto& u : updated) {
        py::Ref& slot = cache_[toLowerAscii(u.first)];
        if (slot) retired_.push_back(slot);
        slot = u.second;
    }
}

const py::Object* GameData::global(const std::string& module, const std::string& variable) {
    return py::dictGet(&this->module(module), variable);
}

std::string GameData::stringById(int32_t stringId) {
    return py::asString(py::dictGet(global("generated.client.stringtable", "lookup"), stringId));
}

std::vector<std::pair<int32_t, std::string>> GameData::stringsWithExtension(const std::string& extension) {
    std::vector<std::pair<int32_t, std::string>> out;
    const py::Object* lookup = global("generated.client.stringtable", "lookup");
    if (!lookup || lookup->kind != py::Kind::Dict) return out;
    const auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string ext = lower(extension);
    for (const auto& entry : lookup->entries) {
        if (!py::isString(entry.second.get())) continue;
        const std::string s = py::asString(entry.second.get());
        if (s.size() >= ext.size() && lower(s.substr(s.size() - ext.size())) == ext) out.push_back({int32_t(py::asInt(entry.first.get())), s});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}

const char* mapScreenMarkerTypeName(int32_t type) {
    switch (type) {
        case 1: return "Control point";
        case 2: return "Waypoint";
        case 3: return "Hospital";
        case 4: return "Crafting station";
        case 5: return "Region label";
        case 6: return "Point of interest";
        case 7: return "Instance entrance";
        case 8: return "Battlefield entrance";
        case 9: return "Local teleporter";
        case 10: return "Wormhole";
        case 11: return "Dropship teleporter";
        case 15: return "Vendor";
        case 16: return "Trainer";
        case 17: return "Footlocker";
        case 18: return "Auctioneer";
        case 19: return "Safe zone";
        case 20: return "Prestige vendor";
        case 21: return "Clan footlocker";
        default: return "Marker";
    }
}

std::vector<std::pair<int32_t, std::string>> GameData::moduleConstants(const std::string& moduleName) {
    std::vector<std::pair<int32_t, std::string>> out;
    const py::Object& globals = module(moduleName);
    for (const auto& entry : globals.entries) {
        if (!py::isString(entry.first.get()) || entry.second->kind != py::Kind::Int) continue;
        const std::string name = py::asString(entry.first.get());
        if (name.empty() || name[0] == '_') continue;
        out.push_back({int32_t(entry.second->integer), name});
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::pair<int32_t, std::string>> GameData::lookupNames(const std::string& moduleName, const std::string& variable, int32_t gender,
                                                                  size_t nameIndex) {
    std::vector<std::pair<int32_t, std::string>> out;
    const py::Object* dict = global(moduleName, variable);
    if (!dict || dict->kind != py::Kind::Dict) return out;
    for (const auto& entry : dict->entries) {
        const py::Object* key = entry.first.get();
        int64_t id = 0;
        if (key->kind == py::Kind::Int) {
            id = key->integer;
        } else if (key->kind == py::Kind::Tuple && py::length(key) == 2) {
            if (py::asInt(py::item(key, 1)) != gender) continue;              // (id, gender) keys: the wanted gender only
            id = py::asInt(py::item(key, 0));
        } else {
            continue;
        }
        // The value itself when a string; else its element nameIndex, or that element's first line when it is a list of lines.
        const py::Object* v = entry.second.get();
        if (!py::isString(v)) v = py::item(v, nameIndex);
        if (v && !py::isString(v)) v = py::item(v, 0);
        out.push_back({int32_t(id), v && py::isString(v) ? py::asString(v) : std::string()});
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string GameData::effectTooltip(int32_t effectId) {
    const py::Object* tip = py::dictGet(global("generated.client.gameeffectdata", "tooltip"), effectId);
    if (!tip || py::isNone(tip)) return "";
    return py::asString(py::dictGet(global("generated.client.language.english.tooltiplanguage", "lookup"), py::asInt(tip)));
}

std::vector<ClientRecipe> GameData::clientRecipes() {
    const py::Object* data = global("generated.shared.recipe", "data");
    // A dtuple table: its rows, and the column names in "<table>.desc" (1-tuples).
    const auto table = [&](const std::string& name, std::initializer_list<const char*> wanted, std::vector<int>& indices) -> const py::Object* {
        const py::Object* desc = py::dictGet(data, name + ".desc");
        indices.clear();
        for (const char* w : wanted) {
            int found = -1;
            for (size_t k = 0; k < py::length(desc); ++k)
                if (py::asString(py::item(py::item(desc, k), 0)) == w) found = int(k);
            if (found < 0) throw FormatError("generated.shared.recipe: " + name + " has no column " + w);
            indices.push_back(found);
        }
        return py::dictGet(data, name);
    };
    std::vector<int> c;
    std::map<int32_t, ClientRecipe> recipes;
    const py::Object* rows = table("spGenShared_RecipeTemplate",
                                   {"TemplateId", "EnergyUnitCost", "KraftwerksTimeSeconds", "ResultItemTemplateId", "ResultItemAmount", "MinimumPlayerLevel"}, c);
    for (size_t k = 0; k < py::length(rows); ++k) {
        const py::Object* row = py::item(rows, k);
        const auto v = [&](int i) { return int32_t(py::asInt(py::item(row, size_t(c[size_t(i)])))); };
        ClientRecipe r{v(0), v(1), v(2), v(3), v(4), v(5), {}};
        recipes[r.templateId] = r;
    }
    rows = table("spGenShared_RecipeTemplateInputClass", {"RecipeTemplateId", "InputItemClassId", "ItemQuantityAmount"}, c);
    for (size_t k = 0; k < py::length(rows); ++k) {
        const py::Object* row = py::item(rows, k);
        const int32_t id = int32_t(py::asInt(py::item(row, size_t(c[0]))));
        const auto it = recipes.find(id);
        if (it != recipes.end())
            it->second.inputs.push_back({int32_t(py::asInt(py::item(row, size_t(c[1])))), int32_t(py::asInt(py::item(row, size_t(c[2]))))});
    }
    std::vector<ClientRecipe> out;
    for (auto& [id, r] : recipes) out.push_back(std::move(r));
    return out;
}

std::string GameData::moduleSetName(int32_t moduleId) {
    return py::asString(py::dictGet(global("generated.client.language.english.modulesetnamelanguage", "lookup"), moduleId));
}

std::vector<MapScreenMarker> GameData::factionedMapMarkers(int32_t templateId) {
    std::vector<MapScreenMarker> out;
    const py::Object* list = py::dictGet(global("generated.client.uimapmarker", "factionedmarkers"), templateId);
    const py::Object* names = global("generated.client.language.english.uimapmarkertextlanguage", "lookup");
    for (size_t k = 0; k < py::length(list); ++k) {
        const py::Object* row = py::item(list, k);
        MapScreenMarker m;
        m.entityId = py::asInt(py::item(row, 0));
        m.type = py::isNone(py::item(row, 1)) ? 0 : int32_t(py::asInt(py::item(row, 1)));
        m.nameId = py::isNone(py::item(row, 2)) ? -1 : int32_t(py::asInt(py::item(row, 2)));
        if (m.nameId >= 0) m.name = py::asString(py::dictGet(names, m.nameId));
        m.x = float(py::asFloat(py::item(row, 4)));
        m.y = float(py::asFloat(py::item(row, 5)));
        m.z = float(py::asFloat(py::item(row, 6)));
        out.push_back(std::move(m));
    }
    return out;
}

std::string GameData::className(int32_t classId) {
    return py::asString(py::item(py::dictGet(global("generated.client.entityclass", "lookup"), classId), 0));
}

int32_t GameData::classMeshId(int32_t classId) {
    const py::Object* mesh = py::item(py::dictGet(global("generated.client.entityclass", "lookup"), classId), 1);
    return py::isInt(mesh) ? int32_t(py::asInt(mesh)) : -1;
}

std::string GameData::classMeshName(int32_t classId) {
    const int32_t meshId = classMeshId(classId);
    return meshId < 0 ? "" : stringById(meshId);
}

int32_t GameData::classCullingLayer(int32_t classId) {
    const py::Object* layer = py::item(py::dictGet(global("generated.client.vizsetting", "lookup"), classId), 2);
    return py::isInt(layer) ? int32_t(py::asInt(layer)) : 3;
}

bool GameData::classHue(int32_t classId, HueColour& hue1, HueColour& hue2) {
    const py::Object* row = py::dictGet(global("generated.client.physicalentityclasshue", "lookup"), classId);
    if (py::length(row) < 8) return false;
    const auto byte = [&](size_t k) { return uint8_t(py::asInt(py::item(row, k), 0) & 0xFF); };
    hue1 = HueColour{byte(0), byte(1), byte(2), byte(3)};
    hue2 = HueColour{byte(4), byte(5), byte(6), byte(7)};
    return true;
}

std::string GameData::usableStatePackage(int32_t classId, int32_t stateId) {
    const py::Object* id = py::dictGetTuple(global("generated.client.usabledata", "specialFX"), {classId, stateId, 97});
    if (!py::isInt(id)) return "";
    return stringById(int32_t(py::asInt(id)));
}

GameData::UsableAnimation GameData::usableStateAnimation(int32_t classId, int32_t state0, int32_t state1) {
    UsableAnimation out;
    const py::Object* list = py::dictGetTuple(global("generated.client.usabledata", "animation"), {classId, state0, state1});
    const py::Object* entry = py::length(list) > 0 ? py::item(list, 0) : nullptr;
    if (!entry || !py::isInt(py::item(entry, 0))) return out;
    const int32_t specId = int32_t(py::asInt(py::item(entry, 0)));
    const py::Object* overrideType = py::item(entry, 1);
    const py::Object* spec = py::dictGet(global("generated.client.animationdata", "objectAnimationSpecification"), specId);
    if (!spec || !py::isInt(py::item(spec, 2))) return out;
    out.type = py::isInt(overrideType) && py::asInt(overrideType) != 0 ? int32_t(py::asInt(overrideType))
                                                                        : py::isInt(py::item(spec, 1)) ? int32_t(py::asInt(py::item(spec, 1))) : 0;
    const int32_t familyId = int32_t(py::asInt(py::item(spec, 2)));
    if (py::isInt(py::item(spec, 3))) out.delayLower = int32_t(py::asInt(py::item(spec, 3)));
    if (py::isInt(py::item(spec, 4))) out.delayUpper = int32_t(py::asInt(py::item(spec, 4)));
    const int32_t meshId = classMeshId(classId);
    const py::Object* animations = py::dictGetTuple(global("generated.client.animationdata", "animationFamilyObject"), {familyId, meshId});
    for (size_t k = 0; k < py::length(animations); ++k) {
        const py::Object* a = py::item(animations, k);
        if (!py::isInt(py::item(a, 0))) continue;
        FamilyAnimation f;
        f.name = stringById(int32_t(py::asInt(py::item(a, 0))));
        f.frequency = py::isInt(py::item(a, 1)) ? int32_t(py::asInt(py::item(a, 1))) : 1;
        if (!f.name.empty()) out.animations.push_back(std::move(f));
    }
    return out;
}

std::string GameData::mapTemplateName(int32_t templateId) {
    return py::asString(py::dictGet(global("generated.client.maptemplate", "lookup"), templateId));
}

std::vector<int32_t> GameData::contextIdsForMapTemplate(int32_t templateId) {
    std::vector<int32_t> ids;
    const py::Object* lookup = global("generated.client.gamecontext", "lookup");
    if (lookup == nullptr || lookup->kind != py::Kind::Dict) return ids;
    for (const auto& entry : lookup->entries)
        if (py::asInt(py::item(entry.second.get(), 3), -1) == templateId) ids.push_back(int32_t(py::asInt(entry.first.get())));
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool GameData::radarInfo(int32_t contextId, int32_t regionId, RadarInfo& info) {
    const py::Object* entry = py::item(py::dictGetTuple(global("generated.client.gamecontextuiradarinfo", "lookup"), {contextId, regionId}), 0);
    if (py::length(entry) < 4) return false;
    info.extentX = int32_t(py::asInt(py::item(entry, 0)));
    info.extentZ = int32_t(py::asInt(py::item(entry, 1)));
    info.offsetX = int32_t(py::asInt(py::item(entry, 2)));
    info.offsetZ = int32_t(py::asInt(py::item(entry, 3)));
    return info.extentX > 0 && info.extentZ > 0;
}

std::string GameData::creatureName(int32_t nameId) {
    const py::Object* name = py::dictGet(global("generated.client.language.english.creaturenamelanguage", "lookup"), nameId);
    return name && py::isString(name) ? py::asString(name) : "";
}

std::vector<int32_t> GameData::battlecryPackages() {
    std::vector<int32_t> packages;
    const py::Object* lookup = global("generated.client.battlecrypackages", "lookup");
    if (lookup && lookup->kind == py::Kind::Dict)
        for (const auto& entry : lookup->entries) packages.push_back(int32_t(py::asInt(py::item(entry.first.get(), 0))));
    std::sort(packages.begin(), packages.end());
    packages.erase(std::unique(packages.begin(), packages.end()), packages.end());
    return packages;
}

std::vector<GameData::BattlecryCry> GameData::battlecryCries(int32_t packageId) {
    std::vector<BattlecryCry> cries;
    const py::Object* lookup = global("generated.client.battlecrypackages", "lookup");
    if (!lookup || lookup->kind != py::Kind::Dict) return cries;
    for (const auto& entry : lookup->entries) {
        if (py::asInt(py::item(entry.first.get(), 0)) != packageId) continue;
        BattlecryCry cry;
        cry.type = int32_t(py::asInt(py::item(entry.first.get(), 1)));
        cry.audioSet = int32_t(py::asInt(py::item(entry.second.get(), 0)));
        cry.audioSetName = audioSetName(cry.audioSet);
        cries.push_back(std::move(cry));
    }
    std::sort(cries.begin(), cries.end(), [](const BattlecryCry& a, const BattlecryCry& b) { return a.type < b.type; });
    return cries;
}

std::vector<GameData::AudioSetSound> GameData::audioSetSounds(int32_t audioSetId) {
    std::vector<AudioSetSound> sounds;
    const py::Object* members = global("generated.client.audiosetdata", "audioSetMember");
    const py::Object* table = global("generated.client.audiosetdata", "sound");
    if (!members || members->kind != py::Kind::Dict) return sounds;
    for (const auto& entry : members->entries) {
        if (py::asInt(py::item(entry.first.get(), 0)) != audioSetId) continue;
        AudioSetSound s;
        s.soundId = int32_t(py::asInt(py::item(entry.first.get(), 1)));
        const py::Object* sound = py::dictGet(table, s.soundId);
        s.file = py::asString(py::item(sound, 0));
        s.description = py::asString(py::item(sound, 1));
        sounds.push_back(std::move(s));
    }
    return sounds;
}

int32_t GameData::audioSetSelectionType(int32_t audioSetId) {
    return int32_t(py::asInt(py::item(py::dictGet(global("generated.client.audiosetdata", "audioSet"), audioSetId), 3), 0));
}

std::string GameData::itemName(int32_t templateId, int32_t classId) {
    const py::Object* name = py::dictGet(global("generated.client.language.english.itemtemplatelanguage", "lookup"), templateId);
    if (name && py::isString(name)) return py::asString(name);
    name = py::dictGet(global("generated.client.language.english.physicalentityclassnamelanguage", "lookup"), classId);
    return name && py::isString(name) ? py::asString(name) : "";
}

std::string GameData::audioSetName(int32_t audioSetId) {
    return py::asString(py::item(py::dictGet(global("generated.client.audiosetdata", "audioSet"), audioSetId), 0));
}

std::string GameData::regionDisplayName(int32_t displayName, int32_t gender) {
    const std::string name = py::asString(py::dictGet(global("generated.client.uielement", "byvalue"), displayName));
    if (name.empty()) return "";
    return uiText(name, gender);
}

std::string GameData::uiText(const std::string& name, int32_t gender) {
    const int64_t index = py::asInt(py::dictGet(global("generated.client.uielement", "bystring"), name), 0);
    if (index == 0) return "";
    const py::Object* lookup = global("generated.client.language.english.uielementlanguage", "lookup");
    const py::Object* lines = py::dictGetTuple(lookup, {index, gender});
    const py::Object* line = py::length(lines) > 0 ? py::item(lines, 0) : py::dictGet(lookup, index);
    return line && py::isString(line) ? py::asString(line) : "";
}

bool GameData::uiTextExists(const std::string& name, int32_t gender, std::string* text) {
    const int64_t index = py::asInt(py::dictGet(global("generated.client.uielement", "bystring"), name), 0);
    if (index == 0) return false;
    const py::Object* lookup = global("generated.client.language.english.uielementlanguage", "lookup");
    const py::Object* lines = py::dictGetTuple(lookup, {index, gender});
    const py::Object* line = py::length(lines) > 0 ? py::item(lines, 0) : py::dictGet(lookup, index);
    if (!line || !py::isString(line)) return false;
    if (text) *text = py::asString(line);
    return true;
}

std::string GameData::missionText(int32_t textId, int32_t gender) {
    const py::Object* lines = py::dictGetTuple(global("generated.client.language.english.missiontextlanguage", "lookup"), {textId, gender});
    const py::Object* line = py::length(lines) > 0 ? py::item(lines, 0) : nullptr;
    return line && py::isString(line) ? py::asString(line) : "";
}

std::string GameData::npcGreeting(int32_t greetingId, int32_t gender) {
    const py::Object* lines = py::dictGetTuple(global("generated.client.language.english.npcgreetinglanguage", "lookup"), {greetingId, gender});
    const py::Object* line = py::length(lines) > 0 ? py::item(lines, 0) : nullptr;
    return line && py::isString(line) ? py::asString(line) : "";
}

std::vector<SwapsetPart> GameData::itemSwapsetParts(int32_t itemClassId, int32_t baseMeshId) {
    std::vector<SwapsetPart> parts;
    const py::Object* list = py::dictGetTuple(global("generated.client.equipmentdata", "equipableClassMeshSwapset"), {itemClassId, baseMeshId});
    for (size_t k = 0; k < py::length(list); ++k) {
        const py::Object* pair = py::item(list, k);
        SwapsetPart part;
        part.suitId = int32_t(py::asInt(py::item(pair, 0)));
        part.swapsetId = int32_t(py::asInt(py::item(pair, 1)));
        parts.push_back(part);
    }
    return parts;
}

int32_t GameData::itemEquipmentSlot(int32_t itemClassId) {
    const py::Object* slot = py::dictGet(global("generated.client.equipmentdata", "equipableClassEquipmentSlot"), itemClassId);
    return py::isInt(slot) ? int32_t(py::asInt(slot)) : -1;
}

std::vector<int32_t> GameData::itemSlotExclusions(int32_t itemClassId) {
    std::vector<int32_t> slots;
    const py::Object* list = py::dictGet(global("generated.client.equipmentdata", "equipableClassEquipmentSlotExclusion"), itemClassId);
    for (size_t k = 0; k < py::length(list); ++k)
        if (py::isInt(py::item(list, k))) slots.push_back(int32_t(py::asInt(py::item(list, k))));
    return slots;
}

std::vector<AccessoryAttachment> GameData::itemAccessories(int32_t itemClassId, int32_t weaponPosition) {
    std::vector<AccessoryAttachment> out;
    const py::Object* list = py::dictGetTuple(global("generated.client.equipmentdata", "equipableClassAccessoryConnectionPointWeaponOverride"),
                                              {itemClassId, weaponPosition});
    if (!list) list = py::dictGet(global("generated.client.equipmentdata", "equipableClassAccessoryConnectionPoint"), itemClassId);
    for (size_t k = 0; k < py::length(list); ++k) {
        const py::Object* pair = py::item(list, k);
        AccessoryAttachment a;
        a.mesh = stringById(int32_t(py::asInt(py::item(pair, 0))));
        a.connectionPoint = int32_t(py::asInt(py::item(pair, 1)));
        if (!a.mesh.empty()) out.push_back(a);
    }
    return out;
}

std::string GameData::suitName(int32_t suitId) {
    return py::asString(py::item(py::dictGet(global("generated.client.equipmentdata", "suit"), suitId), 0));
}

std::string GameData::suitMeshName(int32_t suitId) {
    const py::Object* mesh = py::item(py::dictGet(global("generated.client.equipmentdata", "suit"), suitId), 1);
    return py::isInt(mesh) ? stringById(int32_t(py::asInt(mesh))) : "";
}

std::string GameData::swapsetName(int32_t swapsetId) {
    return py::asString(py::dictGet(global("generated.client.equipmentdata", "swapset"), swapsetId));
}

int32_t GameData::meshSkeletonId(int32_t meshId) {
    const py::Object* id = py::dictGet(global("generated.client.animationdata", "meshSkeleton"), meshId);
    return py::isInt(id) ? int32_t(py::asInt(id)) : -1;
}

std::vector<int32_t> GameData::meshStringIds(const std::string& meshName) {
    std::vector<int32_t> out;
    const auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string want = lower(meshName);
    const py::Object* lookup = global("generated.client.stringtable", "lookup");
    if (!lookup || lookup->kind != py::Kind::Dict) return out;
    for (const auto& entry : lookup->entries)
        if (py::isString(entry.second.get()) && lower(py::asString(entry.second.get())) == want) out.push_back(int32_t(py::asInt(entry.first.get())));
    std::sort(out.begin(), out.end());
    return out;
}

std::string GameData::animationFamilyName(int32_t familyId) {
    const py::Object* name = py::dictGet(global("generated.client.animationdata", "animationFamily"), familyId);
    return py::isString(name) ? py::asString(name) : std::string();
}

namespace {
// The (family, second) tuple keys of an animationdata dict whose second element is `second`, as (family id, family name).
std::vector<std::pair<int32_t, std::string>> familiesKeyedBy(GameData& data, const char* variable, int32_t second) {
    std::vector<std::pair<int32_t, std::string>> out;
    const py::Object* dict = data.global("generated.client.animationdata", variable);
    if (!dict || dict->kind != py::Kind::Dict) return out;
    for (const auto& entry : dict->entries) {
        const py::Object* key = entry.first.get();
        if (key->kind != py::Kind::Tuple || py::length(key) != 2 || !py::isInt(py::item(key, 0)) || py::asInt(py::item(key, 1)) != second) continue;
        const int32_t family = int32_t(py::asInt(py::item(key, 0)));
        out.push_back({family, data.animationFamilyName(family)});
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
}  // namespace

std::vector<std::pair<int32_t, std::string>> GameData::skeletonFamilies(int32_t skeletonId) {
    return familiesKeyedBy(*this, "animationFamilyData", skeletonId);
}

std::vector<std::pair<int32_t, std::string>> GameData::objectFamilies(int32_t meshId) { return familiesKeyedBy(*this, "animationFamilyObject", meshId); }

std::vector<GameData::FamilyAnimation> GameData::familyObjectAnimations(int32_t familyId, int32_t meshId) {
    std::vector<FamilyAnimation> out;
    const py::Object* list = py::dictGetTuple(global("generated.client.animationdata", "animationFamilyObject"), {familyId, meshId});
    for (size_t k = 0; k < py::length(list); ++k) {
        const py::Object* a = py::item(list, k);
        if (!py::isInt(py::item(a, 0))) continue;
        FamilyAnimation f;
        f.name = stringById(int32_t(py::asInt(py::item(a, 0))));
        f.frequency = py::isInt(py::item(a, 1)) ? int32_t(py::asInt(py::item(a, 1))) : 1;
        if (!f.name.empty()) out.push_back(std::move(f));
    }
    return out;
}

size_t GameData::skeletonMeshCount(int32_t skeletonId) {
    size_t count = 0;
    const py::Object* dict = global("generated.client.animationdata", "meshSkeleton");
    if (!dict || dict->kind != py::Kind::Dict) return 0;
    for (const auto& entry : dict->entries) count += py::isInt(entry.second.get()) && py::asInt(entry.second.get()) == skeletonId ? 1 : 0;
    return count;
}

std::vector<int32_t> GameData::suitBaseMeshes(int32_t meshId) {
    std::vector<int32_t> out;
    const py::Object* suits = global("generated.client.equipmentdata", "suit");
    const py::Object* swaps = global("generated.client.equipmentdata", "equipableClassMeshSwapset");
    if (!suits || suits->kind != py::Kind::Dict || !swaps || swaps->kind != py::Kind::Dict) return out;
    std::vector<int64_t> suitIds;                     // the suits made of this mesh
    for (const auto& entry : suits->entries) {
        const py::Object* mesh = py::item(entry.second.get(), 1);
        if (py::isInt(mesh) && py::asInt(mesh) == meshId && py::isInt(entry.first.get())) suitIds.push_back(py::asInt(entry.first.get()));
    }
    if (suitIds.empty()) return out;
    for (const auto& entry : swaps->entries) {
        const py::Object* key = entry.first.get();
        if (key->kind != py::Kind::Tuple || py::length(key) != 2 || !py::isInt(py::item(key, 1))) continue;
        for (size_t k = 0; k < py::length(entry.second.get()); ++k) {
            const py::Object* suit = py::item(py::item(entry.second.get(), k), 0);
            if (py::isInt(suit) && std::find(suitIds.begin(), suitIds.end(), py::asInt(suit)) != suitIds.end())
                out.push_back(int32_t(py::asInt(py::item(key, 1))));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

namespace {
// The (a, b) tuple keys of an animationdata dict whose list of (animation string id, ...) entries names the animation.
std::vector<std::pair<int32_t, int32_t>> keysPlaying(GameData& data, const char* variable, int32_t animationStringId) {
    std::vector<std::pair<int32_t, int32_t>> out;
    const py::Object* dict = data.global("generated.client.animationdata", variable);
    if (!dict || dict->kind != py::Kind::Dict) return out;
    for (const auto& entry : dict->entries) {
        const py::Object* key = entry.first.get();
        if (key->kind != py::Kind::Tuple || py::length(key) != 2 || !py::isInt(py::item(key, 0)) || !py::isInt(py::item(key, 1))) continue;
        for (size_t k = 0; k < py::length(entry.second.get()); ++k) {
            const py::Object* id = py::item(py::item(entry.second.get(), k), 0);
            if (py::isInt(id) && py::asInt(id) == animationStringId) {
                out.push_back({int32_t(py::asInt(py::item(key, 0))), int32_t(py::asInt(py::item(key, 1)))});
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

std::vector<std::pair<int32_t, int32_t>> GameData::familiesPlaying(int32_t animationStringId) {
    return keysPlaying(*this, "animationFamilyData", animationStringId);
}

std::vector<std::pair<int32_t, int32_t>> GameData::objectFamiliesPlaying(int32_t animationStringId) {
    return keysPlaying(*this, "animationFamilyObject", animationStringId);
}

std::vector<int32_t> GameData::skeletonMeshes(int32_t skeletonId) {
    std::vector<int32_t> out;
    const py::Object* dict = global("generated.client.animationdata", "meshSkeleton");
    if (!dict || dict->kind != py::Kind::Dict) return out;
    for (const auto& entry : dict->entries)
        if (py::isInt(entry.first.get()) && py::isInt(entry.second.get()) && py::asInt(entry.second.get()) == skeletonId)
            out.push_back(int32_t(py::asInt(entry.first.get())));
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<GameData::FamilyAnimation> GameData::familyAnimations(int32_t familyId, int32_t skeletonId) {
    std::vector<FamilyAnimation> animations;
    const py::Object* list = py::dictGetTuple(global("generated.client.animationdata", "animationFamilyData"), {familyId, skeletonId});
    for (size_t k = 0; k < py::length(list); ++k) {
        const py::Object* entry = py::item(list, k);
        const py::Object* animation = py::item(entry, 0);
        const py::Object* frequency = py::item(entry, 1);
        if (!py::isInt(animation)) continue;
        FamilyAnimation a;
        a.name = stringById(int32_t(py::asInt(animation)));
        a.frequency = py::isInt(frequency) ? int32_t(py::asInt(frequency)) : 1;
        if (!a.name.empty()) animations.push_back(std::move(a));     // _FilterAnimList drops ids the string table lacks
    }
    return animations;
}

}  // namespace tr
