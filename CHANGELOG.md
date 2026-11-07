# 更新日志

## v1.4.0 — 2026-11-07

### 管理器

- 重写了 UI，优化了模块状态判断，添加了应用内作用域调整，添加了应用内更新检查
- 新增英文语言

### 内核模块

- 名单同步增加前置条件：管理器必须已安装，且本次开机的 LSPosed 层注入成功，否则清空内核名单

### Zygisk 模块

- 构建时 strip：`.so` 由 1.7 MB 降到 263 KB，模块 zip 由 409 KB 降到 88 KB

### 签名

- 由于不小心泄露了签名，因此更换签名密钥（SHA-256 `3920ed6f…`）。与 1.3.1 不兼容，
  升级前必须卸载旧版本，LSPosed 作用域和 KernelSU 授权需要重新设置
- APK 改由 GitHub Actions 构建（本地构建由于未知问题无法被 LSPosed 识别）

---

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

## v1.4.0 — 2026-11-07

### Manager

- Rewrote the UI, improved module state detection, added in-app scope selection and an in-app update check
- Added English

### Kernel module

- List sync now requires the manager to be installed and the LSPosed layer to have been injected this boot; otherwise the kernel list is cleared

### Zygisk module

- The build strips the binary: `.so` 1.7 MB → 263 KB, module zip 409 KB → 88 KB

### Signing

- Signing key replaced after the previous one was leaked by mistake (SHA-256 `3920ed6f…`). Not compatible with 1.3.1: uninstall before installing; LSPosed scope and KernelSU grants must be set again
- The APK is now built by GitHub Actions (local builds were not recognised by LSPosed)

---

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
