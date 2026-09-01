#!/usr/bin/env bash
# Flash a BK7258 board by asking the running firmware to reboot into bootrom.
# Keep operational logs visible in the invoking shell; file logs are secondary.

set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

find_root() {
  local dir=$SCRIPT_DIR

  while [ "$dir" != "/" ]; do
    if [ -x "$dir/bk_loader" ] || [ -x "$dir/tools/bk_loader" ]; then
      printf '%s\n' "$dir"
      return 0
    fi
    dir=$(dirname -- "$dir")
  done
  return 1
}

ROOT=${VELA_ROOT:-$(find_root)}
LOADER=${BK_LOADER:-}
if [ -z "$LOADER" ]; then
  if [ -x "$ROOT/bk_loader" ]; then
    LOADER="$ROOT/bk_loader"
  else
    LOADER="$ROOT/tools/bk_loader"
  fi
fi

DEFAULT_IMAGE="$ROOT/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin"
RUNTIME_DIR=${AUTOFLASH_RUNTIME_DIR:-"$SCRIPT_DIR/logs/runtime"}

PORT=/dev/ttyUSB0
PORTNUM=""
PORTNUM_SET=0
BAUD=115200
CONSOLE_BAUD=115200
IMAGE="$DEFAULT_IMAGE"
ATTACH_SERIAL=0
TEST_ONLY=0

# ------------------------------------------------------------ 软复位的时序常量
#
# 决定这些数值的是 bk_loader 什么时候会再看一眼，而不是复位本身多快：
#
#   bootrom 只在复位后很短时间应答      本文件头部实测 ~473ms
#   bk_loader 的 LinkCheck 节奏         t=0、t≈1.13s 两次快速尝试，之后每 ~15s 一次
#
# 两者相乘的后果：复位落在两次 LinkCheck 之间，这一轮就白费，要等下一次。
#
# 关于复位延迟本身，注意不要照抄 bk7258_reset.c 头部那个 8.26s（CP 的 reboot 走
# NMI + 中断看门狗，2026-08-14 实测）。本机 2026-09-01 实测的握手是 0.24s ~ 2.8s
# 量级，比那个数字快一个数量级，说明当前固件上这条 `reboot` 走的不是那条 8s 的路。
# 窗口按 LinkCheck 节奏定，不按 8.26s 定。

# 每次 reboot 之后的观察窗。看到 Gotten Bus 立刻退出，所以快路径不受它影响，
# 它只决定慢路径上愿意等多久。要够长到能容下一次 ~15s 的 LinkCheck 间隔。
REBOOT_WINDOW=10

# 次数往少调而不是往多调（原值 8）。两种失效不对等：
#   拿不到总线   → 提示手按 RST，板子完好
#   往 bootrom 里插了字节 → 可能擦除完成后写到中途掉链，板子半砖，必须重烧
# 每一次额外写入都是一次把字节插进 bootrom 的机会，所以宁可多提示按 RST。
REBOOT_TRIES=3

# 两条命令之间的最小间隔。
CMD_GAP=0.2

# 逃逸序列之后、reboot 之前的等待，故意比 CMD_GAP 长：CP 在本地消费 Ctrl-] '.'，
# 但控制台归属切换是它 shell 任务里的异步动作，抢在切换前发 reboot 会丢或发错核。
BRIDGE_SETTLE=0.3

# 看到 Getting Bus 之后先静置再动手。0.1s 的轮询粒度不能替代它：轮询只保证
# 第一次写入落在打印之后 0~100ms 的任意点，而 0ms 正是要避开的那一点。
SETTLE_AFTER_GETTING_BUS=0.1

# 每次重试额外加 1..REBOOT_JITTER_MS 毫秒抖动。固定间隔就是相对板子启动时序的
# 固定相位，一次落在坏点上，后面每次都落在同一个坏点上。
REBOOT_JITTER_MS=100

