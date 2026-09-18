#include "ipcrelay/byteorder.hpp"
#include "test_framework.hpp"

using namespace ipcrelay;

TEST(byteorder_little_endian_layout) {
    uint8_t buf[8];
    put_u16le(buf, 0x1234);
    CHECK_EQ(buf[0], 0x34);
    CHECK_EQ(buf[1], 0x12);
    put_u32le(buf, 0x11223344u);
    CHECK_EQ(buf[0], 0x44);
    CHECK_EQ(buf[3], 0x11);
    put_u64le(buf, 0x0102030405060708ull);
    CHECK_EQ(buf[0], 0x08);
    CHECK_EQ(buf[7], 0x01);
}

TEST(byteorder_roundtrip) {
    uint8_t buf[8];
    put_u16le(buf, 0xBEEF);
    CHECK_EQ(get_u16le(buf), 0xBEEF);
    put_u32le(buf, 0xDEADBEEFu);
    CHECK_EQ(get_u32le(buf), 0xDEADBEEFu);
    put_u64le(buf, 0xFEDCBA9876543210ull);
    CHECK_EQ(get_u64le(buf), 0xFEDCBA9876543210ull);
}
