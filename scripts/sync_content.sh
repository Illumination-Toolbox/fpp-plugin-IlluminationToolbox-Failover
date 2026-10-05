#!/bin/bash
#
# fpp-plugin-IlluminationToolbox-Failover content pipeline.  The primary is where new sequences land
# ("cutting edge"); this node keeps the last promoted, known-good set
# ("stable") and a history of snapshots to roll back to.
#
# usage: sync_content.sh <primary-address> check
#        sync_content.sh <primary-address> promote
#        sync_content.sh <primary-address> rollback <snapshot-id|latest> <local|primary|both>
#
#   check     dry run: what would change here if the primary's content were
#             promoted now.  Writes SYNC_PENDING_FILE.  Touches nothing.
#   promote   snapshot the current stable set, then pull the primary's
#             content over it (FPP's own rsync daemon, same transport the
#             MultiSync page uses).  Writes SYNC_STATUS_FILE.
#   rollback  restore a snapshot onto this node and/or the primary.  The
#             primary's current content is snapshotted first ("pre-rollback")
#             so a rollback is itself reversible.  Writes SYNC_STATUS_FILE.
#
# Options from the environment (set by the plugin):
#   SYNC_DIRS          comma list of media dirs (default below)
#   SYNC_PLAYLISTS     1 to include playlists/ (the hold playlist is left alone)
#   SYNC_SCHEDULE      1 to include config/schedule.json (reloaded after change)
#   SYNC_DELETE        1 to delete files the source no longer has
#   SYNC_COMPRESS      1 to compress transfers
#   SYNC_SNAPSHOTS     how many snapshots to keep (0 = no snapshots)
#   SYNC_STATUS_FILE   result JSON for promote/rollback
#   SYNC_PENDING_FILE  result JSON for check
#   SYNC_SNAPSHOT_DIR  where snapshots live
#
# Snapshots are rsync --link-dest copies: unchanged files are hard links, so a
# snapshot costs only the space of files that later change.  rsync writes each
# transferred file to a temporary name and renames it into place, so a
# sequence that is playing keeps its old copy until it closes.

: "${FPPDIR:=/opt/fpp}"
if [ -f "${FPPDIR}/scripts/common" ]; then
    . "${FPPDIR}/scripts/common"
fi
: "${MEDIADIR:=/home/fpp/media}"
: "${LOGDIR:=${MEDIADIR}/logs}"

PRIMARY="$1"
MODE="${2:-promote}"
ARG3="$3"
ARG4="$4"

DIRS="${SYNC_DIRS:-sequences,music,videos,effects,images,scripts}"
DATADIR="${MEDIADIR}/plugindata/fpp-plugin-IlluminationToolbox-Failover"
STATUS="${SYNC_STATUS_FILE:-${DATADIR}/sync-status.json}"
PENDING="${SYNC_PENDING_FILE:-${DATADIR}/sync-pending.json}"
SNAPDIR="${SYNC_SNAPSHOT_DIR:-${DATADIR}/snapshots}"
KEEP="${SYNC_SNAPSHOTS:-5}"
LOG="${LOGDIR}/plugin-fpp-plugin-IlluminationToolbox-Failover.log"
LOCK="${DATADIR}/sync.lock"

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') sync: $*" >> "$LOG"
}

json_escape() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/ /g' | tr -d '\n\r'
}

now_str() { date '+%Y-%m-%d %H:%M:%S'; }

write_status() {
    # write_status <ok> <action> <transferred> <deleted> <error> <details-json> [extra-json]
    printf '{"ok":%s,"action":"%s","primary":"%s","startedAt":"%s","finishedAt":"%s","transferred":%d,"deleted":%d,"error":"%s","dirs":{%s}%s}\n' \
        "$1" "$2" "$(json_escape "$PRIMARY")" "$STARTED" "$(now_str)" "$3" "$4" "$(json_escape "$5")" "$6" "${7:+,$7}" > "${STATUS}.tmp"
    mv -f "${STATUS}.tmp" "$STATUS"
}

fail_early() {
    log "$1"
    STARTED="${STARTED:-$(now_str)}"
    if [ "$MODE" = "check" ]; then
        printf '{"ok":false,"error":"%s","checkedAt":"%s","count":0,"files":[]}\n' "$(json_escape "$1")" "$(now_str)" > "$PENDING"
    else
        write_status false "$MODE" 0 0 "$1" ""
    fi
    exit 2
}

