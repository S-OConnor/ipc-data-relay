#include "telemetry.hpp"

#include <cmath>
#include <cstring>

#include "ipcrelay/common/byteorder.hpp"
#include "ipcrelay/common/time_util.hpp"

namespace ipcrelay::testpub {

namespace {

constexpr float kRailNominalV[kNumPowerRails] = {12.0f, 5.0f, 3.3f, 1.8f, 1.0f, 1.2f};
constexpr float kRailTypicalA[kNumPowerRails] = {1.9f, 0.85f, 1.4f, 0.6f, 3.2f, 0.9f};
constexpr float kTempBaseC[kNumTempSensors] = {55.0f, 62.0f, 48.0f, 41.0f};
constexpr float kTempLimitC[kNumTempSensors] = {95.0f, 100.0f, 105.0f, 85.0f};
constexpr uint8_t kGrandmasterId[8] = {0xEC, 0x46, 0x70, 0xFF, 0xFE, 0x0A, 0x12, 0x34};

// Deterministic noise in [-1, 1] (splitmix64 of the message index and a salt).
double noise(uint64_t i, uint64_t salt) {
    uint64_t z = i * 0x9E3779B97F4A7C15ull + salt * 0xBF58476D1CE4E5B9ull + 1;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) / static_cast<double>(1ull << 52) - 1.0;
}

void put_f32le(uint8_t* p, float v) {
    uint32_t u;
    std::memcpy(&u, &v, sizeof u);
    put_u32le(p, u);
}

void put_i64le(uint8_t* p, int64_t v) { put_u64le(p, static_cast<uint64_t>(v)); }

float get_f32le(const uint8_t* p) {
    const uint32_t u = get_u32le(p);
    float v;
    std::memcpy(&v, &u, sizeof v);
    return v;
}

// Mode schedule by message index: boot, standby, then a repeating cycle of
// operational / maintenance / fault / standby.
uint8_t scheduled_mode(uint64_t i) {
    if (i < 10) return kModeBoot;
    if (i < 50) return kModeStandby;
    const uint64_t c = (i - 50) % 5000;
    if (c < 4000) return kModeOperational;
    if (c < 4200) return kModeMaintenance;
    if (c < 4250) return kModeFault;
    return kModeStandby;
}

}  // namespace

std::size_t data_size(DataType type) {
    switch (type) {
        case kDataBoardHealth: return kBoardHealthSize;
        case kDataModeStatus: return kModeStatusSize;
        case kDataPtpStats: return kPtpStatsSize;
    }
    return 0;
}

Simulator::Simulator(DataType type)
    : type_(type), start_mono_ns_(now_monotonic_ns()), mode_since_mono_ns_(start_mono_ns_) {}

void Simulator::encode_next(uint64_t msg_index, uint8_t* out) {
    const uint64_t now_ns = now_realtime_ns();
    switch (type_) {
        case kDataBoardHealth: encode(next_board_health(msg_index, now_ns), out); break;
        case kDataModeStatus: encode(next_mode_status(msg_index, now_ns), out); break;
        case kDataPtpStats: encode(next_ptp_stats(msg_index, now_ns), out); break;
    }
}

BoardHealth Simulator::next_board_health(uint64_t i, uint64_t now_ns) {
    BoardHealth d{};
    d.sample_time_ns = now_ns;
    const double t = static_cast<double>(i);
    const double load = 0.5 + 0.5 * std::sin(t / 400.0);  // 0..1
    // Every 3000 samples the 3V3 rail droops for 5 samples and raises an alarm.
    const bool droop = i % 3000 >= 2995;
    for (int r = 0; r < kNumPowerRails; ++r) {
        double v = kRailNominalV[r] * (1.0 - 0.01 * load + 0.004 * noise(i, 10u + static_cast<unsigned>(r)));
        if (droop && r == kRail3V3) v *= 0.92;
        const double a = kRailTypicalA[r] * (0.8 + 0.4 * load + 0.02 * noise(i, 20u + static_cast<unsigned>(r)));
        d.rails[r].voltage_v = static_cast<float>(v);
        d.rails[r].current_a = static_cast<float>(a);
        if (std::fabs(v - kRailNominalV[r]) > 0.05 * kRailNominalV[r]) {
            d.alarm_flags |= 1u << (kAlarmRailBase + static_cast<unsigned>(r));
        }
    }
    const double heat = 0.5 + 0.5 * std::sin(t / 2000.0);
    for (int s = 0; s < kNumTempSensors; ++s) {
        const double c = kTempBaseC[s] + 12.0 * heat + 4.0 * load + 0.3 * noise(i, 30u + static_cast<unsigned>(s));
        d.temperature_c[s] = static_cast<float>(c);
        if (c > kTempLimitC[s]) d.alarm_flags |= 1u << (kAlarmTempBase + static_cast<unsigned>(s));
    }
    d.sample_count = static_cast<uint32_t>(i);
    return d;
}

