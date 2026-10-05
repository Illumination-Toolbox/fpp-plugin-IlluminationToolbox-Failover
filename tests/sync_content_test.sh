#!/bin/bash
# End-to-end test of scripts/sync_content.sh against a throwaway local rsync
# daemon. Needs a real rsync 3.x (on macOS: brew install rsync). Run from anywhere:
#   bash tests/sync_content_test.sh
set -u
export PATH="/opt/homebrew/bin:$PATH"
SCRIPT="$(cd "$(dirname "$0")/.." && pwd)/scripts/sync_content.sh"
ROOT=$(mktemp -d /tmp/fosync.XXXXXX)
PRIM="$ROOT/primary"      # served by the daemon as module "media"
LOCAL="$ROOT/local"       # MEDIADIR of the backup
PORT=8873
mkdir -p "$PRIM/sequences" "$PRIM/playlists" "$PRIM/config" "$LOCAL/logs" "$LOCAL/plugindata/fpp-plugin-IlluminationToolbox-Failover"

cat > "$ROOT/rsyncd.conf" <<EOF
pid file = $ROOT/rsyncd.pid
log file = $ROOT/rsyncd.log
lock file = $ROOT/rsyncd.lock
use chroot = no
max connections = 4
[media]
path = $PRIM
read only = no
list = no
EOF
rsync --daemon --no-detach --config="$ROOT/rsyncd.conf" --port=$PORT --address=127.0.0.1 &
DPID=$!
sleep 1

export FPPDIR="$ROOT/nofpp" MEDIADIR="$LOCAL" LOGDIR="$LOCAL/logs"
export SYNC_RSYNC_PORT=$PORT SYNC_DIRS=sequences SYNC_PLAYLISTS=1 SYNC_SCHEDULE=1 SYNC_SNAPSHOTS=3 SYNC_DELETE=0
DATA="$LOCAL/plugindata/fpp-plugin-IlluminationToolbox-Failover"
PASS=0; FAIL=0
check() { if eval "$2"; then echo "PASS: $1"; PASS=$((PASS+1)); else echo "FAIL: $1"; FAIL=$((FAIL+1)); fi; }
run() { bash "$SCRIPT" 127.0.0.1 "$@"; echo "  -> exit $?"; }
jget() { python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d[sys.argv[2]])" "$1" "$2"; }

# 1. seed the primary
echo "v1 of a" > "$PRIM/sequences/a.fseq"; echo "v1 of b" > "$PRIM/sequences/b.fseq"
echo '{"name":"show"}' > "$PRIM/playlists/show.json"; echo '{"name":"fpp-failover-hold"}' > "$PRIM/playlists/fpp-failover-hold.json"
echo '[{"v":1}]' > "$PRIM/config/schedule.json"
echo '{"local hold"}' > "$LOCAL/playlists/fpp-failover-hold.json" 2>/dev/null || { mkdir -p "$LOCAL/playlists"; echo '{"local hold"}' > "$LOCAL/playlists/fpp-failover-hold.json"; }

echo "== check on empty local"; run check
V=$(jget $DATA/sync-pending.json count); check "check reports 4 pending (2 seq + playlist + schedule)" "[ '$V' = 4 ]"
check "hold playlist excluded from pending" "! grep -q fpp-failover-hold $DATA/sync-pending.json"

echo "== promote #1"; run promote
V=$(jget $DATA/sync-status.json ok); check "promote ok" "[ '$V' = True ]"
check "a.fseq arrived" "[ \"$(cat $LOCAL/sequences/a.fseq)\" = 'v1 of a' ]"
check "schedule arrived" "[ -f $LOCAL/config/schedule.json ]"
check "local hold playlist untouched" "grep -q 'local hold' $LOCAL/playlists/fpp-failover-hold.json"
check "one snapshot created" "[ $(ls -1 $DATA/snapshots | wc -l) -eq 1 ]"
V=$(jget $DATA/sync-pending.json count); check "pending reset to 0" "[ '$V' = 0 ]"

echo "== promote again with nothing new"; run promote
check "nothingToDo reported" "grep -q '\"nothingToDo\":true' $DATA/sync-status.json"
check "no extra snapshot" "[ $(ls -1 $DATA/snapshots | wc -l) -eq 1 ]"