if [ -z "$PRIMARY" ]; then fail_early "no primary address given"; fi
if ! command -v rsync >/dev/null 2>&1; then fail_early "rsync is not installed on this system"; fi
mkdir -p "$DATADIR" "$SNAPDIR"

exec 9>"$LOCK"
if command -v flock >/dev/null 2>&1; then
    if ! flock -n 9; then
        log "another sync operation is still running, skipping ${MODE}"
        exit 3
    fi
else
    flock() { :; } # no util-linux flock (development machine); the plugin never runs two jobs at once anyway
fi

STARTED="$(now_str)"

# SYNC_RSYNC_PORT lets a test point at a daemon on a non-standard port.
PORTARG=()
[ -n "${SYNC_RSYNC_PORT}" ] && PORTARG=(--port="${SYNC_RSYNC_PORT}")

FLAGS=(-rtDl --modify-window=1 --timeout=600 --stats)
DAEMONARGS=(--contimeout=10 "${PORTARG[@]}")   # only valid when one side is host::module
[ "${SYNC_COMPRESS:-0}" = "1" ] && FLAGS+=(-z)
[ "${SYNC_DELETE:-0}" = "1" ] && FLAGS+=(--delete)

OK=true
ERROR=""
TOTAL_XFER=0
TOTAL_DEL=0
DETAILS=""

add_detail() { DETAILS="${DETAILS}${DETAILS:+,}$1"; }

# fppd runs as root, so files this script pulls land root-owned and FPP's web
# UI can no longer replace or delete them.  Hand anything we wrote back to
# whoever owns the media directory (fpp on a normal install).
MEDIA_OWNER=""
if [ -d "$MEDIADIR" ]; then
    MEDIA_OWNER=$(stat -c '%U:%G' "$MEDIADIR" 2>/dev/null || stat -f '%Su:%Sg' "$MEDIADIR" 2>/dev/null)
fi
reown() {
    [ -z "$MEDIA_OWNER" ] && return 0
    [ "$(id -u)" = "0" ] || return 0
    case "$1" in *::*) return 0 ;; esac   # remote destination, not ours to chown
    [ -e "$1" ] && chown -R "$MEDIA_OWNER" "$1" 2>/dev/null
    return 0
}

friendly_error() {
    local msg="$1"
    case "$msg" in
        *"failed to connect"*|*"Connection refused"*|*"No route to host"*)
            echo "cannot reach the rsync daemon on ${2}; enable 'rsync' under Settings > Services there" ;;
        *"Unknown module"*|*"@ERROR"*)
            echo "rsync daemon on ${2} refused the 'media' module: ${msg}" ;;
        *) echo "$msg" ;;
    esac
}

# Enumerate the folders in scope as "<name>|<local path>|<extra excludes>"
scope_list() {
    local IFS=',' d
    read -r -a DIRLIST <<< "$DIRS"
    for d in "${DIRLIST[@]}"; do
        d="${d// /}"
        case "$d" in
            sequences|music|videos|effects|images|scripts|events|upload)
                echo "${d}|${MEDIADIR}/${d}|" ;;
            "") ;;
            *) log "ignoring unknown directory '${d}'" ;;
        esac
    done
    if [ "${SYNC_PLAYLISTS:-0}" = "1" ]; then
        echo "playlists|${MEDIADIR}/playlists|fpp-failover-hold.json"
    fi
}

# transfer <label> <src> <dst> [extra-exclude]   (directories, trailing slashes added)
transfer() {
    local label="$1" src="$2" dst="$3" extra="$4" out rc xfer del msg host
    local args=("${FLAGS[@]}" --exclude='.*' --exclude='*.tmp')
    case "${src}${dst}" in *::*) args+=("${DAEMONARGS[@]}") ;; esac
    [ -n "$extra" ] && args+=(--exclude="$extra")
    case "$dst" in *::*) ;; *) mkdir -p "$dst" ;; esac
    out=$(rsync "${args[@]}" "${src%/}/" "${dst%/}/" 2>&1)
    rc=$?
    xfer=$(printf '%s\n' "$out" | sed -n 's/^Number of regular files transferred: *\([0-9,]*\).*/\1/p' | tr -d ',' | head -1)
    del=$(printf '%s\n' "$out" | sed -n 's/^Number of deleted files: *\([0-9,]*\).*/\1/p' | tr -d ',' | head -1)
    : "${xfer:=0}" "${del:=0}"
    if [ $rc -ne 0 ]; then
        host="$PRIMARY"
        msg=$(printf '%s\n' "$out" | grep -m1 -E 'rsync|@ERROR|error' | head -c 300)
        msg=$(friendly_error "$msg" "$host")
        log "${label}: FAILED rc=${rc}: ${msg}"
        OK=false
        [ -z "$ERROR" ] && ERROR="$msg"
        add_detail "\"$(json_escape "$label")\":{\"rc\":${rc},\"transferred\":${xfer},\"deleted\":${del},\"error\":\"$(json_escape "$msg")\"}"
    else
        reown "$dst"
        log "${label}: ok, ${xfer} file(s) updated, ${del} deleted"
        add_detail "\"$(json_escape "$label")\":{\"rc\":0,\"transferred\":${xfer},\"deleted\":${del}}"
    fi
    TOTAL_XFER=$((TOTAL_XFER + xfer))
    TOTAL_DEL=$((TOTAL_DEL + del))
}

