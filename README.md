# AdbHide-Kernel
[简体中文](README.md) | [English](README.en.md)

> **已知问题（2026-11-08）**：无线调试隐藏不完整。`_adb` 的 mDNS 广播已经被丢弃，
> 但 adbd 的随机监听端口没有被挡——名单内的应用扫描本机端口仍能发现并连上它
> （当前只对 5555 做了 REJECT）。计划一周内修复。

管理器（LSPosed 模块）、内核模块、Zygisk 模块，三层各管一部分：

1. **LSPosed 层**：把应用读到的开发者模式、USB 调试、无线调试的设置值改写成未启用，全局生效。
   管理器没装或本次开机注入失败时，内核模块和 Zygisk 模块随之失效：内核名单被清空，`blacklist.conf` 本身保留。
2. **内核层**：对名单内的应用隐藏 adbd 的 socket、端口、`/proc/net` 记录与属性。
   只有管理器已安装、且本次开机 LSPosed 层注入成功时才生效。
3. **Zygisk 层**：在目标进程内替换属性内存映射，覆盖绕过 Java 层直接读属性文件的应用。

> 下面的步骤写成了三个模块全装、按顺序安装，实际不必：也可以只安装管理器（丢失一些隐藏），
> 或者只安装管理器和内核模块。安装顺序随意，装完都要重启。Zygisk 模块依赖内核模块生成的名单，
> 只安装管理器和 Zygisk 模块时 Zygisk 隐藏不生效。模块运行中卸载管理器，内核层和 Zygisk 层
> 要等到下次开机才停用。

## 安装

1. 执行 `uname -r`，按下表选择模块，在 Root 管理器里安装。
2. 安装 `AdbHide-native-v*.zip`。
3. 安装 `AdbHide-*.apk`，在 Root 管理器里授予 root，在 LSPosed 里启用 AdbHide，
   作用域勾选「系统框架」；如果列表里还有「设置存储」，也一并勾选，重启。
4. 打开 AdbHide，点「管理应用」，勾选要隐藏的应用，保存即写入并同步到内核。

> 1.3.1 及更早版本的签名与 1.4.0 不兼容。升级前需要先卸载旧版本，
> 装好之后 LSPosed 作用域和 root 授权需要重新设置。

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
KernelSU 3.3.x）上实际加载过；其余内核版本的模块和其他 Root 管理器没有实机验证。
内核版本不匹配可能导致无法开机，装之前请确保自己有救砖能力。

救砖顺序：

1. 开机时按住音量下键，（如果你的 Root 实现是 KernelSU）进入安全模式，全部模块自动禁用。
2. 能进系统时禁用模块后重启：
   ```bash
   su -c 'touch /data/adb/modules/adbhide_kmod/disable'
   su -c 'touch /data/adb/modules/adbhide_native/disable'
   ```
3. Recovery 或 Fastboot：删除模块目录，或刷回原厂 `boot` 镜像。

## 使用

- 系统属性伪造：总开关和开发者选项、USB 调试、无线调试四项即时生效，不需要重启。
  已经运行中的应用可能需要重启才会重新读取设置。
- 增删应用：「管理应用」面板里勾选或取消，保存后写入
  `/data/adb/adbhide/blacklist.conf` 并立即同步到内核。
  面板默认不列出系统应用，顶部可切换（本功能需要授予访问应用列表权限）。
- 确认生效：顶部三条（可能出现的）横幅分别提示 root、内核模块、Zygisk 模块的状态；
  「系统属性」卡片显示 LSPosed 层是否注入成功。
- 卸载：删除应用，卸载两个模块，重启。

## 已知限制

- 无线调试的自动发现不可用：`_adb` 的 mDNS 记录对所有监听者丢弃，`adb mdns services`、
  IDE 设备列表和扫码配对都找不到手机；手动 `adb pair` / `adb connect` 仍然可用。
- `/sys/module/adbhide_kmod` 对普通应用可见。
- 系统中会留下 ADB 曾被启用的记录（不是 ADB 正在运行的标志）。
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
它产出的包 LSPosed 无法解析：multiDex 把 `MainHook` 分到了 `classes2.dex`，而 LSPosed 只读
`classes.dex`。

如果你需要自行编译，请使用你自己的签名密钥。

## License

MIT