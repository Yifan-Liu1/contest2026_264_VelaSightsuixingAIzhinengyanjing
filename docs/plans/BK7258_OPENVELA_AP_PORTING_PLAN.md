## BK7258 移植开发目录和原则

BK7258 移植代码维护在比赛仓库中，并通过OpenVela工作树的vendor符号链接参与构建：

```text
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
├── boards/bk7258/bk7258-ap/
└── chips/bk7258/

contest/vendor/beken/chips/bk7258
  -> ../../../contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/chips/bk7258
contest/vendor/beken/boards/bk7258/bk7258-ap
  -> ../../../../contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/boards/bk7258/bk7258-ap
```

目录格式必须遵守 `contest/docs/zh-cn/chip_porting/Vendor.md`。不得把 BK7258 代码放在工作区同级的 `vendor_beken/`，也不得直接修改 `bk_avdk_smp/ap` 来实现OpenVela内核移植。符号链接只负责接入构建，源码提交仍以比赛仓库为准。

开发时以工作区同级 `vendor_beken/` 中已有的 BK7236N 实现为代码组织和 NuttX 接入方式参考，重点参考其 `chips/bk7236n/`、`boards/bk7236n/bk7236n-evb/`、Kconfig、Make.defs、CMakeLists、启动、中断、UART 和 SysTick 实现；但不得直接复用 BK7236N 的寄存器、库、启动文件、链接脚本或镜像工具。

# BK7258 AP 移植 openvela 实施与验证方案

> 文档状态：源码交叉验证后的实施基线。本文区分“已由当前仓库证实”“工程假设”和“量产前待原厂确认”。在 BK7258 专属安全启动、OTP/eFuse 和 CPU1 镜像认证资料闭环前，本文只能指导功能性 AP 移植，不能作为量产安全作业指导书。

## 1. 目标和结论

本文给出在 BK7258 上运行 openvela 的可执行移植方案。目标是保留现有 CPU0/CP 固件、bootloader、分区和低层芯片服务，将运行在全芯片 CPU1+CPU2 上的 AP SMP 固件替换为 openvela/NuttX，并完成可启动、可调度、可交互和可打包的内核适配。首版允许先在 AP primary core（物理 CPU1）上完成单核 bring-up，但这只是降低调试复杂度的实验路径，正式 AP 兼容目标仍是 CPU1+CPU2 SMP。

架构结论必须先固定：BK7258 AP 是 Cortex-M33/ARMv8-M Mainline Thumb，不是 Cortex-A/ARMv8-A。移植必须使用 `openvela/nuttx/arch/arm`、NVIC、SysTick 和 MPU；任何 `arch/arm64`、GIC、AArch64 exception level、generic timer、MMU/页表、TF-A/BL31 或 `OUTPUT_ARCH(aarch64)` 方案均不适用。安全模型应讨论 TrustZone-M、SAU/IDAU、PPC/MPC 和可能的 TF-M，而不是 TF-A/EL3。

本文只采用一条正式实现路径，不把以下做法作为交付方案：

- 不修改 CP 固件来绕过 AP 启动等待。
- 不把 openvela 与 FreeRTOS/Armino AP 固件混链。
- 不使用只发送 ready 的临时 mailbox 驱动。
- 不通过构建后覆盖临时 `app1.bin` 的方式集成 AP 镜像。
- 不在 Secure/Non-secure 地址之间运行时自动选择。
- 不关闭 MPU、FPU 来回避架构问题。
- 不让 AP 自己初始化 PSRAM。

正式结论如下：

| 全芯片核 | 正式角色 | 本方案处理方式 |
| --- | --- | --- |
| CPU0 | CP、系统控制核、原 FreeRTOS 固件 | 原样保留 |
| CPU1 | AP primary core，当前 AP 固件第一启动目标 | 首版按现有 SPE 配置验证单核 openvela bring-up；实际安全状态必须实测 |
| CPU2 | AP secondary，AP SMP 的第二执行核 | 是否可在首版持续保持 reset 必须由启动链和实板确认；正式兼容目标启用 NuttX SMP |

AP 原 FreeRTOS 工程内部把物理 CPU1 称为逻辑 `core0`，把物理 CPU2 称为逻辑 `core1`。这只是 AP SMP 内部的编号，不能用于替代全芯片 CPU 编号。单核 bring-up 时 openvela 只运行逻辑 CPU index 0，BK mailbox 和系统控制固定使用物理 CPU1 对应资源；进入正式 SMP 阶段后必须建立逻辑 CPU 0/1 与物理 CPU1/CPU2 的稳定映射，并分别处理私有 NVIC、cache、TCM 和跨核启动。

### 1.1 结论可信度

| 结论 | 状态 | 当前依据或约束 |
| --- | --- | --- |
| CPU1 是 Cortex-M33、ARMv8-M、hard-float AP 目标 | 已证实 | BK7258 CPU1/AP defconfig 和工具链 |
| 目标 `app_ab` AP XIP 为 `0x02150000` | 已证实 | 目标分区 CSV 和 CPU0 的 34:32 换算代码 |
| AP 私有 SRAM 为 `0x28010000..0x28063fff` | 已证实 | `ram_regions.csv` 顺序累计及生成结果 |
| mailbox 外部中断源为 63 | 已证实 | `icu_map.h`、dispatch 和 mailbox driver |
| NuttX 中 mailbox IRQ 为 `16 + 63 = 79` | 已固化 | `contest/.../board/beken/chips/bk7258/include/irq.h` |
| CPU1 按 Secure alias 启动 | 强工程假设 | `CONFIG_SPE=1`、`-mcmse` 和现有 AP 行为支持，仍需读回安全寄存器 |
| CPU1 AP 被 BL1/BL2 验签、加密和防回滚 | 未证实 | 默认安全配置关闭，缺最终签名覆盖和篡改测试 |
| CPU1 可不配置 SAU而完全继承 CPU0设置 | 不成立 | 原 AP `SystemInitCpu0()` 在 SPE 下调用 `TZ_SAU_Setup()` |
| BK7236N vendor 静态库可用于 BK7258 | 不成立 | 库、寄存器、IRQ、startup 和分区均为 BK7236N 专用 |
| 正式 AP 是物理 CPU1+CPU2 SMP | 已证实 | 官方 AP(SMP)+CP 资料和 `bk_solution_ai` AP 配置均为 `CONFIG_CPU_CNT=2`、`CONFIG_SOC_SMP=y` |
| CPU1-only 可作为最终交付架构 | 不成立 | 当前正式 AP 为物理 CPU1+CPU2 的 NuttX SMP；CPU1-only 仅可作为早期诊断配置 |
| AP WDT 是本地硬件 watchdog | 不成立 | 硬件 WDT 由 CP 管理，AP 通过 mailbox 请求 reboot 并由 heartbeat 接受存活检测 |
| AP 可直接擦写 Flash | 不成立 | AP 的 Flash 擦写依赖 mailbox/CP 服务；XIP读取与擦写编程必须分开设计 |

### 1.2 当前实现状态

截至当前提交，代码已完成 AP 核启动、Mailbox 日志转发、GPIO/PWM 和 16 MB PSRAM allocator 基线。以下状态必须与"方案目标"区分：

| 项目 | 当前状态 | 说明 |
| --- | --- | --- |
| AP vector、C runtime、MPU、NuttX 链接 | 已实现 | `nuttx.bin` 已由 OpenVela 工程生成并通过实板启动 |
| MBOX0 channel 1 FIFO | 已实现 | CPU1 channel 使用 FIFO start 2、length 3；实板行为已验证 |
| RX指针校验 | 基础已修复 | 当前已接受 CP SRAM envelope 窗口 `0x28064000..0x2809f700`；Wi-Fi 等子系统的跨核链表逐节点校验仍有缺口 |
| TX buffer所有权 | 已实现 | 通用logical transport为每个channel保留pending副本，并用单active transaction和稳定slot约束in-flight生命周期 |
| Transport ACK消费 | 已实现基础恢复 | 已实现channel/sequence/command关联、稳定ACK pool、timeout、reset/quarantine和诊断；长时间故障注入仍待实板验收 |
| IPC power-up indication | 已实现 | UART link ready后在HW_CTRL `0x10`按原厂16字节ABI发送command 1并等待ACK，先于PWC `0x5` |
| IPC heartbeat | 已实现 | cmd1由启动worker同步发送；周期cmd2由CPU0-only mailbox TX worker按全局NuttX tick每2秒发送，使用原厂SWAP payload `0x2809f900`并检查`ack_state=2` |
| PWC worker和ready | 已实现 | PWC仅承担`0x5` SMP boot milestone、`0x11`最终commit/rollback及电源/PSRAM语义，不再替代IPC heartbeat |
| AP日志转发到CP UART0 | 基础已实现 | AP日志经Mailbox虚拟UART转发到CP UART0已工作；完整压力验证和reset恢复仍有缺口 |
| CPU2/NuttX SMP | 已实现基础启动 | 当前生成配置为双逻辑CPU；raw Mailbox boot handshake、IPI和CPU2 idle已通过启动轨迹验证 |
| 16 MB PSRAM MPU/布局 | 基础已实现 | OpenVela 已映射 `0x60000000..0x60ffffff`；AP heap、四个媒体pool和部分PWC已接入；CP ID/容量query、DMA lease和完整实测未闭环 |
| 最终 `all-app.bin` | 已构建 | 离线打包成功且AP镜像哈希一致；实板启动已验证 |
| CP不再AP boot retry | 待新固件验证 | 旧PWC `0x12`替代方案仍触发`IPC retry`；当前已恢复原厂HW_CTRL cmd1/cmd2并删除CP `0x12`翻译层 |

## 2. 资料和代码依据

### 2.1 openvela 移植规范

新平台应按 Architecture、Chip/SoC、Board 三层组织，芯片层负责启动、中断、串口、定时器、堆和芯片寄存器，板级层负责链接脚本、GPIO、控制台和板级初始化：

- `openvela/docs/zh-cn/chip_porting/porting_guide.md:13-29`
- `openvela/docs/zh-cn/chip_porting/porting_guide.md:83-114`
- `openvela/docs/zh-cn/chip_porting/porting_guide.md:452-539`

openvela 已提供 ARMv8-M 公共实现，不应重写 Cortex-M33 的上下文切换：

- `openvela/nuttx/arch/arm/src/armv8-m/Make.defs:21-45`
- `openvela/nuttx/arch/arm/src/arm_m/Make.defs:21-50`
- `openvela/nuttx/arch/arm/src/arm_m/arm_vectors.c`
- `openvela/nuttx/arch/arm/src/armv8-m/arm_initialstate.c`
- `openvela/nuttx/arch/arm/src/armv8-m/arm_doirq.c`

完整 Cortex-M33 参考平台使用 STM32L5，最小 ARMv8-M/NSH 配置参考 MPS2 AN521：

- `openvela/nuttx/arch/arm/src/stm32l5/`
- `openvela/nuttx/boards/arm/mps/mps2-an521/configs/nsh/defconfig`

### 2.2 BK7258 芯片和三核架构

BK7258 是三核 Cortex-M33 AMP 系统。当前工作区没有原稿所引用的 `bk_idk/docs/bk7258/` 文档目录，因此不能继续把这些路径作为证据；可复核依据改为源码配置：

- `bk_idk/middleware/soc/bk7258/bk7258.defconfig`
- `bk_idk/middleware/soc/bk7258_cp1/bk7258_cp1.defconfig`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/bk7258_ap.defconfig`
- `bk_idk/middleware/soc/bk7258_cp1/compile-options.cmake`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/compile-options.cmake`

三个核的资源和角色：

- CPU0 是主核，负责系统控制和主日志输出。
- CPU1 通过 mailbox 与 CPU0 通信，主要承载多媒体/AP业务。
- CPU1和CPU2组成AP SMP；外设中断可路由到任一物理核，USB等外设的默认路由不能替代正式资源表。
- CPU1/AP 编译属性为 `-mcpu=cortex-m33+nodsp -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mcmse`。
- 频率上限、TCM容量等产品参数在缺失正式 BK7258 文档时只作为待确认参数，不能作为链接或 timer 的硬编码输入。

### 2.3 `vendor_beken` 参考适配的适用边界

当前 `vendor_beken` 只包含 BK7236N：

- `vendor_beken/chips/bk7236n/`
- `vendor_beken/boards/bk7236n/bk7236n-evb/`

