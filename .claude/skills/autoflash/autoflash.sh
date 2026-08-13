#!/bin/bash
# autoflash.sh -- flash the BK7258 board without pressing RST by hand.
#
# How it works
# ------------
# bk_loader resets the board by pulsing DTR/RTS (CCommPort::do_reset_signal),
# but this board has no auto-reset circuit on those lines -- measured: pulsing
# either one produces zero bytes.  That is why a human has to press RST while
# bk_loader is looping on "Getting Bus...".
#
# The bootrom only answers the LinkCheck handshake for a short window after a
# reset.  Measured on this board: issuing "reboot" over the CP shell gives
# about 473 ms of silence (0.005 s -> 0.478 s) before firmware output starts,
# and the shell prompt is back at 1.04 s.  So the window is wide enough.
#
# bk_loader does not take TIOCEXCL on the tty, so a second process can write
# into the same port while bk_loader is looping.  Writing "reboot" there makes
# the running firmware reset itself -- exactly what pressing RST does, only
# scripted.  Verified: "Gotten Bus..." arrives ~0.5 s after the write.
#
# IMPORTANT: once the bus is taken, anything else written to the port would
# corrupt the protocol, so the reboot loop stops the moment "Gotten Bus"
# appears in the log.

set -u

# ############################################################################
# 改这一行：你的工作区根目录（里面应有 tools/ 和 bk_avdk_smp/）
# ############################################################################
ROOT=/home/mi/vela_competition_ap_console_dev

# 也可以不改文件，临时用环境变量覆盖：
#   VELA_WS=/your/workspace ./autoflash.sh
ROOT="${VELA_WS:-$ROOT}"

# 下面两个路径由 ROOT 派生，通常不用改。
LOADER="$ROOT/tools/bk_loader"
DEFAULT_IMAGE="$ROOT/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin"

# app_ab's partition layout for BK7258 is a fixed size, so a differently sized
# all-app.bin means the packaging step did not finish.
EXPECTED_SIZE=2646016

PORT=/dev/ttyUSB0
PORTNUM=0

# Two independent baud rates, do not conflate them.
#
# BAUD is only how fast bk_loader talks to the bootrom.  CONSOLE_BAUD is what
# the firmware's console runs at, which is fixed by the build (defconfig), so
# raising the flash speed must not change it.  Using one variable for both
# meant that after "-b 1500000" the post-flash screen opened at 1500000 and
# nothing typed into it worked.
BAUD=115200
CONSOLE_BAUD=115200
IMAGE="$DEFAULT_IMAGE"
LOG=/tmp/autoflash.log
REBOOT_TRIES=8
REBOOT_INTERVAL=1.5

# Overall timeout is derived from the image size, never hard-coded.
#
# A fixed 180 s was wrong and actively harmful: 2,646,016 bytes at 115200 baud
# is 10 bits per byte on the wire, i.e. ~230 s of writing alone, plus ~10 s of
# 64K erase.  timeout(1) killed a real flash at "Writing Flash ... 69%",
# leaving the board with an erased-but-incomplete image.  Computed below as
# transfer time x 2 plus 60 s of headroom.
OVERALL_TIMEOUT=0
ATTACH_SERIAL=0
SKIP_SIZE_CHECK=0
TEST_ONLY=0

usage() {
  cat <<'EOF'
用法: autoflash.sh [选项]

  -i <file>    要烧录的镜像，默认 all-app.bin
  -p <port>    串口设备，默认 /dev/ttyUSB0
  -n <num>     bk_loader 的端口号，默认 0
  -b <baud>    烧录波特率，默认 115200（只影响下载速度）
  -B <baud>    控制台波特率，默认 115200（固件编译时定死的，一般别改）
  -s           跳过 all-app.bin 字节数校验
  -a           烧完自动挂上串口看日志
  -t           只测握手，不写 flash（用 read 代替 download）
  -h           显示本帮助

说明: 不需要手按 RST。脚本会在 bk_loader 握手循环期间，从另一个进程往
      同一个串口写 reboot 来触发软复位。
EOF
}

