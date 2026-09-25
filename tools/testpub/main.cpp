// ipc-relay-testpub: test ZeroMQ PUB publisher generating simulated
// telemetry on one or more IPC endpoints (BRG-130).
//
// Each endpoint carries one data type, by position on the command line:
//   1st  board health (rail voltages/currents, temperatures)  data_type 1
//   2nd  mode and status                                      data_type 2
//   3rd  PTP statistics                                       data_type 3
// and again from the 1st for further endpoints. The structures are in
// telemetry.hpp.
//
// Payload layout (after the optional topic prefix), all little-endian:
//   u32 magic            0x54534554 ("TEST")
//   u32 publisher_index  index of the endpoint on the command line (0-based)
//   u64 message_index    0,1,2,... per endpoint
//   u32 payload_length   total payload length including topic prefix
//   u16 data_type        see above
//   u16 data_length      encoded size of the data structure
//   u8  data[data_length]
//   u8  filler[]         byte i = (message_index + i) & 0xFF, i counted from magic
// tools/capture_inspect.py --verify-testpub checks the header and filler.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <zmq.h>

#include "ipcrelay/common/byteorder.hpp"
#include "ipcrelay/common/signal_handler.hpp"
#include "ipcrelay/common/time_util.hpp"
#include "ipcrelay/common/zmq_util.hpp"
#include "telemetry.hpp"

namespace {

using ipcrelay::testpub::kTestHeaderSize;
using ipcrelay::testpub::kTestMagic;

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --endpoint EP [--endpoint EP ...] [options]\n"
        "\n"
        "Options:\n"
        "  -e, --endpoint EP      ZeroMQ PUB bind endpoint, e.g. ipc:///tmp/src1.sock (repeatable)\n"
        "  -n, --count N          Messages per endpoint (0 = until SIGINT/SIGTERM)   [1000]\n"
        "  -r, --rate HZ          Messages per second per endpoint (0 = unthrottled) [1000]\n"
        "  -s, --size BYTES       Minimum payload size (excluding topic); raised to\n"
        "                         fit the header and the channel's data structure  [64]\n"
        "  -S, --size-max BYTES   Maximum payload size; random in [size, size-max]   [=size]\n"
        "  -t, --topic PREFIX     Topic prefix prepended to every message            [\"\"]\n"
        "  -m, --multipart        Send the topic as a separate first frame\n"
        "      --settle-ms MS     Delay after bind before publishing (slow joiner)   [500]\n"
        "      --linger-ms MS     Delay after the last message before exiting        [500]\n"
        "      --seed N           Random seed for sizes                              [1]\n"
        "  -q, --quiet            Do not print progress\n"
        "  -h, --help\n",
        prog);
}

