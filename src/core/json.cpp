#include "core/json.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace json {

namespace {
const std::string kEmptyString;
const std::vector<Value> kEmptyArray;
const std::vector<Member> kEmptyObject;
const Value kNullValue;
constexpr int kMaxDepth = 256;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool parseDocument(Value& out, std::string& error) {
        skipWhitespace();
        if (!parseValue(out, 0)) {
            error = error_;
            return false;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            fail(pos_, "unexpected characters after the JSON value");
            error = error_;
            return false;
        }
        return true;
    }

private:
    std::string_view text_;
    size_t pos_ = 0;
    std::string error_;

    bool fail(size_t at, const char* message) {
        if (!error_.empty()) return false;  // keep the first (innermost) error
        size_t line = 1, col = 1;
        for (size_t i = 0; i < at && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf), "line %zu, column %zu: %s", line, col, message);
        error_ = buf;
        return false;
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    bool parseValue(Value& out, int depth) {
        if (depth > kMaxDepth) return fail(pos_, "nesting is too deep");
        if (pos_ >= text_.size()) return fail(pos_, "unexpected end of input");
        const char c = text_[pos_];
        switch (c) {
        case '{': return parseObject(out, depth);
        case '[': return parseArray(out, depth);
        case '"': {
            std::string s;
            if (!parseString(s)) return false;
            out = Value::makeString(std::move(s));
            return true;
        }
        case 't': return parseLiteral("true", Value::makeBool(true), out);
        case 'f': return parseLiteral("false", Value::makeBool(false), out);
        case 'n': return parseLiteral("null", Value(), out);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
            return fail(pos_, "expected a value");
        }
    }

    bool parseLiteral(std::string_view word, Value value, Value& out) {
        if (text_.substr(pos_, word.size()) != word) return fail(pos_, "invalid literal");
        pos_ += word.size();
        out = std::move(value);
        return true;
    }

    bool parseNumber(Value& out) {
        const size_t start = pos_;
        auto digits = [&]() {
            const size_t s = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            return pos_ > s;
        };
        if (text_[pos_] == '-') ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '0') {
            ++pos_;
        } else if (!digits()) {
            return fail(start, "invalid number");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!digits()) return fail(start, "invalid number: digits expected after '.'");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (!digits()) return fail(start, "invalid number: digits expected in exponent");
        }
        const std::string token(text_.substr(start, pos_ - start));
        out = Value::makeNumber(std::strtod(token.c_str(), nullptr));
        return true;
    }

    static void appendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseHex4(unsigned& out) {
        if (pos_ + 4 > text_.size()) return fail(pos_, "truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<unsigned>(c - 'A' + 10);
            else
                return fail(pos_ - 1, "invalid hex digit in \\u escape");
        }
        return true;
    }

    bool parseString(std::string& out) {
        const size_t start = pos_;
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= text_.size()) return fail(start, "unterminated string");
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail(pos_ - 1, "control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return fail(start, "unterminated string");
            const char e = text_[pos_++];
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
                unsigned cp = 0;
                if (!parseHex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (text_.substr(pos_, 2) != "\\u") return fail(pos_, "unpaired surrogate");
                    pos_ += 2;
                    if (!parseHex4(lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF) return fail(pos_ - 4, "invalid low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail(pos_ - 4, "unpaired surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail(pos_ - 1, "invalid escape sequence");
            }
        }
    }

    bool parseArray(Value& out, int depth) {
        ++pos_;  // [
        std::vector<Value> items;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out = Value::makeArray(std::move(items));
            return true;
        }
        while (true) {
            skipWhitespace();
            Value item;
            if (!parseValue(item, depth + 1)) return false;
            items.push_back(std::move(item));
            skipWhitespace();
            if (pos_ >= text_.size()) return fail(pos_, "unterminated array");
            if (text_[pos_] == ',') {
                ++pos_;
                skipWhitespace();
                if (pos_ < text_.size() && text_[pos_] == ']') return fail(pos_, "trailing comma in array");
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                out = Value::makeArray(std::move(items));
                return true;
            }
            return fail(pos_, "expected ',' or ']'");
        }
    }

    bool parseObject(Value& out, int depth) {
        ++pos_;  // {
        std::vector<Member> members;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = Value::makeObject(std::move(members));
            return true;
        }
        while (true) {
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                if (pos_ < text_.size() && text_[pos_] == '}') return fail(pos_, "trailing comma in object");
                return fail(pos_, "expected a string key");
            }
            Member m;
            if (!parseString(m.key)) return false;
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail(pos_, "expected ':' after key");
            ++pos_;
            skipWhitespace();
            if (!parseValue(m.value, depth + 1)) return false;
            members.push_back(std::move(m));
            skipWhitespace();
            if (pos_ >= text_.size()) return fail(pos_, "unterminated object");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                out = Value::makeObject(std::move(members));
                return true;
            }
            return fail(pos_, "expected ',' or '}'");
        }
    }
};

} // namespace

const std::string& Value::asString() const { return type_ == Type::String ? string_ : kEmptyString; }
const std::vector<Value>& Value::asArray() const { return type_ == Type::Array ? array_ : kEmptyArray; }
const std::vector<Member>& Value::asObject() const { return type_ == Type::Object ? object_ : kEmptyObject; }

size_t Value::size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

const Value& Value::operator[](size_t index) const {
    if (type_ != Type::Array || index >= array_.size()) return kNullValue;
    return array_[index];
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const Member& m : object_)
        if (m.key == key) return &m.value;
    return nullptr;
}

Value Value::makeBool(bool b) {
    Value v;
    v.type_ = Type::Bool;
    v.bool_ = b;
    return v;
}

Value Value::makeNumber(double n) {
    Value v;
    v.type_ = Type::Number;
    v.number_ = n;
    return v;
}

Value Value::makeString(std::string s) {
    Value v;
    v.type_ = Type::String;
    v.string_ = std::move(s);
    return v;
}

Value Value::makeArray(std::vector<Value> items) {
    Value v;
    v.type_ = Type::Array;
    v.array_ = std::move(items);
    return v;
}

Value Value::makeObject(std::vector<Member> members) {
    Value v;
    v.type_ = Type::Object;
    v.object_ = std::move(members);
    return v;
}

bool parse(std::string_view text, Value& out, std::string& error) {
    error.clear();
    Parser p(text);
    Value result;
    if (!p.parseDocument(result, error)) return false;
    out = std::move(result);
    return true;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    // Skip a UTF-8 byte order mark if present.
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);
    return true;
}

} // namespace json
