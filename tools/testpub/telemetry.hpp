// Simulated application data published by ipc-relay-testpub.
//
// Each IPC channel carries one of the structures below, chosen by the
// endpoint's position on the command line (see data_type_for_endpoint()).
// The structures are plain C-style records as an embedded board API would
// return them. They are serialized field by field in declaration order,
// little-endian, IEEE-754 floats, no padding; every field is naturally
// aligned so the encoded size equals sizeof() and a little-endian consumer
// may memcpy the bytes straight into the struct.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ipcrelay::testpub {

// Header in front of the data structure in every testpub payload, after the
// optional topic prefix (see main.cpp for the full payload layout).
constexpr uint32_t kTestMagic = 0x54534554u;  // "TEST"
constexpr std::size_t kTestHeaderSize = 24;

struct TestHeader {
    uint32_t magic;
    uint32_t publisher_index;
    uint64_t message_index;
    uint32_t payload_length;  // including the topic prefix
    uint16_t data_type;       // DataType
    uint16_t data_length;
};

enum DataType : uint16_t {
    kDataBoardHealth = 1,
    kDataModeStatus = 2,
    kDataPtpStats = 3,
};
constexpr uint16_t kNumDataTypes = 3;

// ---------------------------------------------------------------------------
// 1. Board health: voltage, current and temperature (80 bytes)

enum PowerRail : uint8_t {
    kRail12V0In = 0,  // board input
    kRail5V0 = 1,
    kRail3V3 = 2,
    kRail1V8 = 3,
    kRail1V0Core = 4,  // SoC core
    kRail1V2Ddr = 5,
    kNumPowerRails = 6,
};

enum TempSensor : uint8_t {
    kTempCpu = 0,
    kTempFpga = 1,
    kTempPmic = 2,
    kTempBoard = 3,
    kNumTempSensors = 4,
};

// alarm_flags: bit r = rail r outside +/-5% of nominal,
//              bit 16 + t = sensor t above its limit.
constexpr uint32_t kAlarmRailBase = 0;
constexpr uint32_t kAlarmTempBase = 16;

struct PowerRailReading {
    float voltage_v;
    float current_a;
};

struct BoardHealth {
    uint64_t sample_time_ns;  // CLOCK_REALTIME
    PowerRailReading rails[kNumPowerRails];
    float temperature_c[kNumTempSensors];
    uint32_t alarm_flags;
    uint32_t sample_count;
};

// ---------------------------------------------------------------------------
// 2. Mode and status (32 bytes)

enum SystemMode : uint8_t {
    kModeBoot = 0,
    kModeStandby = 1,
    kModeOperational = 2,
    kModeMaintenance = 3,
    kModeFault = 4,
};

enum StatusFlag : uint32_t {
    kStatusPowerGood = 1u << 0,
    kStatusClockLocked = 1u << 1,
    kStatusLinkUp = 1u << 2,
    kStatusBitPassed = 1u << 3,  // built-in test
    kStatusFanOk = 1u << 4,
    kStatusOverTempWarning = 1u << 5,
};

struct ModeStatus {
    uint64_t sample_time_ns;  // CLOCK_REALTIME
    uint32_t uptime_s;
    uint8_t mode;           // SystemMode
    uint8_t previous_mode;  // SystemMode
    uint16_t mode_change_count;
    uint32_t status_flags;  // StatusFlag bits
    uint32_t fault_code;    // 0 = no fault
    uint32_t time_in_mode_s;
    uint32_t heartbeat;
};

// ---------------------------------------------------------------------------
// 3. PTP (IEEE 1588) statistics, as reported by a ptp4l-style daemon (56 bytes)

enum PtpPortState : uint8_t {  // IEEE 1588-2008 portState
    kPtpInitializing = 1,
    kPtpFaulty = 2,
    kPtpDisabled = 3,
    kPtpListening = 4,
    kPtpPreMaster = 5,
    kPtpMaster = 6,
    kPtpPassive = 7,
    kPtpUncalibrated = 8,
    kPtpSlave = 9,
};

enum PtpServoState : uint8_t {
    kServoUnlocked = 0,
    kServoJump = 1,
    kServoLocked = 2,
};

enum PtpFlag : uint8_t {
    kPtpGmPresent = 1u << 0,
    kPtpUtcOffsetValid = 1u << 1,
    kPtpTimeTraceable = 1u << 2,
    kPtpFreqTraceable = 1u << 3,
};

struct PtpStats {
    uint64_t sample_time_ns;          // CLOCK_REALTIME
    uint8_t grandmaster_identity[8];  // clockIdentity (EUI-64)
    int64_t offset_from_master_ns;
    int64_t mean_path_delay_ns;
    int32_t freq_adjustment_ppb;
    uint16_t steps_removed;
    int16_t current_utc_offset_s;
    uint8_t port_state;   // PtpPortState
    uint8_t servo_state;  // PtpServoState
    uint8_t gm_clock_class;
    uint8_t flags;  // PtpFlag bits
    uint32_t sync_rx_count;
    uint32_t announce_timeout_count;
    uint32_t reserved;
};

constexpr std::size_t kBoardHealthSize = 80;
constexpr std::size_t kModeStatusSize = 32;
constexpr std::size_t kPtpStatsSize = 56;
static_assert(sizeof(BoardHealth) == kBoardHealthSize, "BoardHealth has padding");
static_assert(sizeof(ModeStatus) == kModeStatusSize, "ModeStatus has padding");
static_assert(sizeof(PtpStats) == kPtpStatsSize, "PtpStats has padding");

// 1st endpoint -> board health, 2nd -> mode/status, 3rd -> PTP; repeats for more.
inline DataType data_type_for_endpoint(std::size_t endpoint_index) {
    return static_cast<DataType>(1 + endpoint_index % kNumDataTypes);
}

// Encoded size of a data type in bytes.
std::size_t data_size(DataType type);

// Generates plausible, slowly varying readings for one channel. Values are a
// deterministic function of the message index; only the timestamps and
// uptime come from the clock.
class Simulator {
public:
    explicit Simulator(DataType type);

    DataType type() const { return type_; }
    std::size_t size() const { return data_size(type_); }

    // Writes size() bytes for message msg_index to out.
    void encode_next(uint64_t msg_index, uint8_t* out);

private:
    BoardHealth next_board_health(uint64_t i, uint64_t now_ns);
    ModeStatus next_mode_status(uint64_t i, uint64_t now_ns);
    PtpStats next_ptp_stats(uint64_t i, uint64_t now_ns);

    DataType type_;
    uint64_t start_mono_ns_;
    uint8_t mode_ = kModeBoot;
    uint8_t previous_mode_ = kModeBoot;
    uint16_t mode_changes_ = 0;
    uint64_t mode_since_mono_ns_;
};

void encode(const BoardHealth& d, uint8_t* out);
void encode(const ModeStatus& d, uint8_t* out);
void encode(const PtpStats& d, uint8_t* out);

// Inverse of encode(); in holds data_size() (or kTestHeaderSize) bytes.
void decode(const uint8_t* in, TestHeader& h);
void decode(const uint8_t* in, BoardHealth& d);
void decode(const uint8_t* in, ModeStatus& d);
void decode(const uint8_t* in, PtpStats& d);

}  // namespace ipcrelay::testpub
