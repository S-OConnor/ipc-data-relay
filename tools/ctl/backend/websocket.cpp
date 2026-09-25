#include "websocket.hpp"

#include <array>
#include <vector>

namespace ipcrelay::ctl {

namespace {

uint32_t rotl(uint32_t v, unsigned n) { return (v << n) | (v >> (32U - n)); }

}  // namespace

std::string sha1(const std::string& data) {
    uint32_t h[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};

    std::vector<uint8_t> m(data.begin(), data.end());
    const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8U;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (unsigned i = 0; i < 8; ++i) m.push_back(static_cast<uint8_t>(bit_len >> (56U - 8U * i)));

    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::array<uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const uint8_t* p = &m[off + 4 * i];
            w[i] = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
        }
        for (std::size_t i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (std::size_t i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::string out;
    out.reserve(20);
    for (uint32_t v : h) {
        for (unsigned s = 24;; s -= 8) {
            out.push_back(static_cast<char>(static_cast<uint8_t>(v >> s)));
            if (s == 0) break;
        }
    }
    return out;
}

std::string base64_encode(const std::string& data) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    const std::size_t n = data.size();
    for (std::size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t{static_cast<uint8_t>(data[i])} << 16;
        if (i + 1 < n) v |= uint32_t{static_cast<uint8_t>(data[i + 1])} << 8;
        if (i + 2 < n) v |= uint32_t{static_cast<uint8_t>(data[i + 2])};
        out.push_back(kTable[(v >> 18) & 63U]);
        out.push_back(kTable[(v >> 12) & 63U]);
        out.push_back(i + 1 < n ? kTable[(v >> 6) & 63U] : '=');
        out.push_back(i + 2 < n ? kTable[v & 63U] : '=');
    }
    return out;
}

std::string websocket_accept_key(const std::string& client_key) {
    return base64_encode(sha1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

WsParse ws_parse_frame(const std::string& buf, std::size_t max_payload, WsFrame& frame, std::size_t& consumed,
                       std::string& error) {
    auto byte = [&buf](std::size_t i) { return static_cast<uint8_t>(buf[i]); };
    const std::size_t len = buf.size();
    if (len < 2) return WsParse::NeedMore;

    const uint8_t b0 = byte(0);
    const uint8_t b1 = byte(1);
    if (b0 & 0x70) {
        error = "reserved bits set (no extensions negotiated)";
        return WsParse::Error;
    }
    const bool fin = (b0 & 0x80) != 0;
    const uint8_t opcode = b0 & 0x0F;
    switch (opcode) {
        case ws_opcode::kContinuation:
        case ws_opcode::kText:
        case ws_opcode::kBinary:
        case ws_opcode::kClose:
        case ws_opcode::kPing:
        case ws_opcode::kPong:
            break;
        default:
            error = "unknown opcode " + std::to_string(opcode);
            return WsParse::Error;
    }
    if ((b1 & 0x80) == 0) {
        error = "client frame is not masked";
        return WsParse::Error;
    }

    uint64_t payload_len = b1 & 0x7FU;
    std::size_t pos = 2;
    if (payload_len == 126) {
        if (len < 4) return WsParse::NeedMore;
        payload_len = (uint64_t{byte(2)} << 8) | uint64_t{byte(3)};
        pos = 4;
    } else if (payload_len == 127) {
        if (len < 10) return WsParse::NeedMore;
        payload_len = 0;
        for (std::size_t i = 0; i < 8; ++i) payload_len = (payload_len << 8) | uint64_t{byte(2 + i)};
        pos = 10;
    }
    if ((opcode & 0x08) != 0 && (!fin || payload_len > 125)) {
        error = "invalid control frame";
        return WsParse::Error;
    }
    if (payload_len > max_payload) {
        error = "frame payload of " + std::to_string(payload_len) + " bytes exceeds limit";
        return WsParse::Error;
    }
    if (len < pos + 4) return WsParse::NeedMore;
    const std::size_t mask_pos = pos;
    pos += 4;
    const std::size_t n = static_cast<std::size_t>(payload_len);
    if (len - pos < n) return WsParse::NeedMore;

    frame.fin = fin;
    frame.opcode = opcode;
    frame.payload.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        frame.payload[i] = static_cast<char>(byte(pos + i) ^ byte(mask_pos + (i & 3U)));
    }
    consumed = pos + n;
    return WsParse::Frame;
}

std::string ws_encode_frame(uint8_t opcode, const std::string& payload) {
    std::string out;
    const std::size_t n = payload.size();
    out.reserve(n + 10);
    out.push_back(static_cast<char>(0x80U | (opcode & 0x0FU)));
    if (n < 126) {
        out.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>(static_cast<uint8_t>(n >> 8)));
        out.push_back(static_cast<char>(static_cast<uint8_t>(n)));
    } else {
        out.push_back(static_cast<char>(127));
        const uint64_t v = n;
        for (unsigned s = 56;; s -= 8) {
            out.push_back(static_cast<char>(static_cast<uint8_t>(v >> s)));
            if (s == 0) break;
        }
    }
    out += payload;
    return out;
}

std::string ws_close_payload(uint16_t code, const std::string& reason) {
    std::string out;
    out.push_back(static_cast<char>(static_cast<uint8_t>(code >> 8)));
    out.push_back(static_cast<char>(static_cast<uint8_t>(code)));
    out += reason.substr(0, 123);
    return out;
}

}  // namespace ipcrelay::ctl
