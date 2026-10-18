#!/system/bin/sh
# Netfilter rules for the current uid list: adbd port reset for target uids,
# direct mDNS queries dropped, and mdnsd replies carrying an _adb record
# dropped globally. xt_string/xt_owner load lazily, so the rules are verified
# and retried until the counters add up.

MODDIR=${0%/*}
LOG=/data/adb/adbhide/kmod.log
UIDS=/data/adb/adbhide/uids.txt

[ -r "$UIDS" ] || { echo "$(date) rules: no uid file" >>"$LOG"; exit 0; }

PORT=$(getprop service.adb.tcp.port 2>/dev/null)
case "$PORT" in ''|*[!0-9]*) PORT=5555;; esac
WANT=$(( 2 * $(grep -c . "$UIDS") ))

apply_family() {
    _cmd="$1"
    $_cmd -N ADBHIDE_PORTS 2>/dev/null
    $_cmd -F ADBHIDE_PORTS 2>/dev/null || return 0
    while read -r u; do
        case "$u" in ''|*[!0-9]*) continue;; esac
        $_cmd -A ADBHIDE_PORTS -m owner --uid-owner "$u" -p tcp --dport "$PORT" \
              -j REJECT --reject-with tcp-reset 2>>"$LOG"
        $_cmd -A ADBHIDE_PORTS -m owner --uid-owner "$u" -p udp --dport 5353 \
              -j DROP 2>>"$LOG"
    done <"$UIDS"
    $_cmd -C OUTPUT -j ADBHIDE_PORTS 2>/dev/null || $_cmd -I OUTPUT 1 -j ADBHIDE_PORTS
}

block_adb_mdns() {
    _cmd="$1"
    $_cmd -N ADBHIDE_MDNS_G 2>/dev/null
    $_cmd -F ADBHIDE_MDNS_G 2>/dev/null || return 0
    $_cmd -A ADBHIDE_MDNS_G -p udp --sport 5353 -m string --string "_adb" \
          --algo bm -j DROP 2>>"$LOG" || return 0
    $_cmd -C OUTPUT -j ADBHIDE_MDNS_G 2>/dev/null || $_cmd -I OUTPUT 1 -j ADBHIDE_MDNS_G
}

i=0
while [ $i -lt 6 ]; do
    apply_family iptables
    apply_family ip6tables
    block_adb_mdns iptables
    block_adb_mdns ip6tables
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

echo "$(date) rules: adbd=$PORT want=$WANT v4=$n4 v6=$n6 mdns=$m4 mdns6=$m6" >>"$LOG"
