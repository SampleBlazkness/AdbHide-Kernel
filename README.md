# AdbHide-Kernel

[简体中文](README.md) | [English](README.en.md)

分为管理器（LSPosed 模块）、内核模块、Zygisk 模块共三个模块。

1. **管理器（LSPosed）层**：把应用读到的开发者模式、USB 调试、无线调试的设置值改写成未启用，全局生效（支持选择启用/关闭）。管理器未安装或开机注入失败时，内核模块和 Zygisk 模块随之失效。
2. **内核层**：对名单内的应用隐藏 adbd 的 socket、监听端口、`/proc/net` 记录与属性。
3. **Zygisk 层**：在目标进程内替换属性内存映射，覆盖绕过 Java 层直接读属性文件的应用。

## 完整安装

1. 执行 `uname -r`，按下表选择内核模块，在 Root 管理器里安装。
2. 安装 `AdbHide-native-v*.zip`。
3. 安装 `AdbHide-*.apk`，在 Root 管理器里授予 root，在 LSPosed 里启用 AdbHide，作用域勾选「系统框架」（推荐作用域已包含它，默认勾选）。
4. 重启。
5. 打开 AdbHide，点「管理应用」，勾选要隐藏的应用，保存即生效。

> 1、2、3 步顺序没有要求。
> 如果你害怕变砖而只安装管理器来实现仅伪造系统设置，这是不推荐的行为，因为有别的模块在纯 LSPosed 层面上做了更多的功能，例如 DuckUSB、「我不是开发者」。

| 内核版本（`uname -r`） | 要安装的内核模块名称                      |
| ---------------------- | ----------------------------------------- |
| `5.10.x-android12-*`   | `AdbHide-kmod-v1.3.13-android12-5.10.zip` |
| `5.10.x-android13-*`   | `AdbHide-kmod-v1.3.13-android13-5.10.zip` |
| `5.15.x-android13-*`   | `AdbHide-kmod-v1.3.13-android13-5.15.zip` |
| `5.15.x-android14-*`   | `AdbHide-kmod-v1.3.13-android14-5.15.zip` |
| `6.1.x-android14-*`    | `AdbHide-kmod-v1.3.13-android14-6.1.zip`  |
| `6.6.x-android15-*`    | `AdbHide-kmod-v1.3.13-android15-6.6.zip`  |
| `6.12.x-android16-*`   | `AdbHide-kmod-v1.3.13-android16-6.12.zip` |

按照内核版本选择刷入的模块，而不是按照安卓版本选择。首次打开应用写入默认黑名单（Duck Detector、AdbHide Detector）。

## 风险

存在内核崩溃或内核不匹配导致变砖的风险。安装前请确保你有救砖能力并备份原厂 `boot.img`。

可参考的救砖顺序：

1. 开机时按住音量下键，（如果你的 Root 实现是 KernelSU 就会）进入安全模式，全部模块自动禁用。

2. 能进系统时禁用模块后重启：

   ```bash
   su -c 'touch /data/adb/modules/adbhide_kmod/disable'
   su -c 'touch /data/adb/modules/adbhide_native/disable'
   ```

3. Recovery 或 Fastboot：删除模块目录或刷回原厂 `boot` 镜像。

4. 如果都不行那就 9008 或者售后。

## 使用

- 系统设置伪造：总开关和开发者选项、USB 调试、无线调试四项即时生效。
- 增删应用：「管理应用」面板里勾选或取消，保存后立即生效。面板默认不列出系统应用，顶部可切换。

> 应用可能需要重启以适配隐藏更改。

- 确认生效：顶部三条（可能出现的）横幅分别提示 root、内核模块、Zygisk 模块的状态；「系统设置」卡片显示 LSPosed 层是否生效。底部也有一些提示。
- 卸载：删除管理器（LSPosed 模块），卸载两个模块，重启。

## 检测工具

`AdbHide Detector`（`com.blazkness.adbhidedetector`）用于验证无线调试隐藏是否生效。实测 1.4.0 及更早的版本无法通过检测，1.4.15 可以通过。

## 已知限制

- 一些无线调试功能受到影响：Shizuku 无法使用无线调试启动（不过都 ROOT 了，Shizuku 还用什么无线调试），无法使用扫码配对，AS 等 IDE 无法通过无线调试自动发现设备。手动执行 `adb pair` 和 `adb connect` 不受影响。
- 系统中会留下 ADB 曾被启用的记录（不是 ADB 正在运行的标志）。
- 只在 Redmi K90（Android 16，内核 `6.6.77-android15-8-g4a507830d890-ab13636293-4k`，KernelSU 3.3.x）上实测过且正常工作。

## 命令行检查

```bash
su -c 'cat /proc/adbhide_kmod'                    # 模块状态、计数、目标 UID
su -c 'cat /proc/adbhide_uids'                     # 内核当前的名单
su <目标UID> -c 'getprop persist.sys.usb.config'   # -> mtp
su -c 'iptables -S ADBHIDE_PORTS'                  # 每个 UID 的 adbd 端口规则
su -c 'cat /sys/module/adbhide_kmod/parameters/adb_port'  # 当前跟随的端口
```

## 构建

三个模块各自独立构建。

**内核模块**：需要目标 KMI 的内核源码，或使用 DDK 镜像：

```bash
make -C kmod strip KERNEL_SRC=/opt/ddk/kdir/android15-6.6
```

产物为 `kmod/adbhide_kmod.ko`。CI 使用 `ghcr.io/ylarod/ddk-min:android<版本>-<KMI>` 覆盖 7 个 KMI，见 `.github/workflows/build.yml`。

**Zygisk 模块**：NDK r27c 或更新，打包需要 python3：

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
tools/build-native.sh
```

**管理器 APK**：JDK 17 或更新：

```bash
./gradlew :app:assembleRelease
```

产物在 `app/build/outputs/apk/release/`。签名配置从 `~/.android/adbhide-release.properties` 读取；未配置时产出未签名包。自行编译请使用自己的签名密钥。

## License

MIT
