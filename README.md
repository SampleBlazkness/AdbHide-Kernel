# AdbHide-Kernel

[简体中文](README.md) | [English](README.en.md)

对名单内的 UID，`init.svc.adbd` 返回 `stopped`、`persist.sys.usb.config` 返回 `mtp`，
发往 adbd 端口的连接被丢弃；名单外的进程读到的仍是真实值。

## 安装

1. 执行 `uname -r`，按下表选 zip，在 KernelSU 里安装，先不要重启。
2. 安装 `AdbHide-native-v*.zip`，重启。
3. 安装 `AdbHide-*.apk`，打开一次并在 KernelSU 里授予 root；在 LSPosed 里启用 AdbHide，作用域勾选系统框架，再重启。

| 内核版本（`uname -r`） | 要安装的文件 |
|---|---|
| `5.10.x-android12-*` | `adbhide-kmod-android12-5.10.zip` |
| `5.10.x-android13-*` | `adbhide-kmod-android13-5.10.zip` |
| `5.15.x-android13-*` | `adbhide-kmod-android13-5.15.zip` |
| `5.15.x-android14-*` | `adbhide-kmod-android14-5.15.zip` |
| `6.1.x-android14-*` | `adbhide-kmod-android14-6.1.zip` |
| `6.6.x-android15-*` | `adbhide-kmod-android15-6.6.zip` |
| `6.12.x-android16-*` | `adbhide-kmod-android16-6.12.zip` |

选包看内核版本，不看设置里的 Android 版本。首次打开应用会把默认名单写到
`/data/adb/adbhide/blacklist.conf`。

## 风险

目前只在 Redmi K90（Android 16，内核 `6.6.77-android15-8-g4a507830d890-ab13636293-4k`，
KernelSU 3.3.x）上实际加载过；其余内核版本的模块只经过 CI 编译，没有开机验证。
内核版本不匹配可能导致无法开机，装之前先确认自己能把手机救回来。

救砖顺序：

1. 开机时按住音量下键，KernelSU 安全模式会跳过所有模块。
2. 能进系统时禁用模块后重启：
   ```bash
   su -c 'touch /data/adb/modules/adbhide_kmod/disable'
   su -c 'touch /data/adb/modules/adbhide_native/disable'
   ```
3. Recovery 或 fastboot：删除模块目录，或刷回原厂 `boot` 镜像。

## 使用

- 增删应用：编辑 `/data/adb/adbhide/blacklist.conf`，一行一个包名，然后在面板里点同步名单。
  名单经 `/proc/adbhide_uids` 实时写入内核，netfilter 规则同时重建。
- 确认生效：面板列出内核模块、Native 模块、系统设置层和端口规则，生效的打勾。
- 卸载：删除应用，卸载两个 KernelSU 模块，重启。

## 已知限制

- 无线调试的自动发现不可用：`_adb` 的 mDNS 记录对所有监听者丢弃，`adb mdns services`、
  IDE 设备列表和扫码配对都找不到手机；手动 `adb pair` / `adb connect` 仍然可用。
- `/sys/module/adbhide_kmod` 对普通应用可见。
- 内核模块保存目标 UID 的快照，改完名单要在应用里同步一次。
- 除测试机外，其他内核组合按实验性对待。

## 命令行检查

```bash
su -c 'cat /proc/adbhide_kmod'                 # 模块状态、计数、目标 UID
su -c 'cat /proc/adbhide_uids'                 # 内核当前的名单
su 10361 -c 'getprop persist.sys.usb.config'   # -> mtp
su -c 'iptables -S ADBHIDE_PORTS'              # 每个 UID 的 adbd 端口规则
```

## 构建

内核模块由 CI 使用 DDK 镜像 `ghcr.io/ylarod/ddk-min:android<版本>-<KMI>` 构建，
覆盖 5.10 到 6.12 共 7 个 KMI：

```bash
make -C kmod strip KERNEL_SRC=/opt/ddk/kdir/android15-6.6
```

Zygisk 库（NDK r27c 或更新）：

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
tools/build-native.sh
```

管理器 APK（aapt2 → javac → d8 → apksigner，不走 Gradle）：

```bash
export ANDROID_SDK_ROOT=/path/to/android-sdk
tools/build-apk.sh
```

两个脚本可在 Windows（Git Bash / MSYS2）、Linux 和 macOS 上运行，会自动选择最新的
build-tools 和 platform。签名密钥从 `~/.android/adbhide-release.properties` 读取，
可用 `RELEASE_PROPS` 指定别的路径，该文件不入库。

## License

MIT
