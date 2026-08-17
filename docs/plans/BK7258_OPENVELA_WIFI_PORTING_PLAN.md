# 工作目录说明

`contest` 是 BK7258 OpenVela 移植开发仓库。所有正式移植代码必须维护在：

```text
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
├── chips/bk7258/
└── boards/bk7258/bk7258-ap/
```

`bk_avdk_smp` 是 BK7258 原厂 1CP2AP 参考工程。本方案将其 CPU0/CP Wi-Fi 固件、Mailbox 协议和 AP 侧 Wi-Fi 前端作为兼容性基线，但不直接修改 `bk_avdk_smp` 来实现 OpenVela 适配。

`bk_idk` 用于核对芯片寄存器、基础驱动和非 SMP 参考实现。`vendor_beken` 中的 BK7236N OpenVela 代码只用于参考 NuttX 网络设备接入方式，不得复用其芯片寄存器、预编译库、启动文件或链接脚本。

构建和提交要求继续遵守：

- `docs/plans/BK7258_OPENVELA_AP_PORTING_PLAN.md`
- `docs/固件构建步骤.md`
- `docs/github开发指南.md`

# BK7258 OpenVela AP Wi-Fi 移植实施与验证方案

> 文档状态：2026-08-05 实施基线。OpenVela AP Wi-Fi 前端、NuttX `wlan0`、Mailbox CMD/DATA transport、STA 控制面、分页扫描、国家码、direct-push Ethernet 数据面以及最终固件构建已经完成。本文原先固定的“完全不修改 CP”结论已被实际源码证伪：当前 CP 会消费 DHCP/DNS/ARP，且不会向 AP 上报普通 STA connected event，因此必须对 CP 的 `controller_if` 和 VNET 分流做最小兼容扩展。射频、RWNX、WPA、MAC/PHY、校准和原 Mailbox channel ABI 保持不变。当前状态是离线编译和打包完成，不等同于实板扫描、关联、DHCP 和压力测试通过。

## 1. 目标和固定结论

### 1.1 移植目标

在物理 CPU1 单核运行的 OpenVela AP 上实现 Wi-Fi STA 基础能力，同时保持以下内容原样：

- CPU0/CP 的 FreeRTOS、Wi-Fi controller、RWNX、WPA 和射频运行环境。
- CP 侧 Wi-Fi RWNX、UMAC/LMAC、MAC/PHY、RF、校准和共存逻辑。
- CP 侧 `controller_if`、`cif_ipc` 和原厂 Mailbox logical-channel wire ABI。
- bootloader、分区、CP 启动 AP 和 AP heartbeat/PWC 契约。

OpenVela AP 新增：

- 与原厂 `MB_CHNL_WIFI_CMD`、`MB_CHNL_WIFI_DATA` 兼容的 logical channel。
- 原厂 Wi-Fi command/event 协议兼容层。
- AP/CP 共享 Wi-Fi buffer 的地址校验、生命周期和 cache 处理。
- NuttX `netdev_lowerhalf` Wi-Fi 网卡 `wlan0`。
- NuttX wireless handlers，用于配置 SSID、密码、国家码、扫描、连接和断开。
- Wi-Fi transport、控制面和数据面的统计、超时与复位恢复。
- CP 侧 `CONFIG_WIFI_VNET_AP_IPV4` 透明 IPv4/ARP 分流和 STA link event。
- 不含远端 pointer 的 OpenVela scan-page/country 扩展 command。

### 1.2 固定架构

```text
CPU1/OpenVela AP，单核
  ├─ OpenVela application/socket
  ├─ NuttX IPv4/IPv6/TCP/UDP/DHCP/DNS
  ├─ BK7258 wlan0 netdev lower-half
  ├─ Wi-Fi control proxy
  └─ Wi-Fi IPC transport
          │
          │ AP→CP CMD/DATA 0x14/0x15
          │ CP→AP CMD/DATA 0x44/0x45
          │ MBOX0 + 共享 SRAM pointer/list
          ▼
CPU0/Armino CP，保留 controller，增加最小 VNET 兼容扩展
  ├─ controller_if/cif
  ├─ Wi-Fi command/event handler
  ├─ RWNX/FHOST/UMAC/LMAC
  ├─ WPA/关联控制
  └─ Wi-Fi MAC/PHY/RF
```

本方案中：

- CPU0 是 CP，不是 OpenVela AP。
- CPU1 是当前 OpenVela AP primary，也是唯一启用的 AP CPU。
- CPU2 保持当前单核 bring-up 状态，不参与 Wi-Fi ISR、worker 或共享 buffer 管理。
- CP 是 Wi-Fi radio/controller 的唯一所有者。
- NuttX 是 AP 网络协议栈的所有者。
- 第一版采用 copy 接入 NuttX `netpkt_t`，不追求 NuttX packet 与 CP pbuf 零拷贝。

### 1.3 本轮明确不采用的方案

- 不修改 CP 的 RWNX、WPA、MAC/PHY/RF，不增加新 descriptor ring。
- 不把 CP Wi-Fi/RWNX 静态库链接进 OpenVela AP。
- 不让 OpenVela AP直接访问 Wi-Fi MAC/PHY/RF 寄存器。
- 不在 AP 内同时运行 Armino lwIP 和 NuttX 网络栈。
- 不通过 socket RPC 把全部 socket 操作代理到 CP。
- 不改变原厂 `MB_CHNL_WIFI_CMD`、`MB_CHNL_WIFI_DATA` 编号和既有 command ID；仅新增 OpenVela 专用 `0x20d..0x20f`。
- 不在第一阶段启用 SoftAP、P2P、monitor、raw link、CSI、FTM 或零拷贝。
- 不在第一阶段启用 CPU2 SMP。