可复用的是 custom chip/board 组织、Make/CMake 接入模式、`uart_ops_s`、SysTick lower-half，以及用强符号将 BK ISR 注册 ABI 接入 NuttX 的思路：

- `vendor_beken/chips/bk7236n/Kconfig`
- `vendor_beken/chips/bk7236n/Make.defs`
- `vendor_beken/chips/bk7236n/CMakeLists.txt`
- `vendor_beken/chips/bk7236n/beken_irq.c`
- `vendor_beken/chips/bk7236n/beken_interrupt_base.c`
- `vendor_beken/chips/bk7236n/beken_timerisr.c`
- `vendor_beken/chips/bk7236n/beken_uart.c`

不得直接复用 BK7236N 的 `bootloader.bin`、`libs/*.a`、IRQ header、UART0 配置、链接脚本和 `bootloader + nuttx_crc.bin` 拼接工具。现有 `beken_head.S` 未被 Make/CMake 源列表可靠加入，链接入口又可能来自预编译 startup 库，因此它只能说明 legacy magic 的布局，不能作为已验证的 BK7258 reset 模板。

### 2.4 AVDK AP/CP 代码事实

CP 启动 AP 的代码路径：

```text
CP main()
  -> bk_init()
  -> bk_pm_module_vote_boot_cp1_ctrl(..., ON)
  -> pm_module_bootup_cpu1()
  -> start_cpu1_core()
  -> reset_cpu1_core()
  -> CPU1 从 AP image 启动
```

证据：

- `bk_avdk_smp/projects/app_ab/cp/cp_main.c:12-24`
- `bk_avdk_smp/cp/middleware/driver/pwr_clk/pwr_clk.c:323-374`
- `bk_avdk_smp/cp/components/bk_startup/system_main.c:141-202`

CPU1 启动地址由 AP 分区物理地址换算得到：

```c
logical_offset = physical_partition_offset / 34 * 32;
boot_address = SOC_FLASH_DATA_BASE + logical_offset;
```

`34` 是 Flash 中 32 字节数据加 2 字节 CRC 的物理占用，代码见：

- `bk_avdk_smp/cp/components/bk_startup/system_main.c:160-170`
- `bk_idk/components/bk_startup/system_main.c:141-174`

CP 设置 CPU1 power、boot address 和 software reset：

- `bk_avdk_smp/cp/components/bk_startup/system_main.c:173-195`
- `bk_idk/middleware/soc/bk7258/soc/sys_reg.h:117-141`
- `bk_idk/middleware/soc/bk7258/hal/sys_hal.c:297-316`

原 AP 使用 `Reset_Handler_Cpu0` 作为 AP 镜像入口，但 AP 内部第二核实际通过 `reset_cpu2_core()`启动并进入 `Reset_Handler_Cpu1`：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/bk7258_ap_bsp.ld:86-90`
- `bk_avdk_smp/ap/components/cmsis/CMSIS_5/Device/Beken/bk7236xx/Source/smp/startup_cpu1.c:259-318`

因此，openvela固件作为由物理CPU1引导、同时释放物理CPU2的双核AP镜像处理。当前实现已经接入NuttX SMP和CPU2 raw Mailbox启动握手；CPU1-only不属于正式交付契约，只用于隔离SMP故障。

## 3. 固定地址和安全模型

### 3.1 Secure 地址模型

当前 AVDK 和 IDK 的 CPU1 配置均启用 `CONFIG_SPE=1`：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/bk7258_ap.defconfig:41-43`
- `bk_idk/middleware/soc/bk7258_cp1/bk7258_cp1.defconfig:29-31`

BK 地址定义使用 Secure alias：

- Secure Flash：`0x02000000`
- Secure SRAM：`0x28000000`
- Secure PSRAM：`0x60000000`
- Secure mailbox：`0x41000000` 和 `0x41020000`

Non-secure 视图整体增加 `0x10000000`，但本方案不使用 Non-secure 视图。openvela 固定配置为 Secure 执行环境，所有镜像、向量、外设和共享内存地址都使用 Secure alias。

openvela 配置含义：

```text
CONFIG_ARCH_TRUSTZONE_DISABLED is not required
CONFIG_ARCH_TRUSTZONE_SECURE=y
CONFIG_ARMV8M_CMSE=y
```

这里的“Secure”仅表示首版按现有 AP 的 SPE 编译属性和 Secure alias 建立功能移植基线，不等于镜像已经通过 secure boot，也不等于 CPU1 已被限制在最小权限安全域。原 AP 的 `SystemInitCpu0()` 在 `CONFIG_SPE` 下调用 `TZ_SAU_Setup()`：

- `bk_avdk_smp/ap/components/cmsis/CMSIS_5/Device/Beken/bk7236xx/Source/smp/system_cpu0.c`

因此 openvela 不能简单省略 SAU 步骤。实施时必须先在 golden AP 上导出 SAU、PPC/MPC/PRRO 和关键访问属性，再决定由 openvela 复现原表，还是由不可变 bootloader 预配置并锁定。IDAU 的固定归属和配置锁定规则需要 TRM 说明。

当前仓库能证明存在安全工具和配置素材，但不能证明 CPU1 AP 已进入最终认证链：

- `bk_idk/middleware/boards/bk7258/csv/security.csv` 默认 `secureboot_en=FALSE`、`flash_aes_type=NONE`。
- `bk_idk/middleware/boards/bk7258/csv/otp1.csv` 定义 key hash、security counter、HUK/IAK 等字段。
- `bk_idk/middleware/boards/bk7258/csv/ppc.csv` 和 `mpc.csv` 提供生成素材，但默认值不是经威胁建模的最小权限策略。
- `bk_idk/tools/env_tools/beken_utils/scripts/partition.py` 和 `pack.py` 存在 `primary_all` 聚合与签名逻辑。
- 默认 BK7258 分区没有形成可复核的 BL2/TF-M/CPU1 安全构建产物，也没有 CPU1 单 bit 篡改后拒绝启动的证据。

开发阶段保持与 golden CP/bootloader 相同的启动模式，不烧写不可逆 OTP/eFuse，不自行切换 Non-secure image。正式量产必须由 bootloader 提供方确认 CPU1 是独立签名对象还是 `primary_all` 子镜像，并从最终签名输入、TLV/hash范围和实板负向测试三方面证明 AP 字节被覆盖。

### 3.2 Flash 分区

`projects/app_ab` 当前分区如下：

来源：`bk_avdk_smp/projects/app_ab/partitions/bk7258/auto_partitions.csv:1-16`

| 分区 | 物理起始 | 物理大小 | CRC解码后的虚拟起始 | 虚拟大小 |
| --- | ---: | ---: | ---: | ---: |
| `primary_bootloader` | `0x000000` | `0x011000` | bootloader管理 | bootloader管理 |
| `primary_cp_app` | `0x011000` | `0x154000` | `0x010000` | `0x140000` |
| `primary_ap_app` | `0x165000` | `0x121000` | `0x150000` | `0x110000` |

当前 Secure AP XIP 地址为：

```text
SOC_FLASH_DATA_BASE + CONFIG_AP_VIRTUAL_PARTITION_OFFSET
= 0x02000000 + 0x00150000
= 0x02150000
```

最终链接脚本不得长期维护另一份手写地址。`0x02150000`和`0x28010000..0x28064000`只属于当前`app_ab`布局，不是BK7258固定地址。`bk_solution_ai`中的实际AI项目还在`AP_SPINLOCK`之后加入`HARDWARE_ACC`，使AP RAM起点和大小发生变化。建议由目标工程的生成结果转换出最小的 `bk7258_ap_layout.h`，只导出 AP Flash/RAM、保留区和硬件加速区必需宏，再预处理得到 `bk7258_openvela_ap.ld`。不要直接包含整个 BK `sdkconfig.h`，避免和 NuttX `nuttx/config.h` 宏冲突。生成链必须是显式依赖：

```text
target partition CSV
  -> BK partition generator
  -> partitions_gen.h + ram_regions.h + bk_package.json
  -> validate/convert_bk7258_layout
  -> bk7258_ap_layout.h
  -> preprocess ld.script
  -> link openvela AP
```

干净构建必须先生成布局 header，缺文件或值与 `bk_package.json` 不一致时立即失败。

### 3.3 SRAM 分区

当前 `app_ab` SRAM布局来自 `bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv:2-10`：

| 区域 | 起始地址 | 大小 | 结束地址 | 所有者 |
| --- | ---: | ---: | ---: | --- |
| `AP_SPINLOCK` | `0x28000000` | `0x10000` | `0x2800ffff` | CP/AP协议保留 |
| `AP_RAM` | `0x28010000` | `0x54000` | `0x28063fff` | CPU1 openvela |
| `CP_RAM` | `0x28064000` | `0x3b700` | `0x2809f6ff` | CPU0 CP |
| `PWR_MNG` | `0x2809f700` | `0x100` | `0x2809f7ff` | CP/AP固定ABI |
| `SWAP` | `0x2809f800` | `0x800` | `0x2809ffff` | 系统保留 |

openvela 的主 RAM 只能是：

```text
RAM_START = 0x28010000
RAM_END   = 0x28064000
RAM_SIZE  = 0x54000
```

必须禁止：

- 清零 CP_RAM、PWR_MNG、SWAP。
- 把 `AP_SPINLOCK` 放入 heap。
- 把 mailbox共享 buffer放入 CP_RAM或PWR_MNG。
- 让链接器隐式生成 `.itcm`、`.dtcm`、`.psram` alloc section。

### 3.4 PSRAM所有权

PSRAM映射起点为 `0x60000000`，当前目标 `app_ab` 已采用 16 MB 布局
`0x60000000..0x60ffffff`。实际器件 ID、容量和地址线仍需实板确认；其他 AI
项目可能使用不同 SRAM/Flash/PSRAM 比例，不能只复制容量数字。PSRAM职责必须
拆分为 PHY/电压/时钟与 ID 检测、AP 多媒体静态区域初始化、AP/CP heap 和低功耗
投票四层，不能统一描述成“全部由 CP 初始化”或“全部由 AP 初始化”。

- `bk_avdk_smp/cp/middleware/driver/psram/psram_driver.c:249-369`
- `bk_avdk_smp/ap/middleware/driver/pwr_clk/pwr_clk.c:346-382`

正式规则：

1. CPU1/openvela不得重复执行与CP冲突的PSRAM PHY、电压、时钟和ID检测初始化；实际执行主体由golden启动链确认。
2. 若原AP在CPU1启动后初始化AP多媒体静态PSRAM区域，openvela必须实现等价步骤或通过正式PWC服务完成，不能因禁用PHY初始化而遗漏AP侧内存管理初始化。
3. CPU1使用动态PSRAM前通过正式PWC协议请求/确认上电，并等待semantic response；不能假设存在独立的异步power-on广播。
4. CPU1状态机确认响应成功后才启用独立PSRAM allocator/pool；不能直接复用依赖FreeRTOS的`CONFIG_PSRAM_AS_SYS_MEMORY`和`psram_malloc()`实现。
5. power-off或recovery前冻结独立allocator；只有受控对象引用计数和outstanding allocation均为0时才确认断电，否则返回busy/fail。
6. PSRAM初始配置为MPU normal non-cacheable；未经共享buffer一致性验证不得打开D-cache。

AP侧不得未经确认直接调用 `bk_psram_init()`、`bk_psram_deinit()` 或 `bk_psram_id_auto_detect()`。开工前必须从golden启动链分别确认PHY初始化、AP静态slab初始化和heap初始化的执行主体。openvela实现基于PWC请求/响应的状态机和独立allocator，不复用FreeRTOS AP的PSRAM heap接口。

当前单核/双核启动 bring-up 仍只依赖 AP SRAM；16 MB PSRAM 已具备硬件窗口和
MPU 映射，但 allocator 尚未上线。NuttX 通用 heap region 注册后通常不能安全
注销，也不能迁移任意调用者持有的对象，因此动态可掉电 PSRAM 不得直接加入
通用 kernel heap；它是由 CP 管理、由 AP 专用 allocator 控制的可选数据面。

## 4. 正式启动时序

### 4.1 CPU0/CP阶段

CP保持原代码，不改变 `projects/app_ab/cp/cp_main.c`：

