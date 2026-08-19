# BK7258 OpenVela SDIO、SD-NAND 与 FAT 移植适配计划

> 文档版本：V5
>
> 文档状态：2026-08-17 实施基线。1-bit PIO 读写、CMD24、MMCSD、FAT32、
> `/mnt/sdnand` 自动挂载和重启持久化已通过实板门禁；压力、断电、DMA、4-bit
> 和 CMD25 多块写仍未完成或未启用。后续 P0-P7 保留为阶段设计和未完成门禁。
>
> 适用范围：BK7258 AP/CPU1 上的 OpenVela/NuttX，通过 SDIO Host 访问板载
> SD-NAND，并以 NuttX 原生 FAT/VFAT 提供文件访问。

## 0. 使用规则

本计划必须在以下工作区根目录执行：

```text
/home/mi/vela_competition
```

正式代码仓库为：

```text
/home/mi/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
```

NuttX/OpenVela 源码为：

```text
/home/mi/vela_competition/openvela
```

原厂 BK7258 参考源码为：

```text
/home/mi/vela_competition/bk_avdk_smp
```

目标板原理图已经确认板载 SD-NAND 使用 GPIO14 至 GPIO19；因此该引脚组是本
目标板的正式 SDIO pin profile。`bk_solution_ai` 仍只能作为另一板型的辅助软件
参考，不能替代目标板原理图和实物连通性验证。原厂 `app_ab` 的 GPIO2 至 GPIO4
只作为另一种 BK7258 pinmux 参考。`vendor_beken` 的 BK7236N 代码不能作为
BK7258 SDIO 驱动或 FAT glue 的实现来源。

执行期间遵守以下安全规则：

1. 普通启动只挂载已有 FAT，禁止因挂载失败自动格式化。
2. 原始写测试和整盘 FAT32 重建必须通过 `--confirm` 显式授权。
3. 原厂 AP 和 OpenVela 不能同时访问同一个 SD-NAND。
4. CP 不能因为存在 dormant SDIO 源码而被认为拥有存储数据面。
5. 任何 SDIO clock request 失败时，禁止访问 SDIO MMIO 寄存器。
6. 不在 SDIO ISR 中执行 FAT 操作、PWC 请求、动态内存分配、长时间等待或逐字打印。
7. 所有生成配置、ELF、map 和测试日志必须记录构建目录、时间、源码状态和 hash。

## 1. 目标与当前状态

### 1.1 目标架构

```text
NSH / application
        |
        v
NuttX native FAT/VFAT (CONFIG_FS_FAT, mount type vfat)
        |
        v
MMCSD upper-half (/dev/mmcsd0)
        |
        v
NuttX struct sdio_dev_s lower-half
        |
        v
BK7258 SDIO V2 Host, IRQ-assisted PIO
        |
        v
板载 SD-NAND：SD protocol + internal FTL/ECC/bad-block management
```

CPU1/AP 是 SDIO Host、sector 和文件系统的数据面 owner。CPU0/CP 只提供已经
确认的 PWC/PM 时钟控制服务；CPU2 不访问 SDIO。当前不实现 USB MSC、CP proxy、
热插拔、自动格式化、CMD25 multiblock 和 4-bit PIO。

### 1.2 当前 contest 状态基线

截至 2026-08-17，当前实现状态如下：

| 项目 | 当前状态 | 证据 |
|---|---|---|
| BK7258 SDIO lower-half | 已实现 1-bit IRQ-assisted PIO RX/TX | CMD17/CMD24 与错误清理路径已接入 |
| MMCSD board bind | 已实现 | 延迟 5 秒初始化并注册 `/dev/mmcsd0` |
| MBR / whole-disk FAT | 已实现 | 有效 MBR 优先 `/dev/mmcsd0p0`，否则使用 `/dev/mmcsd0` |
| 持久挂载 | 已实现 | 已有 FAT 自动挂载 `/mnt/sdnand`，失败不自动格式化 |
| 显式 provisioning | 已实现 | `provision --confirm` 使用 1 sector/cluster 重建约 119 MiB FAT32 |
| 文件写与重启持久化 | 实板通过 | 0/512/4096-byte、rename/unlink、`fsync` 和 `VELA.TST` 重启读回 |
| SDIO DMA / 4-bit / CMD25 | 未启用 | 保持 1-bit、PIO、`CONFIG_MMCSD_MULTIBLOCK_LIMIT=1` |
| 压力与掉电 | 未完成 | 10,000 次随机写、24 小时、断电和高并发仍是后续门禁 |

### 1.3 工作区状态记录

在引用原厂配置和 ELF 之前执行：

```bash
cd /home/mi/vela_competition
git -C bk_avdk_smp status --short
git -C contest/contest2026_264_VelaSightsuixingAIzhinengyanjing status --short
git -C contest/contest2026_264_VelaSightsuixingAIzhinengyanjing log -1 --oneline
```

当前工作区的 `bk_avdk_smp` 和 contest 内层仓库是可直接检查的 Git 工作树；
`openvela` 由 contest 的 manifest 管理，不在本计划中单独执行 `git -C openvela`。
若原厂或目标仓库有修改，不得把当前产物称为“未修改原厂最终固件”。应记录：

```text
原厂/目标仓库 commit 或工作树状态
构建时间
config 文件 sha256
ELF/map/bin sha256
```

本计划的 SDIO 底层门禁历史上使用 `contest/cmake_out/bk7258-ap_nsh`；它是
最小启动/资源对照配置，不是 VelaSight 产品镜像。VelaSight 集成验证应使用
`contest/cmake_out/bk7258-ap_ai_agent`。旧的 `contest/out/beken_bk7258-ap_nsh`
不可用于判断 SDIO、FAT 或 work queue 状态。

## 2. 原厂基线与必须区分的事实

### 2.1 原厂 app_ab 的文件系统栈

当前生成的原厂 AP 配置显示：

```text
CONFIG_VFS=y
CONFIG_FATFS=y
CONFIG_FATFS_SDCARD=y
CONFIG_SDIO_HOST=y
CONFIG_SDCARD=y
CONFIG_SDIO_V2P0=y
CONFIG_SDIO_GDMA_EN=n
CONFIG_SDCARD_BUSWIDTH_4LINE=n
```

配置文件：

```text
bk_avdk_smp/projects/app_ab/ap/config/bk7258_ap/config
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258_ap/config/sdkconfig.h
```

原厂使用 Armino 的 FatFS 软件栈；介质上的实际格式必须通过 sector dump
确认，不能仅凭 `CONFIG_FATFS` 推断为某一种 FAT 版本。NuttX 迁移使用：

```text
CONFIG_FS_FAT=y
mount type = vfat
```

NuttX 原生 `fs/fat` 支持 FAT12、FAT16、FAT32 和 VFAT 长文件名功能。对于本
计划的约 1 GB SD-NAND，预期为 FAT32，但“FAT32”在 P0 阶段确认前只能标记为
目标假设，不能写入或格式化验证。

### 2.2 目标板原理图确认的 SD-NAND pinout

