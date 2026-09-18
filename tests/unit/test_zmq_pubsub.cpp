// In-process demonstration that a second SUB socket (the bridge) does not
// steal messages from an existing subscriber on the same PUB endpoint
// (BRG-131). The end-to-end script repeats this with the real binaries.
#include <chrono>
#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <zmq.h>

#include "ipcrelay/zmq_util.hpp"
#include "test_framework.hpp"

using namespace ipcrelay;

TEST(zmq_pubsub_multiple_subscribers_receive_everything) {
    const std::string endpoint = "ipc:///tmp/ipcrelay_unit_pubsub_" + std::to_string(getpid()) + ".sock";
    void* ctx = zmq_ctx_new();
    void* pub = zmq_socket(ctx, ZMQ_PUB);
    REQUIRE(zmq_bind(pub, endpoint.c_str()) == 0);
    void* sub_a = zmq_socket(ctx, ZMQ_SUB);
    void* sub_b = zmq_socket(ctx, ZMQ_SUB);
    zmq_setsockopt(sub_a, ZMQ_SUBSCRIBE, "", 0);
    zmq_setsockopt(sub_b, ZMQ_SUBSCRIBE, "", 0);
    REQUIRE(zmq_connect(sub_a, endpoint.c_str()) == 0);
    REQUIRE(zmq_connect(sub_b, endpoint.c_str()) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // slow joiner

    const int n = 500;
    for (int i = 0; i < n; ++i) {
        std::string m = "msg" + std::to_string(i);
        REQUIRE(zmq_send(pub, m.data(), m.size(), 0) >= 0);
    }
    auto drain = [&](void* s) -> int {
        int got = 0;
        std::vector<std::vector<uint8_t>> frames;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (got < n && std::chrono::steady_clock::now() < deadline) {
            zmq_pollitem_t item{s, 0, ZMQ_POLLIN, 0};
            if (zmq_poll(&item, 1, 100) <= 0) continue;
            while (zmq_recv_multipart_nowait(s, frames) > 0) {
                std::string expect = "msg" + std::to_string(got);
                CHECK(frames.size() == 1);
                if (frames.size() != 1) return -1;
                CHECK_EQ(std::string(frames[0].begin(), frames[0].end()), expect);
                ++got;
            }
        }
        return got;
    };
    CHECK_EQ(drain(sub_a), n);
    CHECK_EQ(drain(sub_b), n);

    int linger = 0;
    zmq_setsockopt(pub, ZMQ_LINGER, &linger, sizeof linger);
    zmq_close(sub_a);
    zmq_close(sub_b);
    zmq_close(pub);
    zmq_ctx_term(ctx);
    std::remove(endpoint.substr(6).c_str());
}