## 2. 原厂部署和代码依据

### 2.1 CP 运行真实 Wi-Fi controller

CP 配置启用 Wi-Fi 和 virtual network controller：

- `bk_avdk_smp/cp/middleware/soc/bk7258/bk7258.defconfig:99-101`
- `bk_avdk_smp/cp/middleware/soc/bk7258/bk7258.defconfig:286`

CP Wi-Fi 初始化在完成 RWNX/FHOST、RX buffer 和 WPA 线程后初始化 controller interface：

```text
wifi_init()
  -> fhost_rxbuf_push()
  -> wpas_thread_start()
  -> cif_init()
```

代码：

- `bk_avdk_smp/cp/components/bk_wifi/src/wifi_init.c:119-130`
- `bk_avdk_smp/cp/components/controller_if/cif_main.c:319-364`

`cif_init()` 完成：

- `cif_ipc_init()`。
- command/event buffer 初始化。
- CP 本地 queue 和 `cif_thread` 创建。
- host/low-voltage 状态初始化。

### 2.2 原厂 AP 是 Wi-Fi virtual front-end

原 AVDK AP 关闭本地 Wi-Fi controller，但启用 virtual controller：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/bk7258_ap.defconfig:240-243`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/bk7258_ap.defconfig:570`

原 AP 初始化：

```text
bk_wifi_init()
  -> wdrv_init()
      -> wdrv_ipc_init()
      -> wdrv command/event buffer init
      -> wdrv_thread
      -> wdrv_host_init()
  -> host_wlan_add_netif()
```

代码：

- `bk_avdk_smp/ap/components/bk_wifi/src/wifi_api.c:236-269`
- `bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_main.c:356-411`
- `bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_ipc.c:60-109`

OpenVela 不应照搬 `wdrv_main.c` 的 FreeRTOS/lwIP 执行环境；应保留 wire protocol 和业务状态机语义，使用 NuttX primitive 和 `netpkt_t` 重写 AP 前端。

需要特别说明，当前 `app_ab` CP 最终配置中 `CONFIG_WIFI_VNET_CONTROLLER=y`，但 `CONFIG_WIFI_CP_IPC` 未启用：

```text
CONFIG_WIFI_VNET_CONTROLLER=y
# CONFIG_WIFI_CP_IPC is not set
```

这不表示 `controller_if/cif_ipc` 未运行。当前源码中 `wifi_init.c` 直接以 `CONFIG_WIFI_VNET_CONTROLLER` 守卫 `cif_init()`，而 `CONFIG_WIFI_CP_IPC` 在该基线下不是启用这条 Mailbox 数据路径的必要条件。移植和故障定位应以最终链接产物、`CONFIG_WIFI_VNET_CONTROLLER` 及 `cif_init()` 实际调用为准，不能把 `CONFIG_WIFI_CP_IPC=n` 误判成 Wi-Fi IPC 已关闭：

- `bk_avdk_smp/projects/app_ab/cp/config/bk7258/config:419-420`
- `bk_avdk_smp/cp/components/bk_wifi/src/wifi_init.c:128-129`

### 2.3 Wi-Fi logical channel

CPU1/AP 到 CPU0/CP：

```text
MB_CHNL_WIFI_CMD  = 0x14
MB_CHNL_WIFI_DATA = 0x15
```

CPU0/CP 到 CPU1/AP：

```text
MB_CHNL_WIFI_CMD  = 0x44
MB_CHNL_WIFI_DATA = 0x45
```

原厂定义：

- AP：`bk_avdk_smp/ap/include/driver/mailbox_channel.h:77-96`
- CP：`bk_avdk_smp/cp/include/driver/mailbox_channel.h:64-83`

通道值由 `CPX_LOG_CHNL_START(src_cpu, dst_cpu)` 编码，其中 bit[5:4] 是源 CPU、bit[7:6] 是目标 CPU。AP 侧 `SELF_CPU=MAILBOX_CPU1`、目标为 CPU0，所以 AP→CP 是 `0x14/0x15`；CP 侧 `SELF_CPU=MAILBOX_CPU0`、目标为 CPU1，所以 CP→AP 是 `0x44/0x45`。后续所有 TX/RX 白名单和抓包分析必须按该方向解释。

AP 侧映射：

- `bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_ipc.h:17-18`
- `bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_ipc.c:29-45`

CP 侧映射：

- `bk_avdk_smp/cp/components/controller_if/cif_ipc.h:17-18`
- `bk_avdk_smp/cp/components/controller_if/cif_ipc.c:44-57`

两侧都使用 `mb_chnl_open()`、`mb_chnl_write()` 和 `mb_chnl_ctrl()`，并分别注册 RX 与 TX-complete callback：

- AP：`bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_ipc.c:72-107`
- CP：`bk_avdk_smp/cp/components/controller_if/cif_ipc.c:59-103`

### 2.4 Wi-Fi data wire ABI

Mailbox logical payload 是 16 字节 `ipc_chnl_node_t`：

```c
typedef struct
{
    uint32_t ipc_hdr;
    uint32_t head;
    uint32_t tail;
    uint8_t channel;
    uint8_t num;
    uint16_t rsve;
} ipc_chnl_node_t;
```

依据：

- `bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_ipc.h:38-47`

数据对象使用 `cpdu_t`：

