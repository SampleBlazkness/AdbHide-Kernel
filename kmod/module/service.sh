#!/system/bin/sh
# Record whether the module actually loaded, then resolve the package
# blacklist to uids and install the netfilter rules (sync.sh does both).

MODDIR=${0%/*}
LOG=/data/adb/adbhide/kmod.log

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

sh "$MODDIR/sync.sh"

# Wireless debugging picks a new random port on every toggle. Re-apply the
# netfilter rules when it changes so the REJECT follows it.
(
    LAST=""
    while true; do
        NOW=$(ss -ltnp 2>/dev/null | awk '$1 == "LISTEN" && /adbd/ {
            n = split($4, a, ":"); print a[n]; exit }')
        if [ -n "$NOW" ] && [ "$NOW" != "$LAST" ]; then
            LAST="$NOW"
            echo "$(date) portwatch: adbd port $NOW" >>"$LOG"
            sh "$MODDIR/rules.sh"
        fi
        sleep 10
    done
) &
