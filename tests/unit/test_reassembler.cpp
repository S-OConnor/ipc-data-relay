#include <cstring>
#include <vector>

#include "ipcrelay/reassembler.hpp"
#include "test_framework.hpp"

using namespace ipcrelay;
using namespace ipcrelay::wire;

namespace {

struct Frag {
    Header hdr;
    std::vector<uint8_t> payload;
};

// Fragments a message exactly like the bridge does.
std::vector<Frag> fragment(uint32_t source, uint64_t seq, const std::vector<uint8_t>& msg, std::size_t chunk) {
    std::vector<Frag> out;
    std::size_t count = fragment_count_for(msg.size(), chunk);
    std::size_t off = 0;
    for (std::size_t i = 0; i < count; ++i) {
        Frag f;
        std::size_t n = std::min(chunk, msg.size() - off);
        f.hdr.source_id = source;
        f.hdr.sequence = seq;
        f.hdr.timestamp_ns = 1000 + seq;
        f.hdr.message_length = static_cast<uint32_t>(msg.size());
        f.hdr.fragment_offset = static_cast<uint32_t>(off);
        f.hdr.fragment_index = static_cast<uint16_t>(i);
        f.hdr.fragment_count = static_cast<uint16_t>(count);
        f.hdr.fragment_length = static_cast<uint16_t>(n);
        f.hdr.flags = count > 1 ? kFlagFragmented : 0;
        f.payload.assign(msg.begin() + static_cast<long>(off), msg.begin() + static_cast<long>(off + n));
        out.push_back(f);
        off += n;
    }
    return out;
}

std::vector<uint8_t> pattern(std::size_t n) {
    std::vector<uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + 3);
    return v;
}

}  // namespace

TEST(reassembler_single_fragment_fast_path) {
    Reassembler r(Reassembler::Config{});
    auto msg = pattern(100);
    auto frags = fragment(1, 5, msg, 1356);
    REQUIRE(frags.size() == 1);
    CompletedMessage out;
    bool fresh = false;
    CHECK(r.add(frags[0].hdr, frags[0].payload.data(), 0, out, fresh) == Reassembler::Result::Complete);
    CHECK(fresh);
    CHECK_EQ(out.length, 100u);
    CHECK(out.data == frags[0].payload.data());  // no copy
    CHECK_EQ(r.pending(), 0u);
}

TEST(reassembler_in_order) {
    Reassembler r(Reassembler::Config{});
    auto msg = pattern(5000);
    auto frags = fragment(2, 9, msg, 1356);
    REQUIRE(frags.size() == 4);
    CompletedMessage out;
    bool fresh = false;
    for (std::size_t i = 0; i < frags.size(); ++i) {
        auto res = r.add(frags[i].hdr, frags[i].payload.data(), 0, out, fresh);
        CHECK_EQ(fresh, i == 0);
        if (i + 1 < frags.size()) CHECK(res == Reassembler::Result::Incomplete);
        else CHECK(res == Reassembler::Result::Complete);
    }
    REQUIRE(out.length == msg.size());
    CHECK(std::memcmp(out.data, msg.data(), msg.size()) == 0);
    CHECK_EQ(out.sequence, 9u);
    CHECK_EQ(out.timestamp_ns, 1009u);
    CHECK_EQ(r.pending(), 0u);
}

TEST(reassembler_out_of_order_and_duplicates) {
    Reassembler r(Reassembler::Config{});
    auto msg = pattern(4000);
    auto frags = fragment(3, 1, msg, 1356);
    REQUIRE(frags.size() == 3);
    CompletedMessage out;
    bool fresh = false;
    CHECK(r.add(frags[2].hdr, frags[2].payload.data(), 0, out, fresh) == Reassembler::Result::Incomplete);
    CHECK(fresh);
    CHECK(r.add(frags[0].hdr, frags[0].payload.data(), 0, out, fresh) == Reassembler::Result::Incomplete);
    CHECK(!fresh);
    CHECK(r.add(frags[0].hdr, frags[0].payload.data(), 0, out, fresh) == Reassembler::Result::Duplicate);
    CHECK(r.add(frags[1].hdr, frags[1].payload.data(), 0, out, fresh) == Reassembler::Result::Complete);
    REQUIRE(out.length == msg.size());
    CHECK(std::memcmp(out.data, msg.data(), msg.size()) == 0);
}

