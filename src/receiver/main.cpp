// ipc-relay-receiver: joins the bridge's UDP multicast group, reassembles
// messages and records them to a binary capture file.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ipcrelay/config.hpp"
#include "ipcrelay/log.hpp"
#include "ipcrelay/signal_handler.hpp"
#include "ipcrelay/zmq_util.hpp"
#include "receiver.hpp"
#include "receiver_config.hpp"

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "UDP multicast receiver and binary capture for ipc-relay-bridge streams.\n"
        "\n"
        "Options:\n"
        "  -c, --config FILE        Configuration file (see examples/receiver.conf)\n"
        "  -o, --set KEY=VALUE      Override an option, e.g. capture_file=/tmp/out.cap\n"
        "  -l, --log-level LEVEL    error | warn | info | debug\n"
        "      --check              Validate the configuration and exit\n"
        "  -V, --version            Print version and exit\n"
        "  -h, --help               Show this help\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace ipcrelay;
    std::string config_path;
    std::vector<std::string> overrides;
    std::string log_level_text;
    bool check_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", opt);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-c" || a == "--config") config_path = need("--config");
        else if (a == "-o" || a == "--set") overrides.push_back(need("--set"));
        else if (a == "-l" || a == "--log-level") log_level_text = need("--log-level");
        else if (a == "--check") check_only = true;
        else if (a == "-V" || a == "--version") {
            std::printf("ipc-relay-receiver %s (libzmq %s)\n", IPCRELAY_VERSION_STRING, zmq_version_string().c_str());
            return 0;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument '%s'\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    ConfigFile file;
    std::string err;
    if (!config_path.empty()) {
        if (!load_config_file(config_path, file, err)) {
            std::fprintf(stderr, "configuration error: %s\n", err.c_str());
            return 2;
        }
    } else {
        parse_config_text("", file, err);
    }
    for (const auto& o : overrides) {
        if (!apply_override(file, o, err)) {
            std::fprintf(stderr, "configuration error: %s\n", err.c_str());
            return 2;
        }
    }
    if (!log_level_text.empty()) apply_override(file, "log_level=" + log_level_text, err);

    ReceiverConfig cfg;
    std::vector<std::string> errors;
    if (!build_receiver_config(file, cfg, errors)) {
        std::fprintf(stderr, "configuration invalid (%zu problem%s):\n", errors.size(), errors.size() == 1 ? "" : "s");
        for (const auto& e : errors) std::fprintf(stderr, "  - %s\n", e.c_str());
        return 2;
    }
    set_log_level(cfg.log_level);
    if (check_only) {
        std::printf("configuration OK: %s\n", describe_receiver_config(cfg).c_str());
        return 0;
    }

    install_shutdown_handlers();
    Receiver receiver(cfg);
    if (!receiver.init(err)) {
        LOG_ERROR("startup failed: %s", err.c_str());
        return 1;
    }
    return receiver.run();
}