目标板原理图中，U1 为 `BK7258_QFN88_9X9`，U7 为板载 SD-NAND。U1 的
GPIO14 至 GPIO19 直接连接到 U7，完整连接关系如下：

| SDIO 信号 | BK7258 GPIO | U1 QFN88 脚号 | 原理图网络 | U7 SD-NAND 脚号 |
|---|---:|---:|---|---:|
| SD_CLK | GPIO14 / P14 | 54 | `SD_CLK` | 3 |
| SD_CMD | GPIO15 / P15 | 55 | `SD_CMD` | 5 |
| SD_D0 | GPIO16 / P16 | 56 | `SD_D0` | 6 |
| SD_D1 | GPIO17 / P17 | 57 | `SD_D1` | 7 |
| SD_D2 | GPIO18 / P18 | 58 | `SD_D2` | 1 |
| SD_D3 | GPIO19 / P19 | 59 | `SD_D3` | 2 |

U7 的其余硬件连接为：

```text
U7 pin 8 VCC -> R45 0R -> LDO_3V3，标称约 3.3 V
U7 pin 4 VSS -> GND
SD_CMD、SD_D0、SD_D1、SD_D2、SD_D3 -> 各 10K 上拉 -> NAND_VDD
```

原理图没有为该 SD-NAND 引出独立的 card-detect 或 write-protect 信号，因此
首版应使用固定介质模型：`SDIO_STATUS_PRESENT` 常量表示介质存在，并关闭
`CONFIG_MMCSD_HAVE_CARDDETECT` 和 `CONFIG_MMCSD_HAVE_WRITEPROTECT`。原理图
已经布出完整四线数据接口，但实施仍按 1-bit -> 4-bit 分阶段推进：首版只使用
`SD_CLK`、`SD_CMD`、`SD_D0`，稳定后才通过 SCR/ACMD6 和 `widebus()` 启用
`SD_D1` 至 `SD_D3`。

正式代码中的目标板 pin profile 必须固定为：

```text
SDIO CLK = GPIO14 / P14
SDIO CMD = GPIO15 / P15
SDIO D0  = GPIO16 / P16
SDIO D1  = GPIO17 / P17
SDIO D2  = GPIO18 / P18
SDIO D3  = GPIO19 / P19
```

软件接入前仍须在实板上验证 pin-1 方向、U7 电源电压、各网络连通性以及目标
封装/PCB 未发生改版。上述实测是对原理图装配和走线的验证，不再是 GPIO2-4
与 GPIO14-19 二选一的设计决策。

### 2.3 原厂 app_ab 的 pinmux 不是目标板 pinmux

原厂 `app_ab` AP 当前使用：

```text
GPIO2 -> SDIO CLK
GPIO3 -> SDIO CMD
GPIO4 -> SDIO DATA0
```

证据：

```text
bk_avdk_smp/projects/app_ab/ap/config/bk7258_ap/usr_gpio_cfg.h
```

同一文件中 GPIO14 至 GPIO19 是 LCD 功能，不是该 `app_ab` 的 SDIO。原厂
`app_ab` 的 `CONFIG_SDCARD_DEFAULT_CLOCK_FREQ=14` 表示 Host 时钟编码，原厂
HAL 将其解释为约 20 MHz；这只是 GPIO2 至 GPIO4 配置下的参考，不能替代目标
板波形测试。

`bk_solution_ai/projects/beken_genie` 另一板型也使用：

```text
GPIO14 -> SDIO CLK
GPIO15 -> SDIO CMD
GPIO16 -> SDIO DATA0
GPIO17 -> SDIO DATA1
GPIO18 -> SDIO DATA2
GPIO19 -> SDIO DATA3
```

这组配置与当前目标板原理图的 GPIO14 至 GPIO19 连接关系一致，但仍不能把该
另一板型的 `GPIO_DEFAULT_DEV_CONFIG` 或其他板级初始化代码直接复制到目标板。
目标板最终配置必须在唯一的 board pin profile 中实现，不能同时保留 GPIO2-4
和 GPIO14-19 两套 SDIO pinmux。

### 2.4 CP/AP 所有权

当前生成的 `app_ab` CP 配置没有发现以下数据面选项：

```text
CONFIG_SDIO_HOST
CONFIG_SDCARD
CONFIG_FATFS_SDCARD
CONFIG_USBD_MSC
```

CP 中允许出现通用 `SDIO_Handler` vector 符号，但不能出现 Host/card/FAT 实现。
每次 CP 构建后执行：

```bash
arm-none-eabi-nm <cp-elf> | \
  grep -E 'bk_sdio_host_|bk_sd_card_|sdio_host_isr|fatfs_adapter'
```

空结果才符合当前 CP owner 约束。CP/AP 的配置和 ELF 必须来自同一构建批次，
不得用旧 map 配新 bin。

CP 可能仍然负责 `CLK_PWR_ID_SDIO` 对应的中央 clock gate。该结论是原厂架构
参考，不代表当前 contest 已实现。当前 `bk7258_pm_pwc.c` 只有 PSRAM/boot-ready/
recovery 相关语义，因此 SDIO PWC command、module ID、参数和语义 ACK 必须单独
取证后实现。

## 3. 硬件、协议与不可变约束

### 3.1 介质假设

当前外部输入为一颗固定焊接的约 1 GB、8-pin SD-NAND。SD-NAND 内部完成 FTL、
ECC、坏块管理和磨损均衡，Host 只应看到标准 SD sector。OpenVela 不实现：

```text
MTD、raw NAND、page/OOB、软件 ECC、软件 FTL
```

待硬件确认：

```text
器件厂商/型号、VCC 电压、上电/掉电延时、pin table top/bottom view、容量
```

封装信号与 GPIO 的对应关系只有在原理图和实测确认后才生效。`DAT3` 是 SD
协议线，不把它当作固定焊接介质的独立机械 card-detect GPIO。

### 3.2 SDIO Host 基线

原厂 BK7258 AP 参考值如下，所有值必须在 P1 用逻辑分析仪和寄存器读回确认：

```text
secure SDIO base = 0x458d0000
TX FIFO          = base + 0x3c
RX FIFO          = base + 0x40
FIFO threshold   = base + 0x44
external IRQ     = 10
NuttX IRQ        = BK7258_IRQ_FIRST + 10 = 26
sector           = 512 bytes
```

参考文件：

```text
bk_avdk_smp/ap/include/soc/bk7258/reg_base.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sdio_reg.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sdio_struct.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/sdio_ll.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/icu_map.h
```

不要按旧注释猜 reset polarity、CRC shift、FIFO threshold 单位或 clock divider。
将这些行为封装在少量 helper 中，并用 golden trace 逐项确认。

### 3.3 首版功能边界

