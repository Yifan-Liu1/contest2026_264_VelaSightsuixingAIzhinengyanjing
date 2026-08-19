# BK7258 OpenVela UART0 Mailbox V2完整收发移植方案

## 1. 文档目标和结论

本文是`docs/plans/BK7258_OPENVELA_AP_PORTING_PLAN.md`中UART0和Mailbox V2章节的可实施细化方案。依据当前工作区的OpenVela AP实现、`bk_avdk_smp` AP/CP源码、比赛仓CP覆盖文件、原厂网页文档和生成的内存布局，目标是在不占用CPU1物理UART1、不改变CP物理UART0下载和启动行为的前提下，实现：

```text
AP/OpenVela输出
  NuttX console/syslog/NSH stdout
    -> AP MB_UART0 TX
    -> Mailbox V2 logical channel 0x19
    -> CP MB_UART0 RX
    -> CP shell TX queue
    -> CP UART0/GPIO11/CH340/PC

PC输入
  PC/CH340/UART0 RX/GPIO10
    -> CP shell UART RX owner
    -> 显式AP console输入模式
    -> CP MB_UART0 TX
    -> Mailbox V2 logical channel 0x49
    -> AP MB_UART0 RX
    -> NuttX serial upper-half
    -> /dev/console/NSH stdin
```

最终结论：

- `MB_UART0`本身是全双工、带软件RTS/CTS流控的虚拟UART，不是单向日志命令。
- AP发送使用logical channel `0x19`，CP发送使用logical channel `0x49`；两者都是同一个MB_UART0的本端发送编号。
- 每个Mailbox V2 envelope只携带16字节message的地址和长度；DATA message再携带固定128字节交换区的地址和有效长度。
- 当前本地AP实现只完成AP到CP的输出；收到CP DATA后固定返回成功但不复制数据。当前CP覆盖也只读取AP输出，没有PC到AP输入路径。
- 完整移植必须同时修改OpenVela AP和Armino CP。只补AP RX或只调用CP `bk_mb_uart_write()`都不能形成完整控制台。
- 正式实现保持原厂16字节message、DATA/STATE和RTS/CTS线协议兼容，但必须修复本地实现的buffer所有权、ACK队列、reset、RX校验和NuttX serial接入问题；不得照搬原厂未初始化尾字节、未校验payload指针和失败后静默丢包等缺陷。

## 2. 基线、范围和非目标

### 2.1 开发和交付目录

- OpenVela AP开发目录：`contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/chips/bk7258/`。
- OpenVela board配置：`contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/boards/bk7258/bk7258-ap/`。
- CP可复现覆盖的权威副本：`contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/bk_avdk_smp/`。
- CP实际构建树：`bk_avdk_smp/`。CP改动必须先落权威副本，再按该目录`README.md`同步到构建树并逐字节检查。
- `bk_idk`、`bk_solution_ai`、`vendor_beken`和网页文档仅作为参考，不是BK7258 OpenVela正式开发目录。

### 2.2 本轮范围

- Mailbox V2 CPU1 channel 1物理收发、IRQ79、FIFO drain和错误恢复。
- CPU1到CPU0 logical channel复用调度，至少不破坏HW_CTRL、PWC和UART0。
- AP侧MB_UART0 DATA/STATE、双向ring、流控、ACK和NuttX serial lower-half。
- CP侧MB_UART0接收输出和UART0输入转发。
- CP CLI与AP NSH对唯一UART0输入的显式所有权切换。
- 冷启动、AP重启、CP重启、Mailbox reset和压力验收。

### 2.3 非目标

- 不启用CPU1物理UART1，不切换GPIO0/1，不要求第二个USB-TTL。
- 不用`MB_CHNL_LOG`的`MB_CMD_USER_INPUT`替代逐字节TTY。该协议只能转发完整Armino CLI命令，不具备NSH readline、退格和Ctrl-C语义。
- 不修改bootloader下载串口协议。AP console接管只能发生在CP运行期，并且必须在AP ready以后。
- 不承诺在Mailbox IRQ和调度器可用前输出启动日志。panic时若中断关闭，MB_UART也不能工作；原厂文档同样明确其不是无中断polling UART。
- 第一版不启用MB_UART CRC8。原厂当前配置下CRC恒为0；单边启用会不兼容。保留字段并校验其为0，后续只能在AP/CP同时升级后启用。

## 3. 已确认的协议事实

### 3.1 CPU和logical channel编码

logical channel为8位：

```text
bit[7:6] destination CPU
bit[5:4] source CPU
bit[3:0] logical channel index
```

CPU0/CP为0，CPU1/AP为1。UART0的组内index为9，因此：

| 本端 | 方向 | 发送channel | 接收报文中的channel |
| --- | --- | ---: | ---: |
| AP/OpenVela | CPU1 -> CPU0 | `0x19` | `0x49` |
| CP/Armino | CPU0 -> CPU1 | `0x49` | `0x19` |

参考：

- `bk_avdk_smp/ap/include/driver/mailbox_channel.h:32-44,53,77-96`
- `bk_avdk_smp/cp/include/driver/mailbox_channel.h:29-41,49,64-83`

不得将`0x19`和`0x49`理解为两个独立UART，也不得用IPC `0x10`、PWC `0x12/0x42`或`MB_CHNL_LOG`承载UART字节流。

### 3.2 Mailbox V2 envelope

MBOX0硬件FIFO中的两个data word是：

| FIFO word | 含义 |
| --- | --- |
| `data[0]` | 发送端稳定16字节message的32位地址 |
| `data[1]` | 固定为16 |

接收端根据硬件SID确认源CPU，再复制16字节message。AP接收的message地址来自CP拥有的稳定SRAM，CP接收的地址来自AP拥有的稳定SRAM。不能让FIFO引用栈变量，也不能在对端复制前复用或改写该slot。

### 3.3 16字节message header

word0布局固定如下：

```text
bit[7:0]   cmd
bit[11:8]  state
bit[15:12] ctrl
bit[23:16] tx_seq
bit[31:24] logical_chnl
```

`ctrl`定义：

| 位 | 名称 | 语义 |
| ---: | --- | --- |
| 0 | `CHNL_CTRL_ACK_BOX` | 1表示transport ACK，0表示command |
| 1 | `CHNL_CTRL_SYNC_TX` | 无ACK同步发送；正式UART普通路径不使用 |
| 2 | `CHNL_CTRL_RESET` | transport重同步 |

