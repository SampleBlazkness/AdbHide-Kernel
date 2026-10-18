#!/system/bin/sh
# blacklist.conf (package names) -> uids.txt -> live kernel list -> netfilter.
#
# Run at boot by service.sh and on demand from the app's "sync" button, so a
# newly installed app is covered without a reboot.

MODDIR=${0%/*}
LOG=/data/adb/adbhide/kmod.log
CONF=/data/adb/adbhide/blacklist.conf
UIDS=/data/adb/adbhide/uids.txt

LIST=""
if [ -r "$CONF" ]; then
    # pm may not answer yet when service.sh runs; wait for it
    i=0
    PM_LIST=""
    while [ $i -lt 12 ]; do
        PM_LIST=$(pm list packages -U 2>/dev/null)
        [ -n "$PM_LIST" ] && break
        i=$((i + 1))
        sleep 5
    done
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue;; esac
        pkg=$(echo "$line" | tr -d ' \r')
        [ -n "$pkg" ] || continue
        uid=$(echo "$PM_LIST" | sed -n "s/^package:$pkg uid:\([0-9]*\)$/\1/p" | head -n1)
        if [ -n "$uid" ]; then
            LIST="${LIST}${LIST:+,}$uid"
        else
            echo "$(date) sync: package not installed: $pkg" >>"$LOG"
        fi
    done <"$CONF"
else
    echo "$(date) sync: no blacklist.conf" >>"$LOG"
fi

TMP="$UIDS.tmp"
printf '%s\n' "$LIST" | tr ',' '\n' | sed '/^$/d' >"$TMP"
chown root:root "$TMP" 2>/dev/null
chmod 600 "$TMP" 2>/dev/null
mv -f "$TMP" "$UIDS"

if [ -w /proc/adbhide_uids ]; then
    echo "$LIST" >/proc/adbhide_uids 2>>"$LOG"
    echo "$(date) sync: applied [$LIST] -> /proc/adbhide_uids" >>"$LOG"
else
    echo "$(date) sync: /proc/adbhide_uids missing, list applies after reboot" >>"$LOG"
fi

sh "$MODDIR/rules.sh"

# The app is the parent node: without it the modules say so in their
# description, so a half-installed stack is visible in the manager.
HAVE_APK=0
pm list packages 2>/dev/null | grep -q "^package:com.blazkness.adbhide$" && HAVE_APK=1

# The app owns the list. Without it there is nothing to read, so drop the
# config instead of leaving a stale blacklist behind.
if [ "$HAVE_APK" = 0 ]; then
    rm -f /data/adb/adbhide/blacklist.conf /data/adb/adbhide/uids.txt
    [ -w /proc/adbhide_uids ] && : > /proc/adbhide_uids
    echo "$(date) sync: core app missing, config removed" >>"$LOG"
fi

set_desc() {
    _p="$1"
    [ -f "$_p" ] || return 0
    _d=$(sed -n 's/^description=//p' "$_p" | head -1)
    _d=${_d#未安装 AdbHide 核心应用; }
    if [ "$HAVE_APK" = 1 ]; then
        _new="$_d"
    else
        _new="未安装 AdbHide 核心应用; $_d"
    fi
    sed "s|^description=.*|description=$_new|" "$_p" > "$_p.tmp" 2>/dev/null &&         cat "$_p.tmp" > "$_p" && rm -f "$_p.tmp"
}

set_desc /data/adb/modules/adbhide_kmod/module.prop
set_desc /data/adb/modules/adbhide_native/module.prop