```c
struct common_header
{
    uint16_t length;
    uint8_t type:4;
    uint8_t dst_index:4;
    uint8_t need_free:1;
    uint8_t is_buf_bank:1;
    uint8_t vif_idx:2;
    uint8_t special_type:3;
    uint8_t rsve:1;
};

typedef struct cpdu_t
{
    struct cpdu_t *next;
    struct common_header co_hdr;
} cpdu_t;
```

依据：

- `bk_avdk_smp/cp/components/controller_if/cif_main.h:242-289`

AP→CP TX 通过 `head/tail/num` 传递 command 或 MSDU 链：

- `bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_tx.c:113-180`

CP→AP RX 使用相同 node 和链表协议：

- `bk_avdk_smp/cp/components/controller_if/cif_main.c:149-220`

原 AP 接收后直接按照 Beken `pbuf + cpdu_t` 布局解引用：

- `bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_rx.c:152-207`

这是 W1 最大兼容约束。OpenVela 不得假设 CP 能理解 NuttX `netpkt_t` 或 `iob_s`。

### 2.5 Wi-Fi command/confirmation

原 AP command 结构带 `cmd_id` 和 `cmd_sn`，等待 confirmation 时按：

```text
confirmation ID = command ID + WDRV_CMD_CFM_OFFSET
confirmation sequence = command sequence
```

进行匹配：

- command sequence 和等待：`bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_tx.c:307-378`
- confirmation 匹配：`bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_rx.c:13-43`

CP 公开的 Wi-Fi API command ID 包括：

- Scan：`0x300..0x304`
- STA：`0x310..0x31d`
- SoftAP：`0x320..0x329`
- Power save：`0x330..0x332`
- Common：`0x360..0x371`

定义：

- `bk_avdk_smp/cp/components/controller_if/cif_wifi_api.h:11-166`

第一版只实现以下子集：

| 功能 | 原厂 command |
| --- | --- |
| 扫描开始/停止/结果 | `SCAN_START`、`SCAN_STOP`、`SCAN_RESULT`、`SCAN_RESULT_FREE` |
| STA 配置 | `STA_SET_CONFIG`、`STA_GET_CONFIG` |
| STA 启停 | `STA_START`、`STA_STOP` |
| 链路状态 | `STA_GET_LINK_STATUS`、`STA_GET_LINK_STATE_WITH_REASON` |
| MAC | `STA_GET_MAC` |
| 国家码 | `WIFI_SET_COUNTRY`、`WIFI_GET_COUNTRY` |
| 通用状态 | `GET_STATUS` |

## 3. 当前 OpenVela 适配状态

### 3.0 2026-08-05 实际实现状态

本节后续的“阻断缺口”表是实施前审计记录，不能再作为当前源码状态。当前已完成：

- `bk7258_mailbox_channel.c` 支持 AP TX `0x14/0x15`、CP RX `0x44/0x45`、per-channel RX callback、8-slot ACK、timeout、probe 和 peer-reset generation。
- Mailbox callback 返回 `-EAGAIN` 时保留物理 RX descriptor，不发送错误 ACK；Wi-Fi worker 释放容量后主动 kick 重试。
- `bk7258_wifi.c` 注册 `NET_LL_IEEE80211` lower-half，实际设备名为 `wlan0`。
- STA 使用 `BK_CMD_CONNECT`、`BK_CMD_DISCONNECT`、`BK_CMD_GET_MAC_ADDR` 和 `BK_CMD_GET_WLAN_STATUS`；密码不写普通日志。
- 扫描结果使用 `0x20d` 固定宽度分页 confirmation，国家码使用 `0x20e/0x20f`，不再跨核传递 `wifi_scan_result_t *` 或其他私有 pointer。
- TX 兼容当前 16-byte lwIP pbuf、708-byte RWNX headroom 和 8-byte CPDU；RX 最多校验 60 节点，复制到 NuttX `netpkt_t` 后通过可重试 recycle queue 归还。
- CP RAM MPU 为 RW/XN/non-cacheable/shareable，因为 command/event pattern、pbuf ref 和 `need_free` 是原厂所有权协议的一部分。
- CP `CONFIG_WIFI_VNET_AP_IPV4=y` 时仅将基础设施 STA 的 ARP/IPv4 交给 AP，EAPOL、SoftAP 和 P2P 仍归 CP；CP STA DHCP/ARP reply 被关闭。
- NuttX 已启用 IPv4、ARP、ICMP、TCP、UDP、WAPI、DHCP client 和 1514-byte frame。
- OpenVela 和 CP 均通过 `-Werror` 构建，最终 `all-app.bin` 已生成且 AP 输入哈希一致。

仍未完成：实板扫描/关联/DHCP/DNS/TCP/UDP、断线重连、独立复位、长稳和吞吐验收。

### 3.1 已完成且可复用

当前 contest 芯片层已经具备：

- CPU1 MBOX0 channel 1 寄存器配置。
- FIFO start=2、length=3。
- NuttX IRQ 79 注册和 RX FIFO 循环 drain。
- 两字 pointer/length envelope。
- 原厂 16-byte logical header 编码。
- logical channel、sequence 和 ACK 匹配的基础实现。
- 物理通道串行发送、200 ms busy timeout 和 10 ms retry worker。
- CP SRAM envelope 地址、长度和对齐校验。
- HW_CTRL power-up/heartbeat、PWC 和 UART0 日志基础通道。

代码：

- `contest/.../board/beken/chips/bk7258/bk7258_mbox0.c:19-124`
- `contest/.../board/beken/chips/bk7258/bk7258_mailbox_channel.c:167-239`
- `contest/.../board/beken/chips/bk7258/bk7258_mailbox_channel.c:241-303`
- `contest/.../board/beken/chips/bk7258/bk7258_mailbox_channel.c:305-375`

