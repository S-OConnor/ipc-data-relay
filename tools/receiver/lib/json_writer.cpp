#include "ipcrelay/json_writer.hpp"

#include <cstdio>

namespace ipcrelay {

void JsonWriter::separator() {
    if (need_comma_) out_.push_back(',');
    need_comma_ = true;
}

void JsonWriter::escape(const std::string& s, std::string& out) {
    out.push_back('"');
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

JsonWriter& JsonWriter::begin_object() { separator(); out_.push_back('{'); need_comma_ = false; return *this; }
JsonWriter& JsonWriter::end_object() { out_.push_back('}'); need_comma_ = true; return *this; }
JsonWriter& JsonWriter::begin_array() { separator(); out_.push_back('['); need_comma_ = false; return *this; }
JsonWriter& JsonWriter::end_array() { out_.push_back(']'); need_comma_ = true; return *this; }

JsonWriter& JsonWriter::key(const std::string& k) {
    separator();
    escape(k, out_);
    out_.push_back(':');
    need_comma_ = false;
    return *this;
}

JsonWriter& JsonWriter::value(const std::string& v) { separator(); escape(v, out_); return *this; }
JsonWriter& JsonWriter::value(const char* v) { return value(std::string(v)); }
JsonWriter& JsonWriter::value(uint64_t v) { separator(); out_ += std::to_string(v); return *this; }
JsonWriter& JsonWriter::value(int64_t v) { separator(); out_ += std::to_string(v); return *this; }
JsonWriter& JsonWriter::value(double v) {
    separator();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    out_ += buf;
    return *this;
}
JsonWriter& JsonWriter::value(bool v) { separator(); out_ += v ? "true" : "false"; return *this; }
JsonWriter& JsonWriter::null() { separator(); out_ += "null"; return *this; }
JsonWriter& JsonWriter::raw(const std::string& json) { separator(); out_ += json; return *this; }

}  // namespace ipcrelay
