#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cctype>

// Минимальный JSON: значение + рекурсивный парсер + сериализатор.
// Достаточно для сообщений LSP (объекты, массивы, строки, числа, bool, null).
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string str;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    bool isObject() const { return type == Type::Object; }
    bool isArray()  const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isNull()   const { return type == Type::Null; }

    const JsonValue& operator[](const std::string& key) const {
        static const JsonValue nullv;
        auto it = object.find(key);
        return it == object.end() ? nullv : it->second;
    }
    std::string asString() const { return str; }
    double asNumber() const { return number; }
    bool asBool() const { return boolean; }
};

struct JsonParser {
    const std::string& s;
    size_t i = 0;
    explicit JsonParser(const std::string& src) : s(src) {}

    void skipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }

    JsonValue parse() { skipWs(); return parseValue(); }

    JsonValue parseValue() {
        skipWs();
        if (i >= s.size()) return JsonValue{};
        char c = s[i];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') { JsonValue v; v.type = JsonValue::Type::String; v.str = parseString(); return v; }
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { i += 4; return JsonValue{}; }
        return parseNumber();
    }

    JsonValue parseObject() {
        JsonValue v; v.type = JsonValue::Type::Object;
        i++; // {
        skipWs();
        if (i < s.size() && s[i] == '}') { i++; return v; }
        while (true) {
            skipWs();
            std::string key = parseString();
            skipWs();
            if (i < s.size() && s[i] == ':') i++;
            skipWs();
            JsonValue val = parseValue();
            v.object[key] = val;
            skipWs();
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == '}') { i++; break; }
            break;
        }
        return v;
    }

    JsonValue parseArray() {
        JsonValue v; v.type = JsonValue::Type::Array;
        i++; // [
        skipWs();
        if (i < s.size() && s[i] == ']') { i++; return v; }
        while ( // consume elements
            i < s.size()) {
            JsonValue val = parseValue();
            v.array.push_back(val);
            skipWs();
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == ']') { i++; break; }
            break;
        }
        return v;
    }

    std::string parseString() {
        std::string out;
        if (i < s.size() && s[i] == '"') i++; // opening quote
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                i++;
                char e = (i < s.size()) ? s[i++] : '\0';
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        unsigned int cp = 0;
                        for (int k = 0; k < 4 && i < s.size(); k++) {
                            char h = s[i++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp += h - '0';
                            else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
                        }
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += s[i++];
            }
        }
        if (i < s.size() && s[i] == '"') i++;
        return out;
    }

    JsonValue parseBool() {
        JsonValue v; v.type = JsonValue::Type::Bool;
        if (s[i] == 't') { v.boolean = true; i += 4; }
        else { v.boolean = false; i += 5; }
        return v;
    }

    JsonValue parseNumber() {
        JsonValue v; v.type = JsonValue::Type::Number;
        size_t start = i;
        while (i < s.size() &&
               (isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == '-' ||
                s[i] == '+' || s[i] == 'e' || s[i] == 'E')) i++;
        v.number = (start < i) ? std::stod(s.substr(start, i - start)) : 0.0;
        return v;
    }
};

inline JsonValue parseJson(const std::string& src) {
    JsonParser p(src);
    return p.parse();
}

inline void jsonEscape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

inline void jsonBuild(std::string& out, const JsonValue& v) {
    switch (v.type) {
        case JsonValue::Type::Null:   out += "null"; break;
        case JsonValue::Type::Bool:   out += v.boolean ? "true" : "false"; break;
        case JsonValue::Type::Number: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", v.number);
            out += buf;
            break;
        }
        case JsonValue::Type::String: jsonEscape(out, v.str); break;
        case JsonValue::Type::Array: {
            out += '[';
            for (size_t k = 0; k < v.array.size(); k++) {
                if (k) out += ',';
                jsonBuild(out, v.array[k]);
            }
            out += ']';
            break;
        }
        case JsonValue::Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& kv : v.object) {
                if (!first) out += ',';
                first = false;
                jsonEscape(out, kv.first);
                out += ':';
                jsonBuild(out, kv.second);
            }
            out += '}';
            break;
        }
    }
}

inline std::string toJson(const JsonValue& v) {
    std::string out;
    jsonBuild(out, v);
    return out;
}
