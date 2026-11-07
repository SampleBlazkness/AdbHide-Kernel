#!/system/bin/sh
# Keeps the kernel layer in step with the LSPosed layer: if the hook marker is
# missing or from another boot, the uid list is dropped within ~15 s.

LOG=/data/adb/adbhide/kmod.log

while true; do
    BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -d ' 
')
    HOOK_ID=$(cat /data/system/adbhide.hook 2>/dev/null | tr -d ' 
')
    if [ -n "$BOOT_ID" ] && [ "$BOOT_ID" != "$HOOK_ID" ]; then
        HAVE=$(cat /proc/adbhide_uids 2>/dev/null | tr -d ' 
')
        if [ -n "$HAVE" ] || [ -s /data/adb/adbhide/uids.txt ]; then
            : > /proc/adbhide_uids 2>/dev/null
            rm -f /data/adb/adbhide/uids.txt
            sh /data/adb/modules/adbhide_kmod/rules.sh 2>/dev/null
            echo "$(date) guard: lsposed layer gone, kernel list dropped" >>"$LOG"
        fi
    fi
    sleep 15
done
