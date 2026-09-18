#include "ipcrelay/wire_protocol.hpp"

#include "ipcrelay/byteorder.hpp"

namespace ipcrelay::wire {

void encode_header(const Header& h, uint8_t* out) {
    put_u32le(out + 0, kMagic);
    put_u16le(out + 4, h.version);
    put_u16le(out + 6, h.flags);
    put_u32le(out + 8, h.source_id);
    put_u32le(out + 12, h.message_length);
    put_u64le(out + 16, h.sequence);
    put_u64le(out + 24, h.timestamp_ns);
    put_u32le(out + 32, h.fragment_offset);
    put_u16le(out + 36, h.fragment_index);
    put_u16le(out + 38, h.fragment_count);
    put_u16le(out + 40, h.fragment_length);
    put_u16le(out + 42, 0);
}

const char* to_string(ParseError err) {
    switch (err) {
        case ParseError::None: return "ok";
        case ParseError::Truncated: return "truncated";
        case ParseError::BadMagic: return "bad_magic";
        case ParseError::BadVersion: return "bad_version";
        case ParseError::BadFragmentCount: return "bad_fragment_count";
        case ParseError::BadFragmentIndex: return "bad_fragment_index";
        case ParseError::LengthMismatch: return "length_mismatch";
        case ParseError::RangeError: return "range_error";
        case ParseError::FlagMismatch: return "flag_mismatch";
    }
    return "unknown";
}

ParseError parse_datagram(const uint8_t* data, std::size_t len, Header& h, const uint8_t*& payload) {
    payload = nullptr;
    if (len < kHeaderSize) return ParseError::Truncated;
    if (get_u32le(data + 0) != kMagic) return ParseError::BadMagic;
    h.version = get_u16le(data + 4);
    if (h.version != kVersion) return ParseError::BadVersion;
    h.flags = get_u16le(data + 6);
    h.source_id = get_u32le(data + 8);
    h.message_length = get_u32le(data + 12);
    h.sequence = get_u64le(data + 16);
    h.timestamp_ns = get_u64le(data + 24);
    h.fragment_offset = get_u32le(data + 32);
    h.fragment_index = get_u16le(data + 36);
    h.fragment_count = get_u16le(data + 38);
    h.fragment_length = get_u16le(data + 40);

    if (h.fragment_count == 0) return ParseError::BadFragmentCount;
    if (h.fragment_index >= h.fragment_count) return ParseError::BadFragmentIndex;
    if (len != kHeaderSize + h.fragment_length) return ParseError::LengthMismatch;

    const uint64_t end = static_cast<uint64_t>(h.fragment_offset) + h.fragment_length;
    if (end > h.message_length) return ParseError::RangeError;
    if (h.fragment_count == 1) {
        if (h.fragment_offset != 0 || h.fragment_length != h.message_length) return ParseError::RangeError;
        if (h.flags & kFlagFragmented) return ParseError::FlagMismatch;
    } else {
        if (!(h.flags & kFlagFragmented)) return ParseError::FlagMismatch;
        // Every fragment except the last must carry at least one byte, and the
        // last fragment must end exactly at message_length.
        if (h.fragment_index + 1u == h.fragment_count) {
            if (end != h.message_length) return ParseError::RangeError;
        } else if (h.fragment_length == 0) {
            return ParseError::RangeError;
        }
    }
    payload = data + kHeaderSize;
    return ParseError::None;
}

std::size_t fragment_count_for(std::size_t message_length, std::size_t fragment_payload) {
    if (fragment_payload == 0) return 0;
    if (message_length == 0) return 1;
    return (message_length + fragment_payload - 1) / fragment_payload;
}

}  // namespace ipcrelay::wire
