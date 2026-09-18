// Per-source sequence gap detection (BRG-073).
#pragma once

#include <cstdint>
#include <map>

namespace ipcrelay {

struct SourceSequenceState {
    bool seen = false;
    uint64_t last_sequence = 0;
    uint64_t gap_events = 0;     // number of times a forward jump was observed
    uint64_t missing = 0;        // total sequence numbers skipped
    uint64_t out_of_order = 0;   // sequence <= last (late, duplicate or restart)
};

class SequenceTracker {
public:
    enum class Outcome { First, InOrder, Gap, OutOfOrder };

    // Records the observation of sequence `seq` for `source_id`.
    // If a gap was detected, `missing` receives the number of skipped values.
    Outcome observe(uint32_t source_id, uint64_t seq, uint64_t& missing);

    const std::map<uint32_t, SourceSequenceState>& sources() const { return sources_; }

private:
    std::map<uint32_t, SourceSequenceState> sources_;
};

}  // namespace ipcrelay
