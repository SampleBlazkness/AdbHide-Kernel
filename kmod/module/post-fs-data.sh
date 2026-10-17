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

mk_req() {
    # build "target_uids=1,2,3" from the uid file; the kernel never reads files
    _f=/data/adb/adbhide/uids.txt
    _list=""
    [ -r "$_f" ] || { echo "$(date) no uid file: $_f" >>"$LOG"; return 1; }
    while read -r _u; do
        case "$_u" in ''|*[!0-9]*) continue;; esac
        _list="${_list:+$_list,}$_u"
    done <"$_f"
    [ -n "$_list" ] || { echo "$(date) uid file empty" >>"$LOG"; return 1; }
    echo "target_uids=$_list"
}

REQ=$(mk_req)
echo "$(date) loading ko with: $REQ" >>"$LOG"

if [ -x /data/adb/ksud ]; then
    /data/adb/ksud insmod "$KO" $REQ >>"$LOG" 2>&1 && exit 0
fi

insmod "$KO" $REQ >>"$LOG" 2>&1
