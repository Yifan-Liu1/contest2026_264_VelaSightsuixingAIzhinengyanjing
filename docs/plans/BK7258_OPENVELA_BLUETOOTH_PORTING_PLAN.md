# BK7258 OpenVela AP Bluetooth 移植实施与验证方案

> 核验日期：2026-08-13
> 状态：实施基线，尚未实现。当前有效 OpenVela 配置为
> `CONFIG_NCPUS=1`，Bluetooth 配置尚未启用。本文只把已经由源码、最终
> `.config` 或生成 `map` 证明的内容写成现状；需要实板确认的内容均列为门禁，
> 不再写成既成事实。

## 1. 工作区、范围和结论

正式 OpenVela 移植代码只能维护在：

```text
/home/mi/vela_competition/contest/
└── contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
    ├── chips/bk7258/
    └── boards/bk7258/bk7258-ap/
```

参考和构建输入：

- `bk_avdk_smp`：当前最终固件使用的 Armino CPU0/CP、bootloader、分区和打包工程。
- `bk_avdk_smp_original_backup`：只用于比较当前 CP 与工作区原始备份。
- `openvela`：NuttX native BLE Host、`bt_driver_s` 和 STM32WB 双核 HCI 参考。
- `vendor_beken`、`bk_idk`：交叉核验，不是 BK7258 正式源码目录。
- `contest/cmake_out/bk7258-ap_nsh`：当前 CMake 产物；`contest/out` 是旧产物，不能作为当前配置证据。

固定采用以下架构：

```text
CPU1/OpenVela AP
  OpenVela application / btsak
          |
  NuttX native BLE Host
          |
  struct bt_driver_s
          |
  BK7258 BT transport + Mailbox V2
          |  AP -> CP 0x13
          |  CP -> AP 0x43
          v
CPU0/Armino CP
  bt_ipc_core -> HCI adapter -> BLE controller-only library -> RF
```

当前最终 CP `app.map` 实际链接
`libbluetooth_controller_controller_only_ble.a`。CP 配置同时定义
`CONFIG_BTDM_5_2` 和 `CONFIG_BTDM_CONTROLLER_ONLY`，但不能据此把当前镜像写成
经典 BT 双模交付；第一版只支持 BLE Command、Event 和 ACL，不支持 BR/EDR、SCO
和 ISO。

本文交付的是 NuttX native BLE Host 接入，不等于 OpenVela 独立的
Bluetooth Framework/SAL、QuickApp 或 NDK 已经接入。当前构建树没有已 checkout
并参与构建的 `frameworks_bluetooth` 或 `external_zblue` 源码；`.repo` 中存在的
Git 元数据不能作为 Framework 已接入的证据。

### 1.1 CPU 和 CP 边界

- 当前权威 `.config` 是 `CONFIG_NCPUS=1`、`CONFIG_SMP_NCPUS=1`，本轮在 CPU1
  单核配置上实现和验收。
- 代码必须保持 SMP-safe：Mailbox IRQ 仍由物理 CPU1 所有；transport queue、
  ownership table 和 driver state 使用可在未来 SMP 下成立的同步原语。
- 不把“CPU2 永远不参与 Host 状态”写成协议条件。未来启用 SMP 后，NuttX Host
  线程可能调度到 CPU2，除非另行配置 affinity。
- 本 Bluetooth 工作默认不修改 CP `bt_ipc_core` 及其 wire ABI。
- 当前最终 CP 并非未经修改的原厂固件：`cp_main.c` 和 CP
  `mailbox_channel.c` 已有项目级改动。完成条件只能写成“本轮不新增 BT 协议修改”，
  不能写“CP 固件未修改”。

### 1.2 本轮完成范围

必须完成：

- vendor INIT 同步握手。
- 标准 HCI 初始化命令。
- BLE advertising、scan、单连接。
- GATT discovery、read、write、notification。
- AP/CP 双向 HCI wrapper 的 `HCI_FREE_PKT` 生命周期。
- transport timeout、非法 pointer/length、queue/pool 耗尽的受控失败。
- 无在途 HCI payload 时的 vendor DEINIT/INIT 恢复，以及整机复位后的恢复。
- Wi-Fi 与 BLE 基础共存压力测试。

本轮不承诺：

- AP 单独复位而 CP 不复位时的无损自动恢复。
- 只有 logical mailbox reset、无法证明 CP 停止访问旧 pointer 时立即复用 AP buffer。
- bond key 掉电保存。
- 经典 BT、SCO、ISO、多连接和高吞吐 HCI ring。

前两项受原厂 pointer ABI 无 version/epoch 的限制；若产品要求 AP/CP 任一侧独立
复位后自动恢复，必须进入第 11.2 节的 CP 协议演进，不能靠 AP 本地 generation
伪造安全性。

## 2. 已核验的原厂协议

### 2.1 最终配置和初始化链

当前生成配置：

