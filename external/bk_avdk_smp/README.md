# Beken AVDK 覆盖与构建基线

本目录保存 BK7258 OpenVela 最终固件构建所需的 Beken AVDK 覆盖文件，并作为
多人协作时 `bk_avdk_smp` 修改的同步记录。不要只修改工作区中的
`~/vela_competition/bk_avdk_smp`；可复现修改必须同时更新本目录及本文档。

## 1. 当前覆盖文件

| 比赛仓权威副本 | `bk_avdk_smp` 目标文件 | 作用 |
| --- | --- | --- |
| `cp/components/bk_cli/cli_main.c` | `cp/components/bk_cli/cli_main.c` | 注册 `ap_console` CLI 命令 |
| `cp/components/bk_cli/shell_task.c` | `cp/components/bk_cli/shell_task.c` | UART0 CP_CLI/AP_CONSOLE owner 状态机、延迟切换和退出转义 |
| `cp/components/bk_pm/pm.c` | `cp/components/bk_pm/pm.c` | DVFS 错误传播、vote/cache rollback 和 CPU/PSRAM 联合 VDDDIG 下限 |
| `cp/components/controller_if/cif_ipc.c` | `cp/components/controller_if/cif_ipc.c` | 保留 Wi-Fi IPC RX status callback ABI |
| `cp/components/controller_if/Kconfig` | `cp/components/controller_if/Kconfig` | `CONFIG_WIFI_VNET_AP_IPV4` 配置 |
| `cp/components/controller_if/cif_cntrl.c` | `cp/components/controller_if/cif_cntrl.c` | STA scan/country IPC、扫描完成通知和结果快照 |
| `cp/components/controller_if/cif_main.c` | `cp/components/controller_if/cif_main.c` | IPC 批处理、enqueue failure 返回和 command/data ownership |
| `cp/components/controller_if/cif_main.h` | `cp/components/controller_if/cif_main.h` | OpenVela command ID、批处理消息和 callback ABI |
| `cp/components/controller_if/cif_wifi_dp.c` | `cp/components/controller_if/cif_wifi_dp.c` | STA ARP/IPv4 分流和 direct-push pbuf ownership |
| `cp/components/lwip_intf_v2_1/lwip-2.1.2/port/net.c` | `cp/components/lwip_intf_v2_1/lwip-2.1.2/port/net.c` | CP STA IPv4/DHCP/ARP ownership 禁用 |
| `cp/components/wpa_supplicant-2.10/wpa_supplicant/notify.c` | `cp/components/wpa_supplicant-2.10/wpa_supplicant/notify.c` | STA connected/disconnected event 转发 |
| `cp/components/wpa_supplicant-2.10/wpa_supplicant/wpa_scan.c` | `cp/components/wpa_supplicant-2.10/wpa_supplicant/wpa_scan.c` | AP-owned IPv4 下重复 disconnect 抑制 |
| `cp/include/components/ap_console_bridge.h` | `cp/include/components/ap_console_bridge.h` | CP UART0 bridge 模式、写入和统计接口 |
| `cp/include/components/shell_task.h` | `cp/include/components/shell_task.h` | shell 与 bridge 的切换/链路事件接口 |
| `cp/include/driver/mailbox_channel.h` | `cp/include/driver/mailbox_channel.h` | transport event、诊断和 poll 接口 |
| `cp/include/driver/mb_uart_driver.h` | `cp/include/driver/mb_uart_driver.h` | MB_UART link event、状态和 poll 接口 |
| `cp/include/driver/pwr_clk.h` | `cp/include/driver/pwr_clk.h` | PSRAM V1、OpenVela lifecycle 与 PM 诊断接口 |
| `cp/middleware/driver/common/driver.c` | `cp/middleware/driver/common/driver.c` | MB_UART0 唯一 owner、LOG/RAW RX、1024 B TX ring 和 worker |
| `cp/middleware/driver/mailbox/mailbox_channel.c` | `cp/middleware/driver/mailbox/mailbox_channel.c` | 完整 ACK 匹配、timeout/reset、probe 和诊断 |
| `cp/middleware/driver/mailbox/mb_ipc_cmd.c` | `cp/middleware/driver/mailbox/mb_ipc_cmd.c` | IPC power-up ready 门禁事件 |
| `cp/middleware/driver/mailbox/mb_ipc_heartbeat.c` | `cp/middleware/driver/mailbox/mb_ipc_heartbeat.c` | heartbeat/reset/down 自动回退事件 |
| `cp/middleware/driver/mailbox/mb_uart_driver.c` | `cp/middleware/driver/mailbox/mb_uart_driver.c` | 严格 DATA/STATE 校验、ACK 后推进和 link event |
| `cp/middleware/driver/mailbox/mbox0_adapter.c` | `cp/middleware/driver/mailbox/mbox0_adapter.c` | command/ACK/reset/sync 分离稳定 slot |
| `cp/middleware/driver/mailbox/mbox0_adapter.h` | `cp/middleware/driver/mailbox/mbox0_adapter.h` | 稳定 slot 完成和 quarantine 接口 |
| `cp/middleware/driver/mailbox/mbox0_drv.c` | `cp/middleware/driver/mailbox/mbox0_drv.c` | RX 资源门禁、FIFO poll 和 destination count |
| `cp/middleware/driver/mailbox/mbox0_drv.h` | `cp/middleware/driver/mailbox/mbox0_drv.h` | MBOX0 poll、ready callback 和 FIFO count API |
| `cp/middleware/driver/psram/psram_driver.c` | `cp/middleware/driver/psram/psram_driver.c` | PSRAM 初始化失败时关闭已启用的 power/clock |
| `cp/middleware/driver/pwr_clk/Kconfig` | `cp/middleware/driver/pwr_clk/Kconfig` | 只定义单核 CPU1 频率 vote 的 `CONFIG_OPENVELA_AP_CPU1_480M` 开关 |
| `cp/middleware/driver/pwr_clk/low_pwr_core.c` | `cp/middleware/driver/pwr_clk/low_pwr_core.c` | PSRAM V1 真实 ret/version、PM readback 与 OpenVela ready worker |
| `cp/middleware/driver/pwr_clk/pwr_clk.c` | `cp/middleware/driver/pwr_clk/pwr_clk.c` | 单核 CPU1 启动、失败回滚、480 MHz vote 与可取消 recovery |
| `cp/middleware/driver/sys_ctrl/sys_ps_driver.c` | `cp/middleware/driver/sys_ctrl/sys_ps_driver.c` | 单目标 DVFS 切换与错误/cache 传播 |
| `cp/middleware/soc/bk7258/hal/sys_hal.c` | `cp/middleware/soc/bk7258/hal/sys_hal.c` | bus divider bit 6、寄存器 readback 与失败恢复 |
| `cp/middleware/soc/common/hal/include/sys_hal.h` | `cp/middleware/soc/common/hal/include/sys_hal.h` | VDDD/VDDDIG readback API |
| `projects/app_ab/cp/cp_main.c` | `projects/app_ab/cp/cp_main.c` | OpenVela boot transaction 启动与失败日志保留 |
| `projects/app_ab/cp/config/bk7258/config` | `projects/app_ab/cp/config/bk7258/config` | app_ab CP 启用单核 CPU1 480 MHz |
| `projects/app_ab/partitions/bk7258/ram_regions.csv` | `projects/app_ab/partitions/bk7258/ram_regions.csv` | BK7258 640 KiB Share SRAM 和 16 MiB PSRAM 布局 |

