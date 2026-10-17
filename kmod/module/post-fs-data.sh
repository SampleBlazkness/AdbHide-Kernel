#!/system/bin/sh
# Load adbhide_kmod.ko as early as KernelSU allows.
# ksud insmod gives the module kallsyms access; plain insmod is the fallback.

MODDIR=${0%/*}
KO="$MODDIR/adbhide_kmod.ko"
LOG=/data/adb/adbhide/kmod.log

mkdir -p /data/adb/adbhide

[ -f "$KO" ] || {
    echo "$(date) ko missing: $KO" >>"$LOG"
    exit 0
}

if [ -x /data/adb/ksud ]; then
    /data/adb/ksud insmod "$KO" >>"$LOG" 2>&1 && exit 0
fi

insmod "$KO" >>"$LOG" 2>&1