`state bit0`为`CHNL_STATE_COM_FAIL`。ACK必须至少匹配`source/destination + logical_chnl + tx_seq + cmd`，不匹配的ACK只能计为stale/bad ACK，不能释放当前事务。

### 3.4 MB_UART message布局

协议运行于ARM32 little-endian。不得在OpenVela实现中继续依赖编译器bitfield，使用显式mask/shift和定宽字段。

DATA/STATE command为16字节：

| offset | 大小 | 字段 |
| ---: | ---: | --- |
| 0 | 1 | `cmd`：DATA=0，STATE=1 |
| 1 | 1 | state/ctrl，发送command时初始为0 |
| 2 | 1 | transport sequence |
| 3 | 1 | logical channel |
| 4 | 4 | DATA交换buffer地址；STATE为0 |
| 8 | 2 | DATA有效长度1..128；STATE为0 |
| 10 | 1 | bit0=`uart_rts`，其余必须0 |
| 11 | 1 | `crc8`，当前兼容值为0 |
| 12 | 4 | 保留，必须清0 |

ACK为同一个16字节slot的响应语义：

| offset | 含义 |
| ---: | --- |
| 0..3 | 原cmd、state，加ACK ctrl、sequence和channel |
| 4 bit0 | 接收端当前`uart_rts` |
| 4 bit1 | `uart_tx_fail`，本DATA整包未接收 |
| 4 bit7:2 | 0 |
| 5..15 | 0 |

所有command和ACK结构在构造前必须`memset(..., 0, 16)`，避免原厂实现中offset 12..15栈残留和ACK尾部旧数据泄漏。这是AP/CP同步升级后的正式ABI要求：当前原厂CP command确实可能带非零栈残留，因此CP清零修复必须先于AP启用严格保留字段拒绝策略，不能只升级AP。

### 3.5 UART0交换区和所有权

项目RAM布局固定预留：

```text
SWAP = 0x2809f800..0x2809ffff, size 0x800
```

来自：

- `bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv:6-10`
- 生成的`ram_regions.h`中`CONFIG_SWAP_ADDR=0x2809f800`、`CONFIG_SWAP_SIZE=0x800`

每个支持共享payload的logical channel占`0x200`字节，顺序为HW_CTRL、LOG、UART0、UART1。每个方向/CPU slot为128字节。当前CPU0/CPU1 UART0精确映射为：

| 地址 | owner/方向 | AP视角 |
| --- | --- | --- |
| `0x2809fc00..0x2809fc7f` | CP host_tx[CPU1] | AP RX payload，只读后复制 |
| `0x2809fc80..0x2809fcff` | CPU2预留 | AP不得使用 |
| `0x2809fd00..0x2809fd7f` | AP host_rx[CPU1] | AP TX payload，ACK前保持不变 |
| `0x2809fd80..0x2809fdff` | CPU2预留 | AP不得使用 |

OpenVela linker必须新增独立`SWAP` memory region并标记为`NOLOAD`。当前构建顺序是先构建OpenVela、后构建SMP，OpenVela不能读取尚未生成的CP `ram_regions.h`；单一来源固定为比赛仓已提交的`external/bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv`。增加预构建生成/校验脚本，由该CSV同时生成或核对OpenVela board header和linker fragment；最终SMP构建后再与Armino生成的`CONFIG_SWAP_ADDR/SIZE`自动比较。不能维护两套无校验硬编码。

不能把SWAP并入AP heap，不能由AP启动代码清零整个SWAP，也不能在AP `.bss`另建一个“看起来等价”的交换buffer。

首版在`bk7258_start.c`增加显式共享MPU region，并先读取MPU `TYPE.DREGION`确认region预算：CP普通RAM中的envelope窗口为shareable、non-cacheable、RO/XN；CP拥有的UART0 RX payload `0x2809fc00..0x2809fc7f`为RO/XN；AP拥有的UART0 TX payload `0x2809fd00..0x2809fd7f`为RW/XN。当前MPU使用`PRIVDEFENA`，未覆盖地址对privileged内核仍可访问，因此CPU2预留slot只能由MB_UART软件白名单/API禁止访问，不能声称由MPU隔离。

若硬件region数不足以按128字节拆分，才允许将整个SWAP设为RW/XN，但必须记录这是硬件粒度降级并继续以软件白名单强制方向所有权，不能把CP普通RAM扩大为RW。若未来要求硬件阻止访问预留slot，必须关闭`PRIVDEFENA`并完整映射所有必要内存，这不属于本轮UART移植。

若后续启用D-cache，所有ownership交接统一由共享内存API执行：发布前clean + DMB，读取前invalidate + DMB，ACK后才复用；cache line为32字节，操作必须向外对齐且不能误刷相邻owner数据。

## 4. 当前实现差距

### 4.1 AP侧

当前`bk7258_mailbox_channel.c`具备基础`0x19/0x49`分派、AP TX ring和transport ACK匹配，但存在以下交付阻断项：

1. `MB_CHNL_UART0_RX`只检查DATA/STATE后固定返回`uart_rts=0, uart_tx_fail=0`，不校验、不复制CP payload，输入被静默丢弃。
2. 没有MB_UART RX ring、RTS高低水位、STATE发送和对端RTS/CTS门控。
3. AP TX payload使用普通AP `.bss`中的`g_uart_xchg`，没有复用原厂SWAP地址和跨核固定所有权。
4. 单个`g_ack_message`在FIFO full时可被下一条RX覆盖。
5. ACK超时后清busy并原地改sequence重发同一buffer，会造成旧FIFO envelope观察到新内容、重复日志和非幂等PWC重复执行。
6. reset只清physical busy，没有终止/失败完成active transaction、修复UART inflight或向CP重新同步。
7. 初始化在注册IRQ后直接丢弃RX FIFO，已到达command得不到ACK，可能令CP永久busy。
8. envelope仅白名单CP普通RAM，payload还需要独立SWAP精确白名单。
9. logical channel和MB_UART混在一个文件，无法清晰复用和测试状态机。
10. 仅`up_irq_save()`适用于当前单核AP；进入NuttX SMP后不能阻止CPU2并发。