# transfer_schedule <src-file> <dst-file> [reload-host]
transfer_schedule() {
    local src="$1" dst="$2" reload="$3" before after out rc msg changed=0
    case "$dst" in *::*) before="" ;; *) mkdir -p "$(dirname "$dst")"; before=$(stat -c %Y "$dst" 2>/dev/null || echo 0) ;; esac
    local sargs=(-tD --modify-window=1 --timeout=120 --itemize-changes)
    case "${src}${dst}" in *::*) sargs+=("${DAEMONARGS[@]}") ;; esac
    out=$(rsync "${sargs[@]}" "$src" "$dst" 2>&1)
    rc=$?
    if [ $rc -ne 0 ]; then
        msg=$(friendly_error "$(printf '%s\n' "$out" | head -1 | head -c 300)" "$PRIMARY")
        log "schedule: FAILED rc=${rc}: ${msg}"
        OK=false
        [ -z "$ERROR" ] && ERROR="$msg"
        add_detail "\"schedule\":{\"rc\":${rc},\"transferred\":0,\"deleted\":0,\"error\":\"$(json_escape "$msg")\"}"
        return
    fi
    if printf '%s\n' "$out" | grep -q '^[<>]f'; then changed=1; fi
    reown "$dst"
    if [ "$changed" = "1" ] && [ -n "$reload" ]; then
        curl -s -m 5 -X POST "http://${reload}/api/schedule/reload" >/dev/null 2>&1
        log "schedule: updated on ${reload} and reloaded"
    else
        log "schedule: unchanged"
    fi
    add_detail "\"schedule\":{\"rc\":0,\"transferred\":${changed},\"deleted\":0}"
    TOTAL_XFER=$((TOTAL_XFER + changed))
}

