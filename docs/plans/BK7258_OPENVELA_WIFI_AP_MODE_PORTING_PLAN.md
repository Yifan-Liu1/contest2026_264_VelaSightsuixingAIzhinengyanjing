# BK7258 OpenVela Wi-Fi SoftAP 模式移植方案

> 文档状态：2026-08-18 AP0 至 AP3 代码移植和构建门禁已完成；`nsh -Werror`、
> `ai_agent`、CP clean build 与最终打包通过。实板已确认 WPA2 SoftAP 进入
> `RUNNING`、手机可发现并成功关联、NuttX DHCP server 可启动。AP4 的 DHCP lease
> 结果、ARP/Socket 双向数据面、反复热切换和稳定性门禁仍待继续验收。
> 在这些实板门禁完成前，不声明 SoftAP 首版移植全部完成。

### 0.1 本轮实施记录

已完成：

- pointer-free `0x210..0x212` AP command、固定结构和双侧静态 ABI 断言。
- 单 `wlan0` STA/SoftAP 互斥角色状态机，默认初始化为 STA。
- STA/SoftAP 独立 SSID、密码、安全和 channel pending config。
- AP start/stop/client event、真实 SoftAP MAC、carrier 和 status fallback。
- TX/RX wire VIF 0/1 标记、RX role epoch 隔离、切换前 TX ownership 门禁。
- CP 保留 EAPOL/hostapd，SoftAP ARP/IPv4 direct-push 给 OpenVela。
- `CONFIG_WIFI_VNET_OPENVELA_SOFTAP_IPV4` 下 CP `g_uap` L3/DHCP 入口级禁用。
- NuttX broadcast、DHCP server、地址池和最多 4 个 lease 配置。
- WAPI MASTER 模式 `disconnect` 下发 ESSID-OFF，支持命令行停止 SoftAP。

构建门禁：

- `nsh` 全量及增量 `-Werror` 构建通过。
- `ai_agent` 正常构建通过；补齐其既有 audio test 的 `CONFIG_LIB_OPUS` 依赖。
- 实际 CP 构建树与比赛仓镜像一致，CP clean build 和最终 package 通过。
- 生成 CP 配置包含 STA 与 SoftAP 两个 OpenVela IPv4 ownership 开关。
- 最终 `all-app.bin` 已生成，外部 AP 输入与 package `app1.bin` 字节一致。

实板基础验证（2026-08-18）：

- `VelaSight_AP`、channel 6、WPA2-CCMP 在实板进入 `RUNNING`。
- 手机可发现热点并使用正确密码成功关联。
- `dhcpd_start wlan0` 可启动，板端地址为 `192.168.10.1/24`。

待实板射频验收：

- 默认开机 STA 实际关联、DHCP、DNS、TLS/HTTPS 回归。
- Open SoftAP 和 WPA2 错误密码拒绝行为。
- DHCP lease 地址确认、ARP、ping 和 TCP/UDP 双向通信。
- STA -> AP -> STA 100 次、客户端计数、复位和 24 小时稳定性。

## 1. 目标与结论

### 1.1 目标

在当前已通过实板 STA 验收的 BK7258 CP-backed Wi-Fi 基线上增加 SoftAP 模式，
使手机或电脑能够发现热点、完成 Open/WPA2-PSK 认证、从 OpenVela DHCP server
获取 IPv4 地址，并与 BK7258 上的 NuttX socket 服务双向通信。

本文中的“AP”若无特别说明均指 Wi-Fi SoftAP，不是运行 OpenVela 的 CPU1/AP
处理器。

### 1.2 固定实施路线

首版采用一张 NuttX 网卡 `wlan0`，在 STA 与 SoftAP 之间互斥切换：

```text
wlan0 / IW_MODE_INFRA   -> wire VIF 0 -> Beken infra STA
wlan0 / IW_MODE_MASTER  -> wire VIF 1 -> Beken SoftAP
```

首版不直接实现 `wlan0 + wlan1` 并发，原因如下：

- 当前 `bk7258_wifi.c` 只有一个 lower-half、一个 carrier、一个 RX queue 和一组
  TX slot。
- 当前 TX 的 `co_hdr.vif_idx` 被清零，实际固定发送到 wire VIF 0，即 STA。
- 当前 RX 未读取 `co_hdr.vif_idx`，无法把 STA 和 SoftAP 帧分派给不同 netdev。
- 当前 CP 的 `CONFIG_WIFI_VNET_AP_IPV4` 只把普通 STA 的 ARP/IPv4 交给
  OpenVela，SoftAP 流量和 DHCP server 仍由 CP lwIP 持有。
- 直接注册第二张网卡只能得到一个不可用的接口，并会造成 CP lwIP 与 NuttX
  同时响应 DHCP/ARP。

