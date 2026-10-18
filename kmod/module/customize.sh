#!/system/bin/sh
# KernelSU installer hook.

ui_print "- AdbHide Kernel"

mkdir -p /data/adb/adbhide
set_perm /data/adb/adbhide 0 0 0755

# seed the package list once; it is the only file the user edits
if [ ! -f /data/adb/adbhide/blacklist.conf ]; then
    cat >/data/adb/adbhide/blacklist.conf <<'EOF'
# One package name per line. Resolved to uids at boot and on "sync".
EOF
fi
set_perm /data/adb/adbhide/blacklist.conf 0 0 0600

set_perm "$MODPATH/adbhide_kmod.ko" 0 0 0644
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/sync.sh" 0 0 0755
set_perm "$MODPATH/rules.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755

ui_print "- Blacklist: /data/adb/adbhide/blacklist.conf (package names)"
ui_print "- Reboot required"
