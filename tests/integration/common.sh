# shellcheck shell=bash
# Shared helpers for the integration scripts. Sourced, not executed.
set -u

: "${BRIDGE:?BRIDGE not set}"
: "${RECEIVER:?RECEIVER not set}"
: "${TESTPUB:?TESTPUB not set}"
: "${INSPECT:?INSPECT not set}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ipcrelay-test.XXXXXX")"
PIDS=()

cleanup() {
    for p in "${PIDS[@]:-}"; do
        if [ -n "$p" ]; then kill -TERM "$p" 2>/dev/null || true; fi
    done
    sleep 0.3
    for p in "${PIDS[@]:-}"; do
        if [ -n "$p" ]; then kill -KILL "$p" 2>/dev/null || true; fi
    done
    if [ "${KEEP_WORK:-0}" = "1" ]; then
        echo "work dir kept: $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

# Unique multicast group / ports per run so parallel ctest jobs do not collide.
SALT=$(( $$ % 200 + 1 ))
MCAST_GROUP="${MCAST_GROUP:-239.255.$SALT.$(( RANDOM % 200 + 1 ))}"
MCAST_PORT="${MCAST_PORT:-$(( 30000 + RANDOM % 20000 ))}"
MCAST_IFACE="${MCAST_IFACE:-127.0.0.1}"
STATS_PORT=$(( 20000 + RANDOM % 9000 ))
CMD_PORT=$(( STATS_PORT + 1 ))
export STATS_PORT CMD_PORT

fail() {
    echo "FAIL: $*" >&2
    echo "--- bridge log ---" >&2;   cat "$WORK/bridge.log" 2>/dev/null >&2 || true
    echo "--- receiver log ---" >&2; cat "$WORK/receiver.log" 2>/dev/null >&2 || true
    exit 1
}

# wait_for_log FILE PATTERN [TIMEOUT_S]
wait_for_log() {
    local file="$1" pat="$2" timeout="${3:-10}" i=0
    while ! grep -q -- "$pat" "$file" 2>/dev/null; do
        i=$(( i + 1 ))
        [ "$i" -ge $(( timeout * 10 )) ] && return 1
        sleep 0.1
    done
    return 0
}

# stop_and_wait PID -> exit code
stop_and_wait() {
    local pid="$1"
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid"
}

# final_stats_json LOGFILE -> prints the JSON object from the "final stats:" line
final_stats_json() {
    grep 'final stats: ' "$1" | tail -1 | sed 's/.*final stats: //'
}

# json_get JSON PYTHON_EXPR  (e.g. json_get "$J" "d['sources']['1']['messages']")
json_get() {
    python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print('"$2"')' "$1"
}
