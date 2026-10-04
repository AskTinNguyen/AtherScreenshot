#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ather {

// Minimal JSON DOM for library.json (UTF-8). Numbers keep their source text, so 64-bit integers such as
// perceptual hashes survive a round trip exactly. Object keys keep their order; lookups are linear, which is
// fine for the small objects we read field by field.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), b_(b) {}
    Json(int v) : Json((int64_t)v) {}
    Json(int64_t v);
    Json(uint64_t v);
    Json(double v);
    Json(const char* s) : type_(Type::String), s_(s) {}
    Json(std::string s) : type_(Type::String), s_(std::move(s)) {}
    Json(const std::wstring& s);
    static Json Array() { Json j; j.type_ = Type::Array; return j; }
    static Json Object() { Json j; j.type_ = Type::Object; return j; }

    // Returns Null on malformed input and sets *ok = false.
    static Json Parse(const std::string& text, bool* ok = nullptr);
    std::string Dump() const;

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    // Lenient accessors: the wrong type gives the default, so a field from another app version never throws.
    bool Bool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
    double Num(double def = 0) const;
    int64_t Int(int64_t def = 0) const;
    uint64_t UInt(uint64_t def = 0) const;
    const std::string& Str() const { return s_; }  // empty unless a string
    std::wstring WStr(const std::wstring& def = L"") const;

    // Arrays
    size_t size() const { return type_ == Type::Array ? arr_.size() : type_ == Type::Object ? obj_.size() : 0; }
    const Json& operator[](size_t i) const;
    const Json& operator[](int i) const { return (*this)[(size_t)i]; }
    void Push(Json v);
    const std::vector<Json>& Items() const { return arr_; }

    // Objects
    const Json& operator[](const char* key) const;  // Null if missing
    bool Has(const char* key) const;
    void Set(const std::string& key, Json v);  // appends (no duplicate check: callers build fresh objects)
    const std::vector<std::pair<std::string, Json>>& Members() const { return obj_; }

private:
    Type type_ = Type::Null;
    bool b_ = false;
    std::string s_;  // string value, or the number's text
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
    void DumpTo(std::string& out) const;
    friend class JsonParser;
};

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

}  // namespace ather
