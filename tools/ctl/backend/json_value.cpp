#include "json_value.hpp"

#include <cstdint>
#include <cstdlib>

namespace ipcrelay::ctl::json {

const Value* Value::get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : object) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

namespace {

class Parser {
public:
    Parser(const std::string& text, int max_depth) : s_(text), max_depth_(max_depth) {}

    bool document(Value& out, std::string& error) {
        skip_ws();
        if (!value(out, 0)) {
            error = error_ + " at offset " + std::to_string(pos_);
            return false;
        }
        skip_ws();
        if (pos_ != s_.size()) {
            error = "trailing characters at offset " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    bool fail(const char* what) {
        error_ = what;
        return false;
    }

    bool at_end() const { return pos_ >= s_.size(); }
    char peek() const { return s_[pos_]; }

    void skip_ws() {
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++pos_;
    }

    bool literal(const char* word) {
        std::size_t i = 0;
        for (; word[i] != '\0'; ++i) {
            if (pos_ + i >= s_.size() || s_[pos_ + i] != word[i]) return fail("invalid literal");
        }
        pos_ += i;
        return true;
    }

    bool value(Value& v, int depth) {
        if (at_end()) return fail("unexpected end of input");
        v = Value{};
        switch (peek()) {
            case '{': return object(v, depth + 1);
            case '[': return array(v, depth + 1);
            case '"':
                v.type = Value::Type::String;
                return string(v.string);
            case 't':
                v.type = Value::Type::Bool;
                v.boolean = true;
                return literal("true");
            case 'f':
                v.type = Value::Type::Bool;
                return literal("false");
            case 'n': return literal("null");
            default: return number(v);
        }
    }

    bool object(Value& v, int depth) {
        if (depth > max_depth_) return fail("nesting too deep");
        v.type = Value::Type::Object;
        ++pos_;  // '{'
        skip_ws();
        if (!at_end() && peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            if (at_end() || peek() != '"') return fail("expected object key");
            std::string key;
            if (!string(key)) return false;
            skip_ws();
            if (at_end() || peek() != ':') return fail("expected ':'");
            ++pos_;
            skip_ws();
            v.object.emplace_back(std::move(key), Value{});
            if (!value(v.object.back().second, depth)) return false;
            skip_ws();
            if (at_end()) return fail("unterminated object");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Value& v, int depth) {
        if (depth > max_depth_) return fail("nesting too deep");
        v.type = Value::Type::Array;
        ++pos_;  // '['
        skip_ws();
        if (!at_end() && peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            v.array.emplace_back();
            if (!value(v.array.back(), depth)) return false;
            skip_ws();
            if (at_end()) return fail("unterminated array");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool hex4(uint32_t& out) {
        if (s_.size() - pos_ < 4) return fail("truncated \\u escape");
        out = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = s_[pos_ + i];
            uint32_t d;
            if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
            else return fail("invalid \\u escape");
            out = (out << 4) | d;
        }
        pos_ += 4;
        return true;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        auto put = [&out](uint32_t b) { out.push_back(static_cast<char>(static_cast<uint8_t>(b))); };
        if (cp < 0x80) {
            put(cp);
        } else if (cp < 0x800) {
            put(0xC0 | (cp >> 6));
            put(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            put(0xE0 | (cp >> 12));
            put(0x80 | ((cp >> 6) & 0x3F));
            put(0x80 | (cp & 0x3F));
        } else {
            put(0xF0 | (cp >> 18));
            put(0x80 | ((cp >> 12) & 0x3F));
            put(0x80 | ((cp >> 6) & 0x3F));
            put(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string& out) {
        ++pos_;  // opening quote
        for (;;) {
            if (at_end()) return fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_end()) return fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo;
                        if (s_.compare(pos_, 2, "\\u") != 0) return fail("unpaired surrogate");
                        pos_ += 2;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    bool digits() {
        const std::size_t start = pos_;
        while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
        return pos_ > start;
    }

    bool number(Value& v) {
        const std::size_t start = pos_;
        if (!at_end() && peek() == '-') ++pos_;
        if (at_end()) return fail("invalid number");
        if (peek() == '0') {
            ++pos_;
        } else if (!digits()) {
            return fail("invalid value");
        }
        if (!at_end() && peek() == '.') {
            ++pos_;
            if (!digits()) return fail("invalid number");
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!at_end() && (peek() == '+' || peek() == '-')) ++pos_;
            if (!digits()) return fail("invalid number");
        }
        v.type = Value::Type::Number;
        v.number = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    const std::string& s_;
    std::size_t pos_ = 0;
    int max_depth_;
    std::string error_;
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string& error, int max_depth) {
    Parser p(text, max_depth);
    return p.document(out, error);
}

}  // namespace ipcrelay::ctl::json
