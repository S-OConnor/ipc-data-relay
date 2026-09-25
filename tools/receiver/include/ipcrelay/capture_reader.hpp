// Sequential reader for the binary capture file. Used by the tests and
// ipc-relay-capture-to-csv; the canonical standalone validator is
// tools/capture_inspect.py.
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "ipcrelay/capture_format.hpp"

namespace ipcrelay::capture {

class CaptureReader {
public:
    bool open(const std::string& path);
    const FileHeader& file_header() const { return file_header_; }

    // Returns true and fills hdr/payload for the next record. Returns false at
    // end of file or on error; check error() to distinguish.
    bool next(RecordHeader& hdr, std::vector<uint8_t>& payload);
    const std::string& error() const { return error_; }

private:
    std::ifstream file_;
    FileHeader file_header_;
    std::string error_;
};

}  // namespace ipcrelay::capture
