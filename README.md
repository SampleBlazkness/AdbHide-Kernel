# adbng

自写用户态 ADB **设备端**实现。目标：PC 上用原厂 `adb` 直接连上本机，不经过系统 `adbd`。

> 状态：M2（协议核心）+ M3 前半（shell / exec）完成，真机 8/8 回归通过。
> `sync:` 与 pty 未做。**当前不是可发布形态。**

## 已实现（真的）

- 24 字节 ADB 帧格式，`magic = command ^ 0xFFFFFFFF`，超长包（> `A_MAXDATA`）拒绝
- `CNXN` 双向 banner 交换
- `AUTH`：`A_SIGNATURE`（RSA-2048 / SHA-1 / PKCS#1 v1.5 验签）+ `A_RSAPUBLICKEY` 记录指纹，3 轮上限
- 通道分配 / `OKAY` / `WRTE` / `CLSE` 语义
- `adb_keys` 格式解析（自写 bignum + SHA-1 + base64，无 openssl 依赖）
- 单线程 `poll` 事件循环，每通道双向流控（收 WRTE 回 OKAY，发 WRTE 等 OKAY）
- `shell:` v1（stdout/stderr 合并，无退出码）
- `shell,v2,raw:` 子包格式（stdout/stderr 分流 + 退出码）
- `exec:`（NUL 分隔 argv，不过 shell，参数原样透传）
- 未识别服务 → `CLSE` 拒绝

## 未实现（别当真）

- banner 里的机型属性取自 `ADBNG_PRODUCT_*` 环境变量，**不是**读 Android 属性服务
- 新 host 只记录指纹后断开，**没有**本机确认流程
- `sync:`（推/拉文件）、`forward:`、`reboot:` 仍走拒绝分支
- pty 未实现：请求 `pty` 时降级为管道，交互式 `adb shell` 的体验会不一样
- **仅 socketpair transport，没有 FunctionFS** → PC 现在还连不上
- 本机构建是 glibc 静态链接，非发布形态；发布走 CI 的 NDK / bionic

## 构建

需要 clang / lld / make。产物在 `build/`。

```sh
make key      # 生成测试用 RSA 密钥对（python3）
make          # usbgwd + host_tester + bn_test + sig_tool
make test     # host_tester 驱动 usbgwd 跑全部回归
make test-bad # 只跑"签名错误应被拒绝"
```

## 测试

`tests/host_tester.c` 是手写的 host 端，用 `socketpair` 驱动 `usbgwd`，不依赖 USB。
当前 8 项回归：

```
[host] PASS handshake + auth
[host] PASS shell: v1
[host] PASS shell: v2 stdout/stderr/exit
[host] PASS shell: v2 300 KB chunked output
[host] PASS shell: v2 stdin
[host] PASS exec: NUL-separated argv
[host] PASS shell: command not found -> 127
[host] PASS refused service -> CLSE
[host] RESULT: 8/8 passed
```

`bn_test` 与 python `pow()` 逐字节比对；`sig_tool` 做签/验往返。

## 许可

MIT