### 3.2 主移植文档中的旧状态修正

当前实际代码已经将 CP envelope 校验窗口改为：

```text
0x28064000 <= pointer < 0x2809f700
```

代码：

- `contest/.../bk7258_mailbox_channel.c:33-34`
- `contest/.../bk7258_mailbox_channel.c:305-315`

因此 `docs/plans/BK7258_OPENVELA_AP_PORTING_PLAN.md` 中“当前只接受 AP RAM”的描述已过期。当前问题不是 CP 16-byte envelope 无法接收，而是：

- 只接受 `sizeof(struct mb_message)`，不能据此信任任意 Wi-Fi payload。
- 只分派 PWC `0x42` 和 UART0 `0x49`。
- 没有 Wi-Fi buffer/list 的逐节点地址、长度、数量和循环校验。

实施时以当前代码为准。

### 3.3 实施前 Wi-Fi 接入阻断缺口（历史记录）

| 缺失项 | 当前表现 | Wi-Fi 后果 |
| --- | --- | --- |
| Wi-Fi channel | AP TX仅有 `0x10/0x12/0x19`，AP RX仅接受 `0x42/0x49` | AP TX `0x14/0x15`、AP RX `0x44/0x45` 被拒绝 |
| 通用 channel open/close | 固定三元素数组 | 无法按原厂 API 初始化 Wi-Fi |
| per-channel RX callback | 只有 PWC callback | command/event/data 无接收入口 |
| per-channel TX-complete | 不存在 | `sending_flag` 无法清除，buffer 无法回收 |
| ACK queue | 单个 `g_ack_message` | burst 时 ACK 可被覆盖 |
| channel queue | 每通道单个 pending slot | Wi-Fi command/data 容易 `-EBUSY` |
| shared payload validation | 只校验 16-byte envelope | 跨核链表可能越界、循环或伪造 |
| pbuf compatibility | 不存在 | CP 无法理解 NuttX packet layout |
| command confirmation | 不存在 | scan/connect/status 无同步结果 |
| reset epoch | reset 只清 physical busy | 旧 pointer/ACK 可能污染新会话 |
| cache ownership | 只有 `dmb sy` | cache 开启后可能读到旧 descriptor/payload |
| NuttX netdev | 未启用网络配置和 driver | 无 `wlan0`、socket 或 DHCP |

关键当前源码：

- 固定通道：`contest/.../bk7258_mailbox_channel.c:67-73`
- 单 ACK：`contest/.../bk7258_mailbox_channel.c:76-77`
- 单 pending：`contest/.../bk7258_mailbox_channel.c:48-52`
- RX 白名单：`contest/.../bk7258_mailbox_channel.c:404-409`
- 公共头文件缺少 logical-channel API：`contest/.../hardware/bk7258_mbox.h:20-36`

## 4. 正式实施架构

### 4.1 代码分层

当前 contest 的 BK7258 chip/board 实际增加：

```text
chips/bk7258/
├── bk7258_mailbox_channel.c      # 扩展为通用 logical channel
├── bk7258_wifi.c                 # transport/control/netdev/worker
└── hardware/
    ├── bk7258_mbox.h
    └── bk7258_wifi_ipc.h         # 只含固定宽度 wire protocol

boards/bk7258/bk7258-ap/src/
└── bk7258_bringup.c              # 在 Mailbox/PWC ready 后启动 Wi-Fi
```

`bk7258_wifi_ipc.h` 只允许包含：

- 固定宽度整型。
- logical channel ID。
- `ipc_chnl_node_t`。
- `common_header` 和必要 command/event wire header。
- 原厂 command ID。
- 编译时 `sizeof`、`offsetof` 和 alignment assertion。

不得包含：

- `os/os.h`。
- FreeRTOS/Beken thread、queue、semaphore 类型。
- lwIP `pbuf` 作为 NuttX runtime 类型。
- CP 私有 driver 对象。

### 4.2 logical channel 扩展

第一阶段将当前固定实现改成通用表：

```c
struct bk7258_logical_channel
{
  uint8_t tx_id;
  uint8_t rx_id;
  bool opened;
  bool pending;
  mb_rx_callback_t rx;
  mb_tx_complete_t tx_complete;
  void *arg;
  struct mb_message tx_message;
};
```

至少注册：

```text
HW_CTRL  0x10/0x40
PWC      0x12/0x42
WIFI_CMD AP TX/RX 0x14/0x44
WIFI_DATA AP TX/RX 0x15/0x45
UART0    0x19/0x49
```

实现语义必须与原厂一致：

- channel 值越小，物理调度优先级越高。
- 每个 logical command 都写入递增 `tx_seq`。
- transport ACK 必须匹配 logical channel 和 sequence。
- 无 handler 的合法通道返回 `CHNL_STATE_COM_FAIL`。
- TX completion 只在匹配 ACK 后调用。
- timeout/reset 对 active transaction 回报失败，不能静默清 busy。

### 4.3 Wi-Fi command transport

AP command 路径：

```text
wireless_ops/ioctl
  -> bk7258_wifi_control_request()
  -> 分配 command buffer
  -> 填写 cmd_id/cmd_sn/payload
  -> cpdu_t header
  -> ipc_chnl_node_t(head=tail, num=1, channel=TX_BK_CMD_DATA)
  -> MB_CHNL_WIFI_CMD 0x14
  -> transport ACK
  -> CP command confirmation 0x44
  -> 按 cmd_id + sequence 唤醒等待者
```

必须区分：