while getopts "i:p:n:b:B:sath" opt; do
  case "$opt" in
    i) IMAGE="$OPTARG" ;;
    p) PORT="$OPTARG" ;;
    n) PORTNUM="$OPTARG" ;;
    b) BAUD="$OPTARG" ;;
    B) CONSOLE_BAUD="$OPTARG" ;;
    s) SKIP_SIZE_CHECK=1 ;;
    a) ATTACH_SERIAL=1 ;;
    t) TEST_ONLY=1 ;;
    h) usage; exit 0 ;;
    *) usage; exit 2 ;;
  esac
done

die() { echo "错误: $*" >&2; exit 1; }

# A wrong ROOT is the first thing to go wrong on someone else's machine, so say
# where it came from and which line to edit instead of just naming a missing
# file.
die_root() {
  echo "错误: $1" >&2
  echo "      当前 ROOT=$ROOT" >&2
  if [ -n "${VELA_WS:-}" ]; then
    echo "      （来自环境变量 VELA_WS）" >&2
  else
    echo "      改本脚本顶部的 ROOT= 那一行，或用 VELA_WS=/your/workspace 覆盖" >&2
  fi
  exit 1
}

# ---------------------------------------------------------------- 1. 前置检查

[ -x "$LOADER" ] || die_root "找不到 bk_loader: $LOADER"

# Only the default image implicates ROOT; an explicit -i is the caller's path.
if [ ! -f "$IMAGE" ]; then
  if [ "$IMAGE" = "$DEFAULT_IMAGE" ]; then
    die_root "找不到镜像: $IMAGE（也可能是还没打包）"
  fi
  die "找不到镜像: $IMAGE"
fi
[ -c "$PORT" ]   || die "串口不存在: $PORT（板子插了吗？）"

SIZE=$(stat -c%s "$IMAGE")
SHA=$(sha256sum "$IMAGE" | cut -d' ' -f1)

# 10 bits on the wire per byte (8N1 plus start and stop).
XFER_SECS=$(( SIZE * 10 / BAUD ))
OVERALL_TIMEOUT=$(( XFER_SECS * 2 + 60 ))

echo "=== 待烧录镜像 ==="
echo "  路径   : $IMAGE"
echo "  字节数 : $SIZE"
echo "  SHA256 : $SHA"
echo "  串口   : $PORT"
echo "  烧录   : ${BAUD} baud，传输约 ${XFER_SECS}s，超时上限 ${OVERALL_TIMEOUT}s"
echo "  控制台 : ${CONSOLE_BAUD} baud（烧完会恢复成这个）"

if [ "$SKIP_SIZE_CHECK" -eq 0 ] && [ "$(basename "$IMAGE")" = "all-app.bin" ]; then
  if [ "$SIZE" -ne "$EXPECTED_SIZE" ]; then
    die "all-app.bin 应为 $EXPECTED_SIZE 字节，实际 $SIZE。打包可能不完整，用 -s 可跳过此检查"
  fi
  echo "  字节数校验通过"
fi

# ---------------------------------------------------------- 2. 释放串口占用

echo
echo "=== 释放串口 ==="
HOLDERS=$(fuser "$PORT" 2>/dev/null | tr -s ' ')
if [ -n "$HOLDERS" ]; then
  echo "  当前占用: $HOLDERS"

  # Detached screen sessions are the usual culprit: Ctrl-A D leaves them
  # running and holding the tty.
  while read -r sid; do
    [ -n "$sid" ] || continue
    echo "  关闭 screen 会话 $sid"
    screen -X -S "$sid" quit >/dev/null 2>&1
  done < <(screen -ls 2>/dev/null | grep -oE '^\s+[0-9]+\.[^ \t]+' | tr -d ' \t')

  pkill -f "cat $PORT" >/dev/null 2>&1

  sleep 0.5
  HOLDERS=$(fuser "$PORT" 2>/dev/null | tr -s ' ')
  if [ -n "$HOLDERS" ]; then
    die "串口仍被占用: $HOLDERS。手动处理后重试（参考 docs/端口冲突解决办法.md）"
  fi
fi
echo "  串口空闲"

