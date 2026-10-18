# AdbHide-Kernel

[English](README.md) | [简体中文](README.zh-CN.md)

**Kernel-level ADB hiding for Android.** Chosen apps see no debugging state,
while the phone keeps working normally for you and your computer.

- Target: GKI **android15-6.6** (`6.6.x-android15-*`), KernelSU / KernelSU Next
- Blacklist model: the app writes `/data/adb/adbhide/blacklist.conf`, one package name per line

## What it is made of

| Component | Role |
|---|---|
| `AdbHide` (LSPosed APK) | Parent. Owns the blacklist, the status panel and the `Settings.Global` spoof |
| `adbhide_kmod` (kernel module) | Property-area redirect for `getprop`, `/proc/net/tcp{,6}` filtering, VMA flag restore |
| `adbhide_native` (Zygisk) | Rewrites the property mapping inside target app processes |

The kernel and Zygisk modules only read config produced by the app. Without the
app they report `未安装 AdbHide 核心应用` and hide nothing.

## Install

1. Install the KernelSU modules `adbhide_kmod` and `adbhide_native`, then reboot.
2. Install `AdbHide-*.apk`, open it once and grant root in the KernelSU manager.
3. In LSPosed, enable **AdbHide** and tick the **system framework** scope.
4. Reboot.

On first launch the app writes the default blacklist (six common detectors).

## Use

- **Add or remove an app**: edit `/data/adb/adbhide/blacklist.conf`, one package
  name per line, then tap **同步名单** in the panel. No reboot: the list is pushed
  into the kernel through `/proc/adbhide_uids` and the netfilter rules are rebuilt.
- **Check it works**: the panel shows the kernel module, the native module, the
  Settings layer and the port rules; each line is green when active.
- **Uninstall**: removing the app removes the blacklist and empties the kernel
  list on the next sync (the app owns the config).

## Wireless debugging

Hiding requires that `_adb` mDNS records are not advertised, so automatic
discovery is gone by design. Everything else still works:

- Pairing: `adb pair <ip>:<port> <code>` — use the address and port shown on the
  phone's "Pair device with pairing code" screen.
- Connecting: `adb connect <ip>:5555` (or the port shown on the wireless
  debugging screen).

`adb mdns services`, IDE device lists and QR-code pairing will not find the
phone, because the record is suppressed for every listener.

## Command-line check

```bash
su -c 'cat /proc/adbhide_kmod'                 # module state, counters, target uids
su -c 'cat /proc/adbhide_uids'                 # live kernel list
su 10361 -c 'getprop persist.sys.usb.config'   # -> mtp
iptables -S ADBHIDE_PORTS                      # adbd port resets per uid
```

## Build

Kernel module (CI uses the DDK image `ghcr.io/ylarod/ddk-min:android15-6.6-20260313`):

```bash
make -C kmod KERNEL_SRC=/path/to/android15-6.6 CLANG_DIR=/path/to/clang strip
```

Zygisk library:

```bash
aarch64-linux-android26-clang++ -shared -fPIC -O2 -std=c++17 \
  -static-libstdc++ -o zygisk/arm64-v8a.so adbhide_native.cpp -llog
```

APK: `tools/build-apk.sh` (aapt2 → javac → d8 → apksigner, no Gradle).

## License

MIT
