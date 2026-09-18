// Small helpers around libzmq's C API.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <zmq.h>

namespace ipcrelay {

std::string zmq_error_string(int err);
std::string zmq_version_string();

// Receives all frames of the next message from a socket without blocking.
// Returns 1 if a message was received, 0 if none was available, -1 on error
// (errno set). frames is cleared and filled with each frame's bytes.
int zmq_recv_multipart_nowait(void* socket, std::vector<std::vector<uint8_t>>& frames);

// Sends a message with an optional topic frame. Returns true on success.
bool zmq_send_frames(void* socket, const std::vector<std::string>& frames, bool dont_wait);

}  // namespace ipcrelay