首版完成后再评估双网卡共存。双网卡不是本方案首版完成定义的一部分。

### 1.2.1 当前项目选定方案

结合当前产品需求，正式选择“单 `wlan0` STA/SoftAP 互斥运行时切换”作为首选
实施方案：

- 平时 `wlan0` 运行 STA，维持设备的常规联网功能。
- 用户进入配置或文件服务模式后，允许关闭 STA，切换同一 `wlan0` 为 SoftAP。
- SoftAP 由 OpenVela/NuttX 提供 IPv4、ARP、DHCP 和本地 HTTP/File Server 数据面。
- 配置完成后停止 SoftAP，不重启单片机，恢复 STA 并重新获取地址。
- 不要求 STA 和 SoftAP 同时工作，不承担双网卡、NAT、bridge 和同信道并发风险。

该方案满足“常用 STA、配置时可关闭 STA 换用 AP、切换不可重启”的要求。这里的
“热切换”表示设备运行期间完成角色切换，不表示网络连接和 socket 无损保持。

### 1.3 首版能力边界

| 项目 | 首版范围 |
| --- | --- |
| 射频 | 2.4 GHz，channel 1 至 13；channel 14 仅在 JP 国家码下允许 |
| 模式 | STA/SoftAP 互斥切换 |
| 安全 | Open、WPA2-PSK/CCMP |
| 地址族 | IPv4 |
| DHCP | OpenVela/NuttX 为唯一 DHCP server |
| 本地通信 | 客户端与 BK7258 本地 TCP/UDP/HTTP 服务通信 |
| 最大客户端 | 首版默认 4，受 Beken driver 上限约束 |
| 隐藏 SSID | wire ABI 预留，首版不作为验收必选项 |
| NAT/路由 | 不支持，不承诺客户端经 STA 上网 |
| WPA3、IPv6 | 后续阶段 |
| STA+AP 并发 | 后续双网卡阶段 |

## 2. 当前代码事实

### 2.1 已有 STA 基线

当前正式实现位于：

```text
board/beken/chips/bk7258/bk7258_wifi.c
board/beken/chips/bk7258/hardware/bk7258_wifi_ipc.h
board/beken/boards/bk7258/bk7258-ap/src/bk7258_bringup.c
```

已经具备：

- Mailbox CMD/DATA 通道 `0x14/0x15` 和 `0x44/0x45`。
- command/confirmation sequence、超时和 peer-reset generation。
- STA 扫描、连接、断开、国家码和 `wlan0` lower-half。
- NuttX Ethernet TX/RX 与 Beken pbuf/CPDU ownership 适配。
- STA ARP/IPv4 由 CP 转发给 NuttX。
- 实板关联、DHCP、DNS、TCP/TLS/HTTPS 基线。

依据：

- `board/beken/chips/bk7258/bk7258_wifi.c:104-144`
- `board/beken/chips/bk7258/bk7258_wifi.c:723-819`
- `board/beken/chips/bk7258/bk7258_wifi.c:1143-1316`
- `board/beken/chips/bk7258/bk7258_wifi.c:1462-1569`
- `docs/8.16基础适配门禁验收记录.md:69-88`

### 2.2 Beken 底层具备 SoftAP 能力

当前 CP controller 已保留：

```text
BK_CMD_START_AP       = 0x4
BK_CMD_STOP_AP        = 0x6
BK_EVT_START_AP_IND   = 0x4
BK_EVT_ASSOC_AP_IND   = 0x5
BK_EVT_DISASSOC_AP_IND= 0x6
BK_EVT_STOP_AP_IND    = 0x7
```

CP 的数据面 wire role 已定义：

```text
wire 0 = infra STA
wire 1 = SoftAP
wire 2 = P2P GO
wire 3 = P2P GC
```

`bk_idk` 和 AVDK 还提供 `bk_wifi_ap_set_config()`、`bk_wifi_ap_start()`、
`bk_wifi_ap_stop()`、`bk_wifi_ap_get_config()` 和 `bk_wifi_ap_get_mac()`。

依据：

- `external/bk_avdk_smp/cp/components/controller_if/cif_main.h:93-169`
- `external/bk_avdk_smp/cp/components/controller_if/cif_main.h:262-293`
- `external/bk_avdk_smp/cp/components/controller_if/cif_wifi_dp.c:46-102`
- `external/bk_avdk_smp/cp/components/controller_if/cif_cntrl.c:362-460`
- `bk_idk/include/modules/wifi.h:338-407`

结论是“底层能力存在”，不是“OpenVela SoftAP 已可用”。

### 2.3 当前阻断项