- transport ACK：CP 已接管 `ipc_chnl_node_t`。
- command confirmation：CP 已执行业务命令并返回结果。

控制请求使用 NuttX mutex + semaphore，要求：

- 每个请求有有界 timeout。
- timeout 后从 pending list 删除。
- CP/AP reset 时全部 pending request 返回 `-ECONNRESET`。
- 迟到 confirmation 只计数，不访问已释放 request。
- 不能在 Mailbox ISR 中阻塞。

### 4.4 Wi-Fi TX 数据面

由于 CP 不修改，CP 仍要求 Beken `pbuf/cpdu_t` 兼容布局。第一版在 AP 定义最小 wire-compatible TX allocation，不把 lwIP 协议栈带入 NuttX：

```text
[wire pbuf-compatible metadata][cpdu_t][Ethernet frame]
```

是否必须完整复制原厂 `struct pbuf`，必须通过 CP `cif_handle_txdata()` 和实际编译配置确认。不能凭 `cpdu_t` 单独推断。正式编码前要固定：

- `sizeof(struct pbuf)`。
- payload 指针相对 pbuf/cpdu 的布局。
- `CONFIG_CONTROLLER_AP_BUFFER_COPY` 最终值。
- CP 是否修改 pbuf refcount、flags、next 和 payload。
- TX completion 是通过反向 `need_free` packet 还是 command confirmation。

当前 `app_ab` direct-push 基线已经显示，CP 的 `cif_handle_txdata()` 会用 `head - sizeof(struct pbuf)` 还原并直接访问 AP 创建的 `struct pbuf`；TX 完成时 `cif_free_ap_txbuf()` 把同一对象的 `need_free` 置 1，再通过 CP→AP `WIFI_DATA 0x45` 返回。因此 W3 不是只兼容一个抽象的“pbuf-like metadata”，而是必须冻结并复现该最终构建所使用的完整 lwIP `struct pbuf` ABI，包括字段大小、顺序、对齐、payload 指针、refcount 和 `pbuf + 1 == cpdu` 布局。若无法证明 NuttX AP RAM 地址可被 CP 直接访问，或者无法安全复现该 ABI，则“不修改 CP”的 W1 数据面方案被阻断；不得继续靠猜测实现。

依据：

- `bk_avdk_smp/cp/components/controller_if/cif_wifi_dp.c:138-179`
- `bk_avdk_smp/cp/components/controller_if/cif_mem_mgmt.c:8-24`
- `bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_tx.c:49-111`

TX 路径：

```text
NuttX transmit(netpkt)
  -> 从 AP Wi-Fi TX pool 取对象
  -> netpkt_copyout() 到兼容 Ethernet payload
  -> 设置 cpdu.length/type/vif/need_free
  -> 保存 netpkt 和 shared object 的 pending 关系
  -> MB_CHNL_WIFI_DATA
  -> CP 接管并发往 Wi-Fi
  -> CP 返回 TX completion/free indication
  -> 释放 shared object 和 netpkt
  -> netdev_lower_txdone()
```

`transmit()` 必须遵守：

- `openvela/nuttx/include/nuttx/net/netdev_lowerhalf.h:159-169`

返回 `OK` 后 driver 已接管 `netpkt`，必须在完成路径释放；返回负 errno 时由 upper half 回收。

第一版必须 copy，不能把 NuttX heap pointer 或 `netpkt_t *` 直接发送给 CP。

### 4.5 Wi-Fi RX 数据面

CP→AP `ipc_chnl_node_t` 到达后：

1. Mailbox ISR 复制 CP→AP `WIFI_DATA 0x45` 的 16-byte node，只做不需要解引用远端 pointer 的 `channel/num` 基础检查并投递 RX worker。
2. RX worker 校验 `head/tail` 地址窗口和对齐。
3. RX worker 按 `num` 遍历 CP 链，每一步校验 pointer、最小 header、length、next 和是否循环。
4. RX worker为合法 frame分配 `NETPKT_RX` 并复制 Ethernet frame。
5. 无论复制成功、队列满还是帧被拒绝，都必须按冻结后的所有权协议决定回收、暂缓或触发 channel reset；不能既丢 descriptor 又不归还 CP buffer。
6. 按原厂协议向 CP 返回 free/recycle indication；该方向使用 AP→CP `WIFI_DATA 0x15`，具体 `need_free`/buffer-bank 状态必须在 W0 trace 中冻结。
7. 调用 `netdev_lower_rxready()`；`receive()` 返回准备好的 packet。

禁止在 Mailbox ISR 中：

- 调用完整 NuttX IP input。
- 动态执行复杂 command handler。
- 等待 semaphore。
- 递归发送大量 free message。
- 直接释放 CP heap pointer。

原 AP 的 RX 逻辑参考：

- `bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_rx.c:72-149`
- `bk_avdk_smp/ap/components/bk_wifi_driver/wdrv_rx.c:152-207`

### 4.6 跨核 pointer 校验

W1 必须接受原厂 CP 指针协议，但每次解引用前都要校验。至少包括：

- pointer 4-byte 对齐。
- pointer 位于允许的 CP RAM/PSRAM shared window。
- `length` 不小于 header，不超过协商的最大 frame。
- `num` 不超过固定上限。
- `next` 每次都位于允许窗口。
- 链表实际节点数不超过 `num`。
- 最后节点和 `tail` 一致。
- 地址加长度无 32-bit overflow。
- 同一地址不能在当前 ownership table 中重复出现。
- reset epoch 后不接受旧 transaction 中的 pointer。

当前 `CP_RAM_START/END` 硬编码只能作为 bring-up 值：