# ---------------------------------------------------------------------------
# check: dry run from the primary into the local media folders
# ---------------------------------------------------------------------------
do_check() {
    local files="" count=0 dels=0 line name kind err="" rc out label src dst extra
    local dryargs=("${FLAGS[@]}" "${DAEMONARGS[@]}" --dry-run --itemize-changes --out-format='%i %n')
    while IFS='|' read -r label dst extra; do
        [ -z "$label" ] && continue
        mkdir -p "$dst"
        src="${PRIMARY}::media/${label}/"
        local args=("${dryargs[@]}" --exclude='.*' --exclude='*.tmp')
        [ -n "$extra" ] && args+=(--exclude="$extra")
        out=$(rsync "${args[@]}" "$src" "${dst%/}/" 2>&1)
        rc=$?
        if [ $rc -ne 0 ]; then
            err=$(friendly_error "$(printf '%s\n' "$out" | grep -m1 -E 'rsync|@ERROR|error' | head -c 300)" "$PRIMARY")
            log "check ${label}: FAILED: ${err}"
            break
        fi
        while IFS= read -r line; do
            case "$line" in
                \>f+++++++++*) kind="new" ;;
                \>f*)          kind="changed" ;;
                \*deleting*)   kind="deleted" ;;
                *)             continue ;;
            esac
            name="${line#* }"
            name="${name#"${name%%[! ]*}"}" # trim leading spaces ("*deleting   name")
            if [ "$kind" = "deleted" ]; then
                dels=$((dels + 1))
            else
                count=$((count + 1))
            fi
            files="${files}${files:+,}{\"dir\":\"$(json_escape "$label")\",\"name\":\"$(json_escape "$name")\",\"change\":\"${kind}\"}"
        done <<< "$out"
    done < <(scope_list)

    if [ -z "$err" ] && [ "${SYNC_SCHEDULE:-0}" = "1" ]; then
        mkdir -p "${MEDIADIR}/config"
        out=$(rsync -tD --modify-window=1 --dry-run --itemize-changes --out-format='%i %n' "${DAEMONARGS[@]}" "${PRIMARY}::media/config/schedule.json" "${MEDIADIR}/config/schedule.json" 2>&1)
        rc=$?
        if [ $rc -ne 0 ]; then
            err=$(friendly_error "$(printf '%s\n' "$out" | head -1 | head -c 300)" "$PRIMARY")
        elif printf '%s\n' "$out" | grep -q '^>f'; then
            count=$((count + 1))
            files="${files}${files:+,}{\"dir\":\"config\",\"name\":\"schedule.json\",\"change\":\"changed\"}"
        fi
    fi

    local fp
    if command -v md5sum >/dev/null 2>&1; then
        fp=$(printf '%s' "$files" | md5sum | cut -c1-16)
    else
        fp=$(printf '%s' "$files" | md5 | cut -c1-16)
    fi
    if [ -n "$err" ]; then
        printf '{"ok":false,"error":"%s","checkedAt":"%s","count":0,"deletions":0,"fingerprint":"","files":[]}\n' \
            "$(json_escape "$err")" "$(now_str)" > "${PENDING}.tmp"
        mv -f "${PENDING}.tmp" "$PENDING"
        exit 1
    fi
    printf '{"ok":true,"error":"","checkedAt":"%s","count":%d,"deletions":%d,"fingerprint":"%s","files":[%s]}\n' \
        "$(now_str)" "$count" "$dels" "$fp" "$files" > "${PENDING}.tmp"
    mv -f "${PENDING}.tmp" "$PENDING"
    log "check: ${count} file(s) new or changed on ${PRIMARY}, ${dels} would be deleted"
    exit 0
}

# ---------------------------------------------------------------------------
# snapshots
# ---------------------------------------------------------------------------
# snapshot_local <id> <reason>  - hard-link copy of the current local stable set
snapshot_local() {
    local id="$1" reason="$2" label dst extra n total=0 out rc
    [ "$KEEP" -le 0 ] 2>/dev/null && return 0
    local dir="${SNAPDIR}/${id}"
    mkdir -p "$dir"
    while IFS='|' read -r label dst extra; do
        [ -z "$label" ] && continue
        [ -d "$dst" ] || continue
        mkdir -p "${dir}/${label}"
        local args=(-rtDl --link-dest="${dst%/}/" --exclude='.*' --exclude='*.tmp')
        [ -n "$extra" ] && args+=(--exclude="$extra")
        out=$(rsync "${args[@]}" "${dst%/}/" "${dir}/${label}/" 2>&1)
        rc=$?
        if [ $rc -ne 0 ]; then
            log "snapshot ${id}/${label}: FAILED: $(printf '%s\n' "$out" | head -1)"
            OK=false
            [ -z "$ERROR" ] && ERROR="snapshot failed: $(printf '%s\n' "$out" | head -1 | head -c 200)"
        fi
        n=$(find "${dir}/${label}" -type f | wc -l | tr -d ' ')
        total=$((total + n))
    done < <(scope_list)
    if [ "${SYNC_SCHEDULE:-0}" = "1" ] && [ -f "${MEDIADIR}/config/schedule.json" ]; then
        mkdir -p "${dir}/config"
        cp -p "${MEDIADIR}/config/schedule.json" "${dir}/config/schedule.json"
        total=$((total + 1))
    fi
    printf '{"id":"%s","createdAt":"%s","reason":"%s","source":"local","primary":"%s","files":%d,"dirs":"%s"}\n' \
        "$id" "$(now_str)" "$(json_escape "$reason")" "$(json_escape "$PRIMARY")" "$total" "$(json_escape "$DIRS")" > "${dir}/manifest.json"
    log "snapshot ${id}: ${total} file(s) (${reason})"
}

