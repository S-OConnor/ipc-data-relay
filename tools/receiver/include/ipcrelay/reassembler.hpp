// Application-level fragment reassembly for the receiver (BRG-064..BRG-066).
//
// Fragments are keyed by (source_id, sequence). Memory is bounded by a maximum
// number of in-flight messages and by a maximum message length; entries older
// than the timeout are discarded and reported as incomplete (BRG-114).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

#include "ipcrelay/common/wire_protocol.hpp"

namespace ipcrelay {

struct CompletedMessage {
    uint32_t source_id = 0;
    uint64_t sequence = 0;
    uint64_t timestamp_ns = 0;
    uint16_t flags = 0;
    const uint8_t* data = nullptr;   // valid until the next Reassembler call
    std::size_t length = 0;
};

struct ExpiredMessage {
    uint32_t source_id = 0;
    uint64_t sequence = 0;
    uint16_t fragments_received = 0;
    uint16_t fragment_count = 0;
};

class Reassembler {
public:
    struct Config {
        std::size_t max_pending = 256;            // in-flight fragmented messages
        uint64_t timeout_ns = 500ull * 1000000ull;  // discard after this long
        std::size_t max_message_length = 16u * 1024u * 1024u;
    };

    enum class Result {
        Complete,    // out is valid
        Incomplete,  // stored, waiting for more fragments
        Duplicate,   // fragment already received; ignored
        Malformed,   // inconsistent with earlier fragments or exceeds limits
    };

    explicit Reassembler(const Config& cfg);

    // Feeds one validated fragment. new_message is set to true when this is the
    // first fragment seen for its (source, sequence) key, which is the point at
    // which sequence tracking should observe the message.
    Result add(const wire::Header& hdr, const uint8_t* payload, uint64_t now_ns,
               CompletedMessage& out, bool& new_message);

    // Discards entries older than the timeout, invoking on_expired for each.
    std::size_t expire(uint64_t now_ns, const std::function<void(const ExpiredMessage&)>& on_expired);

    std::size_t pending() const { return entries_.size(); }
    uint64_t evictions() const { return evictions_; }

private:
    struct Key {
        uint32_t source_id;
        uint64_t sequence;
        bool operator==(const Key& o) const { return source_id == o.source_id && sequence == o.sequence; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return std::hash<uint64_t>()(k.sequence * 1000003ull ^ (static_cast<uint64_t>(k.source_id) << 32));
        }
    };
    struct Entry {
        uint64_t first_seen_ns = 0;
        uint64_t timestamp_ns = 0;
        uint16_t flags = 0;
        uint16_t fragment_count = 0;
        uint16_t fragments_received = 0;
        uint32_t message_length = 0;
        std::vector<uint8_t> buffer;
        std::vector<uint8_t> received;  // one byte per fragment (0/1)
    };

    void evict_oldest(const std::function<void(const ExpiredMessage&)>& on_expired);

    Config cfg_;
    std::unordered_map<Key, Entry, KeyHash> entries_;
    std::deque<Key> order_;  // insertion order for eviction
    std::vector<uint8_t> completed_;  // holds the last completed reassembled payload
    uint64_t evictions_ = 0;
};

}  // namespace ipcrelay