- `contest/.../bk7258_mailbox_channel.c:33-34`

正式实现应由目标分区生成结果导出 shared window，不能在 Wi-Fi driver 再复制一组常量。

### 4.7 Cache 和内存属性

首版要求 Wi-Fi shared pool 位于双方都可访问的 non-cacheable、shareable 区域。若无法做到，所有权交接必须按以下顺序：

发送端：

```text
写 payload
写 descriptor/header
clean payload cache
clean descriptor cache
DMB
发送 mailbox doorbell
```

接收端：

```text
收到 mailbox
invalidate descriptor cache
读取并校验 descriptor
invalidate payload cache
读取 payload
处理完成后 clean ownership/completion
DMB
发送 ACK/free indication
```

`volatile` 和 `dmb sy` 不能替代 cache maintenance。

### 4.8 Reset 和恢复

不改变原厂 wire descriptor 意味着不能增加新 epoch 字段，因此 AP 必须在本地维护 transport generation，并使用 logical channel reset 行为完成重同步：

```text
检测 CP reset/channel reset/heartbeat failure
  -> carrier off
  -> 阻止新 TX
  -> 所有 control request 返回 -ECONNRESET
  -> 回收 AP 自有 TX packet
  -> 丢弃尚未交给 NuttX 的 CP RX descriptor
  -> 清理 channel active/pending/ACK queue
  -> 重新 open WIFI_CMD/WIFI_DATA
  -> 重新获取 MAC/status
  -> 恢复 STA config
  -> 重新连接
  -> 收到 connected event 后 carrier on
```

由于 CP wire 协议本身没有 generation，旧 CP pointer 的安全回收能力有限。当前实现用 peer-reset generation、deferred descriptor 丢弃和 ownership quarantine 降低风险；没有明确 completion 的 AP TX slot 不会被猜测性复用。

## 5. NuttX 网络接入

### 5.1 netdev lower-half

已实现：

```c
static int bk7258_wlan_ifup(struct netdev_lowerhalf_s *dev);
static int bk7258_wlan_ifdown(struct netdev_lowerhalf_s *dev);
static int bk7258_wlan_transmit(struct netdev_lowerhalf_s *dev,
                                netpkt_t *pkt);
static netpkt_t *bk7258_wlan_receive(struct netdev_lowerhalf_s *dev);
static void bk7258_wlan_reclaim(struct netdev_lowerhalf_s *dev);
```

NuttX接口依据：

- `openvela/nuttx/include/nuttx/net/netdev_lowerhalf.h:117-192`
- `openvela/nuttx/include/nuttx/net/netdev_lowerhalf.h:267-354`

BK7236N 参考只用于 NuttX 接口形态：

- `vendor_beken/chips/bk7236n/beken_wlan.c:54-123`
- `vendor_beken/chips/bk7236n/beken_wlan.c:167-303`

不得直接复用其 `bk_wifi_send_tx_eth()` 等单核接口。

### 5.2 carrier 语义

`ifup()` 只表示 driver 启动成功，不表示 STA 已关联。正确语义：

```text
ifup
  -> transport ready
  -> STA_START/启用控制面
  -> 保持 carrier off

CP STA_CONNECTED event
  -> netdev_lower_carrier_on()

CP STA_DISCONNECTED event
  -> netdev_lower_carrier_off()
```

不要照搬 BK7236N 在 `ifup()` 后立即 carrier-on 的行为。

### 5.3 wireless handlers

启用 `CONFIG_NETDEV_WIRELESS_HANDLER` 后实现：

- `connect`
- `disconnect`
- `essid`
- `passwd`
- `auth`
- `country`
- `scan`
- 必要的 `range`

接口：

- `openvela/nuttx/include/nuttx/net/netdev_lowerhalf.h:194-231`

BSSID、bitrate、txpower 和 sensitivity 当前 handler 为 `NULL`，不会伪造成功。

### 5.4 网络栈所有权

正式目标是 NuttX 作为 AP IP 栈。必须核对 CP `cif_wifi_dp` 对 ARP、IPv4、IPv6 和广播包的过滤逻辑，确保 AP 获得完整 Ethernet 流量。若 CP 仍消费 DHCP/ARP 或只转发特定业务包，`wlan0` 无法成为透明接口。

当前实现不再假设原 CP 已经提供透明 Ethernet。必须从 CP 最终 `sdkconfig.h` 固定并记录：

- `CONFIG_WIFI_VNET_CONTROLLER`
- `CONFIG_WIFI_CP_IPC` 的最终值，并记录它在当前源码中不负责守卫 `cif_init()`。
- `CONFIG_CONTROLLER_AP_BUFFER_COPY`
- RX direct-push/filter 相关配置
- CP lwIP 是否保留本地 IP

当前 `app_ab` 生成配置已经确认：

```text
CONFIG_WIFI_VNET_CONTROLLER=y
CONFIG_CONTROLLER_AP_BUFFER_COPY=n
CONFIG_CONTROLLER_RX_DIRECT_PSH=y
CONFIG_WIFI_VNET_AP_IPV4=y
```

依据：

- `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.h:124-129`
- `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.cmake:256`
- `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258_ap/config/sdkconfig.h:153-154`

当前基线走原厂 direct-push/共享 pbuf 所有权路径，不启用 CP copy adapter。`CONFIG_WIFI_VNET_AP_IPV4` 只改变基础设施 STA 的 IP/ARP 所有权，不改变 EAPOL 或 Wi-Fi controller 所有权。

