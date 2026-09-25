#include "control_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include <zmq.h>

#include "ipcrelay/common/config.hpp"
#include "ipcrelay/common/log.hpp"
#include "ipcrelay/common/net_util.hpp"
#include "ipcrelay/common/signal_handler.hpp"
#include "ipcrelay/common/time_util.hpp"
#include "ipcrelay/common/zmq_util.hpp"
#include "ipcrelay/json_writer.hpp"
#include "json_value.hpp"
#include "web_assets.hpp"
#include "websocket.hpp"

namespace ipcrelay::ctl {

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxWsMessage = 64 * 1024;
// A browser that cannot keep up is skipped (it gets the next snapshot once
// its backlog drains) rather than buffered without bound.
constexpr std::size_t kMaxOutputBacklog = 1024 * 1024;
constexpr int kProtocolVersion = 1;

// Same-origin check for WebSocket upgrades so that an arbitrary web page open
// in the operator's browser cannot drive the receiver. Non-browser clients do
// not send Origin and are allowed.
bool origin_allowed(const HttpRequest& req) {
    if (!req.has_header("origin")) return true;
    const std::string& origin = req.header("origin");
    const std::size_t p = origin.find("://");
    if (p == std::string::npos) return false;
    return to_lower(origin.substr(p + 3)) == to_lower(req.header("host"));
}

void write_json_number(JsonWriter& j, double v) {
    if (std::isfinite(v) && std::floor(v) == v && std::fabs(v) < 9.0e15) j.value(static_cast<int64_t>(v));
    else j.value(v);
}

}  // namespace

ControlServer::ControlServer(ControlServerConfig cfg) : cfg_(std::move(cfg)) {}

ControlServer::~ControlServer() {
    for (Client& c : clients_) close_client(c);
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (stats_sub_) zmq_close(stats_sub_);
    if (command_pub_) zmq_close(command_pub_);
    if (ctx_) zmq_ctx_term(ctx_);
}

bool ControlServer::init(std::string& error) {
    ctx_ = zmq_ctx_new();
    if (!ctx_) {
        error = "zmq_ctx_new: " + zmq_error_string(errno);
        return false;
    }
    int linger = 0;

    stats_sub_ = zmq_socket(ctx_, ZMQ_SUB);
    if (!stats_sub_) {
        error = "zmq_socket(SUB): " + zmq_error_string(errno);
        return false;
    }
    zmq_setsockopt(stats_sub_, ZMQ_LINGER, &linger, sizeof linger);
    zmq_setsockopt(stats_sub_, ZMQ_SUBSCRIBE, cfg_.stats_topic.data(), cfg_.stats_topic.size());
    if (zmq_connect(stats_sub_, cfg_.stats_endpoint.c_str()) != 0) {
        error = "zmq_connect('" + cfg_.stats_endpoint + "') for statistics: " + zmq_error_string(errno);
        return false;
    }

    command_pub_ = zmq_socket(ctx_, ZMQ_PUB);
    if (!command_pub_) {
        error = "zmq_socket(PUB): " + zmq_error_string(errno);
        return false;
    }
    int command_linger = 500;
    zmq_setsockopt(command_pub_, ZMQ_LINGER, &command_linger, sizeof command_linger);
    int rc = cfg_.command_connect ? zmq_connect(command_pub_, cfg_.command_endpoint.c_str())
                                  : zmq_bind(command_pub_, cfg_.command_endpoint.c_str());
    if (rc != 0) {
        error = std::string(cfg_.command_connect ? "zmq_connect('" : "zmq_bind('") + cfg_.command_endpoint +
                "') for commands: " + zmq_error_string(errno);
        return false;
    }

    uint32_t host = INADDR_ANY;
    if (cfg_.http_host == "localhost") {
        host = INADDR_LOOPBACK;
    } else if (cfg_.http_host != "*" && !cfg_.http_host.empty()) {
        std::string perr;
        if (!parse_ipv4(cfg_.http_host, host, perr)) {
            error = "HTTP listen address: " + perr;
            return false;
        }
    }
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.http_port);
    addr.sin_addr.s_addr = htonl(host);
    if (::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        error = "bind HTTP " + cfg_.http_host + ":" + std::to_string(cfg_.http_port) + ": " + std::strerror(errno);
        return false;
    }
    if (::listen(listen_fd_, 16) != 0) {
        error = std::string("listen: ") + std::strerror(errno);
        return false;
    }
    socklen_t alen = sizeof addr;
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
    bound_port_ = ntohs(addr.sin_port);

    LOG_INFO("statistics SUB connected to %s (topic '%s')", cfg_.stats_endpoint.c_str(), cfg_.stats_topic.c_str());
    LOG_INFO("command PUB %s %s", cfg_.command_connect ? "connected to" : "bound to", cfg_.command_endpoint.c_str());
    if (!cfg_.web_root.empty()) LOG_INFO("serving frontend files from %s", cfg_.web_root.c_str());
    next_update_ns_ = now_monotonic_ns() + static_cast<uint64_t>(cfg_.update_interval_ms) * kNsPerMs;
    return true;
}