| 阻断项 | 当前行为 | 后果 |
| --- | --- | --- |
| mode handler | `wifi_mode()` 只保存数值 | `wapi mode wlan0 3` 不会启动 SoftAP |
| connect/disconnect | 固定发送 STA command | AP 模式仍执行 STA 连接/断开 |
| TX role | CPDU flags 清零 | 所有 TX 都以 wire VIF 0 发送 |
| RX role | 不读取 CPDU `vif_idx` | 无法识别 SoftAP RX |
| AP event | 只处理 STA/IP/scan event | AP start 后 carrier 不会打开 |
| MAC | 只获取 base MAC | 未获取 Beken SoftAP MAC/BSSID |
| channel | `freq` handler 为 `NULL` | 无法从 WAPI 设置 AP channel |
| auth | 只保存 `iw_param` | 未映射到 Beken AP security |
| ifdown | 只关闭 carrier | CP 上的 STA/AP 可能继续运行 |
| CP 数据所有权 | SoftAP ARP/IPv4 仍归 CP | NuttX DHCP server 收不到客户端包 |
| DHCP | defconfig 未启用 DHCP server/broadcast | 无法给客户端分配地址 |

关键依据：

- `board/beken/chips/bk7258/bk7258_wifi.c:1449-1459`
- `board/beken/chips/bk7258/bk7258_wifi.c:1497-1507`
- `board/beken/chips/bk7258/bk7258_wifi.c:1582-1600`
- `board/beken/chips/bk7258/bk7258_wifi.c:1723-1751`
- `board/beken/chips/bk7258/bk7258_wifi.c:1860-1876`
- `external/bk_avdk_smp/cp/components/controller_if/cif_wifi_dp.c:438-448`

## 3. 目标架构与所有权

### 3.1 控制面

```text
NSH/WAPI
  -> NuttX wireless_ops
  -> BK7258 role state machine
  -> pointer-free OpenVela AP command
  -> Mailbox WIFI_CMD 0x14
  -> CP controller_if
  -> bk_wifi_ap_set_config/start/stop
  -> RWNX/hostapd/MAC/PHY/RF
  -> AP start/stop/client event
  -> Mailbox WIFI_CMD 0x44
  -> carrier/client state
```

CP 继续独占射频、RWNX、hostapd、认证、关联和真实 VIF。OpenVela 不链接
Armino Wi-Fi 库，也不直接访问 Wi-Fi 寄存器。

### 3.2 数据面

```text
SoftAP client Ethernet frame
  -> CP Wi-Fi RX
  -> 保留 EAPOL 给 CP hostapd
  -> 普通 ARP/IPv4 标记 wire VIF 1
  -> Mailbox WIFI_DATA 0x45
  -> OpenVela RX worker
  -> NuttX wlan0
  -> DHCP server / local socket

NuttX wlan0 TX
  -> TX CPDU vif_idx=1
  -> Mailbox WIFI_DATA 0x15
  -> CP wire-to-LMAC VIF 映射
  -> SoftAP client
```

### 3.3 唯一所有者规则

SoftAP 模式下必须固定：

- CP 拥有 EAPOL、WPA2 handshake、hostapd、射频和 station association。
- NuttX 拥有 SoftAP 的 ARP、IPv4、UDP/TCP、DHCP server 和本地 socket。
- CP 不为 SoftAP 地址配置 IPv4，不启动 CP DHCP server，不响应 SoftAP ARP。
- 同一 SoftAP 上不得同时运行 CP DHCP server 和 NuttX DHCP server。
- CP 保留 `g_uap`/VIF 元数据只用于 Wi-Fi 数据路径映射，不能因此取得 L3
  所有权。

只启动 Beken 热点、继续让 CP 托管 DHCP/IP 的方案不作为交付方案。它无法形成
可由 OpenVela 应用使用的 `wlan0` 数据面。

## 4. 模式与状态机

### 4.1 状态

```text
DOWN
  -> IDLE
      -> STA_STARTING -> STA_UP -> STA_STOPPING -> IDLE
      -> AP_STARTING  -> AP_UP  -> AP_STOPPING  -> IDLE
      -> ERROR -> IDLE
```

状态至少拆分：

- `admin_up`：NuttX interface 是否 up。
- `configured_role`：WAPI 最近配置的 `INFRA` 或 `MASTER`。
- `active_role`：CP 当前实际运行的角色。
- `sta_associated`：STA 关联状态。
- `ap_started`：SoftAP 是否已开始发 beacon。
- `ap_client_count`：已关联客户端数量。
- `carrier`：投射到 NuttX 的链路状态。

### 4.2 状态规则

- `wapi mode` 只允许 `IW_MODE_INFRA` 和 `IW_MODE_MASTER`。
- active role 运行时修改 mode 返回 `-EBUSY`；用户必须先执行 disconnect。
- STA carrier 在 STA connected event 后打开，在 disconnected event 后关闭。
- SoftAP carrier 在 AP start success event 后打开，在 AP stop/fail 后关闭。
- AP client join/leave 只更新 client count，不得切换 carrier。
- AP active 时扫描首版返回 `-EBUSY`，避免扫描影响 beacon/channel。
- `ifdown` 必须停止 active role、关闭 carrier、阻止新 TX，并有界等待旧 TX
  ownership 完成；不能只改本地标志。
