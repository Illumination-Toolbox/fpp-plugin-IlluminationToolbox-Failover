#!/bin/bash
#
# Run the fpp-failover plugin inside a real fppd, against one FPP release.
#
#   bash tests/fpp_matrix.sh <fpp-image-tag> [real.fseq]
#   bash tests/fpp_matrix.sh 10.1.2
#   bash tests/fpp_matrix.sh 10.2 "~/Desktop/05 - Hot Chocolate.fseq"
#
# Brings up three containers from falconchristmas/fpp:<tag> on a private
# network - a primary, a backup and a remote - builds the plugin from this repo
# inside each one against that release's own headers, and checks it.  With a
# real .fseq it also runs the hot-standby test: the backup boots in Remote
# mode and mirrors the show, the primary is killed mid-song, and an independent
# remote is sampled to prove the song never restarts.
#
# Prints PASS / FAIL / INFO per check and exits non-zero on any FAIL.  Needs
# Docker.  Takes a few minutes; the hot-standby test adds a couple more.
#
# Container quirks this works around (none apply to a real FPP box):
#  - `docker restart` does not work on this image: a stale Apache pid file in
#    the media volume makes the entrypoint exit.  Containers are recreated.
#  - There is no systemd, so rsyncd is started by hand.

set -u
TAG="${1:?usage: fpp_matrix.sh <fpp-image-tag> [real.fseq]}"
SEQ="${2:-}"
IMAGE="falconchristmas/fpp:${TAG}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
NET="fpp-matrix"
SUBNET="172.31.250.0/24"
IP_PRIMARY=172.31.250.10
IP_BACKUP=172.31.250.11
IP_REMOTE=172.31.250.12
LAB="${TMPDIR:-/tmp}/fpp-matrix-${TAG}"

