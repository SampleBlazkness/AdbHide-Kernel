# AdbHide-Kernel
[简体中文](README.md) | [English](README.en.md)

管理器（LSPosed 模块）、内核模块、Zygisk 模块，三层各管一部分：

1. **LSPosed 层**：把应用读到的开发者模式、USB 调试、无线调试的设置项改写成未启用，全局生效。
2. **内核层**：对名单内的应用隐藏 adbd 的 socket、端口、`/proc/net` 记录与属性。
   只有管理器已安装、且本次开机 LSPosed 层注入成功时才生效。
3. **Zygisk 层**：在目标进程内替换属性内存映射，覆盖绕过 Java 层直接读属性文件的应用。

> 下面的步骤按三层全装来写。三层可以分开装：只装 APK 就只有 LSPosed 层，应用读到的是未启用；
> 再加内核模块才会隐藏 adbd 的 socket、端口和 `/proc/net`；再加 Zygisk 模块才能覆盖直接读属性文件的应用。
> 内核模块依赖 LSPosed 层：本次开机注入没成功时，内核名单会被清空。

## 安装

1. 执行 `uname -r`，按下表选择模块，在 KernelSU 里安装。
2. 安装 `AdbHide-native-v*.zip`，重启。
3. 安装 `AdbHide-*.apk`，在 KernelSU 里授予 root，在 LSPosed 里启用 AdbHide，
   作用域勾选「系统框架」（设置存储与它同进程，不需要单独勾选），再重启。
4. 打开 AdbHide，点「管理应用」，勾选要隐藏的应用，保存即写入并同步到内核。

> 1.3.1 及更早版本的签名与 1.4.0 不兼容。升级前需要先卸载旧版本，
> 装好之后 LSPosed 作用域和 KernelSU 授权需要重新设置。

| 内核版本（`uname -r`） | 要安装的模块名称                         |
| ---------------------- | ---------------------------------------- |
| `5.10.x-android12-*`   | `AdbHide-kmod-v1.2.6-android12-5.10.zip` |
| `5.10.x-android13-*`   | `AdbHide-kmod-v1.2.6-android13-5.10.zip` |
| `5.15.x-android13-*`   | `AdbHide-kmod-v1.2.6-android13-5.15.zip` |
| `5.15.x-android14-*`   | `AdbHide-kmod-v1.2.6-android14-5.15.zip` |
| `6.1.x-android14-*`    | `AdbHide-kmod-v1.2.6-android14-6.1.zip`  |
| `6.6.x-android15-*`    | `AdbHide-kmod-v1.2.6-android15-6.6.zip`  |
| `6.12.x-android16-*`   | `AdbHide-kmod-v1.2.6-android16-6.12.zip` |

按照内核版本选择刷入的模块，而不是按照安卓版本选择。首次打开应用会把默认黑名单写到
`/data/adb/adbhide/blacklist.conf`。

## 风险

目前只在 Redmi K90（Android 16，内核 `6.6.77-android15-8-g4a507830d890-ab13636293-4k`，
KernelSU 3.3.x）上实际加载过；其余内核版本的模块没有实机验证。
内核版本不匹配可能导致无法开机，装之前先确认自己有救砖能力。

救砖顺序：

1. 开机时按住音量下键，KernelSU 安全模式会跳过所有模块。
2. 能进系统时禁用模块后重启：
   ```bash
   su -c 'touch /data/adb/modules/adbhide_kmod/disable'
   su -c 'touch /data/adb/modules/adbhide_native/disable'
   ```
3. Recovery 或 fastboot：删除模块目录，或刷回原厂 `boot` 镜像。

## 使用

- 增删应用：「管理应用」面板里勾选或取消，保存后写入
  `/data/adb/adbhide/blacklist.conf` 并立即同步到内核，不需要重启。
  面板默认不列系统应用，顶部可切换。
- 开关：总开关和开发者选项 / ADB 调试 / 无线调试三个分项即时生效，不需要重启。
  已经运行中的应用要重启才会重新读取设置。
- 确认生效：顶部三条横幅分别提示 su、内核模块、Zygisk 模块的状态；
  「系统属性」卡片显示 LSPosed 层是否注入成功。
- 卸载：删除应用，卸载两个 KernelSU 模块，重启。

## 已知限制

- 无线调试的自动发现不可用：`_adb` 的 mDNS 记录对所有监听者丢弃，`adb mdns services`、
  IDE 设备列表和扫码配对都找不到手机；手动 `adb pair` / `adb connect` 仍然可用。
- `/sys/module/adbhide_kmod` 对普通应用可见。
- 系统中会留下 ADB 曾被启用的记录。
- 运行中卸载管理器，内核层要等到下次开机才停用。
- 除测试机外，其他内核组合按实验性对待。

## 命令行检查

```bash
su -c 'cat /proc/adbhide_kmod'                 # 模块状态、计数、目标 UID
su -c 'cat /proc/adbhide_uids'                 # 内核当前的名单
su <目标UID> -c 'getprop persist.sys.usb.config'   # -> mtp
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

管理器 APK 由 GitHub Actions 构建（`.github/workflows/build-apk.yml`）。
release 构建在本机用 `apksigner` 签名，密钥从 `~/.android/adbhide-release.properties`
读取，不进入仓库。旧的 `tools/build-apk.sh`（aapt2 → javac → d8 手工打包）已弃用，
产出的包 LSPosed 无法解析。

如果你需要自行编译，请使用你自己的签名密钥。

## License

MIT