int ControlServer::run() {
    LOG_INFO("control UI listening on http://%s:%u/ (updates every %d ms)",
             cfg_.http_host == "*" ? "0.0.0.0" : cfg_.http_host.c_str(), static_cast<unsigned>(bound_port_),
             cfg_.update_interval_ms);
    while (!shutdown_requested()) {
        const uint64_t now = now_monotonic_ns();
        const long timeout_ms = next_update_ns_ > now ? static_cast<long>((next_update_ns_ - now) / kNsPerMs + 1) : 0;
        if (!poll_once(timeout_ms)) return 1;
    }
    LOG_INFO("shutdown requested (signal %d)", shutdown_signal());
    for (Client& c : clients_) {
        if (c.websocket && !c.dead) {
            c.out += ws_encode_frame(ws_opcode::kClose, ws_close_payload(ws_close::kGoingAway, "server shutting down"));
            write_client(c);
        }
    }
    return 0;
}

bool ControlServer::poll_once(long timeout_ms) {
    std::vector<zmq_pollitem_t> items;
    items.reserve(clients_.size() + 2);
    items.push_back({stats_sub_, 0, ZMQ_POLLIN, 0});
    items.push_back({nullptr, listen_fd_, ZMQ_POLLIN, 0});
    for (const Client& c : clients_) {
        short events = ZMQ_POLLIN;
        if (!c.out.empty()) events = static_cast<short>(events | ZMQ_POLLOUT);
        items.push_back({nullptr, c.fd, events, 0});
    }
    const int rc = zmq_poll(items.data(), static_cast<int>(items.size()), timeout_ms);
    if (rc < 0) {
        if (errno == EINTR) return true;
        LOG_ERROR("zmq_poll failed: %s", zmq_error_string(errno).c_str());
        return false;
    }
    if (rc > 0) {
        if (items[0].revents & ZMQ_POLLIN) drain_stats();
        // Clients accepted below are not in items; they are polled next round.
        const std::size_t polled = items.size() - 2;
        for (std::size_t i = 0; i < polled; ++i) {
            const short revents = items[i + 2].revents;
            Client& c = clients_[i];
            if (revents & ZMQ_POLLIN) read_client(c);
            if (!c.dead && (revents & ZMQ_POLLOUT)) write_client(c);
            if (revents & ZMQ_POLLERR) c.dead = true;
        }
        if (items[1].revents & ZMQ_POLLIN) accept_clients();
    }

    const uint64_t now = now_monotonic_ns();
    if (now >= next_update_ns_) {
        broadcast_stats();
        next_update_ns_ += static_cast<uint64_t>(cfg_.update_interval_ms) * kNsPerMs;
        if (next_update_ns_ <= now) next_update_ns_ = now + static_cast<uint64_t>(cfg_.update_interval_ms) * kNsPerMs;
    }

    for (Client& c : clients_) {
        if (!c.dead && !c.out.empty()) write_client(c);
        if (c.dead) close_client(c);
    }
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [](const Client& c) { return c.fd < 0; }),
                   clients_.end());
    return true;
}

