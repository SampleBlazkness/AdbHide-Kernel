#!/system/bin/sh
# The module is loaded permanently, so a reboot is what removes it.
# Keep the log so a failed load can be diagnosed after reinstall.

echo "$(date) module removed (reboot to unload)" >>/data/adb/adbhide/kmod.log 2>/dev/null