# 第一条 reboot 必须尽快写出去，这里没有可以拖延的余量。
#
# 成功的几次实测：reboot 写在首个 Getting Bus 之后 0.1s 上下，总线在 0.24s ~ 2.8s
# 之间拿到 —— 复位落在 bk_loader 那两次快速尝试的射程里。
#
# 反例也是实测的（2026-09-01 11:30）：这里曾经加过 1s 的"先让 bk_loader 自己拿
# 总线"宽限期，第一条 reboot 因此推迟到 ~1.1s 才写出去，复位落在快速尝试用完之后，
# 下一次 LinkCheck 在 15s 之后 —— 直接错过，整轮没拿到总线。宽限期因此删掉，只留
# 一次零成本的 bus_taken 判定。

LOG=""
FIFO=""
LOADER_PID=""
TEE_PID=""

usage() {
  cat <<'EOF'
用法: autoflash.sh [选项]

  -i <file>    要烧录的镜像，默认使用工程构建出的 all-app.bin
  -p <port>    串口设备，默认 /dev/ttyUSB0
  -n <num>     bk_loader 端口号；默认从 ttyUSB<num> 自动推导
  -b <baud>    烧录波特率，默认 115200
  -B <baud>    固件控制台波特率，默认 115200
  -a           烧录成功后用 picocom 连接串口
  -t           只测试握手，不写 flash
  -h           显示帮助

环境变量: VELA_ROOT、BK_LOADER、AUTOFLASH_RUNTIME_DIR

日志约束: 烧录过程优先在当前 Shell 实时显示，同时保存文件用于追溯；
          不使用仅写文件、完成后再读取的日志方式。
EOF
}

die() {
  echo "错误: $*" >&2
  exit 1
}

cleanup() {
  local rc=$?

  trap - EXIT INT TERM HUP
  if [ -n "$LOADER_PID" ] && kill -0 "$LOADER_PID" 2>/dev/null; then
    kill "$LOADER_PID" 2>/dev/null || true
    wait "$LOADER_PID" 2>/dev/null || true
  fi
  if [ -n "$TEE_PID" ] && kill -0 "$TEE_PID" 2>/dev/null; then
    kill "$TEE_PID" 2>/dev/null || true
    wait "$TEE_PID" 2>/dev/null || true
  fi
  [ -z "$FIFO" ] || rm -f -- "$FIFO"
  exit "$rc"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

while getopts "i:p:n:b:B:ath" opt; do
  case "$opt" in
    i) IMAGE=$OPTARG ;;
    p) PORT=$OPTARG ;;
    n) PORTNUM=$OPTARG; PORTNUM_SET=1 ;;
    b) BAUD=$OPTARG ;;
    B) CONSOLE_BAUD=$OPTARG ;;
    a) ATTACH_SERIAL=1 ;;
    t) TEST_ONLY=1 ;;
    h) trap - EXIT INT TERM HUP; usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

case "$BAUD" in *[!0-9]*|'') die "烧录波特率必须是正整数: $BAUD" ;; esac
case "$CONSOLE_BAUD" in *[!0-9]*|'') die "控制台波特率必须是正整数: $CONSOLE_BAUD" ;; esac
[ "$BAUD" -gt 0 ] || die "烧录波特率必须大于 0"
[ "$CONSOLE_BAUD" -gt 0 ] || die "控制台波特率必须大于 0"

if [ "$PORTNUM_SET" -eq 0 ]; then
  case "$PORT" in
    /dev/ttyUSB[0-9]*) PORTNUM=${PORT#/dev/ttyUSB} ;;
    *) die "无法从 $PORT 推导 bk_loader 端口号，请使用 -n 指定" ;;
  esac
fi
case "$PORTNUM" in *[!0-9]*|'') die "bk_loader 端口号必须是非负整数: $PORTNUM" ;; esac

for command_name in fuser grep mkfifo sha256sum stat stdbuf stty tee timeout tr; do
  command -v "$command_name" >/dev/null 2>&1 || die "缺少命令: $command_name"
done

