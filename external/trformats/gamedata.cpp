// gamedata.cpp - see gamedata.h.
#include "gamedata.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
// ZipArchive
// ------------------------------------------------------------------------------------------
namespace {
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
}

ZipArchive::ZipArchive(const std::string& path) : path_(path), file_(path, std::ios::binary) {
    if (!file_) throw FormatError("cannot open " + path);
    file_.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(file_.tellg());
    const uint64_t tail = std::min<uint64_t>(size, 65557);
    Bytes end(tail);
    file_.seekg(std::streamoff(size - tail));
    file_.read(reinterpret_cast<char*>(end.data()), std::streamsize(tail));
    size_t eocd = SIZE_MAX;
    for (size_t k = tail >= 22 ? tail - 22 + 1 : 0; k-- > 0;)
        if (le32(end.data() + k) == 0x06054b50) { eocd = k; break; }
    if (eocd == SIZE_MAX) throw FormatError(path + ": not a zip file");
    const uint16_t count = le16(end.data() + eocd + 10);
    const uint32_t cdSize = le32(end.data() + eocd + 12);
    const uint32_t cdOffset = le32(end.data() + eocd + 16);
    if (uint64_t(cdOffset) + cdSize > size) throw FormatError(path + ": bad central directory");
    Bytes cd(cdSize);
    file_.seekg(std::streamoff(cdOffset));
    file_.read(reinterpret_cast<char*>(cd.data()), std::streamsize(cdSize));
    size_t p = 0;
    for (uint16_t k = 0; k < count; ++k) {
        if (p + 46 > cd.size() || le32(cd.data() + p) != 0x02014b50) throw FormatError(path + ": bad central directory entry");
        ZipEntry e;
        e.method = le16(cd.data() + p + 10);
        e.compressedSize = le32(cd.data() + p + 20);
        e.size = le32(cd.data() + p + 24);
        const uint16_t nameLength = le16(cd.data() + p + 28);
        const uint16_t extraLength = le16(cd.data() + p + 30);
        const uint16_t commentLength = le16(cd.data() + p + 32);
        e.localHeaderOffset = le32(cd.data() + p + 42);
        if (p + 46 + nameLength > cd.size()) throw FormatError(path + ": bad entry name");
        e.name.assign(reinterpret_cast<const char*>(cd.data() + p + 46), nameLength);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        entries_.push_back(std::move(e));
        p += 46 + size_t(nameLength) + extraLength + commentLength;
    }
}

Bytes ZipArchive::read(const ZipEntry& entry) const {
    Bytes stored(entry.compressedSize);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint8_t header[30];
        file_.clear();
        file_.seekg(std::streamoff(entry.localHeaderOffset));
        if (!file_.read(reinterpret_cast<char*>(header), 30) || le32(header) != 0x04034b50)
            throw FormatError(path_ + ": bad local header for " + entry.name);
        const uint64_t dataOffset = uint64_t(entry.localHeaderOffset) + 30 + le16(header + 26) + le16(header + 28);
        file_.seekg(std::streamoff(dataOffset));
        if (entry.compressedSize && !file_.read(reinterpret_cast<char*>(stored.data()), std::streamsize(entry.compressedSize)))
            throw FormatError(path_ + ": cannot read " + entry.name);
    }
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

// ------------------------------------------------------------------------------------------
// GameData
// ------------------------------------------------------------------------------------------
GameData::GameData(const std::string& gameZipPath) : zip_(gameZipPath) {
    const auto& entries = zip_.entries();
    for (size_t k = 0; k < entries.size(); ++k) {
        const std::string name = toLowerAscii(entries[k].name);
        if (name.size() < 5 || (name.compare(name.size() - 4, 4, ".pyo") != 0 && name.compare(name.size() - 4, 4, ".pyc") != 0)) continue;
        size_t start = 0;
        for (size_t pos = name.find("generated/"); pos != std::string::npos; pos = name.find("generated/", pos + 1))
            if (pos == 0 || name[pos - 1] == '/') { start = pos; break; }
        std::string dotted = name.substr(start, name.size() - 4 - start);
        std::replace(dotted.begin(), dotted.end(), '/', '.');
        moduleEntries_.emplace(dotted, k);
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
    py::Ref globals = evaluateModule(zip_.read(zip_.entries()[it->second]), module);
    return *cache_.emplace(key, globals).first->second;
}

const py::Object* GameData::global(const std::string& module, const std::string& variable) {
    return py::dictGet(&this->module(module), variable);
}

std::string GameData::stringById(int32_t stringId) {
    return py::asString(py::dictGet(global("generated.client.stringtable", "lookup"), stringId));
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

std::vector<std::string> GameData::familyAnimations(int32_t skeletonId, int32_t familyId) {
    std::vector<std::string> names;
    const py::Object* list = py::dictGetTuple(global("generated.client.animationdata", "animationFamilyData"), {skeletonId, familyId});
    for (size_t k = 0; k < py::length(list); ++k) {
        const py::Object* animation = py::item(py::item(list, k), 0);
        if (py::isInt(animation)) names.push_back(stringById(int32_t(py::asInt(animation))));
    }
    return names;
}

}  // namespace tr