UART0 bridge 保持 `MB_UART0` 唯一初始化 owner。AP 输出 LOG 模式保留 `ap0:`
前缀、50 ms 半行刷新和有限重试；RAW 模式不改写字节。PC 到 AP 输入使用
1024 B 有界 ring 和部分写 worker，所有物理 UART0 输出仍经 shell queue 串行化。

`ram_regions.csv`采用原厂 `lvgl/img_decode`、`lvgl/freetype_font` 的 16 MiB
七区域布局：

```text
0x60000000..0x60700000  四个媒体 slab，共 7 MiB
0x60700000..0x60720000  CP PSRAM heap，128 KiB
0x60720000..0x60a00000  AP PSRAM heap，2.875 MiB
0x60a00000..0x61000000  AP PSRAM section，6 MiB
```

## 2. 同步到 Armino 工作树

从工作区根目录执行：

```bash
mirror=contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/bk_avdk_smp
for path in \
  cp/components/bk_cli/cli_main.c \
  cp/components/bk_cli/shell_task.c \
  cp/components/bk_pm/pm.c \
  cp/components/controller_if/cif_ipc.c \
  cp/components/controller_if/Kconfig \
  cp/components/controller_if/cif_cntrl.c \
  cp/components/controller_if/cif_main.c \
  cp/components/controller_if/cif_main.h \
  cp/components/controller_if/cif_wifi_dp.c \
  cp/components/lwip_intf_v2_1/lwip-2.1.2/port/net.c \
  cp/components/wpa_supplicant-2.10/wpa_supplicant/notify.c \
  cp/components/wpa_supplicant-2.10/wpa_supplicant/wpa_scan.c \
  cp/include/components/ap_console_bridge.h \
  cp/include/components/shell_task.h \
  cp/include/driver/mailbox_channel.h \
  cp/include/driver/mb_uart_driver.h \
  cp/include/driver/pwr_clk.h \
  cp/middleware/driver/common/driver.c \
  cp/middleware/driver/mailbox/mailbox_channel.c \
  cp/middleware/driver/mailbox/mb_ipc_cmd.c \
  cp/middleware/driver/mailbox/mb_ipc_heartbeat.c \
  cp/middleware/driver/mailbox/mb_uart_driver.c \
  cp/middleware/driver/mailbox/mbox0_adapter.c \
  cp/middleware/driver/mailbox/mbox0_adapter.h \
  cp/middleware/driver/mailbox/mbox0_drv.c \
  cp/middleware/driver/mailbox/mbox0_drv.h \
  cp/middleware/driver/psram/psram_driver.c \
  cp/middleware/driver/pwr_clk/Kconfig \
  cp/middleware/driver/pwr_clk/low_pwr_core.c \
  cp/middleware/driver/pwr_clk/pwr_clk.c \
  cp/middleware/driver/sys_ctrl/sys_ps_driver.c \
  cp/middleware/soc/bk7258/hal/sys_hal.c \
  cp/middleware/soc/common/hal/include/sys_hal.h \
  projects/app_ab/cp/cp_main.c \
  projects/app_ab/cp/config/bk7258/config \
  projects/app_ab/partitions/bk7258/ram_regions.csv
do
  cp "$mirror/$path" "bk_avdk_smp/$path"
done
```