| 项目 | P0-P5 首版要求 |
|---|---|
| Host | SD Memory，`CONFIG_MMCSD_SDIO=y` |
| MMC/eMMC | 关闭 `CONFIG_MMCSD_MMCSUPPORT` |
| 总线 | 1-bit，稳定后可单独评估 4-bit |
| 数据方式 | IRQ-assisted PIO |
| DMA | `CONFIG_SDIO_DMA=n`，不返回 DMA capability |
| 时钟 | ID 约 100 kHz；transfer 先 6.5/13 MHz，后验 20 MHz |
| 请求 | 首版单 block，`CONFIG_MMCSD_MULTIBLOCK_LIMIT=1` |
| card detect | 固定 present，不启用 GPIO detect |
| write protect | 固定介质，不启用 GPIO write protect |
| 文件系统 | 先只读识别已有 FAT；随后受控读写 |
| 格式化 | P0-P5 禁止；当前仅 `provision --confirm` 可调用 `mkfatfs` |
| USB MSC | 关闭 |

4-bit PIO、multiblock、自动掉电、热插拔和 USB MSC 必须在基础单块读写通过后
分别提交和验证，不能在同一阶段叠加变量。

## 4. NuttX 接口基线

### 4.1 初始化和设备节点

lower-half 导出：

```c
struct sdio_dev_s *bk7258_sdio_initialize(int slot);
```

board 层调用：

```c
struct sdio_dev_s *dev;
int ret;

dev = bk7258_sdio_initialize(0);
if (dev == NULL)
  {
    return -ENODEV;
  }

ret = mmcsd_slotinitialize(0, dev);
```

`mmcsd_slotinitialize()` 要求 Host 已初始化并可用。它会尝试 MMC/SD 探测；
探测成功后通常注册 `/dev/mmcsd0`，而不是只要函数返回就保证 block node 存在。
固定介质的 board bring-up 必须同时记录返回值和节点是否存在。

NuttX 接口依据：

```text
openvela/nuttx/include/nuttx/sdio.h
openvela/nuttx/include/nuttx/mmcsd.h
openvela/nuttx/drivers/mmcsd/mmcsd_sdio.c
```

### 4.2 必须实现的 vtable

`struct sdio_dev_s` 当前接口要求实现或明确提供以下函数：

```text
lock
reset
capabilities
status
widebus
clock
attach
sendcmd
blocksetup
recvsetup
sendsetup
cancel
waitresponse
recv_r1 / recv_r2 / recv_r3 / recv_r4 / recv_r5 / recv_r6 / recv_r7
waitenable
eventwait
callbackenable
registercallback
```

`registercallback` 的函数类型是 NuttX 的 `worker_t`，不是自定义的
`sdio_event_callback_t`。它保存 callback 和 arg；callback 不得从 ISR 直接调用。
`callbackenable` 对固定介质可实现为状态记录/no-op，但必须符合 NuttX 调用契约。
`CONFIG_SCHED_HPWORK=y` 必须保留，因为 MMCSD 初始化会注册 media callback。

不要实现或加入以下伪接口：

```text
dmarecvsetup
dmasendsetup
SDIO_CAPS_DMASUPPORTED
```

### 4.3 command 和 data 时序

`sendcmd()` 接收 NuttX decorated command，必须解析：

```text
MMCSD_CMDIDX_MASK
MMCSD_RESPONSE_MASK
MMCSD_DATAXFR_MASK
MMCSD_MULTIBLOCK
MMCSD_STOPXFR
MMCSD_OPENDRAIN
```

上层负责卡协议状态机，lower-half 只负责硬件命令、response 和数据搬运。P2
按以下顺序验收：

```text
CMD0
CMD8 -> R7 = 0x1aa
CMD55 + ACMD41 -> OCR ready
CMD2 -> CID
CMD3 -> RCA
CMD9 -> CSD
CMD7 -> select
ACMD51 -> SCR，8-byte RX
```

SCR、单块读写的 `recvsetup/sendsetup` 与 `waitenable` 顺序不能硬编码为一条
路径。实现必须逐项对照当前 `mmcsd_sdio.c` 的真实调用顺序，特别是：

```text
SCR：blocksetup -> recvsetup -> waitenable -> command -> eventwait
读： blocksetup -> waitenable -> recvsetup -> CMD17 -> eventwait
写： CMD24/R1 -> blocksetup -> waitenable -> sendsetup -> eventwait
```

`eventwait()` 必须能消费 ISR 已经产生的 event；不能因 event 在 wait 前到达而
再次睡眠。`waitenable()` 只能清理 stale event/status，不能破坏已经配置的
transfer。

### 4.4 IRQ-assisted PIO

RX ISR：

1. 读取 enabled pending status。
2. timeout、CRC fail、overflow 立即停止数据通路并唤醒 waiter。
3. `RX_NEED_READ` 或 `RX_END` 时按固定 word budget 从 FIFO 搬运。
4. 最后 drain 后，只有 `remaining == 0` 且 terminal status 合法才成功。
5. 用明确 W1C mask 清已处理状态。

TX ISR：

1. `sendsetup()` 先以 TX ready prime FIFO。
2. `TX_NEED_WRITE` 时按固定 word budget 填 FIFO。
3. buffer 用尽后关闭 watermark IRQ，但保留 write-end/error IRQ。
4. 只有 `DATA_WR_END` 且 card write-response status 为 accepted 才成功。

RX/TX 都必须支持任意 buffer alignment，禁止：

```c
*(uint32_t *)buffer
```

使用 `uint8_t *` 和显式 little-endian word pack/unpack。尾部不足 4 字节时只复制
有效字节。ISR 不得无界清空 FIFO；初始 budget 可用 16 或 32 words，依据实测
FIFO depth、watermark 重触发和最坏 IRQ latency 调整。

### 4.5 错误、取消和恢复

以下错误必须返回明确 errno，并记录诊断快照：

```text
command timeout -> -ETIMEDOUT
CRC/index error -> -EIO
RX overflow    -> -EIO
TX underflow   -> -EIO
cancel         -> 唤醒 waiter，清理 transfer 状态
```

恢复顺序：

1. mask watermark 和 terminal IRQ。
2. 停止 data engine/SDCLK。
3. 保存 command、status、remaining 和 error。
4. 清明确的 W1C 状态。
5. 复位对应 FIFO/data/command 状态。
6. 清 software transfer state 和 stale semaphore。
7. 唤醒 waiter。
8. 后续 multiblock 才允许按安全状态发送 CMD12。
9. 连续失败时 reset controller 并重新枚举。

首版必须保留计数器：

```text
cmd_count, cmd_timeout, cmd_crc_error
rx_words, tx_words, rx_blocks, tx_blocks
data_timeout, data_crc_error, rx_overflow, tx_underflow
cancel_count, reset_count
last_cmd, last_status, last_error, last_remaining
```

## 5. 代码落点与实施顺序

### 5.1 目标文件

在正式 contest 仓库内新增或修改：

```text
board/beken/chips/bk7258/Kconfig
board/beken/chips/bk7258/CMakeLists.txt
board/beken/chips/bk7258/Make.defs
board/beken/chips/bk7258/include/irq.h
board/beken/chips/bk7258/bk7258_sdio.c
board/beken/chips/bk7258/bk7258_sdio.h
board/beken/chips/bk7258/hardware/bk7258_sdio.h
board/beken/chips/bk7258/hardware/bk7258_memorymap.h
board/beken/boards/bk7258/bk7258-ap/src/CMakeLists.txt
board/beken/boards/bk7258/bk7258-ap/src/bk7258_mmcsd.c
board/beken/boards/bk7258/bk7258-ap/src/bk7258_bringup.c
board/beken/boards/bk7258/bk7258-ap/configs/nsh/defconfig
```