```text
AP reference:
  CONFIG_BLUETOOTH_AP=y
  CONFIG_BLUETOOTH_HOST_ONLY=y
  CONFIG_BLE=y
  CONFIG_BLUETOOTH_SUPPORT_IPC=y

CP final:
  CONFIG_BLUETOOTH=y
  CONFIG_BLE=y
  CONFIG_BTDM_5_2=y
  CONFIG_BTDM_CONTROLLER_ONLY=y
  CONFIG_BLUETOOTH_SUPPORT_IPC=y
```

证据：

- `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258_ap/config/sdkconfig.h`
- `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.h`
- `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/app.map`

原 AP 顺序是 `bt_ipc_init()`、vendor INIT、AP OS/feature adapter、HCI driver、
Armino Host。OpenVela 替换 AP Host 和 AP OS adapter，不改变 CP controller 所有权。
当前 CP 在系统 `bk_init()` 阶段已经调用 `bk_bluetooth_init()`；因此 AP vendor INIT
在当前固件上通常是幂等同步握手，不应描述成必然首次启动 controller。

### 2.2 logical channel

```text
AP -> CP: MB_CHNL_BT_CMD = 0x13
CP -> AP: MB_CHNL_BT_CMD = 0x43
```

编码为 `destination[7:6] | source[5:4] | logical-id[3:0]`。依据：

- `bk_avdk_smp/ap/include/driver/mailbox_channel.h`
- `bk_avdk_smp/cp/include/driver/mailbox_channel.h`
- AP/CP `components/bk_bluetooth/ipc/src/bt_ipc_core.c`

### 2.3 16-byte HCI descriptor ABI

原厂定义：

```c
struct bk7258_bt_descriptor
{
  uint32_t header;       /* offset 0, cmd/state/ctrl/seq/channel */
  uint8_t  packet_type;  /* offset 4 */
  uint8_t  pointer[4];   /* offset 5, little-endian, unaligned */
  uint8_t  reserved[7];  /* offset 9, must be zero in OpenVela */
} __attribute__((packed));
```

packet type：

```text
HCI_COMMAND_PKT  = 0x01
HCI_ACL_DATA_PKT = 0x02
HCI_SCO_DATA_PKT = 0x03
HCI_EVENT_PKT    = 0x04
HCI_FREE_PKT     = 0x0a
```

原 `hci_hdr_t` 是 9 字节，`hdr_ptr` offset 为 5；它与 16 字节
`bt_ipc_cmd_t/mb_chnl_cmd_t` union 共用存储。所有 OpenVela TX descriptor 必须先
清零完整 16 字节，`header.cmd` 固定为 0，再由 Mailbox 层填 state、ctrl、seq 和
channel。pointer 必须用 `memcpy` 或逐字节 helper 访问，不能对 packed offset 5
执行未对齐 `uint32_t` 解引用。

当前 `struct bk7258_mb_wire_message` 的 offset 4 是 `payload_address`，offset 8
是 `payload_length`，与上述 BT descriptor 不兼容。BT 代码不得把 pointer 写入
`message.payload_address`。必须增加 raw 16-byte overlay/accessor；通用 message
和 BT descriptor 只能共享 header 以及“总长 16 字节”这一事实。

建议在 `hardware/bk7258_bt_ipc.h` 固定：

```c
union bk7258_mb_frame
{
  struct bk7258_mb_wire_message generic;
  struct bk7258_bt_descriptor bt;
  uint8_t raw[BK7258_MB_MESSAGE_SIZE];
};

_Static_assert(sizeof(union bk7258_mb_frame) == 16, "mailbox frame size");
_Static_assert(offsetof(struct bk7258_bt_descriptor, packet_type) == 4,
               "BT packet type offset");
_Static_assert(offsetof(struct bk7258_bt_descriptor, pointer) == 5,
               "BT pointer offset");
```

Mailbox transport内部存储、send 和 RX callback改用 `union bk7258_mb_frame`，或提供
等价的 raw 16-byte API。对非 BT channel 继续使用 `.generic`；对 `0x13/0x43`
只使用 `.bt/.raw`，不得读取 `payload_length/flags/crc8`。

### 2.4 payload 和三类完成语义

descriptor 只携带 pointer，实际 HCI payload 位于发送方可被另一核访问的内存：

```c
struct hci_cmd  { le16 opcode; u8 len; u8 params[]; } packed;
struct hci_evt  { u8 event; u8 len; u8 params[]; } packed;
struct hci_acl  { le16 handle_flags; le16 len; u8 data[]; } packed;
```

三类完成语义不能混用：

1. Mailbox ACK：对端 logical layer 已复制 16-byte descriptor。
2. Beken `HCI_FREE_PKT`：对端已处理 wrapper，原分配方可以回收 pointer。
3. HCI Complete/Status 或 Number Of Completed Packets：controller 协议结果/credit。

AP/CP 收到 Event、Command 或 ACL wrapper后会向原分配方回发
`HCI_FREE_PKT(pointer)`。收到 FREE 的原厂 CP 会直接 `os_free(pointer)`，所以 AP
不能把一个未经白名单验证的 CP pointer原样回发给 CP。

### 2.5 vendor INIT ABI

