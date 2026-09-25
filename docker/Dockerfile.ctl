# ipc-relay-ctl web control image: the control tool's backend with its
# browser frontend compiled in. Serves the page on port 8083, pushes the
# receiver's statistics to it over a WebSocket and forwards start/stop
# recording to the receiver.
#
#   docker build -f docker/Dockerfile.build -t ipc-relay-build .
#   docker build -f docker/Dockerfile.ctl -t ipc-relay-ctl .
#   docker run --rm -p 8083:8083 ipc-relay-ctl \
#       --stats-endpoint tcp://RECEIVER:5556 --command-endpoint tcp://RECEIVER:5557 --connect
#
# Build context: repository root. The receiver must bind its command endpoint
# (command_bind = true, as in docker/config/receiver.conf) because this
# container connects to it. Then open http://localhost:8083/.

ARG BUILD_IMAGE=ipc-relay-build:latest
ARG DEBIAN_RELEASE=bookworm
FROM ${BUILD_IMAGE} AS build

FROM debian:${DEBIAN_RELEASE}-slim

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends libzmq5 \
 && rm -rf /var/lib/apt/lists/*

RUN useradd --system --uid 10001 --user-group --no-create-home ipcrelay

COPY --from=build /opt/ipc-relay/bin/ipc-relay-ctl /usr/local/bin/

USER ipcrelay
# Control web page + WebSocket (/ws).
EXPOSE 8083
ENTRYPOINT ["ipc-relay-ctl", "serve", "--http", "0.0.0.0:8083"]
CMD ["--stats-endpoint", "tcp://receiver:5556", \
     "--command-endpoint", "tcp://receiver:5557", "--connect", \
     "--update-ms", "1000"]