[ -x "$LOADER" ] || die "找不到 bk_loader: $LOADER"
[ -c "$PORT" ] || die "串口不存在: $PORT（板子插了吗？）"
[ -r "$PORT" ] && [ -w "$PORT" ] || die "无权读写串口: $PORT（检查 dialout 组权限）"
mkdir -p -- "$RUNTIME_DIR" || die "无法创建运行记录目录: $RUNTIME_DIR"

if [ "$TEST_ONLY" -eq 0 ]; then
  [ -f "$IMAGE" ] && [ -r "$IMAGE" ] || die "找不到或无法读取镜像: $IMAGE"
  SIZE=$(stat -c%s -- "$IMAGE") || die "无法读取镜像大小"
  SHA=$(sha256sum -- "$IMAGE" | cut -d' ' -f1) || die "无法计算镜像 SHA256"
  XFER_SECS=$(( (SIZE * 10 + BAUD - 1) / BAUD ))

  # 握手预算单独算进来，不要让它和擦写预算互相挤。
  #
  # 原来的 XFER*2+60 只描述擦写：XFER 随波特率变小，而擦除（实测 ~10s）和每块
  # 命令应答的开销（实测写入 56.5s 对理论线时 28.5s，两倍）都不随波特率缩小。
  # -b 2000000 时它算出 118s，而握手在慢路径上本身就要几十秒 —— 那笔时间以前是
  # 从擦写预算里偷的，一慢就被 timeout 掐在写入中途，把板子留成半砖。
  HANDSHAKE_BUDGET=$(( (REBOOT_TRIES + 1) * REBOOT_WINDOW ))
  OVERALL_TIMEOUT=$(( XFER_SECS * 2 + 60 + HANDSHAKE_BUDGET ))

  echo "=== 待烧录镜像 ==="
  echo "  路径   : $IMAGE"
  echo "  字节数 : $SIZE"
  echo "  SHA256 : $SHA"
  echo "  串口   : $PORT（bk_loader 端口 $PORTNUM）"
  echo "  烧录   : ${BAUD} baud，传输约 ${XFER_SECS}s"
  echo "  超时   : ${OVERALL_TIMEOUT}s（擦写 $(( XFER_SECS * 2 + 60 ))s + 握手 ${HANDSHAKE_BUDGET}s）"
  echo "  控制台 : ${CONSOLE_BAUD} baud"

  # 不校验 all-app.bin 的字节数。分区布局变过几次，写死的期望值只会拦下正常
  # 镜像 —— 对一个只为发现"打包没跑完"的检查来说，这是最糟的失效方式。
  # 字节数和 SHA256 照旧打印，需要确认烧的是刚编的就比对 SHA256。
else
  HANDSHAKE_BUDGET=$(( (REBOOT_TRIES + 1) * REBOOT_WINDOW ))
  OVERALL_TIMEOUT=$(( HANDSHAKE_BUDGET + 20 ))
  echo "=== 只测试握手，不写 flash ==="
  echo "  串口   : $PORT（bk_loader 端口 $PORTNUM）"
  echo "  烧录   : ${BAUD} baud，超时 ${OVERALL_TIMEOUT}s"
  echo "  控制台 : ${CONSOLE_BAUD} baud"
fi

echo
echo "=== 检查串口 ==="
HOLDERS=$(fuser "$PORT" 2>/dev/null | tr -s ' ')
if [ -n "$HOLDERS" ]; then
  echo "  当前占用: $HOLDERS"
  for holder in $HOLDERS; do
    PROCESS_NAME=""
    IFS= read -r PROCESS_NAME < "/proc/$holder/comm" 2>/dev/null || true
    case "$PROCESS_NAME" in
      cat|picocom|minicom|screen|SCREEN)
        echo "  关闭 $PROCESS_NAME（PID $holder）"
        kill "$holder" 2>/dev/null || true
        ;;
    esac
  done
  sleep 0.5
  HOLDERS=$(fuser "$PORT" 2>/dev/null | tr -s ' ')
  [ -z "$HOLDERS" ] || die "串口仍被占用: $HOLDERS，请关闭对应程序后重试"
