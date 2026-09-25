// ipc-relay-ctl backend: bridges the receiver's ZeroMQ statistics/command
// channels to browsers. Serves the embedded frontend over HTTP and pushes the
// latest receiver statistics to every WebSocket client at a fixed rate;
// browser requests (start/stop recording) are forwarded as receiver commands.
//
// Frontend <-> backend messages are JSON text frames on /ws (documented in
// docs/architecture.md, "Control tool web interface").
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "http.hpp"

namespace ipcrelay::ctl {

struct ControlServerConfig {
    std::string http_host = "127.0.0.1";  // "*" or "0.0.0.0" for all interfaces
    uint16_t http_port = 8083;            // 0 = ephemeral (logged at startup)
    int update_interval_ms = 1000;        // statistics push rate to browsers
    int stale_after_ms = 3000;            // receiver reported offline after this long without stats
    std::string stats_endpoint = "tcp://127.0.0.1:5556";
    std::string stats_topic = "stats";
    std::string command_endpoint = "tcp://127.0.0.1:5557";
    bool command_connect = false;  // false: bind (receiver connects), true: connect (receiver binds)
    std::string command_topic;     // empty: single-frame commands
    std::string web_root;          // serve frontend files from here instead of the embedded copies
    std::size_t max_clients = 32;
};

class ControlServer {
public:
    explicit ControlServer(ControlServerConfig cfg);
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    bool init(std::string& error);
    // Runs until SIGINT/SIGTERM. Returns the process exit code.
    int run();

    uint16_t http_port() const { return bound_port_; }

private:
    struct Client {
        int fd = -1;
        std::string peer;
        bool websocket = false;
        bool close_after_write = false;
        bool dead = false;
        std::string in;
        std::string out;
        std::string message;  // fragmented WebSocket message being assembled
        bool in_message = false;
    };

    bool poll_once(long timeout_ms);
    void accept_clients();
    void read_client(Client& c);
    void write_client(Client& c);
    void close_client(Client& c);
    void handle_http(Client& c);
    void serve_http(Client& c, const HttpRequest& req);
    void upgrade_websocket(Client& c, const HttpRequest& req);
    void handle_ws_frames(Client& c);
    void handle_ws_message(Client& c, const std::string& text);
    void ws_send(Client& c, const std::string& text);
    void ws_fail(Client& c, uint16_t code, const std::string& reason);
    void drain_stats();
    void broadcast_stats();
    bool send_command(const std::string& command, std::string& error);
    bool load_asset(const std::string& path, std::string& body, std::string& content_type) const;
    std::string hello_json() const;
    std::string stats_envelope();

    ControlServerConfig cfg_;
    void* ctx_ = nullptr;
    void* stats_sub_ = nullptr;
    void* command_pub_ = nullptr;
    int listen_fd_ = -1;
    uint16_t bound_port_ = 0;
    std::vector<Client> clients_;
    std::vector<std::vector<uint8_t>> frames_;
    std::string latest_stats_;  // last valid receiver statistics JSON object
    uint64_t latest_stats_ns_ = 0;
    uint64_t stats_received_ = 0;
    uint64_t stats_invalid_ = 0;
    uint64_t updates_sent_ = 0;
    uint64_t next_update_ns_ = 0;
    bool push_on_next_stats_ = false;  // a command was sent: show its effect without waiting a full tick
};

}  // namespace ipcrelay::ctl