```text
HCI vendor opcode: 0xfefe
INIT sub-opcode:    0x0001
DEINIT sub-opcode:  0x0002

AP -> CP INIT params:       00 01
CP -> AP vendor event data: 01 00 status
```

两条路径的 sub-opcode 字节序不同，必须逐字节编码：

- AP 发送 `[subop >> 8, subop & 0xff]`。
- CP 回复 `[subop & 0xff, subop >> 8, status]`。

CP 回复是 event code `0xfe`，不是普通 HCI Command Complete。transport在 Host
初始化前拦截该 event、验证长度和 sub-opcode、更新 ready状态并 FREE CP wrapper；
不能把它直接提交给尚未 ready 的 NuttX Host。

## 3. 当前实现状态和真正缺口

当前 OpenVela 已具备：

- CPU1 MBOX0 channel 1 FIFO、IRQ79 和 16-byte descriptor deferral。
- `bk7258_mailbox_send_wire()` 通用异步发送和 TX complete callback。
- `bk7258_mailbox_register_rx()` per-channel RX callback。
- 8 个稳定 ACK slots，ACK 优先于 reset 和普通 command。
- 单 active transaction、每 logical channel 一个 pending slot。
- 200 ms timeout、link reset/probe、`peer_reset_generation`。
- CP SRAM `0x28064000..0x2809f6ff` non-cacheable/shareable MPU 映射。

证据位于：

- `board/beken/chips/bk7258/hardware/bk7258_mbox.h`
- `board/beken/chips/bk7258/bk7258_mailbox_channel.c`
- `board/beken/chips/bk7258/bk7258_start.c`

不要重复实现上述能力。真正缺口是：

| 缺口 | 必须实现的结果 |
| --- | --- |
| BT channel ID | TX `0x13`、RX `0x43` 加入白名单和 callback table |
| raw frame | BT offset 4/5 ABI，不复用 generic payload字段 |
| 硬编码索引 | 删除 `g_channels[4]`，按 UART channel ID 查询索引 |
| BT TX queue | 在 generic transport之上增加有界优先级队列 |
| AP TX pool | 固定 AP SRAM wrapper，直到匹配 FREE 才复用 |
| CP pointer校验 | SRAM窗口、总长、overflow、type、重复 descriptor |
| RX worker | ISR只复制 descriptor，worker校验、copy、deliver、FREE |
| NuttX lower-half | `bt_driver_s`、Kconfig、Make/CMake、board bring-up |
| HCI恢复 | quarantine、DEINIT/INIT和明确的不可恢复边界 |
| 测试工具 | `btsak` 和可写/可通知的最小 GATT fixture |

当前每个 logical channel 只有一个 pending slot，第二个请求返回 `-EBUSY`。BT 同一
channel需要发送 FREE、vendor command、HCI command和ACL，因此不能让 Host直接调用
`bk7258_mailbox_send_wire()`。否则 FREE 会被普通 command/ACL 阻塞，且 NuttX TX
线程未必重试 lower-half 的 `-EBUSY`。

## 4. 正式代码结构

新增和修改：

```text
board/beken/chips/bk7258/
├── hardware/bk7258_mbox.h         # raw frame API和0x13/0x43
├── hardware/bk7258_bt_ipc.h       # Beken BT wire ABI、assert和accessor
├── bk7258_mailbox_channel.c       # BT channel、raw dispatch、索引修复
├── bk7258_bt_transport.c          # queue/pool/ownership/RX worker/recovery
├── bk7258_bt_driver.c             # struct bt_driver_s
├── bk7258_bt_selftest.c           # 可选开机raw HCI自检，不注册Host
├── bk7258_bt.h                    # board可调用的初始化/诊断API
├── Kconfig
├── CMakeLists.txt
└── Make.defs

board/beken/boards/bk7258/bk7258-ap/src/
├── bk7258_bringup.c               # link/PWC ready后注册BT
├── bk7258_bt_gatt_test.c          # 可选最小GATT验收服务
├── CMakeLists.txt
└── Makefile
```

所有 BT 源码在 Make/CMake 两侧都受 `CONFIG_BK7258_BLUETOOTH` 条件控制。不要
修改 `openvela/nuttx/wireless/bluetooth` 公共代码来掩盖 controller 错误。

## 5. Transport 设计

### 5.1 Mailbox channel接入

在原厂优先级顺序中，BT 位于 PWC 后、Wi-Fi 前：

```text
HW_CTRL, PWC, BT, WIFI_CMD, WIFI_DATA, UART0
```

修改时同时更新 TX/RX count、ID数组、callback数组、统计和 dump。先把：

```c
g_channels[4].queued
```

改为通过 `channel_index(BK7258_MB_CHAN_UART0_TX)` 获取，避免插入 BT 后 probe
检查错误 channel。

generic Mailbox层仍只负责：

- 赋 logical channel和sequence。
- 一次发送一个 descriptor。
- 匹配 transport ACK。
- timeout和link状态。

它不理解 HCI payload，也不根据 `HCI_FREE_PKT` 回收内存。

