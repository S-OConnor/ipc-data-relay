// UDP wire protocol: the compact binary transport header prepended to every
// UDP datagram emitted by the bridge (BRG-036, BRG-040..BRG-053).
//
// All multi-byte fields are little-endian. See docs/wire_protocol.md for the
// normative field-by-field description.
//
//   offset  size  field
//   0       4     magic            0x42554D5A ("ZMUB" as bytes 5A 4D 55 42)
//   4       2     version          1
//   6       2     flags            bit0 FRAGMENTED, bit1 MULTIPART
//   8       4     source_id        configured per ZeroMQ source
//   12      4     message_length   total original ZeroMQ payload length
//   16      8     sequence         per-source, +1 per complete ZeroMQ message
//   24      8     timestamp_ns     CLOCK_REALTIME nanoseconds at receive
//   32      4     fragment_offset  byte offset of this fragment in the message
//   36      2     fragment_index   0-based
//   38      2     fragment_count   >= 1
//   40      2     fragment_length  payload bytes following the header
//   42      2     reserved         0
//   44      ...   payload (fragment_length bytes)
#pragma once

#include <cstddef>
#include <cstdint>

namespace ipcrelay::wire {

constexpr uint32_t kMagic = 0x42554D5Au;
constexpr uint16_t kVersion = 1;
constexpr std::size_t kHeaderSize = 44;

constexpr uint16_t kFlagFragmented = 0x0001;  // fragment_count > 1
constexpr uint16_t kFlagMultipart = 0x0002;   // ZeroMQ message had >1 frame; frames concatenated

// Largest UDP payload that fits in a single IPv4 datagram (65535 - 20 - 8).
constexpr std::size_t kMaxUdpPayload = 65507;
// Conservative default that fits a 1500-byte Ethernet MTU without IP
// fragmentation even with IP options / VLAN tags present (BRG-060, BRG-061).
constexpr std::size_t kDefaultMaxDatagramSize = 1400;
// Largest message that can be described by the header (u16 fragment_count *
// u16 fragment_length is far larger than the u32 message_length; the u32 wins).
constexpr uint64_t kMaxMessageLength = 0xFFFFFFFFull;

struct Header {
    uint16_t version = kVersion;
    uint16_t flags = 0;
    uint32_t source_id = 0;
    uint32_t message_length = 0;
    uint64_t sequence = 0;
    uint64_t timestamp_ns = 0;
    uint32_t fragment_offset = 0;
    uint16_t fragment_index = 0;
    uint16_t fragment_count = 1;
    uint16_t fragment_length = 0;
};

// Serializes hdr into exactly kHeaderSize bytes at out.
void encode_header(const Header& hdr, uint8_t* out);

enum class ParseError {
    None = 0,
    Truncated,          // datagram shorter than the header
    BadMagic,
    BadVersion,
    BadFragmentCount,   // fragment_count == 0
    BadFragmentIndex,   // fragment_index >= fragment_count
    LengthMismatch,     // datagram length != header + fragment_length
    RangeError,         // fragment_offset + fragment_length > message_length,
                        // or single-fragment message not covering whole payload
    FlagMismatch,       // FRAGMENTED flag inconsistent with fragment_count
};

const char* to_string(ParseError err);

// Validates and decodes a datagram. On success returns ParseError::None,
// fills hdr and sets payload to point at the fragment payload inside data.
ParseError parse_datagram(const uint8_t* data, std::size_t len, Header& hdr,
                          const uint8_t*& payload);

// Number of fragments needed for a message of message_length bytes when each
// datagram may carry at most fragment_payload bytes of payload. A zero-length
// message uses exactly one fragment.
std::size_t fragment_count_for(std::size_t message_length, std::size_t fragment_payload);

}  // namespace ipcrelay::wire