ModeStatus Simulator::next_mode_status(uint64_t i, uint64_t now_ns) {
    const uint64_t mono = now_monotonic_ns();
    const uint8_t mode = scheduled_mode(i);
    if (mode != mode_) {
        previous_mode_ = mode_;
        mode_ = mode;
        ++mode_changes_;
        mode_since_mono_ns_ = mono;
    }
    ModeStatus d{};
    d.sample_time_ns = now_ns;
    d.uptime_s = static_cast<uint32_t>((mono - start_mono_ns_) / kNsPerSec);
    d.mode = mode_;
    d.previous_mode = previous_mode_;
    d.mode_change_count = mode_changes_;
    d.status_flags = kStatusFanOk;
    if (mode_ != kModeFault) d.status_flags |= kStatusPowerGood;
    if (mode_ != kModeBoot) d.status_flags |= kStatusLinkUp | kStatusBitPassed;
    if (i >= 50) d.status_flags |= kStatusClockLocked;
    if (mode_ == kModeFault) {
        d.status_flags |= kStatusOverTempWarning;
        d.fault_code = 0x0301;  // over-temperature shutdown
    }
    d.time_in_mode_s = static_cast<uint32_t>((mono - mode_since_mono_ns_) / kNsPerSec);
    d.heartbeat = static_cast<uint32_t>(i);
    return d;
}

PtpStats Simulator::next_ptp_stats(uint64_t i, uint64_t now_ns) {
    PtpStats d{};
    d.sample_time_ns = now_ns;
    std::memcpy(d.grandmaster_identity, kGrandmasterId, sizeof kGrandmasterId);
    const double t = static_cast<double>(i);
    double offset;
    if (i < 20) {
        d.port_state = kPtpListening;
        d.servo_state = kServoUnlocked;
        offset = 0.0;
    } else if (i < 100) {
        // Stepped the clock at i == 20, now converging.
        d.port_state = kPtpUncalibrated;
        d.servo_state = i == 20 ? kServoJump : kServoUnlocked;
        offset = 25000.0 * std::exp(-(t - 20.0) / 15.0);
    } else {
        d.port_state = kPtpSlave;
        d.servo_state = kServoLocked;
        offset = 12.0 * std::sin(t / 50.0);
    }
    if (i >= 20) {
        d.flags = kPtpGmPresent | kPtpUtcOffsetValid | kPtpTimeTraceable | kPtpFreqTraceable;
        offset += 8.0 * noise(i, 40);
    }
    d.offset_from_master_ns = static_cast<int64_t>(std::llround(offset));
    d.mean_path_delay_ns = static_cast<int64_t>(std::llround(850.0 + 6.0 * std::sin(t / 300.0) + 2.0 * noise(i, 41)));
    d.freq_adjustment_ppb = static_cast<int32_t>(std::lround(-12500.0 + 40.0 * std::sin(t / 1000.0) + 0.3 * offset));
    d.steps_removed = 1;
    d.current_utc_offset_s = 37;
    d.gm_clock_class = 6;  // locked to a primary reference (GNSS)
    d.sync_rx_count = static_cast<uint32_t>(i);
    d.announce_timeout_count = 0;
    return d;
}

void encode(const BoardHealth& d, uint8_t* out) {
    put_u64le(out + 0, d.sample_time_ns);
    uint8_t* p = out + 8;
    for (const PowerRailReading& r : d.rails) {
        put_f32le(p, r.voltage_v);
        put_f32le(p + 4, r.current_a);
        p += 8;
    }
    for (float c : d.temperature_c) {
        put_f32le(p, c);
        p += 4;
    }
    put_u32le(p, d.alarm_flags);
    put_u32le(p + 4, d.sample_count);
}

