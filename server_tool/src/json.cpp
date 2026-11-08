#include "json.h"

#include <cstdlib>

namespace json {

const Value& Value::operator[](const std::string& key) const {
    static const Value null;
    if (type != Type::Object) return null;
    auto it = o.find(key);
    return it == o.end() ? null : it->second;
}

namespace {

struct Reader {
    const std::string& t;
    size_t i = 0;
    std::string err;
    int depth = 0;

    void Space() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
    }
    bool Fail(const char* what) {
        if (err.empty()) err = std::string(what) + " at " + std::to_string(i);
        return false;
    }
    bool Word(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (t.compare(i, n, w) != 0) return Fail("unexpected word");
        i += n;
        return true;
    }
    bool String(std::string& out) {
        if (i >= t.size() || t[i] != '"') return Fail("expected a string");
        i++;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size()) return Fail("unfinished escape");
            char e = t[i++];
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
                if (i + 4 > t.size()) return Fail("short \\u escape");
                unsigned v = unsigned(std::strtoul(t.substr(i, 4).c_str(), nullptr, 16));
                i += 4;
                if (v < 0x80) out += char(v);
                else if (v < 0x800) out += char(0xC0 | (v >> 6)), out += char(0x80 | (v & 0x3F));
                else out += char(0xE0 | (v >> 12)), out += char(0x80 | ((v >> 6) & 0x3F)), out += char(0x80 | (v & 0x3F));
                break;
            }
            default: return Fail("bad escape");
            }
        }
        if (i >= t.size()) return Fail("unfinished string");
        i++;
        return true;
    }
    bool Val(Value& v) {
        if (++depth > 64) return Fail("too deep");
        Space();
        if (i >= t.size()) return Fail("unexpected end");
        char c = t[i];
        bool ok = true;
        if (c == '{') {
            v.type = Value::Type::Object;
            i++;
            Space();
            if (i < t.size() && t[i] == '}') i++;
            else
                for (;;) {
                    Space();
                    std::string k;
                    if (!String(k)) return false;
                    Space();
                    if (i >= t.size() || t[i] != ':') return Fail("expected :");
                    i++;
                    if (!Val(v.o[k])) return false;
                    Space();
                    if (i < t.size() && t[i] == ',') { i++; continue; }
                    if (i < t.size() && t[i] == '}') { i++; break; }
                    return Fail("expected , or }");
                }
        } else if (c == '[') {
            v.type = Value::Type::Array;
            i++;
            Space();
            if (i < t.size() && t[i] == ']') i++;
            else
                for (;;) {
                    v.a.emplace_back();
                    if (!Val(v.a.back())) return false;
                    Space();
                    if (i < t.size() && t[i] == ',') { i++; continue; }
                    if (i < t.size() && t[i] == ']') { i++; break; }
                    return Fail("expected , or ]");
                }
        } else if (c == '"') {
            v.type = Value::Type::String;
            ok = String(v.s);
        } else if (c == 't') {
            v.type = Value::Type::Bool, v.b = true;
            ok = Word("true");
        } else if (c == 'f') {
            v.type = Value::Type::Bool, v.b = false;
            ok = Word("false");
        } else if (c == 'n') {
            ok = Word("null");
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = t.c_str() + i;
            char* end = nullptr;
            v.type = Value::Type::Number;
            v.n = std::strtod(start, &end);
            if (end == start) return Fail("bad number");
            i += size_t(end - start);
        } else {
            return Fail("unexpected character");
        }
        depth--;
        return ok;
    }
};

}  // namespace

bool Parse(const std::string& text, Value& out, std::string* error) {
    Reader r{text};
    out = Value();
    bool ok = r.Val(out);
    if (ok) {
        r.Space();
        if (r.i != text.size()) ok = r.Fail("trailing text");
    }
    if (!ok && error) *error = r.err;
    return ok;
}

}  // namespace json
