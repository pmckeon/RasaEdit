#pragma once

// A small JSON reader for the documents the World database stores in text columns (Rasa.NET's mission channel radio sources,
// mission item intents, mission scene bindings). Read-only: objects keep their member order; numbers keep their text so
// 64-bit ids survive.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct JsonValue {
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::string text;                                            // a string's value, or a number's literal
    std::vector<JsonValue> items;                                // array elements
    std::vector<std::pair<std::string, JsonValue>> members;      // object members, in document order

    bool isNull() const { return type == Type::Null; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }
    const JsonValue* member(const std::string& name) const;     // nullptr if absent (or not an object)
    double number() const;                                       // 0 unless a number
    bool integer(int64_t& out) const;                            // a number that is a whole value
};

// Parses a whole document; false with `error` set (what and where) when it is not well-formed JSON.
bool parseJson(const std::string& textIn, JsonValue& out, std::string& error);
