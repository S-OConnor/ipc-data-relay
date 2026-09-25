#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "csv_export.hpp"
#include "ipcrelay/capture_writer.hpp"
#include "ipcrelay/common/byteorder.hpp"
#include "test_framework.hpp"
#include "testpub/telemetry.hpp"

using namespace ipcrelay;
using namespace ipcrelay::capture;
using namespace ipcrelay::testpub;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const char* tag) {
    return fs::temp_directory_path() / ("ipcrelay_unit_csv_" + std::string(tag) + "_" + std::to_string(getpid()));
}

template <typename Data>
std::vector<uint8_t> testpub_payload(const std::string& topic, uint32_t pub, uint64_t idx, DataType type,
                                     const Data& d) {
    const std::size_t len = data_size(type);
    std::vector<uint8_t> p(topic.size() + kTestHeaderSize + len + 4, 0xEE);  // + filler
    std::memcpy(p.data(), topic.data(), topic.size());
    uint8_t* h = p.data() + topic.size();
    put_u32le(h + 0, kTestMagic);
    put_u32le(h + 4, pub);
    put_u64le(h + 8, idx);
    put_u32le(h + 16, static_cast<uint32_t>(p.size()));
    put_u16le(h + 20, type);
    put_u16le(h + 22, static_cast<uint16_t>(len));
    encode(d, h + kTestHeaderSize);
    return p;
}

void add_record(CaptureWriter& w, uint32_t sid, uint64_t seq, const std::vector<uint8_t>& payload) {
    RecordHeader h;
    h.source_id = sid;
    h.sequence = seq;
    h.timestamp_ns = 1000 + seq;
    h.payload_length = static_cast<uint32_t>(payload.size());
    w.write_record(h, payload.data(), payload.size());
}

// Rows of a CSV file as column name -> value.
std::vector<std::map<std::string, std::string>> read_csv(const fs::path& path) {
    std::vector<std::map<std::string, std::string>> rows;
    std::ifstream f(path);
    std::string line;
    std::vector<std::string> header;
    auto split = [](const std::string& s) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string field;
        while (std::getline(ss, field, ',')) out.push_back(field);
        if (!s.empty() && s.back() == ',') out.emplace_back();
        return out;
    };
    if (std::getline(f, line)) header = split(line);
    while (std::getline(f, line)) {
        const std::vector<std::string> fields = split(line);
        std::map<std::string, std::string> row;
        for (std::size_t i = 0; i < header.size() && i < fields.size(); ++i) row[header[i]] = fields[i];
        row["#fields"] = std::to_string(fields.size());
        row["#columns"] = std::to_string(header.size());
        rows.push_back(row);
    }
    return rows;
}

// A capture with two board health records, one of each other type and a
// payload that is not testpub telemetry.
void write_sample_capture(const std::string& path) {
    BoardHealth bh{};
    bh.sample_time_ns = 111;
    for (PowerRailReading& r : bh.rails) r = {1.0f, 0.5f};
    bh.rails[kRail3V3].voltage_v = 3.036f;
    bh.temperature_c[kTempPmic] = 48.25f;
    bh.alarm_flags = (1u << (kAlarmRailBase + kRail3V3)) | (1u << (kAlarmTempBase + kTempFpga));
    bh.sample_count = 7;

    ModeStatus ms{};
    ms.sample_time_ns = 222;
    ms.mode = kModeFault;
    ms.previous_mode = kModeOperational;
    ms.status_flags = kStatusPowerGood | kStatusFanOk | kStatusOverTempWarning;
    ms.fault_code = 0x0301;

    PtpStats ps{};
    ps.sample_time_ns = 333;
    const uint8_t gm[8] = {0xEC, 0x46, 0x70, 0xFF, 0xFE, 0x0A, 0x12, 0x34};
    std::memcpy(ps.grandmaster_identity, gm, sizeof gm);
    ps.offset_from_master_ns = -12;
    ps.current_utc_offset_s = 37;
    ps.port_state = kPtpSlave;
    ps.servo_state = kServoLocked;
    ps.flags = kPtpGmPresent | kPtpTimeTraceable;

    const std::string text = "not testpub";
    CaptureWriter w;
    w.open(path, 0, 1);
    add_record(w, 1, 0, testpub_payload("telemetry", 0, 7, kDataBoardHealth, bh));
    add_record(w, 2, 0, testpub_payload("", 1, 8, kDataModeStatus, ms));
    add_record(w, 3, 0, testpub_payload("", 2, 9, kDataPtpStats, ps));
    add_record(w, 4, 0, std::vector<uint8_t>(text.begin(), text.end()));
    add_record(w, 1, 1, testpub_payload("telemetry", 0, 8, kDataBoardHealth, bh));
    w.close();
}

}  // namespace

