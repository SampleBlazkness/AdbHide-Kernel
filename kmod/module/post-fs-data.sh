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
    _fb=/data/adb/adbhide/uids.last
    _list=""
    for _src in "$_f" "$_fb"; do
        [ -r "$_src" ] || continue
        while read -r _u; do
            case "$_u" in ''|*[!0-9]*) continue;; esac
            _list="${_list:+$_list,}$_u"
        done <"$_src"
        [ -n "$_list" ] && break
        echo "$(date) uid list empty in $_src" >>"$LOG"
    done
    # An unclean shutdown can leave uids.txt truncated; without a fallback the
    # module would load with zero targets and stay unhidden until service.sh
    # runs sync.sh a minute later. uids.last is the previous good list.
    [ -n "$_list" ] || { echo "$(date) no usable uid list" >>"$LOG"; return 1; }
    echo "target_uids=$_list"
}

REQ="redirect=1 $(mk_req)"
# adbd listens on 5555 unless the tcp port was overridden
_p=$(getprop service.adb.tcp.port 2>/dev/null)
case "$_p" in ''|*[!0-9]*) _p=5555;; esac
REQ="$REQ adb_port=$_p"
echo "$(date) loading ko with: $REQ" >>"$LOG"

if [ -x /data/adb/ksud ]; then
    /data/adb/ksud insmod "$KO" $REQ >>"$LOG" 2>&1 && exit 0
fi

insmod "$KO" $REQ >>"$LOG" 2>&1