1. Bootloader/CP完成CPU0启动。
2. CP初始化系统时钟、电源管理、UART0、CPU0侧mailbox和必要共享资源。
3. CP按 `PM_BOOT_CP1_MODULE_NAME_APP`投票打开CPU1。
4. CP清除CPU1 power-down。
5. CP将AP逻辑起始地址写入CPU1 boot offset寄存器。写入值为 `boot_address >> 8`。
6. CP选择CPU1 RXEVT路径，释放CPU1 software reset。
7. CP启动CPU1后存在两套独立等待：原厂IPC heartbeat任务每次最多等待2秒接收HW_CTRL power-up indication，最多打印到`retry_cnt:4`；OpenVela boot transaction在当前实现中给AP 10秒完成UART link、HW_CTRL cmd1和PWC `0x5` milestone，失败保留诊断并rollback。

代码依据：

- `bk_avdk_smp/cp/middleware/driver/pwr_clk/pwr_clk.c:323-374`
- `bk_avdk_smp/cp/components/bk_startup/system_main.c:173-195`
- `bk_avdk_smp/cp/components/bk_startup/system_main.c:203-220`
- `bk_avdk_smp/cp/include/driver/pwr_clk.h:26-43`

### 4.2 CPU1/openvela阶段

CPU1 reset handler必须严格按以下顺序执行：

```text
CPU1 boot vector
  1. 关闭异常和外部中断
  2. 使用 boot vector 提供的 MSP
  3. 设置 MSPLIM/栈边界
  4. 设置 CPU1 固定身份，不写 CPU0 身份
  5. 初始化 Secure CM33 基础状态
  6. 设置 CP10/CP11，建立 hard-float运行环境
  7. 初始化必要的 TCM/系统 fault 状态
  8. 复制 .data 和 runtime vector；首版禁止可执行 .ramfunc
  9. 清零 AP 私有 .bss
 10. 初始化 MPU
 11. 设置 VTOR 到 RAM runtime vector
 12. DSB; ISB
  13. 仅完成必要的低级Mailbox准备；不调用NuttX IRQ/同步API
 14. 调用 nx_start()，不得返回
      -> hardware_initialize()
         -> irq_initialize()调用BK up_irqinitialize()
         -> clock_initialize()调用SysTick lower-half
         -> up_initialize()/early serial注册Mailbox UART console并启动physical MBOX IRQ79
      -> nx_bringup()/board late
          -> 启动Mailbox和Mailbox UART workers并完成STATE link
          -> 启动CPU1->CPU0 HW_CTRL power-up和2秒heartbeat服务
          -> 创建PM worker并等待worker_ready握手
          -> open MB_CHNL_PWC
          -> 发送一次 PM_CPU1_BOOT_READY_CMD(1, 0, 0)
```

这个顺序的依据是：

- C代码调用前必须先完成SP、C runtime和FPU状态。
- MPU必须在NuttX访问heap和外设前建立，但runtime vector复制必须在MPU限制生效前完成或放在允许区域。
- VTOR必须在NVIC开启前切换，避免中断进入错误向量。
- reset阶段不能调用 `irq_attach()`、NuttX spinlock/semaphore或创建worker，因为 `irq_initialize()`、memory和scheduler尚未完成。mailbox必须在 `up_initialize()` 后具备完整RX/ACK能力；PWC复杂动作由PM worker执行，因为CP可能在ready后立即发送命令。
- 不得在 `nx_start()`返回后手工调用 `irq_initialize()`、`clock_initialize()`或 `up_initialize()`；这些步骤是上面展示的内部调用链，`nx_start()`正常情况下不返回。
- `nx_start()`会初始化NuttX的任务、IRQ、clock和硬件，并最终创建应用启动流程，见 `openvela/nuttx/sched/init/nx_start.c:658-826`。

ready的正式含义是：CPU1已经具备持续运行、响应CP IPC/PWC命令并发送heartbeat的能力，不表示NSH或所有应用已启动。CPU1必须先发送兼容Armino的IPC power-up indication；ready发送必须在NuttX调度器、Mailbox RX路径、IPC heartbeat和PM worker具备后执行，禁止只发送一个裸ready消息。

### 4.3 NuttX启动阶段

进入 `nx_start()`后的调用顺序：

```text
nx_start()
  -> tasklist_initialize()
  -> memory_initialize()
  -> hardware_initialize()
       -> irq_initialize()
       -> clock_initialize()
       -> up_initialize()
       -> drivers_initialize()
       -> board_early_initialize()
  -> nx_bringup()
       -> board_late_initialize()
       -> nsh_main()
```

对应实现位置：

- `openvela/nuttx/sched/init/nx_start.c:757-826`
- `openvela/nuttx/sched/init/nx_bringup.c:293-501`

BK特有的CPU1身份、MPU和vector属于reset阶段；NuttX IRQ注册、UART正式设备和mailbox transport正式目标属于 `up_initialize()`；PM worker、PWC命令语义和ready发送属于board bring-up。UART不在board late重复注册。这样既符合NuttX生命周期，也保证ready发出时调度器和RX路径已可用。

当前实现已消除早期偏差：early serial阶段执行logical init、`bk7258_mbox_init()`、IRQ79注册和console设备注册，不创建或等待worker；board late只启动Mailbox/MB-UART workers、完成STATE link、HW_CTRL liveness、PWC worker及`0x5/0x11`生命周期。这样既避开scheduler-lock上下文创建线程，也保证CP进入OpenVela transaction时AP已有physical RX/ACK路径。

## 5. 双向量镜像设计

### 5.1 必须保留Flash boot vector

CPU1 boot address直接指向AP镜像起点，因此镜像offset 0必须是有效的 Cortex-M vector：

- word 0：AP RAM范围内的初始MSP。
- word 1：位于AP Flash的Thumb reset handler地址；仅凭该值不能证明Secure state。

原BK镜像在offset `0x100`保留 `BK7236` legacy download magic：

- `bk_avdk_smp/ap/components/cmsis/CMSIS_5/Device/Beken/bk7236xx/Source/smp/startup_cpu0.c:229-231`

外部IRQ向量表中的该位置会被magic覆盖，因此不能把这份Flash表作为运行时完整向量。

### 5.2 RAM runtime vector

正式链接脚本定义两套内容：

```text
.bk_boot_vectors
  VMA/LMA: Flash image offset 0
  内容: MSP、Reset、bootloader magic兼容布局

.vectors
  LMA: Flash
  VMA: AP SRAM固定区域
  内容: 完整NuttX ARMv8-M向量表
```

Reset handler完成 `.vectors` 从 Flash LMA复制到 AP SRAM VMA，然后设置 `SCB->VTOR`并执行：

```c
__DSB();
__ISB();
```

芯片 Kconfig 必须选择 `ARCH_HAVE_CUSTOM_VECTORS`。不能直接启用NuttX通用 `CONFIG_ARCH_RAMVECTORS`，因为其实现会从当前Flash vector逐项复制：

- `openvela/nuttx/arch/arm/src/arm_m/arm_ramvec_initialize.c:77-105`

当前Flash vector含magic时，这会把magic复制成IRQ handler。正式配置关闭通用RAM vector复制，由BK7258芯片层自行完成一次独立的runtime vector安装。

runtime vector可按NuttX公共vector定义生成，但必须作为独立对象放入 `.ram_vectors`，不能从boot table复制：

- `openvela/nuttx/arch/arm/src/arm_m/arm_vectors.c:102-127`

链接器必须对两套vector执行512字节对齐断言，原BK脚本已有相同要求：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/bk7258_ap_bsp.ld:94-114`

## 6. openvela芯片层实现

BK7258移植代码必须放在比赛仓库的`board/beken/`中，并遵守OpenVela官方vendor目录格式；`contest/vendor/beken/`通过符号链接接入。工作区同级的`vendor_beken/`只作为BK7236N参考实现。当前目录：

```text
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
├── chips/bk7258/
│   ├── CMakeLists.txt
│   ├── Kconfig
│   ├── Make.defs
│   ├── chip.h
│   ├── include/irq.h
│   ├── bk7258_start.c
│   ├── bk7258_vectors.c
│   ├── bk7258_irq.c
│   ├── bk7258_timerisr.c
│   ├── bk7258_lowputc.c
│   ├── bk7258_serial.c
│   ├── bk7258_allocateheap.c
│   ├── bk7258_mbox0.c
│   ├── bk7258_mailbox_channel.c
│   ├── bk7258_pm_pwc.c
│   └── hardware/
│       ├── bk7258_memorymap.h
│       ├── bk7258_irq.h
│       ├── bk7258_sysctrl.h
│       ├── bk7258_uart.h
│       └── bk7258_mbox.h
└── boards/bk7258/bk7258-ap/
    ├── Kconfig
    ├── CMakeLists.txt
    ├── configs/nsh/defconfig
    ├── include/board.h
    ├── scripts/Make.defs
    ├── scripts/ld.script
    └── src/
        ├── CMakeLists.txt
        ├── bk7258_boot.c
        ├── bk7258_bringup.c
        └── bk7258_appinit.c
```

从`contest/nuttx`解析时，defconfig当前使用实际相对路径：

```text
CONFIG_ARCH_CHIP_CUSTOM_DIR="../vendor/beken/chips/bk7258"
CONFIG_ARCH_CHIP_CUSTOM_DIR_RELPATH=y
CONFIG_ARCH_BOARD_CUSTOM_DIR="../vendor/beken/boards/bk7258/bk7258-ap"
CONFIG_ARCH_BOARD_CUSTOM_DIR_RELPATH=y
```

Make 和 CMake 源列表必须一致，启动 vector 源文件必须确实加入两种构建。BK7236N 代码仅用于参考组织方式和 NuttX 接入方式，BK7258 必须重新实现芯片相关代码。

### 6.1 Kconfig和工具链

芯片Kconfig必须选择：

```kconfig
config ARCH_CHIP_BK7258
    bool "BK7258"
    select ARCH_CORTEXM33
    select ARCH_HAVE_CUSTOM_VECTORS
    select ARCH_HAVE_FPU
    select ARCH_HAVE_MPU
    select ARM_HAVE_MPU_UNIFIED
    select ARCH_HAVE_TRUSTZONE
```

Make.defs必须引用：

```make
include armv8-m/Make.defs
```

必须使用与原AP一致的CPU属性：

```text
-mcpu=cortex-m33+nodsp
-mthumb
-mfpu=fpv5-sp-d16
-mfloat-abi=hard
-mcmse
```

`+nodsp`是为了保持与现有AP binary和SDK编译属性一致，不代表Datasheet证明硬件不支持DSP。openvela不启用 `CONFIG_ARM_DSP`，以后单独评审启用DSP的ABI和指令属性。

### 6.2 Reset/C runtime

`bk7258_start.c`必须实现：

- `__start()`。
- `_sbss/_ebss`清零。
- `_eronly -> _sdata/_edata`复制。
- 首版拒绝 `.ramfunc` alloc section；确需执行时再单独增加可执行MPU override和复制符号。
- runtime vector复制和VTOR切换。
- CPU1 Secure基础状态和FPU状态。
- MPU初始化。
- 调用 `nx_start()`且不得返回。

不能直接链接原FreeRTOS CMSIS startup，因为它会定义另一套 Reset、SVC、PendSV、SysTick和 `_start`：

- `bk_avdk_smp/ap/components/cmsis/CMSIS_5/Device/Beken/bk7236xx/Source/smp/startup_cpu0.c:360-400`

应复用其硬件事实，例如VTOR、TCM、FPU和MSPLIM的设置方式，但由openvela芯片层重新组织调用顺序。

### 6.3 IRQ

BK7258 AP IRQ表来源：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/icu_map.h:28-94`
- `bk_avdk_smp/ap/middleware/driver/bk7258_ap/interrupt.c:114-245`

已确认的外部NVIC line：

| 外设 | 外部line | NuttX IRQ |
| --- | ---: | ---: |
| TIMER0 | 3 | 19 |
| UART0 | 4 | 20 |
| TIMER1 | 13 | 29 |
| UART1 | 15 | 31 |
| UART2 | 16 | 32 |
| MAILBOX统一ICU source | 63 | 79 |

openvela `bk7258_irq.c`实现：

