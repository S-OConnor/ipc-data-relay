#include "ipcrelay/zmq_util.hpp"

#include <cerrno>
#include <cstring>

namespace ipcrelay {

std::string zmq_error_string(int err) { return zmq_strerror(err); }

std::string zmq_version_string() {
    int major = 0, minor = 0, patch = 0;
    zmq_version(&major, &minor, &patch);
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

int zmq_recv_multipart_nowait(void* socket, std::vector<std::vector<uint8_t>>& frames) {
    frames.clear();
    for (;;) {
        zmq_msg_t msg;
        zmq_msg_init(&msg);
        int rc = zmq_msg_recv(&msg, socket, ZMQ_DONTWAIT);
        if (rc < 0) {
            int err = errno;
            zmq_msg_close(&msg);
            if (err == EAGAIN && frames.empty()) return 0;
            if (err == EINTR) continue;
            errno = err;
            return -1;
        }
        const auto* data = static_cast<const uint8_t*>(zmq_msg_data(&msg));
        frames.emplace_back(data, data + zmq_msg_size(&msg));
        int more = zmq_msg_more(&msg);
        zmq_msg_close(&msg);
        if (!more) return 1;
    }
}

bool zmq_send_frames(void* socket, const std::vector<std::string>& frames, bool dont_wait) {
    for (std::size_t i = 0; i < frames.size(); ++i) {
        int flags = (i + 1 < frames.size()) ? ZMQ_SNDMORE : 0;
        if (dont_wait) flags |= ZMQ_DONTWAIT;
        int rc = zmq_send(socket, frames[i].data(), frames[i].size(), flags);
        if (rc < 0) return false;
    }
    return true;
}

}  // namespace ipcrelay
