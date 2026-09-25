#pragma once

#include <cstdint>

namespace ipcrelay {

// CLOCK_REALTIME in nanoseconds since the Unix epoch (BRG-048, BRG-049).
uint64_t now_realtime_ns();
// CLOCK_MONOTONIC in nanoseconds; used for timeouts and intervals.
uint64_t now_monotonic_ns();

constexpr uint64_t kNsPerMs = 1000000ull;
constexpr uint64_t kNsPerSec = 1000000000ull;

}  // namespace ipcrelay
