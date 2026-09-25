// Independent ZeroMQ subscriber used by the integration tests: connects to
// one endpoint, subscribes to everything, and counts messages until it has
// seen --expect messages or --timeout-ms passes. Prints the count on stdout.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <zmq.h>

#include "ipcrelay/common/zmq_util.hpp"

int main(int argc, char** argv) {
    std::string endpoint;
    long expect = 0;
    long timeout_ms = 10000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--endpoint" && i + 1 < argc) endpoint = argv[++i];
        else if (a == "--expect" && i + 1 < argc) expect = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--timeout-ms" && i + 1 < argc) timeout_ms = std::strtol(argv[++i], nullptr, 10);
        else {
            std::fprintf(stderr, "usage: sub_counter --endpoint EP [--expect N] [--timeout-ms MS]\n");
            return 2;
        }
    }
    if (endpoint.empty()) return 2;
    void* ctx = zmq_ctx_new();
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);
    if (zmq_connect(sub, endpoint.c_str()) != 0) {
        std::fprintf(stderr, "connect failed: %s\n", ipcrelay::zmq_error_string(errno).c_str());
        return 1;
    }
    long count = 0;
    uint64_t bytes = 0;
    std::vector<std::vector<uint8_t>> frames;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while ((expect == 0 || count < expect) && std::chrono::steady_clock::now() < deadline) {
        zmq_pollitem_t item{sub, 0, ZMQ_POLLIN, 0};
        int rc = zmq_poll(&item, 1, 100);
        if (rc <= 0) continue;
        while (ipcrelay::zmq_recv_multipart_nowait(sub, frames) > 0) {
            ++count;
            for (const auto& f : frames) bytes += f.size();
        }
    }
    std::printf("%ld %llu\n", count, static_cast<unsigned long long>(bytes));
    int linger = 0;
    zmq_setsockopt(sub, ZMQ_LINGER, &linger, sizeof linger);
    zmq_close(sub);
    zmq_ctx_term(ctx);
    return 0;
}