同步后必须逐字节检查：

```bash
for path in \
  cp/components/bk_cli/cli_main.c \
  cp/components/bk_cli/shell_task.c \
  cp/components/bk_pm/pm.c \
  cp/components/controller_if/cif_ipc.c \
  cp/components/controller_if/Kconfig \
  cp/components/controller_if/cif_cntrl.c \
  cp/components/controller_if/cif_main.c \
  cp/components/controller_if/cif_main.h \
  cp/components/controller_if/cif_wifi_dp.c \
  cp/components/lwip_intf_v2_1/lwip-2.1.2/port/net.c \
  cp/components/wpa_supplicant-2.10/wpa_supplicant/notify.c \
  cp/components/wpa_supplicant-2.10/wpa_supplicant/wpa_scan.c \
  cp/include/components/ap_console_bridge.h \
  cp/include/components/shell_task.h \
  cp/include/driver/mailbox_channel.h \
  cp/include/driver/mb_uart_driver.h \
  cp/include/driver/pwr_clk.h \
  cp/middleware/driver/common/driver.c \
  cp/middleware/driver/mailbox/mailbox_channel.c \
  cp/middleware/driver/mailbox/mb_ipc_cmd.c \
  cp/middleware/driver/mailbox/mb_ipc_heartbeat.c \
  cp/middleware/driver/mailbox/mb_uart_driver.c \
  cp/middleware/driver/mailbox/mbox0_adapter.c \
  cp/middleware/driver/mailbox/mbox0_adapter.h \
  cp/middleware/driver/mailbox/mbox0_drv.c \
  cp/middleware/driver/mailbox/mbox0_drv.h \
  cp/middleware/driver/psram/psram_driver.c \
  cp/middleware/driver/pwr_clk/Kconfig \
  cp/middleware/driver/pwr_clk/low_pwr_core.c \
  cp/middleware/driver/pwr_clk/pwr_clk.c \
  cp/middleware/driver/sys_ctrl/sys_ps_driver.c \
  cp/middleware/soc/bk7258/hal/sys_hal.c \
  cp/middleware/soc/common/hal/include/sys_hal.h \
  projects/app_ab/cp/cp_main.c \
  projects/app_ab/cp/config/bk7258/config \
  projects/app_ab/partitions/bk7258/ram_regions.csv
do
  cmp -s "$mirror/$path" "bk_avdk_smp/$path" || exit 1
done
```