### 5.2 BT出站队列

`bk7258_bt_transport.c` 在 generic transport之上维护一个有界队列。建议初值：

```text
CONFIG_BK7258_BT_TX_BUFFERS=8
CONFIG_BK7258_BT_TX_QUEUE=16
CONFIG_BK7258_BT_RX_QUEUE=16
```

调度规则：

1. 已验证 CP pointer 的 `HCI_FREE_PKT`。
2. vendor INIT/DEINIT 和恢复控制。
3. 普通 HCI Command和ACL按全局入队顺序发送，不跨类型重排。

恢复控制只在Host已quiesce后进入队列，不能越过仍有效的普通HCI业务。必须为FREE和
Command保留容量，ACL不能占满所有项。
`bt_driver_s.send()` 在复制 payload并成功入队后返回 `len`；失败返回负 errno。
Command使用保留 slot或有界等待，ACL queue满返回 `-ENOBUFS`。任何路径都不能在
Mailbox ISR中等待 semaphore。

generic TX complete只表示 descriptor ACK：

- ACK成功：payload保持 `IN_FLIGHT`，等待 FREE。
- ACK失败且 descriptor确认未交给 CP：可回收 payload。
- timeout/结果不确定：payload进入 quarantine，不能立即复用。

### 5.3 AP HCI TX pool

第一版固定使用 AP SRAM中的静态小对象池，不使用任意 NuttX heap pointer，也不把
HCI pool放入 PSRAM。当前 AP SRAM和CP SRAM映射均为 non-cacheable，仍需在 B0
设计时核对地址，在 B1通过真实无副作用HCI command证明 CPU0能读取 AP pool内容。

每个对象记录：

```text
state, generation, wire_address, packet_type, length, enqueue_time
```

状态：

```text
FREE -> BUILDING -> QUEUED -> IN_FLIGHT -> FREE_RECEIVED -> FREE
                                  |
                                  +-> QUARANTINED
```

收到 `HCI_FREE_PKT` 时只能按 wire address查 ownership table。仅完全匹配当前
`IN_FLIGHT` 对象才回收；unknown、duplicate、旧 generation和非对象起始地址均记录
错误并触发受控恢复，绝不调用 `kmm_free(remote_pointer)`。

### 5.4 AP发送 Command/ACL

NuttX lower-half只接受 `BT_CMD` 和 `BT_ACL_OUT`：

```text
send(type, data, len)
  -> 验证type、HCI header和len
  -> 从AP pool分配完整wrapper
  -> memcpy NuttX HCI header+payload，不添加H4 byte
  -> 构造清零的16-byte BT descriptor
  -> 入BT优先级队列
  -> 返回len
  -> generic mailbox发送并收到ACK
  -> 等CP发HCI_FREE_PKT
  -> ownership匹配后回收wrapper
```

Host在 `send()` 返回后可以复用原 buffer，所以必须复制。HCI multi-byte字段使用
little-endian helper或 `memcpy`，不能直接访问 packed halfword。

### 5.5 CP接收 Event/ACL

```text
Mailbox IRQ / logical RX callback
  -> 只验证channel、descriptor type
  -> 复制完整16字节到BT RX queue
  -> 不解引用CP pointer

BT RX worker
  -> 解析unaligned pointer
  -> 验证pointer窗口和最小header
  -> 用memcpy读取Event/ACL length
  -> 验证pointer + total不overflow、不越窗口、不超Host上限
  -> 复制到本地临时buffer
  -> Event: bt_netdev_receive(driver, BT_EVT, data, total)
  -> ACL:   bt_netdev_receive(driver, BT_ACL_IN, data, total)
  -> 成功或Host明确拒绝后，为可信wrapper排队HCI_FREE_PKT
```

传给 NuttX 的 data从 Event/ACL header开始，不包含 H4 type。`bt_receive()` 自己会
再次复制并投递 HPWORK/LPWORK，所以本地临时buffer在其返回后可释放。

RX queue必须保持原 descriptor顺序。不能为了“Event优先”越过先到的 ACL；NuttX
在 `bt_receive()` 后已经会把 Command Complete/Status和 Number Of Completed
Packets投递 HPWORK。queue满时不能静默丢包：停止接收新业务、记录未释放可信
pointer并进入恢复；完全非法 pointer不回发 FREE，因为 CP会直接对收到的地址
执行 `os_free()`。

具体 backpressure 使用现有 descriptor deferral：RX callback在本地queue满时返回
`-EAGAIN`，不发送该 descriptor 的 transport ACK；worker释放queue空间后调用
`bk7258_mbox_kick_rx()`，使同一 descriptor按原FIFO顺序重试。若持续超过有界超时，
才进入恢复。不能在queue满后返回成功并丢弃pointer。

### 5.6 pointer、长度和Host上限

第一阶段 CP outbound wrapper白名单先限定当前 CP SRAM：

```text
0x28064000 <= pointer < 0x2809f700
```

