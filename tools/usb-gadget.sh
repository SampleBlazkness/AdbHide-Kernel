#!/system/bin/sh
# adbng - SPDX-License-Identifier: MIT
#
# Gadget plumbing for the FunctionFS transport.
#
#   install   create the ffs function and mount functionfs on its directory
#   add       link it into the active config and rebind the UDC
#   del       unlink it and rebind (adbd's own function is left alone)
#   remove    del + unmount + drop the function directory
#   status    show what is currently wired
#
# Everything is overridable for other devices:
#   ADBNG_GADGET=/config/usb_gadget/g1  ADBNG_UDC=a600000.dwc3
#   ADBNG_FUNC=ffs.adbng                ADBNG_FFS_DIR=/dev/usb-ffs/adbng
#   ADBNG_SLOT=function9

G=${ADBNG_GADGET:-/config/usb_gadget/g1}
CFG=${ADBNG_CFG:-b.1}
FUNC=${ADBNG_FUNC:-ffs.adbng}
MNT=${ADBNG_FFS_DIR:-/dev/usb-ffs/adbng}
# configfs names the function ffs.<x>, but functionfs is mounted with the bare
# <x>: creating functions/ffs.adbng registers an ffs_dev called "adbng".
FFSNAME=${ADBNG_FFS_NAME:-${FUNC#ffs.}}
SLOT=${ADBNG_SLOT:-function9}
UDC=${ADBNG_UDC:-$(cat "$G/UDC" 2>/dev/null)}
[ -n "$UDC" ] || UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)

die() { echo "adbng-gadget: $*" >&2; exit 1; }

is_mounted() {
    grep -q " $MNT functionfs" /proc/mounts 2>/dev/null
}

case "$1" in
install)
    [ -d "$G" ] || die "no gadget at $G"
    [ -d "$G/functions/$FUNC" ] || mkdir "$G/functions/$FUNC" || die "cannot create $G/functions/$FUNC"
    mkdir -p "$MNT" || die "cannot create $MNT"
    is_mounted || mount -t functionfs "$FFSNAME" "$MNT" || die "mount functionfs on $MNT failed"
    echo "installed: function=$FUNC ($FFSNAME) mount=$MNT"
    ;;

add)
    [ -d "$G/functions/$FUNC" ] || die "run 'install' first"
    is_mounted || die "$MNT is not mounted, run 'install'"
    [ -L "$G/configs/$CFG/$SLOT" ] && die "$CFG/$SLOT is already linked"
    [ -n "$UDC" ] || die "no UDC"
    # configfs refuses to add a function to a config that is bound to a UDC
    # (config_usb_cfg_link() -> -EINVAL), so unbind first.  The link target has
    # to be absolute: configfs resolves the target against our cwd, not against
    # the directory the symlink lives in.  init/UsbGadget may rebind behind our
    # back, hence the retry.
    i=0
    while :; do
        echo "" > "$G/UDC"
        ln -s "$G/functions/$FUNC" "$G/configs/$CFG/$SLOT" && break
        i=$((i + 1))
        if [ "$i" -ge 3 ]; then
            echo "$UDC" > "$G/UDC"
            die "symlink failed after $i tries"
        fi
        sleep 0.1
    done
    echo "$UDC" > "$G/UDC"
    if [ "$(cat "$G/UDC" 2>/dev/null)" != "$UDC" ]; then
        rm -f "$G/configs/$CFG/$SLOT"
        echo "$UDC" > "$G/UDC"
        die "rebind failed - is the daemon running with ep0 open and descriptors+strings written?"
    fi
    echo "linked $CFG/$SLOT -> $FUNC, UDC=$UDC"
    ;;

del)
    if [ -L "$G/configs/$CFG/$SLOT" ]; then
        echo "" > "$G/UDC"
        rm -f "$G/configs/$CFG/$SLOT"
        if [ -L "$G/configs/$CFG/$SLOT" ]; then
            [ -n "$UDC" ] && echo "$UDC" > "$G/UDC"
            die "unlink failed (was the gadget rebound behind our back?)"
        fi
        [ -n "$UDC" ] && echo "$UDC" > "$G/UDC"
        echo "unlinked $CFG/$SLOT, UDC=$UDC"
    else
        echo "nothing to unlink"
    fi
    ;;

remove)
    if [ -L "$G/configs/$CFG/$SLOT" ]; then
        echo "" > "$G/UDC"
        rm -f "$G/configs/$CFG/$SLOT"
        [ -n "$UDC" ] && echo "$UDC" > "$G/UDC"
    fi
    is_mounted && umount "$MNT"
    rmdir "$MNT" 2>/dev/null
    rmdir "$G/functions/$FUNC" 2>/dev/null
    echo "removed $FUNC"
    ;;

status)
    echo "gadget   $G"
    echo "udc      ${UDC:-none} (state: $(cat "/sys/class/udc/$UDC/state" 2>/dev/null))"
    echo "function $FUNC $([ -d "$G/functions/$FUNC" ] && echo exists || echo missing)"
    echo "mount    $MNT $(is_mounted && echo mounted || echo not-mounted)"
    [ -L "$G/configs/$CFG/$SLOT" ] && echo "link     $CFG/$SLOT -> $(readlink "$G/configs/$CFG/$SLOT")" || echo "link     $CFG/$SLOT absent"
    ls "$G/configs/$CFG/" 2>/dev/null | grep '^function' | while read -r f; do
        echo "         $f -> $(readlink "$G/configs/$CFG/$f")"
    done
    ls -l "$MNT" 2>/dev/null | tail -n +2
    ;;

*)
    echo "usage: $0 {install|add|del|remove|status}" >&2
    exit 2
    ;;
esac