echo "== change the primary: a.fseq v2, new c.fseq"
sleep 1; echo "v2 of a" > "$PRIM/sequences/a.fseq"; echo "v1 of c" > "$PRIM/sequences/c.fseq"
run check
V=$(jget $DATA/sync-pending.json count); check "check reports 2 pending" "[ '$V' = 2 ]"
check "pending lists c.fseq as new" "grep -q '\"name\":\"c.fseq\",\"change\":\"new\"' $DATA/sync-pending.json"
check "pending lists a.fseq as changed" "grep -q '\"name\":\"a.fseq\",\"change\":\"changed\"' $DATA/sync-pending.json"
FP1=$(jget $DATA/sync-pending.json fingerprint)
run check
V=$(jget $DATA/sync-pending.json fingerprint); check "fingerprint stable across identical checks" "[ '$V' = '$FP1' ]"

echo "== promote #2"; run promote
check "a.fseq updated to v2" "[ \"$(cat $LOCAL/sequences/a.fseq)\" = 'v2 of a' ]"
check "c.fseq arrived" "[ -f $LOCAL/sequences/c.fseq ]"
check "two snapshots now" "[ $(ls -1 $DATA/snapshots | wc -l) -eq 2 ]"
SNAP2=$(jget $DATA/sync-status.json snapshot)
check "snapshot holds old a.fseq (v1)" "[ \"$(cat $DATA/snapshots/$SNAP2/sequences/a.fseq)\" = 'v1 of a' ]"
check "snapshot b.fseq is a hard link (space-free)" "[ $(stat -f %l $DATA/snapshots/$SNAP2/sequences/b.fseq) -ge 2 ]"

echo "== rollback local to $SNAP2"; run rollback "$SNAP2" local
V=$(jget $DATA/sync-status.json ok); check "rollback ok" "[ '$V' = True ]"
check "local a.fseq back to v1" "[ \"$(cat $LOCAL/sequences/a.fseq)\" = 'v1 of a' ]"
check "primary untouched by local rollback" "[ \"$(cat $PRIM/sequences/a.fseq)\" = 'v2 of a' ]"

echo "== rollback primary to $SNAP2"; run rollback "$SNAP2" primary
check "primary a.fseq back to v1" "[ \"$(cat $PRIM/sequences/a.fseq)\" = 'v1 of a' ]"
check "pre-rollback snapshot created" "ls -1 $DATA/snapshots | grep -q pre-rollback"
PRE=$(ls -1 $DATA/snapshots | grep pre-rollback | head -1)
check "pre-rollback snapshot holds primary's v2" "[ \"$(cat $DATA/snapshots/$PRE/sequences/a.fseq)\" = 'v2 of a' ]"
check "pre-rollback snapshot holds c.fseq" "[ -f $DATA/snapshots/$PRE/sequences/c.fseq ]"

echo "== undo: rollback both to $PRE"; run rollback "$PRE" both
check "primary a.fseq forward to v2 again" "[ \"$(cat $PRIM/sequences/a.fseq)\" = 'v2 of a' ]"
check "local a.fseq forward to v2 again" "[ \"$(cat $LOCAL/sequences/a.fseq)\" = 'v2 of a' ]"

echo "== pruning (keep 3)"
for i in 1 2; do sleep 1; echo "v$((i+2)) of a" > "$PRIM/sequences/a.fseq"; run promote >/dev/null; done
check "snapshots pruned to 3" "[ $(ls -1 $DATA/snapshots | wc -l) -eq 3 ]"

echo "== daemon down: friendly error"
kill $DPID; wait $DPID 2>/dev/null; sleep 0.5
run check
check "check reports rsync daemon error" "grep -q \"enable 'rsync' under Settings\" $DATA/sync-pending.json"
BEFORE=$(ls -1 $DATA/snapshots | wc -l)
run promote
V=$(jget $DATA/sync-status.json ok); check "promote fails cleanly" "[ '$V' = False ]"
check "failed promote leaves snapshots untouched" "[ $(ls -1 $DATA/snapshots | wc -l) -eq $BEFORE ]"

echo; echo "log tail:"; tail -4 "$LOCAL/logs/plugin-fpp-plugin-IlluminationToolbox-Failover.log"
echo; echo "RESULT: $PASS passed, $FAIL failed"
rm -rf "$ROOT"
[ $FAIL -eq 0 ]