原因是当前 `bt_ipc_core` 使用 `os_malloc`，最终 CP开启 `CONFIG_MEM_DEBUG`，现有
源码路径指向 CP SRAM allocator。只有 golden trace实际出现 PSRAM pointer且确认
allocator边界后，才能增加 PSRAM白名单。不能把整个 `0x60000000..0x60ffffff`
直接设为可接受窗口。

长度规则：

- Event total = `2 + param_len`。
- ACL total = `4 + little_endian(datalen)`。
- AP -> CP ACL上限来自 controller `LE Read Buffer Size`。
- CP -> AP ACL上限来自 AP发送的 `Host Buffer Size`。
- 两个方向还必须满足当前 NuttX `BLUETOOTH_MAX_FRAMELEN`。当前源码值为 79字节；
  driver在调用 `bt_netdev_receive()` 前必须显式检查，因为 `bt_receive()` copy前没有
  独立的传入长度上限保护。
- `param_len <= 255` 不是有效门禁，因为字段本身就是 `uint8_t`；真正门禁是总长、
  pointer窗口和Host buffer容量。

### 5.7 四种流控统计

分别统计，禁止合并：

- Mailbox descriptor ACK。
- Beken `HCI_FREE_PKT` wrapper回收。
- Controller `Number Of Completed Packets`，归还 AP -> CP ACL credit。
- Host `Host Number Of Completed Packets`，归还 CP -> AP ACL credit。

NuttX Host初始化还会发送 `Host Buffer Size` 和 `Set Controller To Host Flow Control`。
B2必须验证这些命令，不得只验证 `LE Read Buffer Size`。

## 6. NuttX lower-half和生命周期

### 6.1 Driver对象

```c
static struct bt_driver_s g_bk7258_bt_driver =
{
  .head_reserve = 0,
  .open         = bk7258_bt_open,
  .send         = bk7258_bt_send,
  .close        = bk7258_bt_close,
  .ioctl        = NULL,
  .priv         = &g_bk7258_bt_priv,
  /* receive和bt_net由bt_netdev_register()填写。 */
};
```

`head_reserve=0`，因为 Beken wrapper不使用H4 byte。第一版不启用
`CONFIG_DRIVERS_BLUETOOTH`，使 `bt_driver_register()` 直接映射到
`bt_netdev_register()`；当前 `CONFIG_DRIVERS_BLUETOOTH` wrapper的 unregister路径
不适合作为恢复基础。

### 6.2 唯一初始化顺序

采用一个明确模型，不在 register前和 `open()` 中重复 INIT：

```text
board late bring-up
  -> mailbox link READY
  -> PWC/heartbeat已启动
  -> bk7258_bt_transport_initialize()
       注册0x43 RX callback并启动TX/RX worker
  -> bt_driver_register(&g_bk7258_bt_driver)
       -> 注册层先填写driver.receive
       -> bt_initialize()
          -> driver.open()
             -> 发送vendor INIT
             -> transport拦截0xfe ready event
             -> 等待status==0
             -> 为CP vendor-event wrapper排队FREE
             -> 等待CP对AP vendor-command wrapper返回匹配FREE
             -> 返回OK
          -> NuttX立即执行标准HCI初始化
       -> bt_add_services()
          -> 注册GAP服务并自动开始advertising
       -> 注册bnep0
```

`open()` 使用有界 5秒等待。timeout返回 `-ETIMEDOUT`，不能假装成功。vendor event
只在 `open()` 的预Host状态拦截；ready后其他标准Event交给 NuttX。

`bt_netdev_register()` 当前会调用 `bt_add_services()` 并自动 advertising。因此阶段
B3的成功条件必须包括默认 advertising；不能写成“B3只初始化、B4才首次启动”。

### 6.3 close和恢复

`close()`：

1. 禁止新 Host send。
2. 等待已知 in-flight payload有界清空。
3. 发送 vendor DEINIT并等待匹配 event。
4. 停止 BT worker或置为 quiesced。
5. 有不确定 ownership 时保留 quarantine，不复用地址。

当前 `bt_netdev_unregister()` 可以执行 Host deinitialize、driver close和netdev注销，
但重注册仍需验证所有 NuttX全局状态可重复初始化。第一版恢复门禁是：

- 无连接、无in-flight payload时 DEINIT -> INIT 100次。
- 整机复位后重新初始化。
- CP明确整核复位后再回收旧 AP payload。

仅收到 Mailbox RESET/control generation不等于 CP controller已停止访问旧 pointer，
不得据此清空 quarantine。

## 7. Kconfig和构建接入

### 7.1 配置闭包

`WIRELESS_BLUETOOTH` 位于 `if WIRELESS` 内并依赖 BSD许可；Bluetooth netdev还需要
`NET_BLUETOOTH`。最终 defconfig至少加入：