当前`bk7258_serial.c`的`receive()`固定返回-1、`rxavailable()`固定false、`rxint()`为空，因而`read()`、`poll(POLLIN)`和NSH均无输入。设备同时冒充硬件UART1并注册`/dev/ttyS1`，Kconfig/defconfig也错误命名为UART1。

当前`bk7258_syslog.c`忽略实际写入结果并总是返回全部长度，单次`size_t`还隐式截断为`uint16_t`；`force`与普通路径相同，不能提供panic flush保证。

### 4.2 CP侧

当前CP覆盖`cp/middleware/driver/common/driver.c`只完成：

```text
bk_mb_uart_read(MB_UART0) -> 按行聚合 -> 添加"ap0: " -> shell_log_raw_data()
```

它没有`bk_mb_uart_write(MB_UART0, ...)`，也没有从物理UART0 RX到MB_UART0 TX的路由。50 ms半行刷新和每行前缀适合日志，不适合作为NSH raw terminal：prompt会延迟，echo和ANSI序列会被改写。

不能在`driver.c`另外注册UART0 RX ISR，因为CP shell已经通过`shell_uart_open()`占用UART driver唯一RX callback；后注册会覆盖CP shell。输入路由必须在现有shell UART owner内部或其读取后的`rx_ind_process()`入口完成。

## 5. 目标软件分层

### 5.1 AP文件边界

建议将当前单文件拆为最小但清晰的三层：

```text
bk7258_mbox0.c
  Mailbox V2寄存器、FIFO、IRQ、SID、W1C错误、suspend/resume

bk7258_mailbox_channel.c
  16字节message slot、logical channel调度、ACK queue、seq/reset/timeout
  HW_CTRL/PWC/UART0共用，不包含NuttX serial语义

bk7258_mb_uart.c
  MB_UART0 DATA/STATE、SWAP payload、TX/RX ring、RTS/CTS、统计

bk7258_serial.c
  NuttX uart_ops_s、/dev/console和/dev/ttyMB0注册

bk7258_syslog.c / bk7258_lowputc.c
  有界早期输出和Mailbox ready后的非阻塞日志入口
```

新增内部头文件导出定宽message、状态查询和callback，不在多个C文件重复`extern`声明。Kconfig新增`CONFIG_BK7258_MBOX_V2`和`CONFIG_BK7258_MB_UART0_CONSOLE`，并分别配置AP软件TX/RX ring大小；交换chunk保持协议固定128字节，不做Kconfig。

### 5.2 CP文件边界

CP保持原厂`mb_uart_driver.c`和唯一`MB_UART0`实例。新增桥服务和shell输入hook：

```text
cp/middleware/driver/common/driver.c
  或独立ap_console_bridge.c：MB_UART0唯一init、RX输出、TX队列和统计

cp/components/bk_cli/shell_task.c
  物理UART0收到字节后的输入owner选择

cp/components/bk_cli/...头文件
  AP console模式切换API和CLI命令
```

若新增CP源码文件，还必须同步修改对应CMake/Make源列表。所有可复现CP改动都要保存在比赛仓`external/bk_avdk_smp`镜像中，不能只修改`bk_avdk_smp`构建树。

## 6. AP Mailbox V2 transport设计

### 6.1 物理层

CPU1只配置自己拥有的channel 1：FIFO start=2、length=3，接收IRQ source 63映射到NuttX IRQ79。物理层初始化必须在`up_initialize()`可使用NuttX IRQ API后、board late逻辑服务前完成。

ISR固定执行：

1. 读取并累计`wrerr/rderr/wrfull`等W1C错误状态。
2. 循环读取FIFO直到empty，不能每次中断只取一个。
3. 校验SID为CPU0、length为16、地址4字节对齐、`address + 16`无溢出且位于CP RAM `0x28064000..0x2809f6ff`。当前CP `mbox0_adapter.c`的envelope slot是可随链接变化的普通`.bss mailbox_buff`，并没有固定地址ABI或32字节对齐，AP不能写死更小窗口。
4. 立即复制到AP-owned RX descriptor queue，随后上层处理；不能长期持有CP message指针。
5. 若RX descriptor queue或ACK资源不足，停止继续drain并保留中断，不能读出后无ACK丢弃。

初始化时不再盲目drain。应先安装logical callback和RX/ACK资源，再处理已有FIFO。无法识别的有效command也必须返回`COM_FAIL` ACK。

### 6.2 稳定transaction slot

每个CPU0物理方向同时只允许一个command in-flight，以兼容原厂physical BUSY模型。实现至少准备：

- 每个logical channel一个16字节pending command副本。
- 一个独立16字节active slot；发送后直到ACK/reset完成不可修改。
- 至少3个AP-owned ACK slots和一个ACK FIFO，容量不小于硬件RX FIFO深度。
- AP新建的active/ACK message slot按32字节对齐并位于AP RAM白名单；接收CP envelope只要求原厂ABI的4字节对齐和CP RAM范围。payload SWAP另行精确校验。

slot释放事件必须明确，因为硬件FIFO保存的是地址而不是16字节值：

- command正常收到匹配ACK后可释放。
- command timeout后转为quarantine；发送RESET不释放它。RESET和随后STATE使用各自独立稳定slot。只有STATE收到成功ACK，利用同一AP->CP FIFO顺序证明旧command、RESET和STATE都已被复制后，才一起释放旧command和RESET slot。STATE失败时保持quarantine，直到MBOX硬件重新初始化，不能因进入DOWN就复用。
- ACK slot不能以“收到对端下一条command”为释放证据，因为对端timeout后可能绕过旧ACK继续发送。只有本端随后一个同方向、需要ACK的普通事务收到成功ACK，才能证明排在它之前的本端ACK envelope已经被对端复制。
- ACK pool达到高水位（建议已占用2/3）时，transport必须立即在所有待发ACK之后调度一个独立稳定slot的STATE probe。probe收到成功ACK后，按本端FIFO发送顺序批量释放排在它之前的ACK slots；probe失败则进入DOWN并保持这些slot quarantine。不能依赖heartbeat或应用数据未来恰好出现。
- RESET和SYNC都没有自己的ACK。RESET由紧随其后的成功STATE释放；通用SYNC使用独立slot，并在下一个同方向普通事务成功完成后释放。专用slot尚未释放时再次调用同类旁路发送返回`-EBUSY`，不得有限轮转后覆盖。
- ACK在DOWN、ABORTING、PROBING和QUIESCING状态下仍是最高发送优先级；状态变化不能免除已经接收的对端command所需ACK。

