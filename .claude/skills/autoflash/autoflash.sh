#!/usr/bin/env bash
# 转发到仓库根目录的 autoflash.sh，本文件不再包含实现。
#
# 这里曾经有第二份实现。两份在时序处理上已经分叉到互不相同的程度，而使用者按
# 路径随手挑一份跑，看到的行为和日志格式都不一样 —— 上一次排查就是先花时间发现
# "你跑的不是这一份"。实现只保留一处，这个文件只负责把参数递过去。
set -u

SELF_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
SELF="$SELF_DIR/$(basename -- "${BASH_SOURCE[0]}")"

dir=$SELF_DIR
while [ "$dir" != "/" ]; do
  candidate="$dir/autoflash.sh"
  if [ "$candidate" != "$SELF" ] && [ -x "$candidate" ]; then
    exec "$candidate" "$@"
  fi
  dir=$(dirname -- "$dir")
done

echo "错误: 往上找不到 autoflash.sh 的实现（应在仓库根目录）" >&2
exit 1
