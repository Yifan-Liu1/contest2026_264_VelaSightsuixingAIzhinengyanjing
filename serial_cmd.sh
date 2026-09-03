#!/usr/bin/env bash
############################################################################
# serial_cmd.sh
#
# 往板子的控制台发命令并抓回输出，用于上板回归。
#
# 为什么是脚本而不是交互终端：回归要能在 CI 或一行命令里跑完，且判据是文本
# （fps、计数器、错误行），交互终端两者都做不到。
#
# 与 autoflash.sh 的分工：这个脚本只说话，不烧固件；两者都要独占
# /dev/ttyUSB0，所以都在开工前用 fuser 检查，占用时的措辞也保持一致
# （app/web_tool/host/serial_console.py 的 SerialBusy 依赖 "串口被占用"
# 这几个字来让两边的失败长得一样）。
#
# 不用 pyserial：app/web_tool 那一侧已经论证过——新克出来的机器上可能没有，
# 而这里需要的只是固定波特率的裸字节。用 stty 配置、后台 cat 读，参数与
# serial_console.py 保持一致（115200 cs8 -cstopb -parenb raw -echo -crtscts）。
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

set -u

PORT=/dev/ttyUSB0
BAUD=115200
WAIT=30
EXPECT=""
OUTFILE=""
DO_RESET=0
DO_BRIDGE=0

TRANSCRIPT=$(mktemp -t serial_cmd.XXXXXX)
CAT_PID=""

die() { printf '错误: %s\n' "$*" >&2; exit 1; }

cleanup() {
  if [ -n "$CAT_PID" ] && kill -0 "$CAT_PID" 2>/dev/null; then
    kill "$CAT_PID" 2>/dev/null
    wait "$CAT_PID" 2>/dev/null
  fi
  rm -f "$TRANSCRIPT"
}
trap cleanup EXIT

usage() {
  cat <<'EOF'
用法: ./serial_cmd.sh [选项] [命令...]

  -r           先复位板子，再桥接到 AP 控制台。复位路径是「从 AP 的 NSH 逃回
               CP shell（Ctrl-] 松手 .），在 CP 上 reboot」，因为 AP 的 NSH
               没有 reboot
  -b           只桥接到 AP 控制台，不复位。上一次会话把控制台留在 CP shell 时
               用这个；已经在 AP 上时重复发也无害
  -w <秒>      总等待上限，默认 30。给了 -e 时它退化为上限
  -e <正则>    看到匹配就立即收工，不等满 -w
  -o <文件>    把完整抓取写到文件（stdout 照常打印）
  -p <设备>    串口设备，默认 /dev/ttyUSB0
  -B <波特率>  控制台波特率，默认 115200（由 CP 的 CONFIG_UART_PRINT_BAUD_RATE
               定死，一般别动；不要和 autoflash.sh 的 -b 下载速率混淆）
  -h           显示本帮助

例子:

  ./serial_cmd.sh -r -w 30 -e 'agent_camera: OK' -o /tmp/caps.log 'agent_camera caps'
  ./serial_cmd.sh -b -w 100 -e 'agent_camera: OK' 'agent_camera 640x480 n=30'
  ./serial_cmd.sh -w 60 'ap_console open' '你的命令'      # 手工桥接的等价写法

注意 -e 的正则要挑命令自己的结束标记。不给 -e 时脚本会死等满 -w，取帧这类
命令因此会明显变慢（实测同一条 640x480 取帧：带 -e 15.5s，不带 61.7s）。
EOF
}

while getopts "rbw:e:o:p:B:h" opt; do
  case "$opt" in
    r) DO_RESET=1 ;;
    b) DO_BRIDGE=1 ;;
    w) WAIT=$OPTARG ;;
    e) EXPECT=$OPTARG ;;
    o) OUTFILE=$OPTARG ;;
    p) PORT=$OPTARG ;;
    B) BAUD=$OPTARG ;;
    h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))

case "$WAIT" in *[!0-9.]*|'') die "-w 必须是秒数: $WAIT" ;; esac
[ -e "$PORT" ] || die "串口不存在: $PORT"

