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
    PM_LIST=$(pm list packages -U 2>/dev/null)
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
