# AdbHide-Kernel

[简体中文](README.md) | [English](README.en.md)

1. At the system property level, developer options and the debugging state are reported as disabled (globally).
2. (Optional) At the kernel level, the same state is reported as disabled, only for apps on the blacklist.

## Install

1. Run `uname -r`, pick the module from the table below, install it in KernelSU.
2. Install `AdbHide-native-v*.zip`, then reboot.
3. Install `AdbHide-*.apk`, grant root in KernelSU, enable AdbHide in LSPosed with only the system framework scope (selected by default), then reboot.

| Kernel line (`uname -r`) | Module to install                 |
| ------------------------ | --------------------------------- |
| `5.10.x-android12-*`     | `adbhide-kmod-android12-5.10.zip` |
| `5.10.x-android13-*`     | `adbhide-kmod-android13-5.10.zip` |
| `5.15.x-android13-*`     | `adbhide-kmod-android13-5.15.zip` |
| `5.15.x-android14-*`     | `adbhide-kmod-android14-5.15.zip` |
| `6.1.x-android14-*`      | `adbhide-kmod-android14-6.1.zip`  |
| `6.6.x-android15-*`      | `adbhide-kmod-android15-6.6.zip`  |
| `6.12.x-android16-*`     | `adbhide-kmod-android16-6.12.zip` |

Pick the module by kernel version, not by the Android version. On first launch the app writes
the default blacklist to `/data/adb/adbhide/blacklist.conf`.

## Risk

It has only been loaded on a Redmi K90 (Android 16, kernel
`6.6.77-android15-8-g4a507830d890-ab13636293-4k`, KernelSU 3.3.x). Modules for the other
kernel lines have not been tested on a device. A mismatched kernel can leave the phone unable
to boot, so make sure you can recover it first.

Recovery order:

1. Hold Volume Down while booting; KernelSU safe mode skips every module.
2. If the phone boots, disable the modules and reboot:
   ```bash
   su -c 'touch /data/adb/modules/adbhide_kmod/disable'
   su -c 'touch /data/adb/modules/adbhide_native/disable'
   ```
3. Recovery or fastboot: remove the module directories, or flash the stock `boot` image.

## Usage

- Add or remove an app: edit `/data/adb/adbhide/blacklist.conf`, one package per line, then tap
  the sync button in AdbHide. The list goes into the kernel through `/proc/adbhide_uids` and the
  netfilter rules are rebuilt.
- Verify: the panel lists the kernel module, the native module, the settings layer and the port
  rules, and ticks the ones that are active.
- Uninstall: remove the app, uninstall both KernelSU modules, reboot.

## Known limitations

- Wireless debugging discovery does not work: `_adb` mDNS records are dropped for every listener,
  so `adb mdns services`, IDE device lists and QR pairing cannot find the phone. Manual `adb pair`
  and `adb connect` still work.
- `/sys/module/adbhide_kmod` is visible to any app.
- The system keeps a record that ADB was enabled.
- After every change to the list, tap sync in AdbHide.
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

Manager APK (aapt2 → javac → d8 → apksigner, no Gradle):

```bash
export ANDROID_SDK_ROOT=/path/to/android-sdk
tools/build-apk.sh
```

Both scripts run on Windows (Git Bash / MSYS2), Linux and macOS, and pick the newest build-tools
and platform automatically. The signing key is read from `~/.android/adbhide-release.properties`
(override with `RELEASE_PROPS`). If you build it yourself, use your own signing key.

## License

MIT