CP `mbox0_adapter.c`也按相同原则分别管理command、ACK、RESET和SYNC slot，禁止现有“每次发送无条件轮转两个slot”的做法。

调度优先级保持原厂“logical index越小优先级越高”：HW_CTRL、PWC优先于UART0，避免日志压力阻塞heartbeat和电源命令。ACK永远高于新command。

### 6.3 ACK和超时

收到ACK后完整检查：

```text
SID == CPU0
ACK_BOX == 1
logical_channel == active.channel
tx_seq == active.seq
cmd == active.cmd
RESET/state/reserved组合合法
```

匹配成功后才释放active slot并调用logical completion。UART completion再根据`COM_FAIL`和`uart_tx_fail`决定成功或失败。

超时不得直接修改active buffer并重发。第一版策略：

1. 200 ms无ACK，标记当前transaction失败并保留错误快照。
2. 停止所有普通新TX，向CPU0发送原厂兼容RESET abort通知；RESET使用独立稳定slot。
3. RESET是`ACK_BOX|RESET`单向通知，CP空闲时会忽略，不返回RESET ACK。AP发送成功后进入PROBING，并用独立slot发送后续MB_UART STATE；不能等待不存在的RESET ACK，也不能复用被abort事务的slot。
4. abort发出或收到peer reset后，所有pending非幂等PWC请求以`-ECONNRESET`完成，不自动重放。
5. UART chunk可保留在本地TX ring，重新交换STATE后最多重发一次；日志允许重复风险时也必须通过计数可见。建议默认不自动重发已经可能被对端消费但ACK丢失的chunk，而是丢弃该chunk并累计`tx_unknown`，避免终端出现重复字符。
6. RESET envelope发送失败或后续STATE仍无ACK则link进入DOWN，heartbeat/ready逻辑可检测并执行既定降级或复位策略。

8位sequence在每次新transaction递增，0可自然出现；本地abort后建立新的软件epoch。不能仅靠wire sequence解决跨reset的极晚ACK。链路状态统一为`DOWN/ABORTING/PROBING/READY/QUIESCING`：ABORTING不发新的普通command，PROBING只发STATE普通command，但所有状态始终优先发送已欠的ACK。

只有STATE ACK同时满足channel/sequence/cmd匹配、`COM_FAIL==0`、`uart_tx_fail==0`、保留字段合法并成功取得peer RTS时，才能从PROBING进入READY。匹配但带`COM_FAIL`的ACK表示CP尚未open MB_UART0，应在有界退避后继续探测，达到总超时后进入DOWN。

### 6.4 reset方向

AP transport初始化完成后主动向CP发送一次原厂RESET abort通知，作用是让CP结束可能指向上一个AP boot的in-flight transaction。它不是双向握手：AP不等待响应，而是随后发送STATE探测。收到CP RESET时：

- 对当前active事务生成`-ECONNRESET` completion，但其slot转入quarantine而不复用。反方向收到peer RESET不能证明本端目标FIFO中的旧envelope已经被复制。
- 终止本端active/pending outgoing事务并清理其physical busy和peer RTS已知状态；不得清理已经接收且尚未发送的ACK queue。
- 保留尚未提交的本地UART TX ring，丢弃不完整AP RX chunk。
- 不返回RESET ACK；按原厂语义完成本端abort，再由MB_UART STATE/ACK恢复流控。

CP侧原厂channel层已有处理AP reset并将旧事务以`COM_FAIL`完成的基础逻辑；CP空闲时会直接忽略RESET。上板前需验证当前CP分支未被其他补丁破坏。

quarantine slot只能由后续同方向STATE成功ACK释放，或由明确清空AP->CP目标channel/FIFO的CP侧重初始化、全系统复位释放。AP只重初始化自己的channel 1、IRQ或RX FIFO不能证明CPU0目标FIFO中的旧地址已消失，不得作为释放条件。

## 7. AP MB_UART0设计

### 7.1 控制块

AP MB_UART0至少维护：

- `tx_ring`：建议4096或8192字节，用于console/syslog burst，单producer假设不可成立，写入需NuttX spinlock保护。
- `rx_ring`：257字节，实际容量256，与原厂语义一致。
- 固定AP TX SWAP地址`0x2809fd00`，固定AP RX SWAP地址`0x2809fc00`。
- `tx_inflight_len`、`tx_command`、`tx_busy`和`state_pending`。
- `local_rts`：AP RX流控；0允许CP发送，1暂停。
- `peer_rts`：来自CP command/ACK；0允许AP发送，1暂停。
- `rx_irq_enabled`：NuttX serial upper-half当前是否允许接收。
- link状态：DOWN/ABORTING/PROBING/READY/QUIESCING。
- 分项统计：write/drop、DATA/STATE、overflow、bad pointer/length/CRC、remote fail、ACK timeout、reset、high watermark持续时间。

### 7.2 AP发送

`write()`只把实际可容纳的字节数写入TX ring并返回该字节数，不返回“部分写入后-ENOSPC”这种不可恢复语义。NuttX serial `send()`每次一个字符，syslog bulk write则循环分片并准确报告短写。

发送条件：link READY、无MB_UART transaction in-flight、`peer_rts==0`、TX ring非空。每次最多取128字节复制到`0x2809fd00`，构造DATA：

```text
cmd_buff     = 0x2809fd00
cmd_data_len = min(tx_count, 128)
uart_rts     = local_rts
crc8         = 0
reserved     = 0
```

与当前本地代码不同，TX ring读指针在成功ACK后才推进。收到`COM_FAIL`或`uart_tx_fail`时不推进；先记录失败并按reset策略处理，避免原厂“提交logical write即消费，ACK失败静默丢失”的缺陷。

若没有数据但`local_rts`发生变化，发送STATE。DATA ACK本身已携带最新RTS，因此在DATA处理中可合并待发STATE，但不能丢掉最终状态变化。

### 7.3 AP接收

收到channel `0x49` command后先验证header，再处理：

DATA必须同时满足：

- `cmd_buff == 0x2809fc00`，不接受“位于SWAP任意位置”或普通CP RAM中的任意指针。
- `1 <= cmd_data_len <= 128`，地址加法无溢出。
- `crc8 == 0`且保留位为0。
- 当前link允许接收。

