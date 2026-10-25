# AdbHide-Kernel

[简体中文](README.md) | [English](README.en.md)

**Kernel-level ADB hiding for Android.** Apps on the blacklist see no debugging
state; you and your computer keep working normally.

> [!WARNING]
> **This is a kernel module. Read this before installing.**
>
> - Verified on exactly **one** device: Redmi K90 (2510DRK44C), Android 16,
>   kernel `6.6.77-android15-8-g4a507830d890-ab13636293-4k`, KernelSU 3.3.x.
> - Modules for the other kernel versions are **compiled by CI but have never
>   been booted**. They may not load, may hide nothing, or may crash the kernel.
> - A module built for the wrong kernel, or loaded on a vendor kernel that does
>   not match its KMI, can cause a **kernel panic, boot loop or data loss**.
> - Keep a working rescue path before you flash anything (see
>   [If it boot loops](#if-it-boot-loops)). Do not install this on a phone you
>   cannot recover.

## Requirements

- arm64 device, Android 12-16, **GKI kernel**
- KernelSU or KernelSU Next (the module is installed through it)
- LSPosed (Zygisk) for the manager app
- Android 11 is **not supported**: no GKI 2.0 KMI and no build target

## Pick the right kernel module first

The zip is chosen by **kernel version**, not by the Android version shown in
Settings. Run this in a terminal (or check your kernel in the KernelSU manager):

```bash
uname -r
```

| Kernel line (`uname -r`) | Android (typical) | File to install |
|---|---|---|
| `5.10.x-android12-*` | 12 | `adbhide-kmod-android12-5.10.zip` |
| `5.10.x-android13-*` | 13 | `adbhide-kmod-android13-5.10.zip` |
| `5.15.x-android13-*` | 13 | `adbhide-kmod-android13-5.15.zip` |
| `5.15.x-android14-*` | 14 | `adbhide-kmod-android14-5.15.zip` |
| `6.1.x-android14-*` | 14 | `adbhide-kmod-android14-6.1.zip` |
| `6.6.x-android15-*` | 15, and the tested Android 16 device | `adbhide-kmod-android15-6.6.zip` |
| `6.12.x-android16-*` | 16 | `adbhide-kmod-android16-6.12.zip` |

The tested Redmi K90 runs **Android 16 on a 6.6 kernel**, so it uses the
`android15-6.6` build. Using the Android version as the only hint is the most
common way to install the wrong module.

## What it is made of

| Component | Role |
|---|---|
| `AdbHide` (LSPosed APK) | Parent. Owns the blacklist, the status panel and the `Settings.Global` spoof |
| `adbhide_kmod` (kernel module) | Property-area redirect for `getprop`, `/proc/net/tcp{,6}` filtering, VMA flag restore |
| `adbhide_native` (Zygisk) | Rewrites the property mapping inside target app processes |

The kernel and Zygisk modules only read config produced by the app. Without the
app they report `core app not installed` and hide nothing.

## Install

1. Install the kernel module zip that matches `uname -r` in KernelSU, **do not
   reboot yet**.
2. Install `AdbHide-native-v*.zip` in KernelSU as well.
3. Reboot.
4. Install `AdbHide-*.apk`, open it once and grant root in the KernelSU manager.
5. In LSPosed, enable **AdbHide** and tick the **system framework** scope.
6. Reboot.

On first launch the app writes the default blacklist (six common detectors) to
`/data/adb/adbhide/blacklist.conf`.

If the phone does not boot after step 3, go to
[If it boot loops](#if-it-boot-loops) immediately - do not keep rebooting.

## Use

- **Add or remove an app**: edit `/data/adb/adbhide/blacklist.conf`, one package
  name per line, then tap **sync list** in the panel. No reboot: the list is
  pushed into the kernel through `/proc/adbhide_uids` and the netfilter rules are
  rebuilt.
- **Check it works**: the panel shows the kernel module, the native module, the
  Settings layer and the port rules; each line is green when active.
- **Uninstall**: remove the app, then uninstall both KernelSU modules and reboot.

## If it boot loops

A kernel module that panics during boot leaves no UI to tap. Use one of these,
in order:

1. **KernelSU safe mode** - hold **Volume Down** while the phone boots. KernelSU
   then starts without loading modules, so the phone boots normally.
2. **Disable the module from a working boot**:
   ```bash
   su -c 'touch /data/adb/modules/adbhide_kmod/disable'
   su -c 'touch /data/adb/modules/adbhide_native/disable'
   ```
   then reboot.
3. **Recovery / fastboot** - remove the module directory, or restore the stock
   `boot` image. This always works, but it needs a PC and the right image.

If `panic_on_oops` is 1 on your kernel (common on GKI kernels), one oops is enough to
reboot the phone; repeated boots into the same module can look like a hard brick
until a module is disabled.

## Known limitations

- Wireless debugging discovery is gone on purpose: `_adb` mDNS records are
  dropped for every listener, so `adb mdns services`, IDE device lists and QR
  pairing cannot find the phone. Manual `adb pair` / `adb connect` still work.
- `/sys/module/adbhide_kmod` is visible to any app; the module name is not
  hidden.
- The kernel module keeps a snapshot of the target uids; after changing the
  blacklist, sync from the app (no reboot needed).
- Only the tested device above is known to work. Treat every other combination
  as experimental.

## Command-line check

```bash
su -c 'cat /proc/adbhide_kmod'                 # module state, counters, target uids
su -c 'cat /proc/adbhide_uids'                 # live kernel list
su 10361 -c 'getprop persist.sys.usb.config'   # -> mtp
su -c 'iptables -S ADBHIDE_PORTS'               # adbd port resets per uid
```

## Build

Kernel modules are built in CI with the DDK images
`ghcr.io/ylarod/ddk-min:android<version>-<kmi>` for seven KMIs (5.10 to 6.12):

```bash
make -C kmod strip KERNEL_SRC=/opt/ddk/kdir/android15-6.6
```

Zygisk library (NDK r27c or newer):

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
tools/build-native.sh
```

Manager APK (aapt2 -> javac -> d8 -> apksigner, no Gradle):

```bash
export ANDROID_SDK_ROOT=/path/to/android-sdk
tools/build-apk.sh
```

Both scripts run on Windows (Git Bash / MSYS2), Linux and macOS, and pick the
newest build-tools and platform automatically. The APK signing key is read from
`~/.android/adbhide-release.properties` (override with `RELEASE_PROPS`); that
file is not tracked.

## License

MIT