TEST(reassembler_interleaved_sources) {
    Reassembler r(Reassembler::Config{});
    auto a = pattern(3000);
    auto b = pattern(2900);
    auto fa = fragment(1, 0, a, 1356);
    auto fb = fragment(2, 0, b, 1356);
    CompletedMessage out;
    bool fresh = false;
    CHECK(r.add(fa[0].hdr, fa[0].payload.data(), 0, out, fresh) == Reassembler::Result::Incomplete);
    CHECK(r.add(fb[0].hdr, fb[0].payload.data(), 0, out, fresh) == Reassembler::Result::Incomplete);
    CHECK(r.add(fa[1].hdr, fa[1].payload.data(), 0, out, fresh) == Reassembler::Result::Incomplete);
    CHECK(r.add(fb[1].hdr, fb[1].payload.data(), 0, out, fresh) == Reassembler::Result::Incomplete);
    CHECK_EQ(r.pending(), 2u);
    CHECK(r.add(fb[2].hdr, fb[2].payload.data(), 0, out, fresh) == Reassembler::Result::Complete);
    CHECK_EQ(out.source_id, 2u);
    CHECK(std::memcmp(out.data, b.data(), b.size()) == 0);
    CHECK(r.add(fa[2].hdr, fa[2].payload.data(), 0, out, fresh) == Reassembler::Result::Complete);
    CHECK_EQ(out.source_id, 1u);
    CHECK(std::memcmp(out.data, a.data(), a.size()) == 0);
}

TEST(reassembler_missing_fragment_times_out) {
    Reassembler::Config cfg;
    cfg.timeout_ns = 1000;
    Reassembler r(cfg);
    auto msg = pattern(3000);
    auto frags = fragment(4, 77, msg, 1356);
    CompletedMessage out;
    bool fresh = false;
    CHECK(r.add(frags[0].hdr, frags[0].payload.data(), 100, out, fresh) == Reassembler::Result::Incomplete);
    CHECK(r.add(frags[2].hdr, frags[2].payload.data(), 200, out, fresh) == Reassembler::Result::Incomplete);
    std::vector<ExpiredMessage> expired;
    CHECK_EQ(r.expire(500, [&](const ExpiredMessage& e) { expired.push_back(e); }), 0u);
    CHECK_EQ(r.pending(), 1u);
    CHECK_EQ(r.expire(1100, [&](const ExpiredMessage& e) { expired.push_back(e); }), 1u);
    REQUIRE(expired.size() == 1);
    CHECK_EQ(expired[0].source_id, 4u);
    CHECK_EQ(expired[0].sequence, 77u);
    CHECK_EQ(expired[0].fragments_received, 2);
    CHECK_EQ(expired[0].fragment_count, 3);
    CHECK_EQ(r.pending(), 0u);
    // A late fragment after expiry starts a fresh (incomplete) entry.
    CHECK(r.add(frags[1].hdr, frags[1].payload.data(), 1200, out, fresh) == Reassembler::Result::Incomplete);
    CHECK(fresh);
}

TEST(reassembler_bounded_pending) {
    Reassembler::Config cfg;
    cfg.max_pending = 4;
    Reassembler r(cfg);
    auto msg = pattern(3000);
    CompletedMessage out;
    bool fresh = false;
    for (uint64_t seq = 0; seq < 10; ++seq) {
        auto frags = fragment(1, seq, msg, 1356);
        r.add(frags[0].hdr, frags[0].payload.data(), seq, out, fresh);
    }
    CHECK_EQ(r.pending(), 4u);
    CHECK_EQ(r.evictions(), 6u);
}

TEST(reassembler_inconsistent_fragment_is_malformed) {
    Reassembler r(Reassembler::Config{});
    auto msg = pattern(3000);
    auto frags = fragment(1, 3, msg, 1356);
    CompletedMessage out;
    bool fresh = false;
    CHECK(r.add(frags[0].hdr, frags[0].payload.data(), 0, out, fresh) == Reassembler::Result::Incomplete);
    Frag bad = frags[1];
    bad.hdr.fragment_count = 5;
    CHECK(r.add(bad.hdr, bad.payload.data(), 0, out, fresh) == Reassembler::Result::Malformed);
    CHECK_EQ(r.pending(), 0u);
}

TEST(reassembler_rejects_oversize_message) {
    Reassembler::Config cfg;
    cfg.max_message_length = 2000;
    Reassembler r(cfg);
    auto msg = pattern(3000);
    auto frags = fragment(1, 3, msg, 1356);
    CompletedMessage out;
    bool fresh = false;
    CHECK(r.add(frags[0].hdr, frags[0].payload.data(), 0, out, fresh) == Reassembler::Result::Malformed);
    CHECK_EQ(r.pending(), 0u);
}