- peer reset 后 active role 变为 unknown，carrier off；重新查询 CP status 后才可
  恢复，不能猜测 SoftAP 仍在运行。

### 4.3 配置隔离

STA 与 SoftAP 使用独立 pending config，至少包括：

```text
STA: ssid, password, auth
AP:  ssid, password, security, channel, hidden, max_clients
```

切换 mode 不能让上一次 STA 密码静默成为 AP 密码，也不能让旧 AP 密码污染下一次
开放热点配置。

### 4.4 产品运行时序

#### 常规 STA 模式

```text
STA_ONLINE
  -> wlan0 连接外部路由器
  -> DHCP client 获取地址
  -> 维持云端或其他外部网络连接
```

#### 进入配置/文件服务模式

```text
STA_ONLINE
  -> ENTER_CONFIG
  -> 关闭云端新请求并停止现有网络连接
  -> 停止 STA DHCP client
  -> STA disconnect/stop
  -> 等待 STA disconnected event
  -> 清理 STA 地址、路由、TX/RX 状态
  -> wlan0 切换到 SoftAP
  -> AP start success
  -> 设置 AP 静态 IPv4
  -> 启动 NuttX DHCP server
  -> 启动 HTTP/File Server
  -> AP_SERVING
```

#### 退出配置/文件服务模式

```text
AP_SERVING
  -> 停止接受新的 HTTP/File 请求
  -> 等待或取消在途文件操作
  -> 停止 DHCP server
  -> SoftAP stop
  -> 等待 AP stop event
  -> 清理 AP 地址、客户端计数、TX/RX 状态
  -> wlan0 切换到 STA
  -> 恢复 STA 配置和 STA MAC
  -> STA start/connect
  -> connected event
  -> DHCP renew
  -> 恢复云端或其他外部网络连接
  -> STA_ONLINE
```

切换期间不重启单片机，但以下连接状态不保留：

- STA 的 TCP/TLS/MQTT 等外部连接需要关闭并重建。
- AP 客户端在退出配置模式时会断开，需要重新关联。
- STA 与 AP 的 IPv4 地址、默认路由和 DHCP 状态必须分别清理和重新建立。
- HTTP/File Server 进程可以继续运行，但只能在 AP 地址有效且 `AP_SERVING`
  状态下对外提供服务。

### 4.5 运行时切换兼容性要求

本方案不依赖系统重启，但必须把切换设计成有界状态机：

- 切换前拒绝新的网络业务请求，避免应用继续创建外部 socket。
- 停止 active role 后等待对应 CP stop/disconnect event，不以发送 command 成功
  作为角色已停止的依据。
- 切换前关闭 DHCP client/server，清除旧 IP、网关、DNS 和默认路由。
- 阻止新 TX，等待 in-flight TX completion；超时对象进入 quarantine，不能直接
  复用共享 buffer。
- 清理旧 RX queue，给 RX packet 记录 role，禁止旧 STA frame 被 AP 模式消费，或
  反向消费。
- 切换 SoftAP/STA 时使用对应角色 MAC，不使用 base MAC 猜测替代。
- AP carrier 只有在 AP start success event 后打开，STA carrier 只有在 STA
  connected event 后打开。
- AP 模式下 HTTP/File Server 只绑定 AP 静态地址；STA 恢复后外部网络服务再绑定
  新的 STA 地址或由应用重新建立连接。
- 任意 command timeout、CP reset 或 event 丢失都进入 `ERROR`，关闭 carrier 和
  DHCP，禁止直接跳到下一角色。

目标体验是“设备不重启、网络角色可恢复”，不是“网络业务零中断”。实际切换时间
以实板为准，方案目标为 STA 到 AP 可访问约 3 至 8 秒，AP 到 STA 恢复联网约 3
至 15 秒；STA 重新关联和 DHCP 可能使时间进一步增加。

## 5. Pointer-free SoftAP Wire ABI

### 5.1 不直接复用 pointer API

CP 的 `cif_wifi_api` 中部分 `AP_SET_CONFIG` API 把参数解释为 CP 可解引用的
`wifi_ap_config_t *`。OpenVela 不得跨核传递 NuttX pointer，也不得复制由编译宏、
bitfield 和 packing 决定的完整私有结构作为长期 ABI。

旧 `BK_CMD_START_AP` 虽然是固定 payload，但存在以下问题：

