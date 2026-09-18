// ipc-relay-bridge: subscribes to N ZeroMQ IPC publishers and republishes
// every message over one UDP multicast stream.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bridge.hpp"
#include "bridge_config.hpp"
#include "ipcrelay/config.hpp"
#include "ipcrelay/log.hpp"
#include "ipcrelay/signal_handler.hpp"
#include "ipcrelay/zmq_util.hpp"

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "ZeroMQ IPC PUB/SUB -> UDP multicast bridge.\n"
        "\n"
        "Options:\n"
        "  -c, --config FILE        Configuration file (see examples/bridge.conf)\n"
        "  -s, --source ID=ENDPOINT[,FILTER]\n"
        "                           Add a ZeroMQ source (repeatable). Without FILTER all\n"
        "                           messages are forwarded.\n"
        "  -o, --set KEY=VALUE      Override a global option, e.g. multicast_port=6000.\n"
        "                           source.KEY=VALUE applies to the last [source].\n"
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
    std::vector<std::string> cli_sources;
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
        else if (a == "-s" || a == "--source") cli_sources.push_back(need("--source"));
        else if (a == "-o" || a == "--set") overrides.push_back(need("--set"));
        else if (a == "-l" || a == "--log-level") log_level_text = need("--log-level");
        else if (a == "--check") check_only = true;
        else if (a == "-V" || a == "--version") {
            std::printf("ipc-relay-bridge %s (libzmq %s)\n", IPCRELAY_VERSION_STRING, zmq_version_string().c_str());
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
    for (const auto& s : cli_sources) {
        // ID=ENDPOINT[,FILTER]
        std::size_t eq = s.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "--source expects ID=ENDPOINT[,FILTER], got '%s'\n", s.c_str());
            return 2;
        }
        ConfigSection sec;
        sec.name = "source";
        sec.entries.push_back({"id", trim(s.substr(0, eq)), 0});
        std::string rest = s.substr(eq + 1);
        std::size_t comma = rest.find(',');
        if (comma == std::string::npos) {
            sec.entries.push_back({"endpoint", trim(rest), 0});
        } else {
            sec.entries.push_back({"endpoint", trim(rest.substr(0, comma)), 0});
            sec.entries.push_back({"filter", rest.substr(comma + 1), 0});
        }
        file.sections.push_back(std::move(sec));
    }
    for (const auto& o : overrides) {
        if (!apply_override(file, o, err)) {
            std::fprintf(stderr, "configuration error: %s\n", err.c_str());
            return 2;
        }
    }
    if (!log_level_text.empty()) {
        if (!apply_override(file, "log_level=" + log_level_text, err)) {
            std::fprintf(stderr, "configuration error: %s\n", err.c_str());
            return 2;
        }
    }

    BridgeConfig cfg;
    std::vector<std::string> errors;
    if (!build_bridge_config(file, cfg, errors)) {
        std::fprintf(stderr, "configuration invalid (%zu problem%s):\n", errors.size(), errors.size() == 1 ? "" : "s");
        for (const auto& e : errors) std::fprintf(stderr, "  - %s\n", e.c_str());
        return 2;
    }
    set_log_level(cfg.log_level);
    if (check_only) {
        std::printf("configuration OK: %s\n", describe_bridge_config(cfg).c_str());
        return 0;
    }

    install_shutdown_handlers();
    Bridge bridge(cfg);
    if (!bridge.init(err)) {
        LOG_ERROR("startup failed: %s", err.c_str());
        return 1;
    }
    return bridge.run();
}
