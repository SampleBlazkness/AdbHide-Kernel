#!/system/bin/sh
# KernelSU installer hook.

ui_print "- AdbHide Kernel"

mkdir -p /data/adb/adbhide
set_perm /data/adb/adbhide 0 0 0755

set_perm "$MODPATH/adbhide_kmod.ko" 0 0 0644
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/sync.sh" 0 0 0755
set_perm "$MODPATH/rules.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755

ui_print "- Blacklist: /data/adb/adbhide/blacklist.conf (package names)"
ui_print "- Reboot required"