接收前执行共享内存acquire/cache invalidate。只有RX ring剩余空间大于等于整包长度时才整包复制；否则整包不复制，设置sticky overflow并在ACK中置`uart_tx_fail=1`。绝不部分接收后报告失败，否则发送端无法确定消费边界。

无论DATA还是STATE，都更新`peer_rts=command.uart_rts`。响应ACK包含最新`local_rts`。成功DATA复制后触发NuttX RX处理；ISR内不运行NSH、不格式化日志。

### 7.4 RTS/CTS水位

保持原厂兼容阈值：

- RX ring总数组257字节、容量256。
- 剩余空间`<=128`时设置`local_rts=1`，确保已经在途的一个最大128字节chunk仍有明确处理结果。
- 应用读取后，occupancy低于102字节时设置`local_rts=0`并发送STATE，提供滞回避免状态抖动。
- `peer_rts=1`时AP禁止新DATA，但仍允许必要的ACK、RESET和本端STATE。

由于NuttX还有一层`uart_dev_s.recv` ring，MB_UART RX ring的RTS必须根据“MB_UART ring可用空间”管理；`uart_recvchars()`应及时搬运到NuttX ring。新的`BK7258_MB_UART0_CONSOLE` Kconfig必须`select OTHER_UART_SERIALDRIVER`、`select OTHER_SERIAL_CONSOLE`和`select SERIAL_IFLOWCONTROL`，不能只在defconfig手写隐藏symbol；`OTHER_SERIAL_CONSOLE`再通过NuttX console choice选择`SERIAL_CONSOLE`。

第一版同时启用`SERIAL_IFLOWCONTROL_WATERMARKS`，upper/lower watermark固定为75%/25%。NuttX upper-half到达75%时停止继续搬运并保持/拉高MB_UART RTS；低于25%后重新调用搬运并释放RTS。若不启watermark选项，NuttX只会在full/empty回调，不能声称具备本文的高低水位行为。

### 7.5 NuttX serial接入

正式设备不再冒充物理UART1：

- 注册`/dev/console`和`/dev/ttyMB0`到同一个Mailbox UART device。
- 删除`CONFIG_UART1_SERIALDRIVER`、`CONFIG_UART1_SERIAL_CONSOLE`和`BK7258_AP_UART1`误导配置。
- 新增板级`BK7258_MB_UART0_CONSOLE`选择，并让NuttX console宏映射到该虚拟设备。
- 不执行任何UART1 clock、pinmux或GPIO0/1配置。

`uart_ops_s`实现：

| op | 行为 |
| --- | --- |
| `setup/attach` | 确认MB_UART已注册callback，不能重复初始化physical mailbox |
| `shutdown/detach` | 普通console关闭不销毁全局heartbeat共用transport |
| `receive` | 从MB_UART RX ring弹出1字节，status返回overflow/parity sticky快照 |
| `rxavailable` | 查询MB_UART RX ring非空 |
| `rxint` | 保存enable；enable时投递RX worker或调用受控`uart_recvchars()` |
| `send` | 写一个字节到MB_UART TX ring；只在`txready`为true时由upper-half调用 |
| `txint` | enable时调用`uart_xmitchars()`，ACK/CTS恢复后再次kick |
| `txready` | link ready、TX ring有空间；不要求物理channel当前idle |
| `txempty` | NuttX xmit ring、MB_UART TX ring和active DATA都为空 |
| `rxflowcontrol` | upper-half高水位时拉高local RTS，恢复时拉低并kick RX |

Mailbox ISR只将DATA复制到MB_UART RX ring并post一个高优先级worker/semaphore。worker调用`uart_recvchars()`，避免在硬件Mailbox ISR里进入完整TTY路径。TX ACK或peer RTS从1变0时，同样由worker调用`uart_xmitchars()`。

### 7.6 syslog和early putc

- transport ready前允许写入一个小型有界early TX ring；满后丢弃并计数，不阻塞启动。
- transport ready后由MB_UART worker排空early数据。
- bulk syslog按`size_t`循环，每次传入不超过`UINT16_MAX`，返回实际接受字节数；不能无条件返回原长度。
- `sc_force`和`arm_lowputc`仍只能best-effort，不宣称IRQ关闭时可靠。可提供有界flush，例如等待不超过20 ms；禁止永久等待ACK。
- 避免console和独立syslog backend重复输出同一条日志。最终配置只能有一个主syslog路径。

## 8. CP UART0双向桥设计

### 8.1 唯一MB_UART0 owner

CP只调用一次`bk_mb_uart_dev_init(MB_UART0)`。现有`ap_uart0_log_init()`扩展为统一bridge：

- 注册MB_UART RX callback，唤醒AP输出worker。
- 注册MB_UART TX callback，唤醒PC输入发送worker。
- 使用有界CP->AP TX ring，建议512或1024字节。
- worker根据`bk_mb_uart_write_ready()`处理部分写入，调用`bk_mb_uart_write()`，直到队列为空或peer RTS暂停。
- 暴露drop、partial write、MB_UART status、mode switch和last error统计。

不得为RX和TX分别初始化两次MB_UART0，也不得让日志服务和console服务分别open channel。

### 8.2 AP输出模式

提供两个输出策略，但共享同一个MB_UART RX：

1. `LOG`模式：保持当前按行添加`ap0: `、50 ms半行flush和有限重试，适合AP后台日志。
2. `RAW_CONSOLE`模式：不添加前缀、不等待换行，收到后立即通过CP shell TX queue输出，保证`nsh>`、echo、退格和ANSI字节透明。

进入AP交互console时切为RAW；退出后恢复LOG。模式切换前先flush旧的半行缓存，防止旧日志与prompt拼接。CP自身异步日志仍可能插入AP终端，第一版应在RAW模式下抑制非关键CP日志或明确串行化，至少不能从两个线程直接写物理UART0。

### 8.3 UART0输入所有权

CP shell已拥有物理UART0 RX ISR和buffer，因此路由点放在`shell_task.c`读取UART字节之后、执行`handle_shell_input()`之前。状态固定为：

```text
CP_CLI       所有输入由CP CLI处理
AP_CONSOLE   原始输入转发给MB_UART0，CP CLI不再解析
```

上电默认`CP_CLI`。只有满足以下条件才允许进入`AP_CONSOLE`：

