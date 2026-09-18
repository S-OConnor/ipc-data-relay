#include "ipcrelay/capture_format.hpp"

#include <cstring>

#include "ipcrelay/byteorder.hpp"

namespace ipcrelay::capture {

void encode_file_header(const FileHeader& h, uint8_t* out) {
    std::memset(out, 0, kFileHeaderSize);
    put_u32le(out + 0, kFileMagic);
    put_u16le(out + 4, h.version);
    put_u16le(out + 6, h.header_length);
    put_u64le(out + 8, h.created_ns);
    put_u16le(out + 16, h.wire_version);
}

void encode_record_header(const RecordHeader& h, uint8_t* out) {
    put_u32le(out + 0, kRecordMagic);
    put_u32le(out + 4, h.source_id);
    put_u64le(out + 8, h.sequence);
    put_u64le(out + 16, h.timestamp_ns);
    put_u32le(out + 24, h.payload_length);
    put_u16le(out + 28, h.flags);
    put_u16le(out + 30, 0);
}

bool decode_file_header(const uint8_t* in, std::size_t len, FileHeader& h) {
    if (len < kFileHeaderSize) return false;
    if (get_u32le(in + 0) != kFileMagic) return false;
    h.version = get_u16le(in + 4);
    h.header_length = get_u16le(in + 6);
    h.created_ns = get_u64le(in + 8);
    h.wire_version = get_u16le(in + 16);
    return h.version == kFormatVersion && h.header_length >= kFileHeaderSize;
}

bool decode_record_header(const uint8_t* in, std::size_t len, RecordHeader& h) {
    if (len < kRecordHeaderSize) return false;
    if (get_u32le(in + 0) != kRecordMagic) return false;
    h.source_id = get_u32le(in + 4);
    h.sequence = get_u64le(in + 8);
    h.timestamp_ns = get_u64le(in + 16);
    h.payload_length = get_u32le(in + 24);
    h.flags = get_u16le(in + 28);
    return true;
}

}  // namespace ipcrelay::capture
