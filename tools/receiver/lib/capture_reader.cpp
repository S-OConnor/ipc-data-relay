#include "ipcrelay/capture_reader.hpp"

#include <cerrno>
#include <cstring>

namespace ipcrelay::capture {

CaptureReader::~CaptureReader() {
    if (file_) std::fclose(file_);
}

bool CaptureReader::open(const std::string& path) {
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) {
        error_ = std::string("open '") + path + "': " + std::strerror(errno);
        return false;
    }
    uint8_t raw[kFileHeaderSize];
    if (std::fread(raw, 1, kFileHeaderSize, file_) != kFileHeaderSize) {
        error_ = "short file header";
        return false;
    }
    if (!decode_file_header(raw, kFileHeaderSize, file_header_)) {
        error_ = "bad file magic or unsupported version";
        return false;
    }
    if (file_header_.header_length > kFileHeaderSize) {
        if (std::fseek(file_, static_cast<long>(file_header_.header_length - kFileHeaderSize), SEEK_CUR) != 0) {
            error_ = "cannot skip extended header";
            return false;
        }
    }
    return true;
}

bool CaptureReader::next(RecordHeader& hdr, std::vector<uint8_t>& payload) {
    if (!file_) return false;
    uint8_t raw[kRecordHeaderSize];
    std::size_t n = std::fread(raw, 1, kRecordHeaderSize, file_);
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
    if (hdr.payload_length > 0 && std::fread(payload.data(), 1, hdr.payload_length, file_) != hdr.payload_length) {
        error_ = "truncated record payload";
        return false;
    }
    return true;
}

}  // namespace ipcrelay::capture
