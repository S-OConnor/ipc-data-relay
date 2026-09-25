#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

#include "ipcrelay/capture_format.hpp"
#include "ipcrelay/capture_reader.hpp"
#include "ipcrelay/capture_writer.hpp"
#include "ipcrelay/common/byteorder.hpp"
#include "test_framework.hpp"

using namespace ipcrelay;
using namespace ipcrelay::capture;

static std::string temp_path(const char* tag) {
    const char* dir = std::getenv("TMPDIR");
    std::string p = std::string(dir ? dir : "/tmp") + "/ipcrelay_unit_" + tag + "_" + std::to_string(getpid()) + ".cap";
    return p;
}

TEST(capture_header_layout) {
    FileHeader fh;
    fh.created_ns = 0x1122334455667788ull;
    fh.wire_version = 1;
    uint8_t b[kFileHeaderSize];
    encode_file_header(fh, b);
    CHECK_EQ(b[0], 0x5A); CHECK_EQ(b[1], 0x4D); CHECK_EQ(b[2], 0x43); CHECK_EQ(b[3], 0x50);  // "ZMCP"
    CHECK_EQ(get_u16le(b + 4), kFormatVersion);
    CHECK_EQ(get_u16le(b + 6), 32);
    CHECK_EQ(get_u64le(b + 8), 0x1122334455667788ull);
    CHECK_EQ(get_u16le(b + 16), 1);
    for (std::size_t i = 18; i < kFileHeaderSize; ++i) CHECK_EQ(b[i], 0);

    RecordHeader rh;
    rh.source_id = 9;
    rh.sequence = 0x0102030405060708ull;
    rh.timestamp_ns = 77;
    rh.payload_length = 3;
    rh.flags = 2;
    uint8_t r[kRecordHeaderSize];
    encode_record_header(rh, r);
    CHECK_EQ(r[0], 0x5A); CHECK_EQ(r[1], 0x4D); CHECK_EQ(r[2], 0x43); CHECK_EQ(r[3], 0x52);  // "ZMCR"
    CHECK_EQ(get_u32le(r + 4), 9u);
    CHECK_EQ(get_u64le(r + 8), 0x0102030405060708ull);
    CHECK_EQ(get_u64le(r + 16), 77u);
    CHECK_EQ(get_u32le(r + 24), 3u);
    CHECK_EQ(get_u16le(r + 28), 2);
    CHECK_EQ(get_u16le(r + 30), 0);

    FileHeader fh2;
    CHECK(decode_file_header(b, sizeof b, fh2));
    CHECK_EQ(fh2.created_ns, fh.created_ns);
    b[0] = 0;
    CHECK(!decode_file_header(b, sizeof b, fh2));
    RecordHeader rh2;
    CHECK(decode_record_header(r, sizeof r, rh2));
    CHECK_EQ(rh2.sequence, rh.sequence);
    CHECK(!decode_record_header(r, 10, rh2));
}

TEST(capture_writer_reader_roundtrip) {
    std::string path = temp_path("rt");
    {
        CaptureWriter w;
        REQUIRE(w.open(path, 4096, 1));
        for (uint32_t i = 0; i < 50; ++i) {
            RecordHeader h;
            h.source_id = i % 3;
            h.sequence = i;
            h.timestamp_ns = 1000 + i;
            h.flags = i % 2;
            std::vector<uint8_t> payload(i * 10);
            for (std::size_t k = 0; k < payload.size(); ++k) payload[k] = static_cast<uint8_t>(i + k);
            REQUIRE(w.write_record(h, payload.data(), payload.size()));
        }
        CHECK_EQ(w.records_written(), 50u);
        CHECK(w.flush(true));
        CHECK(w.close());
        CHECK(!w.failed());
    }
    CaptureReader r;
    REQUIRE(r.open(path));
    CHECK_EQ(r.file_header().wire_version, 1);
    RecordHeader h;
    std::vector<uint8_t> payload;
    uint32_t n = 0;
    while (r.next(h, payload)) {
        CHECK_EQ(h.sequence, n);
        CHECK_EQ(h.source_id, n % 3);
        CHECK_EQ(h.timestamp_ns, 1000u + n);
        CHECK_EQ(h.flags, n % 2);
        REQUIRE(payload.size() == n * 10);
        for (std::size_t k = 0; k < payload.size(); ++k) CHECK_EQ(payload[k], static_cast<uint8_t>(n + k));
        ++n;
    }
    CHECK(r.error().empty());
    CHECK_EQ(n, 50u);
    std::remove(path.c_str());
}

TEST(capture_writer_reports_open_error) {
    CaptureWriter w;
    CHECK(!w.open("/nonexistent-dir/x/y.cap", 0, 1));
    CHECK(w.failed());
    CHECK(!w.last_error().empty());
    CHECK(!w.is_open());
}

TEST(capture_reader_detects_truncation) {
    std::string path = temp_path("trunc");
    {
        CaptureWriter w;
        REQUIRE(w.open(path, 0, 1));
        RecordHeader h;
        std::vector<uint8_t> p(100, 1);
        REQUIRE(w.write_record(h, p.data(), p.size()));
        REQUIRE(w.close());
    }
    // Chop the file so the payload is incomplete.
    REQUIRE(truncate(path.c_str(), static_cast<off_t>(kFileHeaderSize + kRecordHeaderSize + 50)) == 0);
    CaptureReader r;
    REQUIRE(r.open(path));
    RecordHeader h;
    std::vector<uint8_t> payload;
    CHECK(!r.next(h, payload));
    CHECK(!r.error().empty());
    std::remove(path.c_str());
}
