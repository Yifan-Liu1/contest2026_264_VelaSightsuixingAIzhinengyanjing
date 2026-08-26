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
# The package size changes when the AP partition layout changes.  Keep a
# product-build default, while allowing the build flow to override it.
EXPECTED_SIZE=${AUTOFLASH_EXPECTED_SIZE:-5709824}

PORT=/dev/ttyUSB0
PORTNUM=""
PORTNUM_SET=0
BAUD=115200
CONSOLE_BAUD=115200
IMAGE="$DEFAULT_IMAGE"
REBOOT_TRIES=8
ATTACH_SERIAL=0
SKIP_SIZE_CHECK=0
TEST_ONLY=0

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
  -s           跳过 all-app.bin 字节数校验
  -a           烧录成功后用 picocom 连接串口
  -t           只测试握手，不写 flash
  -h           显示帮助

环境变量: VELA_ROOT、BK_LOADER、AUTOFLASH_RUNTIME_DIR、
  AUTOFLASH_EXPECTED_SIZE 可覆盖当前产品包大小校验（当前默认 5709824）。

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

while getopts "i:p:n:b:B:sath" opt; do
  case "$opt" in
    i) IMAGE=$OPTARG ;;
    p) PORT=$OPTARG ;;
    n) PORTNUM=$OPTARG; PORTNUM_SET=1 ;;
    b) BAUD=$OPTARG ;;
    B) CONSOLE_BAUD=$OPTARG ;;
    s) SKIP_SIZE_CHECK=1 ;;
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
  OVERALL_TIMEOUT=$(( XFER_SECS * 2 + 60 ))

  echo "=== 待烧录镜像 ==="
  echo "  路径   : $IMAGE"
  echo "  字节数 : $SIZE"
  echo "  SHA256 : $SHA"
  echo "  串口   : $PORT（bk_loader 端口 $PORTNUM）"
  echo "  烧录   : ${BAUD} baud，传输约 ${XFER_SECS}s，超时 ${OVERALL_TIMEOUT}s"
  echo "  控制台 : ${CONSOLE_BAUD} baud"

  if [ "$SKIP_SIZE_CHECK" -eq 0 ] && [ "$(basename -- "$IMAGE")" = "all-app.bin" ]; then
    [ "$SIZE" -eq "$EXPECTED_SIZE" ] \
      || die "all-app.bin 应为 $EXPECTED_SIZE 字节，实际 $SIZE；用 -s 可跳过检查"
    echo "  字节数校验通过"
  fi
else
  OVERALL_TIMEOUT=40
  echo "=== 只测试握手，不写 flash ==="
  echo "  串口   : $PORT（bk_loader 端口 $PORTNUM）"
  echo "  烧录   : ${BAUD} baud"
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
  echo "  bk_loader 已进入握手循环，开始发送软复位"
fi

GOT_BUS=0

# Both current shells accept reboot.  The AP implementation resets the whole
# chip directly through the AON watchdog, so try the current owner first and
# avoid the asynchronous AP-to-CP console hand-off when it is unnecessary.
printf '\r\nreboot\r\n' > "$PORT" 2>/dev/null || true
echo "  已向当前控制台发送直接软复位"
for _ in $(seq 1 15); do
  if grep -aq "Gotten Bus" "$LOG"; then GOT_BUS=1; break; fi
  kill -0 "$LOADER_PID" 2>/dev/null || break
  sleep 0.1
done

for i in $(seq 1 "$REBOOT_TRIES"); do
  [ "$GOT_BUS" -eq 0 ] || break
  if grep -aq "Gotten Bus" "$LOG"; then GOT_BUS=1; break; fi
  kill -0 "$LOADER_PID" 2>/dev/null || break

  # Fallback for an unresponsive AP shell.  The CP consumes Ctrl-] '.' locally,
  # but owner switching runs asynchronously in its shell task.  Wait for that
  # transition before sending reboot; the previous 50 ms delay could race the
  # bridge and lose or misroute the command.
  printf '\035' > "$PORT" 2>/dev/null || true
  sleep 0.05
  printf '.' > "$PORT" 2>/dev/null || true
  sleep 0.3
  printf '\r\nreboot\r\n' > "$PORT" 2>/dev/null || true
  echo "  第 $i 次 CP 回退软复位已发出"

  # CP's own `reboot` takes the NMI/interrupt-watchdog path, measured at
  # ~8.37s before the part actually resets (see
  # board/beken/chips/bk7258/bk7258_reset.c for the measurement writeup).
  # A 1s observation window was too short to ever see that path complete
  # before sending another reboot; 5s gives it room to land within a
  # couple of rounds instead of only via many short, overlapping retries.
  for _ in $(seq 1 50); do
    if grep -aq "Gotten Bus" "$LOG"; then GOT_BUS=1; break; fi
    kill -0 "$LOADER_PID" 2>/dev/null || break
    sleep 0.1
  done
  [ "$GOT_BUS" -eq 0 ] || break
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
