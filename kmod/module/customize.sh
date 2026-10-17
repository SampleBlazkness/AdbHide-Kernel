#!/system/bin/sh
# KernelSU installer hook.

ui_print "- AdbHide (Kernel)"

mkdir -p /data/adb/adbhide
set_perm /data/adb/adbhide 0 0 0755

set_perm "$MODPATH/adbhide_kmod.ko" 0 0 0644
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755

ui_print "- Target list: /data/adb/adbhide/uids.txt"
ui_print "- Reboot required"