chip 层负责寄存器、IRQ、clock、command、FIFO、PIO 和 `sdio_dev_s`；board 层
负责 pinmux、介质电源、`mmcsd_slotinitialize()`、分区注册和 mount；PWC 层只
负责 CP-compatible clock request 与完成等待。

### 5.2 P0：冻结输入和原厂 golden

执行步骤：

1. 记录两个仓库工作树状态和 commit/hash。
2. 确认器件型号、VCC、pin-1 方向、SD-NAND 与 BK7258 的实际连线。
3. 按目标板原理图固定使用 GPIO14-19，实测确认 U1 pin 54-59 到 U7 对应脚的
   连通性，并确认 GPIO14-19 没有被其他 board pin profile 同时复用。
4. 用原厂 AP 在不修改介质的情况下读取 OCR、CID、CSD、SCR、RCA 和 geometry。
5. 读取 sector 0 和 LBA1，并保存为只读证据。
6. 备份现有目录、文件长度和 hash。
7. 记录 ID/transfer SDCLK 波形和复位/上电时间。

host 侧只读备份示例，设备名必须由实际探测结果替换：

```bash
mkdir -p /home/mi/vela_competition/artifacts/sdio-golden
dd if=/dev/<original-block-device> \
   of=/home/mi/vela_competition/artifacts/sdio-golden/sector0-lba1.bin \
   bs=512 count=2 iflag=direct status=progress
sha256sum /home/mi/vela_competition/artifacts/sdio-golden/sector0-lba1.bin
xxd -g1 -l 1024 /home/mi/vela_competition/artifacts/sdio-golden/sector0-lba1.bin
```

sector 0/LBA1 解析规则：

| 识别结果 | 后续设备 |
|---|---|
| sector 0 直接是 FAT boot sector | `/dev/mmcsd0` |
| MBR + FAT 分区 | 自定义注册 `/dev/mmcsd0p0` 等 |
| protective MBR + LBA1 `EFI PART` | 启用 GPT parser 后注册产品定义路径 |
| 无合法结构 | 停止，不写入，不格式化 |

P0 门禁：硬件 pin、VCC、容量、SD response、sector layout 和 owner 全部有证据。

### 5.3 P1：PWC clock request 和 Host MMIO

先不要写 lower-half。根据 CP handler 和 mailbox 抓包确认：

```text
SDIO clock/power command ID
SDIO module ID
ON/OFF 参数编码
transport ACK
semantic ACK/result/version
timeout 和拒绝行为
```

当前 AP 只有 `bk7258_pwc_start()` 和 PSRAM 语义；不要假设可直接调用
`bk7258_pwc_clock_request()`。新增接口应满足：

```text
bk7258_pwc_clock_request(CLK_PWR_ID_SDIO, ON, timeout)
    -> serialize PWC request
    -> wait transport ACK
    -> wait semantic ACK or validated gate readback
    -> success only then access 0x458d0000
```

PWC 请求只能在 task context，不能从 SDIO ISR 发起。失败必须使初始化返回错误，
不得用固定 delay 代替 ACK/readback。P1 门禁：100 次冷启动均能得到确定的成功
或有限超时，且 clock request 失败时无 SDIO MMIO 访问。

### 5.4 P2：IRQ、pinmux、reset 和 command-only

新增 IRQ 定义：

```c
#define BK7258_EXTIRQ_SDIO 10
#define BK7258_IRQ_SDIO    (BK7258_IRQ_FIRST + BK7258_EXTIRQ_SDIO)
```

同时实现并验证：

```text
CPU1 route bit 10
NVIC IRQ 26
irq_attach(BK7258_IRQ_SDIO, ...)
up_enable_irq(BK7258_IRQ_SDIO)
```

先实现 `reset`、`clock`、`attach`、`sendcmd`、`waitresponse` 和 response readers，
不注册 FAT、不启用 data path。按 CMD0、CMD8、ACMD41、CMD2、CMD3、CMD9、CMD7
顺序验证。P2 门禁：CID/CSD/OCR 与 golden 一致；无卡或断电时有限 timeout，
不得 hard fault 或无限等待。

### 5.5 P3：PIO RX、MMCSD bind 和只读 block 验证

实现 `blocksetup`、`recvsetup`、`waitenable`、`eventwait`、`cancel`，先支持：

```text
8-byte SCR
512-byte CMD17
任意 alignment 的 destination
```

然后 board 层执行：

```c
dev = bk7258_sdio_initialize(0);
ret = mmcsd_slotinitialize(0, dev);
```

固定介质的 `status()` 返回 `SDIO_STATUS_PRESENT`。`registercallback()` 保存
NuttX `worker_t` 和 arg，`callbackenable()` 不伪造 card removal。成功探测后
检查 `/dev/mmcsd0` 是否出现；不要把 `mmcsd_slotinitialize()` 返回成功等同于
block node 已注册。

P3 门禁：能够读 geometry、sector 0 和测试 LBA；sector hash 与原厂一致；连续
和随机只读无 CRC、timeout、overflow；错误 cancel 后可以继续发 command。

### 5.6 P4：PIO TX 和受控写验证

只有完成备份并确认专用测试 LBA 后才实现 write。首版只做 CMD24 单块写：

```text
清 stale status
配置 block size/data timer
TX ready prime FIFO
sendsetup
enable TX watermark/write-end/error IRQ
等待 DATA_WR_END 和 accepted write response
```

使用 zero、ones、walking bit、PRNG 和 unaligned source 做写回读验证。生产
FAT 卷不能执行 `cmocka_driver_block` 或破坏性 block test。P4 门禁：至少 10,000
次随机单块读写无 silent corruption，错误后 reset/cancel 可恢复。

### 5.7 P5：FAT/VFAT 只读挂载

先根据 P0 布局选择 whole-disk 或自定义 partition device。superfloppy 可直接
挂载 `/dev/mmcsd0`；MBR/GPT 必须先启用对应 parser 并通过 `parse_block_partition()`
调用 `register_blockpartition()` 注册路径。NuttX 不会自动产生 `/dev/mmcsd0p0`。

NuttX 配置：

```text
CONFIG_MMCSD=y
CONFIG_MMCSD_SDIO=y
CONFIG_MMCSD_NSLOTS=1
CONFIG_MMCSD_MULTIBLOCK_LIMIT=1
# CONFIG_MMCSD_SPI is not set
# CONFIG_MMCSD_MMCSUPPORT is not set
# CONFIG_MMCSD_HAVE_CARDDETECT is not set
# CONFIG_MMCSD_HAVE_WRITEPROTECT is not set
CONFIG_SDIO_BLOCKSETUP=y
# CONFIG_SDIO_DMA is not set
CONFIG_FS_FAT=y
# CONFIG_FSUTILS_MKFATFS is not set
CONFIG_SCHED_HPWORK=y
```

