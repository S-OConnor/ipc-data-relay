// WebSocket (RFC 6455) primitives for the control tool's browser channel:
// handshake key derivation and frame encoding/decoding. No I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ipcrelay::ctl {

// SHA-1 digest (20 raw bytes). Only used for the WebSocket handshake.
std::string sha1(const std::string& data);
std::string base64_encode(const std::string& data);

// Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key.
std::string websocket_accept_key(const std::string& client_key);

namespace ws_opcode {
constexpr uint8_t kContinuation = 0x0;
constexpr uint8_t kText = 0x1;
constexpr uint8_t kBinary = 0x2;
constexpr uint8_t kClose = 0x8;
constexpr uint8_t kPing = 0x9;
constexpr uint8_t kPong = 0xA;
}  // namespace ws_opcode

namespace ws_close {
constexpr uint16_t kNormal = 1000;
constexpr uint16_t kGoingAway = 1001;
constexpr uint16_t kProtocolError = 1002;
constexpr uint16_t kUnsupportedData = 1003;
constexpr uint16_t kInvalidPayload = 1007;
constexpr uint16_t kTooBig = 1009;
}  // namespace ws_close

struct WsFrame {
    bool fin = false;
    uint8_t opcode = 0;
    std::string payload;  // unmasked
};

enum class WsParse { NeedMore, Frame, Error };

// Parses one client-to-server frame from the start of buf. On Frame, consumed
// is the number of bytes the frame occupied. Client frames must be masked;
// payloads larger than max_payload are rejected.
WsParse ws_parse_frame(const std::string& buf, std::size_t max_payload, WsFrame& frame, std::size_t& consumed,
                       std::string& error);

// Encodes an unmasked server-to-client frame with FIN set.
std::string ws_encode_frame(uint8_t opcode, const std::string& payload);

// Payload of a close frame: 2-byte status code followed by a UTF-8 reason.
std::string ws_close_payload(uint16_t code, const std::string& reason);

}  // namespace ipcrelay::ctl
