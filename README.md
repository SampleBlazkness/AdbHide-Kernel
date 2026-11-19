# adbng

自写用户态 ADB **设备端**实现。目标：PC 上用原厂 `adb` 直接连上本机，不经过系统 `adbd`。

> 状态：M2（协议核心）完成并在真机验证。M3 未开始。**当前不是可发布形态。**

## 已实现（真的）

- 24 字节 ADB 帧格式，`magic = command ^ 0xFFFFFFFF`，超长包（> `A_MAXDATA`）拒绝
- `CNXN` 双向 banner 交换
- `AUTH`：`A_SIGNATURE`（RSA-2048 / SHA-1 / PKCS#1 v1.5 验签）+ `A_RSAPUBLICKEY` 记录指纹，3 轮上限
- 通道分配 / `OKAY` / `WRTE` / `CLSE` 语义
- `adb_keys` 格式解析（自写 bignum + SHA-1 + base64，无 openssl 依赖）

## 未实现（别当真）

- banner 里的机型属性取自 `ADBNG_PRODUCT_*` 环境变量，**不是**读 Android 属性服务
- 新 host 只记录指纹后断开，**没有**本机确认流程
- `shell:` / `exec:` / `sync:` 全部走拒绝分支
- `echo:` 是脚手架，M3 删除
- **仅 socketpair transport，没有 FunctionFS** → PC 现在还连不上
- 本机构建是 glibc 静态链接，非发布形态；发布走 CI 的 NDK / bionic

## 构建

需要 clang / lld / make。产物在 `build/`。

```sh
make key      # 生成测试用 RSA 密钥对（python3）
make          # usbgwd + host_tester + bn_test + sig_tool
make test     # host_tester 驱动 usbgwd 跑完整握手
```

## 测试

`tests/host_tester.c` 是手写的 host 端，用 `socketpair` 驱动 `usbgwd`，不依赖 USB。

```
[host] banner: device::ro.product.name=adbng;...
[host] token: 8bf588aa...
[host] authenticated, banner again
[host] service payload: pong:hello-adbng
[host] RESULT: handshake + auth + channel round trip OK
```

`bn_test` 与 python `pow()` 逐字节比对；`sig_tool` 做签/验往返。

## 许可

MIT
