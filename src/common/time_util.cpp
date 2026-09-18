#include "ipcrelay/time_util.hpp"

#include <ctime>

namespace ipcrelay {

static uint64_t clock_ns(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * kNsPerSec + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t now_realtime_ns() { return clock_ns(CLOCK_REALTIME); }
uint64_t now_monotonic_ns() { return clock_ns(CLOCK_MONOTONIC); }

}  // namespace ipcrelay