按实际布局追加且只追加需要的配置：

```text
CONFIG_MBR_PARTITION=y
```

或：

```text
CONFIG_GPT_PARTITION=y
```

挂载命令：

```text
mount -t vfat /dev/mmcsd0 /mnt/sdnand
```

有分区时将 source 替换为实际注册的 `/dev/mmcsd0p0`。首次只执行 `mount`、
`ls`、文件长度和 hash 对比，不执行 `mkfatfs`、`fsck` 自动修复或写操作。

P5 门禁：原厂目录和文件 hash 一致，挂载/卸载 100 次稳定，原有 FAT 数据无损。

### 5.8 P6：受控文件写和掉电

按以下顺序测试：

```text
create -> write -> fsync -> read -> rename -> unlink
```

覆盖 0-byte、小文件、大文件、空间耗尽、重复 mount/unmount 和并发日志/Wi-Fi/
BT 负载。正常掉电前执行：

```text
阻止新 writer
fsync 所有文件
sync filesystem
unmount
等待 active transfer 完成
CMD13 确认 card 不 busy
遵守器件 datasheet 的 power-off delay
```

没有器件型号和 datasheet 前，不声明 write cache 的掉电安全性。至少执行 idle、
write 中、fsync 后、unmount 后四类受控断电测试。

### 5.9 P7：multiblock 和 4-bit PIO（可选）

multiblock 先单独解除 `CONFIG_MMCSD_MULTIBLOCK_LIMIT=1`，验证 CMD18/CMD25/
CMD12、2/4/8/32/128 sectors 和错误恢复。通过后再评估 4-bit：

1. 1-bit 6.5/13 MHz 长稳。
2. 1-bit 20 MHz 长稳。
3. watermark IRQ 压力测试。
4. `capabilities()` 从 `SDIO_CAPS_1BIT_ONLY` 改为 `SDIO_CAPS_4BIT`。
5. 由 MMCSD 上层读取 SCR、发送 ACMD6，lower-half 的 `widebus()` 只改 Host width。
6. 4-bit 先低速，最后评估 20 MHz。

若 4-bit PIO 在高负载下失败，交付可靠的 1-bit，不引入未适配 SDIO DMA。

## 6. 配置与构建执行清单

### 6.1 配置修改原则

当前 `defconfig` 顶部注明由 menuconfig 生成。修改配置时以该文件所属配置为
输入，通过 menuconfig 或 `savedefconfig` 更新，不手工维护生成的 `.config`。
当前显式 provisioning 命令需要 `CONFIG_FSUTILS_MKFATFS=y`。保留已有的
`CONFIG_FS_FAT=y` 并启用 MMCSD/SDIO；`CONFIG_DMA` 是否因其他外设需要保持开启，
不影响本计划，但 `CONFIG_SDIO_DMA` 必须关闭。

当前交付配置：

```text
CONFIG_BK7258_SDIO=y
CONFIG_MMCSD=y
CONFIG_MMCSD_SDIO=y
CONFIG_MMCSD_NSLOTS=1
CONFIG_MMCSD_MULTIBLOCK_LIMIT=1
CONFIG_SDIO_BLOCKSETUP=y
CONFIG_SCHED_HPWORK=y
CONFIG_FS_FAT=y
CONFIG_FSUTILS_MKFATFS=y
CONFIG_BK7258_SDNAND_AUTOMOUNT=y
CONFIG_BK7258_SDNAND_MOUNTPOINT="/mnt/sdnand"
# CONFIG_SDIO_DMA is not set
# CONFIG_MMCSD_MMCSUPPORT is not set
# CONFIG_MMCSD_SPI is not set
# CONFIG_MMCSD_HAVE_CARDDETECT is not set
# CONFIG_MMCSD_HAVE_WRITEPROTECT is not set
```

`ARCH_HAVE_SDIO` 是 capability symbol，不应作为手工 defconfig 输入。若本树的
Kconfig 由 `BK7258_SDIO` 自动 select，则以最终 `.config` 为准。`BK7258_SDIO`
应 `select ARCH_HAVE_SDIO`，而不是把 capability 伪造为运行时功能。

### 6.2 源码加入

实现完成后，把 chip lower-half 加入 chip `CMakeLists.txt` 和 `Make.defs`，把
board bind/mount 加入 board `src/CMakeLists.txt`。在 P2 前不要加入 MMCSD bind，
避免 command-only 未完成时自动访问介质。

推荐 board 调用位置：

```text
mailbox link ready
PWC worker ready
CPU1 boot-ready ACK
SDIO clock request 成功
目标 pinmux/VCC ready
bk7258_sdio_initialize(0)
mmcsd_slotinitialize(0, dev)
只读 mount（由配置或显式 NSH 命令控制）
```

SDIO 初始化失败只应打印错误并保留 NSH/日志，不应注册虚假的 `/dev/mmcsd0`，
也不应使整个 `board_late_initialize()` 因可选存储失败而退出。

### 6.3 唯一构建目录和命令

进入正式仓库：

```bash
cd /home/mi/vela_competition/contest
```

首次构建或修改 Kconfig/defconfig 后，先清理 CMake 配置：

```bash
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  --cmake distclean
```

然后使用仓库当前已经验证的构建命令：

```bash
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  -e -Werror \
  --cmake \
  -j8
```

若 `vendor/beken` 在当前 checkout 中不是有效路径，改用同一目标的实际
`board/beken` 路径，并记录实际命令；不要切换到旧的 `out/` 目录。核心要求是
目标固定为 `bk7258-ap_nsh`，构建输出固定为：

```text
cmake_out/bk7258-ap_nsh/.config
cmake_out/bk7258-ap_nsh/nuttx
cmake_out/bk7258-ap_nsh/nuttx.bin
cmake_out/bk7258-ap_nsh/System.map
```

构建后执行：

```bash
test -f cmake_out/bk7258-ap_nsh/.config
test -f cmake_out/bk7258-ap_nsh/nuttx
test -f cmake_out/bk7258-ap_nsh/nuttx.bin
grep -E 'CONFIG_(BK7258_SDIO|MMCSD|MMCSD_SDIO|FS_FAT|SDIO_BLOCKSETUP|SDIO_DMA|FSUTILS_MKFATFS)=' \
  cmake_out/bk7258-ap_nsh/.config
arm-none-eabi-nm cmake_out/bk7258-ap_nsh/nuttx | \
  grep -E 'bk7258_sdio|mmcsd_slotinitialize|SDIO_Handler|BK7258_IRQ_SDIO'
sha256sum cmake_out/bk7258-ap_nsh/nuttx \
          cmake_out/bk7258-ap_nsh/nuttx.bin \
          cmake_out/bk7258-ap_nsh/System.map
```

验证项必须包括：

```text
CONFIG_MMCSD=y
CONFIG_MMCSD_SDIO=y
CONFIG_FS_FAT=y
CONFIG_SDIO_BLOCKSETUP=y
CONFIG_SDIO_DMA 未设置
CONFIG_FSUTILS_MKFATFS=y
CONFIG_BK7258_SDNAND_AUTOMOUNT=y
SDIO lower-half 符号存在
无 Armino SDIO/FAT glue 链接
```