- security 依赖密码长度隐式推导。
- 5 GHz 拒绝路径不返回正常 confirmation。
- confirmation 在实际 AP start 前发送，无法表示 start 最终结果。
- 无 protocol version、配置长度和 SoftAP MAC/status 查询。

为避免改变原 AVDK AP 的 legacy command 语义，建议增加 OpenVela 专用 command：

```text
0x210 BK_CMD_OPENVELA_AP_START
0x211 BK_CMD_OPENVELA_AP_STOP
0x212 BK_CMD_OPENVELA_AP_STATUS
```

### 5.2 建议结构

```c
struct bk7258_wifi_ap_start_request
{
  uint8_t version;
  uint8_t channel;
  uint8_t security;
  uint8_t hidden;
  uint8_t max_clients;
  uint8_t ssid_length;
  uint8_t password_length;
  uint8_t reserved;
  uint8_t ssid[32];
  uint8_t password[64];
};

struct bk7258_wifi_ap_status_response
{
  int32_t status;
  uint8_t started;
  uint8_t channel;
  uint8_t security;
  uint8_t client_count;
  uint8_t mac[6];
  uint8_t reserved[2];
};
```

实现时必须为 `sizeof`、字段 offset 和 reserved=0 增加两侧 `_Static_assert`。
所有字符串按显式长度解析，CP 落地到 Beken API 前再复制并补 NUL，禁止对 wire
数组直接调用无界 `strlen()`。

### 5.3 输入校验

AP 和 CP 两侧都必须检查：

- `version` 是双方支持的版本。
- SSID 长度为 1 至 32 字节。
- Open 模式 password 长度必须为 0。
- WPA2-PSK password 长度为 8 至 63 字节。
- channel 符合当前 country code。
- `hidden` 只能为 0 或 1。
- `max_clients` 在 1 至 Beken driver 上限内。
- `reserved` 全零。
- payload 长度必须精确匹配结构大小。

普通日志不得打印 SSID 对应的密码或完整 command payload。

## 6. OpenVela AP 侧修改方案

### 6.1 `bk7258_wifi_ipc.h`

新增：

- OpenVela AP command/event 命名常量。
- wire VIF 常量：STA=0、SoftAP=1。
- 固定宽度 AP request/response。
- CPDU `flags` 中 `vif_idx` 的 mask/shift helper，避免 C bitfield ABI。
- AP start/stop/assoc/disassoc event payload。
- 全部结构的 size/offset assertion。

禁止引入 `wifi_ap_config_t`、lwIP 类型或 FreeRTOS 类型。

### 6.2 `bk7258_wifi.c` 控制面

调整：

- 用 role state mutex 保护 mode/config/start/stop/event。
- `wifi_connect()` 按 configured role 分派 STA connect 或 SoftAP start。
- `wifi_disconnect()` 按 active role 分派 STA disconnect 或 SoftAP stop。
- `wifi_mode()` 校验角色和状态，不再无条件保存任意值。
- `wifi_passwd()` 根据 WAPI algorithm 写入角色独立配置。
- 实现 `wifi_freq()`，将 channel 作为 AP pending config；active AP 不在线修改。
- `wifi_auth()` 只接受首版能够实际执行的 Open/WPA2 参数，未支持值返回
  `-EOPNOTSUPP`。
- AP config validation 在发送 command 前完成一次，CP 再完成第二次。
- 解析 AP start/stop/client events，维护 `ap_started/client_count/carrier`。
- 初始化和 peer-reset 恢复时通过 AP status command 取得 SoftAP MAC 与真实状态。

### 6.3 `bk7258_wifi.c` 数据面

调整：

- TX slot 保存发送时的 wire role。
- STA TX 设置 `vif_idx=0`，SoftAP TX 设置 `vif_idx=1`。
- RX 校验并提取 CPDU `vif_idx`。
- 首版只接收 active role 对应的 frame；其他 role frame 计数并按原 ownership
  协议归还 CP。
- RX packet 记录 wire role，防止切换过程中旧 frame 被新角色消费。
- role switch 前阻止新 TX，并有界等待旧 TX completion；超时沿用 quarantine，
  不猜测性复用共享 buffer。
- TX completion 仍按 slot address 回收，不把 Mailbox transport ACK 当成 Wi-Fi
  packet completion。

### 6.4 MAC 与 carrier

- 初始化时分别获取 STA MAC 和 SoftAP MAC。
- role 只在 interface 无 carrier、无 in-flight TX 时切换。
- 切到 SoftAP 前把 `d_mac` 更新为 Beken `MAC_TYPE_AP` 对应地址。
- AP start success 前 carrier 保持 off。
- AP stop 后 carrier off，再清 active role。

若 NuttX upper-half 不允许已注册 netdev 安全更新 MAC，则实施时改为启动时固定选择
SoftAP-only 配置；不得静默用 STA/base MAC 冒充 SoftAP MAC。

