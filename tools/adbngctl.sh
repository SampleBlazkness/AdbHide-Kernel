#!/system/bin/sh
# adbng - SPDX-License-Identifier: MIT
#
# adbngctl: the state machine that owns the gadget and usbgwd.
#
#   start            take USB over from adbd and run usbgwd (checks first)
#   stop             stop usbgwd and unlink the function; adbd stays stopped
#   status           key=value dump for the manager app
#   ensure           boot path: start only if /data/adb/adbng/enabled says so
#   allow <fp>       answer a pairing request with "allow"
#   deny <fp>        answer a pairing request with "deny"
#   pending          list the parked host, if any
#   devices          the decision log (ts, fp, decision, host)
#
# Design decisions baked in here:
#   * start is refuse-not-fight: every adb is stopped first and the state is
#     then re-checked.  If that fails we report "adbng unavailable, use
#     adbhide" instead of fighting adbd for the UDC.
#   * stop never restarts adbd.  The user asked for adbng, not for a fallback.
#   * the watchdog retakes the UDC when adbd comes back (user toggled USB
#     debugging, or init restarted it).  After three failed rounds it gives up
#     and stops adbng rather than making the port flap forever.
#
STATE=${ADBNG_STATE_DIR:-/data/adb/adbng}
BIN=${ADBNG_BIN:-$STATE/usbgwd}
GADGET_SH=${ADBNG_GADGET_SH:-$STATE/usb-gadget.sh}
FFS=${ADBNG_FFS_DIR:-/dev/usb-ffs/adbng}
G=${ADBNG_GADGET:-/config/usb_gadget/g1}
CFG=${ADBNG_CFG:-b.1}
SLOT=${ADBNG_SLOT:-function9}
UDC=${ADBNG_UDC:-$(cat "$G/UDC" 2>/dev/null)}
[ -n "$UDC" ] || UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)

KEYS=$STATE/adb_keys
ENABLED=$STATE/enabled
LOG=$STATE/adbng.log
PIDF=$STATE/usbgwd.pid
WDPID=$STATE/watchdog.pid
PDIR=$STATE/pending
TAKEOVER_FAILS=$STATE/takeover.fails
MAX_TAKEOVER_FAILS=3

log() {
    mkdir -p "$STATE" 2>/dev/null
    echo "[$(date '+%F %T')] $*" >>"$LOG"
}

say() {
    echo "$*"
    log "$*"
}

usbgwd_pid() {
    p=$(cat "$PIDF" 2>/dev/null)
    if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then
        echo "$p"
        return 0
    fi
    p=$(pgrep -f "usbgwd .*--ffs" 2>/dev/null | head -n 1)
    [ -n "$p" ] && echo "$p"
}

adbd_running() {
    [ -n "$(pidof adbd 2>/dev/null)" ]
}

# adb's function is not always function0 (this device puts mtp there), so look
# at every link in the active config instead of guessing a slot number.
system_adb_linked() {
    for l in "$G/configs/$CFG"/function*; do
        [ -L "$l" ] || continue
        t=$(readlink "$l" 2>/dev/null)
        [ "${t##*/}" = "ffs.adb" ] && return 0
    done
    return 1
}

# 0 = adbd is really gone and its function is no longer in the config
takeover_possible() {
    adbd_running && return 1
    system_adb_linked && return 1
    [ -n "$UDC" ] || return 1
    [ -d "$G" ] || return 1
    return 0
}

stop_adbd() {
    adbd_running || return 0
    log "stopping adbd (pids: $(pidof adbd 2>/dev/null | tr ' ' ','))"
    ctl.stop adbd >/dev/null 2>&1 || stop adbd >/dev/null 2>&1
    i=0
    while [ $i -lt 40 ]; do
        adbd_running || return 0
        sleep 0.1
        i=$((i + 1))
    done
    log "adbd still running after ctl.stop"
    return 1
}

