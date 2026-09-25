#include "csv_export.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <type_traits>
#include <vector>

#include "ipcrelay/capture_reader.hpp"
#include "testpub/telemetry.hpp"

namespace ipcrelay::csvexport {

namespace {

using namespace ipcrelay::testpub;

const char* const kRailNames[kNumPowerRails] = {"12v0_in", "5v0", "3v3", "1v8", "1v0_core", "1v2_ddr"};
const char* const kTempNames[kNumTempSensors] = {"cpu", "fpga", "pmic", "board"};
const char* const kModeNames[] = {"boot", "standby", "operational", "maintenance", "fault"};
const char* const kStatusFlagNames[] = {"power_good", "clock_locked", "link_up", "bit_passed", "fan_ok",
                                        "over_temp_warning"};
const char* const kPortStateNames[] = {"", "initializing", "faulty", "disabled", "listening", "pre_master",
                                       "master", "passive", "uncalibrated", "slave"};
const char* const kServoStateNames[] = {"unlocked", "jump", "locked"};
const char* const kPtpFlagNames[] = {"gm_present", "utc_offset_valid", "time_traceable", "freq_traceable"};

const char* const kRecordColumns = "source_id,sequence,timestamp_ns,flags";

template <std::size_t N>
std::string enum_name(const char* const (&names)[N], unsigned value) {
    if (value < N && names[value][0] != '\0') return names[value];
    return "unknown_" + std::to_string(value);
}

// Appends prefix + names[i] for every bit (base + i) set in value, '|'-separated.
template <std::size_t N>
void append_flag_names(std::string& out, const char* const (&names)[N], uint32_t value, unsigned base = 0,
                       const char* prefix = "") {
    for (std::size_t i = 0; i < N; ++i) {
        if (value & (1u << (base + i))) {
            if (!out.empty()) out += '|';
            out += prefix;
            out += names[i];
        }
    }
}

template <std::size_t N>
std::string flag_names(const char* const (&names)[N], uint32_t value) {
    std::string out;
    append_flag_names(out, names, value);
    return out;
}

std::string hex(const uint8_t* p, std::size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
        out += digits[p[i] >> 4];
        out += digits[p[i] & 0x0F];
    }
    return out;
}

// One CSV line. Fields never contain commas, quotes or newlines.
class Row {
public:
    template <typename T>
    Row& operator<<(T v) {
        static_assert(std::is_integral_v<T>, "use the float or string overloads");
        if constexpr (std::is_signed_v<T>) {
            return field(std::to_string(static_cast<long long>(v)));
        } else {
            return field(std::to_string(static_cast<unsigned long long>(v)));
        }
    }
    // Shortest decimal that reads back as the same float.
    Row& operator<<(float v) {
        char buf[32];
        auto res = std::to_chars(buf, buf + sizeof buf, v);
        return field(std::string(buf, res.ptr));
    }
    Row& operator<<(const std::string& s) { return field(s); }
    Row& operator<<(const char* s) { return field(s); }

    const std::string& str() const { return line_; }

private:
    Row& field(const std::string& s) {
        if (!first_) line_ += ',';
        first_ = false;
        line_ += s;
        return *this;
    }

    std::string line_;
    bool first_ = true;
};

std::string board_health_columns() {
    std::string c = "sample_time_ns";
    for (const char* r : kRailNames) {
        c += std::string(",rail_") + r + "_voltage_v,rail_" + r + "_current_a";
    }
    for (const char* t : kTempNames) c += std::string(",temp_") + t + "_c";
    return c + ",alarm_flags,alarms,sample_count";
}

void add_fields(Row& row, const BoardHealth& d) {
    row << d.sample_time_ns;
    for (const PowerRailReading& r : d.rails) row << r.voltage_v << r.current_a;
    for (float c : d.temperature_c) row << c;
    std::string alarms;
    append_flag_names(alarms, kRailNames, d.alarm_flags, kAlarmRailBase, "rail_");
    append_flag_names(alarms, kTempNames, d.alarm_flags, kAlarmTempBase, "temp_");
    row << d.alarm_flags << alarms << d.sample_count;
}

const char* const kModeStatusColumns =
    "sample_time_ns,uptime_s,mode,previous_mode,mode_change_count,status_flags,status,fault_code,"
    "time_in_mode_s,heartbeat";

void add_fields(Row& row, const ModeStatus& d) {
    char fault[16];
    std::snprintf(fault, sizeof fault, "0x%04X", static_cast<unsigned>(d.fault_code));
    row << d.sample_time_ns << d.uptime_s << enum_name(kModeNames, d.mode) << enum_name(kModeNames, d.previous_mode)
        << d.mode_change_count << d.status_flags << flag_names(kStatusFlagNames, d.status_flags) << std::string(fault)
        << d.time_in_mode_s << d.heartbeat;
}

const char* const kPtpStatsColumns =
    "sample_time_ns,grandmaster_identity,offset_from_master_ns,mean_path_delay_ns,freq_adjustment_ppb,"
    "steps_removed,current_utc_offset_s,port_state,servo_state,gm_clock_class,ptp_flags,ptp_flag_names,"
    "sync_rx_count,announce_timeout_count";

void add_fields(Row& row, const PtpStats& d) {
    const uint8_t* gm = d.grandmaster_identity;
    // ptp4l clockIdentity style: xxxxxx.xxxx.xxxxxx
    const std::string gm_id = hex(gm, 3) + "." + hex(gm + 3, 2) + "." + hex(gm + 5, 3);
    row << d.sample_time_ns << gm_id << d.offset_from_master_ns << d.mean_path_delay_ns << d.freq_adjustment_ppb
        << d.steps_removed << d.current_utc_offset_s << enum_name(kPortStateNames, d.port_state)
        << enum_name(kServoStateNames, d.servo_state) << d.gm_clock_class << d.flags
        << flag_names(kPtpFlagNames, d.flags) << d.sync_rx_count << d.announce_timeout_count;
}

// Opens <out_dir>/<stem>.csv on first use and writes the header line.
class CsvFiles {
public:
    explicit CsvFiles(std::filesystem::path out_dir) : out_dir_(std::move(out_dir)) {}