# snapshot_primary <id> <reason> - copy of the primary's current content,
# hard-linked against the local stable set where files are identical.
snapshot_primary() {
    local id="$1" reason="$2" label dst extra n total=0 out rc
    local dir="${SNAPDIR}/${id}"
    mkdir -p "$dir"
    while IFS='|' read -r label dst extra; do
        [ -z "$label" ] && continue
        mkdir -p "${dir}/${label}"
        local args=(-rtDl --timeout=600 --exclude='.*' --exclude='*.tmp' "${DAEMONARGS[@]}")
        [ -d "$dst" ] && args+=(--link-dest="${dst%/}/")
        [ -n "$extra" ] && args+=(--exclude="$extra")
        [ "${SYNC_COMPRESS:-0}" = "1" ] && args+=(-z)
        out=$(rsync "${args[@]}" "${PRIMARY}::media/${label}/" "${dir}/${label}/" 2>&1)
        rc=$?
        if [ $rc -ne 0 ]; then
            local msg
            msg=$(friendly_error "$(printf '%s\n' "$out" | grep -m1 -E 'rsync|@ERROR|error' | head -c 300)" "$PRIMARY")
            log "snapshot ${id}/${label} from primary: FAILED: ${msg}"
            OK=false
            [ -z "$ERROR" ] && ERROR="could not snapshot the primary: ${msg}"
            return 1
        fi
        n=$(find "${dir}/${label}" -type f | wc -l | tr -d ' ')
        total=$((total + n))
    done < <(scope_list)
    if [ "${SYNC_SCHEDULE:-0}" = "1" ]; then
        mkdir -p "${dir}/config"
        rsync -tD "${DAEMONARGS[@]}" "${PRIMARY}::media/config/schedule.json" "${dir}/config/schedule.json" >/dev/null 2>&1 && total=$((total + 1))
    fi
    printf '{"id":"%s","createdAt":"%s","reason":"%s","source":"primary","primary":"%s","files":%d,"dirs":"%s"}\n' \
        "$id" "$(now_str)" "$(json_escape "$reason")" "$(json_escape "$PRIMARY")" "$total" "$(json_escape "$DIRS")" > "${dir}/manifest.json"
    log "snapshot ${id}: ${total} file(s) from the primary (${reason})"
    return 0
}

# new_snapshot_id [suffix] - timestamp id, made unique if one already exists
new_snapshot_id() {
    local base id n=1
    base="$(date '+%Y%m%d-%H%M%S')"
    id="${base}${1}"
    while [ -e "${SNAPDIR}/${id}" ]; do
        n=$((n + 1))
        id="${base}.${n}${1}"
    done
    echo "$id"
}

prune_snapshots() {
    [ "$KEEP" -le 0 ] 2>/dev/null && return 0
    local ids
    ids=$(ls -1 "$SNAPDIR" 2>/dev/null | grep -E '^[0-9]{8}-[0-9]{6}' | sort -V)
    local n
    n=$(printf '%s\n' "$ids" | grep -c . )
    while [ "$n" -gt "$KEEP" ]; do
        local oldest
        oldest=$(printf '%s\n' "$ids" | head -1)
        [ -z "$oldest" ] && break
        rm -rf "${SNAPDIR:?}/${oldest}"
        log "pruned snapshot ${oldest}"
        ids=$(printf '%s\n' "$ids" | tail -n +2)
        n=$((n - 1))
    done
}

# ---------------------------------------------------------------------------
# promote: snapshot stable, then pull the primary's content over it
# ---------------------------------------------------------------------------
do_promote() {
    local label dst extra
    # Nothing to do?  Then no snapshot either, or history fills with no-ops.
    # The check runs as a child that needs the lock, so hand it over briefly.
    local pend dels
    flock -u 9
    if SYNC_PENDING_FILE="${PENDING}" bash "$0" "$PRIMARY" check >/dev/null 2>&1; then
        pend=$(sed -n 's/.*"count":\([0-9]*\).*/\1/p' "$PENDING" 2>/dev/null)
        dels=$(sed -n 's/.*"deletions":\([0-9]*\).*/\1/p' "$PENDING" 2>/dev/null)
        if [ "${pend:-0}" = "0" ] && [ "${dels:-0}" = "0" ]; then
            log "promote: nothing new on ${PRIMARY}"
            write_status true promote 0 0 "" "" "\"nothingToDo\":true"
            exit 0
        fi
    else
        # The primary cannot be read: stop before touching snapshots, or a
        # dead primary would leave an empty rollback point behind and push a
        # real one out of the history.
        ERROR=$(sed -n 's/.*"error":"\([^"]*\)".*/\1/p' "$PENDING" 2>/dev/null)
        [ -z "$ERROR" ] && ERROR="could not read the primary's content"
        log "promote: aborted, ${ERROR}"
        write_status false promote 0 0 "$ERROR" ""
        exit 1
    fi
    flock -n 9 || { log "promote: lock busy"; exit 3; }

    local id
    id="$(new_snapshot_id)"
    snapshot_local "$id" "before promote"

    log "promote: pulling from ${PRIMARY}"
    while IFS='|' read -r label dst extra; do
        [ -z "$label" ] && continue
        transfer "$label" "${PRIMARY}::media/${label}" "$dst" "$extra"
    done < <(scope_list)
    if [ "${SYNC_SCHEDULE:-0}" = "1" ]; then
        transfer_schedule "${PRIMARY}::media/config/schedule.json" "${MEDIADIR}/config/schedule.json" "127.0.0.1"
    fi
    [ "$OK" = "true" ] && prune_snapshots
    # The pending list is now stale: rewrite it as empty.
    printf '{"ok":true,"error":"","checkedAt":"%s","count":0,"deletions":0,"fingerprint":"","files":[]}\n' "$(now_str)" > "$PENDING"
    write_status "$OK" promote "$TOTAL_XFER" "$TOTAL_DEL" "$ERROR" "$DETAILS" "\"snapshot\":\"${id}\""
    if [ "$OK" = "true" ]; then
        log "promote finished: ${TOTAL_XFER} file(s) updated, ${TOTAL_DEL} deleted (rollback point ${id})"
        exit 0
    fi
    log "promote finished with errors: ${ERROR}"
    exit 1
}