- `up_irqinitialize()`：关闭所有外部IRQ、设置VTOR、配置默认priority、绑定SVC/HardFault/BusFault/UsageFault/MemFault/SecureFault、最后开启中断。
- `up_enable_irq()`：按NVIC ISER或系统异常寄存器开启。
- `up_disable_irq()`：按NVIC ICER或系统异常寄存器关闭。
- `up_prioritize_irq()`：按NVIC优先级寄存器设置。
- `arm_ack_irq()`：NVIC无需单独ack，外设ISR负责清设备pending。

BK7258外部中断不是只配置NVIC即可，必须实现完整三级路径：

```text
外设中断源
  -> 系统控制层将source路由到物理CPU1或CPU2
  -> 目标core私有NVIC external line
  -> NuttX IRQ编号和ISR
```

例如mailbox必须完成`source 63 -> CPU1系统路由 -> NVIC line 63 -> NuttX IRQ 79`。只在`irq.h`定义编号不能使中断到达CPU1。进入SMP阶段后，每个外设还必须明确唯一owner或共享路由策略，不能让CPU1和CPU2同时无保护处理同一外设中断；AP日志使用Mailbox UART逻辑通道，不要求CPU1接管物理UART1中断。

必须在 `include/irq.h` 固化两个编号域，例如 `BK7258_EXTIRQ_MAILBOX=63`、`BK7258_IRQ_MAILBOX=BK7258_IRQ_FIRST+63=79`，并设置 `NR_IRQS=80`。`irq_attach()`使用79，NVIC bit和BK gate使用63。`IRQ_MAILBOX0=48`、`IRQ_MAILBOX1=49`等旧header注释不能代替CPU1最终映射。

还必须区分两套“group”命名：`icu_map.h`中的逻辑group与系统寄存器0-31/32-63 enable bank不是同一概念。mailbox driver实际对source 63调用 `sys_drv_core_intr_group2_enable(..., 1 << (63 - 32))`，即打开32-63 bank的bit31。文档和实现均应使用“32-63 bank”描述寄存器，避免写成含糊的GROUP1。

### 6.4 Timer

系统tick固定使用 Cortex-M SysTick：

- `CONFIG_TIMER=y`
- `CONFIG_ARMV8M_SYSTICK=y`
- `CONFIG_USEC_PER_TICK=1000`
- `CONFIG_SCHED_TICKLESS`关闭

优先复用openvela ARMv8-M SysTick lower-half，而不是自写旧式ISR直接调用 `nxsched_process_timer()`：

- `vendor_beken/chips/bk7236n/beken_timerisr.c`
- `openvela/nuttx/arch/arm/src/armv8-m/Kconfig`

`up_timer_initialize()`调用 `systick_initialize(true, CONFIG_CPU_FREQ_HZ, -1)`，再调用 `up_timer_set_lowerhalf()`。

首版固定CPU1频率，不支持runtime DVFS，直到确认CP通知发生在频率切换前还是后、SysTick时钟源及时间补偿协议。仅暂停并重装reload会丢tick，不能作为正式DVFS实现。后续必须设计pre-change/post-change握手、临界区更新和已流逝时间补偿测试后再启用。

不把BK TIMER0同时用于系统tick，避免与CP和外设共享同一timer资源。

### 6.5 AP日志转发到CP UART0

正式物理日志口使用CP UART0。AP日志目标路径是Mailbox虚拟UART转发到CP，由CP侧UART0统一输出；当前已实现基础日志转发，完整压力验证和reset恢复仍有缺口：

- AP通过Mailbox UART逻辑通道向CP发送日志数据。
- CP接收后通过UART0输出到开发板的物理日志口。
- CP UART0：115200 8N1。
- CPU1不接管GPIO0/1，不启用UART1作为AP console。

UART0是板上的物理日志口：BK7258 UART0 TX/RX 经 CH340 USB转串口芯片接到 Type-C 接口，PC 端通过 Type-C 枚举出 CH340 串口设备接收日志。因此 CP 的所有 log、AP 经Mailbox转发的 log，以及烧录工具的下载通信，都通过这条 UART0->CH340->Type-C->PC 物理链路完成。OpenVela AP不建立独立的UART1控制台。

地址和GPIO映射见：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/gpio_map.h:26-40`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/gpio_map.h:169-179`

当前 `app_ab` AP GPIO配置把GPIO0/1作为I2C1，见：

- `bk_avdk_smp/projects/app_ab/ap/config/bk7258_ap/usr_gpio_cfg.h:22-25`

正式OpenVela board配置不应接管GPIO0/1为UART1，保留CP侧UART0和现有AP GPIO资源配置。CP GPIO配置不修改。

芯片层实现：

- `bk7258_lowputc.c`：早期bring-up使用UART1 polling；当前正式日志路径已切换到Mailbox UART转发，UART1临时路径应已禁用或删除。
- `bk7258_mailbox_channel.c`：承载Mailbox逻辑命令和PWC；Mailbox UART日志转发基础已实现，完整流控、压力和reset恢复仍有缺口。
- AP侧日志转发不得阻塞Mailbox中断，也不能在中断上下文执行复杂格式化。
- CP侧使用现有UART0 log driver输出到物理串口；AP不注册物理UART1 console。

AP控制台目标必须依赖CPU0 mailbox日志转发，因为网页文档规定BK7258 AP日志通过Mailbox发送到CP，再由CP侧日志串口输出。ready之前的启动故障日志暂不能承诺可见；不能另行切换到UART1作为正式路径。

官方AP日志路径通过Mailbox转发到CP的UART0。控制台只保留一条正式路径：AP日志经Mailbox虚拟UART发送，CP通过UART0输出。必须区分Mailbox虚拟UART和CPU1物理UART1，不能把二者混用。

#### 6.5.1 本轮方案：仅使用开发板UART0

本轮开发板只有一条可连接电脑的物理串口，因此暂不使用CPU1物理UART1。正式调试路径固定为：

```text
OpenVela CPU1
  -> AP syslog/console backend
  -> Mailbox UART logical channel
  -> MBOX0 shared buffer and ACK
  -> CP Mailbox UART receiver
  -> CP UART0 log/output driver
  -> GPIO11/UART0_TX
  -> CH340/USB Type-C/PC
```

UART1在本方案中不是备用控制台，而是明确禁用的临时bring-up路径：

- 不把GPIO0/1切换为UART1。
- 不注册物理UART1为`/dev/console`。
- 不要求第二个USB-TTL或额外串口设备。
- AP的NSH标准输入、标准输出和标准错误均通过Mailbox虚拟UART连接到CP UART0。
- CP原有UART0日志、CP Shell和AP转发日志共享同一物理输出路径；输出必须保持可区分的`ap0`前缀，避免把AP日志误认为CP日志。

#### 6.5.2 参考协议和通道选择

实现前必须从Armino源代码确认以下内容，禁止仅根据`MB_CHNL_LOG`名称自行定义新协议：

- `bk_avdk_smp/ap/middleware/driver/mb_uart/`或对应`mb_uart_driver.c`的实际路径、逻辑通道和消息结构。
- `bk_avdk_smp/ap/include/driver/mb_uart.h`及相关类型、状态码和回调语义。
- CP侧对应的Mailbox UART接收、发送和UART0输出实现。
- `博通网页文档/多核通讯-Uart通讯接口 — 博通集成 ARMINO SMP开发框架 文档.html`中`MB_UART0`的初始化、读写、TX/RX回调和溢出语义。
- AP/CP两侧的共享buffer位置、长度、32字节对齐要求、缓存属性和buffer所有权。

优先复用Armino已有的`MB_UART0`作为AP到CP的日志输出通道。只有在源码交叉验证确认`MB_UART0`不是CPU1到CPU0日志方向时，才选择对应的日志逻辑通道；不能把IPC `0x10`或PWC通道复用于日志。

#### 6.5.3 AP侧分层设计

AP侧应拆成三个层次，避免在syslog路径直接操作MBOX寄存器：

1. **Mailbox UART transport**：负责标准logical channel、共享buffer、sequence、transport ACK、FIFO full、reset和错误计数。
2. **NuttX serial/syslog backend**：将`/dev/console`、`/dev/ttyS0`或等价的NuttX console设备映射到Mailbox UART虚拟设备；具体设备名以NuttX配置和驱动注册方式为准，不预先假定`ttyS0`。
3. **NSH和syslog使用层**：让`nsh_main`使用同一虚拟console，syslog输出复用同一路径；禁止为NSH另建UART1物理后端。

日志数据路径要求：

- 普通日志在任务上下文写入有界环形TX队列，由Mailbox UART worker异步发送。
- Mailbox ISR只完成FIFO drain、消息校验、transport ACK和事件投递，不格式化日志、不等待semaphore、不执行文件系统操作。
- TX队列满时采用明确策略：普通日志丢弃并累计计数；错误日志优先但仍受有界队列和单条最大长度限制；不能无限阻塞影响heartbeat和系统启动。
- 日志消息必须支持换行、连续短消息合并和单条消息分片；分片不得破坏消息边界或复用仍处于in-flight状态的共享buffer。
- AP启动早期在NuttX mailbox transport正式可用前，不承诺完整日志转发；如保留最小故障路径，必须是有界、无阻塞、不会接管GPIO0/1的机制。
- AP日志应由CP侧统一添加或保留`ap0`来源标识，并禁止在AP和CP两侧重复添加前缀。

#### 6.5.4 CP侧接收和UART0输出

CP侧保持原有UART0硬件和GPIO配置，不修改`projects/app_ab/cp/config/bk7258/usr_gpio_cfg.h`。Mailbox UART接收服务应：

- 在Mailbox接收路径完成source、destination、长度、对齐、地址窗口和sequence校验。
- 对有效日志完成transport ACK后，将数据投递到CP日志输出队列；不在Mailbox ISR中直接阻塞写UART0。
- 由CP日志任务按原有UART0驱动输出，保持115200 8N1和现有CP Shell行为。
- 正确处理AP重启、旧消息、重复ACK、FIFO full和reset重同步，避免旧AP日志污染新一轮启动。
- 对AP日志转发失败保留计数和最后错误原因；诊断信息不能反过来依赖同一条已经失效的Mailbox日志通道。

#### 6.5.5 启动时序和ready约束

日志转发服务不能改变CPU1启动契约：

```text
CPU1 reset
  -> NuttX基础启动和UART0-independent console backend准备
  -> MBOX0物理层、IRQ79和RX drain可用
  -> Mailbox UART logical channel建立
  -> AP日志TX队列和worker就绪
  -> IPC power-up indication
  -> heartbeat worker
  -> PWC worker
  -> PM_CPU1_BOOT_READY_CMD
  -> NSH启动并使用Mailbox console
```

其中`IPC power-up indication`和heartbeat仍使用`MB_CHNL_HW_CTRL`，Mailbox UART日志不得覆盖、复用或改变这两个IPC command。`PM_CPU1_BOOT_READY_CMD`只能在日志worker、Mailbox RX路径和PWC worker均已可运行后发送，但日志worker失败不能通过无界等待阻塞heartbeat；应记录失败并按预先定义的降级策略继续或停止ready。

#### 6.5.6 分阶段实施和验收

本轮review通过后按以下顺序实施：

1. **协议确认**：从Armino AP/CP源码确认MB_UART0方向、channel ID、消息格式、buffer和回调语义，形成AP/CP字段对照表。
2. **只读验证**：先在OpenVela中保留UART1代码但不启用，增加Mailbox UART最小发送测试；测试数据不能接管GPIO0/1。
3. **AP transport**：实现对齐共享buffer、TX队列、分片、ACK、超时、FIFO full和reset状态机。
4. **CP接收桥接**：接入现有CP UART0日志任务，验证CP日志和AP日志并发输出不互相破坏。
5. **NuttX console接入**：将NSH console、syslog和`up_putc`的正式后端切换到Mailbox UART；删除或禁用UART1 console注册和GPIO0/1 pinmux。
6. **启动诊断**：在Mailbox不可用阶段只保留最小故障计数；确认正常启动后AP日志和`nsh>`均能从UART0观察到。
7. **压力验收**：验证连续日志、长行分片、队列满、AP复位、CP复位、Mailbox reset、heartbeat并发和CP Shell输入。

最低验收日志：

```text
CP UART0: CP boot log
CP UART0: ap0: OpenVela early/bringup log
CP UART0: ap0: mailbox uart ready
CP UART0: nsh>
```

必须同时满足：