## 7. 分区、文件系统和数据安全

### 7.1 块设备与容量

Host 只接受 512-byte sector。容量必须来自 CSD 和 NuttX geometry，不得硬编码
“1 GB = 固定 sector 数”：

```text
sector size = 512
capacity = nsectors * 512
```

商业 1 GB 介质可能约 1,953,125 sectors，也可能接近 2,097,152 sectors；以实测
geometry 和原厂 golden 为准。

### 7.2 分区注册

NuttX 的 `parse_block_partition()` 只有在 `CONFIG_MBR_PARTITION`、
`CONFIG_GPT_PARTITION` 或其他对应 parser 打开时才有实际解析器。MBR/GPT parser
不会自动为 MMCSD 生成 `/dev/mmcsd0p0`。需要产品路径时参考：

```text
openvela/nuttx/boards/risc-v/mpfs/common/src/mpfs_emmcsd.c
openvela/nuttx/include/nuttx/fs/partition.h
openvela/nuttx/include/nuttx/fs/fs.h
```

partition handler 使用 `register_blockpartition()`，将 `firstblock` 和 `nblocks`
绑定到 `/dev/mmcsd0`。GPT 只在 P0 确认 LBA1 为 `EFI PART` 后启用；不要为了“兼容
所有格式”无条件自动选择或修改分区。

### 7.3 NuttX FAT 与原厂 FatFS 的边界

```text
原厂：CONFIG_FATFS + FATFS_SDCARD + Armino adapter
NuttX：CONFIG_FS_FAT + vfat + mmcsd block device
```

不使用：

```text
CONFIG_FATFS
CONFIG_FATFS_SDCARD
bk_fatfs_partition
FATFS_DEV_SDCARD
Armino sd_card_driver.c 作为 NuttX block device
```

NuttX `fs/fat` 会识别 FAT12/16/32 的 boot sector；是否为 FAT32、是否有 MBR/GPT
分区，以介质 dump 为准。FAT LFN、UTF-8、时间戳等选项根据产品需求启用，首轮
只保留现有文件可正确读取所需的最小配置。

### 7.4 owner 状态机

当前阶段只允许：

```text
CP SDIO/SDCARD/FATFS disabled
USB MSC disabled
OpenVela owns Host and mounted filesystem
```

未来转交 USB MSC 前必须：

```text
stop new writers
fsync files
sync filesystem
unmount
wait active transfer complete
确认 card not busy
停止 OpenVela block access
再 expose USB MSC
```

返回 OpenVela 前执行 USB eject/disconnect、media reset/re-enumeration 和 mount。
单个固件地址空间内的 owner variable 不能作为 CP/AP 跨核锁。

## 8. 验证矩阵

### 8.1 构建和静态检查

```text
-Werror 构建通过
目标只使用 cmake_out/bk7258-ap_nsh
CP 配置无 SDIO_HOST/SDCARD/FATFS_SDCARD/USBD_MSC
CP ELF 无 Host/card/FAT 实现符号
OpenVela 不链接 Armino SDIO/FAT glue
SDIO_DMA=n
不返回 SDIO_CAPS_DMASUPPORTED
FSUTILS_MKFATFS=y，仅由显式 provision --confirm 使用
MMCSD_READONLY/BCH_DEVICE_READONLY 未设置
```

### 8.2 枚举

```text
冷启动 100 次：Host init 成功或有限失败，不死锁
CMD0/CMD8/ACMD41/CID/CSD/RCA/SCR 与 golden 一致
无介质/VCC off：有限 timeout，无 hard fault
clock request fail：不访问 SDIO MMIO
geometry：512-byte sector，容量等于实测值
sector 0：与 golden 完全一致
```

### 8.3 PIO 数据

```text
SCR 8 bytes
single read/write：first/middle/last LBA
alignment：offset 0/1/2/3/4/8/16/32
随机单块读写：至少 10,000 次
错误：command/data timeout、CRC、overflow、underflow、cancel
压力：Wi-Fi、BT、日志、PWM 并发
```

### 8.4 FAT 与长稳

```text
只读 mount/list/hash
create/write/fsync/read/rename/unlink
0-byte、小文件、大文件、空间耗尽
mount/unmount 1000 次
24 小时循环 IO 和 hash
100 次冷启动
idle、write 中、fsync 后、unmount 后受控断电
```

`cmocka_driver_block` 破坏性测试只能在备份后的专用测试介质/分区执行，不能在
生产 FAT 卷上执行。

### 8.5 性能记录

在正确性门禁通过后记录：

```text
1-bit 6.5/13/20 MHz throughput
4 KiB random IOPS
IRQ rate、最大 ISR 搬运量、最坏 ISR latency
CPU utilization
高负载下 CRC/overflow/underflow 计数
```

无 DMA 前提下，不以性能优化掩盖 PIO 数据损坏或错误恢复问题。

## 9. 交付物与回滚

每个阶段提交必须包含：

```text
源代码和 Kconfig/defconfig
构建命令和最终 .config
nuttx、nuttx.bin、System.map hash
原厂 golden 证据和硬件波形
测试命令、日志、统计计数器
失败场景和恢复结果
```

推荐拆分提交：

```text
docs: refine BK7258 SDIO porting plan
fix: add BK7258 SDIO PWC clock handshake
feat: add BK7258 SDIO command lower-half
feat: add BK7258 SDIO IRQ PIO receive path
feat: add BK7258 SDIO IRQ PIO transmit path
feat: bind BK7258 SD-NAND to NuttX MMCSD
feat: mount BK7258 SD-NAND FAT volume
feat: enable BK7258 SDIO multiblock PIO
feat: enable BK7258 SDIO four-bit PIO
```

任何阶段出现 sector hash 不一致、CRC 错误无法解释、FIFO overflow/underflow、
PWC ACK 不确定、owner 不明确或掉电数据损坏，立即回退到最近一个只读通过的
阶段。不得通过自动格式化、升频、4-bit 或 DMA 规避问题。

## 10. 完成标准

只有同时满足以下条件才称为“SDIO/FAT 适配完成”：

1. 目标 pinmux、pin-1、VCC 和上电时序有原理图与实测闭环。
2. 原厂 CP 不拥有 SDIO/FAT/MSC 数据面，且有每次构建的符号检查。
3. CPU1/OpenVela 是唯一 Host、sector 和文件系统 owner。
4. PWC SDIO clock request 有 transport/semantic ACK 或可靠 readback 闭环。
5. secure base、IRQ26、W1C、FIFO 和 reset 序列经过实板验证。
6. lower-half 使用 NuttX `struct sdio_dev_s`，不混入 Armino RTOS/FatFS glue。
7. SDIO DMA 未适配、未上报、无伪实现。
8. `/dev/mmcsd0` 稳定枚举，分区路径由实际 MBR/GPT 解析结果注册。
9. SCR、sector、unaligned buffer、timeout、cancel 和恢复测试通过。
10. 单块和多块测试无 silent corruption；多块只有单独阶段通过后才启用。
11. 现有 FAT 卷以 NuttX `vfat` 无损挂载，不自动格式化。
12. 24 小时、100 次冷启动、1000 次 mount/unmount 和受控掉电达到门槛。
13. 4-bit 若交付，必须在无 SDIO DMA 和高系统负载下通过长稳；否则交付 1-bit。
14. USB MSC、CP dormant driver 和 CPU2 不会并发访问介质。