原 CP 配置不能向 AP 提供完整 Ethernet frame，因此当前实现显式启用 `CONFIG_WIFI_VNET_AP_IPV4`；不能通过 AP 侧猜测绕过 CP 的过滤和 IP 所有权。

## 6. Kconfig 和构建接入

实际新增和启用：

```kconfig
config BK7258_WIFI
    bool "BK7258 CP-backed Wi-Fi"
    depends on ARCH_CHIP_BK7258
    select NET
    select NETDEV_LATEINIT

config BK7258_WIFI_TX_SLOTS
    int "Wi-Fi AP TX buffer count"
    default 8

```

最终配置至少需要：

```text
CONFIG_NET=y
CONFIG_NET_ETHERNET=y
CONFIG_NETDEV_LATEINIT=y
CONFIG_NETDEV_WIRELESS_IOCTL=y
CONFIG_NETDEV_WIRELESS_HANDLER=y
CONFIG_NET_IPv4=y
CONFIG_NET_TCP=y
CONFIG_NET_UDP=y
CONFIG_NET_ICMP=y
CONFIG_NET_ARP=y
CONFIG_NETUTILS_DHCPC=y
CONFIG_BK7258_WIFI=y
CONFIG_BK7258_WIFI_TX_SLOTS=8
CONFIG_WIRELESS_WAPI=y
CONFIG_WIRELESS_WAPI_CMDTOOL=y
CONFIG_SYSTEM_DHCPC_RENEW=y
```

IPv6、DNS 和应用协议按阶段增加。具体 symbol 以当前 OpenVela Kconfig 为准，配置后必须检查最终 `.config`，不能只修改 defconfig。

新增源码必须同时加入：

- `chips/bk7258/CMakeLists.txt`
- `chips/bk7258/Make.defs`

当前 CMake 只包含启动、IRQ、timer、UART、Mailbox、PWC 和 heap：

- `contest/.../chips/bk7258/CMakeLists.txt:3-13`

## 7. 实施步骤

### 阶段 W0：冻结原厂 wire ABI（离线完成）

完成以下输出物：

- AP/CP channel ID 对照表。
- `ipc_chnl_node_t`、`cpdu_t`、`common_header` 以及最终 lwIP `struct pbuf` 的 `sizeof/offsetof/alignment`。
- AP TX 对象满足 `pbuf + 1 == cpdu`、payload/refcount/next 字段语义的双工具链 ABI 验证。
- 最终 CP `sdkconfig.h` 中 buffer-copy 和 VNET 配置。
- command、confirmation 和 event header 格式。
- TX completion、RX release 和 `need_free` 的状态图。
- AP_RAM、CP_RAM、PWR_MNG、SWAP、PSRAM 的双方可见地址窗口。
- golden AVDK AP/CP 的 Mailbox trace。

未完成这些项目不得开始数据面编码。

### 阶段 W1：通用 logical channel（实现并通过构建）

实现并验证：

- AP TX/RX `0x14/0x44`。
- AP TX/RX `0x15/0x45`。
- open/close/write/control。
- RX callback。
- TX-complete callback。
- ACK queue。
- timeout/reset completion。
- 统计查询。

验收：每个通道双向 10,000 次 command/ACK，无永久 busy、ACK 覆盖和 sequence 错配。

### 阶段 W2：Wi-Fi 控制面（实现并通过构建）

按顺序实现：

1. transport/channel ready。
2. `STA_GET_MAC`。
3. `GET_STATUS`。
4. `WIFI_GET_COUNTRY/WIFI_SET_COUNTRY`。
5. `SCAN_START/SCAN_RESULT/SCAN_RESULT_FREE`。
6. `STA_SET_CONFIG`。
7. `STA_START/STA_STOP`。
8. connected/disconnected event。

此阶段不注册 `wlan0` 数据面也可以完成。验收包括扫描、设置 SSID/密码、关联和断开。

### 阶段 W3：NuttX `wlan0` 和 TX（实现并通过构建）

实现：

- `netdev_lower_register(..., NET_LL_IEEE80211)`。
- MAC address 设置。
- TX pool。
- `netpkt_copyout()`。
- CP pbuf/cpdu wire-compatible object。
- TX completion/free。
- `netdev_lower_txdone()`。

先发送 ARP/ICMP 小包，再验证 MTU 边界和 fragmented `netpkt`。

### 阶段 W4：RX（实现并通过构建）

实现：

- CP node/list 校验。
- RX worker。
- `netpkt_alloc/copyin()`。
- CP buffer release。
- `netdev_lower_rxready()`。
- RX queue backpressure。

验收：ARP、DHCP 和 ping。

### 阶段 W5：完整网络功能（等待实板验收）

依次验证：

- DHCP renew。
- DNS。
- TCP client/server。
- UDP。
- IPv6/NDP。
- Wi-Fi断线重连。
- AP复位和CP复位恢复。
- OTA 使用路径，但不直接擦写 Flash。

### 阶段 W6：稳定性和性能（未开始）

验证：

- 72 小时联网。
- TCP/UDP并发。
- burst RX/TX。
- Mailbox日志、heartbeat、PWC 和 Wi-Fi并发。
- buffer pool耗尽和恢复。
- 网络压力下无 heartbeat timeout。
- CPU1 单核负载和栈水位。

零拷贝、descriptor ring 和 CPU2 SMP 不属于该阶段。

## 8. 测试和门禁

### 8.1 Transport 门禁

- Wi-Fi CMD/DATA 双向各 10,000 次。
- FIFO full 可自动恢复。
- transport ACK 与 command confirmation 不混淆。
- bad source、bad pointer、bad length、bad list、循环 list 全部拒绝。
- timeout 后 channel 不永久 busy。
- AP/CP reset 后可以重新 open。

