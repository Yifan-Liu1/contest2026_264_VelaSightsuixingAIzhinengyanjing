---
name: autoflash
description: "Flash a BK7258 board without pressing RST by hand. Use when: 烧录 BK7258, flash BK7258, autoflash, all-app.bin, bk_loader, Getting Bus, Gotten Bus, 手动按 RST, 免按复位, 自动烧录, 串口被占用, Device or resource busy, screen is terminating, 烧完打字没反应, 烧录进度条, 烧录卡住, 烧录超时, 波特率 1500000, dev/ttyUSB0."
---

# autoflash — BK7258 免按 RST 自动烧录

`bk_loader` 只能靠拨 DTR/RTS 来复位板子，而这块板**没有自动复位电路**，所以原始流程需要人在 `Getting Bus...` 循环期间手按 RST。

`autoflash.sh` 用软件复位替代按键：在 `bk_loader` 握手循环期间，从另一个进程往同一个串口写 `reboot`，让还在运行的固件自己复位，正好撞上 bootrom 的监听窗口。

## 快速开始

```bash
sg dialout -c "./autoflash.sh -a"                 # 烧默认镜像，烧完自动挂串口
sg dialout -c "./autoflash.sh -b 1500000 -a"      # 高速烧录，40 秒而不是 4 分钟
sg dialout -c "./autoflash.sh -t"                 # 只测握手，不写 flash
```

需要 `sg dialout`（串口属 dialout 组）。

## 实现只有一份，在仓库根目录

`<repo>/autoflash.sh` 是唯一的实现。`.claude/skills/autoflash/autoflash.sh` 只是转发过去的壳子 —— 这里以前放着第二份实现，两份在时序处理上分叉到行为和日志格式都不一样，排查时先得发现"跑的不是这一份"，所以合并了。

不用改任何路径：脚本从自身位置往上走，找到含 `bk_loader` 的目录当作工作区根，再由它派生默认镜像 `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin`。要覆盖就用环境变量：

```bash
VELA_ROOT=/your/workspace ./autoflash.sh -a       # 工作区根
BK_LOADER=/path/to/bk_loader ./autoflash.sh       # 直接指定烧录器
AUTOFLASH_RUNTIME_DIR=/tmp/flashlogs ./autoflash.sh   # 运行日志目录
```

日志实时打在当前 Shell，同时留档到 `<repo>/logs/runtime/autoflash-<时间戳>.log`。

## 参数

| 参数 | 含义 |
|---|---|
`-i <file>` | 要烧的镜像，默认 `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin` |
`-p <port>` | 串口设备，默认 `/dev/ttyUSB0` |
`-n <num>` | `bk_loader` 的端口号，默认从 `ttyUSB<num>` 自动推导 |
`-b <baud>` | **烧录**波特率，默认 `115200`。只影响下载速度 |
`-B <baud>` | **控制台**波特率，默认 `115200`。固件编译期定死，一般别动 |
`-a` | 烧完自动用 `picocom` 连上串口（`Ctrl-A Ctrl-X` 退出）|
`-t` | 只测握手，用 `read` 代替 `download`，不碰 flash |
`-h` | 显示帮助 |

## 它替你做的五件事

1. **检查串口** — 占用者是 `cat` / `picocom` / `minicom` / `screen` 就自动关掉。这是最高频的失败原因
2. **烧前信息** — 打印字节数和 SHA256，但**不断言**字节数。分区大小变过几次，
   写死的期望值只会拦下正常镜像。要确认烧的是刚编的，比对 SHA256 或用下面
   「完整流程」里的 `cmp`。
3. **软复位** — 先向当前控制台发一次 `reboot`，不行再最多 3 次「逃逸回 CP shell
   再 `reboot`」；每次写串口之前都重新判定一次总线是否已被拿到，看到
   `Getting Bus` 后先等 0.1s，命令之间隔 0.2s，重试之间等 10s 加 1~100ms 抖动
4. **实时进度条** — 擦除和写入两条进度条照常滚动，同时留档到 `logs/runtime/`
5. **恢复串口速率** — 高速烧录后把串口拨回控制台波特率

### 为什么写串口之前必须重新判定

总线一旦被 `bk_loader` 拿到，往串口再写任何字节都会插进下载协议。老版本在检测到
`Getting Bus` 后**无条件**写一次 `reboot`，而板子只要本来就停在 bootrom（任何一次
失败的烧录都会把它留在那儿），`bk_loader` 0.2s 就拿到总线，那 10 个字节就落进了
正在进行的下载。

判定也不是万能的：日志里的 `Gotten Bus` 是既成事实，握手进行中的那 0.2s 里查到的
仍然是"没拿到"。所以除了逐次判定，次数也从 8 降到 3 —— 每一次额外写入都是一次
把字节插进 bootrom 的机会，而两种失效并不对等：拿不到总线只是提示手按 RST，板子
完好；写坏链路则可能擦除完成后写到中途掉链，板子半砖必须重烧。

## 波特率：两个独立的概念

**这是最容易搞错的地方。** `-b` 和控制台速率是两件事：

- `-b 1500000` 只让 `bk_loader` 传得快，2.6 MB 从 4 分钟降到 40 秒
- 固件控制台**始终**是 defconfig 里定死的 115200

把 `-b` 的值用在烧完后的 `screen` 上，你会看到乱码且打字无反应。脚本内部严格分开这两个变量，并在结束时把串口拨回控制台速率。

