# adbng

自写用户态 ADB **设备端**实现。目标：PC 上用原厂 `adb` 直接连上本机，不经过系统 `adbd`。

> 状态：M2（协议核心）+ M3（shell / exec / pty / sync 读方向）完成，proot 与真机各 15/15 回归通过。
> `sync` 写方向（push）未做。**当前不是可发布形态。**

## 已实现（真的）

- 24 字节 ADB 帧格式，`magic = command ^ 0xFFFFFFFF`，超长包（> `A_MAXDATA`）拒绝
- `CNXN` 双向 banner 交换
- `AUTH`：`A_SIGNATURE`（RSA-2048 / SHA-1 / PKCS#1 v1.5 验签）+ `A_RSAPUBLICKEY` 记录指纹，3 轮上限
- 通道分配 / `OKAY` / `WRTE` / `CLSE` 语义
- `adb_keys` 格式解析（自写 bignum + SHA-1 + base64，无 openssl 依赖）
- 单线程 `poll` 事件循环，每通道双向流控（收 WRTE 回 OKAY，发 WRTE 等 OKAY）
- `shell:` v1（stdout/stderr 合并，无退出码）
- `shell,v2,raw:` 子包格式（stdout/stderr 分流 + 退出码）
- `shell,pty:` / `shell,v2,pty:` 真伪终端：`/dev/ptmx` 直接开、`setsid` + `TIOCSCTTY` 取得控制终端（^C/^\/^Z 变成 `SIGINT`/`SIGQUIT`/`SIGTSTP`）、`kIdCloseStdin` 用 tty EOF 字符表达半关闭、`kIdWindowSize` 走 `TIOCSWINSZ` 并触发 `SIGWINCH`；pty 不可用时自动降级为管道
- `TERM=` 子服务参数透传到子进程环境；pty 默认 `xterm-256color`，管道默认 `dumb`
- `exec:`（NUL 分隔 argv，不过 shell，参数原样透传）
- `sync:` v1 **只读**：`STAT` / `LIST` / `RECV`，`DATA`…`DONE` 分帧，`QUIT` 收尾
- 未识别服务 → `CLSE` 拒绝

## 未实现（别当真）

- banner 里的机型属性取自 `ADBNG_PRODUCT_*` 环境变量，**不是**读 Android 属性服务
- 新 host 只记录指纹后断开，**没有**本机确认流程
- **`sync:` 的 SEND（push）明确拒绝**：那是 root 级写文件能力，等 M5 的授权层就位再做
- `sync_v2`（`STA2`/`LIS2`/`SND2`）未做，banner 也**没有**宣告 `sendrecv_v2`，老客户端会退回 v1
- `forward:` / `reboot:` / `root:` 仍走拒绝分支
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

回归需要可写目录，默认 `/tmp`；在 Android 上跑时用
`ADBNG_TEST_DIR=/data/local/tmp ./host_tester ...`。

## 测试

`tests/host_tester.c` 是手写的 host 端，用 `socketpair` 驱动 `usbgwd`，不依赖 USB。
当前 15 项回归：

```
[host] PASS handshake + auth
[host] PASS shell: v1
[host] PASS shell: v2 stdout/stderr/exit
[host] PASS shell: v2 300 KB chunked output
[host] PASS shell: v2 stdin
[host] PASS shell: pty is a tty
[host] PASS shell: pty ^C -> SIGINT
[host] PASS shell: pty window size
[host] PASS exec: NUL-separated argv
[host] PASS shell: command not found -> 127
[host] PASS refused service -> CLSE
[host] PASS sync: STAT
[host] PASS sync: LIST
[host] PASS sync: RECV 200 KB
[host] PASS sync: SEND refused (read-only)
[host] RESULT: 15/15 passed
```

pty 三项分别在验证：`tty` 报出 `/dev/pts/N`（管道只会说 "not a tty"）、
`0x03` 变成 `SIGINT`（`sleep 60` 被杀掉、退出码 130；管道下这个字节只是数据）、
以及 `kIdWindowSize` 落到 `TIOCSWINSZ`（`stty size` 报出 31 97）。

`bn_test` 与 python `pow()` 逐字节比对；`sig_tool` 做签/验往返。

## 许可

MIT