全部`cmp`都必须返回0。此门禁应在构建前执行；Kconfig 会把项目的最小
`projects/app_ab/cp/config/bk7258/config`输入 fragment 原地展开为完整生成配置，
因此不得声称构建后该输入仍与权威最小种子逐字节一致。

### 2.1 NuttX Bluetooth Host 最小补丁

BK7258 BLE 重连还依赖比赛仓保存的 NuttX Host 补丁：

```text
external/nuttx/patches/
  0001-bluetooth-fix-acl-buffer-and-connection-references.patch
```

该补丁只修改 `wireless/bluetooth/bt_hcicore.c` 的 `hci_acl()`：

```c
bt_buf_addref(buf);
bt_conn_receive(conn, buf, flags);
bt_conn_release(conn);
```

- `bt_buf_addref()` 为 L2CAP/ATT consumer 保留独立 ACL buffer 引用，避免 HCI RX
  worker 返回后形成双重释放或分片 PDU 悬空引用。
- `bt_conn_release()` 释放 `bt_conn_lookup_handle()` 返回的 connection 引用，避免
  每个入站 ACL 包泄漏引用并导致 `CONFIG_BLUETOOTH_MAX_CONN=1` 的槽位在断开后
  无法复用。

这两项是 NuttX Bluetooth Host 的通用引用计数修复，不属于 BK7258 Controller
兼容逻辑。BK7258 特定的 HCI command 过滤、ACL PB 标准化和 mailbox credit 仍放在
`board/beken/chips/bk7258/bk7258_bt_transport.c`，不得复制进公共 Host 栈。

在 OpenVela 工作区根目录构建前应用补丁：

```bash
patch_file="$PWD/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/nuttx/patches/0001-bluetooth-fix-acl-buffer-and-connection-references.patch"

git -C nuttx apply --check "$patch_file"
git -C nuttx apply "$patch_file"
git -C nuttx diff --check
```

若 `git apply --check` 报告补丁已应用，先确认以下两行已经位于 `hci_acl()`，不要
重复应用：

```bash
grep -n -A3 'bt_buf_addref(buf)' nuttx/wireless/bluetooth/bt_hcicore.c
```

验证补丁范围只能包含目标文件和两行行为修改：

```bash
git -C nuttx diff --stat
git -C nuttx diff -- wireless/bluetooth/bt_hcicore.c
```

该文件是公共 NuttX 源码的补丁归档，不是 `bk_avdk_smp` 覆盖文件。正式方案仍应
向对应 NuttX 公共仓库提交独立 PR；在公共修复合入前，本补丁保证比赛固件可复现。

## 3. 正确构建链条

最终固件链条固定为：

```text
OpenVela nuttx.bin
  -> bk_avdk_smp/build/openvela-ap.bin
  -> EXTERNAL_AP_BIN
  -> package/tmp/app1.bin
  -> package/all-app.bin

Armino CP app.bin + Beken bootloader.bin
  ---------------------------------------> package/all-app.bin
```

修改以下任一输入后，不得直接做增量构建：

- `ram_regions.csv`
- AP/CP `config`
- Kconfig 默认值
- 分区生成脚本
- SoC 链接脚本

原因是顶层 `partitions/ram_regions.h` 可以先更新，而 AP/CP CMake 构建目录中的
`armino/partitions/_build/ram_regions.csv`、生成头和预处理链接脚本仍可能保留旧值。
这会形成顶层 16 MiB、核内仍 8 MiB 的不一致产物。

先按第 2.1 节确认 NuttX Bluetooth Host 补丁已应用，再构建 OpenVela AP，并将
输入放到 Armino 工作树。这里要区分 `nsh` 和 `ai_agent`：前者是最小启动/对照
配置，后者是包含 VelaSight、LVGL 和 `packages/ai_agent` 的产品配置。早期基础
移植记录使用 `nsh`，不代表最终产品也应使用 `nsh`；本节最终打包链统一采用
`ai_agent`。

```bash
cd ~/vela_competition/contest
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent \
  --cmake -j8

cp \
  cmake_out/bk7258-ap_ai_agent/nuttx.bin \
  ~/vela_competition/bk_avdk_smp/build/openvela-ap.bin
```

