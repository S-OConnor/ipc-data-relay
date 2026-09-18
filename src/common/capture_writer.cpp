#include "ipcrelay/capture_writer.hpp"

#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "ipcrelay/time_util.hpp"

namespace ipcrelay::capture {

CaptureWriter::~CaptureWriter() {
    if (file_) {
        (void)close();
    }
    delete[] buffer_;
}

void CaptureWriter::fail(const std::string& what) {
    failed_ = true;
    last_error_ = what;
}

bool CaptureWriter::open(const std::string& path, std::size_t buffer_bytes, uint16_t wire_version) {
    if (file_) {
        fail("capture file already open");
        return false;
    }
    failed_ = false;
    last_error_.clear();
    path_ = path;
    records_ = 0;
    bytes_ = 0;

    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) {
        fail(std::string("open '") + path + "': " + std::strerror(errno));
        return false;
    }
    if (buffer_bytes > 0) {
        delete[] buffer_;
        buffer_ = new char[buffer_bytes];
        std::setvbuf(file_, buffer_, _IOFBF, buffer_bytes);
    }

    FileHeader h;
    h.created_ns = now_realtime_ns();
    h.wire_version = wire_version;
    uint8_t raw[kFileHeaderSize];
    encode_file_header(h, raw);
    if (std::fwrite(raw, 1, kFileHeaderSize, file_) != kFileHeaderSize) {
        fail(std::string("write header '") + path + "': " + std::strerror(errno));
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    bytes_ += kFileHeaderSize;
    return true;
}

bool CaptureWriter::write_record(const RecordHeader& hdr, const uint8_t* payload, std::size_t len) {
    if (!file_ || failed_) return false;
    RecordHeader h = hdr;
    h.payload_length = static_cast<uint32_t>(len);
    uint8_t raw[kRecordHeaderSize];
    encode_record_header(h, raw);
    if (std::fwrite(raw, 1, kRecordHeaderSize, file_) != kRecordHeaderSize) {
        fail(std::string("write record header: ") + std::strerror(errno));
        return false;
    }
    if (len > 0 && std::fwrite(payload, 1, len, file_) != len) {
        fail(std::string("write record payload: ") + std::strerror(errno));
        return false;
    }
    ++records_;
    bytes_ += kRecordHeaderSize + len;
    return true;
}

bool CaptureWriter::flush(bool sync) {
    if (!file_) return false;
    if (std::fflush(file_) != 0) {
        fail(std::string("flush: ") + std::strerror(errno));
        return false;
    }
    if (sync && ::fsync(fileno(file_)) != 0) {
        fail(std::string("fsync: ") + std::strerror(errno));
        return false;
    }
    return true;
}

bool CaptureWriter::close() {
    if (!file_) return true;
    bool ok = true;
    if (std::fflush(file_) != 0) {
        fail(std::string("flush on close: ") + std::strerror(errno));
        ok = false;
    }
    if (::fsync(fileno(file_)) != 0 && errno != EINVAL && errno != EROFS) {
        fail(std::string("fsync on close: ") + std::strerror(errno));
        ok = false;
    }
    if (std::fclose(file_) != 0) {
        fail(std::string("close: ") + std::strerror(errno));
        ok = false;
    }
    file_ = nullptr;
    return ok;
}

}  // namespace ipcrelay::capture
