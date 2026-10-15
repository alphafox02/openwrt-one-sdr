// SPDX-License-Identifier: MIT
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

constexpr unsigned kMaxDepth = 64;

struct Reader {
    const std::string &text;
    size_t at = 0;

    [[noreturn]] void fail(const char *what) const
    {
        throw std::runtime_error(std::string("bad JSON: ") + what + " at " + std::to_string(at));
    }
    void space()
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r'))
            ++at;
    }
    bool take(char c)
    {
        space();
        if (at < text.size() && text[at] == c) {
            ++at;
            return true;
        }
        return false;
    }
    void expect(char c)
    {
        if (!take(c)) fail("unexpected character");
    }
    bool word(const char *w)
    {
        size_t n = std::char_traits<char>::length(w);
        if (text.compare(at, n, w) != 0) return false;
        at += n;
        return true;
    }
    static void utf8(std::string &out, unsigned code)
    {
        if (code < 0x80) {
            out += char(code);
        } else if (code < 0x800) {
            out += char(0xC0 | code >> 6);
            out += char(0x80 | (code & 63));
        } else if (code < 0x10000) {
            out += char(0xE0 | code >> 12);
            out += char(0x80 | (code >> 6 & 63));
            out += char(0x80 | (code & 63));
        } else {
            out += char(0xF0 | code >> 18);
            out += char(0x80 | (code >> 12 & 63));
            out += char(0x80 | (code >> 6 & 63));
            out += char(0x80 | (code & 63));
        }
    }
    unsigned hex4()
    {
        if (at + 4 > text.size()) fail("short \\u escape");
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text[at++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') code |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= unsigned(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return code;
    }
    std::string string()
    {
        expect('"');
        std::string out;
        while (at < text.size() && text[at] != '"') {
            char c = text[at++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= text.size()) fail("unterminated escape");
            char e = text[at++];
            switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned code = hex4();
                if (code >= 0xD800 && code < 0xDC00 && text.compare(at, 2, "\\u") == 0) {
                    at += 2;
                    unsigned low = hex4();
                    if (low < 0xDC00 || low >= 0xE000) fail("bad surrogate pair");
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                utf8(out, code);
                break;
            }
            default: fail("unknown escape");
            }
        }
        if (!take('"')) fail("unterminated string");
        return out;
    }
    double number()
    {
        // The JSON grammar, checked before strtod (which would also take hex, inf and nan).
        const size_t start = at;
        auto digits = [&] {
            size_t from = at;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
            return at > from;
        };
        if (at < text.size() && text[at] == '-') ++at;
        if (!digits()) fail("bad number");
        if (at < text.size() && text[at] == '.') {
            ++at;
            if (!digits()) fail("bad number");
        }
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
            ++at;
            if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
            if (!digits()) fail("bad number");
        }
        double v = std::strtod(text.substr(start, at - start).c_str(), nullptr);
        if (!std::isfinite(v)) fail("number out of range");
        return v;
    }
    JsonValue value(unsigned depth)
    {
        if (depth > kMaxDepth) fail("nested too deeply");
        space();
        JsonValue v;
        if (at >= text.size()) fail("missing value");
        char c = text[at];
        if (c == '"') {
            v.kind = JsonValue::String;
            v.string = string();
        } else if (c == '{') {
            ++at;
            v.kind = JsonValue::Object;
            if (!take('}')) {
                do {
                    space();
                    v.keys.push_back(string());
                    expect(':');
                    v.items.push_back(value(depth + 1));
                } while (take(','));
                expect('}');
            }
        } else if (c == '[') {
            ++at;
            v.kind = JsonValue::Array;
            if (!take(']')) {
                do v.items.push_back(value(depth + 1));
                while (take(','));
                expect(']');
            }
        } else if (word("true")) {
            v.kind = JsonValue::Boolean;
            v.boolean = true;
        } else if (word("false")) {
            v.kind = JsonValue::Boolean;
        } else if (word("null")) {
            v.kind = JsonValue::Null;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            v.kind = JsonValue::Number;
            v.number = number();
        } else {
            fail("unsupported value");
        }
        return v;
    }
};

void write(const JsonValue &v, std::string &out)
{
    switch (v.kind) {
    case JsonValue::Null: out += "null"; break;
    case JsonValue::Boolean: out += v.boolean ? "true" : "false"; break;
    case JsonValue::Number: out += json_number(v.number); break;
    case JsonValue::String: out += json_quote(v.string); break;
    case JsonValue::Array:
        out += '[';
        for (size_t i = 0; i < v.items.size(); ++i) {
            if (i) out += ',';
            write(v.items[i], out);
        }
        out += ']';
        break;
    case JsonValue::Object:
        out += '{';
        for (size_t i = 0; i < v.items.size(); ++i) {
            if (i) out += ',';
            out += json_quote(v.keys[i]);
            out += ':';
            write(v.items[i], out);
        }
        out += '}';
        break;
    }
}

}  // namespace

const JsonValue *JsonValue::find(const std::string &key) const
{
    if (kind != Object) return nullptr;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return &items[i];
    return nullptr;
}

const JsonValue *JsonValue::at(const std::string &path) const
{
    const JsonValue *v = this;
    size_t from = 0;
    while (v) {
        size_t dot = path.find('.', from);
        v = v->find(path.substr(from, dot == std::string::npos ? std::string::npos : dot - from));
        if (dot == std::string::npos) return v;
        from = dot + 1;
    }
    return nullptr;
}

bool JsonValue::has(const std::string &key) const
{
    const JsonValue *v = find(key);
    return v && !v->is_null();
}

double JsonValue::number_or(const std::string &key, double fallback) const
{
    const JsonValue *v = find(key);
    return v && v->is_number() ? v->number : fallback;
}

std::string JsonValue::string_or(const std::string &key, const std::string &fallback) const
{
    const JsonValue *v = find(key);
    return v && v->is_string() ? v->string : fallback;
}

bool JsonValue::boolean_or(const std::string &key, bool fallback) const
{
    const JsonValue *v = find(key);
    return v && v->is_boolean() ? v->boolean : fallback;
}

JsonValue parse_json(const std::string &text)
{
    Reader r{text};
    JsonValue v = r.value(0);
    r.space();
    if (r.at != text.size()) r.fail("trailing characters");
    return v;
}

std::string json_write(const JsonValue &value)
{
    std::string out;
    write(value, out);
    return out;
}

std::string json_quote(const std::string &text)
{
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", c);
            out += escape;
        } else {
            out += char(c);
        }
    }
    return out + "\"";
}

std::string json_number(double value)
{
    if (!std::isfinite(value)) return "null";
    char text[40];
    if (value == std::floor(value) && std::fabs(value) < 9e15) std::snprintf(text, sizeof(text), "%.0f", value);
    else std::snprintf(text, sizeof(text), "%.15g", value);
    return text;
}