然后执行项目级 clean 和容器内完整构建：

```bash
cd ~/vela_competition/bk_avdk_smp
make -C projects/app_ab clean

podman run --rm \
  --userns=keep-id \
  -v "$PWD:/armino" \
  -w /armino \
  -e EXTRA_CFLAGS=-Werror \
  localhost/bekencorp/armino-idk:1.5 \
  make -C projects/app_ab bk7258 \
  SDK_DIR=/armino \
  EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin
```

`make -C projects/app_ab clean`只删除`projects/app_ab/build`，不会删除
`build/openvela-ap.bin`。仅安装 Podman 时不要使用内部调用 Docker 的`dbuild.sh`。

CP 构建后先对生成配置做门禁，再恢复最小输入 fragment：

```bash
generated=projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.h
grep -q '^#define CONFIG_OPENVELA_AP_CPU1_480M 1$' "$generated"
grep -q '^#define CONFIG_WIFI_VNET_AP_IPV4 1$' "$generated"
grep -q '^#define CONFIG_GPIO_DEFAULT_SET_SUPPORT 1$' "$generated"

mirror=../contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/bk_avdk_smp
cp "$mirror/projects/app_ab/cp/config/bk7258/config" \
  projects/app_ab/cp/config/bk7258/config
cmp -s "$mirror/projects/app_ab/cp/config/bk7258/config" \
  projects/app_ab/cp/config/bk7258/config
```

权威 config 经确认是 7 行最小种子（首个空行加 6 行配置，包含单核 CPU1 480 MHz
频率项和显式关闭 SARADC mailbox），完整生成配置只存在于 build 目录，不回写比赛仓。

## 4. 构建验收

以下文件必须都反映同一份 16 MiB 布局：

```text
projects/app_ab/partitions/bk7258/ram_regions.csv
projects/app_ab/build/bk7258/app_ab/partitions/ram_regions.h
projects/app_ab/build/bk7258/app_ab/bk7258/armino/partitions/_build/ram_regions.csv
projects/app_ab/build/bk7258/app_ab/bk7258_ap/armino/partitions/_build/ram_regions.csv
projects/app_ab/build/bk7258/app_ab/bk7258/armino/bk7258/bk7258_out.ld
projects/app_ab/build/bk7258/app_ab/bk7258_ap/armino/bk7258_ap/bk7258_ap_out.ld
```

关键值应为：

```text
CONFIG_PSRAM_CAPACITY          0x01000000
CP_PSRAM_HEAP                 0x60700000 + 0x00020000
AP_PSRAM_HEAP                 0x60720000 + 0x002e0000
AP_PSRAM_SECTION              0x60a00000 + 0x00600000
PSRAM 最终边界                0x61000000
```

外部 AP 输入和最终打包输入必须完全一致：

```bash
sha256sum \
  ~/vela_competition/contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin \
  build/openvela-ap.bin \
  projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin

cmp -s \
  build/openvela-ap.bin \
  projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
```

检查最终产物：

```text
projects/app_ab/build/bk7258/app_ab/package/all-app.bin
projects/app_ab/build/bk7258/app_ab/package/app_ab_crc.rbl
projects/app_ab/build/bk7258/app_ab/package/build_summary.txt
```

`build_summary.txt`中的 AP 内存统计来自 Armino AP 占位镜像，只用于检查生成的
内存区域边界；它不代表最终打包的 OpenVela AP 静态占用。OpenVela AP 的静态
内存应以 `nuttx.map`或`System.map`为准。

## 5. OpenVela PSRAM 接入状态

OpenVela AP侧已经完成以下代码和离线构建基线：

- 精确16 MiB、RW/XN/non-cacheable MPU映射。
- `0x60720000..0x609fffff`独立AP PSRAM heap。
- USER/AUDIO/ENCODE/DISPLAY四个独立媒体pool。
- 明确排除`0x60700000..0x6071ffff` CP heap的地址检查。
- AP owner区域保存/恢复型alias probe，包含4/8/12 MiB测试点。
- `0x60a00000..0x60ffffff`静态section链接保留；reset copy/clear未实现前强制0 B。