## 7. CP 侧修改方案

### 7.1 控制命令

在当前 CP `controller_if` 增加 `0x210..0x212` handler：

- 校验固定 payload 和版本。
- 映射到本地 `wifi_ap_config_t`。
- 依次调用 `bk_wifi_ap_set_config()` 和 `bk_wifi_ap_start()`。
- 返回真实 Beken error，不在执行前提前报告成功。
- stop 返回 `bk_wifi_ap_stop()` 的真实结果。
- status 使用 `bk_wifi_ap_get_config()`、`bk_wifi_ap_get_mac()` 和本地 client
  计数生成值语义 response。

legacy `BK_CMD_START_AP/STOP_AP` 保持兼容，不改变原 AVDK AP 行为。

### 7.2 SoftAP IPv4 所有权开关

新增一个含义明确的 CP 配置，例如：

```text
CONFIG_WIFI_VNET_OPENVELA_SOFTAP_IPV4=y
```

该开关只表示“普通 SoftAP 的 IPv4 数据面由 OpenVela 持有”，不能复用含义仅覆盖
STA 的 `CONFIG_WIFI_VNET_AP_IPV4`。

开启后：

- `cif_rx_local_packet_check()` 对普通 SoftAP VIF 的 ARP/IPv4 走 direct-push，
  并设置 wire VIF 1。
- EAPOL 继续留给 CP hostapd。
- CP 不对 SoftAP 启动 `uap_ip_start()`、DHCP server、DNS server 或 ARP 响应。
- AP stop 只清理 CP 的 Wi-Fi VIF/hostapd 状态，不操作 NuttX DHCP daemon。
- STA 的现有 `CONFIG_WIFI_VNET_AP_IPV4` 行为保持不变，防止 STA 回归。

可能涉及的实际构建源：

```text
external/bk_avdk_smp/cp/components/controller_if/cif_main.h
external/bk_avdk_smp/cp/components/controller_if/cif_cntrl.c
external/bk_avdk_smp/cp/components/controller_if/cif_wifi_dp.c
bk_avdk_smp/cp/components/bk_wifi/src/wifi_v2.c
bk_avdk_smp/cp/components/lwip_intf_v2_1/lwip-2.1.2/port/net.c
bk_avdk_smp/projects/app_ab/cp/config/bk7258/config
```

开工时先用最终 CP compile database/map 确认实际参与构建的源文件。比赛仓中的
`external/bk_avdk_smp` 镜像与最终打包使用的 `bk_avdk_smp/cp` 不一致时，先固定
唯一源码所有权和同步方式，禁止只改参考副本。

### 7.3 CP 数据面门禁

CP 侧必须新增统计或临时 trace 证明：

- SoftAP EAPOL 留在 CP。
- SoftAP ARP、DHCP、普通 IPv4 发送到 wire VIF 1。
- CP DHCP server 未启动。
- NuttX TX wire VIF 1 能映射到当前真实 SoftAP LMAC VIF。
- STA wire VIF 0 仍保持原行为。

## 8. NuttX 配置与使用流程

### 8.1 defconfig 增量

`nsh` 和需要 SoftAP 的 `ai_agent` 配置至少增加：

```text
CONFIG_NET_BROADCAST=y
CONFIG_NETUTILS_DHCPD=y
CONFIG_EXAMPLES_DHCPD=y
CONFIG_NETUTILS_DHCPD_STARTIP=0xc0a80a02
CONFIG_NETUTILS_DHCPD_ROUTERIP=0xc0a80a01
CONFIG_NETUTILS_DHCPD_NETMASK=0xffffff00
CONFIG_NETUTILS_DHCPD_DNSIP=0x00000000
CONFIG_NETUTILS_DHCPD_MAXLEASES=4
```

首版使用 `192.168.10.1/24`，地址池从 `192.168.10.2` 开始。DNS 设置为 0，
因为首版没有 NAT、DNS proxy 或上游转发能力，不能向客户端虚假通告公网 DNS。

### 8.2 预期 NSH 操作

WPA2-PSK：

```sh
wapi disconnect wlan0
ifup wlan0
wapi mode wlan0 3
wapi freq wlan0 6 1
wapi psk wlan0 <8至63字节密码> 3
wapi essid wlan0 <SSID> 1
ifconfig wlan0
dhcpd_start wlan0
```

开放热点只用于受控测试，必须显式清除 AP password/security 后再启动，不能仅
跳过 `wapi psk` 而沿用旧密码。若现有 WAPI 无明确的 clear-PSK 操作，则增加板级
测试命令或在切入 `IW_MODE_MASTER` 时初始化新的 AP pending config。

停止：

```sh
dhcpd_stop
wapi disconnect wlan0
ifdown wlan0
```

