#include "ipcrelay/signal_handler.hpp"

#include <csignal>

namespace ipcrelay {

static volatile std::sig_atomic_t g_shutdown = 0;
static volatile std::sig_atomic_t g_signal = 0;

static void on_signal(int sig) {
    g_shutdown = 1;
    g_signal = sig;
}

void install_shutdown_handlers() {
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: blocking calls return EINTR so loops notice promptly
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

bool shutdown_requested() { return g_shutdown != 0; }
int shutdown_signal() { return static_cast<int>(g_signal); }

}  // namespace ipcrelay
