// ipc-relay-ctl: sends runtime commands to the receiver's ZeroMQ TCP command
// channel and/or monitors its statistics channel (BRG-075A, BRG-077).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <zmq.h>

#include "ipcrelay/signal_handler.hpp"
#include "ipcrelay/zmq_util.hpp"

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s send [--endpoint tcp://127.0.0.1:5557] [--connect] [--topic T] [--settle-ms MS] COMMAND...\n"
        "  %s monitor [--endpoint tcp://127.0.0.1:5556] [--topic stats] [--count N]\n"
        "\n"
        "send:     publishes COMMAND (e.g. \"record on\", \"record off\", \"flush\", \"stats\")\n"
        "          on a PUB socket. By default the socket BINDS the endpoint, matching the\n"
        "          receiver's default of connecting to the command endpoint; use --connect\n"
        "          when the receiver is configured with command_bind = true.\n"
        "monitor:  connects a SUB socket to the receiver's statistics endpoint and prints\n"
        "          each JSON statistics message on its own line (Ctrl-C to stop).\n",
        prog, prog);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace ipcrelay;
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    std::string mode = argv[1];
    std::string endpoint;
    std::string topic;
    bool connect = false;
    long settle_ms = 300;
    long count = 0;
    std::vector<std::string> commands;
    bool topic_set = false;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", opt);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-e" || a == "--endpoint") endpoint = need("--endpoint");
        else if (a == "-t" || a == "--topic") { topic = need("--topic"); topic_set = true; }
        else if (a == "--connect") connect = true;
        else if (a == "--bind") connect = false;
        else if (a == "--settle-ms") settle_ms = std::strtol(need("--settle-ms"), nullptr, 10);
        else if (a == "-n" || a == "--count") count = std::strtol(need("--count"), nullptr, 10);
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else commands.push_back(a);
    }

    void* ctx = zmq_ctx_new();
    int rc = 0;
    if (mode == "send") {
        if (commands.empty()) {
            std::fprintf(stderr, "send: no command given\n");
            return 2;
        }
        if (endpoint.empty()) endpoint = "tcp://127.0.0.1:5557";
        void* pub = zmq_socket(ctx, ZMQ_PUB);
        int linger = 1000;
        zmq_setsockopt(pub, ZMQ_LINGER, &linger, sizeof linger);
        rc = connect ? zmq_connect(pub, endpoint.c_str()) : zmq_bind(pub, endpoint.c_str());
        if (rc != 0) {
            std::fprintf(stderr, "%s '%s' failed: %s\n", connect ? "connect" : "bind", endpoint.c_str(),
                         zmq_error_string(errno).c_str());
            return 1;
        }
        // PUB/SUB slow-joiner: give the receiver time to (re)connect and subscribe.
        std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
        std::string joined;
        for (const auto& c : commands) joined += (joined.empty() ? "" : " ") + c;
        std::vector<std::string> frames;
        if (topic_set) frames.push_back(topic);
        frames.push_back(joined);
        if (!zmq_send_frames(pub, frames, false)) {
            std::fprintf(stderr, "send failed: %s\n", zmq_error_string(errno).c_str());
            return 1;
        }
        std::fprintf(stderr, "sent '%s' to %s\n", joined.c_str(), endpoint.c_str());
        // Allow the message to leave before closing.
        std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
        zmq_close(pub);
    } else if (mode == "monitor") {
        if (endpoint.empty()) endpoint = "tcp://127.0.0.1:5556";
        if (!topic_set) topic = "stats";
        install_shutdown_handlers();
        void* sub = zmq_socket(ctx, ZMQ_SUB);
        int linger = 0;
        zmq_setsockopt(sub, ZMQ_LINGER, &linger, sizeof linger);
        zmq_setsockopt(sub, ZMQ_SUBSCRIBE, topic.data(), topic.size());
        if (zmq_connect(sub, endpoint.c_str()) != 0) {
            std::fprintf(stderr, "connect '%s' failed: %s\n", endpoint.c_str(), zmq_error_string(errno).c_str());
            return 1;
        }
        std::vector<std::vector<uint8_t>> frames;
        long received = 0;
        while (!shutdown_requested() && (count == 0 || received < count)) {
            zmq_pollitem_t item{sub, 0, ZMQ_POLLIN, 0};
            int p = zmq_poll(&item, 1, 500);
            if (p < 0 && errno != EINTR) break;
            if (p <= 0) continue;
            while (zmq_recv_multipart_nowait(sub, frames) > 0) {
                const auto& f = frames.size() >= 2 ? frames[1] : frames[0];
                std::fwrite(f.data(), 1, f.size(), stdout);
                std::fputc('\n', stdout);
                std::fflush(stdout);
                if (++received >= count && count > 0) break;
            }
        }
        zmq_close(sub);
    } else {
        usage(argv[0]);
        rc = 2;
    }
    zmq_ctx_term(ctx);
    return rc;
}
