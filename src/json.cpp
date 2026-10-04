#include "json.h"

#include "common.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ather {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

Json::Json(int64_t v) : type_(Type::Number), s_(std::to_string(v)) {}
Json::Json(uint64_t v) : type_(Type::Number), s_(std::to_string(v)) {}
Json::Json(double v) : type_(Type::Number) {
    if (!std::isfinite(v)) {
        type_ = Type::Null;
        return;
    }
    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);  // shortest text that reads back as the same double
    s_.assign(buf, r.ptr);
}
Json::Json(const std::wstring& s) : type_(Type::String), s_(ToUtf8(s)) {}

double Json::Num(double def) const {
    if (type_ != Type::Number) return def;
    double v = def;
    std::from_chars(s_.data(), s_.data() + s_.size(), v);
    return v;
}

int64_t Json::Int(int64_t def) const {
    if (type_ != Type::Number) return def;
    int64_t v = 0;
    auto r = std::from_chars(s_.data(), s_.data() + s_.size(), v);
    if (r.ec == std::errc() && r.ptr == s_.data() + s_.size()) return v;
    const double d = Num((double)def);  // 3.0, 1e3...
    return std::isfinite(d) && std::fabs(d) < 9.2e18 ? (int64_t)d : def;
}

uint64_t Json::UInt(uint64_t def) const {
    if (type_ != Type::Number) return def;
    uint64_t v = 0;
    auto r = std::from_chars(s_.data(), s_.data() + s_.size(), v);
    if (r.ec == std::errc() && r.ptr == s_.data() + s_.size()) return v;
    const double d = Num(-1);
    return d >= 0 && d < 1.8e19 ? (uint64_t)d : def;
}

std::wstring Json::WStr(const std::wstring& def) const { return type_ == Type::String ? FromUtf8(s_) : def; }

const Json& Json::operator[](size_t i) const {
    static const Json kNull;
    return type_ == Type::Array && i < arr_.size() ? arr_[i] : kNull;
}

void Json::Push(Json v) {
    if (type_ != Type::Array) {
        *this = Array();
    }
    arr_.push_back(std::move(v));
}

const Json& Json::operator[](const char* key) const {
    static const Json kNull;
    if (type_ != Type::Object) return kNull;
    for (const auto& [k, v] : obj_)
        if (k == key) return v;
    return kNull;
}

bool Json::Has(const char* key) const {
    if (type_ != Type::Object) return false;
    for (const auto& kv : obj_)
        if (kv.first == key) return true;
    return false;
}

void Json::Set(const std::string& key, Json v) {
    if (type_ != Type::Object) *this = Object();
    obj_.emplace_back(key, std::move(v));
}

// ---- writing ----

static void DumpString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char u[8];
                    sprintf_s(u, "\\u%04x", c);
                    out += u;
                } else {
                    out += (char)c;  // UTF-8 passes through
                }
        }
    }
    out += '"';
}

void Json::DumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Number: out += s_; break;
        case Type::String: DumpString(out, s_); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                arr_[i].DumpTo(out);
            }
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                DumpString(out, obj_[i].first);
                out += ':';
                obj_[i].second.DumpTo(out);
            }
            out += '}';
            break;
    }
}

std::string Json::Dump() const {
    std::string out;
    DumpTo(out);
    return out;
}

// ---- reading ----

class JsonParser {
public:
    explicit JsonParser(const std::string& t) : s_(t) {}

    bool Document(Json& out) {
        Skip();
        if (!Value(out, 0)) return false;
        Skip();
        return i_ == s_.size();
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void Skip() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool Lit(const char* w) {
        const size_t n = strlen(w);
        if (s_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }

    bool Value(Json& out, int depth) {
        if (depth > 200 || i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') return Object(out, depth);
        if (c == '[') return Array(out, depth);
        if (c == '"') {
            out = Json();
            out.type_ = Json::Type::String;
            return String(out.s_);
        }
        if (Lit("true")) return out = Json(true), true;
        if (Lit("false")) return out = Json(false), true;
        if (Lit("null")) return out = Json(), true;
        return Number(out);
    }

    bool Number(Json& out) {
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        bool digits = false;
        while (i_ < s_.size() && (isdigit((unsigned char)s_[i_]) || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' ||
                                  ((s_[i_] == '+' || s_[i_] == '-') && (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E')))) {
            digits = digits || isdigit((unsigned char)s_[i_]);
            ++i_;
        }
        if (!digits) return false;
        out = Json();
        out.type_ = Json::Type::Number;
        out.s_ = s_.substr(start, i_ - start);
        return true;
    }

    static void PutUtf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) {
            o += (char)cp;
        } else if (cp < 0x800) {
            o += (char)(0xC0 | cp >> 6);
            o += (char)(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            o += (char)(0xE0 | cp >> 12);
            o += (char)(0x80 | ((cp >> 6) & 63));
            o += (char)(0x80 | (cp & 63));
        } else {
            o += (char)(0xF0 | cp >> 18);
            o += (char)(0x80 | ((cp >> 12) & 63));
            o += (char)(0x80 | ((cp >> 6) & 63));
            o += (char)(0x80 | (cp & 63));
        }
    }

    bool Hex4(uint32_t& v) {
        if (i_ + 4 > s_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= h - '0';
            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
            else return false;
        }
        return true;
    }

    bool String(std::string& o) {
        ++i_;  // opening quote
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                o += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;  // Swift's JSONEncoder writes "\/" in every path
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    uint32_t cp;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        uint32_t lo;
                        if (!Hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    PutUtf8(o, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool Array(Json& out, int depth) {
        out = Json::Array();
        ++i_;
        Skip();
        if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
        for (;;) {
            Json v;
            Skip();
            if (!Value(v, depth + 1)) return false;
            out.arr_.push_back(std::move(v));
            Skip();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') return ++i_, true;
            return false;
        }
    }

    bool Object(Json& out, int depth) {
        out = Json::Object();
        ++i_;
        Skip();
        if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
        for (;;) {
            Skip();
            if (i_ >= s_.size() || s_[i_] != '"') return false;
            std::string key;
            if (!String(key)) return false;
            Skip();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            ++i_;
            Skip();
            Json v;
            if (!Value(v, depth + 1)) return false;
            out.obj_.emplace_back(std::move(key), std::move(v));
            Skip();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') return ++i_, true;
            return false;
        }
    }
};

Json Json::Parse(const std::string& text, bool* ok) {
    Json out;
    std::string body = text;
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
        body.erase(0, 3);  // BOM
    JsonParser p(body);
    const bool good = p.Document(out);
    if (ok) *ok = good;
    return good ? out : Json();
}

}  // namespace ather
