#include "ipcrelay/sequence_tracker.hpp"

namespace ipcrelay {

SequenceTracker::Outcome SequenceTracker::observe(uint32_t source_id, uint64_t seq, uint64_t& missing) {
    missing = 0;
    SourceSequenceState& s = sources_[source_id];
    if (!s.seen) {
        s.seen = true;
        s.last_sequence = seq;
        return Outcome::First;
    }
    const uint64_t expected = s.last_sequence + 1;
    if (seq == expected) {
        s.last_sequence = seq;
        return Outcome::InOrder;
    }
    if (seq > expected) {
        missing = seq - expected;
        s.missing += missing;
        ++s.gap_events;
        s.last_sequence = seq;
        return Outcome::Gap;
    }
    // seq <= last_sequence: late/duplicate delivery or the bridge restarted.
    ++s.out_of_order;
    if (seq + 1000 < s.last_sequence || seq == 0) {
        // Large backwards jump: assume the source restarted and resynchronise.
        s.last_sequence = seq;
    }
    return Outcome::OutOfOrder;
}

}  // namespace ipcrelay
