// A small JSON reader for the server's answers (objects, arrays, strings with \u escapes, numbers kept exact when they are integers).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    int64_t i = 0;  // the number when it was written without a fraction or exponent
    bool integral = false;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;

    const JVal& operator[](const char* key) const {
        static const JVal none;
        if (type == Obj)
            for (auto& kv : o)
                if (kv.first == key) return kv.second;
        return none;
    }
    std::string str(const char* def = "") const { return type == Str ? s : def; }
    int64_t i64(int64_t def = 0) const { return type == Num ? (integral ? i : int64_t(n)) : def; }
    double num(double def = 0) const { return type == Num ? n : def; }
    bool truthy() const { return type == Bool ? b : type != Null; }
};

namespace jsonp {
inline void ws(const char*& p, const char* e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
}
inline void utf8(uint32_t c, std::string& out) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) out += char(0xc0 | (c >> 6)), out += char(0x80 | (c & 0x3f));
    else if (c < 0x10000) out += char(0xe0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
    else out += char(0xf0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3f)), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
}
inline bool hex4(const char*& p, const char* e, uint32_t* v) {
    if (e - p < 4) return false;
    *v = 0;
    for (int k = 0; k < 4; k++, p++) {
        char c = *p;
        *v = *v * 16 + uint32_t(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99);
        if (*v > 0xffff) return false;
    }
    return true;
}
inline bool str(const char*& p, const char* e, std::string& out) {
    if (p >= e || *p != '"') return false;
    p++;
    while (p < e && *p != '"') {
        if (*p != '\\') {
            out += *p++;
            continue;
        }
        if (++p >= e) return false;
        char c = *p++;
        switch (c) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            uint32_t v;
            if (!hex4(p, e, &v)) return false;
            if (v >= 0xd800 && v < 0xdc00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                const char* q = p + 2;
                uint32_t lo;
                if (hex4(q, e, &lo) && lo >= 0xdc00 && lo < 0xe000) v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00), p = q;
            }
            utf8(v, out);
            break;
        }
        default: out += c;
        }
    }
    if (p >= e) return false;
    p++;
    return true;
}
inline bool val(const char*& p, const char* e, JVal& v, int depth) {
    if (depth > 64) return false;
    ws(p, e);
    if (p >= e) return false;
    if (*p == '{') {
        v.type = JVal::Obj, p++;
        ws(p, e);
        if (p < e && *p == '}') return p++, true;
        for (;;) {
            std::string k;
            ws(p, e);
            if (!str(p, e, k)) return false;
            ws(p, e);
            if (p >= e || *p != ':') return false;
            p++;
            v.o.emplace_back(std::move(k), JVal{});
            if (!val(p, e, v.o.back().second, depth + 1)) return false;
            ws(p, e);
            if (p < e && *p == ',') { p++; continue; }
            if (p < e && *p == '}') return p++, true;
            return false;
        }
    }
    if (*p == '[') {
        v.type = JVal::Arr, p++;
        ws(p, e);
        if (p < e && *p == ']') return p++, true;
        for (;;) {
            v.a.emplace_back();
            if (!val(p, e, v.a.back(), depth + 1)) return false;
            ws(p, e);
            if (p < e && *p == ',') { p++; continue; }
            if (p < e && *p == ']') return p++, true;
            return false;
        }
    }
    if (*p == '"') return v.type = JVal::Str, str(p, e, v.s);
    if (e - p >= 4 && !std::string(p, 4).compare("true")) return v.type = JVal::Bool, v.b = true, p += 4, true;
    if (e - p >= 5 && !std::string(p, 5).compare("false")) return v.type = JVal::Bool, p += 5, true;
    if (e - p >= 4 && !std::string(p, 4).compare("null")) return p += 4, true;
    const char* s = p;
    bool frac = false;
    if (p < e && *p == '-') p++;
    while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-'))
        frac = frac || *p == '.' || *p == 'e' || *p == 'E', p++;
    if (p == s) return false;
    std::string t(s, p);
    v.type = JVal::Num, v.n = strtod(t.c_str(), nullptr), v.integral = !frac;
    if (!frac) v.i = strtoll(t.c_str(), nullptr, 10);
    return true;
}
}  // namespace jsonp

inline bool json_parse(const std::string& text, JVal* out) {
    const char* p = text.data();
    *out = JVal{};
    return jsonp::val(p, text.data() + text.size(), *out, 0);
}
