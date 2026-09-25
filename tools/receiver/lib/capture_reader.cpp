#include "ipcrelay/capture_reader.hpp"

#include <cerrno>
#include <cstring>

namespace ipcrelay::capture {

namespace {

// Reads up to n bytes; returns the number actually read.
std::size_t read_bytes(std::ifstream& f, uint8_t* out, std::size_t n) {
    f.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(f.gcount());
}

}  // namespace

bool CaptureReader::open(const std::string& path) {
    file_.open(path, std::ios::binary);
    if (!file_) {
        error_ = std::string("open '") + path + "': " + std::strerror(errno);
        return false;
    }
    uint8_t raw[kFileHeaderSize];
    if (read_bytes(file_, raw, kFileHeaderSize) != kFileHeaderSize) {
        error_ = "short file header";
        return false;
    }
    if (!decode_file_header(raw, kFileHeaderSize, file_header_)) {
        error_ = "bad file magic or unsupported version";
        return false;
    }
    if (file_header_.header_length > kFileHeaderSize) {
        if (!file_.seekg(static_cast<std::streamoff>(file_header_.header_length - kFileHeaderSize), std::ios::cur)) {
            error_ = "cannot skip extended header";
            return false;
        }
    }
    return true;
}

bool CaptureReader::next(RecordHeader& hdr, std::vector<uint8_t>& payload) {
    if (!file_.is_open() || !error_.empty()) return false;
    uint8_t raw[kRecordHeaderSize];
    std::size_t n = read_bytes(file_, raw, kRecordHeaderSize);
    if (n == 0) return false;  // clean EOF
    if (n != kRecordHeaderSize) {
        error_ = "truncated record header";
        return false;
    }
    if (!decode_record_header(raw, kRecordHeaderSize, hdr)) {
        error_ = "bad record magic";
        return false;
    }
    payload.resize(hdr.payload_length);
    if (hdr.payload_length > 0 && read_bytes(file_, payload.data(), hdr.payload_length) != hdr.payload_length) {
        error_ = "truncated record payload";
        return false;
    }
    return true;
}

}  // namespace ipcrelay::capture
