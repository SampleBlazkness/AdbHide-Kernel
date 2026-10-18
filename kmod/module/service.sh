#!/system/bin/sh
# Record whether the module actually loaded; useful when a build fails silently.
# Installs the per-uid port rules (M5): target apps must not see the adbd
# endpoint or resolve its mDNS service, while the phone itself keeps wireless
# debugging working for everyone else.
#
# xt_string and xt_owner are loaded lazily by the kernel, and at boot time the
# first attempt can fail before those modules are available, so the rules are
# applied in a retry loop until the counters add up.
#
# The status snapshot at the end is what the app reads when KernelSU does not
# allow it to run `su` (this ROM has no manual "add app" entry in the manager).

MODDIR=${0%/*}
LOG=/data/adb/adbhide/kmod.log
UIDS=/data/adb/adbhide/uids.txt

mkdir -p /data/adb/adbhide
{
    echo "--- $(date '+%Y-%m-%d %H:%M:%S') ---"
    if [ -r /proc/adbhide_kmod ]; then
        cat /proc/adbhide_kmod
    else
        echo "not loaded"
        lsmod | grep -i adbhide || true
    fi
} >>"$LOG"

[ -r "$UIDS" ] || { echo "$(date) ports: no uid file" >>"$LOG"; exit 0; }

PORT=$(getprop service.adb.tcp.port 2>/dev/null)
case "$PORT" in ''|*[!0-9]*) PORT=5555;; esac
WANT=$(( 2 * $(grep -c . "$UIDS") ))

apply_family() {
    _cmd="$1"
    $_cmd -N ADBHIDE_PORTS 2>/dev/null
    $_cmd -F ADBHIDE_PORTS 2>/dev/null || return 0
    while read -r u; do
        case "$u" in ''|*[!0-9]*) continue;; esac
        # adbd endpoint: reset like a closed port
        $_cmd -A ADBHIDE_PORTS -m owner --uid-owner "$u" -p tcp --dport "$PORT" \
              -j REJECT --reject-with tcp-reset 2>>"$LOG"
        # mDNS queries on the direct-socket path
        $_cmd -A ADBHIDE_PORTS -m owner --uid-owner "$u" -p udp --dport 5353 \
              -j DROP 2>>"$LOG"
    done <"$UIDS"
    $_cmd -C OUTPUT -j ADBHIDE_PORTS 2>/dev/null || $_cmd -I OUTPUT 1 -j ADBHIDE_PORTS
}

block_adb_mdns() {
    _cmd="$1"
    $_cmd -N ADBHIDE_MDNS_G 2>/dev/null
    $_cmd -F ADBHIDE_MDNS_G 2>/dev/null || return 0
    if ! $_cmd -A ADBHIDE_MDNS_G -p udp --sport 5353 -m string --string "_adb" \
          --algo bm -j DROP 2>>"$LOG"; then
        return 0
    fi
    $_cmd -C OUTPUT -j ADBHIDE_MDNS_G 2>/dev/null || $_cmd -I OUTPUT 1 -j ADBHIDE_MDNS_G
}

apply_all() {
    apply_family iptables
    apply_family ip6tables
    block_adb_mdns iptables
    block_adb_mdns ip6tables
}

i=0
while [ $i -lt 6 ]; do
    apply_all
    n4=$(iptables -S ADBHIDE_PORTS 2>/dev/null | grep -c uid-owner)
    n6=$(ip6tables -S ADBHIDE_PORTS 2>/dev/null | grep -c uid-owner)
    m4=$(iptables -S ADBHIDE_MDNS_G 2>/dev/null | grep -c string)
    m6=$(ip6tables -S ADBHIDE_MDNS_G 2>/dev/null | grep -c string)
    if [ "$n4" -ge "$WANT" ] && [ "$n6" -ge "$WANT" ] && [ "$m4" -ge 1 ]; then
        break
    fi
    i=$((i + 1))
    sleep 5
done

echo "$(date) ports: adbd=$PORT want=$WANT v4=$n4 v6=$n6 mdns=$m4 mdns6=$m6" >>"$LOG"

# Snapshot for the status app (it may not be allowed to run su).
APP_PKG=com.blazkness.adbhide
SNAP_DIR=/sdcard/Android/data/$APP_PKG/files
SNAP="$SNAP_DIR/status.txt"
APP_UID=$(stat -c %u /data/data/$APP_PKG 2>/dev/null)
mkdir -p "$SNAP_DIR" 2>/dev/null
# root-created dirs are owned by root; the app cannot enter them, so hand the
# tree to the app uid before writing the snapshot
[ -n "$APP_UID" ] && chown -R "$APP_UID:$APP_UID" "/sdcard/Android/data/$APP_PKG" 2>/dev/null
{
    echo "SNAP=1"
    echo "STAMP=$(date '+%Y-%m-%d %H:%M:%S')"
    if [ -r /proc/adbhide_kmod ]; then
        P=/proc/adbhide_kmod
        echo "KMOD=1"
        echo "KVER=$(head -n1 $P | cut -d' ' -f2)"
        echo "KREDIR=$(sed -n 's/^redirect //p' $P)"
        echo "KNETSEQ=$(sed -n 's/^netseq //p' $P)"
        echo "KCOUNTERS=$(sed -n 's/^counters //p' $P)"
        echo "KTARGETS=$(sed -n 's/^targets [0-9]*://p' $P)"
    else
        echo "KMOD=0"
    fi
    echo "UIDS=$(tr '\n' ' ' < $UIDS)"
    echo "PORTS=$n4"
    echo "PORTS6=$n6"
    echo "MDNS=$m4"
    echo "MDNS6=$m6"
    echo "NATIVE=$([ -f /data/adb/modules/adbhide_native/zygisk/arm64-v8a.so ] && echo 1 || echo 0)"
    echo "NATIVEVER=$(sed -n 's/^version=//p' /data/adb/modules/adbhide_native/module.prop 2>/dev/null)"
} > "$SNAP" 2>/dev/null
chmod 0644 "$SNAP" 2>/dev/null
[ -n "$APP_UID" ] && chown "$APP_UID:$APP_UID" "$SNAP" 2>/dev/null
