// Minimal JSON DOM parser (RFC 8259) with line/column error messages.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Member;

class Value {
public:
    Value() = default;

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Accessors return the default (or an empty container) when the type does not match.
    bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double asNumber(double def = 0.0) const { return type_ == Type::Number ? number_ : def; }
    const std::string& asString() const;
    const std::vector<Value>& asArray() const;
    const std::vector<Member>& asObject() const;

    // Array element count or object member count; 0 otherwise.
    size_t size() const;
    // Array element; returns a null value when out of range or not an array.
    const Value& operator[](size_t index) const;
    // Object member lookup (first match); nullptr when absent or not an object.
    const Value* find(std::string_view key) const;

    static Value makeBool(bool b);
    static Value makeNumber(double n);
    static Value makeString(std::string s);
    static Value makeArray(std::vector<Value> items);
    static Value makeObject(std::vector<Member> members);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<Member> object_;
};

struct Member {
    std::string key;
    Value value;
};

// Parses a complete JSON document. On failure returns false and sets `error` to
// "line L, column C: message" (1-based, column counted in bytes).
bool parse(std::string_view text, Value& out, std::string& error);

// Reads a whole file into `out`. Returns false if the file cannot be opened.
bool readFile(const std::string& path, std::string& out);

} // namespace json