void ControlServer::accept_clients() {
    for (;;) {
        sockaddr_in addr{};
        socklen_t alen = sizeof addr;
        const int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                LOG_WARN("accept failed: %s", std::strerror(errno));
            return;
        }
        Client c;
        c.fd = fd;
        c.peer = ipv4_to_string(ntohl(addr.sin_addr.s_addr)) + ":" + std::to_string(ntohs(addr.sin_port));
        if (clients_.size() >= cfg_.max_clients) {
            LOG_WARN("rejecting %s: %zu clients already connected", c.peer.c_str(), clients_.size());
            c.out = http_response(503, "text/plain; charset=utf-8", "too many clients\n", false);
            c.close_after_write = true;
        }
        clients_.push_back(std::move(c));
    }
}

void ControlServer::read_client(Client& c) {
    char buf[16384];
    for (;;) {
        const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
        if (n > 0) {
            // Anything a client sends after we decided to close it is ignored.
            if (!c.close_after_write) c.in.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            c.dead = true;
            return;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) c.dead = true;
        break;
    }
    if (c.close_after_write) return;
    if (c.websocket) handle_ws_frames(c);
    else handle_http(c);
}

void ControlServer::write_client(Client& c) {
    while (!c.out.empty()) {
        const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.out.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        c.dead = true;
        return;
    }
    if (c.close_after_write) c.dead = true;
}

void ControlServer::close_client(Client& c) {
    if (c.fd < 0) return;
    if (c.websocket) LOG_INFO("browser %s disconnected", c.peer.c_str());
    ::close(c.fd);
    c.fd = -1;
}

void ControlServer::handle_http(Client& c) {
    HttpRequest req;
    std::size_t consumed = 0;
    std::string error;
    const HttpParse r = parse_http_request(c.in, kMaxHeaderBytes, req, consumed, error);
    if (r == HttpParse::NeedMore) return;
    if (r == HttpParse::Error) {
        LOG_DEBUG("bad HTTP request from %s: %s", c.peer.c_str(), error.c_str());
        const int status = error.find("too large") != std::string::npos ? 431 : 400;
        c.out += http_response(status, "text/plain; charset=utf-8", error + "\n", false);
        c.close_after_write = true;
        return;
    }
    c.in.erase(0, consumed);
    if (req.path == "/ws") upgrade_websocket(c, req);
    else serve_http(c, req);
}

bool ControlServer::load_asset(const std::string& path, std::string& body, std::string& content_type) const {
    for (std::size_t i = 0; i < kWebAssetCount; ++i) {
        const WebAsset& a = kWebAssets[i];
        if (path != a.path) continue;
        content_type = a.content_type;
        // Only file names known at build time are served from web_root, so
        // the request path can never reach outside that directory.
        if (!cfg_.web_root.empty()) {
            std::ifstream f(cfg_.web_root + a.path, std::ios::binary);
            if (f) {
                std::ostringstream ss;
                ss << f.rdbuf();
                body = ss.str();
                return true;
            }
            LOG_WARN("%s%s not readable; serving the embedded copy", cfg_.web_root.c_str(), a.path);
        }
        body.assign(reinterpret_cast<const char*>(a.data), a.size);
        return true;
    }
    return false;
}

void ControlServer::serve_http(Client& c, const HttpRequest& req) {
    c.close_after_write = true;
    const bool head = req.method == "HEAD";
    if (req.method != "GET" && !head) {
        c.out += http_response(405, "text/plain; charset=utf-8", "method not allowed\n", false, {{"Allow", "GET, HEAD"}});
        return;
    }
    if (req.path == "/api/stats") {
        c.out += http_response(200, "application/json", stats_envelope(), head);
        return;
    }
    std::string body, type;
    if (!load_asset(req.path == "/" ? "/index.html" : req.path, body, type)) {
        c.out += http_response(404, "text/plain; charset=utf-8", "not found\n", head);
        return;
    }
    c.out += http_response(200, type, body, head,
                           {{"Content-Security-Policy",
                             "default-src 'self'; connect-src 'self' ws: wss:; frame-ancestors 'none'"}});
}