TEST(capture_to_csv_one_file_per_type) {
    const fs::path dir = temp_dir("types");
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string cap = (dir / "t.cap").string();
    write_sample_capture(cap);

    const csvexport::ExportResult r = csvexport::export_capture(cap, dir / "csv");
    CHECK(r.error.empty());
    CHECK_EQ(r.rows.size(), 4u);
    CHECK_EQ(r.rows.at("board_health"), 2u);
    CHECK_EQ(r.rows.at("mode_status"), 1u);
    CHECK_EQ(r.rows.at("ptp_stats"), 1u);
    CHECK_EQ(r.rows.at("unknown"), 1u);

    const auto bh = read_csv(dir / "csv" / "board_health.csv");
    REQUIRE(bh.size() == 2);
    CHECK_EQ(bh[0].at("#fields"), bh[0].at("#columns"));
    CHECK_EQ(bh[0].at("source_id"), "1");
    CHECK_EQ(bh[0].at("timestamp_ns"), "1000");
    CHECK_EQ(bh[0].at("message_index"), "7");
    CHECK_EQ(bh[1].at("message_index"), "8");
    CHECK_EQ(bh[0].at("rail_3v3_voltage_v"), "3.036");
    CHECK_EQ(bh[0].at("rail_5v0_current_a"), "0.5");
    CHECK_EQ(bh[0].at("temp_pmic_c"), "48.25");
    CHECK_EQ(bh[0].at("alarms"), "rail_3v3|temp_fpga");
    CHECK_EQ(bh[0].at("sample_count"), "7");

    const auto ms = read_csv(dir / "csv" / "mode_status.csv");
    REQUIRE(ms.size() == 1);
    CHECK_EQ(ms[0].at("#fields"), ms[0].at("#columns"));
    CHECK_EQ(ms[0].at("publisher_index"), "1");
    CHECK_EQ(ms[0].at("mode"), "fault");
    CHECK_EQ(ms[0].at("previous_mode"), "operational");
    CHECK_EQ(ms[0].at("status"), "power_good|fan_ok|over_temp_warning");
    CHECK_EQ(ms[0].at("fault_code"), "0x0301");

    const auto ps = read_csv(dir / "csv" / "ptp_stats.csv");
    REQUIRE(ps.size() == 1);
    CHECK_EQ(ps[0].at("#fields"), ps[0].at("#columns"));
    CHECK_EQ(ps[0].at("grandmaster_identity"), "ec4670.fffe.0a1234");
    CHECK_EQ(ps[0].at("offset_from_master_ns"), "-12");
    CHECK_EQ(ps[0].at("current_utc_offset_s"), "37");
    CHECK_EQ(ps[0].at("port_state"), "slave");
    CHECK_EQ(ps[0].at("servo_state"), "locked");
    CHECK_EQ(ps[0].at("ptp_flag_names"), "gm_present|time_traceable");

    const auto un = read_csv(dir / "csv" / "unknown.csv");
    REQUIRE(un.size() == 1);
    CHECK_EQ(un[0].at("source_id"), "4");
    CHECK_EQ(un[0].at("payload_hex"), "6e6f742074657374707562");  // "not testpub"
    fs::remove_all(dir);
}

TEST(capture_to_csv_keeps_rows_before_truncation) {
    const fs::path dir = temp_dir("trunc");
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path cap = dir / "t.cap";
    write_sample_capture(cap.string());
    fs::resize_file(cap, fs::file_size(cap) - 2);

    const csvexport::ExportResult r = csvexport::export_capture(cap.string(), dir / "csv");
    CHECK(!r.error.empty());
    CHECK_EQ(r.rows.at("board_health"), 1u);
    CHECK_EQ(r.rows.at("unknown"), 1u);
    CHECK_EQ(read_csv(dir / "csv" / "board_health.csv").size(), 1u);
    fs::remove_all(dir);
}

TEST(capture_to_csv_reports_missing_file) {
    const csvexport::ExportResult r = csvexport::export_capture("/nonexistent-dir/x.cap", temp_dir("missing"));
    CHECK(!r.error.empty());
    CHECK(r.rows.empty());
    CHECK(!fs::exists(temp_dir("missing")));
}