fi
echo "  串口空闲"

stty -F "$PORT" "$CONSOLE_BAUD" cs8 -cstopb -parenb raw -echo -crtscts \
  || die "stty 配置失败"

RUN_ID=$(date +%Y%m%d-%H%M%S).$$
LOG="$RUNTIME_DIR/autoflash-$RUN_ID.log"
FIFO="$RUNTIME_DIR/autoflash-$RUN_ID.fifo"
: > "$LOG" || die "无法创建日志: $LOG"
mkfifo -- "$FIFO" || die "无法创建输出管道: $FIFO"

if [ "$TEST_ONLY" -eq 1 ]; then
  set -- read -p "$PORTNUM" -b "$BAUD" \
    -f "$RUNTIME_DIR/autoflash-probe-$RUN_ID.bin@0-1000"
else
  set -- download -p "$PORTNUM" -b "$BAUD" -s 0 -i "$IMAGE"
fi

echo
echo "=== 启动 bk_loader（无需手按 RST）==="
# The shell-visible stream is the primary output. tee only adds a durable copy.
echo "  日志将在当前 Shell 实时显示，并同步留档到: $LOG"
tee "$LOG" < "$FIFO" &
TEE_PID=$!
stdbuf -o0 -e0 timeout --foreground "$OVERALL_TIMEOUT" "$LOADER" "$@" > "$FIFO" 2>&1 &
LOADER_PID=$!

GETTING_BUS=0
for _ in $(seq 1 40); do
  if grep -aq "Getting Bus" "$LOG"; then GETTING_BUS=1; break; fi
  kill -0 "$LOADER_PID" 2>/dev/null || break
  sleep 0.1
done

if [ "$GETTING_BUS" -eq 0 ]; then
  if kill -0 "$LOADER_PID" 2>/dev/null; then
    echo "  未检测到握手提示，继续尝试软复位"
  else
    wait "$LOADER_PID"
    RC=$?
    LOADER_PID=""
    wait "$TEE_PID" 2>/dev/null || true
    TEE_PID=""
    die "bk_loader 在进入握手循环前退出（退出码 $RC，日志: $LOG）"
  fi
else
  echo "  bk_loader 已进入握手循环，等 ${SETTLE_AFTER_GETTING_BUS}s 再发软复位"
  sleep "$SETTLE_AFTER_GETTING_BUS"
fi

GOT_BUS=0

# 总线一旦被 bk_loader 拿到，往这个串口再写任何一个字节都会破坏下载协议。
# 所以每一次写入之前都紧贴一次判定，没有例外 —— 包括下面那条直接软复位。
#
# 曾经的写法是：检测到 Getting Bus 就无条件写一次 reboot。板子只要本来就停在
# bootrom（任何一次失败的烧录都会把它留在那儿），bk_loader 0.2s 就拿到总线，
# 那 10 个字节就直接落进了正在进行的下载。实测那一次：擦除 13.2s 时 bk_loader
# 自己发 do_reset_signal 重新握手，整轮报废。
bus_taken() {
  grep -aq "Gotten Bus" "$LOG"
}

