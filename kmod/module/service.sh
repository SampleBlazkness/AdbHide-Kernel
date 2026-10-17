#!/system/bin/sh
# Record whether the module actually loaded; useful when a build fails silently.

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
