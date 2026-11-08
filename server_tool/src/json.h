// A small JSON reader for the server's telemetry lines (docs/server_tool.md). Objects, arrays, strings (with \uXXXX
// escapes below 0x100, which is all the server writes), numbers, true, false and null. No writer.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    bool IsNull() const { return type == Type::Null; }
    const Value& operator[](const std::string& key) const;  // a missing key (or not an object): a null value
    double Num(double fallback = 0) const { return type == Type::Number ? n : fallback; }
    std::string Str(const std::string& fallback = {}) const { return type == Type::String ? s : fallback; }
    bool Bool(bool fallback = false) const { return type == Type::Bool ? b : fallback; }
};

// false when text is not one JSON value (trailing spaces allowed); error says where
bool Parse(const std::string& text, Value& out, std::string* error = nullptr);

}  // namespace json
