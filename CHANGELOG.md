# 更新日志

## v1.3.1 — 2026-10-25

首个带完整构建产物的版本。

### 内核模块

- 支持 7 个 KMI：`android12-5.10`、`android13-5.10`、`android13-5.15`、`android14-5.15`、`android14-6.1`、`android15-6.6`、`android16-6.12`
- 新增 5.10 / 5.15 兼容层：`kmap_local_page` / `kunmap_local` 回退到 `kmap` / `kunmap`
- `vm_flags_set` 按内核版本分流：6.1 GKI 起使用该接口，5.10 / 5.15 直接写 `vm_flags`
- 每个模块的 `module.prop` 描述开头标注对应 KMI，避免刷错版本
- 已知限制：仅在 Redmi K90（Android 16，内核 6.6.77）实机验证，其余 KMI 只经过编译

### 构建

- CI 只保留内核模块矩阵；APK 与 Zygisk native 改为本地构建
- 新增 `tools/build-native.sh`：使用 NDK r27c 编译 `arm64-v8a.so`，静态链接 libc++，并打包为 Zygisk 模块 zip
- `tools/build-apk.sh` 继续用正式密钥签名管理器 APK

### 文档

- 重写中英文 README：按 `uname -r` 选择 KMI 的对照表、安装顺序、开机循环的三种救援方式、已知限制和内核崩溃风险提示
- 中文 README 调整为默认显示语言

**完整变更**：https://github.com/SampleBlazkness/AdbHide-Kernel/commits/v1.3.1

---

# Changelog

## v1.3.1 — 2026-10-25

First release with the full set of build artifacts.

### Kernel module

- Seven KMIs: `android12-5.10`, `android13-5.10`, `android13-5.15`, `android14-5.15`, `android14-6.1`, `android15-6.6`, `android16-6.12`
- Compatibility shims for 5.10 / 5.15: `kmap_local_page` / `kunmap_local` fall back to `kmap` / `kunmap`
- `vm_flags_set` is selected by kernel version: used from 6.1 GKI, direct `vm_flags` writes on 5.10 / 5.15
- Each module's `module.prop` description is prefixed with its KMI so the wrong zip is easier to spot
- Known limit: only verified on a Redmi K90 (Android 16, kernel 6.6.77); the other KMIs are compile-tested only

### Build

- CI now builds only the kernel module matrix; the APK and the Zygisk native library are built locally
- New `tools/build-native.sh`: builds `arm64-v8a.so` with NDK r27c, statically links libc++, and packages the Zygisk module zip
- `tools/build-apk.sh` still signs the manager APK with the release key

### Docs

- Rewritten English and Chinese READMEs: a `uname -r` based KMI table, install order, three boot-loop recovery paths, known limitations and kernel panic warnings
- Chinese is now the default README language

**Full Changelog**: https://github.com/SampleBlazkness/AdbHide-Kernel/commits/v1.3.1