void ControlServer::upgrade_websocket(Client& c, const HttpRequest& req) {
    auto reject = [&c](int status, const std::string& why,
                       const std::vector<std::pair<std::string, std::string>>& extra = {}) {
        LOG_WARN("WebSocket upgrade from %s rejected: %s", c.peer.c_str(), why.c_str());
        c.out += http_response(status, "text/plain; charset=utf-8", why + "\n", false, extra);
        c.close_after_write = true;
    };
    if (req.method != "GET") return reject(405, "WebSocket upgrade requires GET");
    if (!header_has_token(req.header("upgrade"), "websocket") || !header_has_token(req.header("connection"), "upgrade"))
        return reject(426, "WebSocket upgrade required", {{"Upgrade", "websocket"}});
    if (req.header("sec-websocket-version") != "13")
        return reject(426, "unsupported WebSocket version", {{"Sec-WebSocket-Version", "13"}});
    const std::string& key = req.header("sec-websocket-key");
    if (key.size() != 24) return reject(400, "missing or invalid Sec-WebSocket-Key");
    if (!origin_allowed(req)) return reject(403, "cross-origin WebSocket not allowed (Origin " + req.header("origin") + ")");

    c.out += "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: " + websocket_accept_key(key) + "\r\n\r\n";
    c.websocket = true;
    LOG_INFO("browser %s connected (%zu client(s))", c.peer.c_str(), clients_.size());
    ws_send(c, hello_json());
    ws_send(c, stats_envelope());
    if (!c.in.empty()) handle_ws_frames(c);  // frames pipelined behind the handshake
}

void ControlServer::handle_ws_frames(Client& c) {
    while (!c.dead && !c.close_after_write) {
        WsFrame f;
        std::size_t consumed = 0;
        std::string error;
        const WsParse r = ws_parse_frame(c.in, kMaxWsMessage, f, consumed, error);
        if (r == WsParse::NeedMore) return;
        if (r == WsParse::Error) {
            ws_fail(c, error.find("exceeds") != std::string::npos ? ws_close::kTooBig : ws_close::kProtocolError, error);
            return;
        }
        c.in.erase(0, consumed);
        switch (f.opcode) {
            case ws_opcode::kPing:
                c.out += ws_encode_frame(ws_opcode::kPong, f.payload);
                break;
            case ws_opcode::kPong:
                break;
            case ws_opcode::kClose:
                // Echo the status code and close once it has been sent.
                c.out += ws_encode_frame(ws_opcode::kClose, f.payload.substr(0, 2));
                c.close_after_write = true;
                return;
            case ws_opcode::kBinary:
                ws_fail(c, ws_close::kUnsupportedData, "binary messages are not supported");
                return;
            case ws_opcode::kText:
            case ws_opcode::kContinuation:
                if ((f.opcode == ws_opcode::kText) == c.in_message) {
                    ws_fail(c, ws_close::kProtocolError, "unexpected continuation state");
                    return;
                }
                if (f.opcode == ws_opcode::kText) c.message.clear();
                c.message += f.payload;
                c.in_message = !f.fin;
                if (c.message.size() > kMaxWsMessage) {
                    ws_fail(c, ws_close::kTooBig, "message too large");
                    return;
                }
                if (f.fin) {
                    handle_ws_message(c, c.message);
                    c.message.clear();
                }
                break;
            default:
                break;
        }
    }
}

void ControlServer::handle_ws_message(Client& c, const std::string& text) {
    json::Value msg;
    std::string error;
    std::string command;
    const json::Value* id = nullptr;
    if (!json::parse(text, msg, error)) {
        error = "invalid JSON: " + error;
    } else if (!msg.is_object()) {
        error = "message must be a JSON object";
    } else {
        id = msg.get("id");
        const json::Value* type = msg.get("type");
        const json::Value* enabled = msg.get("enabled");
        if (!type || !type->is_string()) {
            error = "missing \"type\"";
        } else if (type->string == "record") {
            if (enabled && enabled->is_bool()) command = enabled->boolean ? "record on" : "record off";
            else error = "\"record\" needs a boolean \"enabled\"";
        } else {
            error = "unknown message type '" + type->string + "'";
        }
    }

    bool ok = false;
    if (!command.empty()) {
        ok = send_command(command, error);
        LOG_INFO("browser %s: '%s' %s%s", c.peer.c_str(), command.c_str(), ok ? "sent" : "failed: ", error.c_str());
        if (ok) push_on_next_stats_ = true;
    } else {
        LOG_WARN("browser %s: %s", c.peer.c_str(), error.c_str());
    }

    JsonWriter j;
    j.begin_object();
    j.key("type").value("command_result");
    j.key("id");
    if (id && id->is_number()) write_json_number(j, id->number);
    else if (id && id->is_string()) j.value(id->string);
    else j.null();
    j.key("command").value(command);
    j.key("ok").value(ok);
    j.key("error").value(error);
    j.end_object();
    ws_send(c, j.str());
}