- 只有UART0连接电脑即可完成日志观察和NSH交互。
- GPIO0/1没有被OpenVela切换为UART1。
- 连续运行期间不出现IPC heartbeat timeout。
- 日志压力不能导致Mailbox TX buffer覆盖、PWC卡死或CP UART0异常。
- AP重启后旧日志不会在新一轮启动中重复输出。

### 6.6 MPU、FPU和cache

正式配置启用：

```text
CONFIG_ARM_MPU=y
CONFIG_ARM_MPU_RESET=y
CONFIG_ARCH_FPU=y
CONFIG_ARMV8M_CMSE=y
CONFIG_ARM_DSP is not set
```

MPU region不是原BK宽范围表的原样复制，而是基于其memory attribute重新收紧所有权。region数量必须从MPU `TYPE.DREGION`读回，显式写入 `CONFIG_ARM_MPU_NREGIONS`并在启动时校验；在TRM或实测前不得写死16，也不能盲目分配超过硬件能力的region。若region不足，首版只保留Flash、AP SRAM、外设和必需共享窗口，PSRAM/QSPI后置。目标策略：

| 区域 | 地址 | 属性 |
| --- | --- | --- |
| Flash | `0x02000000-0x02ffffff` | Secure RO/X，write-through |
| AP instruction alias | `0x08000000-0x0809ffff` | RO/X |
| DTCM | `0x20000000-0x20003fff` | RW/XN，non-cacheable |
| AP SRAM | `0x28010000-0x28063fff` | RW/XN，shareable；不得包含CP RAM |
| PWR_MNG | `0x2809f700-0x2809f7ff` | RW，Device，shareable |
| peripheral | `0x40000000-0x5fffffff` | Device，XN，shareable |
| PSRAM | `0x60000000-0x63ffffff` | RW/XN，non-cacheable |
| QSPI0 | `0x64000000-0x67ffffff` | non-cacheable，XN |
| QSPI1 | `0x68000000-0x6bffffff` | non-cacheable，XN |

参考：`bk_avdk_smp/ap/middleware/soc/bk7258_ap/mpu_cfg.c:38-149`。原表将SRAM覆盖到 `0x3fffffff`，不能提供AP/CP边界；PWR_MNG若作为AP SRAM region的override，必须明确更高优先级region编号。首版禁止 `.ramfunc`，避免与SRAM整体XN策略矛盾。

FPU从reset阶段启用CP10/CP11，所有对象统一hard-float。NuttX公共层负责线程浮点上下文保存，不能把FPU支持延后到另一个ABI。

I-cache和D-cache在正式第一版保持关闭，因为现有 `mbox0_adapter.c`在发送端没有与接收端对称的D-cache clean/invalidate：

- `bk_avdk_smp/ap/middleware/driver/mailbox/mbox0_adapter.c:15-38`
- `bk_avdk_smp/ap/middleware/driver/mailbox/mbox0_adapter.c:99-128`

后续若启用D-cache，必须先将mailbox共享buffer放到non-cacheable MPU region，或者为发送、接收和所有权交接实现完整32字节cache line clean/invalidate、DMB和DSB。

## 7. Mailbox v2和PWC服务

### 7.1 不能使用旧版裸mailbox

AP配置明确启用 `CONFIG_MAILBOX_V2_0`：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/bk7258_ap.defconfig:28-30`
- `bk_avdk_smp/ap/middleware/soc/common/CMakeLists.txt:71-81`

mailbox v2的实际数据链为：

```text
logical command
  -> mailbox_channel物理头
  -> AP共享SRAM buffer
  -> MBOX0 FIFO发送buffer pointer + length
  -> 对端从共享SRAM读取16字节message
  -> command/ACK状态机
```

证据：

- physical header含 `cmd/state/ctrl/tx_seq/logical_chnl`：`bk_avdk_smp/ap/middleware/driver/mailbox/mailbox_channel.c:51-71`
- logical TX写入sequence和logical channel：`bk_avdk_smp/ap/middleware/driver/mailbox/mailbox_channel.c:207-223`
- mailbox发送共享buffer指针和长度：`bk_avdk_smp/ap/middleware/driver/mailbox/mbox0_adapter.c:99-128`
- MBOX0 FIFO长度和channel配置：`bk_avdk_smp/ap/middleware/driver/mailbox/mbox0_drv.c:6-27`
- 当前AP CMake编译v2驱动：`bk_avdk_smp/ap/middleware/driver/CMakeLists.txt:392-423`

### 7.2 CPU1必须实现的transport

openvela芯片层必须完整移植以下逻辑，不能仅保留ready或仅发送裸指针消息：

1. `mbox0_ll.h`、`mbox0_struct.h`、`mbox0_hal.c`对应的寄存器和FIFO操作。
2. CPU1 channel的FIFO起始地址、长度和owner配置。
3. `INT_SRC_MAILBOX`外部line 63到NuttX IRQ 79的注册、32-63 gate使能、清pending和FIFO drain。
4. RX FIFO中所有message的循环读取，不能每次中断只取一个。
5. 发送共享buffer池，command和ACK独立且32字节对齐；当前实现使用固定对齐发送buffer，仍需补充严格的in-flight/已消费所有权和并发保护，不能把当前实现宣称为完整buffer池。
6. physical message的pointer/length envelope；接收时检查source、destination、指针窗口、对齐、长度、加法溢出和buffer所有权。
7. logical channel source/destination编码。
8. `tx_seq`递增、reset和重新同步。
9. `CHNL_CTRL_ACK_BOX`、`CHNL_CTRL_SYNC_TX`和`CHNL_CTRL_RESET`处理。
10. command RX、ACK RX、发送完成和busy状态机；当前板级实现已具备基础RX drain和发送路径，完整ACK/reset/busy恢复仍待实测。
11. NuttX临界区/spinlock保护，禁止使用FreeRTOS临界区和semaphore。
12. transport错误计数、超时、FIFO full、非法source/destination和reset恢复。

应以这些文件为协议移植依据，而不是自行重新定义格式：

- `bk_avdk_smp/ap/middleware/driver/mailbox/mbox0_drv.c`
- `bk_avdk_smp/ap/middleware/driver/mailbox/mbox0_adapter.c`
- `bk_avdk_smp/ap/middleware/soc/common/hal/mbox0_hal.c`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/mbox0_ll.h`
- `bk_avdk_smp/ap/middleware/driver/mailbox/mailbox_channel.c`
- `bk_avdk_smp/ap/include/driver/mailbox_channel.h`

CPU1的逻辑身份固定：

```c
#define SELF_CPU MAILBOX_CPU1
```

PWC是CPU1到CPU0的逻辑channel：

- `bk_avdk_smp/ap/include/driver/mailbox_channel.h:47-100`

### 7.2.1 CPU1 IPC启动握手

CPU0在启动CPU1后不会只等待`PM_CPU1_BOOT_READY_CMD`。CP侧`mb_ipc_heartbeat.c`先等待CPU1通过`MB_CHNL_HW_CTRL`发送IPC power-up indication，再接受后续heartbeat；缺少该消息时会出现：

```text
IPC retry to start core1, retry_cnt:N
```

协议必须保持与Armino `ap/middleware/driver/mailbox/mb_ipc_cmd.h`一致：

| 消息 | command | 方向 | logical channel | 参数 |
| --- | ---: | --- | ---: | --- |
| `IPC_CPU1_POWER_UP_INDICATION` | `1` | CPU1 -> CPU0 | `0x10` (`MB_CHNL_HW_CTRL`) | payload地址=`0x2809f900`，长度=0 |
| `IPC_CPU1_HEART_BEAT_INDICATION` | `2` | CPU1 -> CPU0 | `0x10` (`MB_CHNL_HW_CTRL`) | payload地址=`0x2809f900`，长度=4，值=0 |

CPU1在Mailbox transport link ready后必须先发送command 1；heartbeat服务随后每2秒发送command 2。两条消息使用标准16字节logical command结构和原厂SWAP共享buffer envelope，发送端等待最多600 ms transport/semantic ACK并要求`ack_state=ACK_STATE_COMPLETE(2)`。IPC power-up和heartbeat是CPU基础存活契约，不能由PWC ready或PWC自定义command替代。

当前NuttX配置为`CONFIG_SMP=y`且未启用`CONFIG_BMP`。`DEFINE_PER_CPU_BMP`因此退化为
全局单实例，生成map中的`g_system_ticks`和`g_wdactivelist`各只有一个对象。系统保持
logical CPU0唯一SysTick并只在该核调用完整`nxsched_process_timer()`；不得在logical CPU1
重复调用该完整入口，否则会重复推进全局tick、watchdog及RR记账。heartbeat不再使用
未经实板确认的AON RTC counter，而由CPU0-only mailbox TX worker依据全局NuttX tick
建立2秒deadline；command 1之后的第一帧command 2同样等待完整2秒协议周期。

late bring-up线程默认affinity包含两个AP CPU，不能依靠`DEBUGASSERT`保证私有SysTick
寄存器写发生在logical CPU0，也不能在CPU2 exception-return窗口通过setaffinity或SMP call
迁移该线程；实板已证明这会让CPU2停在boottrace secondary stage 10。当前在任何worker
创建和父线程setaffinity之前只读轮询`secondary >= 12`，再让`mbox-v2`从首条指令起继承
CPU0-only affinity并直接启动CPU0 SysTick。timer函数运行时再次检查CPU编号，但不发送IPI。
cmd1 ACK后的heartbeat enable/deadline由CPU0 mailbox完成回调发布。所有physical CP
descriptor仍只能由该CPU0 worker发送。AON RTC alarm、深睡唤醒和从核sleep/RR路径仍属
待实板验证边界，不能写成当前已完成能力。

当前职责固定为：HW_CTRL command 1触发CP `CORE_STARTING -> CORE_POWER_ON`，command 2刷新原厂heartbeat；PWC `0x5`只表示OpenVela SMP/PWC worker可服务，PWC `0x11`只表示480 MHz、CPU2和PSRAM ownership最终commit或rollback。已废止的PWC `0x12`迁移方案不得恢复。

### 7.2.2 当前mailbox实现的已知缺口

以下缺口中，部分已在后续开发中修复，部分仍待完善。当前代码尚未达到完整mailbox v2 transport：

1. **RX指针校验窗口**（已修复）：当前代码已将 CP envelope 校验窗口改为 `0x28064000 <= pointer < 0x2809f700`，覆盖 CP SRAM 范围。Wi-Fi 等子系统的跨核链表逐节点 pointer/length/num 校验仍有缺口，见 `docs/plans/BK7258_OPENVELA_WIFI_PORTING_PLAN.md` 第 3.3 节。

2. **IPC TX生命周期**（已修复）：HW_CTRL、PWC、Wi-Fi和UART均进入同一logical transport；每个channel先复制到pending，单个active transaction持有稳定16字节message直到ACK。HW_CTRL另外用mutex覆盖message准备、发送和等待，4字节heartbeat payload固定在原厂SWAP地址`0x2809f900`，不会与UART `0x2809fc00/0x2809fd00`覆盖。

3. **Transport ACK消费**（已修复）：当前实现校验channel、sequence、command、control和state，并使用8个稳定ACK slot、timeout和reset epoch。active的busy/order必须在发布physical descriptor前建立，发送失败再回滚，避免CP快速ACK在`bk7258_mbox_send()`返回前被误判为stale。

4. **worker_ready等待**（已修复）：Mailbox、HW_CTRL heartbeat和PWC worker均检查`kthread_create()`并使用200 ms或1 s有界semaphore handshake；失败返回errno，不再无界`sched_yield()`。

5. **mailbox IRQ注册时序**（已修复）：early serial阶段完成logical init、physical MBOX IRQ和console注册；board late只启动workers、完成UART STATE link、HW_CTRL liveness和PWC服务。

### 7.3 PWC方向和命令服务

PWC命令定义：

- `bk_avdk_smp/ap/include/driver/pwr_clk.h:26-43`

PWC不能按“CPU1收到后执行”统一描述。CP/AP两侧源码表明power/clock/frequency主要是AP向CP请求，由CP执行系统硬件动作并返回；ready由AP发送；PSRAM/recovery存在CP向AP通知。编码前必须为每个命令从两侧调用点生成完整协议表，至少包含request direction、执行主体、3个参数、transport ACK、semantic response、timeout和recovery。已确认的主方向：

