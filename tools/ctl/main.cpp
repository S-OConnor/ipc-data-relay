// ipc-relay-ctl: controls a running receiver over its ZeroMQ TCP command
// channel and shows its statistics channel (BRG-075A, BRG-077).
//
//   serve    (default) long-running backend + browser frontend: pushes the
//            receiver's counts to the web page at a fixed rate and forwards
//            its start/stop recording requests to the receiver.
//   send     one-shot command for scripts.
//   monitor  prints statistics messages to stdout.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <zmq.h>

#include "control_server.hpp"
#include "ipcrelay/common/config.hpp"
#include "ipcrelay/common/log.hpp"
#include "ipcrelay/common/signal_handler.hpp"
#include "ipcrelay/common/zmq_util.hpp"

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s [serve] [--http HOST:PORT] [--update-ms MS] [--stale-ms MS]\n"
        "        [--stats-endpoint EP] [--stats-topic T] [--command-endpoint EP] [--connect]\n"
        "        [--command-topic T] [--web-root DIR] [--log-level LEVEL]\n"
        "  %s send [--endpoint tcp://127.0.0.1:5557] [--connect] [--topic T] [--settle-ms MS] COMMAND...\n"
        "  %s monitor [--endpoint tcp://127.0.0.1:5556] [--topic stats] [--count N]\n"
        "  %s --version\n"
        "\n"
        "serve:    (default) runs until SIGINT/SIGTERM. Serves the control web page on\n"
        "          --http (default 127.0.0.1:8083; use 0.0.0.0:PORT to allow remote browsers)\n"
        "          and pushes the latest receiver statistics to it every --update-ms\n"
        "          (default 1000, 50..60000) over a WebSocket (/ws). Start/stop recording\n"
        "          from the page is sent to the receiver as 'record on'/'record off'.\n"
        "          The receiver counts as offline when no statistics arrived for\n"
        "          --stale-ms (default 3000). Statistics: SUB connects --stats-endpoint\n"
        "          (default tcp://127.0.0.1:5556, topic 'stats'). Commands: PUB binds\n"
        "          --command-endpoint (default tcp://127.0.0.1:5557), or connects with\n"
        "          --connect when the receiver has command_bind = true.\n"
        "          GET /api/stats returns the same JSON snapshot the page receives.\n"
        "send:     publishes COMMAND (e.g. \"record on\", \"record off\", \"flush\", \"stats\")\n"
        "          on a PUB socket. By default the socket BINDS the endpoint, matching the\n"
        "          receiver's default of connecting to the command endpoint; use --connect\n"
        "          when the receiver is configured with command_bind = true.\n"
        "monitor:  connects a SUB socket to the receiver's statistics endpoint and prints\n"
        "          each JSON statistics message on its own line (Ctrl-C to stop).\n",
        prog, prog, prog, prog);
}

[[noreturn]] void bad_usage(const std::string& message) {
    std::fprintf(stderr, "ipc-relay-ctl: %s (see --help)\n", message.c_str());
    std::exit(2);
}

long parse_ms(const std::string& opt, const std::string& text, long min, long max) {
    int64_t v = 0;
    std::string err;
    if (!ipcrelay::parse_i64(text, v, err) || v < min || v > max)
        bad_usage(opt + " must be an integer in " + std::to_string(min) + ".." + std::to_string(max));
    return static_cast<long>(v);
}

// "HOST:PORT", ":PORT" or "PORT".
void parse_http(const std::string& text, ipcrelay::ctl::ControlServerConfig& cfg) {
    const std::size_t colon = text.rfind(':');
    const std::string port = colon == std::string::npos ? text : text.substr(colon + 1);
    if (colon != std::string::npos && colon > 0) cfg.http_host = text.substr(0, colon);
    uint16_t p = 0;
    std::string err;
    if (!ipcrelay::parse_u16(port, p, err)) bad_usage("--http expects HOST:PORT, got '" + text + "'");
    cfg.http_port = p;
}

int run_serve(int argc, char** argv, int first) {
    using namespace ipcrelay;
    ctl::ControlServerConfig cfg;
    for (int i = first; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) bad_usage(a + " requires an argument");
            return argv[++i];
        };
        if (a == "--http") parse_http(need(), cfg);
        else if (a == "--update-ms" || a == "-u") cfg.update_interval_ms = static_cast<int>(parse_ms(a, need(), 50, 60000));
        else if (a == "--stale-ms") cfg.stale_after_ms = static_cast<int>(parse_ms(a, need(), 100, 3600000));
        else if (a == "--stats-endpoint") cfg.stats_endpoint = need();
        else if (a == "--stats-topic") cfg.stats_topic = need();
        else if (a == "--command-endpoint") cfg.command_endpoint = need();
        else if (a == "--command-topic") cfg.command_topic = need();
        else if (a == "--connect") cfg.command_connect = true;
        else if (a == "--bind") cfg.command_connect = false;
        else if (a == "--web-root") cfg.web_root = need();
        else if (a == "--log-level") {
            LogLevel level;
            if (!parse_log_level(need(), level)) bad_usage("unknown log level");
            set_log_level(level);
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            bad_usage("unexpected argument '" + a + "'");
        }
    }

    install_shutdown_handlers();
    ctl::ControlServer server(cfg);
    std::string error;
    if (!server.init(error)) {
        LOG_ERROR("%s", error.c_str());
        return 1;
    }
    return server.run();
}

}  // namespace

int main(int argc, char** argv) {
    using namespace ipcrelay;
    const std::string mode = argc >= 2 ? argv[1] : "serve";
    if (mode == "--version") {
        std::printf("ipc-relay-ctl %s\n", IPCRELAY_VERSION_STRING);
        return 0;
    }
    if (mode == "serve") return run_serve(argc, argv, 2);
    if (mode != "send" && mode != "monitor") return run_serve(argc, argv, 1);

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
    } else {
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
    }
    zmq_ctx_term(ctx);
    return rc;
}