# 占用检查。PID 列表在 stdout，可读的表格在 stderr，所以两者都要看；表格里
# 会混进对无关进程的 "权限不够" 抱怨（fuser 逐个 stat /proc/*/fd），过滤掉，
# 否则真正有用的那一行会被埋掉。lsof 在非 root 下查不出串口占用，不要用它下
# 判断。
if [ -n "$(fuser "$PORT" 2>/dev/null || true)" ]; then
  printf '串口被占用: %s\n' "$PORT" >&2
  fuser -v "$PORT" 2>&1 | grep -avE '权限不够|Permission denied|无法获取|Cannot stat' >&2
  printf '关掉 picocom/minicom/screen/cat 或停掉 web_tool 后端再试；\n' >&2
  printf 'screen 的 Ctrl-A D 只是分离，要释放得 Ctrl-A K 再按 y。\n' >&2
  exit 1
fi

command -v stty >/dev/null || die "找不到 stty"

stty -F "$PORT" "$BAUD" cs8 -cstopb -parenb raw -echo -crtscts \
  || die "配置串口失败: $PORT @ $BAUD"

# 后台读。整个会话只开一个读端，命令之间不重开，否则会丢掉两次读之间到达的
# 字节 —— 板子是自己按节奏打印的，不会等我们。
cat "$PORT" > "$TRANSCRIPT" 2>/dev/null &
CAT_PID=$!
sleep 0.2

send() {
  printf '%s\r\n' "$1" > "$PORT"
}

send_raw() {
  printf '%b' "$1" > "$PORT"
}

# 抓取到超时，或抓到 $2 指定的正则就提前返回 0。
harvest() {
  local seconds=$1
  local pattern=${2:-}
  local deadline
  deadline=$(awk -v s="$seconds" 'BEGIN { printf "%.2f", systime() + s }')

  while :; do
    if [ -n "$pattern" ] && grep -aqE "$pattern" "$TRANSCRIPT"; then
      return 0
    fi

    if awk -v d="$deadline" 'BEGIN { exit !(systime() >= d) }'; then
      return 1
    fi

    sleep 0.2
  done
}

if [ "$DO_RESET" = 1 ]; then
  # 先逃回 CP shell：控制台可能停在 AP 的 NSH 上，而 reboot 只有 CP 有。
  # 逃逸序列是 Ctrl-] 松手再按 . （AP console 自己打印的提示）。
  send_raw '\035'
  sleep 0.3
  send_raw '.'
  sleep 0.5
  send ''
  sleep 0.3
  send 'reboot'
  harvest 6 'NuttShell|nsh>' || true
fi

if [ "$DO_RESET" = 1 ] || [ "$DO_BRIDGE" = 1 ]; then
  # 轮询而不是等固定时长。这里以前写死 4 秒，而当前固件启动到能响应
  # ap_console open 要更久，于是复位分支经常什么都没桥接上就往下走了
  # （使用说明-camera-display-ai_agent.md 曾把它记成环境问题）。
  bridged=0
  for _ in $(seq 1 40); do
    send 'ap_console open'
    if harvest 0.5 'AP console open|switching UART0 to AP console'; then
      bridged=1
      break
    fi
  done

  harvest 1 '' || true

  if [ "$bridged" = 0 ]; then
    printf '提示: 没等到 AP console 的应答，命令会发给当前占用控制台的一侧\n' >&2
  fi
fi

send ''
harvest 0.5 '' || true

START=$(date +%s)
for cmd in "$@"; do
  send "$cmd"
  harvest "$WAIT" "$EXPECT" && break
done
harvest 0.3 '' || true

kill "$CAT_PID" 2>/dev/null
wait "$CAT_PID" 2>/dev/null
CAT_PID=""

cat "$TRANSCRIPT"

if [ -n "$OUTFILE" ]; then
  cp "$TRANSCRIPT" "$OUTFILE" || die "写不进 $OUTFILE"
fi

BYTES=$(wc -c < "$TRANSCRIPT" | tr -d ' ')
ELAPSED=$(( $(date +%s) - START ))
printf '\n=== 抓取 %s 字节，用时 %ss' "$BYTES" "$ELAPSED"
[ -n "$EXPECT" ] && {
  if grep -aqE "$EXPECT" "$TRANSCRIPT"; then
    printf '，命中 -e'
  else
    printf '，未命中 -e（等满上限）'
  fi
}
[ -n "$OUTFILE" ] && printf '，已写入 %s' "$OUTFILE"
printf ' ===\n'