stop_watchdog() {
    p=$(cat "$WDPID" 2>/dev/null)
    [ -n "$p" ] && kill "$p" 2>/dev/null
    rm -f "$WDPID"
    p=$(pgrep -f "adbngctl.sh watchdog" 2>/dev/null)
    [ -n "$p" ] && kill $p 2>/dev/null
    return 0
}

kill_usbgwd() {
    p=$(usbgwd_pid)
    [ -n "$p" ] || { rm -f "$PIDF"; return 0; }
    log "stopping usbgwd pid $p"
    kill "$p" 2>/dev/null
    i=0
    while [ $i -lt 30 ] && kill -0 "$p" 2>/dev/null; do
        sleep 0.1
        i=$((i + 1))
    done
    kill -9 "$p" 2>/dev/null
    rm -f "$PIDF"
    return 0
}

gadget_ready() {
    [ -f "$GADGET_SH" ] || { say "missing $GADGET_SH"; return 1; }
    [ -x "$BIN" ] || { say "missing $BIN"; return 1; }
    return 0
}

start_daemon() {
    mkdir -p "$STATE"
    [ -f "$KEYS" ] || : >"$KEYS"
    chmod 600 "$KEYS" 2>/dev/null
    nohup "$BIN" --ffs "$FFS" --keys "$KEYS" >>"$LOG" 2>&1 &
    echo $! >"$PIDF"
    i=0
    while [ $i -lt 20 ]; do
        kill -0 "$(cat "$PIDF")" 2>/dev/null || return 1
        i=$((i + 1))
        sleep 0.05
    done
    return 0
}

link_function() {
    sh "$GADGET_SH" install >>"$LOG" 2>&1 || return 1
    # the daemon has to be up first: descriptors and the strings table are
    # written on ep0, and configfs will not bind an unprovisioned function
    if [ -z "$(usbgwd_pid)" ]; then
        start_daemon || return 1
    fi
    if ! sh "$GADGET_SH" add >>"$LOG" 2>&1; then
        kill_usbgwd
        return 1
    fi
    return 0
}

# Watchdog: keeps usbgwd alive and takes the UDC back when adbd reappears.
watchdog_loop() {
    log "watchdog start (parent $$)"
    while :; do
        [ "$(cat "$ENABLED" 2>/dev/null)" = "1" ] || { log "watchdog: disabled, exit"; break; }
        sleep 5
        if [ -z "$(usbgwd_pid)" ]; then
            log "watchdog: usbgwd gone, restarting"
            start_daemon || log "watchdog: restart failed"
        fi
        if adbd_running; then
            log "watchdog: adbd came back"
            if stop_adbd; then
                sh "$GADGET_SH" del >>"$LOG" 2>&1
                if link_function; then
                    log "watchdog: UDC retaken"
                    rm -f "$TAKEOVER_FAILS"
                else
                    n=$(( $(cat "$TAKEOVER_FAILS" 2>/dev/null || echo 0) + 1 ))
                    echo "$n" >"$TAKEOVER_FAILS"
                    log "watchdog: retake failed ($n/$MAX_TAKEOVER_FAILS)"
                fi
            else
                n=$(( $(cat "$TAKEOVER_FAILS" 2>/dev/null || echo 0) + 1 ))
                echo "$n" >"$TAKEOVER_FAILS"
                log "watchdog: could not stop adbd ($n/$MAX_TAKEOVER_FAILS)"
            fi
            if [ "$(cat "$TAKEOVER_FAILS" 2>/dev/null || echo 0)" -ge "$MAX_TAKEOVER_FAILS" ]; then
                log "watchdog: giving up, adbng disabled"
                echo 0 >"$ENABLED"
                kill_usbgwd
                sh "$GADGET_SH" del >>"$LOG" 2>&1
                sh "$GADGET_SH" remove >>"$LOG" 2>&1
                cmd notification post -S bigtext -t "adbng" adbng \
                    "adbng 已停止：系统 adb 反复抢占 USB，请改用 adbhide" >/dev/null 2>&1
                break
            fi
        fi
    done
    log "watchdog exit"
}