- AP已发送Mailbox RESET abort通知并完成STATE/ACK链路探测。
- AP已通过HW_CTRL command 1的原厂完成ACK，并完成PWC `0x5` boot milestone；PWC消息不替代IPC门禁。
- CP bridge确认MB_UART0可写。

CP CLI新增明确命令，例如`ap_console open`。命令handler只设置`SWITCH_TO_AP_PENDING`，不能立即修改owner：先等待当前命令响应和CP TX queue flush，抑制尾随`cp>`，原子清理`cmd_line_buf`、BK_REG/AT/escape解析状态并通过`SHELL_IO_CTRL_RX_RESET`清物理UART RX残留，之后才进入AP_CONSOLE并打印一次切换提示。

AP_CONSOLE保留一个本地退出转义，建议使用行首`Ctrl-]`后跟`.`；解析器只在行首识别完整转义，其他字节原样转发。退出时进入`SWITCH_TO_CP_PENDING`，丢弃未完成转义、停止接收新AP输入、清理或明确丢弃尚未发送的bridge TX，flush AP RAW输出后再恢复CP parser和prompt。退出转义由CP消费，不发送给NSH。

禁止把同一字节同时广播给CP CLI和AP NSH，否则两个shell会同时echo/执行。下载工具、ATE和bootloader阶段不进入该状态机，维持原UART0 owner。

### 8.4 PC输入发送

在AP_CONSOLE状态，CP shell现有UART RX路径读取到原始字节后：

1. 先运行最小退出转义状态机。
2. 非转义字节写入CP->AP bridge ring。
3. bridge worker根据MB_UART write-ready发送；不得在UART ISR中调用可能触发Mailbox发送和复杂锁的接口。
4. ring满时优先通过MB_UART RTS自然回压；物理UART0无硬件流控，仍可能在高速粘贴时丢字，必须计数并在退出RAW后报告。
5. 可选的软件输入限速按115200估算，但不得sleep在UART ISR；只能由worker分批发送。

AP link down/reset时CP自动退出AP_CONSOLE，清理未发送输入并恢复CP_CLI，避免用户被锁死在无响应终端。为使该行为可实现，CP `mailbox_channel.c`必须新增peer RESET/physical timeout通知，IPC heartbeat/PWC recovery也要向bridge发布AP_DOWN事件；不能从`bk_mb_uart_get_status()`推断link状态。bridge通过统一event callback处理`AP_READY/AP_DOWN/PEER_RESET`，并在事件中只唤醒worker，不直接操作shell队列。

### 8.5 CP原厂驱动最小加固

在不改变线协议的前提下同步修复：

- `mb_uart_init()`第二个NULL检查应为`rx_xchg_buff`。
- DATA RX校验`cmd_buff`必须等于本端预期`rx_xchg_buff`，长度1..128。
- 所有command/response先清零保留字段。
- bridge必须正确处理`bk_mb_uart_write()`部分写和`bk_mb_uart_get_status()`。
- `mailbox_channel.c`向bridge暴露peer reset/timeout通知；收到AP reset时即使CP physical TX为空也必须发布事件，而不是只记fault后静默返回。

原厂驱动在logical write成功时提前推进TX ring，远端`uart_tx_fail`时不会回滚。若不重写CP mb_uart状态机，CP输入可靠性仍低于AP改进实现。正式交付建议把CP TX也改为“ACK成功后推进ring”，但这会扩大原厂代码改动。实施可分两步：先完成兼容双向链路并测出失败路径，再将该加固作为进入压力验收前的必做项，而不是长期保留静默丢输入行为。

## 9. 启动、ready和恢复时序

### 9.1 CP时序

```text
CP bootloader/download UART0保持原行为
  -> CP UART0 115200 8N1和shell初始化
  -> Mailbox V2 physical/channel初始化
  -> MB_UART0唯一bridge init，注册RX/TX callback
  -> 默认CP_CLI模式
  -> 启动AP
  -> 等待AP IPC power-up
  -> 等待AP PWC boot ready
  -> 允许用户执行ap_console open
```

### 9.2 AP时序

```text
CPU1 reset
  -> MPU/vector/基础NuttX启动
  -> 注册有界early syslog，不访问UART1
  -> up_initialize调用arm_serialinit
  -> arm_serialinit在uart_register前初始化MBOX0 IRQ79和RX资源
  -> 初始化logical transport、ACK pool和MB_UART0 rings/SWAP/callback
  -> 创建Mailbox/UART RX/TX worker；允许其在调度器运行后启动
  -> 发送单向RESET abort通知，不等待RESET ACK
  -> 异步发送STATE(local_rts=0)，arm_serialinit不阻塞等待
  -> 注册/dev/console和/dev/ttyMB0
  -> board late在worker已运行后有界等待STATE transport ACK
  -> 发送IPC power-up并确认transport ACK
  -> 启动heartbeat和PWC worker
  -> 发送PWC boot ready
  -> 启动NSH
```

`arm_serialinit()`运行在公共`up_initialize()`中，OS/driver服务可用，但不得依赖用户初始化线程已经运行，因此这里只安装全部资源并异步kick。STATE ACK的有界等待放到board late，此时worker和系统tick已明确运行。

ready门禁新增：MB_UART RX已能整包复制并调用NuttX worker，TX/STATE完成至少一次探测。日志输出失败不能无限阻塞heartbeat；有界失败后记录link状态，并按产品策略决定继续无console运行还是拒绝ready。比赛调试固件建议MB_UART失败时拒绝ready，避免出现“系统似乎启动但唯一console不可用”。

### 9.3 suspend/resume和重启

首版若未完整实现Mailbox PM，必须对相关deep sleep投票禁止进入，不能让CP关闭共享资源后AP仍认为link READY。

实现PM后流程：

- suspend：停止新DATA，等待active有界完成，拉高RTS，发送STATE，保存统计并关闭IRQ route。
- resume：重配CPU1 FIFO/IRQ、清W1C错误、发送单向RESET abort通知、用STATE/ACK探测，再恢复DATA。
- AP重启：AP主动发送RESET abort通知；CP transport发布PEER_RESET/AP_DOWN事件，bridge将旧AP TX以失败完成并自动退出AP_CONSOLE。
- CP重启：AP ACK timeout进入ABORTING/DOWN，不继续写旧payload；CP恢复后重新执行RESET abort和STATE/ACK探测。