本覆盖中的CP `0x7`协议要求request `param3=PM_PSRAM_PROTO_V1`；response `param1`回显
ON/OFF state、`param2`返回真实PSRAM driver结果、`param3`回显version。CP BOOT与
OpenVela使用独立owner，初始化失败不置owner bit。`0xc`已提供frequency/divider、
speed、voltage、CPU power/reset和PSRAM owner只读诊断；device ID和actual capacity
仍未暴露，因此alias probe和整颗器件实板测试仍是容量门禁。AP board必须同步发送
V1 version并检查真实ret/version后才能联调；本CP侧任务未修改AP board代码。

本轮已放弃 SMP，PWC 现行摘要保留 PSRAM V1 `0x7` 和
`PM_CPU1_BOOT_READY_CMD=0x5`；`0x5` 只解除 CP boot worker 等待，不应解释为 SMP
commit。`PM_OPENVELA_READY_CMD=0x11` 及其 PSRAM/clock/SMP commit 契约已废弃，不再是
现行必需协议；相关设计、联调结论和当时产物 hash 仅见
`docs/archive/BK7258_OPENVELA_SMP_PORTING_PLAN.md` 第 15 节历史记录。正常 recovery 的 INIT/拒绝会
取消 shutdown 并恢复 boot vote；只有 FINISH 才进入 shutdown。runtime DVFS 不由 AP 发起；
CP 保留单核 CPU1 的固定 480 MHz vote，以及 DVFS 错误传播、readback 和 rollback 修复。

## 6. 协作约束

- 不提交 `projects/app_ab/build`、顶层 `build`或`__pycache__`。
- 不把构建生成的完整 AP/CP配置误当作最小项目配置同步回比赛仓；按第3节检查
  生成配置后恢复权威最小种子。
- Armino 工作树可能包含其他成员的未提交修改；同步覆盖前先检查`git diff`。
- 新增任何 SMP 源码修改时，在本目录保留对应路径的权威副本，并更新上面的
  覆盖表、构建步骤和验收项。
- 当前阶段已完成OpenVela AP allocator、四个媒体pool、空静态PSRAM section、CP lifecycle
  修复和离线重打包；尚未完成CP ID/容量query、NSH压力测试、DMA/media lease和实板验证。

## 7. UART0 Mailbox V2 使用和边界

CP 启动后默认 owner 为 `CP_CLI`。只有 MB_UART0 STATE 成功 ACK、IPC power-up 和
PWC boot-ready 三个条件同时成立，且`bk_mb_uart_write_ready()`非零时，
`ap_console open`才允许进入。heartbeat timeout、AP reset/power-off 和 Mailbox
reset/timeout都会清除门禁并请求回退到CP CLI。使用方式：

```text
ap_console status
ap_console open
<RAW AP NSH interaction>
<press Ctrl-], release it, then press .>
ap_console status
```

`open`在当前 CP CLI 响应经 UART TX-complete 后生效，并清 CP parser 和 UART RX；
RAW 模式不添加前缀。退出时按下`Ctrl-]`、松开，再按`.`；该转义不依赖行首状态。
链路 reset/timeout/down 时自动清理
未发送输入并回到 `CP_CLI`。`status`输出 owner/link/mode、bridge ring、MB_UART 状态、
active channel/sequence/command 和 drop/partial/overflow/reset/timeout 统计。

当前实现未声称以下项目已在实板验证：深睡 resume、AP/CP 独立重启、FIFO/stale ACK
故障注入和长时间双向压力。这些仍须按移植计划测试矩阵完成。

## 8. 2026-07-31 验证记录

已使用官方 `localhost/bekencorp/armino-idk:1.5` 镜像执行项目级 clean build。
验证结果：

```text
CP PSRAM_HEAP      0x60700000  0x00020000
AP PSRAM_HEAP      0x60720000  0x002e0000
AP PSRAM_SECTION   0x60a00000  0x00600000
all-app.bin        2646016 bytes
app_ab_crc.rbl     2576384 bytes
OpenVela AP SHA256 abd642f2a82842abb08c61a82c205afe5627b809fc47d328458d27bda44895c1
```

该记录只证明当时版本的`nuttx.bin`、`build/openvela-ap.bin`和
`package/tmp/app1.bin`一致。当前OpenVela AP已更新；SMP联调期间的后续hash已移至
`docs/archive/BK7258_OPENVELA_SMP_PORTING_PLAN.md`第15节且同样只作历史对照。当前hash及最终包状态
必须按第4节重新验证，不得引用历史hash作为当前修改已进入最终包的证据。
