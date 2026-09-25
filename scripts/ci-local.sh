#!/usr/bin/env bash
# Runs the GitLab CI pipeline locally with gitlab-ci-local
# (https://github.com/firecow/gitlab-ci-local), working around two common
# host quirks:
#   * picks docker if its daemon is reachable, otherwise rootless podman;
#   * on hosts with rsync 3.5.0 (Fedora 44 and friends) gitlab-ci-local's
#     internal "rsync --exclude-from=<(...)" fails because of
#     RsyncProject/rsync#1083; a wrapper that materialises the /dev/fd path is
#     put on PATH when that bug is detected.
# The image jobs (build-image, runtime-images, frontend-image) talk to the
# host's docker/podman through its socket instead of docker-in-docker, so the
# images land in local storage (localhost/ipc-relay-*:ci-local, nothing is
# pushed) and the later jobs can use localhost/ipc-relay-build:ci-local as
# their job image. This needs privileged job containers (also for the dind
# service, which is started but unused).
#
# Usage: scripts/ci-local.sh [gitlab-ci-local arguments...]
#   scripts/ci-local.sh                  # whole pipeline
#   scripts/ci-local.sh --list           # show jobs
#   scripts/ci-local.sh integration-tests
#   scripts/ci-local.sh --needs unit-tests   # a job plus the jobs it needs
# scripts/pipeline.sh runs the whole pipeline stage by stage.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

command -v gitlab-ci-local >/dev/null || { echo "gitlab-ci-local is not installed (npm i -g gitlab-ci-local or brew install gitlab-ci-local)" >&2; exit 1; }
command -v rsync >/dev/null || { echo "rsync is required by gitlab-ci-local" >&2; exit 1; }

probe="$(mktemp -d)"
if ! bash -c 'rsync -a --dry-run --exclude-from=<(echo /x) "$1/" "$2/" >/dev/null 2>&1' _ "$HERE" "$probe"; then
    echo "ci-local: rsync cannot read process-substitution paths (rsync#1083); using wrapper" >&2
    REAL_RSYNC="$(command -v rsync)"
    export REAL_RSYNC
    export PATH="$HERE/rsync-wrapper:$PATH"
fi
rmdir "$probe"

# Do not copy job artifacts (the CI build tree) back into the working copy.
args=(--artifacts-to-source=false)
if [ -z "${CONTAINER_EXECUTABLE:-}" ]; then
    if docker info >/dev/null 2>&1; then
        CONTAINER_EXECUTABLE=docker
    elif command -v podman >/dev/null; then
        CONTAINER_EXECUTABLE=podman
        systemctl --user is-active --quiet podman.socket 2>/dev/null || echo "ci-local: hint: 'systemctl --user start podman.socket' if podman is not reachable" >&2
    fi
fi
[ -n "${CONTAINER_EXECUTABLE:-}" ] || { echo "ci-local: neither a docker daemon nor podman is available" >&2; exit 1; }
args+=(--container-executable "$CONTAINER_EXECUTABLE")

# Socket of the host engine, mounted into the image jobs as /var/run/docker.sock.
if [ -z "${ENGINE_SOCKET:-}" ]; then
    if [ "$CONTAINER_EXECUTABLE" = podman ]; then
        ENGINE_SOCKET="$(podman info --format '{{.Host.RemoteSocket.Path}}')"
    else
        ENGINE_SOCKET=/var/run/docker.sock
    fi
fi
[ -S "$ENGINE_SOCKET" ] || { echo "ci-local: container engine socket $ENGINE_SOCKET not found" >&2; exit 1; }
args+=(--privileged --volume "$ENGINE_SOCKET:/var/run/docker.sock"
       --variable DOCKER_HOST=unix:///var/run/docker.sock
       --variable DOCKER_TLS_VERIFY=
       --variable DOCKER_CERT_PATH=
       --variable IMAGE_PREFIX=localhost
       --variable IMAGE_TAG=ci-local
       --variable PUSH_IMAGES=false)
# podman's Docker-compatible API has no BuildKit endpoint.
[ "$CONTAINER_EXECUTABLE" = podman ] && args+=(--variable DOCKER_BUILDKIT=0)

cd "$ROOT"
exec gitlab-ci-local "${args[@]}" "$@"