## 10. 具体文件修改清单

### 10.1 OpenVela AP

| 文件 | 修改 |
| --- | --- |
| `board/beken/chips/bk7258/bk7258_mbox0.c` | 移除盲目drain；补错误W1C、RX资源门禁、suspend/resume和正确destination status |
| `board/beken/chips/bk7258/bk7258_mailbox_channel.c` | 稳定active slot、ACK FIFO、完整ACK关联、reset状态机、有限timeout；移出UART ring逻辑 |
| `board/beken/chips/bk7258/bk7258_mb_uart.c` | 新增完整DATA/STATE、SWAP、TX/RX ring、RTS/CTS和统计 |
| `board/beken/chips/bk7258/bk7258_serial.c` | 实现RX ops、worker kick、flow control；注册`/dev/ttyMB0` |
| `board/beken/chips/bk7258/bk7258_syslog.c` | 正确短写、size_t分片、有界force/flush和drop统计 |
| `board/beken/chips/bk7258/bk7258_lowputc.c` | 保持不pinmux UART1，明确best-effort early输出 |
| `board/beken/chips/bk7258/bk7258_start.c` | 增加CP envelope/SWAP共享MPU region、region数量门禁和RO/RW方向权限 |
| `board/beken/chips/bk7258/hardware/bk7258_mbox.h` | 统一协议结构、公开API和地址常量；加static assert |
| `board/beken/chips/bk7258/Make.defs`、`CMakeLists.txt` | 条件编译新增MB_UART文件 |
| `board/beken/boards/bk7258/bk7258-ap/Kconfig` | 用MB_UART0 console选项替换UART1误导项；select OTHER UART driver/console和iflowcontrol |
| `board/beken/boards/bk7258/bk7258-ap/configs/nsh/defconfig`、`configs/ai_agent/defconfig` | `nsh` 为最小门禁，`ai_agent` 为 VelaSight 产品；两者均删除UART1 console，启用Mailbox console和75%/25% flow-control watermarks |
| `board/beken/boards/bk7258/bk7258-ap/scripts/ld.script` | 增加SWAP NOLOAD region/section和边界ASSERT，heap不得覆盖 |
| board RAM配置生成/校验脚本 | 从external `ram_regions.csv`生成或核对OpenVela SWAP linker/header，最终与Armino生成头比较 |
| `bk7258_serial.c::arm_serialinit()`、`bk7258_bringup.c` | `arm_serialinit()`在注册device前创建并kick worker、排队RESET/STATE；board late只等待成功STATE并继续IPC/PWC ready，不重复启动worker |

建议在公开结构上编译期断言：message 16字节、header offset 0、payload地址offset 4、length offset 8、flags offset 10、CRC offset 11。

### 10.2 CP覆盖

| 权威副本目标 | 修改 |
| --- | --- |
| `external/bk_avdk_smp/cp/middleware/driver/common/driver.c`或新增bridge文件 | MB_UART0统一双向bridge、RAW/LOG输出、TX ring和统计 |
| `external/bk_avdk_smp/cp/components/bk_cli/shell_task.c` | CP_CLI/AP_CONSOLE输入owner状态机和退出转义 |
| 对应shell头文件/CLI注册文件 | `ap_console open/status/close`命令与bridge API |
| `external/bk_avdk_smp/cp/middleware/driver/mailbox/mb_uart_driver.c` | 指针/长度/清零/ACK后消费加固 |
| `external/bk_avdk_smp/cp/middleware/driver/mailbox/mailbox_channel.c`及头文件 | 发布peer reset/timeout/AP link事件，供bridge自动回退 |
| `external/bk_avdk_smp/cp/middleware/driver/mailbox/mbox0_adapter.c` | 以command/ACK专用稳定slot替换无所有权的双slot轮转，直到协议事件证明对端已复制后才复用 |
| CP CMake源列表 | 仅在新增独立bridge源时修改 |

比赛仓当前external镜像尚未包含完整CP源码树。实施前要把所有实际修改文件加入权威覆盖目录并更新`external/bk_avdk_smp/README.md`的同步表和命令，不能只保留一个`driver.c`覆盖而遗漏shell/mb_uart改动。

## 11. 分阶段实施

### 阶段A：协议和内存固化

1. 在AP代码中定义显式wire结构、mask和static assert。
2. 先在CP `mb_uart_driver.c`完成command/ACK全量清零、payload指针/长度校验和NULL检查；AP严格检查保留字段以此为前置条件。
3. linker加入SWAP，不清零、不进heap；构建map确认`0x2809f800..0x2809ffff`唯一。
4. 写主机侧单元测试验证channel编码、header pack/unpack、地址溢出和精确payload白名单。

出口：离线测试能精确生成与原厂一致的16字节DATA/STATE/ACK。

### 阶段B：transport可靠化

1. 修复MBOX0初始化盲目drain和错误状态。
2. AP实现active command、ACK稳定slot/queue、完整ACK关联和单向reset abort；CP adapter同步改为command/ACK专用稳定slot。
3. 先用HW_CTRL/PWC回归，不接console大流量。

出口：HW_CTRL power-up/heartbeat、PWC `0x5/0x11`都收到ACK；注入FIFO full/stale ACK不会永久busy。

### 阶段C：AP MB_UART全双工

1. 加入固定SWAP TX/RX、DATA/STATE和RTS/CTS。
2. 完成NuttX serial RX和`uart_recvchars()` worker。
3. 设备改名`ttyMB0`并移除UART1配置。

出口：用CP测试任务调用`bk_mb_uart_write(MB_UART0, "help\n", 5)`可驱动NSH，AP输出可返回CP。

### 阶段D：CP UART0桥

1. 扩展唯一MB_UART0 bridge和RAW/LOG输出。
2. 在CP shell现有输入owner中加入模式切换，不注册第二个UART ISR。
3. 加入延迟生效的owner切换、退出转义、transport/heartbeat事件驱动的link-down自动回退和统计命令。

出口：单根Type-C上可从CP CLI进入AP NSH、交互、退出并恢复CP CLI。

### 阶段E：恢复和压力

1. AP/CP reset、ACK timeout、FIFO full、RX overflow、RTS持续和高速粘贴故障注入。
2. heartbeat/PWC/日志/输入并发。
3. PM未完成则验证sleep vote确实阻止破坏link；完成后验证suspend/resume重握手。

