# adbng

自写用户态 ADB **设备端**实现。目标：PC 上用原厂 `adb` 直接连上本机，不经过系统 `adbd`。

> 状态：M4（USB FunctionFS 传输层）已在本机 gadget 上绑定成功，端到端（ENABLE / 真主机）尚未验证；
> M2 + M3 + M5a（sync 读写双向）+ M5b（配对授权挂起）proot 与真机各 17/17 回归通过。
> 管理器（开 / 关、gadget 生命周期、配对 UI）未做。**当前不是可发布形态。**

## 已实现（真的）

- 24 字节 ADB 帧格式，`magic = command ^ 0xFFFFFFFF`，超长包（> `A_MAXDATA`）拒绝
- `CNXN` 双向 banner 交换
- `AUTH`：`A_SIGNATURE`（RSA-2048 / SHA-1 / PKCS#1 v1.5 验签），3 轮上限；
  未知 host 的 `A_RSAPUBLICKEY` 不再直接断开，而是**挂起等本机决定**（M5b，见下）
- 通道分配 / `OKAY` / `WRTE` / `CLSE` 语义
- `adb_keys` 格式解析（自写 bignum + SHA-1 + base64，无 openssl 依赖）
- 单线程 `poll` 事件循环，每通道双向流控（收 WRTE 回 OKAY，发 WRTE 等 OKAY）
- `shell:` v1（stdout/stderr 合并，无退出码）
- `shell,v2,raw:` 子包格式（stdout/stderr 分流 + 退出码）
- `shell,pty:` / `shell,v2,pty:` 真伪终端：`/dev/ptmx` 直接开、`setsid` + `TIOCSCTTY` 取得控制终端（^C/^\/^Z 变成 `SIGINT`/`SIGQUIT`/`SIGTSTP`）、`kIdCloseStdin` 用 tty EOF 字符表达半关闭、`kIdWindowSize` 走 `TIOCSWINSZ` 并触发 `SIGWINCH`；pty 不可用时自动降级为管道
- `TERM=` 子服务参数透传到子进程环境；pty 默认 `xterm-256color`，管道默认 `dumb`
- `exec:`（NUL 分隔 argv，不过 shell，参数原样透传）
- `sync:` v1：`STAT` / `LIST` / `RECV`，`DATA`…`DONE` 分帧，`QUIT` 收尾
- `sync:` **SEND（push，M5a）**：`path,mode`（mode 十进制，取 `&07777`）建文件 → 每帧 `DATA` 落盘后回 `OKAY` → `DONE` 的 mtime 用 `futimens` 落时间戳再回 `OKAY`；失败一律 `FAIL` 带 `strerror`
- SEND 默认**关**：只有 `ADBNG_WRITE_ROOTS=/a:/b` 或 `/data/adb/adbng/write_roots.conf`（每行一个前缀，`#` 注释）里列出的目录前缀可写；路径先做文本归一化（折叠 `..`/`.`），逐个前缀匹配，不在白名单内直接 `FAIL`
- 传输层抽象成 `struct link_ops`（`init`/`pollfds`/`on_event`/`send_frame`/`out_fd`/`flush`/`shutdown`），
  `--fd N`（socketpair，测试用）与 `--ffs DIR`（USB）二选一，切换不动协议层
- FunctionFS 后端：写 adb 描述符（v1 头，FS/HS 两套，class `ff/42/01`，ep1 IN / ep2 OUT，HS 512 字节）
  **以及必须跟上的空 strings 表**；ep0 事件机（BIND/UNBIND/ENABLE/DISABLE/SETUP/SUSPEND/RESUME）；
  `ENABLE` 后返回 2 让 daemon 重发 CNXN/AUTH；发送队列带 compaction，`EAGAIN` 转 POLLOUT
- `tools/usb-gadget.sh`：`install` / `add` / `del` / `remove` / `status`（`ADBNG_GADGET`/`ADBNG_UDC`/`ADBNG_FUNC`/`ADBNG_SLOT` 可覆盖）
- 未识别服务 → `CLSE` 拒绝

## 未实现（别当真）

- banner 里的机型属性取自 `ADBNG_PRODUCT_*` 环境变量，**不是**读 Android 属性服务
- 配对授权没有 UI：usbgwd 只负责挂起并写请求文件，真正的“允许 / 拒绝”由尚未写出来的管理器完成
- 授权是**一问一答**：同一时刻只挂起一个 host，第二个未知 host 到来会替换掉前一个请求
- SEND 的白名单是**前缀匹配**，不解析符号链接：白名单内的软链可以把写落在名单外（对 root 客户端只是护栏，不是沙箱）
- `sync:` 的 SEND 没有断点续传 / 原子替换（`O_TRUNC` 直写），失败时目标文件可能已是半截
- `sync_v2`（`STA2`/`LIS2`/`SND2`）未做，banner 也**没有**宣告 `sendrecv_v2`，老客户端会退回 v1
- `forward:` / `reboot:` / `root:` 仍走拒绝分支
- **FunctionFS 端到端未验证**：本机 UDC `a600000.dwc3` 当前 `state = not attached`（没有主机连接），
  描述符被内核接受、gadget 绑定成功、ep1/ep2 已建立，但 `ENABLE`（主机 `set_configuration`）这条路径没跑过