## 11. 稳定引用

NuttX 文件系统和 MMCSD 参考：

```text
openvela/nuttx/Documentation/components/filesystem/fat.rst
openvela/nuttx/fs/fat/Kconfig
openvela/nuttx/fs/mount/fs_mount.c
openvela/nuttx/include/nuttx/sdio.h
openvela/nuttx/include/nuttx/mmcsd.h
openvela/nuttx/drivers/mmcsd/mmcsd_sdio.c
openvela/nuttx/fs/partition/Kconfig
openvela/nuttx/fs/partition/fs_partition.c
openvela/nuttx/boards/risc-v/mpfs/common/src/mpfs_emmcsd.c
```

BK7258 原厂参考：

```text
bk_avdk_smp/projects/app_ab/ap/config/bk7258_ap/config
bk_avdk_smp/projects/app_ab/ap/config/bk7258_ap/usr_gpio_cfg.h
bk_avdk_smp/ap/include/soc/bk7258/reg_base.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sdio_reg.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sdio_struct.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/sdio_ll.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/icu_map.h
bk_avdk_smp/ap/middleware/driver/sdio_host/sdio_host_driver.c
bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.c
```

目标 OpenVela 代码：

```text
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/chips/bk7258
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/boards/bk7258/bk7258-ap
```

行号不是稳定接口；后续引用优先使用函数名、宏名和文件路径，并在每次原厂
SDK 更新后重新执行 P0 取证。

---

## 12. 2026-08-14 实板调试记录与当前结论

本节记录本轮及前序 BK7258 SD-NAND/OpenVela SDIO 调试的实际结果。记录以
`/home/mi/vela_competition` 工作区、目标板 `/dev/ttyUSB1` 和当前比赛仓库为准。

### 12.1 已完成的移植内容

- 确认原理图 pinout：GPIO14=`SD_CLK`、GPIO15=`SD_CMD`、GPIO16~19=`SD_D0~D3`。
- 新增 BK7258 SDIO lower-half、硬件寄存器头文件和 SDIO PWC 电源/时钟请求。
- 接入 `CONFIG_BK7258_SDIO`、CMake、Kconfig、IRQ、bring-up 和 MMCSD board glue。
- 配置为只读、1-bit、PIO；未实现 DMA、写入、格式化或破坏性测试。
- 注册固定只读几何的 `/dev/mmcsd0`：sector size 为 512，sector count 为 2097152。
- `periph_selftest` 改为通过 block-driver geometry/read API 访问存储，避免 BCH 普通
  `open()` 临时代理的 `ENXIO` 干扰。
- 启用 BCH、MMCSD、MMCSD read-only、MBR parser、FAT/VFAT、单块限制和 1-bit 配置。
- GPIO14~19 按原厂配置内部上拉和 drive capacity 3。

### 12.2 已确认正常的部分

SD-NAND 初始化命令链路在多轮冷启动中稳定成功：

```text
CMD8       -> 0x000001aa
ACMD41     -> 0x80ff8000
CMD2       -> response available
CMD3       -> 0x21bb0500
CMD9       -> response available
CMD7       -> 0x00000700
CMD16      -> 0x00000900
ACMD51     -> data transfer completes for 8 bytes
```

已确认 OCR 的 CCS bit 为 0，因此该器件按 SDSC byte addressing 处理，直接读取时使用
`sector << 9`。SDIO 现代寄存器窗口也已与 BK7258 原厂 V2 资料核对：

```text
SDIO base       = 0x458d0000
RX FIFO         = 0x458d0040
FIFO threshold  = 0x458d0044
```

构建、打包和烧录流程稳定成功。最近一次结果：

```text
build.sh ... -e -Werror --cmake -j8  -> build completed successfully
EraseFlash                              -> pass
{All Finished Successfully}
all-app.bin                             -> 2646016 bytes
```

除存储外，最近一次 `periph_selftest` 通过了：

```text
PASS wlan0 interface present
PASS mailbox UART TX
PASS psram
PASS /proc/version
PASS /proc/uptime
PASS /proc/meminfo
PASS systick
```

### 12.3 原厂实现对照结果

本轮逐项对照了以下原厂实现：

```text
bk_avdk_smp/ap/middleware/driver/sdio_host/sdio_host_driver.c
bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.c
bk_avdk_smp/ap/middleware/driver/sdcard/sdcard.c
bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/sdio_ll.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sdio_struct.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sdio_reg.h
```

已在 OpenVela 路径中对齐或验证的项目包括：

- BK7258 V2 的 512-byte `DATA_CTRL` block-size 字段。
- `DATA_CTRL` 配置和 `DATA_EN` 启动分离写入。
- data timeout、W1C interrupt status、RX FIFO/state reset 顺序。
- RX threshold/TX threshold 为 `0x01`，clock recovery bit。
- CMD18 多块读、CMD12 stop、clock gate 和读后 state reset。
- FIFO 32-bit word 的低字节优先搬运。
- `DATA_RECEIVE_END` 后清 W1C 状态，再消费 RX FIFO。
- 100 kHz 和原厂 20 MHz clock code 的四位 divider 字段。
- 负沿/正沿 sampling 对照。
- 原厂 legacy block-size aperture `0x448b000c` 的兼容写入对照。

原厂正式 V2 host 使用 ISR/semaphore 完成 command/data 同步；当前 OpenVela 首版仍是
polling lower-half。这是当前最重要的结构性差异，不能用更多的固定 delay 或忽略 CRC
来替代。

### 12.4 已执行的实验及结果

#### CMD17 单块读

`CMD17` 能稳定得到前部 MBR 引导代码，例如：

```text
first = fa 33 c0 8e
```

但在完整 512-byte 请求中，后续 FIFO 内容主要为 `0xff`，sector 尾部没有 `55aa`。

#### CMD18 多块读 + CMD12

该组合可以搬运 512 字节，并且 CMD12 响应稳定：

```text
SDIO R1 CMD18=0x00000900
SDIO R1 CMD12=0x00000b00
```

但数据不是有效 sector。曾观测到：

```text
first     = fa33c08e
signature = offset 170 或 offset 208
last      != 55aa
```

完整 dump 中虽然首部存在合法 MBR 引导字节，但后续数据呈现错位、重复或非 sector
内容的特征，不能用于 FAT parser。

#### 时钟和采样

- 100 kHz：可以避免部分 FIFO overflow，但 sector 仍错位。
- 20 MHz：数据 CRC 错误仍存在，不能提升为可用状态。
- 正沿采样：与负沿结果没有实质改善，已恢复原厂负沿默认。
- 6.5 MHz/100 kHz/20 MHz 多轮测试均未得到 offset 510 的稳定 `55aa`。

#### FIFO pacing

