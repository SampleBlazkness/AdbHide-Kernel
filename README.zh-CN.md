# AdbHide-Kernel

[English](README.md) | [简体中文](README.zh-CN.md)

**内核级 ADB 隐藏。** 名单里的应用看不到调试状态,而你自己和电脑照常使用。

- 目标内核:GKI **android15-6.6**(`6.6.x-android15-*`),KernelSU / KernelSU Next
- 黑名单模式:由应用写 `/data/adb/adbhide/blacklist.conf`,一行一个包名

## 组成

| 组件 | 作用 |
|---|---|
| `AdbHide`(LSPosed 应用) | 父节点。管理名单、状态面板和 `Settings.Global` 伪造 |
| `adbhide_kmod`(内核模块) | 属性区重定向、`/proc/net/tcp{,6}` 过滤、VMA 标志还原 |
| `adbhide_native`(Zygisk 模块) | 在目标进程内改写属性映射 |

内核模块和 Zygisk 模块只读应用产生的配置。没有应用时,模块描述会显示
`未安装 AdbHide 核心应用`,并且不做任何隐藏。

## 安装

1. 安装 KernelSU 模块 `adbhide_kmod` 和 `adbhide_native`,重启。
2. 安装 `AdbHide-*.apk`,打开一次,在 KernelSU 管理器里授予 root。
3. 在 LSPosed 里启用 **AdbHide**,作用域勾选 **系统框架**。
4. 重启。

首次打开应用会写入默认名单(六个常用检测器)。

## 使用

- **增删应用**:编辑 `/data/adb/adbhide/blacklist.conf`,一行一个包名,然后在面板里点 **同步名单**。不需要重启:名单经 `/proc/adbhide_uids` 实时写入内核,netfilter 规则同时重建。
- **确认生效**:面板会列出内核模块、Native 模块、系统设置层和端口规则,生效的都打勾。
- **卸载**:删掉应用后,下一次同步会删除名单并清空内核列表(配置归应用所有)。

## 对无线调试的影响

隐藏要求不再广播 `_adb` 的 mDNS 记录,所以**自动发现不可用**,其余功能照常:

- 配对:`adb pair <ip>:<端口> <配对码>` —— IP 和端口用手机「使用配对码配对设备」界面上显示的那一组。
- 连接:`adb connect <ip>:5555`(或无线调试界面上显示的端口)。

`adb mdns services`、IDE 的设备列表、扫码配对都找不到手机,因为这条记录对所有监听者都被丢弃了。

## 命令行检查

```bash
su -c 'cat /proc/adbhide_kmod'                 # 模块状态、计数、目标 uid
su -c 'cat /proc/adbhide_uids'                 # 内核当前的名单
su 10361 -c 'getprop persist.sys.usb.config'   # -> mtp
iptables -S ADBHIDE_PORTS                      # 每个 uid 的 adbd 端口规则
```

## 构建

内核模块(CI 使用 DDK 镜像 `ghcr.io/ylarod/ddk-min:android15-6.6-20260313`):

```bash
make -C kmod KERNEL_SRC=/path/to/android15-6.6 CLANG_DIR=/path/to/clang strip
```

Zygisk 库:

```bash
aarch64-linux-android26-clang++ -shared -fPIC -O2 -std=c++17 \
  -static-libstdc++ -o zygisk/arm64-v8a.so adbhide_native.cpp -llog
```

APK:`tools/build-apk.sh`(aapt2 → javac → d8 → apksigner,不走 Gradle)。

## 许可

MIT
