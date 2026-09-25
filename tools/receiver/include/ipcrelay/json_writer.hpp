// Tiny JSON serializer for the runtime statistics message (BRG-075A).
#pragma once

#include <cstdint>
#include <string>

namespace ipcrelay {

class JsonWriter {
public:
    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(const std::string& k);
    JsonWriter& value(const std::string& v);
    JsonWriter& value(const char* v);
    JsonWriter& value(uint64_t v);
    JsonWriter& value(int64_t v);
    JsonWriter& value(int v) { return value(static_cast<int64_t>(v)); }
    JsonWriter& value(unsigned v) { return value(static_cast<uint64_t>(v)); }
    JsonWriter& value(double v);
    JsonWriter& value(bool v);
    JsonWriter& null();

    const std::string& str() const { return out_; }
    void clear() { out_.clear(); need_comma_ = false; }

private:
    void separator();
    static void escape(const std::string& s, std::string& out);
    std::string out_;
    bool need_comma_ = false;
};

}  // namespace ipcrelay