```text
CONFIG_ALLOW_BSD_COMPONENTS=y
CONFIG_NET=y
CONFIG_NET_BLUETOOTH=y
CONFIG_WIRELESS=y
CONFIG_WIRELESS_BLUETOOTH=y
CONFIG_WIRELESS_BLUETOOTH_HOST=y
CONFIG_BLUETOOTH_MAX_CONN=1
CONFIG_BLUETOOTH_MAX_PAIRED=1
CONFIG_BLUETOOTH_MAXSCANRESULT=4
CONFIG_BLUETOOTH_MAXSCANDATA=64
CONFIG_BLUETOOTH_MAXDISCOVER=8
CONFIG_BLUETOOTH_BUFFER_PREALLOC=12
CONFIG_DEVICE_NAME="VelaSights"
CONFIG_DEVICE_LOCAL_NAME="VelaSights"
CONFIG_BTSAK=y
CONFIG_BK7258_BLUETOOTH=y
CONFIG_BK7258_BT_TX_BUFFERS=8
CONFIG_BK7258_BT_TX_QUEUE=16
CONFIG_BK7258_BT_RX_QUEUE=16
CONFIG_BK7258_BT_GATT_TEST=y
# CONFIG_BK7258_BT_RAW_SELFTEST is not set
```

上述 pool/queue值是首轮内存预算起点，必须以 `size`、`System.map`、运行时
`free`和压力测试调整，不能直接宣称量产值。

BK7258 Kconfig使用 `depends on` 保持依赖闭包，不用 `select` 绕过
`WIRELESS_BLUETOOTH` 的 BSD依赖：

```kconfig
config BK7258_BLUETOOTH
    bool "BK7258 CP-backed Bluetooth HCI"
    depends on ARCH_CHIP_BK7258
    depends on BK7258_MBOX_V2
    depends on ALLOW_BSD_COMPONENTS
    depends on NET && NET_BLUETOOTH
    depends on WIRELESS && WIRELESS_BLUETOOTH
    depends on WIRELESS_BLUETOOTH_HOST
```

### 7.2 Make/CMake

Chip `CMakeLists.txt`：

```cmake
if(CONFIG_BK7258_BLUETOOTH)
  list(APPEND SRCS bk7258_bt_transport.c bk7258_bt_driver.c)
endif()
```

`Make.defs` 使用等价的 `ifeq ($(CONFIG_BK7258_BLUETOOTH),y)`。board GATT fixture
同样在 `src/CMakeLists.txt` 和 `src/Makefile` 条件加入。修改 Kconfig/defconfig后先
执行 CMake distclean。

增加 `CONFIG_BK7258_BT_RAW_SELFTEST`，依赖 `BK7258_BLUETOOTH`，默认关闭。启用时
board bring-up只初始化BT transport并运行第B1/B2阶段的开机自检，打印逐opcode结果，
随后保持transport在诊断态，不调用 `bt_driver_register()`。这样raw测试和正式Host
不会同时消费同一Event，也不需要在apps或CP中增加临时命令。正式功能构建必须关闭
该符号。

增加 `CONFIG_BK7258_BT_GATT_TEST`，依赖 `BK7258_BLUETOOTH`，默认关闭；验收构建显式
启用，量产构建关闭。该符号同时控制board GATT fixture源码和notification worker，
避免测试service进入量产镜像。

## 8. 分阶段实施和验收

### B0：冻结输入和ABI

输出一份同日记录，包含：

- CP `app.bin`、`app.map`、`sdkconfig.h` SHA256。
- 实际 controller archive名称。
- AP/CP toolchain下 descriptor `sizeof/offsetof`。
- AP/CP channel ID和packet type常量。
- vendor INIT/DEINIT逐字节 trace。
- 由生成RAM布局得到的CP SRAM coarse envelope和AP pool候选地址。

这些未完成前不注册 NuttX Host。

### B1：raw BT logical transport

启用 `CONFIG_BK7258_BT_RAW_SELFTEST=y`，实现 `0x13/0x43`、raw frame、UART索引
修复、BT TX/RX queue和统计。由于正式 CP
没有任意 BT descriptor echo入口，不执行“无 HCI语义双向 loopback”：

- AP -> CP先完成一次vendor INIT，再对无副作用的 Read Local Version Information做
  10,000次顺序事务；每次都先回收response wrapper和command wrapper，再开始下一次。
- CP -> AP通过真实 vendor event和Command Complete验证。
- 记录CP Event wrapper实际pointer分布，并用Command Complete证明CP读取到AP SRAM
  pool中的正确opcode/params。
- 若要任意 descriptor echo，必须使用独立测试CP固件，不能写成“不修改CP”的正式门禁。

### B2：完整raw HCI初始化

继续使用raw selftest构建且不注册Host，按当前 `hci_initialize()` 顺序发送并检查
真实status：

1. vendor INIT。
2. HCI Reset。
3. Read Local Supported Features。
4. Read Local Version Information。
5. Read BD_ADDR。
6. LE Read Local Supported Features。
7. LE Read Buffer Size。
8. Set Event Mask。
9. LE Set Event Mask。
10. Host Buffer Size。
11. Set Controller To Host Flow Control。
12. 若controller报告BR/EDR能力，再测 Read Buffer Size和LE Write Host Supported。

