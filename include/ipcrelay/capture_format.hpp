// Binary capture file format written by the receiver (BRG-080..BRG-093).
// Little-endian only. See docs/capture_format.md for the normative
// description.
//
// File header (32 bytes):
//   0   4  magic            0x50434D5A ("ZMCP" as bytes 5A 4D 43 50)
//   4   2  version          1
//   6   2  header_length    32
//   8   8  created_ns       CLOCK_REALTIME ns when the file was created
//   16  2  wire_version     wire protocol version the receiver understood
//   18  2  reserved         0
//   20  12 reserved         0
//
// Record header (32 bytes), followed by payload_length payload bytes:
//   0   4  record_magic     0x52434D5A ("ZMCR" as bytes 5A 4D 43 52)
//   4   4  source_id
//   8   8  sequence
//   16  8  timestamp_ns
//   24  4  payload_length
//   28  2  flags            wire header flags of the original message
//   30  2  reserved         0
#pragma once

#include <cstddef>
#include <cstdint>

namespace ipcrelay::capture {

constexpr uint32_t kFileMagic = 0x50434D5Au;
constexpr uint32_t kRecordMagic = 0x52434D5Au;
constexpr uint16_t kFormatVersion = 1;
constexpr std::size_t kFileHeaderSize = 32;
constexpr std::size_t kRecordHeaderSize = 32;

struct FileHeader {
    uint16_t version = kFormatVersion;
    uint16_t header_length = static_cast<uint16_t>(kFileHeaderSize);
    uint64_t created_ns = 0;
    uint16_t wire_version = 0;
};

struct RecordHeader {
    uint32_t source_id = 0;
    uint64_t sequence = 0;
    uint64_t timestamp_ns = 0;
    uint32_t payload_length = 0;
    uint16_t flags = 0;
};

void encode_file_header(const FileHeader& h, uint8_t* out);   // kFileHeaderSize bytes
void encode_record_header(const RecordHeader& h, uint8_t* out);  // kRecordHeaderSize bytes

// Return false if the magic or version does not match.
bool decode_file_header(const uint8_t* in, std::size_t len, FileHeader& h);
bool decode_record_header(const uint8_t* in, std::size_t len, RecordHeader& h);

}  // namespace ipcrelay::capture