- FunctionFS 要**顶替** `ffs.adb`，不能和系统 `adbd` 并列：两者都宣告 interface 0，
  系统 USB 栈（`sys.usb.config=adb`）会把 `function0` 抢回去
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
当前 17 项回归：

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
[host] PASS sync: SEND outside roots denied
[host] PASS sync: SEND 200 KB push
[host] PASS auth: pending allow + deny
[host] RESULT: 17/17 passed
```

pty 三项分别在验证：`tty` 报出 `/dev/pts/N`（管道只会说 "not a tty"）、
`0x03` 变成 `SIGINT`（`sleep 60` 被杀掉、退出码 130；管道下这个字节只是数据）、
以及 `kIdWindowSize` 落到 `TIOCSWINSZ`（`stty size` 报出 31 97）。

`bn_test` 与 python `pow()` 逐字节比对；`sig_tool` 做签/验往返。

最后一项跑两个一次性 daemon（各自 scratch 状态目录）：一个走“允许”，
验证 `pending/<fp>.req` 被写出、decision 落地后密钥进了 `adb_keys` 且**重新加载**、
随后签名直接通过并拿到 `CNXN`；另一个走“拒绝”，验证链路被断开、`devices.tsv` 记了 `deny`、
请求文件被清掉。

## 配对授权（M5b）

```
host: AUTH(RSAPUBLICKEY)  ← 未知 key
usbgwd: 写 /data/adb/adbng/pending/<fp>.req，回 AUTH(TOKEN) 继续吊住
        （host 会不停重试签名，每次都被回 TOKEN）
manager: 写 /data/adb/adbng/pending/<fp>.decision = "allow" | "deny"
usbgwd: allow → 追加到 adb_keys + 重新加载 → host 下一次签名就过
        deny  → 记 devices.tsv 并断开
        120s 没决定 → 断开，**不记录**（用户没做选择）
host 中途拔掉 → .req/.decision 清掉，不记录
```

`<fp>.req` 是 `key=value` 三行（`fingerprint` / `host` / `since`），故意不用 JSON：
管理器 split 一下就能读。`devices.tsv` 每条决定一行：
`<epoch>	<fp>	<allow|deny>	<user@host>`。

状态目录可用 `ADBNG_STATE_DIR` 覆盖（默认 `/data/adb/adbng`），回归就是靠它把状态指到临时目录。
首次安装 `adb_keys` 为空也能启动：不再当致命错误，直接进配对流程。

## 生命周期管理（tools/adbngctl.sh）

`usbgwd` 只管协议，开关、gadget、看门狗、配对决策都在这个脚本里，管理器 app 调 `su -c` 执行。

```
adbngctl start      先 ctl.stop adbd，再复查一遍，过了才 install/起 daemon/add
adbngctl stop       停 daemon、解绑；**不会**把系统 adb 拉回来
adbngctl status     key=value，给 app 直接拆
adbngctl ensure     开机路径：只在 enabled=1 时 start
adbngctl allow <fp> / deny <fp> / pending / devices
```

状态都在 `$ADBNG_STATE_DIR`（默认 `/data/adb/adbng`）：
`enabled`（开关持久化，开机据此恢复）、`adb_keys`、`pending/`、`devices.tsv`、
`adbng.log`、`usbgwd.pid`、`watchdog.pid`、`takeover.fails`。

几个已钉死的规则：

- **拒绝对抗**：start 先杀 adbd 再复查（`pidof adbd` 为空、活动配置里没有指向 `ffs.adb` 的链接），
  任何一步不过就输出“adbng 不可用，请使用 adbhide”，不去抢 UDC。
  注意 adb 的函数不一定是 `function0`（本机 `function0` 是 `ffs.mtp`），所以是扫全部 `function*` 链接而不是猜槽位。
- **顺序**：`install`（挂 functionfs）→ 起 daemon（写描述符和 strings）→ `add`（链接并绑定 UDC）。
  反过来 configfs 会拒掉未初始化的函数。
- **看门狗**：每 5 秒盯一次；daemon 掉了就重启；发现 adbd 回来了就停掉它、重新 `del`+`add` 抢回 UDC。
  连续 3 次失败就**认输**：写 `enabled=0`、停掉 adbng、发一条通知，不再让 USB 来回抖。
- **stop 不恢复 adb**：用户关掉 adbng 就是关掉调试，不替用户做决定。

## USB（M4）踩过的三个坑

调试记录，省得下次再撞：

1. **`ln -s` 进 `configs/b.1/` 报 `Invalid argument`** —— 不是路径写错，是 gadget **已经绑在 UDC 上**。
   `config_usb_cfg_link()` 在绑定态直接返回 `-EINVAL`。正确顺序是 `echo "" > UDC` → `ln -s` → `echo <udc> > UDC`。
   MIUI 的 USB 栈会在几十毫秒内把 UDC 抢回去重绑，所以这三步要连在一起做（脚本里有重试）。
2. **符号链接目标要用绝对路径** —— configfs 解析目标时按**进程 cwd**，不是按链接所在目录；
   写相对路径会出现 `No such file or directory`。`readlink` 事后看到的
   `../../../../usb_gadget/g1/functions/...` 是 configfs 自己归一化出来的，不代表你可以那样创建。
3. **只写描述符不够，还必须写 strings 表** —— 否则 ffs 实例停在 `FFS_READ_STRINGS`，
   `desc_ready` 永远是 false，`composite_bind()` 让整个 gadget 以 `-ENODEV`（dmesg: `failed to start g1: -19`）失败，
   而且 `ep1`/`ep2` 也不会出现。我们 `iInterface = 0`，所以写一个 `str_count = lang_count = 0` 的 16 字节空表就够。
   另外 gadget 每次 UNBIND 后内核会 `ffs_data_reset()` 丢掉描述符集，重绑前得重新写。

## 许可

MIT