# Console baud, not flash baud: the only thing written through this port
# before bk_loader takes over is the "reboot" command, and the firmware's
# console is listening at CONSOLE_BAUD.
stty -F "$PORT" "$CONSOLE_BAUD" cs8 -cstopb -parenb raw -echo -crtscts \
  || die "stty 配置失败"

# --------------------------------------------------- 3. 启动 bk_loader 并软复位

echo
: > "$LOG"
PIDFILE=$(mktemp)

# The in-loop reset works at any -b, and here is why it is safe to rely on:
# bk_loader opens the tty at 115200 no matter what -b says, handshakes there,
# and only switches to the requested rate once it owns the bus.  Measured from
# a real 1500000 run:
#
#   cmdline: ... bk_loader download -p 0 -b 1500000 ...
#   Current port : /dev/ttyUSB0 + BaudRate : 115200 connect success
#   Getting Bus... / Gotten Bus... / All Finished Successfully
#
# So while we are writing "reboot" the line is still at 115200 and the
# firmware console decodes it fine.  An earlier version of this script
# disabled the retry loop for BAUD != CONSOLE_BAUD on the theory that the port
# had already been switched up -- that was reasoning without measurement, and
# it broke a mechanism that worked.

if [ "$TEST_ONLY" -eq 1 ]; then
  echo "=== 只测握手，不写 flash ==="
  set -- read -p "$PORTNUM" -b "$BAUD" -f "/tmp/autoflash_probe.bin@0-1000"
else
  echo "=== 开始烧录（无需按 RST）==="
  set -- download -p "$PORTNUM" -b "$BAUD" -s 0 -i "$IMAGE"
fi

# Output goes to the terminal *and* the log.
#
# bk_loader draws its erase/write progress with cli_printf_progress_bar,
# which emits "Erasing Flash ... 42%\r" -- carriage return, no newline.  Two
# things are needed to see that live: tee, so the bytes reach the terminal at
# all, and stdbuf -o0, because with a pipe on stdout glibc would otherwise
# block-buffer 4 KB at a time and the bar would arrive in lurches.
#
# The PID is stashed in a file because with a pipeline $! would be tee's.
(
  stdbuf -o0 -e0 timeout "$OVERALL_TIMEOUT" "$LOADER" "$@" &
  echo $! > "$PIDFILE"
  wait $!
) 2>&1 | tee "$LOG" &
PIPELINE_PID=$!

LOADER_PID=""
for _ in $(seq 1 50); do
  if [ -s "$PIDFILE" ]; then LOADER_PID=$(cat "$PIDFILE"); break; fi
  sleep 0.1
done
[ -n "$LOADER_PID" ] || die "无法获取 bk_loader 进程号"

# Wait for it to actually enter the retry loop before poking the board.
for _ in $(seq 1 40); do
  grep -q "Getting Bus" "$LOG" && break
  kill -0 "$LOADER_PID" 2>/dev/null || break
  sleep 0.1
done

if ! kill -0 "$LOADER_PID" 2>/dev/null; then
  echo "  bk_loader 提前退出:"
  tail -5 "$LOG" | sed 's/^/    | /'
  wait "$LOADER_PID"; exit 1
fi

GOT_BUS=0

echo "  bk_loader 已进入握手循环，开始发送软复位"

for i in $(seq 1 "$REBOOT_TRIES"); do
  if grep -aq "Gotten Bus" "$LOG"; then GOT_BUS=1; break; fi
  kill -0 "$LOADER_PID" 2>/dev/null || break

  # If the console happens to be bridged to the AP shell, escape back to the
  # CP shell first: Ctrl-] then '.'.
  #
  # The trailing newline after the escape is essential.  Without it the CP
  # shell buffers all of it as a single line and sees "\x1d.reboot", answers
  # "cmd NOT found: .reboot" and never resets -- which made every attempt here
  # fall through to the manual RST prompt.  Measured: with the flush the board
  # reboots, without it it never does.
  printf '\035' > "$PORT" 2>/dev/null
  printf '.'    > "$PORT" 2>/dev/null
  printf '\r\n' > "$PORT" 2>/dev/null
  printf 'reboot\r\n' > "$PORT" 2>/dev/null
  echo "  第 $i 次软复位已发出"

  # Poll frequently so we stop writing the instant the bus is taken --
  # further bytes would corrupt the download protocol.
  for _ in $(seq 1 15); do
    if grep -q "Gotten Bus" "$LOG"; then GOT_BUS=1; break; fi
    sleep 0.1
  done
  [ "$GOT_BUS" -eq 1 ] && break