    // Returns false and sets error() if the file cannot be created or written.
    bool write(const std::string& stem, const std::string& columns, const Row& row) {
        auto it = files_.find(stem);
        if (it == files_.end()) {
            std::error_code ec;
            std::filesystem::create_directories(out_dir_, ec);
            if (ec) return fail("create '" + out_dir_.string() + "': " + ec.message());
            const std::filesystem::path path = out_dir_ / (stem + ".csv");
            std::ofstream f(path, std::ios::trunc);
            if (!f) return fail("open '" + path.string() + "': " + std::strerror(errno));
            f << columns << '\n';
            it = files_.emplace(stem, std::move(f)).first;
            rows_[stem] = 0;
        }
        if (!(it->second << row.str() << '\n')) return fail("write '" + stem + ".csv' failed");
        ++rows_[stem];
        return true;
    }

    bool close() {
        for (auto& [stem, f] : files_) {
            f.close();
            if (!f) return fail("close '" + stem + ".csv' failed");
        }
        return true;
    }

    const std::map<std::string, uint64_t>& rows() const { return rows_; }
    const std::string& error() const { return error_; }

private:
    bool fail(std::string msg) {
        if (error_.empty()) error_ = std::move(msg);
        return false;
    }

    std::filesystem::path out_dir_;
    std::map<std::string, std::ofstream> files_;
    std::map<std::string, uint64_t> rows_;
    std::string error_;
};

template <typename Data>
void add_decoded(Row& row, const uint8_t* in) {
    Data d{};
    decode(in, d);
    add_fields(row, d);
}

// Appends the testpub header and decoded data to row and sets the CSV it
// belongs to. Returns false (row untouched) if the payload is not testpub
// telemetry.
bool decode_testpub(const std::vector<uint8_t>& payload, Row& row, std::string& stem, std::string& columns) {
    // The topic prefix length is unknown in general; locate the TEST magic.
    const uint8_t magic[4] = {0x54, 0x45, 0x53, 0x54};
    const auto it = std::search(payload.begin(), payload.end(), magic, magic + 4);
    if (it == payload.end()) return false;
    const std::size_t idx = static_cast<std::size_t>(it - payload.begin());
    if (payload.size() < idx + kTestHeaderSize) return false;
    TestHeader h{};
    decode(payload.data() + idx, h);
    const std::size_t len = data_size(static_cast<DataType>(h.data_type));
    const std::size_t start = idx + kTestHeaderSize;
    if (len == 0 || h.payload_length != payload.size() || h.data_length != len || payload.size() < start + len) {
        return false;
    }
    row << h.publisher_index << h.message_index;
    const uint8_t* data = payload.data() + start;
    columns = std::string(kRecordColumns) + ",publisher_index,message_index,";
    switch (h.data_type) {
        case kDataBoardHealth:
            add_decoded<BoardHealth>(row, data);
            stem = "board_health";
            columns += board_health_columns();
            break;
        case kDataModeStatus:
            add_decoded<ModeStatus>(row, data);
            stem = "mode_status";
            columns += kModeStatusColumns;
            break;
        case kDataPtpStats:
            add_decoded<PtpStats>(row, data);
            stem = "ptp_stats";
            columns += kPtpStatsColumns;
            break;
    }
    return true;
}

}  // namespace

ExportResult export_capture(const std::string& capture_path, const std::filesystem::path& out_dir) {
    ExportResult result;
    capture::CaptureReader reader;
    if (!reader.open(capture_path)) {
        result.error = reader.error();
        return result;
    }
    CsvFiles out(out_dir);
    capture::RecordHeader hdr;
    std::vector<uint8_t> payload;
    std::string stem;
    std::string columns;
    while (reader.next(hdr, payload)) {
        Row row;
        row << hdr.source_id << hdr.sequence << hdr.timestamp_ns << hdr.flags;
        if (!decode_testpub(payload, row, stem, columns)) {
            row << payload.size() << hex(payload.data(), payload.size());
            stem = "unknown";
            columns = std::string(kRecordColumns) + ",payload_length,payload_hex";
        }
        if (!out.write(stem, columns, row)) break;
    }
    out.close();
    result.rows = out.rows();
    result.error = !out.error().empty() ? out.error() : reader.error();
    return result;
}

}  // namespace ipcrelay::csvexport