每条命令记录 opcode、transport ACK、FREE、Command Complete/Status和status；不得用
ACK代替HCI成功。B2通过后关闭 `CONFIG_BK7258_BT_RAW_SELFTEST`、执行distclean，
再进入B3正式Host构建。将 `LE Read Buffer Size` 返回的 ACL最大长度和数量写入本轮
记录；它不是B0阶段可以静态假定的常量。

### B3：注册NuttX Host

- `open()` 完成唯一 vendor INIT握手。
- Host完整初始化无timeout。
- `ifconfig` 可看到 `bnep0`。
- 注册结束时默认 advertising已经成功。
- `System.map` 包含 `bk7258_bt_*`、`bt_netdev_register`、`bt_initialize`。

### B4：advertising和scan

实板NSH基线：

```sh
bt bnep0 info
bt bnep0 features le
bt bnep0 advertise stop
bt bnep0 advertise start
bt bnep0 scan start -d
bt bnep0 scan get
bt bnep0 scan stop
```

手机使用 nRF Connect或等价工具确认 `VelaSights` 可发现。advertising stop/start
重复100次；scan start/get/stop重复100次，前后 heap和pool计数回到基线。

### B5：连接和GATT fixture

内置 `bt_services.c` 只有GAP name/appearance read，不足以验收write和notification。
在 board目录增加最小测试service，具备：

- 一个 read/write characteristic。
- 一个 notify characteristic。
- CCC descriptor。
- CCC enable后由有界低频worker周期 notification；CCC disable或断连后立即停止。

该service只在 `bt_driver_register()` 成功返回后注册，固定handle从 `0x0006` 或更高
开始，不能与内置GAP service的 `0x0001..0x0005` 冲突。

手机侧完成连接、service discovery、read、write、订阅CCC、notification、disconnect
和reconnect。OpenVela作为client时使用：

```sh
bt bnep0 gatt connect <addr> public
bt bnep0 gatt characteristic <addr> public
bt bnep0 gatt read <addr> public <handle>
bt bnep0 gatt write <addr> public <handle> <byte> [byte...]
bt bnep0 gatt disconnect <addr> public
```

地址类型按scan结果改为 `random`。验收同时检查双向ACL flow control和四类完成统计。
CP -> AP ACL被Host释放后，必须观察到 NuttX发送
`Host Number Of Completed Packets`；该项需要已建立连接，不属于B2初始化自检。

### B6：稳定性、恢复和共存

- advertising/单连接运行72小时。
- connect/disconnect 1,000次。
- notification持续压力，queue/pool无净增长。
- Wi-Fi DHCP/TCP压力下 BLE不异常断开。
- BLE压力下 heartbeat、PWC、Mailbox UART无timeout。
- malformed/unknown/duplicate FREE全部拒绝且不破坏allocator。
- 无in-flight时 DEINIT/INIT 100次。
- 整机复位100次均重新出现 `bnep0` 和 advertising。

### B7：SMP兼容复核

不在本轮启用CPU2，但提交前检查：

- 无依赖 `up_cpu_index()==0` 才正确的未保护全局状态。
- ISR只做descriptor copy，worker/Host调用不在ISR。
- queue和ownership table同步在未来SMP下成立。
- 若未来启用 Host线程 affinity，配置中的 core编号按 NuttX逻辑CPU而非物理CPU解释。

## 9. 构建、打包和静态门禁

### 9.1 冻结CP输入

在每轮B0记录中执行：

```bash
sha256sum \
  /home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/app.bin \
  /home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/app.map \
  /home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.h \
  /home/mi/vela_competition/bk_avdk_smp/cp/components/bk_bluetooth/ipc/src/bt_ipc_core.c \
  /home/mi/vela_competition/bk_avdk_smp/cp/middleware/driver/mailbox/mailbox_channel.c

grep -m1 'libbluetooth_controller.*\.a' \
  /home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/app.map

grep -E 'CONFIG_(BLUETOOTH|BLE|BTDM_5_2|BTDM_CONTROLLER_ONLY|BLUETOOTH_SUPPORT_IPC|MEM_DEBUG|PSRAM_AS_SYS_MEMORY)' \
  /home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.h
```

如果 `app.bin/app.map/sdkconfig.h` 不存在，先按 `docs/固件构建步骤.md` clean构建CP；不能
拿历史产物的hash代替本轮输入。

### 9.2 构建OpenVela AP

```bash
cd /home/mi/vela_competition/contest

./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  --cmake distclean

./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  -e -Werror \
  --cmake \
  -j8
```

检查配置闭包：

```bash
grep -E 'CONFIG_(ALLOW_BSD_COMPONENTS|NET_BLUETOOTH|WIRELESS=|WIRELESS_BLUETOOTH|BTSAK|BK7258_BLUETOOTH)' \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_nsh/.config

grep -E 'bk7258_bt_|bt_netdev_register|bt_initialize' \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_nsh/System.map
```

检查raw ABI的 `_Static_assert` 已编译，并用 `size`/map核对新增BSS、thread stack和
pool没有使 AP SRAM越界。

### 9.3 最终打包