`ifconfig wlan0` 在 AP start event 成功后应显示 `RUNNING` 和 SoftAP MAC；
`dhcpd` 设置 `192.168.10.1/24` 后客户端才能取得 lease。

## 9. 分阶段实施

### 阶段 AP0：冻结 golden 和 ABI

输出：

- 用原 AVDK AP 启动 SoftAP，记录 start/stop/client event trace。
- 记录 SoftAP MAC、channel、WPA2 和 AP+STA 单射频行为。
- 冻结新增 command ID、request/response size/offset 和 error mapping。
- 用最终 CP `.config`、compile database 和 map 确定真实构建源。

门禁：文档、AP 和 CP 对 wire ABI 的静态断言完全一致。

### 阶段 AP1：控制面和角色状态机

实现：

- pointer-free AP command。
- role-aware mode/connect/disconnect/freq/auth。
- AP start/stop/client event。
- SoftAP MAC、carrier 和 status 查询。
- STA/AP 互斥切换。

门禁：

- 手机可发现 SSID。
- Open 和 WPA2-PSK 均可按预期关联或拒绝。
- 错密码不关联。
- start/stop 连续 100 次无状态分裂。
- 此阶段即使尚无 DHCP，也必须明确标为“控制面通过、数据面未通过”。

### 阶段 AP2：SoftAP IPv4 数据面所有权

实现：

- CP SoftAP IPv4 ownership 开关。
- wire VIF 1 TX/RX。
- CP 关闭 SoftAP L3/DHCP。
- AP RX role 校验和 switch drain。

门禁：

- 抓包确认 DHCP Discover 到达 NuttX。
- CP 和 NuttX 不同时响应 ARP/DHCP。
- ARP request/reply、广播 IPv4 和单播 IPv4 双向正常。

### 阶段 AP3：NuttX DHCP 与本地 socket

实现：

- defconfig DHCP server 和广播。
- `192.168.10.1/24` 地址与 lease pool。
- 本地 TCP/UDP 测试服务。

门禁：

- Android、iOS/Windows/Linux 至少两类客户端可取得 lease。
- lease 地址位于 `192.168.10.2..5`，网段和掩码正确。
- 客户端可 ARP 和 ping `192.168.10.1`。
- 客户端与 BK7258 本地 TCP/UDP 服务可双向收发。
- AP stop 后不再发 beacon，DHCP daemon 可正常停止。

### 阶段 AP4：回归与稳定性

门禁：

- STA 原有扫描、关联、DHCP、DNS、TLS/HTTPS 全部回归通过。
- STA -> AP -> STA 切换 100 次，无旧 frame 串入新角色。
- 客户端关联/离开 1,000 次，client count 不负数、不泄漏。
- 4 客户端并发 DHCP 和 TCP/UDP 正常。
- SoftAP 运行 24 小时无 carrier 假状态、buffer 泄漏和 mailbox permanent busy。
- CP reset/AP reset 后不会解引用旧 pointer；恢复策略符合状态机。
- 压力期间 heartbeat、PWC、Mailbox UART 不超时。

### 阶段 AP5：可选双网卡共存

只有 AP1 至 AP4 通过后才评估：

```text
wlan0 = STA, wire VIF 0, DHCP client
wlan1 = SoftAP, wire VIF 1, static IPv4 + DHCP server
```

前置条件：

- driver 改为 per-interface lower-half、carrier、MAC、RX queue 和 config。
- RX 按 wire VIF 分派，TX 从 lower-half 得到固定 wire VIF。
- Beken AP+STA 同信道/CSA 行为有实板证据。
- 明确 STA channel 改变时 SoftAP 的 CSA、客户端保持和失败恢复。
- NAT/bridge 另行设计，不因注册双网卡自动成立。

## 10. 测试矩阵

### 10.1 配置边界

| 用例 | 预期 |
| --- | --- |
| 空 SSID | `-EINVAL` |
| 32 字节 SSID | 成功 |
| 33 字节 SSID | `-E2BIG` |
| WPA2 7 字节密码 | `-EINVAL` |
| WPA2 8/63 字节密码 | 成功 |
| WPA2 64 字节密码 | 首版拒绝，除非明确支持 64 hex PSK |
| Open + 非空密码 | 拒绝 |
| 非法 channel | 拒绝 |
| 非零 reserved | `-EPROTO` 或 `-EINVAL` |
| AP active 时 scan | `-EBUSY` |
| AP active 时改 mode | `-EBUSY` |

### 10.2 数据与异常

- 64、512、1500 字节 Ethernet frame。
- DHCP broadcast、ARP broadcast、IPv4 unicast。
- RX queue 满、TX slot 满和 mailbox `-EAGAIN`。
- AP start command timeout、start event 丢失和迟到 confirmation。
- 客户端在 AP stop 过程中发送数据。
- role switch 时仍有 TX completion。
- CP reset、AP reset、channel reset 和 heartbeat failure。
- 错误密码、重复关联、重复 disconnect 和 stop 幂等性。