void ControlServer::ws_send(Client& c, const std::string& text) {
    c.out += ws_encode_frame(ws_opcode::kText, text);
}

void ControlServer::ws_fail(Client& c, uint16_t code, const std::string& reason) {
    LOG_WARN("closing WebSocket %s: %s", c.peer.c_str(), reason.c_str());
    c.out += ws_encode_frame(ws_opcode::kClose, ws_close_payload(code, reason));
    c.close_after_write = true;
}

bool ControlServer::send_command(const std::string& command, std::string& error) {
    std::vector<std::string> frames;
    if (!cfg_.command_topic.empty()) frames.push_back(cfg_.command_topic);
    frames.push_back(command);
    if (!zmq_send_frames(command_pub_, frames, true)) {
        error = zmq_error_string(errno);
        return false;
    }
    return true;
}

void ControlServer::drain_stats() {
    bool got = false;
    for (int i = 0; i < 64; ++i) {
        const int rc = zmq_recv_multipart_nowait(stats_sub_, frames_);
        if (rc <= 0) {
            if (rc < 0) LOG_ERROR("statistics receive failed: %s", zmq_error_string(errno).c_str());
            break;
        }
        // Accept either [json] or [topic][json], like the receiver's command channel.
        const std::vector<uint8_t>& f = frames_.size() >= 2 ? frames_[1] : frames_[0];
        std::string text(f.begin(), f.end());
        json::Value v;
        std::string error;
        if (!json::parse(text, v, error) || !v.is_object()) {
            if (stats_invalid_++ < 5) LOG_WARN("ignoring invalid statistics message: %s", error.c_str());
            continue;
        }
        latest_stats_ = std::move(text);
        latest_stats_ns_ = now_monotonic_ns();
        ++stats_received_;
        got = true;
    }
    if (got && push_on_next_stats_) {
        push_on_next_stats_ = false;
        broadcast_stats();
    }
}

void ControlServer::broadcast_stats() {
    std::string frame;
    for (Client& c : clients_) {
        if (!c.websocket || c.dead || c.close_after_write) continue;
        if (c.out.size() > kMaxOutputBacklog) {
            LOG_DEBUG("browser %s is behind (%zu bytes queued); skipping update", c.peer.c_str(), c.out.size());
            continue;
        }
        if (frame.empty()) frame = ws_encode_frame(ws_opcode::kText, stats_envelope());
        c.out += frame;
    }
}

std::string ControlServer::hello_json() const {
    JsonWriter j;
    j.begin_object();
    j.key("type").value("hello");
    j.key("protocol_version").value(kProtocolVersion);
    j.key("tool_version").value(IPCRELAY_VERSION_STRING);
    j.key("update_interval_ms").value(cfg_.update_interval_ms);
    j.key("stale_after_ms").value(cfg_.stale_after_ms);
    j.key("stats_endpoint").value(cfg_.stats_endpoint);
    j.key("command_endpoint").value(cfg_.command_endpoint);
    j.end_object();
    return j.str();
}

std::string ControlServer::stats_envelope() {
    const uint64_t now = now_monotonic_ns();
    const bool have = latest_stats_ns_ != 0;
    const uint64_t age_ms = have ? (now - latest_stats_ns_) / kNsPerMs : 0;
    JsonWriter j;
    j.begin_object();
    j.key("type").value("stats");
    j.key("update").value(++updates_sent_);
    j.key("server_time_ns").value(now_realtime_ns());
    j.key("receiver_online").value(have && age_ms <= static_cast<uint64_t>(cfg_.stale_after_ms));
    j.key("stats_age_ms");
    if (have) j.value(age_ms);
    else j.null();
    j.key("stats_received").value(stats_received_);
    j.key("stats");
    if (have) j.raw(latest_stats_);
    else j.null();
    j.end_object();
    return j.str();
}

}  // namespace ipcrelay::ctl