严格使用 `docs/固件构建步骤.md` 的 `EXTERNAL_AP_BIN` 流程：

```bash
cp \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_nsh/nuttx.bin \
  /home/mi/vela_competition/bk_avdk_smp/build/openvela-ap.bin

cd /home/mi/vela_competition/bk_avdk_smp

podman run --rm \
  --userns=keep-id \
  -v "$PWD:/armino" \
  -w /armino \
  localhost/bekencorp/armino-idk:1.5 \
  make -C projects/app_ab bk7258 \
  SDK_DIR=/armino \
  EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin
```

三个AP文件必须逐字节一致：

```bash
sha256sum \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_nsh/nuttx.bin \
  /home/mi/vela_competition/bk_avdk_smp/build/openvela-ap.bin \
  /home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin

cmp -s \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_nsh/nuttx.bin \
  /home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
```

最终烧录文件：

```text
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin
```

## 10. 诊断和安全门禁

每类统计至少包含：

```text
tx_enqueue, tx_ack, tx_timeout, tx_free, tx_free_unknown,
rx_event, rx_acl, rx_sco_rejected, rx_bad_pointer, rx_bad_length,
rx_queue_full, host_receive_error, vendor_timeout, recovery_count,
controller_completed, host_completed
```

故障定位顺序：

1. 确认最终 CP配置、controller archive和CP `bt_ipc_core` hash。
2. 确认 Mailbox link READY，BT TX `0x13`、RX `0x43` 已注册。
3. 确认 vendor INIT event `fe 03 01 00 00` 的语义和对应 FREE。
4. 区分 descriptor ACK、FREE和HCI Complete。
5. 检查传给NuttX的数据不含H4 byte，Event从event code开始、ACL从handle开始。
6. 检查 `BLUETOOTH_MAX_FRAMELEN`、CP pointer窗口和little-endian length。
7. 检查 Command、ACL、FREE队列保留容量和ownership状态。
8. 检查 Host Number Of Completed Packets与controller Number Of Completed Packets。

安全规则：

- unknown FREE绝不直接free。
- 完全非法 CP pointer绝不原样FREE给CP。
- 可信CP wrapper即使SCO策略拒绝，也要安全排队FREE并记录拒绝。
- 任何 pointer加法先检查overflow。
- packed multi-byte字段只通过 `memcpy`/endian helper读取。
- 当前 `dmb sy` 只保证顺序；如果将来任一侧启用cache或把pool移到PSRAM，必须补
  clean/invalidate和明确的内存属性验证。

## 11. 延后项和协议演进

### 11.1 bond持久化

当前 NuttX `bt_keys.c` 只使用静态RAM key pool；contest也没有可用的CP Flash proxy
bond backend。因此本轮可测试运行期 pairing/encryption，但不能把“重启后bond恢复”
列为完成条件。后续必须先实现：

- CP Flash proxy channel和访问控制。
- key序列化版本、完整性和机密性。
- 掉电安全、磨损控制和加载/保存接口。

### 11.2 独立复位恢复

若要求 AP reset而CP继续运行，必须允许修改CP协议，至少增加：

- AP/CP BT generation或epoch。
- CP在AP generation变化时停止访问旧 AP pointer并重置BT IPC/controller。
- 显式 reset-ready handshake。
- 双方确认后统一回收旧pool。

没有上述闭环时，AP reset会丢失 ownership table，CP仍可能访问同一 SRAM地址；
任何“本地清表后继续”的实现都有 use-after-free/串包风险。

### 11.3 OpenVela Bluetooth Framework

产品若需要 Framework/SAL、QuickApp或NDK，另立方案同步对应仓库、服务进程、IPC、
权限和Framework级测试；不得把本计划的 NuttX native Host验收冒充Framework完成。

## 12. 完成定义

满足以下条件才算完成本轮 BK7258 OpenVela Bluetooth移植：

- 当前CP继续运行唯一 BLE controller，OpenVela运行唯一Host。
- 本轮未改变CP `bt_ipc_core` wire ABI；当前项目级CP改动和输入hash已记录。
- `0x13/0x43`、BT raw offset 4/5和`HCI_FREE_PKT`与原厂兼容。
- generic Mailbox现有ACK/reset能力被复用，BT专用有界队列和ownership已实现。
- 所有远端pointer、length、type和FREE经过白名单及状态校验。
- vendor INIT只执行一条明确路径，完整NuttX HCI初始化成功。
- `bnep0`、默认advertising、scan、单连接和GATT read/write/notify通过实板验收。
- 四类完成/流控统计独立，Command和ACL无credit死锁。
- 72小时和共存压力无泄漏、double free、越界、永久busy和heartbeat/PWC回归。
- 无in-flight DEINIT/INIT和整机复位恢复通过门禁。
- AP raw、外部AP输入和最终`app1.bin`哈希一致。

AP单独复位无损恢复、bond掉电保存、OpenVela Bluetooth Framework、经典BT、SCO、
ISO、多连接和CPU2 SMP运行验收不属于本轮完成定义，必须按第11节另行闭环。
