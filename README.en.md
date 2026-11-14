# AdbHide-Kernel

[简体中文](README.md) | [English](README.en.md)

Three modules: a manager (an LSPosed module), a kernel module and a Zygisk module.

1. **Manager (LSPosed) layer**: rewrites the developer options, USB debugging and wireless debugging values that apps read into "disabled", system-wide (the spoofing can be switched on and off). If the manager is not installed, or injection fails during boot, the kernel module and the Zygisk module stop working with it.
2. **Kernel layer**: hides adbd's socket, listening port, `/proc/net` entries and properties from the apps on the list.
3. **Zygisk layer**: replaces the property memory mapping inside target processes, covering apps that bypass the Java layer and read the property file directly.

## Full installation

1. Run `uname -r`, pick the kernel module from the table below and install it in your root manager.
2. Install `AdbHide-native-v*.zip`.
3. Install `AdbHide-*.apk`, grant root in your root manager, enable AdbHide in LSPosed and tick "System framework" in its scope (the recommended scope already includes it, ticked by default).
4. Reboot.
5. Open AdbHide, tap "Manage apps", tick the apps you want to hide, then save; it takes effect immediately.

> Steps 1, 2 and 3 can be done in any order.
> Installing only the manager, to spoof system settings and nothing else, out of fear of bricking the device, is not recommended: other modules already do more at the pure LSPosed level, for example DuckUSB and 「我不是开发者」.

| Kernel version (`uname -r`) | Kernel module to install                  |
| --------------------------- | ----------------------------------------- |
| `5.10.x-android12-*`        | `AdbHide-kmod-v1.3.14-android12-5.10.zip` |
| `5.10.x-android13-*`        | `AdbHide-kmod-v1.3.14-android13-5.10.zip` |
| `5.15.x-android13-*`        | `AdbHide-kmod-v1.3.14-android13-5.15.zip` |
| `5.15.x-android14-*`        | `AdbHide-kmod-v1.3.14-android14-5.15.zip` |
| `6.1.x-android14-*`         | `AdbHide-kmod-v1.3.14-android14-6.1.zip`  |
| `6.6.x-android15-*`         | `AdbHide-kmod-v1.3.14-android15-6.6.zip`  |
| `6.12.x-android16-*`        | `AdbHide-kmod-v1.3.14-android16-6.12.zip` |

Pick the module by kernel version, not by Android version. The first launch writes the default blacklist (Duck Detector, AdbHide Detector).

## Risk

A kernel crash, or a kernel that does not match, can brick the device. Make sure you are able to unbrick it and have backed up the stock `boot.img` before installing.

Suggested recovery order:

1. Hold Volume Down while booting (on KernelSU this enters) safe mode, which disables every module automatically.

2. If the system boots, disable the modules and reboot:

   ```bash
   su -c 'touch /data/adb/modules/adbhide_kmod/disable'
   su -c 'touch /data/adb/modules/adbhide_native/disable'
   ```

3. Recovery or Fastboot: delete the module directories, or flash back the stock `boot` image.

4. If none of that works: 9008, or after-sales service.

## Usage

- System setting spoofing: the master switch plus the developer options, USB debugging and wireless debugging entries apply immediately.
- Adding and removing apps: tick or untick them in the "Manage apps" panel; saving applies immediately. The panel does not list system apps by default; the toggle at the top switches that.

> Apps may need a restart to pick up hiding changes.

- Confirming it works: the three (possibly present) banners at the top report the status of root, the kernel module and the Zygisk module; the "System settings" card shows whether the LSPosed layer is active. There are a few more hints at the bottom.
- Uninstalling: remove the manager (LSPosed module), uninstall both modules, reboot.

## Detector

`AdbHide Detector` (`com.blazkness.adbhidedetector`) verifies whether wireless debugging hiding works. In testing, 1.4.0 and earlier fail the check; 1.4.15 passes.

## Known limitations

- Some wireless debugging features are affected: Shizuku cannot start over wireless debugging (though with root, why would Shizuku need wireless debugging), QR-code pairing does not work, and IDEs such as Android Studio cannot auto-discover the device over wireless debugging. Running `adb pair` and `adb connect` manually still works.
- The system keeps a record that ADB was once enabled (it is not a sign that ADB is currently running).
- Only tested, and known to work, on a Redmi K90 (Android 16, kernel `6.6.77-android15-8-g4a507830d890-ab13636293-4k`, KernelSU 3.3.x).

## Command-line check

```bash
su -c 'cat /proc/adbhide_kmod'                    # module state, counters, target UIDs
su -c 'cat /proc/adbhide_uids'                     # the current kernel list
su <target UID> -c 'getprop persist.sys.usb.config'   # -> mtp
su -c 'iptables -S ADBHIDE_PORTS'                  # adbd port rules per UID
su -c 'cat /sys/module/adbhide_kmod/parameters/adb_port'  # the port currently followed
```

## Build

The three modules are built independently.

**Kernel module**: needs the kernel sources for the target KMI, or a DDK image:

```bash
make -C kmod strip KERNEL_SRC=/opt/ddk/kdir/android15-6.6
```

The output is `kmod/adbhide_kmod.ko`. CI uses `ghcr.io/ylarod/ddk-min:android<version>-<KMI>` to cover 7 KMIs; see `.github/workflows/build.yml`.

**Zygisk module**: NDK r27c or newer; packaging needs python3:

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
tools/build-native.sh
```

**Manager APK**: JDK 17 or newer:

```bash
./gradlew :app:assembleRelease
```

The output lands in `app/build/outputs/apk/release/`. The signing config is read from `~/.android/adbhide-release.properties`; without it the build produces an unsigned APK. Use your own signing key when building it yourself.

## License

MIT