cmd_start() {
    gadget_ready || return 1
    if ! stop_adbd; then
        say "adbng 不可用：无法关闭系统 adb（adbd 仍在运行），请使用 adbhide"
        return 1
    fi
    if ! takeover_possible; then
        say "adbng 不可用：系统 adb 仍占用 USB 配置，请使用 adbhide"
        return 1
    fi
    kill_usbgwd
    if ! link_function; then
        say "adbng 启动失败：无法接管 USB（详见 $LOG）"
        return 1
    fi
    echo 1 >"$ENABLED"
    rm -f "$TAKEOVER_FAILS"
    say "adbng 已启动（usbgwd pid $(usbgwd_pid || echo '?'), UDC $UDC）"
    stop_watchdog
    ( watchdog_loop >>"$LOG" 2>&1 ) &
    echo $! >"$WDPID"
    return 0
}

cmd_stop() {
    echo 0 >"$ENABLED" 2>/dev/null
    stop_watchdog
    kill_usbgwd
    if [ -f "$GADGET_SH" ]; then
        sh "$GADGET_SH" del >>"$LOG" 2>&1
    fi
    # adbd is deliberately NOT restarted: closing adbng means USB debugging is
    # off until the user turns the system adb back on.
    say "adbng 已停止（系统 adb 未恢复）"
    return 0
}

cmd_status() {
    st=stopped
    [ -n "$(usbgwd_pid)" ] && st=running
    echo "state=$st"
    echo "enabled=$(cat "$ENABLED" 2>/dev/null || echo 0)"
    echo "usbgwd_pid=$(usbgwd_pid | head -n 1)"
    if adbd_running; then echo "adbd=running"; else echo "adbd=stopped"; fi
    if adbd_running || system_adb_linked; then
        echo "system_adb=linked"
    else
        echo "system_adb=absent"
    fi
    l=$(readlink "$G/configs/$CFG/$SLOT" 2>/dev/null)
    if [ -n "$l" ]; then echo "adbng_link=present"; else echo "adbng_link=absent"; fi
    echo "udc=${UDC:-none}"
    echo "udc_state=$(cat "/sys/class/udc/$UDC/state" 2>/dev/null)"
    echo "keys=$(grep -c . "$KEYS" 2>/dev/null || echo 0)"
    echo "pending=$(ls "$PDIR" 2>/dev/null | grep '\.req$' | sed 's/\.req$//' | head -n 1)"
    echo "takeover_fails=$(cat "$TAKEOVER_FAILS" 2>/dev/null || echo 0)"
    return 0
}

cmd_pending() {
    found=0
    for f in "$PDIR"/*.req; do
        [ -f "$f" ] || continue
        found=1
        echo "--- ${f##*/}"
        cat "$f"
    done
    [ "$found" = 1 ] || echo "none"
    return 0
}

cmd_decide() {
    fp=$1
    verdict=$2
    [ -n "$fp" ] || { say "usage: adbngctl allow|deny <fingerprint>"; return 2; }
    if [ ! -f "$PDIR/$fp.req" ]; then
        say "没有待处理的请求：$fp"
        return 1
    fi
    mkdir -p "$PDIR"
    echo "$verdict" >"$PDIR/$fp.decision"
    say "已写决策 $verdict -> $fp"
    return 0
}

case "$1" in
start)   cmd_start ;;
stop)    cmd_stop ;;
status)  cmd_status ;;
ensure)
    if [ "$(cat "$ENABLED" 2>/dev/null)" = "1" ]; then
        log "ensure: enabled, starting"
        cmd_start
    else
        log "ensure: not enabled, nothing to do"
    fi
    ;;
watchdog) watchdog_loop ;;
allow)   cmd_decide "$2" allow allow ;;
deny)    cmd_decide "$2" deny deny ;;
pending) cmd_pending ;;
devices) cat "$STATE/devices.tsv" 2>/dev/null || echo "none" ;;
*)
    echo "usage: $0 {start|stop|status|ensure|allow <fp>|deny <fp>|pending|devices}" >&2
    exit 2
    ;;
esac
