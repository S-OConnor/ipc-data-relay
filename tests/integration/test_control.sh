#!/usr/bin/env bash
# Runtime control over ZeroMQ TCP (BRG-075A..BRG-079):
#   - recording starts disabled; messages are received but not recorded
#   - "record on" over the command channel starts recording
#   - "record off" stops recording while reception/statistics continue
#   - the statistics channel reports the recording state and counters
set -e
# shellcheck source=tests/integration/common.sh
. "$(dirname "$0")/common.sh"
: "${CTL:?CTL not set}"

N1=200; N2=300; N3=250
EP="ipc://$WORK/ctl.sock"
CAP="$WORK/capture.cap"

cat > "$WORK/receiver.conf" <<CONF
multicast_group = $MCAST_GROUP
multicast_port = $MCAST_PORT
multicast_interface = $MCAST_IFACE
capture_file = $CAP
record_on_start = false
stats_endpoint = tcp://127.0.0.1:$STATS_PORT
stats_interval_ms = 200
command_endpoint = tcp://127.0.0.1:$CMD_PORT
command_bind = false
log_level = info
CONF

"$RECEIVER" --config "$WORK/receiver.conf" >"$WORK/receiver.log" 2>&1 &
RECV_PID=$!; PIDS+=("$RECV_PID")
wait_for_log "$WORK/receiver.log" "receiver running" || fail "receiver did not start"

"$BRIDGE" --source "7=$EP" --set multicast_group="$MCAST_GROUP" --set multicast_port="$MCAST_PORT" \
    --set multicast_interface="$MCAST_IFACE" --set multicast_ttl=0 >"$WORK/bridge.log" 2>&1 &
BRIDGE_PID=$!; PIDS+=("$BRIDGE_PID")
wait_for_log "$WORK/bridge.log" "bridge running" || fail "bridge did not start"

pub() { "$TESTPUB" -e "$EP" --count "$1" --rate 0 --size 100 --settle-ms 600 --linger-ms 300 --quiet 2>>"$WORK/testpub.log" || fail "testpub failed"; }

pub "$N1"
sleep 0.5
[ ! -e "$CAP" ] || fail "capture file created although record_on_start = false"

"$CTL" send --endpoint "tcp://127.0.0.1:$CMD_PORT" --settle-ms 700 record on 2>>"$WORK/ctl.log" || fail "ctl send failed"
wait_for_log "$WORK/receiver.log" "recording ENABLED" 3 || fail "receiver did not enable recording"
pub "$N2"
sleep 0.5

"$CTL" send --endpoint "tcp://127.0.0.1:$CMD_PORT" --settle-ms 700 record off 2>>"$WORK/ctl.log" || fail "ctl send failed"
wait_for_log "$WORK/receiver.log" "recording DISABLED" 3 || fail "receiver did not disable recording"
pub "$N3"
sleep 0.5

# Statistics over the ZeroMQ TCP channel (BRG-075A/B).
"$CTL" monitor --endpoint "tcp://127.0.0.1:$STATS_PORT" --count 1 >"$WORK/stats.json" 2>>"$WORK/ctl.log" &
MON_PID=$!; PIDS+=("$MON_PID")
wait "$MON_PID" || fail "ctl monitor failed"
J="$(cat "$WORK/stats.json")"
[ -n "$J" ] || fail "no statistics message received"
TOTAL=$(( N1 + N2 + N3 ))
MSGS=$(json_get "$J" "d['messages_received']")
REC=$(json_get "$J" "d['recording']['records_written']")
EN=$(json_get "$J" "d['recording']['enabled']")
SRC=$(json_get "$J" "d['sources']['7']['messages']")
UP=$(json_get "$J" "d['uptime_s'] > 0")
echo "stats: messages=$MSGS records=$REC enabled=$EN source7=$SRC"
[ "$MSGS" = "$TOTAL" ] || fail "expected $TOTAL messages received, got $MSGS"
[ "$SRC" = "$TOTAL" ] || fail "per-source count $SRC != $TOTAL"
[ "$REC" = "$N2" ] || fail "expected $N2 records written, got $REC"
[ "$EN" = "False" ] || fail "recording should be reported disabled"
[ "$UP" = "True" ] || fail "uptime missing"

stop_and_wait "$BRIDGE_PID" || fail "bridge exited with an error"
stop_and_wait "$RECV_PID" || fail "receiver exited with an error"
PIDS=()
python3 "$INSPECT" --verify-testpub --expect-sources 1 --expect-messages "$N2" "$CAP" || fail "capture validation failed"
echo "control OK: $N2 of $TOTAL messages recorded as commanded"