曾测试 100 us、1 us、300 us 固定 delay 和不同 burst 大小。固定 delay 会改变观测到的
偏移，但不能使 sector 正确；这证明 delay 不是可靠修复。原厂路径是先等待 block event，
再通过有界 `RX_READY` polling 读取固定数量的 32-bit word，不应继续扩大固定 delay。

#### CRC 行为

早期诊断代码曾在以下条件下容忍 `DATA_CRC_FAIL`：无 timeout、无 overflow、
`remaining==0`。这会让驱动看似读满 512 字节，但 sector 内容明显损坏。本轮已撤销该
兼容放行，恢复原厂安全语义：

```text
DATA_CRC_FAIL -> error
DATA_TIMEOUT/FIFO_OVERFLOW -> error
DATA_RECEIVE_END + DATA_CRC_OK + complete FIFO block -> success
```

### 12.5 最新实板结果

最新日志：

```text
/tmp/bk7258_selftest_strict_final.log
```

关键输出：

```text
SDIO data setup block=512 count=1 bytes=512
SDIO R1 CMD18=0x00000900
SDIO data CRC error status=0x1004a4c0 remaining=512
SDIO R1 CMD12=0x00000b00
FAIL storage sector0 read=-5
SELFTEST FAIL failures=1
```

因此当前存储验证结论是：

```text
SDIO command path       PASS
SDIO data integrity     FAIL
sector 0 signature      FAIL
MBR/FAT/VFAT mount      NOT REACHED
storage usable          NO
```

不能因为 `/dev/mmcsd0` 已注册、CMD18 有 R1 响应或 buffer 被填充 512 字节，就宣称
SD-NAND 可用。当前自检仍正确地阻止 VFAT 挂载。

### 12.6 当前提交边界和剩余工作

本阶段只交付只读、PIO、1-bit 的诊断性 lower-half 和安全失败行为。完成标准第 8~12 项
仍未通过，尤其是 sector、timeout、恢复、无 silent corruption 和 VFAT 挂载。

下一步必须优先移植原厂 ISR/semaphore 数据面：

1. 注册并启用 `BK7258_IRQ_SDIO`。
2. 按原厂 `sdio_host_isr()` 保存 data interrupt status。
3. 用 RX semaphore 等待 receive block event。
4. 仅在有效 block event 后读取固定数量 FIFO word。
5. CRC fail 时执行原厂 state reset 和错误恢复。
6. 对 CMD18 的非连续读执行 busy/card-ready 检查和 stale receive 状态清理。
7. 重新验证 sector 0 的 offset 510/511 是否稳定为 `55aa`。
8. 只有 MBR、FAT/VFAT 和目录读取全部通过后，才能继续性能、长稳和 mount/unmount
   测试。

在上述门禁通过前，不得启用写入、格式化、4-bit、DMA、自动修复分区或忽略数据 CRC。

### 12.7 2026-08-14 实板闭环结果

本轮使用目标串口 `/dev/ttyUSB1` 完成了重新构建、Armino 打包、自动擦写、重启和
AP 控制台验证。最终烧录镜像为：

```text
projects/app_ab/build/bk7258/app_ab/package/all-app.bin
size: 2646016 bytes
```

本轮 OpenVela AP raw binary 三份文件逐字节一致：

```text
contest/cmake_out/bk7258-ap_nsh/nuttx.bin
bk_avdk_smp/build/openvela-ap.bin
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
sha256: 5e773bfefb2cd18cb6d887310bd494a069814ec764695c94f28cde65f0ad84a8
```

自动烧录结果：

```text
EraseFlash -> pass
WriteFlash -> pass
Boot_Reboot -> pass
All Finished Successfully
```

#### 数据面修复

在本轮之前，SDIO 命令枚举已经成功，但 CMD17 sector 数据因 FIFO 时序和 data
engine 配置错误而 CRC 失败。本轮完成以下修复：

- CMD2/CMD9 的 R2 长响应判定改为使用正确的 decorated-response 编码域，实板日志
  已出现完整四个 response words；
- 512-byte CMD17 使用 BK7258 V2 data engine 的 multi-block data mode。这里是
  控制器数据模式，不代表上层发送 CMD18；上层仍然使用单块 CMD17；
- receive engine 启动后增加原厂要求的约 2 ms settling interval，避免
  `DATA_RECEIVE_END` 丢失；
- 使用 IRQ26/SDIO semaphore 等待数据终止，在任务上下文按 `RX_READY` 有界读取 FIFO；
- 数据 CRC、timeout 和 FIFO 错误不再被兼容放行，CRC 失败仍返回 `-EIO`；
- 删除固定容量 CMD18/CMD12 fallback，恢复 NuttX MMCSD 原生 geometry、寻址和 CMD17
  读路径；
- SDIO lower-half 维持固定只读状态，并拒绝 decorated write command；
- NuttX FAT 实际构建树增加只读 block geometry 识别，允许只读介质挂载，并拒绝文件写、
  truncate、mkdir、rmdir、unlink、rename 和同步修改路径。

#### 实板日志证据

启动枚举和 sector 读取：

```text
SDIO R2 CMD2=25475345 41535443 38800f1e 009b0185
SDIO R2 CMD9=007f0032 535a803c 6ebbff9f 00168000
SDIO data setup block=512 count=1 bytes=512
SDIO R1 CMD17=0x00000900
SDIO data complete status=0x000454c8 event=0x4
```

`periph_selftest` 最终结果：

```text
PASS storage geometry sector=512 count=247808 signature=55aa
PASS storage vfat mount dev=/dev/mmcsd0p0 first=ASR_ST~1.WAV
SELFTEST PASS failures=0
```

这证明以下链路已经在目标板闭环：

```text
GPIO14..19 pinmux
  -> BK7258 SDIO command/data engine
  -> IRQ26 + PIO FIFO
  -> NuttX sdio_dev_s
  -> MMCSD native block device /dev/mmcsd0
  -> MBR partition /dev/mmcsd0p0
  -> NuttX vfat read-only mount
  -> directory read
```

#### 当前状态重新标注

```text
SDIO command enumeration       PASS
R2/CID/CSD response transport  PASS
CMD17 512-byte data integrity  PASS
sector 0 MBR signature          PASS
MMCSD geometry                 PASS (247808 sectors)
MBR partition registration      PASS (/dev/mmcsd0p0)
VFAT read-only mount            PASS
directory read                 PASS
read-only write rejection       STATICALLY IMPLEMENTED; board write test intentionally not run
multiblock CMD18/CMD25         NOT IMPLEMENTED / NOT ENABLED
4-bit                          NOT ENABLED
SDIO DMA                       NOT IMPLEMENTED / NOT ADVERTISED
long-run, cold-start, power-loss NOT YET CLOSED
```

本轮因此可以将 P0-P5 的基础只读识别、单块读取和 VFAT 只读挂载标记为实板通过，
但不能把整个 SDIO/FAT 适配标记为量产完成。后续仍需在专用备份介质和明确 owner
批准后验证受控写入、掉电恢复、多块传输、4-bit、长期压力和 100 次冷启动。