PASS=0; FAIL=0
pass() { echo "PASS: $*"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $*"; FAIL=$((FAIL + 1)); }
info() { echo "INFO: $*"; }

# Major.minor of the tag, as an integer, for version-dependent expectations.
vernum() { local v="${1#v}"; v="${v%%-*}"; local a="${v%%.*}" b; b="${v#*.}"; b="${b%%.*}"; [ "$b" = "$v" ] && b=0; echo $(( a * 100 + b )); }
VER=$(vernum "$TAG")

dx() { docker exec "fpp-$1" bash -c "$2"; }

# Plugin status JSON field, via the query form that works on every FPP 10.
status_field() {
    dx "$1" "curl -s -m 6 'http://127.0.0.1:32322/failover?action=status'" 2>/dev/null |
        python3 -c "import json,sys
try:
  d=json.load(sys.stdin)
  for k in sys.argv[1].split('.'): d=d[k]
  print(d)
except Exception: print('')" "$2"
}

fppd_alive() { dx "$1" "curl -s -m 5 http://127.0.0.1/api/fppd/status" 2>/dev/null | grep -q '"fppd"'; }

cleanup() {
    for n in primary backup remote; do docker rm -f "fpp-$n" >/dev/null 2>&1; done
    docker network rm "$NET" >/dev/null 2>&1
}

# start_node <name> <ip>
start_node() {
    find "$LAB/$1/media" -name '*.pid' -delete 2>/dev/null
    docker run -d --name "fpp-$1" --hostname "fpp-$1" --network "$NET" --ip "$2" \
        --cap-add=SYS_NICE -v "$LAB/$1/media:/home/fpp/media" "$IMAGE" >/dev/null
}

wait_fppd() {
    local i
    for i in $(seq 1 60); do fppd_alive "$1" && return 0; sleep 2; done
    return 1
}

# file_setting <name> <key> <value> - for settings fppd only reads at startup
# (fppMode, MultiSyncEnabled).  Written to the settings file; the caller then
# recreates the container so fppd starts with it.
file_setting() {
    local f="$LAB/$1/media/settings"
    if grep -q "^$2 *=" "$f" 2>/dev/null; then
        sed -i.bak "s/^$2 *=.*/$2 = \"$3\"/" "$f" && rm -f "$f.bak"
    else
        echo "$2 = \"$3\"" >> "$f"
    fi
}

install_plugin() {
    local d="$LAB/$1/media/plugins/fpp-failover"
    mkdir -p "$d"
    rsync -a --delete --exclude '.git' --exclude '*.o' --exclude '*.so' --exclude 'tests' "$REPO/" "$d/"
}

build_plugin() {
    dx "$1" 'cd /home/fpp/media/plugins/fpp-failover && make SRCDIR=/opt/fpp/src 2>&1 | grep -E " error|Error [0-9]"; chown -R fpp:fpp /home/fpp/media/plugins/fpp-failover; test -f libfpp-failover.so'
}

load_plugin()   { dx "$1" 'curl -s -m 30 -X POST http://127.0.0.1:32322/fppd/plugin/fpp-failover/load >/dev/null 2>&1'; }
unload_plugin() { dx "$1" 'curl -s -m 30 -X POST http://127.0.0.1:32322/fppd/plugin/fpp-failover/unload >/dev/null 2>&1'; }
psetting()      { dx "$1" "curl -s -m 10 -X PUT -d '$3' 'http://127.0.0.1/api/plugin/fpp-failover/settings/$2' >/dev/null"; }
setting()       { dx "$1" "curl -s -m 10 -X PUT -d '$3' 'http://127.0.0.1/api/settings/$2' >/dev/null"; }

start_rsyncd() {
    dx "$1" 'sed -e "s#FPPDIR#/opt/fpp#g" -e "s#FPPHOME#/home/fpp#g" -e "s#FPPUSER#fpp#g" < /opt/fpp/etc/rsyncd.conf > /etc/rsyncd.conf; mkdir -p /mnt/remote_filecopy; pgrep -x rsync >/dev/null || rsync --daemon --config=/etc/rsyncd.conf'
}

write_playlist() {
    mkdir -p "$LAB/$1/media/playlists"
    cat > "$LAB/$1/media/playlists/MainShow.json" <<EOF
{"name":"MainShow","version":4,"repeat":1,"loopCount":0,"empty":false,"random":0,
 "leadIn":[],"leadOut":[],
 "mainPlaylist":[{"type":"sequence","enabled":1,"playOnce":0,"sequenceName":"$2"}]}
EOF
}

echo "=== fpp-failover on ${IMAGE} (version code ${VER}) ==="
docker info >/dev/null 2>&1 || { echo "Docker is not running"; exit 2; }
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker pull -q "$IMAGE" >/dev/null || { echo "cannot pull $IMAGE"; exit 2; }
cleanup
rm -rf "$LAB"; mkdir -p "$LAB"/{primary,backup,remote}/media
[ "${KEEP:-0}" = "1" ] || trap cleanup EXIT   # KEEP=1 leaves the containers up to inspect
docker network create --subnet "$SUBNET" "$NET" >/dev/null

# Sequence for the show.  A real .fseq exercises hot standby; without one the
# player/cold checks still run on a playlist of short placeholder items.
SEQNAME="MatrixShow.fseq"
if [ -n "$SEQ" ]; then
    SEQ="${SEQ/#\~/$HOME}"
    [ -f "$SEQ" ] || { echo "no such sequence: $SEQ"; exit 2; }
    for n in primary backup remote; do mkdir -p "$LAB/$n/media/sequences"; cp "$SEQ" "$LAB/$n/media/sequences/$SEQNAME"; done
    write_playlist primary "$SEQNAME"; write_playlist backup "$SEQNAME"
fi

# ---------------------------------------------------------------- bring-up
start_node primary "$IP_PRIMARY"; start_node backup "$IP_BACKUP"; start_node remote "$IP_REMOTE"
for n in primary backup remote; do wait_fppd "$n" || { fail "$n: fppd did not come up"; exit 1; }; done
FPPVER=$(dx primary "curl -s -m 5 http://127.0.0.1/api/fppd/status" | python3 -c "import json,sys;print(json.load(sys.stdin).get('version','?'))")
info "fppd reports version ${FPPVER}"

# Settings fppd reads only at startup, so write them and recreate all three:
#  - the primary sends MultiSync (MultiSyncEnabled is restart-only; set at
#    runtime it is stored but the primary never opens its send socket)
#  - the backup is a Remote-mode hot standby, the remote node an FPP remote
file_setting primary MultiSyncEnabled 1
file_setting primary MultiSyncMulticast 1
file_setting backup fppMode remote
file_setting remote fppMode remote
for n in primary backup remote; do docker rm -f "fpp-$n" >/dev/null; done
start_node primary "$IP_PRIMARY"; start_node backup "$IP_BACKUP"; start_node remote "$IP_REMOTE"
for n in primary backup remote; do wait_fppd "$n" || { fail "$n: fppd did not come back"; exit 1; }; done
ms=$(dx primary "curl -s -m 5 http://127.0.0.1/api/fppd/status" | python3 -c "import json,sys;print(json.load(sys.stdin).get('multisync'))")
[ "$ms" = "True" ] && info "primary fppd has MultiSync sending enabled" || fail "primary fppd started without MultiSync (multisync=$ms)"

# ---------------------------------------------------------------- 1. build
for n in primary backup remote; do
    install_plugin "$n"
    if build_plugin "$n"; then pass "$n: plugin builds against FPP ${TAG} headers"
    else fail "$n: plugin build failed"; fi
done

# ---------------------------------------------------------------- 2. configure + live load
start_rsyncd primary
psetting primary Role primary;  psetting primary PeerAddress "$IP_BACKUP"; psetting primary FailbackMode manual
psetting backup  Role backup;   psetting backup  PeerAddress "$IP_PRIMARY"; psetting backup WitnessCheck 0
psetting remote  Role remote;   psetting remote  PeerAddress "$IP_PRIMARY"
for n in primary backup remote; do load_plugin "$n"; done
sleep 12
for n in primary backup remote; do
    r=$(status_field "$n" plugin)
    [ "$r" = "fpp-failover" ] && pass "$n: loads into a running fppd, no restart" || fail "$n: plugin not answering after live load"
done

# ---------------------------------------------------------------- 3. HTTP routes
code() { dx primary "curl -s -o /dev/null -w '%{http_code}' -m 6 '$1'"; }
c=$(code 'http://127.0.0.1/api/plugin-apis/failover?action=status')
[ "$c" = "200" ] && pass "query form /failover?action=status -> 200 (all FPP 10)" || fail "query form -> $c"
c=$(code 'http://127.0.0.1/api/plugin-apis/failover/status')
if [ "$VER" -ge 1002 ]; then
    [ "$c" = "200" ] && pass "subpath form /failover/status -> 200 (FPP 10.2 routing fix present)" \
                     || fail "subpath form -> $c on ${TAG}, expected 200 from 10.2 on"
else
    [ "$c" = "404" ] && pass "subpath form /failover/status -> 404 as expected before 10.2 (why the query form exists)" \
                     || info "subpath form -> $c on ${TAG} (expected 404 before 10.2)"
fi

# ---------------------------------------------------------------- 4. API docs merge (10.2 lists plugin routes)
DOCS=$(dx primary "curl -s -m 20 'http://127.0.0.1/api/openapi.json' 2>/dev/null || curl -s -m 20 'http://127.0.0.1/api/docs' 2>/dev/null")
if grep -q '"openapi"' <<<"$DOCS"; then
    if grep -q '/api/plugin-apis/api/plugin-apis' <<<"$DOCS"; then
        fail "API docs: plugin path doubled (/api/plugin-apis/api/plugin-apis/...)"
    elif python3 -c "import json,sys;d=json.load(sys.stdin);p=d['paths'].get('/api/plugin-apis/failover',{});sys.exit(0 if 'get' in p and 'post' in p else 1)" <<<"$DOCS"; then
        pass "API docs: /api/plugin-apis/failover documented with GET and POST"
    else
        fail "API docs: /api/plugin-apis/failover missing from the spec"
    fi
else
    info "API docs: no OpenAPI spec endpoint found on ${TAG}"
fi

# ---------------------------------------------------------------- 5. discovery + roles
sleep 4
pa=$(status_field primary peer.alive); ba=$(status_field backup peer.alive)
[ "$pa" = "True" ] && [ "$ba" = "True" ] && pass "peers discover each other" || fail "peer discovery: primary sees backup=$pa, backup sees primary=$ba"
hc=$(status_field backup hot.capable)
[ "$hc" = "True" ] && pass "backup booted in Remote mode reports hot standby capable" || fail "backup hot.capable=$hc"
w=$(status_field backup witness.addresses)
printf '%s' "$w" | grep -q "$IP_REMOTE" && pass "witness discovery finds the remote by hardware type" || fail "witness list: $w"

# ---------------------------------------------------------------- 6. hot standby (needs a real sequence)
if [ -n "$SEQ" ]; then
    dx primary "curl -s -m 15 'http://127.0.0.1/api/playlist/MainShow/start'" >/dev/null
    sleep 14
    locked=1
    for n in primary backup remote; do
        s=$(dx "$n" "curl -s -m 6 http://127.0.0.1/api/fppd/status" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('current_sequence') or '')")
        [ "$s" = "$SEQNAME" ] || locked=0
    done
    if [ $locked = 1 ]; then pass "all three nodes playing the real sequence"
    else
        fail "nodes not all playing $SEQNAME"
        for n in primary backup remote; do
            dx "$n" "curl -s -m 6 http://127.0.0.1/api/fppd/status" | python3 -c "import json,sys
d=json.load(sys.stdin); print('      $n:', d.get('mode_name'), d.get('status_name'), 'seq=', d.get('current_sequence') or '-', 'played=', d.get('seconds_played'))"
        done
        info "primary plugin state=$(status_field primary state) player=$(status_field primary player.playlist)"
    fi

    # Sample the remote once a second while the primary is killed.
    SAMPLES="$LAB/samples.txt"; : > "$SAMPLES"
    sample() {
        dx remote 'curl -s -m 2 http://127.0.0.1/api/fppd/status | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get(\"seconds_played\"),end=\" \")"; curl -s -m 2 http://127.0.0.1/api/fppd/multiSyncStats | python3 -c "import json,sys;print(json.load(sys.stdin).get(\"masterIP\"))"' 2>/dev/null
    }
    for i in 1 2 3 4; do sample >> "$SAMPLES"; sleep 1; done
    docker stop -t 0 fpp-primary >/dev/null
    for i in $(seq 1 16); do sample >> "$SAMPLES"; sleep 1; done
    if python3 - "$SAMPLES" "$IP_BACKUP" <<'PY'
import sys
rows=[l.split() for l in open(sys.argv[1]) if len(l.split())==2 and l.split()[0].isdigit()]
pos=[int(r[0]) for r in rows]; src=[r[1] for r in rows]
mono=all(b>=a for a,b in zip(pos,pos[1:]))
switched=sys.argv[2] in src
print("  remote position:", " ".join(map(str,pos)))
print("  sync source    :", " ".join(s.split(".")[-1] for s in src))
sys.exit(0 if (mono and switched and len(pos)>=10) else 1)
PY
    then pass "hot standby: primary killed mid-song, backup took over as sync source, position never went backwards"
    else fail "hot standby: song restarted or the sync source never moved to the backup"; fi
    log=$(dx backup 'grep -c "YIELDED" /home/fpp/media/logs/plugin-fpp-failover.log' 2>/dev/null)
    [ "${log:-0}" = "0" ] && pass "hot standby: single clean takeover, no flap" || fail "hot standby: backup yielded during the takeover ($log times)"
    m=$(dx backup "grep -m1 '^fppMode' /home/fpp/media/settings")
    printf '%s' "$m" | grep -q remote && pass "backup on-disk mode still Remote after takeover" || fail "backup on-disk mode changed: $m"
else
    info "no real sequence given: skipping the hot-standby test"
fi

# ---------------------------------------------------------------- 7. unload/reload under load
# Exercises the unload path while the channel output thread is calling the
# plugin every frame.  FPP before 10.2 walks that list without a lock; the
# plugin's shutdown() waits before FPP unmaps it.  Reloads are immediate, so
# the old instance is still settling when the new one loads.
TARGET=backup; [ -n "$SEQ" ] || TARGET=primary
cycles=12; ok=1
for i in $(seq 1 $cycles); do
    unload_plugin "$TARGET"; load_plugin "$TARGET"
    fppd_alive "$TARGET" || { ok=0; break; }
done
sleep 4
r=$(status_field "$TARGET" plugin)
[ $ok = 1 ] && [ "$r" = "fpp-failover" ] && pass "$TARGET: ${cycles} immediate unload/reload cycles, fppd alive, plugin answering" \
                                        || fail "$TARGET: fppd died or plugin missing during unload/reload cycling (cycle $i)"

echo
echo "RESULT ${TAG}: ${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ]
