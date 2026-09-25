// Small JSON parser for the control tool: validates the receiver's statistics
// messages and decodes browser requests. Numbers are held as double.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ipcrelay::ctl::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;  // in document order

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    // Member lookup; nullptr if this is not an object or the key is absent.
    const Value* get(const std::string& key) const;
};

// Parses a complete JSON document (RFC 8259). Nesting deeper than max_depth
// is rejected.
bool parse(const std::string& text, Value& out, std::string& error, int max_depth = 64);

}  // namespace ipcrelay::ctl::json
