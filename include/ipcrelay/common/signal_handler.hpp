// SIGINT/SIGTERM handling for orderly shutdown (BRG-110).
#pragma once

namespace ipcrelay {

// Installs handlers for SIGINT and SIGTERM that set the shutdown flag.
void install_shutdown_handlers();
bool shutdown_requested();
int shutdown_signal();  // 0 if none

}  // namespace ipcrelay
