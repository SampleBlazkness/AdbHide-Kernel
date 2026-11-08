# AdbHide-Kernel

[简体中文](README.md) | [English](README.en.md)

Manager (an LSPosed module), a kernel module and a Zygisk module, each covering one layer:

1. **LSPosed layer**: developer options, USB debugging and wireless debugging are reported as disabled to every app.
2. **Kernel layer**: hides adbd's socket, ports, `/proc/net` rows and properties from the apps on the list. It only runs when the manager is installed and the LSPosed layer was injected in this boot.
3. **Zygisk layer**: replaces the property memory mapping inside target processes, covering apps that read the property file directly instead of going through the Java API.

> You can install the layers separately. The APK alone only changes what apps read; add the
> kernel module to hide adbd's socket, ports and `/proc/net`; add the Zygisk module to cover
> apps that read the property file directly. The kernel module depends on the LSPosed layer:
> if injection fails in the current boot, the kernel list is cleared.

## Install

1. Run `uname -r`, pick the module from the table below, install it in your root manager.
2. Install `AdbHide-native-v*.zip`, then reboot.
3. Install `AdbHide-*.apk`, grant root in your root manager, enable AdbHide in LSPosed with the system
   "System framework" scope, plus "Settings storage" if the list shows it, then reboot.
4. Open AdbHide, tap "Manage apps", tick the apps to hide; saving writes and syncs immediately.

> The signing key changed in 1.4.0. You need to uninstall anything older before installing,
> then set the LSPosed scope and KernelSU grant again.

| Kernel line (`uname -r`) | Module to install                        |
| ------------------------ | ---------------------------------------- |
| `5.10.x-android12-*`     | `AdbHide-kmod-v1.2.6-android12-5.10.zip` |
| `5.10.x-android13-*`     | `AdbHide-kmod-v1.2.6-android13-5.10.zip` |
| `5.15.x-android13-*`     | `AdbHide-kmod-v1.2.6-android13-5.15.zip` |
| `5.15.x-android14-*`     | `AdbHide-kmod-v1.2.6-android14-5.15.zip` |
| `6.1.x-android14-*`      | `AdbHide-kmod-v1.2.6-android14-6.1.zip`  |
| `6.6.x-android15-*`      | `AdbHide-kmod-v1.2.6-android15-6.6.zip`  |
| `6.12.x-android16-*`     | `AdbHide-kmod-v1.2.6-android16-6.12.zip` |

Pick the module by kernel version, not by the Android version. On first launch the app writes
the default blacklist to `/data/adb/adbhide/blacklist.conf`.

## Risk

It has only been loaded on a Redmi K90 (Android 16, kernel
`6.6.77-android15-8-g4a507830d890-ab13636293-4k`, KernelSU 3.3.x). Modules for the other
kernel lines, and other root managers, have not been tested on a device. A mismatched kernel
can leave the phone unable to boot, so make sure you can unbrick it first.

Recovery order:

1. Hold Volume Down while booting (on KernelSU) to enter safe mode, which disables every module.
2. If the phone boots, disable the modules and reboot:
   ```bash
   su -c 'touch /data/adb/modules/adbhide_kmod/disable'
   su -c 'touch /data/adb/modules/adbhide_native/disable'
   ```
3. Recovery or Fastboot: remove the module directories, or flash the stock `boot` image.

## Usage

- Property spoofing switches: the master switch and the three per-setting switches apply
  immediately, no reboot needed.
- Add or remove an app: in "Manage apps", tick or untick it. Saving writes
  `/data/adb/adbhide/blacklist.conf` and syncs to the kernel immediately, no reboot needed.
  System apps are not listed by default; the toggle at the top shows them (this needs the app
  list permission). Apps that are already running may need a restart to read the new settings.
- Verify: the banners at the top report root, the kernel module and the Zygisk module; the
  "System properties" card reports whether the LSPosed layer was injected.
- Uninstall: remove the app, uninstall both modules, reboot.

## Known limitations

- Wireless debugging discovery does not work: `_adb` mDNS records are dropped for every listener,
  so `adb mdns services`, IDE device lists and QR pairing cannot find the phone. Manual `adb pair`
  and `adb connect` still work.
- `/sys/module/adbhide_kmod` is visible to any app.
- The system keeps a record that ADB was enabled.
- Uninstalling the manager while it runs leaves the kernel layer active until the next boot.
- Every kernel combination other than the tested device is experimental.

## Command-line check

```bash
su -c 'cat /proc/adbhide_kmod'                     # module state, counters, target UIDs
su -c 'cat /proc/adbhide_uids'                     # live kernel list
su <target UID> -c 'getprop persist.sys.usb.config'  # -> mtp
su -c 'iptables -S ADBHIDE_PORTS'                  # adbd port rules per UID
```

## Build

Kernel modules are built in CI with the DDK images
`ghcr.io/ylarod/ddk-min:android<version>-<kmi>`, covering seven KMIs from 5.10 to 6.12:

```bash
make -C kmod strip KERNEL_SRC=/opt/ddk/kdir/android15-6.6
```

Zygisk library (NDK r27c or newer):

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
tools/build-native.sh
```

The manager APK is built by GitHub Actions (`.github/workflows/build-apk.yml`). Release builds
are signed locally with `apksigner`; the key is read from `~/.android/adbhide-release.properties`
and never enters the repository. The old `tools/build-apk.sh` (aapt2 → javac → d8 by hand) is
deprecated — LSPosed could not parse what it produced.

If you build it yourself, use your own signing key.

## License

MIT