| 命令 | 主请求方向 | 执行主体/CPU1职责 |
| --- | --- | --- |
| `PM_POWER_CTRL_CMD` | AP -> CP | CP执行系统电源操作；AP等待并处理响应 |
| `PM_CLK_CTRL_CMD` | AP -> CP | CP执行主时钟操作；AP处理响应 |
| `PM_SLEEP_CTRL_CMD` | AP -> CP | CP协调睡眠；AP侧状态机配合 |
| `PM_CPU_FREQ_CTRL_CMD` | AP -> CP | CP执行频率操作；首版AP固定频率并拒绝未支持的DVFS |
| `PM_CPU1_BOOT_READY_CMD` | AP -> CP | AP只发送一次，CP记录ready |
| `PM_CTRL_PSRAM_POWER_CMD` | 双向/状态通知 | AP不得重复执行与CP冲突的PHY初始化；按golden职责完成AP静态区域和独立allocator状态管理 |
| `PM_CP1_PSRAM_MALLOC_STATE_CMD` | CP -> AP查询为主 | AP返回受控PSRAM allocator使用量 |
| `PM_CP1_DUMP_PSRAM_MALLOC_INFO_CMD` | CP -> AP | AP投递worker输出受控分配状态 |
| `PM_CP1_RECOVERY_CMD` | CP -> AP | AP quiesce后返回状态 |
| 其他sleep/wakeup/AP状态命令 | 逐调用点确认 | ISR仅ACK/入队，worker执行语义动作 |

原AP PWC RX handler已给出命令处理边界：

- `bk_avdk_smp/ap/middleware/driver/pwr_clk/pwr_clk.c:288-439`

openvela实现必须保持同样的分层：

```text
mailbox ISR
  -> 校验transport和命令
  -> 完成必要ACK
  -> 投递PWC消息到高优先级PM worker
  -> PM worker执行电源/时钟/PSRAM/recovery动作
  -> 发送语义响应
```

ISR禁止执行PSRAM初始化、频率切换、文件系统操作、阻塞等待或复杂日志输出。

### 7.4 WDT、heartbeat和Flash代理

BK7258硬件WDT由CP管理，AP没有可独占的本地硬件watchdog。openvela AP必须实现的是：

- 周期性AP heartbeat，默认协议和超时时间以当前CP实现为准。
- 通过mailbox请求CP执行全系统reboot。
- heartbeat丢失、mailbox失联和AP recovery的状态处理。
- 如向NuttX注册watchdog lower-half，必须明确标记为CP远端代理，不能直接访问WDT寄存器或声称提供独立AP硬件watchdog。

AP对Flash的能力也必须拆分：CPU1可从AP code分区XIP读取和执行，但Flash擦除、编程、配置写入和OTA必须通过CP/mailbox代理。当前OpenVela尚未实现该Flash proxy；后续MTD实现应是mailbox-backed Flash proxy，并至少校验分区白名单、地址溢出、擦除粒度、XIP冲突、超时和断电恢复；不得照搬单核平台的直接Flash controller lower-half。

### 7.5 Ready时序

ready必须在以下条件全部满足后发送：

- runtime vector已经安装。
- MPU已经生效。
- MBOX0物理层已经初始化。
- CPU1 RX FIFO已清空。
- NuttX IRQ79已经注册，NVIC line63和32-63 gate已经使能。
- `MB_CHNL_PWC`已经建立等价的RX/ACK状态机；当前代码已实现固定channel分发和基础ACK匹配，generic channel状态机和完整语义响应仍有缺口。
- CPU1->CPU0 `MB_CHNL_HW_CTRL`已经建立等价channel状态机，IPC power-up indication已经收到`ack_state=2`的原厂完成ACK。
- IPC heartbeat worker已经运行，并按2秒周期发送heartbeat。
- NuttX heap、IRQ和clock基础设施已经初始化。
- NuttX调度器已运行，PM worker线程已真正执行队列初始化并置位 `worker_ready`，board bring-up以有界等待确认，而不是仅检查 `kthread_create()`成功。
- `ready_sent`状态保证同一次boot只发送一次。

ready发送内容固定为：

```text
cmd    = PM_CPU1_BOOT_READY_CMD (0x5)
param1 = 1
param2 = 0
param3 = 0
```

CP OpenVela boot transaction当前给AP 10秒等待窗口；IPC power-up仍由原厂heartbeat task按2秒间隔等待并输出retry诊断。两者职责独立：

- `bk_avdk_smp/cp/middleware/driver/pwr_clk/pwr_clk.c:341-371`

目标仍是冷启动ready P99小于200 ms，为CP窗口保留足够裕量；这不是CP当前硬编码超时值。该指标尚未完成实板统计，当前只能确认离线构建和代码路径。

## 8. 链接脚本

### 8.1 MEMORY

链接脚本从目标项目生成header取得值。基础`app_ab`当前示例值为：

```ld
FLASH (rx) : ORIGIN = 0x02150000, LENGTH = 0x00110000
RAM   (rwx): ORIGIN = 0x28010000, LENGTH = 0x00054000
```

同时定义：

```ld
__ap_ram_start = 0x28010000;
__ap_ram_end   = 0x28064000;
```

切换到AI项目时这些值必须全部由对应项目重新生成，不能继续使用本节示例常量；链接断言也必须以生成宏而非固定数值为准。

### 8.2 sections

必须包含：

- `.bk_boot_vectors`：镜像offset 0，512字节对齐。
- `.gnu.sgstubs`：32字节对齐，Secure build使用。
- `.text`、`.rodata`、`.init_array`。
- `.ARM.extab`、`.ARM.exidx`。
- `.data > RAM AT > FLASH`及 `_sdata/_edata/_eronly`。
- `.bss > RAM`及 `_sbss/_ebss`。
- `.ram_vectors > RAM AT > FLASH`，512字节对齐。
- 首版不生成 `.ramfunc`；后续确需使用时必须建立独立可执行MPU region。
- `__idle_stack_top`和中断栈。
- openvela heap结束于 `__ap_ram_end`。

### 8.3 链接断言

必须加入：

```ld
ASSERT(ORIGIN(FLASH) == (SOC_FLASH_DATA_BASE + CONFIG_AP_VIRTUAL_PARTITION_OFFSET),
       "AP flash origin mismatch")
ASSERT(ORIGIN(RAM) == CONFIG_AP_RAM_ADDR, "AP RAM origin mismatch")
ASSERT((ADDR(.bk_boot_vectors) & 0x1ff) == 0, "boot vector alignment")
ASSERT((ADDR(.ram_vectors) & 0x1ff) == 0, "runtime vector alignment")
ASSERT(_ebss <= __ap_ram_end, "AP bss overlaps CP RAM")
ASSERT(__heap_start < __heap_end, "AP heap is empty")
ASSERT(__heap_end <= __ap_ram_end, "AP heap overlaps CP RAM")
```

构建脚本必须拒绝以下alloc section：

- `.psram.data`
- `.psram.bss`
- `.itcm`，除非有明确ITCM映射和复制表
- `.dtcm`，除非有明确DTCM映射和复制表
- CP共享区中未经批准的section

## 9. openvela配置

以下是正式目标配置，不是临时bring-up配置：

```text
CONFIG_ARCH="arm"
CONFIG_ARCH_CHIP_CUSTOM=y
CONFIG_ARCH_CHIP_ARM_CUSTOM=y
CONFIG_ARCH_CHIP_BK7258=y
CONFIG_ARCH_CHIP_CUSTOM_DIR="../vendor/beken/chips/bk7258"
CONFIG_ARCH_CHIP_CUSTOM_DIR_RELPATH=y
CONFIG_ARCH_CORTEXM33=y
CONFIG_ARCH_BOARD_CUSTOM=y
CONFIG_ARCH_BOARD_CUSTOM_NAME="bk7258-ap"
CONFIG_ARCH_BOARD_CUSTOM_DIR="../vendor/beken/boards/bk7258/bk7258-ap"
CONFIG_ARCH_BOARD_CUSTOM_DIR_RELPATH=y
CONFIG_BUILD_FLAT=y

CONFIG_ARCH_TRUSTZONE_SECURE=y
CONFIG_ARMV8M_CMSE=y
CONFIG_ARCH_FPU=y
CONFIG_ARM_MPU=y
CONFIG_ARM_MPU_NREGIONS=8
CONFIG_ARM_MPU_RESET=y
# CONFIG_ARM_DSP is not set

CONFIG_ARCH_INTERRUPTSTACK=2048
CONFIG_IDLETHREAD_STACKSIZE=4096
CONFIG_DEFAULT_TASK_STACKSIZE=4096
CONFIG_RAM_START=0x28010000
CONFIG_RAM_SIZE=344064

CONFIG_TIMER=y
CONFIG_ARMV8M_SYSTICK=y
CONFIG_USEC_PER_TICK=1000
CONFIG_RR_INTERVAL=200

# 正式目标不启用物理UART1 console；AP日志通过Mailbox转发到CP UART0。

CONFIG_BOARD_EARLY_INITIALIZE=y
CONFIG_BOARD_LATE_INITIALIZE=y
CONFIG_BOARD_INITTHREAD_PRIORITY=100
CONFIG_BOARD_INITTHREAD_STACKSIZE=4096
CONFIG_NSH_ARCHINIT=y

CONFIG_BUILTIN=y
CONFIG_INIT_ENTRYPOINT="nsh_main"
CONFIG_SYSTEM_NSH=y
CONFIG_NSH_READLINE=y
CONFIG_NSH_FILEIOSIZE=512
CONFIG_RAW_BINARY=y

# CONFIG_SMP is not set
# CONFIG_SCHED_TICKLESS is not set
# CONFIG_ARCH_RAMVECTORS is not set
```

`ARCH_HAVE_CUSTOM_VECTORS`和 `ARCH_HAVE_FPU`由芯片Kconfig选择，不依赖defconfig手写；仅有hard-float编译参数不足以启用NuttX线程FPU上下文。`ARCH_CORTEXM33`、`ARCH_FPU`、`ARCH_TRUSTZONE_SECURE`、`ARM_MPU`、实际 `ARM_MPU_NREGIONS`、`ARMV8M_SYSTICK`和board hooks必须由最终`.config`核验。`ARMV8M_SYSTICK`依赖`CONFIG_TIMER`，见`openvela/nuttx/arch/arm/src/armv8-m/Kconfig:131-136`。必须确认最终配置没有`CONFIG_ARCH_ARM64`、GIC、MMU和SMP。

当前`contest/vendor/beken/boards/bk7258/bk7258-ap/configs/nsh/defconfig`仍包含`CONFIG_UART1_SERIALDRIVER`、`CONFIG_UART1_SERIAL_CONSOLE`和UART1 buffer配置，这与正式Mailbox UART目标不一致。当前构建成功只证明临时UART1配置可链接；在正式AP日志转发完成后必须删除这些UART1 console配置，并重新验证NSH输入输出路径。

## 10. Board初始化和服务启动

### 10.1 board early

AP不配置GPIO0/1到UART1。若需要early polling日志，则在 `bk7258_start.c`/chip early阶段使用受限Mailbox transport；`board_early_initialize()`只执行其后的非阻塞板级动作：

- 校验GPIO0/1未被AP错误切换为UART1，不把board early作为首次pinmux点。
- 完成不影响early console的其余板级GPIO配置。
- 必要的CPU1 GPIO状态。
- 不启动USB、Wi-Fi、媒体和CPU2。

不得在此阶段等待semaphore、访问文件系统或启动复杂服务。openvela文档明确early hook位于核心组件完成前：

- `openvela/docs/zh-cn/device_dev_guide/kernel/boot_process.md:37-45`

### 10.2 board late

正式目标中`board_late_initialize()`应执行：

- 启动PWC PM worker并在其可运行后发送CPU1 ready。
- 启动IPC heartbeat和PWC worker；诊断和错误统计线程仍待实现。
- 发送CPU1 ready前确认transport状态。
- NuttX基础设备由公共启动流程初始化，board late只启动板级服务。

当前实现的board late仅调用`bk7258_pwc_start()`，尚未启动Mailbox UART日志转发服务。当前UART1仍在`bk7258_serial.c`注册为`/dev/console`，这是临时bring-up行为，与正式目标不一致。

### 10.3 board app initialize

正式目标中`board_app_initialize()`应执行：

- Mailbox UART console/NSH输入输出验证。
- procfs、ROMFS和NSH依赖初始化。
- 后续启用独立PSRAM allocator，但必须先完成向CP请求上电并收到成功的semantic response。

