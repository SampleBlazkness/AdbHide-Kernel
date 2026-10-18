#!/system/bin/sh
# Record whether the module actually loaded; useful when a build fails silently.
# Also installs the per-uid port rules (M5): target apps must not see the adbd
# endpoint or resolve its mDNS service, while the phone itself keeps wireless
# debugging working for everyone else.

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

apply_family() {
    _cmd="$1"
    $_cmd -N ADBHIDE_PORTS 2>/dev/null
    $_cmd -F ADBHIDE_PORTS 2>/dev/null || return 0
    while read -r u; do
        case "$u" in ''|*[!0-9]*) continue;; esac
        # adbd endpoint: reset like a closed port
        $_cmd -A ADBHIDE_PORTS -m owner --uid-owner "$u" -p tcp --dport "$PORT" \
              -j REJECT --reject-with tcp-reset 2>/dev/null
        # mDNS queries on the direct-socket path
        $_cmd -A ADBHIDE_PORTS -m owner --uid-owner "$u" -p udp --dport 5353 \
              -j DROP 2>/dev/null
    done <"$UIDS"
    $_cmd -C OUTPUT -j ADBHIDE_PORTS 2>/dev/null || $_cmd -I OUTPUT 1 -j ADBHIDE_PORTS
}

apply_family iptables
apply_family ip6tables

# mDNS: mdnsd (uid 1020) answers for adbd, so no per-uid rule can hide the
# record from one app only. Drop responses that carry an _adb service name;
# manual `adb connect ip:port` keeps working, auto-discovery does not.
block_adb_mdns() {
    _cmd="$1"
    $_cmd -N ADBHIDE_MDNS_G 2>/dev/null
    $_cmd -F ADBHIDE_MDNS_G 2>/dev/null || return 0
    $_cmd -A ADBHIDE_MDNS_G -p udp --sport 5353 -m string --string "_adb" \
          --algo bm -j DROP 2>/dev/null || return 0
    $_cmd -C OUTPUT -j ADBHIDE_MDNS_G 2>/dev/null || $_cmd -I OUTPUT 1 -j ADBHIDE_MDNS_G
}

block_adb_mdns iptables
block_adb_mdns ip6tables
echo "$(date) ports: adbd=$PORT rules=$(iptables -S ADBHIDE_PORTS 2>/dev/null | grep -c uid-owner) mdns=$(iptables -S ADBHIDE_MDNS_G 2>/dev/null | grep -c string)" >>"$LOG"