出口：满足第12节全部验收标准后，才删除旧output-only兼容代码和UART1命名。

## 12. 测试和验收矩阵

### 12.1 构建和静态检查

- OpenVela AP `-Werror`构建通过；最终SMP `all-app.bin`完整构建通过。
- map中SWAP地址、大小、NOLOAD和AP heap边界正确；AP TX/RX指针分别固定为`0x2809fd00/0x2809fc00`。
- GPIO0/1没有UART1 pinmux，二进制无物理UART1 console初始化。
- CP覆盖副本与`bk_avdk_smp`目标文件`cmp -s`一致。
- 静态扫描不存在ISR内sleep、无界semaphore wait、printf或直接阻塞UART0输出。

### 12.2 功能

| 用例 | 预期 |
| --- | --- |
| CP冷启动 | CP log和CP prompt正常，默认不劫持UART0输入 |
| AP启动 | 出现带`ap0:`的OpenVela日志，heartbeat无超时 |
| `ap_console open` | 切入RAW，出现`nsh>`且prompt无50 ms按行前缀改写 |
| NSH输入 | 字母、回车、退格、方向键、Ctrl-C均按TTY语义工作 |
| 退出转义 | 不传给AP，恢复CP prompt和LOG模式 |
| AP长输出 | 大于128字节正确分片、顺序一致、无覆盖 |
| PC高速粘贴 | RTS发生且系统不死锁；超出物理能力时drop计数可见 |
| `poll/read` | `/dev/console`和`/dev/ttyMB0`的POLLIN、blocking/nonblocking语义正确 |

### 12.3 故障和并发

- 注入非法envelope地址、长度、SID、channel、payload地址、129字节长度和非零保留位，均拒绝且计数；有效command收到`COM_FAIL` ACK，不拖死CP physical channel。
- ACK FIFO满时不覆盖旧ACK；停止drain或受控恢复。
- 丢失DATA ACK后不原地改写in-flight slot，不出现无限重复字符。
- stale/wrong cmd/wrong sequence ACK不能释放当前事务。
- RX ring不足时整包拒绝，`uart_tx_fail=1`，不部分注入NSH。
- AP重启期间CP自动退出AP_CONSOLE；重启后旧输入和旧输出不重放。
- CP重启后AP进入DOWN并重新执行RESET abort和STATE/ACK探测，无永久`peer_rts=1`。
- 连续10 MiB AP输出、1 MiB双向随机字节和1小时交互期间，heartbeat、PWC和CP UART0不失效。
- UART压力下HW_CTRL/PWC优先于UART0，ready和heartbeat延迟有上界。

### 12.4 最低可观察结果

```text
CP boot...
ap0: OpenVela mailbox uart ready
cp> ap_console open
nsh> uname -a
...
nsh>                         # 行首 Ctrl-] .
AP console closed
cp>
```

统计命令至少输出：link/mode、peer/local RTS、TX/RX ring occupancy、active channel/seq/cmd、DATA/STATE计数、drop、overflow、remote fail、bad envelope/payload/ACK、timeout和reset次数。

## 13. 风险和明确决策

1. **唯一物理UART复用风险**：UART0同时服务下载、CP shell、CP log和AP console。通过运行期显式owner切换解决，不在bootloader阶段透明桥接。
2. **原厂协议无端到端可靠序号**：transport sequence只关联单次message，ACK丢失后无法判断对端是否已消费。正式策略优先避免重复输入，记录`tx_unknown`并reset，而不是盲目重发。
3. **物理UART无硬件流控**：Mailbox RTS只能限制CP到AP交换，不能让CH340停止已经到达CP UART FIFO的数据。高速粘贴仍需有界CP ring和可见drop。
4. **panic日志限制**：MB_UART依赖中断，不能冒充真正polling UART。验收只要求正常调度和可屏蔽短临界区下工作，不承诺hard-fault关中断后的完整日志。
5. **SMP并发**：当前AP为单核时可用local IRQ lock；正式启用CPU2前，transport和ring必须切换为NuttX SMP-safe spinlock并明确CPU1 owner或跨核代理。
6. **cache**：首版SWAP non-cacheable。任何启cache改动必须两端同时验证，不能只加一个DMB就宣称安全。
7. **CP改动维护成本**：完整输入owner必须修改shell路径，而不是只改`driver.c`。这些文件必须纳入external覆盖和构建说明，否则下一次清理构建会丢失功能。

## 14. 参考源码索引

- 总体要求：`docs/plans/BK7258_OPENVELA_AP_PORTING_PLAN.md:633-775,813-906`
- 原厂AP MB_UART：`bk_avdk_smp/ap/middleware/driver/mailbox/mb_uart_driver.c`
- 原厂CP MB_UART：`bk_avdk_smp/cp/middleware/driver/mailbox/mb_uart_driver.c`
- 公开API：`bk_avdk_smp/ap/include/driver/mb_uart_driver.h`
- logical channel：`bk_avdk_smp/ap/middleware/driver/mailbox/mailbox_channel.c`
- 通道定义：`bk_avdk_smp/ap/include/driver/mailbox_channel.h`和CP对应文件
- 交换区：`bk_avdk_smp/ap/middleware/driver/mailbox/mb_chnl_buff.c`
- V2 envelope：`bk_avdk_smp/ap/middleware/driver/mailbox/mbox0_adapter.c`
- RAM布局：`bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv`
- CP当前单向日志桥：`bk_avdk_smp/cp/middleware/driver/common/driver.c:42-165,452-464`
- CP UART shell owner：`bk_avdk_smp/cp/components/bk_cli/shell_uart.c:330-351`
- CP输入处理入口：`bk_avdk_smp/cp/components/bk_cli/shell_task.c:1177-1206`
- 本地AP不完整实现：`contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/chips/bk7258/bk7258_mailbox_channel.c`
- 本地NuttX serial：同目录`bk7258_serial.c`、`bk7258_syslog.c`、`bk7258_lowputc.c`
- 原厂接口说明：`博通网页文档/多核通讯-Uart通讯接口 — 博通集成 ARMINO SMP开发框架 文档.html`

> 文档状态：源码级完整实施方案，尚未代表实板验收完成。实现必须按阶段出口进行构建、故障注入和UART0实测后，才能将UART0完整收发标记为已移植。
