// Buffered writer for the binary capture file. Reports open/write/flush
// errors through last_error() and marks itself failed rather than throwing,
// so the receiver can keep receiving and reporting statistics (BRG-093).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "ipcrelay/capture_format.hpp"

namespace ipcrelay::capture {

class CaptureWriter {
public:
    CaptureWriter() = default;
    ~CaptureWriter();
    CaptureWriter(const CaptureWriter&) = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;

    // Creates (truncates) the file and writes the file header.
    // buffer_bytes sets the stdio buffer size.
    bool open(const std::string& path, std::size_t buffer_bytes, uint16_t wire_version);

    // Appends one record. Returns false on failure (see last_error()).
    bool write_record(const RecordHeader& hdr, const uint8_t* payload, std::size_t len);

    // Flushes stdio buffers to the kernel. If sync is true also calls fsync().
    bool flush(bool sync = false);

    // Flushes, fsyncs and closes. Returns false if any step failed.
    bool close();

    bool is_open() const { return file_ != nullptr; }
    bool failed() const { return failed_; }
    const std::string& last_error() const { return last_error_; }
    const std::string& path() const { return path_; }
    uint64_t records_written() const { return records_; }
    uint64_t bytes_written() const { return bytes_; }

private:
    void fail(const std::string& what);

    std::FILE* file_ = nullptr;
    std::string path_;
    std::string last_error_;
    bool failed_ = false;
    uint64_t records_ = 0;
    uint64_t bytes_ = 0;
    char* buffer_ = nullptr;
};

}  // namespace ipcrelay::capture