## 11. 风险与回滚

| 风险 | 控制措施 |
| --- | --- |
| CP/NuttX 双 DHCP | 用独立 ownership 开关，抓包证明只有一个 Offer |
| wire VIF 写错 | TX/RX 统计按 0/1 分开，非法 role frame 立即回收并计数 |
| role 切换旧包污染 | carrier off、停止新 TX、等待 completion、RX 带 role |
| legacy AVDK 回归 | 新增 OpenVela command，不改旧 command 语义 |
| AP MAC 不正确 | 新增 AP status/MAC response，禁止用 base MAC 猜测 |
| 密码泄漏 | 不打印 payload/PSK，错误日志只含长度和状态码 |
| CP 构建源不唯一 | AP0 用 compile database/map 固定实际编译源后再改 |
| AP+STA 同信道限制 | 首版互斥；并发在 AP5 单独验证 |

每阶段保持可单独回滚：

- AP1 可仅回滚 OpenVela AP command/role state，STA wire 0 不变。
- AP2 的 CP ownership 由独立配置开关控制，关闭后恢复当前 STA-only 行为。
- AP3 只增加 NuttX DHCP 配置和应用，不改变射频控制。
- 未通过 AP4 前不默认启用 SoftAP，不影响当前 STA 交付配置。

## 12. 预计修改文件

确认后预计修改：

```text
board/beken/chips/bk7258/bk7258_wifi.c
board/beken/chips/bk7258/hardware/bk7258_wifi_ipc.h
board/beken/boards/bk7258/bk7258-ap/configs/nsh/defconfig
board/beken/boards/bk7258/bk7258-ap/configs/ai_agent/defconfig

external/bk_avdk_smp/cp/components/controller_if/cif_main.h
external/bk_avdk_smp/cp/components/controller_if/cif_cntrl.c
external/bk_avdk_smp/cp/components/controller_if/cif_wifi_dp.c

最终 CP 构建实际使用的 bk_wifi/net.c/config 文件
```

不计划修改：

- Wi-Fi MAC/PHY/RF、RWNX descriptor 和校准逻辑。
- Mailbox logical channel 编号。
- NuttX 通用 `netdev_upperhalf`、WAPI 或 DHCP server 源码。
- bootloader、分区、PSRAM 布局和 AP/CP 启动协议。

## 13. 完成定义

以下条件全部满足后，才能声明“BK7258 OpenVela Wi-Fi SoftAP 首版移植完成”：

- `wlan0` 可在 STA 和 SoftAP 间互斥切换。
- SoftAP 使用真实 AP MAC，start/stop/client event 状态正确。
- SoftAP TX/RX 使用 wire VIF 1，STA 继续使用 wire VIF 0。
- CP 是 EAPOL/hostapd owner，NuttX 是 SoftAP IPv4/DHCP owner。
- 手机或电脑可通过 Open/WPA2-PSK 关联并取得 NuttX DHCP lease。
- 客户端与 BK7258 本地 IPv4 TCP/UDP 服务双向通信。
- CP/NuttX 无双 DHCP、双 ARP 或同地址冲突。
- STA 全量回归、100 次模式切换和 24 小时稳定性通过。
- 构建、打包、烧录和实板日志均归档。

WPA3、IPv6、NAT、bridge、STA+AP 并发和双网卡不属于首版完成定义。

## 14. 方案决策与实施边界

当前项目已选定：

1. 首版使用单 `wlan0`，STA/SoftAP 互斥运行时切换，不直接做双网卡并发。
2. 平时使用 STA；配置和文件服务期间允许关闭 STA，切换过程中不重启单片机。
3. SoftAP 的 IPv4、ARP、DHCP 和本地 HTTP/File Server 数据面归 OpenVela，CP
   只保留 EAPOL、hostapd、射频和 SoftAP 控制职责。
4. 首版只支持 2.4 GHz、Open/WPA2-PSK、IPv4 和最多 4 个客户端。
5. 首版使用独立 `192.168.10.0/24` AP 网段，不提供 NAT、bridge 或通过 STA
   上网能力。
6. 新增 pointer-free command `0x210..0x212`，保留 legacy AP command 不变。

以下方案当前不实施：

- STA 与 SoftAP 同时常开并注册两个完整 NuttX netdev。
- STA+AP 并发下的 NAT 或 bridge。
- 仅由 CP 托管 SoftAP DHCP/IP、再通过跨核 HTTP/RPC 代理访问 OpenVela 文件系统。

按 AP0 -> AP1 -> AP2 -> AP3 -> AP4 顺序执行移植；AP5 双网卡共存只在首版通过
完整回归和稳定性门禁后重新评审。