# ---------------------------------------------------------------------------
# rollback <id|latest> <local|primary|both>
# ---------------------------------------------------------------------------
do_rollback() {
    local id="$1" target="$2" label dst extra
    if [ "$id" = "latest" ] || [ -z "$id" ]; then
        id=$(ls -1 "$SNAPDIR" 2>/dev/null | grep -E '^[0-9]{8}-[0-9]{6}' | grep -v -- '-pre-rollback' | sort | tail -1)
    fi
    local dir="${SNAPDIR}/${id}"
    if [ -z "$id" ] || [ ! -d "$dir" ]; then
        ERROR="snapshot '${id}' does not exist"
        write_status false rollback 0 0 "$ERROR" ""
        log "rollback: ${ERROR}"
        exit 1
    fi
    case "$target" in local|primary|both) ;; *) target="both" ;; esac
    log "rollback to ${id} on ${target}"

    if [ "$target" = "primary" ] || [ "$target" = "both" ]; then
        # Keep what the primary has now, so this rollback can be undone.
        local pre
        pre="$(new_snapshot_id -pre-rollback)"
        if ! snapshot_primary "$pre" "primary before rollback to ${id}"; then
            write_status false rollback 0 0 "$ERROR" "$DETAILS"
            exit 1
        fi
        while IFS='|' read -r label dst extra; do
            [ -z "$label" ] && continue
            [ -d "${dir}/${label}" ] || continue
            transfer "primary:${label}" "${dir}/${label}" "${PRIMARY}::media/${label}" "$extra"
        done < <(scope_list)
        if [ "${SYNC_SCHEDULE:-0}" = "1" ] && [ -f "${dir}/config/schedule.json" ]; then
            transfer_schedule "${dir}/config/schedule.json" "${PRIMARY}::media/config/schedule.json" "$PRIMARY"
        fi
    fi
    if [ "$target" = "local" ] || [ "$target" = "both" ]; then
        while IFS='|' read -r label dst extra; do
            [ -z "$label" ] && continue
            [ -d "${dir}/${label}" ] || continue
            transfer "local:${label}" "${dir}/${label}" "$dst" "$extra"
        done < <(scope_list)
        if [ "${SYNC_SCHEDULE:-0}" = "1" ] && [ -f "${dir}/config/schedule.json" ]; then
            transfer_schedule "${dir}/config/schedule.json" "${MEDIADIR}/config/schedule.json" "127.0.0.1"
        fi
    fi
    [ "$OK" = "true" ] && prune_snapshots
    write_status "$OK" rollback "$TOTAL_XFER" "$TOTAL_DEL" "$ERROR" "$DETAILS" "\"snapshot\":\"${id}\",\"target\":\"${target}\""
    if [ "$OK" = "true" ]; then
        log "rollback to ${id} on ${target} finished: ${TOTAL_XFER} file(s) restored"
        exit 0
    fi
    log "rollback finished with errors: ${ERROR}"
    exit 1
}

case "$MODE" in
    check)    do_check ;;
    promote)  do_promote ;;
    rollback) do_rollback "$ARG3" "$ARG4" ;;
    *)        fail_early "unknown mode '${MODE}'" ;;
esac