不在首版启动任何要求CPU2、Wi-Fi、BT、媒体或AP SMP的服务。当前`bk7258_boot.c`中的`board_early_initialize()`和`board_app_initialize()`仍为空实现，本文列出的board app工作均未完成。

## 11. 构建和正式打包

### 11.1 两个独立工程

openvela AP和BK CP保持独立构建：

```text
openvela
  -> BK7258 CPU1 SPE配置的ELF/map/raw binary

bk_avdk_smp
  -> bootloader
  -> CPU0 CP app.bin
  -> partitions_gen.h / ram_regions.h / bk_package.json
  -> all-app.bin
```

不把openvela源文件加入 `bk_avdk_smp/ap` 的CMake。这样可以避免两套startup、libc wrapper、heap、SVC、PendSV和SysTick重复定义。

### 11.2 正式AP image provider

现有打包链在 `copy_binaries_to_pack_dir()`中按照 `apps_info`复制CP/AP：

- `bk_avdk_smp/tools/build_tools/build_process/bk_project.py:145-159`
- `bk_avdk_smp/tools/build_tools/build_process/bk_sdk/bk_sdk_project.py:162-175`

正式改造目标及当前完成度：

1. 已增加`EXTERNAL_AP_BIN`；`EXTERNAL_AP_ELF`和metadata参数尚未实现。
2. `bk_sdk_project.py`将AP role的 `app_info.build_bin`直接设为外部输入；不要依赖并不存在或未经证实的字符串条件猜测role。
3. CP仍使用正常 `app.bin`。
4. 当前只检查外部AP raw存在且非空；ELF属性、分区大小、hash和build metadata自动检查尚未实现。
5. `copy_binaries_to_pack_dir()`直接把外部AP raw复制到packager需要的AP firmware文件名。
6. 禁止先复制原AP再覆盖临时文件。
7. `bk_build_package.py`继续调用标准packager，不改变最终包格式。

当前packager执行顺序是先copy再pack：

- `bk_avdk_smp/tools/build_tools/build_process/bk_build_package.py:69-80`

packager从`bk_package.json`的`firmware`字段读取输入文件：

- `bk_avdk_smp/tools/env_tools/bk_py_libs/bk_packager/bk_packager_json.py:44-68`

因此正式接口必须以生成的JSON和AP role为准，不能硬编码`app1.bin`作为不可变协议。当前工程通常将AP映射为`app1.bin`，但该名称由 `bk_sdk_project.py`产生。

### 11.3 当前可执行构建命令

当前仓库尚未实现名为`bk7258_openvela`的统一make target。实际命令分为两步：先构建OpenVela AP，再用Podman运行ARMINO官方镜像构建`app_ab`并注入外部AP：

```bash
cd /home/mi/vela_competition/contest

./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  -e -Werror \
  --cmake \
  -j8

cp cmake_out/bk7258-ap_nsh/nuttx.bin \
  /home/mi/vela_competition/bk_avdk_smp/build/openvela-ap.bin

cd /home/mi/vela_competition/bk_avdk_smp
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

该命令使用本地镜像`localhost/bekencorp/armino-idk:1.5`。`EXTERNAL_AP_BIN`必须使用容器内路径`/armino/build/openvela-ap.bin`，不能使用宿主机绝对路径。不要使用`./dbuild.sh`，因为该脚本内部调用`docker`，仅安装Podman时会直接失败。构建生成：

```text
projects/app_ab/build/bk7258/app_ab/package/all-app.bin
projects/app_ab/build/bk7258/app_ab/package/tmp/bootloader.bin
projects/app_ab/build/bk7258/app_ab/package/tmp/app.bin
projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
```

最近一次AP、CP clean、`-Werror`构建和标准打包结果：OpenVela AP raw与`tmp/app1.bin`均为298304 bytes，SHA-256为`e9a390dd9e3111193ac6c98692f62c8cda12a69081564c1a49f59346f4fd81ed`；最终`all-app.bin` SHA-256为`69c271fe38c59f9c7fde6f491b298665589bdc3e82c2f2f6c8b347aa3eb34b9b`，`app_ab_crc.rbl` SHA-256为`ebd4ab8638d5e25652f648783fa2b4691ad32023279ee333c467aaac76528c52`。这些是离线产物校验，不代表已完成本轮原厂HW_CTRL liveness实板验收。

### 11.4 正式构建目标

目标依赖关系：

```text
make bk7258_openvela
  -> generate partitions and ram_regions
  -> build original CPU0 CP
  -> build openvela CPU1 AP
  -> validate AP ELF/raw
  -> external AP image provider
  -> package all-app.bin
  -> validate final package
