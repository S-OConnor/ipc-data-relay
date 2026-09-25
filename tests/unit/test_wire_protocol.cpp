#include <cstring>
#include <vector>

#include "ipcrelay/common/byteorder.hpp"
#include "ipcrelay/common/wire_protocol.hpp"
#include "test_framework.hpp"

using namespace ipcrelay;
using namespace ipcrelay::wire;

static std::vector<uint8_t> make_datagram(const Header& h, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> d(kHeaderSize + payload.size());
    encode_header(h, d.data());
    std::memcpy(d.data() + kHeaderSize, payload.data(), payload.size());
    return d;
}

TEST(wire_header_layout_is_documented_offsets) {
    Header h;
    h.flags = 0x0003;
    h.source_id = 0x01020304;
    h.message_length = 1000;
    h.sequence = 0x1122334455667788ull;
    h.timestamp_ns = 0x0807060504030201ull;
    h.fragment_offset = 500;
    h.fragment_index = 1;
    h.fragment_count = 2;
    h.fragment_length = 500;
    uint8_t b[kHeaderSize];
    encode_header(h, b);
    // magic "ZMUB" bytes
    CHECK_EQ(b[0], 0x5A); CHECK_EQ(b[1], 0x4D); CHECK_EQ(b[2], 0x55); CHECK_EQ(b[3], 0x42);
    CHECK_EQ(get_u16le(b + 4), kVersion);
    CHECK_EQ(get_u16le(b + 6), 3);
    CHECK_EQ(get_u32le(b + 8), 0x01020304u);
    CHECK_EQ(get_u32le(b + 12), 1000u);
    CHECK_EQ(get_u64le(b + 16), 0x1122334455667788ull);
    CHECK_EQ(get_u64le(b + 24), 0x0807060504030201ull);
    CHECK_EQ(get_u32le(b + 32), 500u);
    CHECK_EQ(get_u16le(b + 36), 1);
    CHECK_EQ(get_u16le(b + 38), 2);
    CHECK_EQ(get_u16le(b + 40), 500);
    CHECK_EQ(get_u16le(b + 42), 0);
}

TEST(wire_roundtrip_single_fragment) {
    Header h;
    h.source_id = 7;
    h.message_length = 5;
    h.sequence = 42;
    h.timestamp_ns = 123456789;
    h.fragment_length = 5;
    auto d = make_datagram(h, {1, 2, 3, 4, 5});
    Header out;
    const uint8_t* payload = nullptr;
    CHECK_EQ(static_cast<int>(parse_datagram(d.data(), d.size(), out, payload)), static_cast<int>(ParseError::None));
    REQUIRE(payload != nullptr);
    CHECK_EQ(out.source_id, 7u);
    CHECK_EQ(out.sequence, 42u);
    CHECK_EQ(out.timestamp_ns, 123456789u);
    CHECK_EQ(out.fragment_count, 1);
    CHECK_EQ(payload[4], 5);
}

TEST(wire_zero_length_message) {
    Header h;
    h.message_length = 0;
    h.fragment_length = 0;
    auto d = make_datagram(h, {});
    Header out;
    const uint8_t* payload = nullptr;
    CHECK_EQ(static_cast<int>(parse_datagram(d.data(), d.size(), out, payload)), static_cast<int>(ParseError::None));
    CHECK_EQ(out.message_length, 0u);
}

TEST(wire_rejects_malformed) {
    Header h;
    h.message_length = 4;
    h.fragment_length = 4;
    auto good = make_datagram(h, {9, 9, 9, 9});
    Header out;
    const uint8_t* payload = nullptr;

    // truncated
    CHECK_EQ(static_cast<int>(parse_datagram(good.data(), 10, out, payload)), static_cast<int>(ParseError::Truncated));
    // bad magic
    auto bad = good; bad[0] = 0;
    CHECK_EQ(static_cast<int>(parse_datagram(bad.data(), bad.size(), out, payload)), static_cast<int>(ParseError::BadMagic));
    // bad version
    bad = good; put_u16le(bad.data() + 4, 99);
    CHECK_EQ(static_cast<int>(parse_datagram(bad.data(), bad.size(), out, payload)), static_cast<int>(ParseError::BadVersion));
    // length mismatch (payload shorter than fragment_length)
    CHECK_EQ(static_cast<int>(parse_datagram(good.data(), good.size() - 1, out, payload)), static_cast<int>(ParseError::LengthMismatch));
    // fragment_count 0
    bad = good; put_u16le(bad.data() + 38, 0);
    CHECK_EQ(static_cast<int>(parse_datagram(bad.data(), bad.size(), out, payload)), static_cast<int>(ParseError::BadFragmentCount));
    // fragment_index >= count
    bad = good; put_u16le(bad.data() + 36, 1);
    CHECK_EQ(static_cast<int>(parse_datagram(bad.data(), bad.size(), out, payload)), static_cast<int>(ParseError::BadFragmentIndex));
    // single fragment not covering the message
    bad = good; put_u32le(bad.data() + 12, 10);
    CHECK_EQ(static_cast<int>(parse_datagram(bad.data(), bad.size(), out, payload)), static_cast<int>(ParseError::RangeError));
    // fragment beyond message_length
    bad = good; put_u32le(bad.data() + 32, 100); put_u16le(bad.data() + 38, 2); put_u16le(bad.data() + 6, kFlagFragmented);
    CHECK_EQ(static_cast<int>(parse_datagram(bad.data(), bad.size(), out, payload)), static_cast<int>(ParseError::RangeError));
    // fragmented flag without multiple fragments
    bad = good; put_u16le(bad.data() + 6, kFlagFragmented);
    CHECK_EQ(static_cast<int>(parse_datagram(bad.data(), bad.size(), out, payload)), static_cast<int>(ParseError::FlagMismatch));
    // multiple fragments without the flag
    bad = good; put_u16le(bad.data() + 38, 2); put_u32le(bad.data() + 12, 8);
    CHECK_EQ(static_cast<int>(parse_datagram(bad.data(), bad.size(), out, payload)), static_cast<int>(ParseError::FlagMismatch));
}

TEST(wire_fragment_count) {
    CHECK_EQ(fragment_count_for(0, 1356), 1u);
    CHECK_EQ(fragment_count_for(1, 1356), 1u);
    CHECK_EQ(fragment_count_for(1356, 1356), 1u);
    CHECK_EQ(fragment_count_for(1357, 1356), 2u);
    CHECK_EQ(fragment_count_for(2712, 1356), 2u);
    CHECK_EQ(fragment_count_for(2713, 1356), 3u);
    CHECK_EQ(fragment_count_for(100, 0), 0u);
    CHECK_EQ(kDefaultMaxDatagramSize - kHeaderSize, 1356u);
}
