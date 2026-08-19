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

## 第一次用：先改一行

脚本顶部有一行工作区根目录，**换机器必须改**：

```bash
# ############################################################################
# 改这一行：你的工作区根目录（里面应有 tools/ 和 bk_avdk_smp/）
# ############################################################################
ROOT=/home/mi/vela_competition_ap_console_dev
```

它派生出两个路径 —— `$ROOT/tools/bk_loader`（烧录器）和 `$ROOT/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin`（默认镜像）。

不想改文件就用环境变量：

```bash
VELA_WS=/your/workspace ./autoflash.sh -a
```

ROOT 不对时报错会带上当前值和该改哪里，不会只丢一句"文件不存在"。

## 参数

| 参数 | 含义 |
|---|---|
`-i <file>` | 要烧的镜像，默认 `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin` |
`-p <port>` | 串口设备，默认 `/dev/ttyUSB0` |
`-n <num>` | `bk_loader` 的端口号，默认 `0` |
`-b <baud>` | **烧录**波特率，默认 `115200`。只影响下载速度 |
`-B <baud>` | **控制台**波特率，默认 `115200`。固件编译期定死，一般别动 |
`-s` | 跳过 `all-app.bin` 字节数校验 |
`-a` | 烧完自动挂 `screen` 看日志 |
`-t` | 只测握手，用 `read` 代替 `download`，不碰 flash |
`-h` | 显示帮助 |

环境变量 `VELA_WS=/path/to/workspace` 可覆盖脚本里写的 `ROOT`。

## 它替你做的五件事

1. **释放串口** — 自动关掉 detached 的 `screen` 会话和残留 `cat`。这是最高频的失败原因
2. **烧前校验** — 打印 SHA256；当前 `2992K` AP 分区产品包必须为
   `4,526,080` 字节，打包不完整直接拒绝。分区变化后用
   `AUTOFLASH_EXPECTED_SIZE` 更新校验值。
3. **软复位** — 最多 8 次，每次先发 AP 控制台逃逸序列再发 `reboot`
4. **实时进度条** — 擦除和写入两条进度条照常滚动
5. **恢复串口速率** — 高速烧录后把串口拨回控制台波特率

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

**拿到总线后立刻停止写入** — 每 100 ms 轮询日志里的 `Gotten Bus`，一出现就停。之后再往端口写任何字节都会破坏下载协议。

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
sg dialout -c "<repo>/.claude/skills/autoflash/autoflash.sh -b 1500000 -a"
```

打包后建议核对三处，确认烧的是刚编的：

```bash
cmp contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin bk_avdk_smp/build/openvela-ap.bin
cmp contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
stat -c%s bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin   # 当前必须 4526080
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
