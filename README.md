# AdbHide-Kernel

**Kernel-level ADB hiding for Android.** Hides the device's debugging state from
selected apps without injecting anything into their processes.

Target: GKI **android15-6.6** (kernel `6.6.x-android15-*`), KernelSU / KernelSU Next.

## Why kernel level

The userspace approach (LSPosed + Zygisk) can cover `Settings.Global` and the
in-process property view, but two things stay visible:

| Signal | Why userspace cannot hide it |
|---|---|
| `getprop` child process | `fork+exec` creates a fresh address space that maps the real property files |
| module mapped but not in `dl_iterate_phdr` | Zygisk loads modules by hand, so `maps` and the loader disagree |

Both are file- and kernel-level facts. This module changes them there instead:

- **property redirect** — for target uids, `/dev/__properties__/*` resolves to a
  pre-patched copy, so parent, child and `getprop` all read the same fake values
- **`/proc/net/tcp{,6}` filtering** — adbd's listening sockets are dropped from
  the seq_file output for target uids
- no code is injected into the app, so nothing appears in its `maps`

## Layout

```
kmod/
  adbhide.h              shared types and prototypes
  adbhide_main.c         init/exit, /proc/adbhide_kmod, event ring
  adbhide_uids.c         target uid list, counters
  adbhide_probes.c       kprobes: vfs_open, seq_read
  Makefile               Kbuild + DDK build entry
  module/                KernelSU module packaging (module.prop, scripts)
app/
  src/                   LSPosed module + status activity
  stubs/                 Xposed API stubs, compile-time only, never packaged
  res/, assets/          xposed_scope array, xposed_init
tools/build-apk.sh       aapt2 -> javac -> d8 -> apksigner, no Gradle
.github/workflows/       CI: build in ghcr.io/ylarod/ddk-min:android15-6.6-*
```

## Status

- [x] M0 — repository, license, CI skeleton
- [x] M1 — minimal module loads, `/proc/adbhide_kmod` readable
- [x] M2 — read-only probes (vfs_open / seq_read), verified on device
- [x] M3 — property redirect for `usb_control_prop` (sys.usb.config → mtp), verified per uid
- [ ] M4 — `/proc/net/tcp` filtering
- [ ] M5 — config integration, package, drop the Zygisk module

## Build

CI builds the module inside the official DDK image:

```
ghcr.io/ylarod/ddk-min:android15-6.6-20260313
```

Locally, with a matching kernel tree:

```bash
make -C kmod KERNEL_SRC=/path/to/android15-6.6 CLANG_DIR=/path/to/clang strip
```

The result is `kmod/adbhide_kmod.ko`; `kmod/module/` is the KernelSU zip layout.

## Safety

A kernel module can panic the device. This project therefore ships in stages:
every new behaviour is preceded by a read-only build that only logs. Recovery
paths are KernelSU's safe mode (press volume-down three times right after the
first boot screen) and the stock `boot.img` backup.

## License

MIT