long parse_long(const char* s, const char* what) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0' || v < 0) {
        std::fprintf(stderr, "invalid value for %s: '%s'\n", what, s);
        std::exit(2);
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace ipcrelay;
    std::vector<std::string> endpoints;
    long count = 1000;
    long rate = 1000;
    long size_min = 64;
    long size_max = -1;
    std::string topic;
    bool multipart = false;
    long settle_ms = 500;
    long linger_ms = 500;
    unsigned seed = 1;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", opt);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-e" || a == "--endpoint") endpoints.push_back(need("--endpoint"));
        else if (a == "-n" || a == "--count") count = parse_long(need("--count"), "--count");
        else if (a == "-r" || a == "--rate") rate = parse_long(need("--rate"), "--rate");
        else if (a == "-s" || a == "--size") size_min = parse_long(need("--size"), "--size");
        else if (a == "-S" || a == "--size-max") size_max = parse_long(need("--size-max"), "--size-max");
        else if (a == "-t" || a == "--topic") topic = need("--topic");
        else if (a == "-m" || a == "--multipart") multipart = true;
        else if (a == "--settle-ms") settle_ms = parse_long(need("--settle-ms"), "--settle-ms");
        else if (a == "--linger-ms") linger_ms = parse_long(need("--linger-ms"), "--linger-ms");
        else if (a == "--seed") seed = static_cast<unsigned>(parse_long(need("--seed"), "--seed"));
        else if (a == "-q" || a == "--quiet") quiet = true;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown argument '%s'\n", a.c_str()); usage(argv[0]); return 2; }
    }
    if (endpoints.empty()) {
        usage(argv[0]);
        return 2;
    }
    if (size_max < 0) size_max = size_min;
    if (size_max < size_min) {
        std::fprintf(stderr, "--size-max must be >= --size\n");
        return 2;
    }

    install_shutdown_handlers();
    void* ctx = zmq_ctx_new();
    std::vector<void*> socks;
    std::vector<testpub::Simulator> sims;
    for (const auto& ep : endpoints) {
        void* s = zmq_socket(ctx, ZMQ_PUB);
        int hwm = 100000;
        zmq_setsockopt(s, ZMQ_SNDHWM, &hwm, sizeof hwm);
        int linger = static_cast<int>(linger_ms);
        zmq_setsockopt(s, ZMQ_LINGER, &linger, sizeof linger);
        if (zmq_bind(s, ep.c_str()) != 0) {
            std::fprintf(stderr, "zmq_bind('%s') failed: %s\n", ep.c_str(), zmq_error_string(errno).c_str());
            return 1;
        }
        socks.push_back(s);
        sims.emplace_back(testpub::data_type_for_endpoint(sims.size()));
        if (!quiet) {
            std::fprintf(stderr, "testpub: PUB bound to %s (data type %u, %zu bytes)\n", ep.c_str(),
                         static_cast<unsigned>(sims.back().type()), sims.back().size());
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));

    std::mt19937 rng(seed);
    std::uniform_int_distribution<long> size_dist(size_min, size_max);
    std::vector<uint8_t> payload;
    std::vector<uint64_t> sent(endpoints.size(), 0);
    const uint64_t period_ns = rate > 0 ? kNsPerSec / static_cast<uint64_t>(rate) : 0;
    uint64_t next_ns = now_monotonic_ns();
    uint64_t total = 0;
    uint64_t last_report = now_monotonic_ns();

    bool done = false;
    while (!done && !shutdown_requested()) {
        done = count > 0;
        for (std::size_t p = 0; p < socks.size(); ++p) {
            if (count > 0 && sent[p] >= static_cast<uint64_t>(count)) continue;
            done = false;
            const std::size_t data_len = sims[p].size();
            const std::size_t body = std::max(static_cast<std::size_t>(size_dist(rng)), kTestHeaderSize + data_len);
            const std::size_t prefix = multipart ? 0 : topic.size();
            payload.resize(prefix + body);
            if (prefix) std::memcpy(payload.data(), topic.data(), prefix);
            uint8_t* h = payload.data() + prefix;
            put_u32le(h + 0, kTestMagic);
            put_u32le(h + 4, static_cast<uint32_t>(p));
            put_u64le(h + 8, sent[p]);
            put_u32le(h + 16, static_cast<uint32_t>(payload.size()));
            put_u16le(h + 20, sims[p].type());
            put_u16le(h + 22, static_cast<uint16_t>(data_len));
            sims[p].encode_next(sent[p], h + kTestHeaderSize);
            for (std::size_t i = kTestHeaderSize + data_len; i < body; ++i) {
                h[i] = static_cast<uint8_t>((sent[p] + i) & 0xFFu);
            }
            if (multipart) {
                zmq_send(socks[p], topic.data(), topic.size(), ZMQ_SNDMORE);
            }
            if (zmq_send(socks[p], payload.data(), payload.size(), 0) < 0) {
                std::fprintf(stderr, "zmq_send failed on %s: %s\n", endpoints[p].c_str(), zmq_error_string(errno).c_str());
                return 1;
            }
            ++sent[p];
            ++total;
        }
        if (period_ns) {
            next_ns += period_ns;
            const uint64_t now = now_monotonic_ns();
            if (next_ns > now) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(next_ns - now));
            } else if (now - next_ns > kNsPerSec) {
                next_ns = now;  // fell far behind; do not try to catch up
            }
        }
        if (!quiet) {
            const uint64_t now = now_monotonic_ns();
            if (now - last_report >= kNsPerSec) {
                std::fprintf(stderr, "testpub: %llu messages sent\n", static_cast<unsigned long long>(total));
                last_report = now;
            }
        }
    }
    if (!quiet) {
        std::fprintf(stderr, "testpub: done, %llu messages sent across %zu endpoint(s)\n",
                     static_cast<unsigned long long>(total), endpoints.size());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(linger_ms));
    for (void* s : socks) zmq_close(s);
    zmq_ctx_term(ctx);
    return 0;
}