void encode(const ModeStatus& d, uint8_t* out) {
    put_u64le(out + 0, d.sample_time_ns);
    put_u32le(out + 8, d.uptime_s);
    put_u8(out + 12, d.mode);
    put_u8(out + 13, d.previous_mode);
    put_u16le(out + 14, d.mode_change_count);
    put_u32le(out + 16, d.status_flags);
    put_u32le(out + 20, d.fault_code);
    put_u32le(out + 24, d.time_in_mode_s);
    put_u32le(out + 28, d.heartbeat);
}

void encode(const PtpStats& d, uint8_t* out) {
    put_u64le(out + 0, d.sample_time_ns);
    std::memcpy(out + 8, d.grandmaster_identity, sizeof d.grandmaster_identity);
    put_i64le(out + 16, d.offset_from_master_ns);
    put_i64le(out + 24, d.mean_path_delay_ns);
    put_u32le(out + 32, static_cast<uint32_t>(d.freq_adjustment_ppb));
    put_u16le(out + 36, d.steps_removed);
    put_u16le(out + 38, static_cast<uint16_t>(d.current_utc_offset_s));
    put_u8(out + 40, d.port_state);
    put_u8(out + 41, d.servo_state);
    put_u8(out + 42, d.gm_clock_class);
    put_u8(out + 43, d.flags);
    put_u32le(out + 44, d.sync_rx_count);
    put_u32le(out + 48, d.announce_timeout_count);
    put_u32le(out + 52, d.reserved);
}

void decode(const uint8_t* in, TestHeader& h) {
    h.magic = get_u32le(in + 0);
    h.publisher_index = get_u32le(in + 4);
    h.message_index = get_u64le(in + 8);
    h.payload_length = get_u32le(in + 16);
    h.data_type = get_u16le(in + 20);
    h.data_length = get_u16le(in + 22);
}

void decode(const uint8_t* in, BoardHealth& d) {
    d.sample_time_ns = get_u64le(in + 0);
    const uint8_t* p = in + 8;
    for (PowerRailReading& r : d.rails) {
        r.voltage_v = get_f32le(p);
        r.current_a = get_f32le(p + 4);
        p += 8;
    }
    for (float& c : d.temperature_c) {
        c = get_f32le(p);
        p += 4;
    }
    d.alarm_flags = get_u32le(p);
    d.sample_count = get_u32le(p + 4);
}

void decode(const uint8_t* in, ModeStatus& d) {
    d.sample_time_ns = get_u64le(in + 0);
    d.uptime_s = get_u32le(in + 8);
    d.mode = get_u8(in + 12);
    d.previous_mode = get_u8(in + 13);
    d.mode_change_count = get_u16le(in + 14);
    d.status_flags = get_u32le(in + 16);
    d.fault_code = get_u32le(in + 20);
    d.time_in_mode_s = get_u32le(in + 24);
    d.heartbeat = get_u32le(in + 28);
}

void decode(const uint8_t* in, PtpStats& d) {
    d.sample_time_ns = get_u64le(in + 0);
    std::memcpy(d.grandmaster_identity, in + 8, sizeof d.grandmaster_identity);
    d.offset_from_master_ns = static_cast<int64_t>(get_u64le(in + 16));
    d.mean_path_delay_ns = static_cast<int64_t>(get_u64le(in + 24));
    d.freq_adjustment_ppb = static_cast<int32_t>(get_u32le(in + 32));
    d.steps_removed = get_u16le(in + 36);
    d.current_utc_offset_s = static_cast<int16_t>(get_u16le(in + 38));
    d.port_state = get_u8(in + 40);
    d.servo_state = get_u8(in + 41);
    d.gm_clock_class = get_u8(in + 42);
    d.flags = get_u8(in + 43);
    d.sync_rx_count = get_u32le(in + 44);
    d.announce_timeout_count = get_u32le(in + 48);
    d.reserved = get_u32le(in + 52);
}

}  // namespace ipcrelay::testpub
