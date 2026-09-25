#include "ipcrelay/reassembler.hpp"

#include <cstring>

namespace ipcrelay {

Reassembler::Reassembler(const Config& cfg) : cfg_(cfg) {}

Reassembler::Result Reassembler::add(const wire::Header& hdr, const uint8_t* payload, uint64_t now_ns,
                                     CompletedMessage& out, bool& new_message) {
    new_message = false;
    out = CompletedMessage{};

    // Fast path: unfragmented message. No allocation, no copy.
    if (hdr.fragment_count == 1) {
        new_message = true;
        out.source_id = hdr.source_id;
        out.sequence = hdr.sequence;
        out.timestamp_ns = hdr.timestamp_ns;
        out.flags = hdr.flags;
        out.data = payload;
        out.length = hdr.fragment_length;
        return Result::Complete;
    }

    if (hdr.message_length > cfg_.max_message_length) {
        // Too large to buffer; treated as malformed for this receiver's limits.
        return Result::Malformed;
    }

    const Key key{hdr.source_id, hdr.sequence};
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        new_message = true;
        while (entries_.size() >= cfg_.max_pending) {
            evict_oldest(nullptr);
        }
        Entry e;
        e.first_seen_ns = now_ns;
        e.timestamp_ns = hdr.timestamp_ns;
        e.flags = hdr.flags;
        e.fragment_count = hdr.fragment_count;
        e.message_length = hdr.message_length;
        e.buffer.resize(hdr.message_length);
        e.received.assign(hdr.fragment_count, 0);
        it = entries_.emplace(key, std::move(e)).first;
        order_.push_back(key);
    }

    Entry& e = it->second;
    if (hdr.fragment_count != e.fragment_count || hdr.message_length != e.message_length) {
        // Inconsistent with the earlier fragments: drop the whole message.
        entries_.erase(it);
        return Result::Malformed;
    }
    if (e.received[hdr.fragment_index]) {
        return Result::Duplicate;
    }
    // parse_datagram already guaranteed offset + length <= message_length.
    if (hdr.fragment_length > 0) {
        std::memcpy(e.buffer.data() + hdr.fragment_offset, payload, hdr.fragment_length);
    }
    e.received[hdr.fragment_index] = 1;
    ++e.fragments_received;

    if (e.fragments_received < e.fragment_count) {
        return Result::Incomplete;
    }

    completed_.swap(e.buffer);
    out.source_id = hdr.source_id;
    out.sequence = hdr.sequence;
    out.timestamp_ns = e.timestamp_ns;
    out.flags = e.flags;
    out.data = completed_.data();
    out.length = completed_.size();
    entries_.erase(it);
    return Result::Complete;
}

void Reassembler::evict_oldest(const std::function<void(const ExpiredMessage&)>& on_expired) {
    while (!order_.empty()) {
        Key k = order_.front();
        order_.pop_front();
        auto it = entries_.find(k);
        if (it == entries_.end()) continue;  // already completed or removed
        ++evictions_;
        if (on_expired) {
            on_expired(ExpiredMessage{k.source_id, k.sequence, it->second.fragments_received,
                                      it->second.fragment_count});
        }
        entries_.erase(it);
        return;
    }
}

std::size_t Reassembler::expire(uint64_t now_ns, const std::function<void(const ExpiredMessage&)>& on_expired) {
    std::size_t expired = 0;
    // order_ is in insertion order, so the oldest entries are at the front.
    while (!order_.empty()) {
        Key k = order_.front();
        auto it = entries_.find(k);
        if (it == entries_.end()) {
            order_.pop_front();
            continue;
        }
        if (now_ns - it->second.first_seen_ns < cfg_.timeout_ns) break;
        order_.pop_front();
        if (on_expired) {
            on_expired(ExpiredMessage{k.source_id, k.sequence, it->second.fragments_received,
                                      it->second.fragment_count});
        }
        entries_.erase(it);
        ++expired;
    }
    // Keep order_ from growing with stale keys of completed messages.
    if (order_.size() > entries_.size() * 4 + 64) {
        std::deque<Key> compact;
        for (const Key& k : order_) {
            if (entries_.count(k)) compact.push_back(k);
        }
        order_.swap(compact);
    }
    return expired;
}

}  // namespace ipcrelay