CH340 的上限是 2 Mbps —— `stty` 会直接拒绝 `6000000`，那个数字只是 `bk_loader` 的参数范围。

## 前置条件

| 条件 | 检查 |
|---|---|
串口存在 | `ls /dev/ttyUSB*` |
在 dialout 组 | `id \| grep dialout`，不在就 `sudo usermod -aG dialout $USER` 然后**重新登录** |
镜像已打包 | 脚本**不编译**。改了代码要先走编译 + 打包两步 |
固件还活着 | 软复位依赖固件能响应 `reboot`。固件崩死时仍需手按 RST |

## 常见问题

**提示要手动按 RST** — 软复位 8 次没拿到总线。按一次 RST 然后**彻底松手**，`bk_loader` 循环还在跑，按下去就接上。连按会在 `Gotten Bus`/`Getting Bus` 之间反复跳，永远进不了擦除。

**`Device or resource busy`** — 有进程占着串口。脚本会自动清，清不掉会明确报出占用者 PID。手动查：

```bash
screen -ls                                    # 有没有 detached 会话
sg dialout -c "fuser -v /dev/ttyUSB0" 2>&1    # 表格输出走 stderr，必须 2>&1
```

`lsof` 在非 root 下查不出串口占用，别用它下判断。

**`screen` 一开就 `[screen is terminating]`** — 有 detached 会话攥着串口。`Ctrl-A D` 是**分离**，会话继续占串口；要释放必须 `Ctrl-A K` 再按 `y`。

**烧完打字没反应** — 串口还停在烧录波特率。拨回来：

```bash
sg dialout -c "stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb raw -echo -crtscts"
```

**烧到中途被掐断** — flash 已擦除但只写了一部分，板子起不来，且此时固件不响应 `reboot`。必须手按 RST 重烧一次完整的。脚本的超时按镜像大小自动计算（传输时间 ×2 + 60 秒），不会再出现这种情况。

## 为什么这样能行（关键实测事实）

写这个脚本时踩过的坑，都是靠实测而非推理定下来的：

**DTR/RTS 没接复位电路** — 分别脉冲两条线，各收到 0 字节。`bk_loader` 里确实有 `SetDTR`/`ClearRTS`/`do_reset_signal`，但在这块板上无效。

**bootrom 窗口约 473 ms** — 发 `reboot` 后 0.005s→0.478s 是静默期，1.04 s 就进 `nsh>`。窗口够宽。

**`bk_loader` 始终在 115200 握手** — 即使传 `-b 1500000`，日志也报 `BaudRate : 115200`，握手成功后才升速。所以循环期间写 `reboot` 总是有效的，与 `-b` 无关。

**`bk_loader` 不独占 tty** — 没设 `TIOCEXCL`，所以第二个进程能往同一端口写。

**转义序列后必须加换行** — CP shell 按行解析。`\x1d` `.` `reboot\r\n` 三次分开写但中间没换行，会被拼成 `.reboot` 这个不存在的命令，板子根本不重启。加 `\r\n` 把那行冲掉才行。

**拿到总线后立刻停止写入** — 每 100 ms 轮询日志里的 `Gotten Bus`，一出现就停，并且每次写串口之前再单独判定一次。之后再往端口写任何字节都会破坏下载协议。

**`bk_loader` 的 LinkCheck 节奏是 `t=0`、`t≈1.13s`，之后每 ~15s 一次** — 2026-09-01 实测。配上 bootrom 只应答约 473ms，意味着复位必须落在某次 LinkCheck 上，错过就要再等十几秒。所以第一条 `reboot` 要尽早写出去：在这里加过 1s 的延迟，复位就落在两次 LinkCheck 之间，整轮拿不到总线。

**别照抄 8.26s 那个复位延迟** — `bk7258_reset.c` 头部记的是 2026-08-14 CP 走 NMI + 中断看门狗的测量值。本机 2026-09-01 实测握手是 0.24s ~ 2.8s 量级，当前固件上这条 `reboot` 走的不是那条 8s 的路。

## 完整流程（改了代码之后）

脚本只烧不编译，所以完整链条是三步：

```bash
# 1. 编译 AP 核。nsh 仅是最小对照配置；VelaSight 产品必须使用 ai_agent。
cd <workspace>/contest
export PATH=$PWD/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake -j8

# 2. 打包
cd <workspace>
cp contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin bk_avdk_smp/build/openvela-ap.bin
cd bk_avdk_smp
sg docker -c "./dbuild.sh make -C projects/app_ab bk7258 SDK_DIR=/armino EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin"

# 3. 烧录
sg dialout -c "<repo>/autoflash.sh -b 2000000 -a"
```

打包后建议核对三处，确认烧的是刚编的：

```bash
cmp contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin bk_avdk_smp/build/openvela-ap.bin
cmp contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
stat -c%s bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin   # 只记录大小，脚本不再断言
```

## 板上两个 shell

串口上挂着两个 shell，提示符不同：

| 提示符 | 是谁 |
|---|---|
`$` | CP 核（armino），串口的直接归属者 |
`nsh>` | AP 核（OpenVela），经 mailbox 桥接 |

```
$ ap_console open          进入 AP 控制台
Ctrl-] 松手再按 .           退回 CP shell
```

`reboot` **必须发给 CP shell**，AP 那边不管重启 —— 这也是脚本发 `reboot` 前先发逃逸序列的原因。`ap_cmd` 在真板上不工作（`bridge tx=0`），要用 `ap_console open` 透传。