done

if [ "$GOT_BUS" -eq 1 ]; then
  echo "  握手成功，下面是 bk_loader 的实时输出："
else
  echo "  软复位没能拿到总线 —— 现在请手动按一次 RST（按一下就彻底松手）"
  echo "  bk_loader 仍在循环握手，按下去就会接上"
fi

# Waiting on the pipeline, not the loader: tee has to finish draining before
# the log is complete enough to judge the outcome.
wait "$PIPELINE_PID"
RC=$?
rm -f "$PIDFILE"

# ------------------------------------------------------------------ 4. 判定

echo
echo "=== 结果 ==="
if grep -aq "All Finished Successfully" "$LOG"; then
  echo "  烧录成功"
  grep -aE "Total Test Time|EraseFlash ->pass" "$LOG" \
    | tail -3 | sed 's/^/    | /'
else
  echo "  烧录失败（退出码 $RC）"

  # Distinguish "never got started" from "killed halfway through the write",
  # because the second case leaves the board unbootable and must be re-flashed
  # before anything else is attempted.
  LAST_PCT=$(tr '\r' '\n' < "$LOG" | grep -aoE "Writing Flash \.\.\. [0-9]+%" \
             | tail -1)
  if [ -n "$LAST_PCT" ]; then
    echo
    echo "  !! flash 已被擦除但只写到 $LAST_PCT —— 固件不完整，板子起不来。"
    echo "     必须重新完整烧录一次才能恢复。"
    echo "     此时固件不响应 reboot，软复位无效，需要手动按一次 RST。"
  elif [ "$RC" -eq 124 ]; then
    echo "  !! 被超时中止（上限 ${OVERALL_TIMEOUT}s）"
  fi

  echo
  echo "  完整日志: $LOG"
  exit 1
fi

# --------------------------------------------------------- 5. 可选挂回串口

# Put the line back to the console rate.  bk_loader left it at BAUD, and if
# that is not CONSOLE_BAUD then anything attaching afterwards -- screen, cat,
# minicom -- would see garbage and typing would do nothing.
# A silent 2>/dev/null here used to claim success even when the restore had
# failed.  stty needs the port to itself, so anything already attached to it --
# a screen session, a stray cat -- makes this fail with EBUSY, and the line is
# then left at BAUD while the message says otherwise.  That is worse than not
# trying: the next person sees a dead console and no reason for it.
if [ "$BAUD" != "$CONSOLE_BAUD" ]; then
  if stty_err=$(stty -F "$PORT" "$CONSOLE_BAUD" cs8 -cstopb -parenb raw \
                  -echo -crtscts 2>&1); then
    echo "  串口已从 ${BAUD} 恢复为控制台的 ${CONSOLE_BAUD} baud"
  else
    echo
    echo "  !! 串口波特率没能恢复：${stty_err}"
    echo "     串口仍停在 ${BAUD}，此时接上去会看到乱码、打字也没反应。"
    echo "     通常是有程序正占着串口（screen / minicom / cat）。"
    echo "     处理：先退出那个程序，再执行"
    echo "       stty -F ${PORT} ${CONSOLE_BAUD} cs8 -cstopb -parenb raw -echo -crtscts"
    echo "     或者重新打开 screen ${PORT} ${CONSOLE_BAUD}（screen 自己会设速率）。"
  fi
fi

if [ "$ATTACH_SERIAL" -eq 1 ]; then
  echo
  echo "=== 挂上串口（Ctrl-A K 再按 y 退出，别用 Ctrl-A D）==="
  sleep 1
  exec screen "$PORT" "$CONSOLE_BAUD"
fi

echo
echo "  接串口看日志:  sg dialout -c \"screen $PORT $CONSOLE_BAUD\""