# 观察窗：看到 Gotten Bus 立刻退出；bk_loader 死了也退出。
# 返回 0 表示拿到总线。
wait_for_bus() {
  local deadline=$(( $1 * 10 ))
  local _

  for _ in $(seq 1 "$deadline"); do
    if bus_taken; then GOT_BUS=1; return 0; fi
    kill -0 "$LOADER_PID" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

if bus_taken; then
  GOT_BUS=1
  echo "  bk_loader 已经拿到总线（板子本来就停在 bootrom），不发任何软复位"
else
  # 两个 shell 都认 reboot。AP 的实现直接捅 AON 看门狗，立刻复位，所以先试当前
  # 归属者，省掉 AP→CP 那次异步交接。
  printf '\r\nreboot\r\n' > "$PORT" 2>/dev/null || true
  echo "  已向当前控制台发送直接软复位，观察 ${REBOOT_WINDOW}s"
  wait_for_bus "$REBOOT_WINDOW" || true
fi

for i in $(seq 1 "$REBOOT_TRIES"); do
  [ "$GOT_BUS" -eq 0 ] || break
  if bus_taken; then GOT_BUS=1; break; fi
  kill -0 "$LOADER_PID" 2>/dev/null || break

  # AP shell 不响应时的回退路径：先逃逸回 CP shell，再发 reboot。
  # 每个 printf 前面都有一次 bus_taken 判定，因为这一串要花 0.5s，总线完全可能
  # 在中途被拿到 —— 之前正是在这半秒里把字节写进了刚起来的 bootrom。
  printf '\035' > "$PORT" 2>/dev/null || true
  sleep "$CMD_GAP"
  if bus_taken; then GOT_BUS=1; break; fi
  printf '.' > "$PORT" 2>/dev/null || true
  sleep "$BRIDGE_SETTLE"
  if bus_taken; then GOT_BUS=1; break; fi
  printf '\r\nreboot\r\n' > "$PORT" 2>/dev/null || true
  echo "  第 $i 次 CP 回退软复位已发出，观察 ${REBOOT_WINDOW}s"

  wait_for_bus "$REBOOT_WINDOW" && break

  JITTER_MS=$(( RANDOM % REBOOT_JITTER_MS + 1 ))
  sleep "0.$(printf '%03d' "$JITTER_MS")"
done

if [ "$GOT_BUS" -eq 1 ]; then
  echo "  握手成功"
else
  echo "  软复位未拿到总线，请手动按一次 RST；bk_loader 仍在等待"
fi

wait "$LOADER_PID"
RC=$?
LOADER_PID=""
wait "$TEE_PID"
TEE_RC=$?
TEE_PID=""
rm -f -- "$FIFO"
FIFO=""

echo
echo "=== 结果 ==="
if [ "$RC" -eq 0 ] && [ "$TEE_RC" -eq 0 ] \
    && grep -aq "All Finished Successfully" "$LOG"; then
  echo "  操作成功"
  grep -aE "Total Test Time|EraseFlash ->pass" "$LOG" \
    | tail -3 | sed 's/^/    | /'
else
  echo "  操作失败（bk_loader: $RC，日志输出: $TEE_RC）"
  LAST_PCT=$(tr '\r' '\n' < "$LOG" \
    | grep -aoE "Writing Flash \.\.\. [0-9]+%" | tail -1)
  if [ -n "$LAST_PCT" ]; then
    echo "  flash 已擦除但只写到 $LAST_PCT，必须重新完整烧录才能恢复"
  elif [ "$RC" -eq 124 ]; then
    echo "  操作被超时中止（上限 ${OVERALL_TIMEOUT}s）"
  fi
  echo "  完整日志: $LOG"
  exit 1
fi

if [ "$BAUD" != "$CONSOLE_BAUD" ]; then
  stty -F "$PORT" "$CONSOLE_BAUD" cs8 -cstopb -parenb raw -echo -crtscts \
    2>/dev/null || true
  echo "  串口已恢复为控制台的 ${CONSOLE_BAUD} baud"
fi

echo "  完整日志: $LOG"
echo
echo "Tips: 连接串口后执行软重启，可获取完整启动日志。"

if [ "$ATTACH_SERIAL" -eq 1 ]; then
  command -v picocom >/dev/null 2>&1 || die "找不到 picocom"
  echo "=== 连接串口（Ctrl-A Ctrl-X 退出）==="
  sleep 1
  trap - EXIT INT TERM HUP
  exec picocom -b "$CONSOLE_BAUD" --flow n --parity n --databits 8 "$PORT"
fi

echo "  连接命令: picocom -b $CONSOLE_BAUD --flow n --parity n --databits 8 $PORT"
