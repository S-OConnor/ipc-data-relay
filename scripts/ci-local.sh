#!/usr/bin/env bash
# Runs the GitLab CI pipeline locally with gitlab-ci-local
# (https://github.com/firecow/gitlab-ci-local), working around two common
# host quirks:
#   * picks docker if its daemon is reachable, otherwise rootless podman;
#   * on hosts with rsync 3.5.0 (Fedora 44 and friends) gitlab-ci-local's
#     internal "rsync --exclude-from=<(...)" fails because of
#     RsyncProject/rsync#1083; a wrapper that materialises the /dev/fd path is
#     put on PATH when that bug is detected.
#
# Usage: scripts/ci-local.sh [gitlab-ci-local arguments...]
#   scripts/ci-local.sh                  # whole pipeline
#   scripts/ci-local.sh --list           # show jobs
#   scripts/ci-local.sh integration-tests
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

command -v gitlab-ci-local >/dev/null || { echo "gitlab-ci-local is not installed (npm i -g gitlab-ci-local or brew install gitlab-ci-local)" >&2; exit 1; }
command -v rsync >/dev/null || { echo "rsync is required by gitlab-ci-local" >&2; exit 1; }

probe="$(mktemp -d)"
if ! bash -c 'rsync -a --dry-run --exclude-from=<(echo /x) "$1/" "$2/" >/dev/null 2>&1' _ "$HERE" "$probe"; then
    echo "ci-local: rsync cannot read process-substitution paths (rsync#1083); using wrapper" >&2
    export REAL_RSYNC="$(command -v rsync)"
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
[ -n "${CONTAINER_EXECUTABLE:-}" ] && args+=(--container-executable "$CONTAINER_EXECUTABLE")

cd "$ROOT"
exec gitlab-ci-local "${args[@]}" "$@"
