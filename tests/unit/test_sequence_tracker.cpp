#include "ipcrelay/sequence_tracker.hpp"
#include "test_framework.hpp"

using namespace ipcrelay;
using O = SequenceTracker::Outcome;

TEST(sequence_in_order_and_gaps) {
    SequenceTracker t;
    uint64_t missing = 0;
    CHECK(t.observe(1, 0, missing) == O::First);
    CHECK(t.observe(1, 1, missing) == O::InOrder);
    CHECK(t.observe(1, 2, missing) == O::InOrder);
    CHECK(t.observe(1, 5, missing) == O::Gap);
    CHECK_EQ(missing, 2u);
    CHECK(t.observe(1, 6, missing) == O::InOrder);
    CHECK(t.observe(1, 106, missing) == O::Gap);
    CHECK_EQ(missing, 99u);
    const auto& s = t.sources().at(1);
    CHECK_EQ(s.gap_events, 2u);
    CHECK_EQ(s.missing, 101u);
    CHECK_EQ(s.last_sequence, 106u);
}

TEST(sequence_independent_per_source) {
    SequenceTracker t;
    uint64_t missing = 0;
    CHECK(t.observe(1, 10, missing) == O::First);
    CHECK(t.observe(2, 0, missing) == O::First);
    CHECK(t.observe(1, 11, missing) == O::InOrder);
    CHECK(t.observe(2, 3, missing) == O::Gap);
    CHECK_EQ(missing, 2u);
    CHECK(t.observe(1, 12, missing) == O::InOrder);
    CHECK_EQ(t.sources().at(1).gap_events, 0u);
    CHECK_EQ(t.sources().at(2).gap_events, 1u);
}

TEST(sequence_out_of_order_and_restart) {
    SequenceTracker t;
    uint64_t missing = 0;
    t.observe(1, 100, missing);
    CHECK(t.observe(1, 99, missing) == O::OutOfOrder);   // late
    CHECK(t.observe(1, 100, missing) == O::OutOfOrder);  // duplicate
    CHECK(t.observe(1, 101, missing) == O::InOrder);
    CHECK_EQ(t.sources().at(1).out_of_order, 2u);
    // Source restarted at 0: resynchronise instead of reporting a huge gap.
    CHECK(t.observe(1, 0, missing) == O::OutOfOrder);
    CHECK(t.observe(1, 1, missing) == O::InOrder);
}
