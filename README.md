# AdbHide-Kernel

[简体中文](README.md) | [English](README.en.md)

**内核级 ADB 隐藏。** 名单里的应用看不到调试状态，而你自己和电脑照常使用。

> [!WARNING]
> **这是一个内核模块，安装前请先读完这一段。**
>
> - 只在 **一台** 设备上验证过：Redmi K90（2510DRK44C），Android 16，内核 `6.6.77-android15-8-g4a507830d890-ab13636293-4k`，KernelSU 3.3.x。
> - 其他内核版本的模块 **只经过 CI 编译，从未真正开机加载过**。它们可能加载失败、可能什么都不隐藏，也可能把内核搞崩。
> - 刷错内核版本，或者在和 KMI 不匹配的厂商内核上加载，可能导致 **内核 panic、无限重启或数据丢失**。
> - 刷之前先准备好能救砖的手段（见 [开机循环怎么办](#开机循环怎么办)）。不能自救的手机不要装。

## 环境要求

- arm64 设备，Android 12–16，**GKI 内核**
- KernelSU 或 KernelSU Next（模块通过它安装）
- LSPosed（Zygisk），管理器应用需要它
- **不支持 Android 11**：没有对应的 GKI 2.0 KMI，也没有构建目标

## 先选对内核模块

选哪个 zip 取决于 **内核版本**，不是设置里显示的 Android 版本。在终端里执行（或在 KernelSU 管理器里看内核版本）：

```bash
uname -r
```

| 内核版本（`uname -r`） | 常见 Android 版本 | 要安装的文件 |
|---|---|---|
| `5.10.x-android12-*` | 12 | `adbhide-kmod-android12-5.10.zip` |
| `5.10.x-android13-*` | 13 | `adbhide-kmod-android13-5.10.zip` |
| `5.15.x-android13-*` | 13 | `adbhide-kmod-android13-5.15.zip` |
| `5.15.x-android14-*` | 14 | `adbhide-kmod-android14-5.15.zip` |
| `6.1.x-android14-*` | 14 | `adbhide-kmod-android14-6.1.zip` |
| `6.6.x-android15-*` | 15，以及测试用的 Android 16 设备 | `adbhide-kmod-android15-6.6.zip` |
| `6.12.x-android16-*` | 16 | `adbhide-kmod-android16-6.12.zip` |

测试用的 Redmi K90 是 **Android 16 + 6.6 内核**，所以它用的是 `android15-6.6`，不是 `android16-6.12`。只按 Android 版本选，是最常见的刷错原因。

## 组成

| 组件 | 作用 |
|---|---|
| `AdbHide`（LSPosed 应用） | 父节点。管理名单、状态面板和 `Settings.Global` 伪造 |
| `adbhide_kmod`（内核模块） | 属性区重定向、`/proc/net/tcp{,6}` 过滤、VMA 标志还原 |
| `adbhide_native`（Zygisk 模块） | 在目标进程内改写属性映射 |

内核模块和 Zygisk 模块只读应用产生的配置。没有应用时，模块描述会显示“核心应用未安装”，并且不做任何隐藏。

## 安装

1. 在 KernelSU 里安装与你 `uname -r` 对应的内核模块 zip，**先不要重启**。
2. 接着安装 `AdbHide-native-v*.zip`。
3. 重启。
4. 安装 `AdbHide-*.apk`，打开一次，在 KernelSU 管理器里授予 root。
5. 在 LSPosed 里启用 **AdbHide**，作用域勾选 **系统框架**。
6. 重启。

首次打开应用会写入默认名单（6 个常用检测器）到 `/data/adb/adbhide/blacklist.conf`。

如果第 3 步之后开不了机，立刻看 [开机循环怎么办](#开机循环怎么办)，不要反复重启。

## 使用

- **增删应用**：编辑 `/data/adb/adbhide/blacklist.conf`，一行一个包名，然后在面板里点 **同步名单**。不需要重启：名单经 `/proc/adbhide_uids` 实时写入内核，netfilter 规则同时重建。
- **确认生效**：面板会列出内核模块、Native 模块、系统设置层和端口规则，生效的都打勾。
- **卸载**：删掉应用，再在 KernelSU 里卸载两个模块，重启。

## 开机循环怎么办

内核模块在启动阶段 panic 时，界面上没有任何可点的东西。按顺序尝试：

1. **KernelSU 安全模式**：开机时按住 **音量下键**。KernelSU 会跳过所有模块启动，手机就能正常进入系统。
2. **能开机时禁用模块**：
   ```bash
   su -c 'touch /data/adb/modules/adbhide_kmod/disable'
   su -c 'touch /data/adb/modules/adbhide_native/disable'
   ```
   然后重启。
3. **Recovery / fastboot**：删掉模块目录，或者刷回原厂 `boot` 镜像。这条一定能救，但需要电脑和对应镜像。

如果内核的 `panic_on_oops` 是 1（默认值），一次 oops 就足以重启；反复启动进同一个模块，看起来就像彻底变砖，直到模块被禁用为止。

## 已知限制

- 无线调试的自动发现按设计不可用：所有监听者都收不到 `_adb` 的 mDNS 记录，所以 `adb mdns services`、IDE 设备列表、扫码配对都找不到手机。手动 `adb pair` / `adb connect` 仍然可用。
- `/sys/module/adbhide_kmod` 对普通应用可见，模块名没有隐藏。
- 内核模块保存的是目标 uid 的快照；改完名单后要在应用里同步一次（不需要重启）。
- 除上面那台测试机外，其他组合都按实验性对待。

## 命令行检查

```bash
su -c 'cat /proc/adbhide_kmod'                 # 模块状态、计数、目标 uid
su -c 'cat /proc/adbhide_uids'                 # 内核当前的名单
su 10361 -c 'getprop persist.sys.usb.config'   # -> mtp
iptables -S ADBHIDE_PORTS                      # 每个 uid 的 adbd 端口规则
```

## 构建

内核模块由 CI 用 DDK 镜像 `ghcr.io/ylarod/ddk-min:android<版本>-<KMI>` 构建，覆盖 5.10 到 6.12 共 7 个 KMI：

```bash
make -C kmod strip KERNEL_SRC=/opt/ddk/kdir/android15-6.6
```

Zygisk 库（NDK r27c）：

```bash
tools/build-native.sh          # 使用 $ANDROID_NDK_HOME 或 D:/Android_SDK/ndk/android-ndk-r27c
```

管理器 APK（aapt2 → javac → d8 → apksigner，不走 Gradle）：

```bash
tools/build-apk.sh
```

## 许可

MIT