```

原 `bk7258_ap`目标保留为golden/reference构建，但不作为openvela package依赖。这样可以在寄存器、启动向量和mailbox联调时随时生成原AP进行对照。

### 11.5 自动门禁

正式构建必须失败于以下任一条件。当前这些检查尚未全部自动化；现阶段只完成了raw/staging hash人工比对，因此本节是待实现门禁而不是当前构建能力：

- `readelf -A`和反汇编不能证明是ARMv8-M Mainline Thumb、Cortex-M33。
- CPU属性不是Cortex-M33、hard-float、FPv5-SP-D16。
- 出现DSP指令属性。
- image offset 0的初始SP不在 `0x28010000..0x28064000`。
- reset vectorbit0未置位、地址不落入AP Flash。仅凭地址和bit0不能证明Secure state，Secure必须靠寄存器读回和fault测试。
- offset `0x100/0x104`两个word不是 `0x32374b42/0x00003633`。
- runtime vector未按512字节对齐。
- `.data/.bss/.ramfunc/stack/heap`越过 `0x28064000`。
- 出现未经批准的PSRAM或CP RAM alloc section。
- AP logical raw超过虚拟AP分区 `0x110000`，或 `ceil(raw_size / 32) * 34`超过物理分区 `0x121000`。
- CP/bootloader hash与golden不一致。
- final package解码后的AP内容与openvela raw不一致。

## 12. 实施步骤和验收

### 阶段一：固化golden

构建原 `projects/app_ab`，保存：

- CPU0 CP ELF/map/raw。
- 原AP ELF/map/raw。
- bootloader和 `bk_package.json`。
- `all-app.bin`。
- CPU1启动日志、ready耗时和mailbox v2 trace。

必须完成100次冷启动，记录CPU1启动到ready的延迟分布。

### 阶段二：CPU1按SPE基线启动

实现：

- Secure hard-float toolchain。
- Flash boot vector。
- RAM runtime vector。
- C runtime。
- MPU。
- FPU。
- Mailbox early log transport，由CP UART0输出。

验收：

- CPU1能从当前目标项目生成的AP XIP origin取vector；基础`app_ab`才使用`0x02150000`。
- MSP位于AP RAM。
- VTOR最终位于RAM runtime vector。
- 无HardFault、BusFault、MemManageFault或SecureFault。
- 100次CPU1启动无随机失败。
- 导出并对比golden/openvela的安全状态、SAU和PPC/MPC关键寄存器；在完成前只称“SPE功能基线”，不称“可信启动”。

### 阶段三：完整mailbox v2

实现并测试：

- MBOX0 FIFO。
- CPU1 endpoint。
- NVIC外部line63、NuttX IRQ79和32-63 gate。
- 共享buffer pointer/length。
- logical channel、sequence、ACK、reset。
- PWC transport。
- CPU1 IPC power-up indication和2秒heartbeat。

当前状态：基础transport、IPC power-up和heartbeat已经编译进AP；完整logical channel ACK/reset/busy恢复、10000次压力和复位重同步尚未完成实板验收。

验收：

- CPU0/CPU1连续完成10000次command/ACK。
- 无FIFO full、busy泄漏、sequence错乱和共享buffer覆盖。
- CPU1复位后能够清理旧状态并重新同步。

### 阶段四：NuttX内核和PWC worker

接入：

- ARMv8-M公共异常和上下文切换。
- NuttX IRQ。
- heap。
- SysTick。
- PWC PM worker。
- ready发送。
- CP启动契约所需的IPC power-up/heartbeat。

当前状态：AP已全量链接，修复包已离线生成；最近一次实板日志仍来自修复前包，尚未证明修复后CP不再重试。

验收：

- ready P99小于200 ms。
- CP不发生启动重试。
- CP发送PWC命令后，CPU1都能完成transport ACK和语义响应。
- 强制执行CP recovery时，CPU1先quiesce再允许CPU0断电。

### 阶段五：Mailbox UART日志转发和NSH

实现并验收AP到CP的Mailbox UART日志转发，启动NSH并通过CP UART0观察。当前已实现基础日志转发和NSH交互，完整压力验证和reset恢复仍有缺口：

```text
help
ps
uptime
free
```

验收：

- AP日志经Mailbox转发后在CPU0 UART0连续稳定输出。
- CPU0 UART0可独立输出CP日志和烧录通信。
- 高并发日志不破坏mailbox。
- 两个浮点线程上下文切换结果正确。

### 阶段六：PSRAM

完成：

- CP power-on通知。
- CPU1 PSRAM状态机。
- 独立PSRAM allocator启用、冻结和拒绝断电机制。
- PSRAM使用量统计。
- power-off前清退。
- recovery和重新启动。

PSRAM在这一步仍保持non-cacheable。power-off时只在outstanding为0时确认，否则返回busy，不尝试注销通用heap。完成72小时压力、掉电恢复和AP recovery测试后，才评估I-cache/D-cache。

### 阶段七：正式package

完成 `EXTERNAL_AP_BIN` provider、ELF/raw/package三层校验和CI构建。当前provider和raw/staging/package基本链路已验证，但统一一键target、空目录可重复构建和最终包解码门禁仍未实现为自动化检查。

验收：

- 一条命令生成最终 `all-app.bin`。
- 空构建目录可重复构建。
- AP image变化不会改变bootloader和CP分区。
- 同一源码和配置重复构建的关键产物一致。
- 现有烧录工具可正常烧录和启动。

## 13. `bk_solution_ai`交叉验证和多项目布局

`bk_solution_ai`是依赖`bk_avdk_smp`的外置AI解决方案仓库，不包含BK7258 startup、mailbox底层、bootloader、packager或芯片基础静态库。它不能替代SMP SDK，但提供了正式产品项目的资源需求和构建边界。

### 13.1 新增项目和组件

当前可见项目包括：

- `beken_genie`：Agora RTC、音频处理、AI Agent和双屏外设方案。
- `volc_rtc`：火山RTC实时音视频和AI Agent方案。
- `volc_rtc_ab`：带AB布局的火山RTC工程。
- `ai_camera`：仍在开发中的AI Camera工程。

新增上层组件包括`audio_engine`、`video_engine`、`network_transfer`、`bk_app_event`、`bk_smart_config`、`bk_factory_config`、`bk_key_app`、`bk_led_blink`、`bk_motor`、`bk_countdown`和`bk_dual_screen_avi_player`。这些组件属于后续应用移植范围，不是内核bring-up依赖。

### 13.2 正式AP SMP证据

AI项目的AP配置明确包含：

```text
CONFIG_CPU_CNT=2
CONFIG_SOC_STR="bk7258_ap"
CONFIG_SOC_SMP=y
```

CP入口也明确通过`bk_pm_module_vote_boot_cp1_ctrl()`启动`smp(cpu1, cpu2)`。因此CPU1-only只能作为bring-up路径；正式兼容目标必须验证CPU2释放、逻辑/物理core ID映射、SMP调度、spinlock、每核NVIC/cache/TCM以及外设中断owner。

### 13.3 项目相关的SRAM布局

AI项目在`AP_SPINLOCK`之后增加`HARDWARE_ACC`：

| 区域 | 大小 |
| --- | ---: |
| `AP_SPINLOCK` | `0x010000` |
| `HARDWARE_ACC` | `0x01b000` |
| `AP_RAM` | `0x043c00` |
| `CP_RAM` | `0x030b00` |
| `PWR_MNG` | `0x000100` |
| `SWAP` | `0x000800` |

按顺序累计，AI项目的AP RAM为：

```text
AP_RAM_START = 0x2802b000
AP_RAM_SIZE  = 0x00043c00
AP_RAM_END   = 0x2806ec00
```

这与基础`app_ab`的`0x28010000/0x54000`不同。openvela chip port只提供地址生成和校验机制；具体board/config必须绑定目标项目生成的layout。`HARDWARE_ACC`、spinlock、CP RAM、PWR_MNG和SWAP都不得进入openvela heap或`.bss`。

### 13.4 AI项目PSRAM和镜像规模

AI项目配置16 MB PSRAM，并显著扩大`AP_PSRAM_HEAP`和`AP_PSRAM_SECTION`。音频、ASR、RTC和视频组件明确依赖PSRAM，因此SRAM-only版本只能作为内核/NSH验收，不代表可承载AI方案。

非AB AI项目通常将AP物理code分区扩大到约2.8 MB，而基础`app_ab`的AP分区约1.1 MB。AP XIP起点、逻辑容量和CRC物理容量必须由各项目分区重新计算，不能复用基础项目的`0x02150000/0x110000`。

`volc_rtc_ab`的分区尺寸在使用前必须运行分区生成器验证总容量和固定尾部分区是否重叠；示例CSV不能未经构建校验直接作为openvela打包模板。

### 13.5 预编译库接入门禁

已复制到dev目录的BK7258 AP预编译库只作为后续适配输入，首版内核bring-up默认不链接音频、蓝牙、图像、编码或RTC库。每个`.a`启用前必须检查：

```text
nm -u        # FreeRTOS/Armino OS和其他未定义符号
readelf -S   # .itcm/.dtcm/.psram及固定alloc section
readelf -A   # Cortex-M33、hard-float、FPv5-SP-D16和DSP属性
```

只有OS ABI、链接section、工具链属性、许可证和运行时资源全部闭环后，才按功能逐项加入Make/CMake；不得通配链接整个`libs/`目录。

## 14. 故障定位顺序

### CPU1无任何输出

检查：

1. CP写入的boot address是否为生成分区对应值。
2. AP image offset 0的SP和reset vector。
3. Secure alias是否与当前目标项目生成的AP XIP origin一致；`0x02150000`仅是基础`app_ab`示例，不能套用于AI项目。
4. image offset `0x100` magic。
5. CPU1 power-down、reset和RXEVT寄存器。
6. CPU1 JTAG选择：`setjtagmode cpu1 group1`。

### CPU1启动后立即fault

检查：

1. FPU CP10/CP11是否在C代码前启用。
2. MPU是否覆盖Flash、AP SRAM、PWR_MNG和外设。
3. VTOR是否已切换到完整RAM vector。
4. runtime vector是否被magic污染。
5. SecureFault状态和SAU/IDAU保持是否与CPU0/原CPU1一致。

### CP等待超时

检查：

1. MBOX0 v2初始化是否完成，尤其是channel 1 FIFO start=2、length=3。
2. CPU1 endpoint是否为 `MAILBOX_CPU1`。
3. NuttX IRQ79、NVIC line63和32-63 enable bank bit31是否打开。
4. `MB_CHNL_HW_CTRL` logical channel `0x10`是否发送IPC power-up command 1。
5. IPC heartbeat command 2是否按2秒周期发送。
6. `MB_CHNL_PWC`逻辑channel source/destination是否正确。
7. pointer/length envelope指向的SRAM范围是否被RX校验接受（当前代码已接受CP SRAM窗口 `0x28064000..0x2809f700`；若仍被拒绝，见7.2.2）。
8. command/ACK sequence是否匹配。
9. ready是否在IPC power-up、heartbeat和PM worker可用后发送。
10. `bk7258_mbox_send_message()` 的单buffer是否被连续发送覆盖（见7.2.2）。

### ready后CP命令超时

检查：

1. mailbox ISR是否循环drain RX FIFO。
2. transport ACK是否及时完成。
3. PWC命令是否投递给PM worker。
4. PM worker是否在等待同一个mailbox IRQ或锁，形成死锁。
5. PSRAM power/malloc/recovery是否按原命令语义返回。
6. RX指针校验是否意外拒绝了CP SRAM范围内的合法消息（此问题曾存在，已修复，见7.2.2）。
7. 发送端是否消费了transport ACK，还是仅发送后立即返回（见7.2.2）。

### NSH无输入

检查：

1. GPIO0/1是否仍被AP I2C1占用。
2. Mailbox UART逻辑通道、缓冲区和流控。
3. AP日志是否通过Mailbox转发到CP UART0。
4. CP UART0是否持续输出转发后的AP日志。
5. AP未注册物理UART1 console，CP UART0仍是唯一物理日志输出口。

## 15. 缺失资料、安全可行性和门禁

### 15.1 当前可行范围

资料缺失时仍可推进“BK7258 CPU1 openvela功能性移植”：保留当前未完成secure-boot认证闭环的启动包和现有CP/bootloader，不烧OTP/eFuse，只替换AP输入；完成vector、NuttX、UART、MPU收紧、mailbox/PWC和recovery。该构建仍使用Secure alias和CMSE，但不承诺secure boot、镜像机密性、防回滚和恶意跨核隔离，应标记为 `development build without secure-boot assurance`，避免与Arm Non-secure execution state混淆。

当前不能声称：CRC等价于签名；`CONFIG_SPE=1`或Secure地址证明镜像可信；MPU等价于MMU进程隔离；mailbox sequence/ACK提供密码学认证；ready是attestation；仓库默认key可量产；`primary_all`一定覆盖CPU1；存在AES/OTP API就表示Flash加密和量产熔丝流程可用。

### 15.2 安全路线可行性重新论证

从源码看，量产安全不是“硬件完全不支持”，而是“具备工具和配置框架，但目标芯片交付链尚未被证明”：

| 能力 | 证据 | 可行性结论 |
| --- | --- | --- |
| TrustZone-M/SPE/CMSE | BK7258 Kconfig、CPU1/AP defconfig、`-mcmse`、`TZ_SAU_Setup()` | 功能可研究；初始状态和最终权限需实测/TRM |
| MPU | BK7258 `mpu_cfg.c`和CM33 MPU代码 | 可实现属性/XN和AP边界；不能替代PPC/MPC、DMA隔离或进程MMU |
| PPC/MPC生成 | BK7258 `ppc.csv`、`mpc.csv`、`gen_ppc.py` | 工具存在；默认表过宽且缺最小权限验证 |
| 签名/聚合镜像 | `partition.py`、`pack.py`、`bl2_sign.py` | 工具可能支持聚合CPU镜像；必须证明CPU1字节进入签名范围 |
| OTP/security counter/HUK | `otp1.csv`、`otp_struct.h`、OTP工具 | 数据模型存在；烧录极性、锁定、LCS和掉电原子性未知 |
| Flash AES | `security.csv`和pack工具 | 当前默认关闭；key slot、XIP/DMA/OTA边界未知 |
| mailbox安全 | source/destination、sequence、ACK | 仅功能鲁棒协议，无HMAC/nonce/attestation；必须加指针窗口和硬件权限验证 |

因此安全路线在获得原厂资料后具有继续验证的工程可行性，但当前没有足够证据承诺量产。TF-A/BL31/EL3对Cortex-M33不适用；若引入安全固件，应评估TF-M及BK7258平台端口。

### 15.3 必须向原厂索取

1. BK7258对应芯片revision的TRM和安全附录：CPU1 reset/clock/boot、SAU/IDAU、PPC/MPC/PRRO、DMA master安全归属、MBOX source保证、cache和barrier。
2. ROM BL1、BL2/MCUboot、可选TF-M的正式版本和安全分区示例。
3. CPU1/AP认证规范：独立image还是`primary_all`子镜像、manifest/TLV/hash范围、失败后CPU0是否仍可释放CPU1。
4. OTP/eFuse provisioning和生命周期说明：bit极性、锁定、counter、掉电恢复、JTAG/SWD/下载口和RMA策略。
5. Flash AES与安全OTA说明：key slot、地址模式、XIP/DMA、三核版本一致性、anti-rollback和断电恢复。
6. mailbox v2协议：endpoint、FIFO ownership、共享buffer生命周期、cache一致性和跨核reset恢复。
7. EVB原理图和板级资料：UART1 GPIO0/1、UART0下载、JTAG CPU1 group、XTAL、PSRAM和电源配置。

### 15.4 分阶段安全门禁

| 门禁 | 通过条件 | 未通过时允许范围 |
| --- | --- | --- |
| S0 证据基线 | 固定芯片revision和boot版本；获得TRM/安全资料；确认CPU1签名归属 | 仅功能移植 |
| S1 权限基线 | 对比golden/openvela SAU/MPU/PPC/MPC；AP不能写CP RAM、OTP和未授权外设；DMA不能绕过 | 功能测试，不宣称隔离 |
| S2 IPC负向测试 | 越界指针、错误长度/source、重复ACK、旧FIFO和消息风暴均被拒绝并可恢复 | 仅可信核之间功能IPC |
| S3 离线签名闭环 | 从最终输入证明CPU1范围被hash覆盖；CPU1单bit篡改校验失败 | 不烧安全fuse |
| S4 可恢复板验证 | 错key、错签、CPU0/CPU1/TF-M篡改均拒启；CPU0不释放未认证CPU1 | 测试生命周期 |
| S5 AES/回滚/OTA | 错key、旧counter、不完整升级和掉电均有确定安全结果 | 不进入量产 |
| S6 量产评审 | HSM/签名服务、测试/量产key隔离、OTP审计、debug/RMA策略、威胁模型和负向报告 | 通过后方可称量产安全方案 |

任何不可逆OTP/eFuse动作必须在S3离线闭环后，先使用有恢复路径的专用测试板执行；不得用仓库示例key或默认CSV直接进入生产。

## 16. 最终完成定义

完成以下条件，才算完成“openvela 在 BK7258 AP 上的功能性初步内核适配”：

- CPU0/CP源代码和运行行为保持原样。
- CPU1从AP分区按SPE/Cortex-M33基线启动，安全状态和关键权限寄存器已与golden对比。
- CPU1使用BK Flash boot vector和独立RAM runtime vector。
- C runtime、FPU、MPU、IRQ和NuttX ARMv8-M上下文切换正常。
- CPU1+CPU2 NuttX SMP启动、raw Mailbox boot handshake和CPU2 idle入口已经建立；CPU1-only仅保留为诊断路径。
- AP日志目标路径是通过Mailbox转发并由CP UART0输出；当前基础日志转发已实现，完整压力验证和reset恢复仍待验收。
- MBOX0 v2 transport已完成基础实现，但完整压力、复位重同步和错误恢复仍待验收。
- `MB_CHNL_PWC`已具备基础worker路径；所有规定命令的语义响应和recovery仍待完成。
- CPU1已按原厂HW_CTRL ABI实现IPC power-up和2秒heartbeat，并独立发送PWC `0x5/0x11`；CP不再包含PWC `0x12`到legacy heartbeat的翻译层，消除retry仍待新固件实板验证。
- AP SRAM heap严格限制在目标项目生成的`AP_RAM`内，基础`app_ab`当前为`0x28010000..0x28064000`；任何`HARDWARE_ACC`、spinlock、CP RAM、PWR_MNG和SWAP都未被覆盖。
- PSRAM PHY、AP静态区域和heap职责已经按golden启动链确认；CPU1按PWC状态通过独立可冻结allocator使用，不重复执行与CP冲突的PHY初始化。
- AP heartbeat代码路径已实现，但CP远端reboot/watchdog代理和丢包恢复尚未完成实板验证；未把AP描述为拥有独立硬件WDT。
- AP Flash擦写代理尚未实现；当前CPU1只依赖AP分区XIP读取，未把直接Flash controller操作作为可用能力。
- 外部AP image通过正式provider进入BK packager，不覆盖临时文件。
- packager staging中的bootloader、CP和OpenVela AP已经分别记录hash，OpenVela raw与`tmp/app1.bin`一致；最终`all-app.bin`经AB RBL覆盖后的三分区解码比对仍需自动化并重新执行。
- package、启动、mailbox、NSH、recovery和PSRAM状态测试全部通过。
- 交付物明确标注是否仅为 `development build without secure-boot assurance`；未通过第15章S6不得声明量产安全启动。

在单核bring-up验收基础上，CPU2/AP secondary、NuttX SMP、D-cache一致性、Wi-Fi/BT/媒体驱动和完整应用服务作为后续工作流实现。正式AP完成定义必须包含CPU1+CPU2 SMP；切换目标项目时重新生成并验证Flash、AP RAM、硬件加速区和PSRAM布局，不继承基础`app_ab`固定地址。
