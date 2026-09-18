// Little-endian serialization helpers.
//
// The application wire protocol and the capture file format are little-endian
// only (BRG-041, BRG-083). Integers are always written and read one byte at a
// time, least-significant byte first, so no raw C structures are ever placed
// on the wire (BRG-053) and no byte-order conversion of any kind is applied to
// application-defined fields (BRG-042).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ipcrelay {

inline void put_u8(uint8_t* p, uint8_t v) { p[0] = v; }

inline void put_u16le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void put_u32le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline void put_u64le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

inline uint8_t get_u8(const uint8_t* p) { return p[0]; }

inline uint16_t get_u16le(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t get_u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t get_u64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    }
    return v;
}

}  // namespace ipcrelay
