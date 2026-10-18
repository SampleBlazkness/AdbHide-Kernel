# AdbHide-Kernel

**Kernel-level ADB hiding for Android.** Chosen apps see no debugging state:
properties, `/proc/net/*` and the adbd endpoint all look inactive, while the
phone keeps working normally for you and your computer.

Target: GKI **android15-6.6** (kernel `6.6.x-android15-*`), KernelSU / KernelSU Next.
Blacklist model: `/data/adb/adbhide/uids.txt`, one uid per line.

## Why kernel level

A `getprop` child is a fresh `fork+exec`; nothing injected into the app reaches
it, and an app process itself never re-opens the property files — it inherits
zygote's shared mapping. Both are facts below the app, so the layers that hide
them sit below the app too.

| Layer | What it does | Why there |
|---|---|---|
| `adbhide_kmod` (kernel) | serves patched property-area copies on open; drops adbd listener rows from `/proc/net/tcp{,6}`; restores `VM_MAYSHARE` on the private remap | `getprop` children and seq_file reads are kernel-side |
| `adbhide_native` (Zygisk) | re-maps the adb property contexts `MAP_PRIVATE` in target app processes and patches the values | in-process reads use zygote's inherited mapping, not the file |
| `service.sh` (netfilter) | per-uid `REJECT` on the adbd port, per-uid 5353 drop, global drop of mDNS replies carrying `_adb` | mdnsd (uid 1020) answers for adbd, so a per-uid rule cannot hide the record from one app |
| `AdbHide` (LSPosed, optional) | `Settings.Global.adb_enabled` / `development_settings_enabled` / `adb_wifi_enabled` → 0 | plain API reads |

## What is covered

- `persist.sys.usb.config`, `sys.usb.config`, `sys.usb.state`, `init.svc.adbd`,
  `ro.debuggable`, `ro.secure`, `ro.adb.secure`, `service.adb.root`
  — for `getprop`, JNI, `SystemProperties` and reflection
- `/proc/net/tcp` and `tcp6`: adbd's listening row is removed for target uids,
  line numbering stays contiguous
- the adbd TCP endpoint: target uids get `ECONNRESET`, as if the port were closed
- mDNS: no `_adb` record is advertised to the network
- `Settings.Global` adb/developer switches for every non-system app (LSPosed layer)

## Trade-offs

- **mDNS advertisement is dropped globally.** `adb connect <ip>:5555` and
  `adb pair <ip>:<port> <code>` still work when you type the address; automatic
  discovery (`adb mdns services`, IDE device lists, QR pairing) does not.
- **Property values in a target process are a snapshot.** They are patched when
  the process starts; a property change afterwards is not reflected there.
- **Root itself is not hidden.** KernelSU, its manager and its modules remain
  visible to a detector that specifically hunts for them.
- The blacklist is by uid: a new detector must be added to `uids.txt`
  (and the module restarted) before it is covered.

## Layout

```
kmod/
  adbhide.h              shared types and prototypes
  adbhide_main.c         init/exit, /proc/adbhide_kmod, event ring
  adbhide_uids.c         target uid list, counters
  adbhide_probes.c       kprobes: vfs_open, seq_read
  adbhide_props.c        property-area redirect for fresh processes
  adbhide_netseq.c       /proc/net/tcp{,6} row removal
  adbhide_vma.c          VM_MAYSHARE restore for the private remap
  Makefile               Kbuild + DDK build entry
  module/                KernelSU packaging (module.prop, post-fs-data.sh, service.sh)
native/
  adbhide_native.cpp     Zygisk module: private property remap
  zygisk.hpp             Zygisk API v5 header (vendored)
  module.prop
app/
  src/                   LSPosed module + status activity
  stubs/                 Xposed API stubs, compile-time only, never packaged
  res/, assets/          xposed_scope array, xposed_init
tools/build-apk.sh       aapt2 -> javac -> d8 -> apksigner, no Gradle
.github/workflows/       CI: build in ghcr.io/ylarod/ddk-min:android15-6.6-*
```

## Build

CI builds the kernel module inside the official DDK image:

```
ghcr.io/ylarod/ddk-min:android15-6.6-20260313
```

Locally, with a matching kernel tree:

```bash
make -C kmod KERNEL_SRC=/path/to/android15-6.6 CLANG_DIR=/path/to/clang strip
```

The Zygisk library needs only the NDK:

```bash
aarch64-linux-android26-clang++ -shared -fPIC -O2 -std=c++17 \
  -static-libstdc++ -o zygisk/arm64-v8a.so adbhide_native.cpp -llog
```

The APK is built without Gradle: `tools/build-apk.sh`.

## Status

- [x] M0 — repository, license, CI
- [x] M1 — minimal module loads, `/proc/adbhide_kmod` readable
- [x] M2 — read-only probes, verified on device
- [x] M3 — property redirect for all adb contexts, verified per uid
- [x] M4 — `/proc/net/tcp{,6}` filtering, VMA flag restore
- [x] M5 — port rules, mDNS suppression, Zygisk property remap, status panel

Verified against Duck Detector 2026.10.18 (18/18 clean, ADB Runtime clear) with
wireless debugging enabled and the adbd port listening.

## Safety

A kernel module can panic the device. Every behaviour was preceded by a
read-only build, and the module only rewrites return values and file mappings
for uids on the list. Recovery: KernelSU safe mode (volume-down three times
right after the first boot screen) and a stock `boot.img` backup.

## License

MIT