### 8.2 控制面门禁

- 连续扫描 100 次无泄漏。
- 连接正确密码成功。
- 错误密码返回明确失败原因。
- 断线 event 驱动 carrier off。
- 重新连接不复用旧 confirmation。
- SSID/password 不出现在普通日志中。

### 8.3 数据面门禁

- DHCP 成功。
- ping gateway 和公网地址成功。
- DNS 成功。
- TCP/UDP 双向正常。
- 64、512、1500 字节 Ethernet frame 正常。
- TX pool耗尽返回 backpressure，不覆盖 buffer。
- RX queue满时按明确策略丢包并计数。
- AP/CP reset 后没有 double free 或旧 pointer 解引用。

### 8.4 性能基线

第一版不规定等同原 AVDK 的吞吐，但必须记录：

- TCP uplink/downlink。
- UDP uplink/downlink 和丢包率。
- Wi-Fi TX/RX copy 次数。
- 每包 Mailbox 数量。
- AP CPU 使用率。
- RX/TX queue峰值。
- buffer pool 最低空闲数。

## 9. 故障定位顺序

### Wi-Fi command无响应

1. 确认 CP 最终配置启用 `CONFIG_WIFI_VNET_CONTROLLER`。
2. 确认 AP TX `0x14` 已 open，CP→AP `0x44` 未被 AP RX 白名单拒绝。
3. 检查 transport ACK channel/sequence。
4. 检查 command `cmd_id/cmd_sn`。
5. 检查 CP pointer窗口和 command buffer布局。
6. 检查 confirmation ID 和 sequence。

### 已关联但无 DHCP

1. 确认 carrier 只在 connected event 后打开。
2. 确认 AP 收到广播、ARP 和 DHCP frame。
3. 检查 CP filter 是否仍把流量留给 CP lwIP。
4. 检查 Ethernet header 是否完整。
5. 检查 RX release是否过早。
6. 检查 NuttX netdev link type。

### TX 卡死

1. 检查 logical TX-complete是否到达。
2. 区分 transport ACK 和 Wi-Fi TX completion。
3. 检查 `sending_flag` 或 NuttX pending table。
4. 检查 CP 是否等待错误的 pbuf/cpdu布局。
5. 检查 buffer ownership和 `need_free`。
6. 检查 FIFO full后 ACK queue是否丢失。

### 随机数据损坏

1. 检查 pointer窗口。
2. 检查 `sizeof/offsetof/packing`。
3. 检查 cache clean/invalidate。
4. 检查 AP 是否在 CP 完成前复用 TX buffer。
5. 检查 CP RX buffer是否在 NuttX copy完成前释放。
6. 检查 reset后旧 descriptor。

## 10. 风险和待确认事项

### 10.1 原厂 pointer ABI 和最小 CP 扩展的固有限制

- Wire ABI 包含 32-bit pointer 和 Beken pbuf布局，不是独立于 OS 的稳定协议。
- CP 无 protocol version/epoch，跨版本和 reset 恢复能力有限。
- 数据面是否能向 NuttX 提供完整 Ethernet frame 取决于 CP 最终 VNET/filter 配置。
- CP/AP 共享 PSRAM 和 cache属性需要原厂资料或实测确认。
- 原厂 W1 的 ownership代码包含大量历史分支，必须以最终配置裁剪状态为准。

### 10.2 量产前必须确认

- CP Wi-Fi IPC ABI 是否承诺版本稳定。
- shared pointer允许窗口和 DMA master访问属性。
- cache line、clean/invalidate语义和 barrier要求。
- CP reset时 Wi-Fi buffer回收规则。
- WPA密钥和凭据跨核传输的安全边界。
- AP/CP网络栈所有权和低功耗唤醒职责。

## 11. 完成定义

当前“实现和最终固件构建”已完成；满足以下全部条件后才算完成实板 Wi-Fi 移植验收：

- CP 仅包含已记录的 VNET IPv4/event/scan-page/country 兼容扩展，controller 保持原样。
- CPU1/OpenVela 单核持续正常运行，CPU2未作为依赖。
- 原厂 AP→CP `0x14/0x15`、CP→AP `0x44/0x45` 协议兼容。
- logical channel具备 RX、TX-complete、ACK queue、timeout和reset恢复。
- `wlan0` 注册为 NuttX netdev。
- 扫描、连接、断开和状态查询正常。
- NuttX拥有 IP、DHCP、DNS、TCP/UDP 数据面。
- Wi-Fi TX/RX buffer无覆盖、泄漏、double free和越界访问。
- AP/CP任一侧复位后可恢复网络。
- Wi-Fi压力不破坏 heartbeat、PWC和Mailbox UART日志。
- 所有协议结构和最终 CP 配置已记录，构建可重复。

截至 2026-08-05，前述代码、配置和最终构建条件已满足；扫描、连接、DHCP、数据面、复位恢复和压力条件尚未在实板证明，因此不能将本次离线构建写成“Wi-Fi 实板移植验收完成”。

CPU2 SMP、SoftAP、P2P、monitor、CSI、FTM、零拷贝和高性能 descriptor ring 是后续工作，不属于本文完成定义。

SoftAP 后续实施边界、CP/NuttX IPv4 与 DHCP 所有权、STA/AP 互斥切换和双网卡
演进条件见 `BK7258_OPENVELA_WIFI_AP_MODE_PORTING_PLAN.md`。该文档已经选定
单 `wlan0` 互斥运行时切换为首选方案，但不代表 SoftAP 已开始移植或已经验收。
