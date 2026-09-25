#!/usr/bin/env bash
# End-to-end pipeline test (BRG-131, BRG-132, BRG-133, BRG-135, BRG-137):
#   testpub (3 IPC endpoints) -> bridge (3 SUB sockets) -> UDP multicast
#   -> receiver -> capture file -> capture_inspect.py and ipc-relay-capture-to-csv
# while an independent subscriber on one endpoint verifies that the bridge
# does not steal any messages.
set -e
# shellcheck source=tests/integration/common.sh
. "$(dirname "$0")/common.sh"
: "${SUB_COUNTER:?SUB_COUNTER not set}"
: "${CAPTURE_TO_CSV:?CAPTURE_TO_CSV not set}"

PER_SOURCE=${PER_SOURCE:-1500}
RATE=${RATE:-3000}
EP1="ipc://$WORK/src1.sock"
EP2="ipc://$WORK/src2.sock"
EP3="ipc://$WORK/src3.sock"
CAP="$WORK/capture.cap"

cat > "$WORK/bridge.conf" <<CONF
multicast_group = $MCAST_GROUP
multicast_port = $MCAST_PORT
multicast_interface = $MCAST_IFACE
multicast_ttl = 0
max_datagram_size = 1400
stats_interval_ms = 1000
log_level = info

[source]
id = 1
name = alpha
endpoint = $EP1

[source]
id = 2
name = beta
endpoint = $EP2

[source]
id = 3
name = gamma
endpoint = $EP3
filter = TLM
CONF

cat > "$WORK/receiver.conf" <<CONF
multicast_group = $MCAST_GROUP
multicast_port = $MCAST_PORT
multicast_interface = $MCAST_IFACE
capture_file = $CAP
stats_endpoint = tcp://127.0.0.1:$STATS_PORT
command_endpoint = tcp://127.0.0.1:$CMD_PORT
stats_interval_ms = 500
reassembly_timeout_ms = 500
log_level = info
CONF

"$BRIDGE" --config "$WORK/bridge.conf" --check >/dev/null || fail "bridge config check failed"
"$RECEIVER" --config "$WORK/receiver.conf" --check >/dev/null || fail "receiver config check failed"

"$RECEIVER" --config "$WORK/receiver.conf" >"$WORK/receiver.log" 2>&1 &
RECV_PID=$!; PIDS+=("$RECV_PID")
wait_for_log "$WORK/receiver.log" "receiver running" || fail "receiver did not start"

"$BRIDGE" --config "$WORK/bridge.conf" >"$WORK/bridge.log" 2>&1 &
BRIDGE_PID=$!; PIDS+=("$BRIDGE_PID")
wait_for_log "$WORK/bridge.log" "bridge running" || fail "bridge did not start"

# Independent subscriber on source 1's endpoint (BRG-131).
"$SUB_COUNTER" --endpoint "$EP1" --expect "$PER_SOURCE" --timeout-ms 30000 >"$WORK/sub_counter.out" &
SUB_PID=$!; PIDS+=("$SUB_PID")
sleep 0.3

# Sizes 64..5000 bytes so most messages need fragmentation (BRG-135).
"$TESTPUB" -e "$EP1" -e "$EP2" -e "$EP3" --count "$PER_SOURCE" --rate "$RATE" \
    --size 64 --size-max 5000 --topic TLM --settle-ms 800 --linger-ms 500 --quiet \
    >"$WORK/testpub.log" 2>&1 || fail "testpub failed"

wait "$SUB_PID" || true
read -r SUB_COUNT SUB_BYTES <"$WORK/sub_counter.out"
[ "$SUB_COUNT" = "$PER_SOURCE" ] || fail "independent subscriber got $SUB_COUNT messages, expected $PER_SOURCE (bridge interfered?)"
echo "independent subscriber received all $SUB_COUNT messages ($SUB_BYTES bytes)"

sleep 1
stop_and_wait "$BRIDGE_PID" || fail "bridge exited with an error"
stop_and_wait "$RECV_PID" || fail "receiver exited with an error"
PIDS=()

J="$(final_stats_json "$WORK/receiver.log")"
[ -n "$J" ] || fail "receiver did not print final stats"
TOTAL=$(( PER_SOURCE * 3 ))
MSGS=$(json_get "$J" "d['messages_received']")
KDROPS=$(json_get "$J" "d['kernel_drops']")
MALFORMED=$(json_get "$J" "d['malformed_packets']")
INCOMPLETE=$(json_get "$J" "d['incomplete_fragments']")
echo "receiver: messages=$MSGS kernel_drops=$KDROPS malformed=$MALFORMED incomplete=$INCOMPLETE"
[ "$MALFORMED" = "0" ] || fail "receiver reported malformed packets"
[ "$MSGS" = "$TOTAL" ] || fail "receiver got $MSGS messages, expected $TOTAL (kernel_drops=$KDROPS)"

python3 "$INSPECT" --verify-testpub --strict --expect-sources 3 --expect-per-source "$PER_SOURCE" "$CAP" \
    || fail "capture validation failed"

# One CSV per testpub data type, one row per message (source N -> type N).
"$CAPTURE_TO_CSV" -o "$WORK/csv" "$CAP" >/dev/null || fail "capture-to-csv failed"
for t in board_health mode_status ptp_stats; do
    ROWS=$(( $(wc -l <"$WORK/csv/$t.csv") - 1 ))
    [ "$ROWS" = "$PER_SOURCE" ] || fail "$t.csv has $ROWS rows, expected $PER_SOURCE"
done
[ ! -e "$WORK/csv/unknown.csv" ] || fail "capture-to-csv could not decode some payloads"
grep -q 'final statistics' "$WORK/bridge.log" || fail "bridge did not print final statistics"
echo "e2e OK: $TOTAL messages over 3 sources captured and verified"
