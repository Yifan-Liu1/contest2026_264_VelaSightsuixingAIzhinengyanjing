# BK7258 OpenVela LCD QSPI DMA 移植适配计划

> 文档版本：V1
>
> 文档状态：2026-08-20 设计基线，**尚未实现**。当前显示链路从
> framebuffer 到 GC9D01 面板的像素搬运完全由 CPU 逐字 `putreg32()` 完成，
> 仓库内不存在任何 LCD/QSPI 方向的 DMA 代码；`bk7258_dma.c` 目前只有一个
> 使用者（摄像头 JPEG 比特流，通道 0），不是显示链路。本计划为全新移植，
> 不是对已有实现的补充说明。
>
> 适用范围：BK7258 AP/CPU1 上 `board/beken/chips/bk7258/bk7258_qspi.c` 的
> `bk7258_lcd_spi_write_frame()`——LVGL（`app/velasight`）和其他 framebuffer
> 使用者共享的同一条整帧推屏路径。目标是用通用 DMA 引擎替换其中的 CPU
> 拷贝循环。
>
> 改动落在两个 chip 层文件：`bk7258_qspi.c`（主体）和
> `bk7258_dma.c`/`bk7258_dma.h`（补上突发长度支持与 `bk7258_dma_init()`
> 的重复调用保护，两项都是显示路径暴露出来的既有缺口，见第 2.3.2、
> 4.3 节）。LVGL、`app/velasight`、`bk7258_gc9d01_fb.c`、
> `bk7258_gc9d01.c` 全部不改。
>
> **收益边界（第 1.3 节给出完整推导，此处先给结论）**：确定能拿到的是
> CPU 时间片——推屏期间 CPU 从 100% 忙等变成可调度睡眠。挂钟时间是否也
> 变快**取决于一项尚未验证的假设**：实测 25ms 与 60MHz 单线 SPI 的理论
> 下限约 6.8ms 之间存在约 18ms 的差距，如果这个差距来自逐字 AHB 事务
> 开销（而不是面板或控制器的固有速率），那么 DMA 的突发传输
> （原厂设置 `BURST_LEN_INC16`，本 port 现有 DMA 驱动尚未支持该字段）
> 有可能显著缩短它。**这是一个必须实测的开放问题，不是本计划承诺的
> 交付指标**；把它写在最前面是为了既不夸大也不低估 DMA 的价值。

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
/home/mi/vela_competition/contest/nuttx
```

（等价于 `/home/mi/vela_competition/openvela`，两者内容不同但都被工作区
引用；chip 层 `Make.defs`/`CMakeLists.txt` 使用的是 `contest/nuttx` 这一份，
后续所有 NuttX 框架代码引用以它为准。）

原厂 BK7258 参考源码为：

```text
/home/mi/vela_competition/bk_avdk_smp
```

仓库内的 `external/bk_avdk_smp/` 只是 CP 侧裁剪镜像，**不含 LCD/QSPI/DMA
驱动**；所有原厂 LCD DMA 取证必须回到上面的完整 SDK 路径
（`ap/middleware/driver/lcd/lcd_spi_driver.c`）。

执行期间遵守以下安全规则：

1. 在确认 QSPI 数据窗口（`0x64000000`/`0x68000000`）当前落在 ARMv8-M
   默认内存映射（不在本 port 任何显式 MPU 区域内，见第 2.2 节）这一事实
   不会引入缓存不一致之前，禁止把 `bk7258_dma_configure_ex()` 的目的
   地址设为这两个窗口并启动通道；确认方法是核实 D-Cache 全局关闭这一
   现状，而不是假设该窗口本身不可缓存。
2. 任何 DMA 通道号选择必须先核对第 2.4 节的通道占用表，禁止与摄像头 JPEG
   通道（0 号）冲突。
3. 字节序交换（`gc9_swap16x2`）在 DMA 路径下必须有等效替代或已验证的绕开
   方案（第 3.3 节），禁止默默丢弃这一步——错误的字节序在面板上表现为
   颜色错误，不是崩溃，容易被忽略。
4. 不在 DMA 完成中断（`bk7258_dma_isr`）里执行 `printf`、动态内存分配、
   `CASET`/`RASET` 命令发送或任何可能阻塞的调用；这与 `bk7258_dma.c` 现有
   ISR 的既有纪律一致（见该文件头注释）。
5. 在两块面板各自的 DMA 传输完成之前，禁止让 LVGL 的下一次 `flush_cb`
   开始向同一块 framebuffer 写入新内容（第 3.4 节的撕裂风险分析）。
6. 所有生成配置、ELF、map 和测试日志必须记录构建目录、时间、源码状态和 hash。

## 1. 目标与当前状态

### 1.1 目标架构

```text
LVGL flush_cb（app/velasight/vs_display.c，两块 lv_display_t）
        |
        v
ioctl(fd, FBIO_UPDATE, &fb_area)   (CONFIG_FB_UPDATE, 同步 ioctl)
        |
        v
nuttx/drivers/video/fb.c: vtable->updatearea()  （调用者上下文内直接执行）
        |
        v
board/.../bk7258_gc9d01_fb.c: gc9d01_updatearea() -> gc9d01_push()
        |
        v
board/.../bk7258_gc9d01.c: bk7258_gc9d01_window_full()  (CASET/RASET，不变)
        |
        v
board/beken/chips/bk7258/bk7258_qspi.c: bk7258_lcd_spi_write_frame()
        |
        +---- 现状：CPU for 循环，每字 gc9_swap16x2() + putreg32()
        |
        +---- 目标：bk7258_dma_configure_ex() 一次性描述 12800 字的
        |           mem-to-mem 传输，dest_addr = QSPI 数据窗口，
        |           dest_inc = true；CPU 等待完成信号而不是自己搬数据
        |
        v
QSPI0/QSPI1 数据窗口（0x64000000 / 0x68000000）-> GC9D01 GRAM
```

这条链路上的改动点只有两处，都在 chip 层：

1. `bk7258_lcd_spi_write_frame()` 内部的搬运方式（本计划的主体）。
2. `bk7258_dma.c`/`bk7258_dma.h` 增加突发长度支持——**这一项不在最初的
   范围设想里**，是第 1.3.1/2.3.2 节的分析暴露出来的：不加它，DMA 很可能
   只带来 CPU 释放而没有任何吞吐改善。它是对通用 DMA 驱动的向后兼容扩展，
   必须保证摄像头 JPEG 通道行为不变。

`gc9d01_push()`、`gc9d01_updatearea()`、`bk7258_gc9d01_window_full()`、
CASET/RASET 时序、`lcd_spi_quad_write_start/stop()` 的寄存器序列全部保持
不变；LVGL 与 `app/velasight` 也完全不动。显示链路从 LVGL 到
`bk7258_lcd_spi_write_frame()` 之间已经在
`VELASIGHT_MCU_APP_DEVELOPMENT_PLAN.md` 和
`VELASIGHT_UI_DESIGN_INSTRUCTION.md` 两份计划中定稿并实现，本计划不重新
讨论那部分的行为。

### 1.2 当前基线（逐项取证）

截至 2026-08-20：

| 项目 | 当前状态 | 证据 |
|---|---|---|
| 推屏搬运方式 | 纯 CPU，逐 32 位字 `putreg32()` | `bk7258_qspi.c:578-662` `bk7258_lcd_spi_write_frame()` |
| 每帧耗时 | 单屏 24-26ms（12800 字） | 实测日志见第 1.3 节 |
| DMA 引擎 | 已就绪，12 通道，但零使用在显示路径 | `bk7258_dma.c`/`bk7258_dma.h`；唯一使用者是 JPEG（通道 0） |
| LCD 专用 DMA 请求线 | 不存在 | `bk7258_dma.h` 的 `BK7258_DMA_DEV_*` 只有 `MEM/AUDIO_TX/AUDIO_RX/JPEG` |
| 字节序交换 | CPU 逐字完成，DMA 无等效字段 | `gc9_swap16x2()`，`bk7258_qspi.c:270-273` |
| LVGL 侵入 | 无需改动 | `lv_nuttx_fbdev.c` 的 `flush_cb` 只发出 `ioctl(FBIO_UPDATE)`，不关心底层是否为 DMA |
| framebuffer 实际位置 | **两块屏不在同一种内存里**：fb0 在 SRAM，fb1 在 PSRAM | 实测日志：`fb[0] ... at 0x280541b0`（AP RAM `0x28010000`-`0x28063fff`）、`fb[1] ... at 0x60a00010`（PSRAM）。`up_fbinitialize()` 用 `kmm_zalloc()`，而系统堆被 `bk7258_psram.c` 的 `kmm_addregion()` 扩展到含 PSRAM，SRAM arena 不够时第二块自然落到 PSRAM |
| framebuffer 缓存属性 | 两者都是非缓存 | AP RAM 与 PSRAM 两个 MPU 区域都标记 `MPU_RLAR_NONCACHEABLE`（`bk7258_start.c`），所以位置差异不影响 cache 结论 |
| D-Cache 是否实际启用 | 未启用（只声明能力） | `CONFIG_ARMV8M_HAVE_DCACHE` 被 `select`，但两个 defconfig 均未设 `CONFIG_ARMV8M_DCACHE=y` |
| FB_SYNC / 异步完成通知 | 不存在 | `gc9d01_fb.c` 的 `fb_vtable_s` 只实现 `getvideoinfo/getplaneinfo/updatearea`，无 `waitforvsync` |
| 双屏并发 | 两块面板各自独立总线，物理上可并行 | `bk7258_qspi.c` 的 `g_bus[2]`：QSPI0 (`0x64000000`)、QSPI1 (`0x68000000`) |

仓库中出现的 DMA 使用者已全部核对：`bk7258_camera_imgdata.c` 用通道 0
搬运 JPEG 比特流（peripheral-to-mem，REPEAT 模式）；`bk7258_audio_dev.c`
的注释说明音频尚未接入 DMA（仍是中断驱动的手工拷贝）。显示路径是零基线。

### 1.3 已测得的性能数据（真机日志）

逐字摘自 `logs/runtime/velasight-final-boot-20260819-100736.log`（`ap0:`
前缀是串口多核标记，非驱动输出的一部分）：

```text
ap0: gc9d01_fb[0]: 160x160 RGB565, framebuffer 51200 bytes at 0x280541b0, bus 1
ap0: gc9d01_fb[1]: 160x160 RGB565, framebuffer 51200 bytes at 0x60a00010, bus 0
ap0: gc9d01_fb[0]: update 1: 51200 bytes in 25ms (1 frames on bus)
ap0: gc9d01_fb[1]: update 1: 51200 bytes in 26ms (1 frames on bus)
ap0: gc9d01_fb[0]: update 2: 51200 bytes in 26ms (2 frames on bus)
ap0: gc9d01_fb[1]: update 2: 51200 bytes in 25ms (2 frames on bus)
ap0: gc9d01_fb[0]: update 3: 51200 bytes in 25ms (3 frames on bus)
ap0: gc9d01_fb[1]: update 3: 51200 bytes in 24ms (3 frames on bus)
```

以及 `bk7258_qspi.c` 内联注释和 `摄像头取帧接口与显示参数.md` 的记录：

```text
160x160 RGB565 (51200 字节，12800 个 32 位字) 单屏整帧推送：~24-27ms
单屏刷新上限：约 40 fps
局部刷新：不支持，updatearea() 忽略 area 参数，永远整帧推
双屏初始化：354ms（并行）/ 720ms（串行）
```

#### 1.3.1 这 25ms 花在哪里——一个尚未闭合的差距

先排除两项：

- `up_udelay(QSPI_FRAME_TAIL_DELAY_US)` 是 **15 微秒**，占 25ms 的
  0.06%，可以忽略；DMA 化后仍然保留，不影响任何结论。
- `gc9_swap16x2()` 的算术本身在 Cortex-M33 上是单条 `REV16` 量级的操作，
  不改变搬运的字节数。

**上面的日志本身提供了一个有力的对照实验**：同一次启动里，fb0 的
framebuffer 在 SRAM（`0x280541b0`），fb1 的在 PSRAM（`0x60a00010`），
两种内存的读延迟差别很大，但**两块屏的耗时完全一样（24-26ms）**。这说明
**源端内存的读取速度不是瓶颈**——否则 PSRAM 那一路会明显更慢。瓶颈在
写出端。

再算理论下限。控制器工作在 4 线 MCU-SPI 的**单数据线**模式
（`cmd_a_cfg2 = 0x80000000` 解码为 `data_line = 0`，见 `bk7258_qspi.c`
头注释；`QSPI_CONFIG_IO2_IO3_MODE` 也印证只有 SDA 一根数据线在用），
时钟为 480MHz / 8 = 60MHz：

```text
51200 字节 * 8 位 / 60e6 Hz ≈ 6.8 ms
```

**这个 6.8ms 是下限，且依赖一项未验证的假设**：`QSPI_OFF_CONFIG` 的
`clk_rate` 字段（bit[15:8]）实测为 0（日志里 `config=0x08410049`，
高位字节为 `0x00`），而原厂 `bk_qspi_init()` 对所有 LCD 路径都把
`clk_rate` 留成 0，只用 sysctrl 分频。但 `clk_rate = 0` 究竟表示
"不再分频（SCK = 60MHz）"还是"再除以 2（SCK = 30MHz）"，本 port 没有
取证过。若是后者，下限是 13.7ms。**P0 必须先确定这一项**，否则下面的
差距计算没有意义。

按 60MHz 计算，实测 25ms 与下限 6.8ms 之间有约 **18ms 无法用线速解释**。
`bk7258_qspi.c` 的注释说控制器 FIFO 满时会背压 AHB 写、从而"self-throttles
to the wire rate"——这解释了 CPU 为什么不会冲爆控制器，但**不能解释为什么
比线速慢 3.7 倍**。最可能的去向是**每个 32 位写都是一次独立的 AHB 事务**
（地址相位 + 数据相位），CPU 无法发出突发传输，于是每 4 个字节都付一次
事务开销。

**这正是 DMA 可能带来挂钟收益的地方，也是本计划一处必须修正的既有认知**：
原厂 `lcd_spi_dma_init()` 在 `CONFIG_SPE` 下显式设置

```c
bk_dma_set_dest_burst_len(dma_id, BURST_LEN_INC16);
bk_dma_set_src_burst_len(dma_id, BURST_LEN_INC16);
```

即让 DMA 以 16 拍突发访问总线，把地址相位摊薄到 1/16。而本 port 的
`bk7258_dma_configure_ex()` **完全没有 burst 字段**，`bk7258_dma.c` 里
把它明确留在复位值并注明"the reference's INC16 destination burst is a
throughput choice, not a requirement, and one variable at a time"。
对之前唯一的使用者（JPEG 比特流，吞吐远低于此）这个判断是合理的；
**对显示路径，它恰好就是那个决定性的变量**。详见第 2.3.2 节。

#### 1.3.2 因此本计划承诺什么、不承诺什么

**承诺（可验证）**：推屏期间 CPU 从 100% 忙等 25ms 变成在信号量上睡眠，
时间片交还调度器，可用于按键响应、网络协议栈、下一帧绘制准备。这一项
与上面的开放问题无关，无论挂钟时间是否改善都成立，验收方法见第 4.6 节。

**不承诺、但需要实测并记录（开放问题）**：挂钟时间是否从 25ms 降低。
乐观上限是接近线速的 ~7ms（若 SCK 确为 60MHz 且 18ms 差距确实来自事务
开销、且 DMA 突发能消除它）；悲观情况是几乎不变（若差距来自面板或
控制器的固有速率）。**不允许在 P0 完成 SCK 取证和 P2 完成实测之前，
对外宣称任何具体的加速倍数**。

这个区分直接决定验收标准：第 4.6/5.4 节的门禁是 CPU 占用率下降（承诺
项），挂钟耗时改善只作为记录项（第 5.3 节），不作为通过与否的判据。

### 1.4 为什么现在才做，之前为什么没做

`bk7258_gc9d01_fb.c` 的 `gc9d01_push()` 里已经留了这句话作为设计意图：

```text
/* First few pushes get a timing report: it is the only way to know the
 * real cost of a frame on this bus, which decides whether the preview
 * path needs DMA. */
```

`bk7258_qspi.c` 里也留了原厂 DMA 配置的结构对照：

```text
/* The destination address increments: lcd_spi_dma_single_mode_config()
 * sets dst.start_addr = qspi_data, dst.end_addr = qspi_data + data_len
 * and dst.addr_inc_en = DMA_ADDR_INC_ENABLE, so the window is a range,
 * not a single FIFO register. */
```

也就是说，"CPU 拷贝先跑通，DMA 留作后续优化"是既有的、写在代码注释里的
计划，本文档是把这个后续优化落到纸面。产品当前的事件驱动刷新策略
（`bk7258_status_screen.c`：只在状态变化时整帧推，不跑实时预览）已经把
"25ms 忙等"的发生频率降到很低——这也是为什么它至今不是阻塞性能问题，而
是一个已知的、有文档记录的优化空间。

## 2. 硬件事实（已取证）

### 2.1 现有 CPU 拷贝的确切位置

```c
/* board/beken/chips/bk7258/bk7258_qspi.c:578-662 */
bool bk7258_lcd_spi_write_frame(int bus, const void *frame, size_t len)
{
  /* ... RAMWR 命令、DC 拉高、清 cmd_c_* 寄存器 ... */

  modifyreg32(b->base + QSPI_OFF_CONFIG, 0, QSPI_CONFIG_FORCE_SPI_CS_LOW);
  modifyreg32(b->base + QSPI_OFF_CONFIG, 0, QSPI_CONFIG_IO_CPU_MEM_SEL);
  modifyreg32(b->base + QSPI_OFF_CONFIG, 0, QSPI_CONFIG_DISABLE_CMD_SCK);

  /* 这一段是要替换的目标 */
  if (g_swap_bytes)
    {
      for (i = 0; i < words; i++)
        {
          putreg32(gc9_swap16x2(src[i]), b->window + i * 4u);
        }
    }
  else
    {
      for (i = 0; i < words; i++)
        {
          putreg32(src[i], b->window + i * 4u);
        }
    }

  up_udelay(QSPI_FRAME_TAIL_DELAY_US);

  modifyreg32(b->base + QSPI_OFF_CONFIG, QSPI_CONFIG_DISABLE_CMD_SCK, 0);
  modifyreg32(b->base + QSPI_OFF_CONFIG, QSPI_CONFIG_FORCE_SPI_CS_LOW, 0);
  modifyreg32(b->base + QSPI_OFF_CONFIG, QSPI_CONFIG_IO_CPU_MEM_SEL, 0);

  b->frames++;
  return true;
}
```

`QSPI_CONFIG_FORCE_SPI_CS_LOW`、`QSPI_CONFIG_IO_CPU_MEM_SEL`、
`QSPI_CONFIG_DISABLE_CMD_SCK` 三个位构成"内存映射直写窗口"模式：置位后，
任何对 `b->window`（`0x64000000`/`0x68000000`）范围内地址的写入都会被控制
器转成 SPI 数据帧发出去，写入者是谁（CPU 还是 DMA）对控制器不可见。这正
是 DMA 化在寄存器层面成立的原因：**不需要新的控制器配置**，只需要把"谁
往这个窗口写"换成 DMA 引擎。

### 2.2 目的地址窗口

```text
QSPI0 (bus 0, LCD2/双屏): window = 0x64000000  (LCD_QSPI0_DATA_ADDR)
QSPI1 (bus 1, LCD1/单屏): window = 0x68000000  (LCD_QSPI1_DATA_ADDR)
```

证据：`bk7258_qspi.c:212-239` 的 `g_bus[BK7258_LCD_NBUSES]` 初始化表；与
原厂 `reg_base.h:44-45` 的 `SOC_QSPI0_DATA_BASE`/`SOC_QSPI1_DATA_BASE` 一致。

**这两个地址不落在 `bk7258_start.c` 任何一个显式 MPU 区域内**——逐项核对
`g_bk7258_mpu_regions[]`：Flash（`0x02150000`起）、AP RAM（`0x28010000`起）、
CP RAM（`0x28064000`起）、MB shared/UART（同样在 `0x28xxxxxx` 段）、PSRAM
（`0x60000000`-`0x60ffffff`，仅 `CONFIG_BK7258_PSRAM` 时存在）、最后一个
`MPU_RLAR_DEVICE` 区域（`0x40000000`-`0x5fffffff`）。`0x64000000` 和
`0x68000000` 都大于 `0x5fffffff`（不在最后一个区域内）也大于
`0x60ffffff`（不在 PSRAM 区域内），**是这张表里唯一没有显式覆盖的地址**。

这不是本计划才发现的边界情况：`bk7258_start.c` 的 MB shared 区域注释
已经明确写出这条规则并给出后果：

```text
/* mpu_initialize() below is called with privdefena, so an unmapped
 * address here would still work -- as cacheable Normal memory, per
 * the ARMv8-M default map -- and the CP would then read whatever
 * the AP's cache had not written back yet. */
```

`mpu_initialize(g_bk7258_mpu_regions, region_count, false, true)`
（`bk7258_start.c` 调用处，最后一个参数 `privdefena = true`）意味着未被
任何显式区域覆盖的地址会退回 ARMv8-M **默认内存映射**，而不是报错或
不可访问。对 Cortex-M33 系列，默认映射里 `0x60000000`-`0x9FFFFFFF`
（"External RAM"）是 **Normal 内存、可缓存**，不是 Device
——这与本计划最初假设的"退回默认映射后是 Device、不可缓存"正好相反。

**这个结论直接影响第 2.5 节和第 3.4 节的分析**，必须重新审视：

1. 当前 D-Cache 关闭（`CONFIG_ARMV8M_DCACHE` 未设置），所以"可缓存"这个
   属性目前不产生实际影响——CPU 现在对这个窗口的 `putreg32()` 写入本来
   就不经过缓存，这也是现状代码能正常工作的原因，与该地址是否被 MPU
   显式覆盖无关。
2. 但如果日后任何配置打开了 `CONFIG_ARMV8M_DCACHE`，QSPI 数据窗口会被
   当成可缓存的 Normal 内存对待——CPU 对它的写入可能先停留在缓存里，
   不会立刻到达控制器，这会让"写这个地址就等于把字节推上 SPI 总线"这一
   现有假设失效，而这个假设是 `lcd_spi_quad_write_start()`/`_stop()`
   整套时序设计的基础，不只是 DMA 移植关心的问题。
3. 对 DMA 而言，DMA 引擎作为总线上的另一个 master，其写入是否经过 CPU
   的缓存层完全取决于互连结构，**不能想当然认为"目的地是内存映射窗口就
   一定安全"**。P1 必须在真机上通过实际可见的画面结果验证 DMA 写入这个
   窗口确实到达了面板，不能只依赖寄存器地图推断，理由比最初写的更充分：
   不只是"没有 MPC 条目"的不确定性，而是这段地址目前完全没有被本 port
   自己的 MPU 表覆盖，其行为由 ARMv8-M 架构默认值决定而不是本 port 的
   显式设计决定。

**这是一处容易被算错的边界，值得专门提醒后续实现者**：最后一个显式区域
写的是 `{ 0x40000000, 0x20000000, ... }`，即"起始地址 + 大小"，如果心算
成"起始地址 + 大小"再顺着 `0x40000000 + 0x20000000 = 0x60000000` 这个
整数结果去判断"两个 QSPI 窗口都在 6 开头，应该也被覆盖"，会误以为区域
延伸到了 `0x6fffffff`——但区域实际终止于 `base + size - 1 =
0x5fffffff`，`0x64000000` 在此范围之外。P0/P1 阶段核对这类地址范围时，
应该用具体数值逐位比较（如第 2.2 节的写法），不能依赖对十六进制前缀的
直觉判断。

### 2.3 现有通用 DMA 引擎能力

`board/beken/chips/bk7258/bk7258_dma.c` + `include/bk7258_dma.h`：

```c
struct bk7258_dma_cfg_s
{
  uint8_t  channel;
  uint32_t src_addr;
  uint32_t dest_addr;
  uint32_t transfer_len;      /* 每次完成中断对应的字节数 */
  uint8_t  src_dev;           /* BK7258_DMA_DEV_* 请求线 */
  uint8_t  dest_dev;
  bool     src_inc;
  bool     dest_inc;
  bool     repeat;            /* true = REPEAT, false = SINGLE */
  uint32_t dest_loop_start;
  uint32_t dest_loop_end;
  uint8_t  data_width;        /* BK7258_DMA_WIDTH_* */
};

int bk7258_dma_configure_ex(const struct bk7258_dma_cfg_s *cfg);
void bk7258_dma_set_channel_callback(uint8_t channel,
                                     bk7258_dma_done_cb_t cb, void *arg);
void bk7258_dma_start_channel(uint8_t channel);
void bk7258_dma_stop_channel(uint8_t channel);
void bk7258_dma_get_channel_progress(uint8_t channel,
                                     uint32_t *remain_len,
                                     bool *finish_pending);
```

对显示路径而言，这是一次**纯 mem-to-mem 传输**：源是 framebuffer 或
（按第 3.3 节的决策）字节交换后的暂存区，两者都在非缓存内存里；目的是
QSPI 数据窗口，地址递增。注意窗口**不是** Device 内存——第 2.2 节已经
纠正过这一点，它落在 ARMv8-M 默认映射的可缓存 Normal 区间，只是当前
D-Cache 全局关闭使这一属性不产生实际影响。对照
`struct bk7258_dma_cfg_s`：

```text
src_addr        = 暂存区地址（若启用字节交换）或 priv->fbmem（若不启用）
dest_addr       = b->window                      （0x64000000 或 0x68000000）
transfer_len    = GC9D01_FBLEN = 51200            （一次完成，SINGLE 模式）
src_dev         = BK7258_DMA_DEV_MEM  (0x00)
dest_dev        = BK7258_DMA_DEV_MEM  (0x00)      （见 2.3.1）
src_inc         = true
dest_inc        = true                            （窗口是地址范围，不是单一
                                                     FIFO 寄存器，见 2.1）
repeat          = false                            （51200 字节一次搬完，
                                                     不需要 REPEAT 环形）
data_width      = BK7258_DMA_WIDTH_32BITS          （与现有 CPU 循环的字宽
                                                     一致）
```

**但仅靠上面这些字段不足以拿到吞吐收益**——现有结构体缺两样东西，见
第 2.3.2 节。

`transfer_len` 编码为 `(byte_count - 1) & 0xffff`（`bk7258_dma.c:280` 附近
`len_field`），16 位宽，51200 未超过其上限（65536 字节），**不需要
REPEAT 分片**，这与原厂 `lcd_spi_get_dma_repeat_once_len()` 只在
`data_len > 0x10000` 才切换到 REPEAT 模式的判断一致（原厂单次模式上限是
`0x10000` 字节，本芯片 `BK7258_DMA_CTRL_LEN_MASK` 是 `0xffff`，即 65536
字节，两者一致）。

#### 2.3.1 `dest_dev` 该填什么——一个必须先解决的疑点

`bk7258_dma.h` 的注释明确写道：`dest_dev`/`src_dev` 写入的是硬件
`req_mux` 编码，**目的是告诉 DMA 引擎在传输前等待哪个外设的硬件请求信号**
（`DMA_V_REQ_MUX_*`，见 `dma_reg.h:27-56`）。`BK7258_DMA_DEV_MEM` 对应
`DMA_V_REQ_MUX_DTCM (0x0)`，语义是"不等待任何外设握手，尽快传输"。

但原厂 `lcd_spi_dma_single_mode_config()`（`lcd_spi_driver.c:223-254`）把
`dma_config.dst.dev` 也设成 `DMA_DEV_DTCM`——即原厂自己在把 QSPI 数据窗口
当"内存"对待，不使用 QSPI 的硬件请求线。这与本 port 的判断一致：
`bk7258_dma.h` 里根本没有定义 QSPI 的 `req_mux` 常量，说明这条链路上没有
硬件流控信号可用，AHB 背压（第 1.3 节）就是唯一的节流机制，DMA 引擎和
CPU 面对的是同一个背压点。

**结论**：`dest_dev = BK7258_DMA_DEV_MEM`，与源端一致，这不是简化，是
和原厂参考实现完全对应的配置。P1 仍需用示波器或逻辑分析仪级别的验证
（若条件允许）或至少用面板上可见的图案确认背压确实在起作用，DMA 没有
因为"以为没有背压"而把数据冲爆到面板来不及处理的速度。

顺带记录一项与可达性相关的事实：`reg_base.h` 里 QSPI0/QSPI1 数据窗口
**各有一个 MPC**（`SOC_MPC_QSPI0_REG_BASE 0x41110000`、
`SOC_MPC_QSPI1_REG_BASE 0x41120000`），SRAM 各块也有
（`SOC_MPC_SMEM0..5_REG_BASE`）。本 port 从未配置过任何 MPC，而 CPU 对
这两个窗口的写入现在是能工作的，说明 MPC 的复位/CP 侧配置已经允许安全
世界访问。`bk7258_dma_configure_ex()` 恰好已经把
`BK7258_DMA_REQ_MUX_SRC_SEC`/`DEST_SEC` 两位置 1（该文件注释说明这是
因为 AP 运行在安全世界，与原厂 `CONFIG_SPE` 下
`bk_dma_set_src_sec_attr(DMA_ATTR_SEC)` 的做法一致），所以 DMA 侧的安全
属性与 CPU 侧一致，**不需要新增 MPC 配置**。这是推断，P1 的探测传输是
它的唯一验证手段。

#### 2.3.2 现有 DMA 驱动缺的两项：突发长度与 loop 地址

**（1）突发长度（burst length）——吞吐的关键，且当前无法表达。**

硬件在 `req_mux` 寄存器（通道组 word 7）里有两个 2 位字段
（`dma_struct.h` 的 `req_mux` 位域，与本 port `bk7258_dma.c` 已定义的
`BK7258_DMA_REQ_MUX_SRC_SEC (1u << 20)`/`DEST_SEC (1u << 21)` 同一个
寄存器）：

```text
src_burst_len  : bit[25:24]
dtst_burst_len : bit[27:26]     （原厂拼写如此，dest 之误）
取值：0 = SINGLE, 1 = INC4, 2 = INC8, 3 = INC16
```

原厂 LCD 路径显式设成 `BURST_LEN_INC16`（两端都设），本 port 的
`bk7258_dma_configure_ex()` **既没有这个入参、也不写这两个字段**，
留在复位值。`bk7258_dma.c` 自己的注释解释了原因：

```text
/* Burst lengths are left at their reset value: the reference's INC16
 * destination burst is a throughput choice, not a requirement, and one
 * variable at a time. */
```

对 JPEG 比特流那个使用者，这个取舍是对的。**对显示路径，它就是第 1.3.1
节那 18ms 差距最可能的解药**：如果不加这个字段，DMA 很可能仍然逐字发
事务，挂钟时间不会改善，只拿到 CPU 释放的收益。因此：

```text
必须扩展 struct bk7258_dma_cfg_s，增加 src_burst_len / dest_burst_len
（或一个统一的 burst_len），并在 bk7258_dma_configure_ex() 里写入
req_mux 的 bit[25:24] / bit[27:26]。
```

这是对 `bk7258_dma.c`/`bk7258_dma.h` 的改动，**超出了本计划最初"只改
`bk7258_qspi.c`"的范围声明**，必须在第 4.1 节的文件清单里显式列出。
新增字段要保持向后兼容：现有 JPEG 调用方不填该字段时（结构体零初始化）
应当仍然得到今天的复位值行为，不能因为新增字段而改变已验证过的摄像头
路径。

**（2）loop 地址寄存器——一个"配置看起来对但不搬数据"的候选陷阱。**

原厂 `dma_hal_init_dma()` **无条件**写四个 loop 地址寄存器，即使工作在
SINGLE 模式、`addr_loop_en` 为 0：

```c
dma_ll_set_src_start_addr(hw, id, config->src.start_addr);
dma_ll_set_src_loop_addr(hw, id, config->src.start_addr, config->src.end_addr);
dma_ll_set_dest_start_addr(hw, id, config->dst.start_addr);
dma_ll_set_dest_loop_addr(hw, id, config->dst.start_addr, config->dst.end_addr);
```

本 port 的 `bk7258_dma_configure_ex()` 只在 `dest_loop_end >
dest_loop_start` 时写 dest 的两个，**从不写 src 的两个**
（`BK7258_DMA_CH_SRC_LOOP_END/START` 宏已定义但无人使用），
`struct bk7258_dma_cfg_s` 里也没有 src loop 字段。

按架构语义，`addr_loop_en` 为 0 时 loop 边界应当是无关项，所以现有写法
理论上正确——摄像头 JPEG 通道能正常工作也是一个正面证据（它用了 dest
loop，但没用 src loop）。但这个仓库已经反复记录过同一类教训（例如
`bk7258_dma.c` 头注释里 DMA 单元级 `soft_reset`/`secure_attr` 不设就
"配置正确却搬不动任何数据"）。因此：

```text
如果 P1 的探测传输出现"通道配置回读正确、remain_len 不减少或数据不
到达"的现象，第一个要试的就是照原厂那样把 src/dest 的 loop 起止都
按线性范围写一遍，而不是先去怀疑地址映射或 MPC。
```

把这条写进计划，是为了让 P1 的调试有一个明确的第一顺位假设，而不是
从最难的方向开始排查。

### 2.4 通道占用与选择

当前唯一的 `bk7258_dma_configure_ex()` 使用者：

```text
BK7258_CAMERA_JPEG_DMA_CH = 0   (bk7258_camera_imgdata.c:141)
```

音频路径（`bk7258_audio_dev.c`）尚未使用 DMA（文件头注释：仍是中断中手工
拷贝 FIFO）。因此显示路径可以使用通道 0 之外的任意通道。两块面板各自的
传输逻辑上独立（不同总线、不同目的地址），如果要支持双屏并发 DMA（见
第 3.2 节），需要两个通道：

```text
BK7258_DISPLAY_DMA_CH0 = 1   (QSPI0 / bus 0 / display 1，双屏footprint)
BK7258_DISPLAY_DMA_CH1 = 2   (QSPI1 / bus 1 / display 0，单屏footprint)
```

通道号本身可调整，只要不与 `BK7258_CAMERA_JPEG_DMA_CH`（0）冲突；选 1、2
是因为它们是最小的两个空闲号，没有特殊语义。**如果日后音频接入 DMA
（`bk7258_audio_dev.c` 头注释预告的工作），需要重新核对这张表**，避免三
个子系统各自假设"我的通道没人用"却在某次功能都打开时冲突。这正是
`bk7258_dma.h` 用 12 通道设计、而不是像旧接口那样固定通道 0 的原因——
本计划是第一个把"选通道"当成显式设计决策的调用方，之前只有 JPEG 一个
使用者时不存在选择问题。

### 2.5 与 D-Cache 的关系（源端安全，目的端窗口需要 P0 重新确认）

`board/beken/chips/bk7258/Kconfig` 对 `ARCH_CHIP_BK7258` `select`
了 `ARMV8M_HAVE_DCACHE`（声明芯片有这个能力），但两个 defconfig
（`nsh`、`ai_agent`）都没有设置 `CONFIG_ARMV8M_DCACHE=y`——即 D-Cache
在本产品配置下**处于关闭状态**，CPU 对 SRAM 的读写都是直接落地，不经过
缓存。这意味着：

1. DMA 引擎读取 framebuffer（源端）时，看到的一定是 CPU 已经写完的最新
   内容（没有脏缓存行的问题），**不需要在发起 DMA 前调用任何
   `up_clean_dcache()`/`up_flush_dcache()`**。
2. 如果日后某个 defconfig 打开了 `CONFIG_ARMV8M_DCACHE`，源端仍然安全，
   **而且这一点对两块屏都成立，尽管它们不在同一种内存里**（第 1.2 节）：
   fb0 落在 AP RAM 区域、fb1 落在 PSRAM 区域，`bk7258_start.c` 把这两个
   区域**都**标记为 `MPU_RLAR_NONCACHEABLE`，所以即使全局打开 D-Cache，
   两块 framebuffer 本身都不会被缓存。方案 3 的暂存区（第 3.3 节）分配
   在 `BK7258_PSRAM_POOL_DISPLAY` 时也落在同一个 PSRAM 区域内
   （PSRAM MPU 区域覆盖 `0x60000000`-`0x60ffffff`，`ld.script` 的
   `PSRAM_SECTION` 是 `0x60a00000`+`0x600000`，包含在内），结论一致。
   **这仍是本计划成立的前提条件，必须在 P0 用实际读回的 MPU 配置确认，
   而不是假设"现在没开就永远不用管"**。
3. **目的端窗口（QSPI 数据窗口）不属于上述任何一条**：第 2.2 节已经
   纠正了最初的错误假设——`0x64000000`/`0x68000000` 并不落在
   `bk7258_start.c` 任何一个显式 MPU 区域内，退回 ARMv8-M 默认内存映射
   后是**可缓存的 Normal 内存**，不是不可缓存的 Device 内存。这意味着
   现在下结论"目的端天然安全"是不成立的，即便当前 D-Cache 全局关闭让
   这个问题暂时不显现。

结论调整为：**本计划现在不需要引入 cache 维护调用**，前提条件是
（a）D-Cache 保持全局关闭这一现状不变，且（b）framebuffer/暂存区继续
在已标记 `MPU_RLAR_NONCACHEABLE` 的区域分配。这两个前提中的第二条对
源端成立、且大概率会一直成立；**第 2.2 节揭示的目的端窗口缺失显式 MPU
覆盖，是一个独立于本计划、但本计划的实施必须先绕开或先修复的既有
缺口**——如果只是"绕开"（保持 D-Cache 关闭），本计划可以按原计划推进；
如果第 4.2 节 P0 阶段发现需要"修复"（给 QSPI 窗口补一个显式 Device
区域），那是对 `bk7258_start.c` 的改动，超出本计划最初"只改
`bk7258_qspi.c`"的范围声明，需要单独评估和征求确认，不能在本计划内
静默完成。

## 3. 设计决策与风险点

### 3.1 CPU 完成等待方式：忙等 vs 阻塞睡眠

`bk7258_lcd_spi_write_frame()` 目前是同步函数，调用者（LVGL 的
`flush_cb` -> `ioctl(FBIO_UPDATE)` -> `gc9d01_updatearea()` ->
`gc9d01_push()`）期望它返回时数据已经发送完毕。DMA 化之后，"发起传输"
和"传输完成"在时间上分离，函数体内必须选择一种方式等待完成：

**方案 A：忙等 `bk7258_dma_get_channel_progress()` 的 `remain_len`。**
不需要中断、不需要信号量，改动最小，但没有把 CPU 时间还给调度器——
和现在的 CPU 拷贝比，唯一的差别是"谁在搬数据"，CPU 仍然在这 25ms 内
100% 占用，只是不再执行 `putreg32`，而是执行忙等轮询。**这基本不产生
第 1.3 节所说的"释放 CPU 时间片"收益**，只是把工作从"确定性的 12800 次
写"换成"不确定次数的状态轮询"，甚至可能因为轮询间隔而变慢。

**方案 B：中断完成 + 信号量，调用线程睡眠等待。** 使用
`bk7258_dma_set_channel_callback()` 注册完成回调，回调里 `nxsem_post()`，
`bk7258_lcd_spi_write_frame()` 内部 `nxsem_wait_uninterruptible()`。这是
本 port 处理"等待硬件完成"的标准写法：`bk7258_sdio.c` 的
`priv->data_sem`（中断里 `nxsem_post()`，等待侧
`nxsem_tickwait_uninterruptible()` 带超时）、`bk7258_pm_pwc.c` 的
`g_psram_power_sem`、`bk7258_bt_transport.c` 的 `g_vendor_sem`、
`bk7258_mb_uart.c` 的 `worker_sem` 都是同一模式，其中 `bk7258_sdio.c`
最贴近本场景（等一次数据传输完成，且带有界超时）。调用线程在等待期间
真正让出 CPU，其他任务（网络、按键扫描、下一帧的绘制准备）可以运行。

**方案 C：真正异步——`ao_updatearea` 立即返回，flush 完成后再通知
LVGL。** 需要给 `fb_vtable_s` 增加事实上的"pending"状态和某种完成通知
（`CONFIG_FB_SYNC`/`waitforvsync`，或者一个自定义 ioctl），并且 LVGL 侧
的 `flush_cb`/`lv_display_flush_ready()` 调用时机要跟着改——这触碰到
`lv_nuttx_fbdev.c`（LVGL 上游代码，不是本仓库拥有）的调用约定，且
`lv_display_flush_is_last()` 之后立即 `lv_display_flush_ready()` 的现有
写法假设 flush 在返回前已完成。真正做成异步意味着 LVGL 可能在上一帧
DMA 还没写完时开始往同一块 framebuffer 渲染下一帧，产生撕裂（第 3.4 节
分析了这个风险窗口）。

**决策：默认方案 B。** 方案 A 不达成本计划的目的（见第 1.3 节的收益定义）；
方案 C 的收益（真正的流水线双缓冲）需要改变 framebuffer 数量和 LVGL 侧
渲染节奏，是比"把 CPU 拷贝换成 DMA 搬运"大得多的另一个项目，本计划不
做（见第 6 节"明确不做"）。方案 B 在保持"`bk7258_lcd_spi_write_frame()`
是同步函数，调用者感知不到内部变化"这一契约的前提下，把 CPU 从"搬运者"
变成"等待者"，是最小的、能达成目标的改动。

### 3.2 双屏并发：两个独立通道还是排队复用一个通道

两块面板各自挂在独立的 QSPI 控制器和独立的内存窗口上（`0x64000000` /
`0x68000000`），物理上互不干扰。如果 LVGL 需要连续刷新两块屏（例如
`vs_display_render()` 里 `content_dirty` 和 `status_dirty` 同时为真的
情况），当前的同步 CPU 实现是严格顺序执行：先整帧推 fb1，再整帧推
fb0，两次各 25ms，总计约 50ms。

DMA 化后，如果给两块屏各配一个独立 DMA 通道（见 2.4 节），理论上可以
同时发起两次传输，让它们在总线层面并行——两个 QSPI 控制器有各自独立
的寄存器组和数据窗口，互不共享硬件资源，DMA 引擎本身有 12 个通道可以
同时跑。这样两次推屏的总耗时可以从"串行 50ms"降到"并行 25ms"（以较慢
的一路为准）。

**但这是一个需要额外改动调用方的决策，本计划默认不做**：当前
`vs_display_render()` 是顺序调用 `vs_render_content()` 后
`vs_render_status()`，各自内部同步走到 `FBIO_UPDATE` 返回。要真正并行，
`bk7258_lcd_spi_write_frame()` 本身的"发起 DMA + 等待完成"是一个原子
操作，两次调用仍然是顺序的（第一次调用等完成之后才返回，第二次调用才
开始）。若要并行，需要在更高层把"发起"和"等待"拆开——这正是方案 C
的范畴，本计划第一阶段不做。

**本计划第一阶段的并发范围**：仅保证两块面板各自使用独立的 DMA 通道，
使得"通道分配"本身不成为将来做并行化的障碍；不改变
`bk7258_lcd_spi_write_frame()` 对调用者呈现的同步语义，也不改变
`vs_display_render()` 的调用顺序。双通道独立分配是为了避免"两块面板轮流
等同一个通道排队"这种毫无必要的新增串行化，不是为了这一阶段就实现并行
推屏。

### 3.3 字节序交换——DMA 无法做，必须找替代方案

这是本计划**最核心的技术风险**。现有 CPU 循环对每个 32 位字（即两个
RGB565 像素）做 `gc9_swap16x2()`：

```c
static inline uint32_t gc9_swap16x2(uint32_t w)
{
  return ((w & 0x00ff00ffu) << 8) | ((w & 0xff00ff00u) >> 8);
}
```

原因是面板要求每个像素高字节先出，而 NuttX 的 `FB_FMT_RGB16_565`
framebuffer 是主机序（小端，低字节先存）。`bk7258_dma.c` 的 DMA 引擎
（对照 `dma_struct.h` 的 `ctrl` 位域和 `bk7258_dma_cfg_s`）**没有任何
字节交换字段**——它只有 `src_data_width`/`dest_data_width` 和地址递增
控制，是纯粹的字节搬运器，不会重排字节。原厂 DMA 配置
（`lcd_spi_dma_single_mode_config()`）同样没有交换字段，这意味着原厂参考
实现能够直接 DMA，隐含的前提是**原厂的 framebuffer 在内存里已经是面板
期望的字节序**（`bk7258_qspi.c` 头注释引用的
`lcd_spi_display_fill_pure_color()`：`data[0] = color >> 8; data[1] = color;`
——即原厂应用层在填充帧时手动把高字节存在前面，而不是依赖 DMA 或 CPU 做
运行时交换）。

本 port 目前选择"framebuffer 保持主机序（符合 `FB_FMT_RGB16_565`
对用户空间的承诺），交换动作放在推屏这一步"，这是为了让
`/dev/fb0`/`/dev/fb1` 对任何写它们的程序（LVGL、`camera_preview`、
`hello_screen`、`jpeg_test`）都表现出标准语义，不需要每个写 framebuffer
的程序都知道"这块面板要交换字节"。DMA 化必须保留这个语义，否则相当于
把"面板的怪癖"从驱动内部泄漏给所有应用层代码，这是明确不可接受的倒退。

三个可选方案（编号只为引用方便，不表示优先级；结论见本节末尾的"决策"）：

**方案 1：LVGL 侧调用 `lv_draw_sw_rgb565_swap()`，framebuffer
本身存大端字节序，DMA 直接搬运不交换。** LVGL 9.1 自带这个 helper
（`apps/graphics/lvgl/lvgl/src/draw/sw/lv_draw_sw.c:186`），文档在
`lv_display.h:316-318` 明确写着"如果面板需要交换 RGB565 的两个字节，
在 `flush_cb` 里调用它"。做法是在 `flush_cb` 里，`ioctl(FBIO_UPDATE)`
之前，对本次 flush 涉及的像素区域调用一次 `lv_draw_sw_rgb565_swap()`，
把 framebuffer 里的数据原地转成大端。这样 DMA 搬运的字节序和目的地一致，
不需要驱动层再交换。

代价：这段调用发生在 LVGL 应用侵入的一侧（`app/velasight/vs_display.c`
或它包装的 `flush_cb`），**不是纯粹的 chip/board 驱动层改动**——这意味
着本计划的范围会从"只改 `bk7258_qspi.c`"扩大到"同时改
`app/velasight` 的显示初始化代码"。而且非 LVGL 的 framebuffer 写入者
（`camera_preview`、`hello_screen`、`jpeg_test`、`bk7258_gc9d01_fb.c`
自己的 `bk7258_gc9d01_fb_fill()`/`_hello()` 等 board 侧绘制函数）都不会
调用这个 LVGL helper，它们写入的仍然是主机序数据——如果驱动层同时改成
"不交换，假设输入已经是大端"，这些非 LVGL 路径会全部变成错误的颜色。
**这个代价定义了方案 1 的真实范围：必须同时改所有 framebuffer 写入者，
或者只对 LVGL 使用的这两个 `/dev/fbN` 节点生效、其他保持 CPU 交换路径
——而 `/dev/fb0`/`/dev/fb1` 本身就是仅有的两个节点，没有"只对 LVGL"这种
折中，两块屏被 LVGL 独占后，事实上没有其他运行时写入者，但诊断工具
（`camera_preview`、`hello_screen`）仍会在非 LVGL 配置或调试场景下写同样
的节点。**

**方案 2：保留驱动层的字节交换，但用 DMA 的 `pixel_trans_type` 或类似
硬件特性做交换，而不是 CPU 逐字算。** 已核实：`dma_struct.h` 的
`req_mux` 字段确实有一个 2 位的 `pixel_trans_type`
（`DMA_TRANS_DEFAULT`/`DMA_TRANS_RGB_TO_YUV`/`DMA_TRANS_YUV_TO_RGB`），
但语义是色彩空间转换（RGB↔YUV），**不是字节序交换**，原厂代码里全部
用法都设成 `DMA_TRANS_DEFAULT`。这条路已被证明不通，记录在此是为了不
让后续实现者重复调查同一个死胡同。

**方案 3：驱动层维护一块与 framebuffer 等大的"交换后暂存区"
（scratch buffer），`bk7258_lcd_spi_write_frame()` 内部先用 CPU 把
framebuffer 的内容做字节交换写入暂存区，再对暂存区发起 DMA。** 这是
唯一能在**不改变任何调用者**（LVGL 侧和其他 framebuffer 写入者都不用
知道交换细节）的前提下引入 DMA 的方案。代价是：

- 需要额外的 51200 字节 * 2（双屏）= 100KB 暂存区。**这不能从 SRAM 出**：
  `bk7258_psram.c` 的注释记录了实测数字——Wi-Fi 落地后静态 RAM 从 97KB
  涨到 182KB，SRAM heap arena 从 247KB 降到 158KB，而 `ai_agent` 本身就
  需要约 200KB 加 32KB 任务栈，正是因此才把 PSRAM 尾部并入系统堆。第 1.2
  节的实测日志也印证了这个紧张程度：两块 framebuffer 里只有第一块拿到了
  SRAM，第二块已经落到 PSRAM。所以暂存区**必须**显式从
  `bk7258_media_pool_alloc(BK7258_PSRAM_POOL_DISPLAY, ...)` 分配，
  不能用 `kmm_zalloc()` 碰运气——后者在 SRAM 还有空间时会挤占
  `ai_agent` 的余量，且两块屏会一块落 SRAM 一块落 PSRAM，行为不一致。
- **CPU 仍然要执行一次 12800 字的交换循环**，只是写目标从 QSPI 窗口
  换成暂存区。这一步会不会比现在快，**取决于第 1.3.1 节那个未闭合的
  问题**：如果 25ms 主要是 QSPI 窗口的逐字事务开销，那么改写到普通内存
  会快一个数量级；但暂存区在 PSRAM（非缓存、比 SRAM 慢），所以不能直接
  套用"SRAM-to-SRAM 很快"的直觉。**有利的证据**是第 1.3.1 节的对照实验：
  fb1 的 framebuffer 本身就在 PSRAM，读它并不比读 SRAM 的 fb0 慢，说明
  PSRAM 在这个数据量级上不是瓶颈。**但"读 PSRAM 不慢"不等于"写 PSRAM
  不慢"**，这一项必须在 P2 单独测量（把交换耗时和 DMA 耗时分别打点），
  不能合在一起报一个总数。
- 净效果：CPU 从"25ms 忙等搬运"变成"一次交换写入（耗时待测）+ 睡眠等待
  DMA 完成"。只要交换耗时显著小于 25ms，就达成第 1.3.2 节承诺的目标
  （CPU 时间片被释放）；如果实测发现交换本身就要十几毫秒，方案 3 的收益
  会大幅缩水，届时应重新评估方案 1。这是 P2 门禁必须回答的问题。

**决策：默认方案 3。** 理由是它是唯一不扩大改动范围到 `app/velasight`
和其他 framebuffer 消费者的方案：改动全部留在 chip 层（第 1.1 节的两个
改动点），`/dev/fbN` 对用户空间的 `FB_FMT_RGB16_565` 语义保持不变，风险
最可控。方案 1 作为**可选的后续优化**记录在第 6 节，如果 P2 实测发现
方案 3 的交换开销显著（见上面第二个要点），再评估是否值得扩大范围换取
方案 1 的"零 CPU 交换"收益。方案 1 展示了一种今天没有采用、但 LVGL 上游
明确支持的方向，因此写入本计划供将来参考，不属于本轮交付范围。

### 3.4 源端（framebuffer）的正确性：不需要 cache 维护，但需要"稳定快照"

第 2.5 节已经确认不需要 cache 维护。还有一个不同的问题：DMA 传输期间，
如果 CPU（或方案 3 里的交换步骤）继续修改 framebuffer 的内容，DMA 搬运
出去的画面会是新旧混合的撕裂帧。这在当前的纯 CPU 实现里不存在，因为
CPU 拷贝本身就是"读一个字，立刻写出去"，不存在"传输窗口内数据被别处
修改"的时间窗——现在引入 DMA（或方案 3 的暂存区），这个时间窗第一次
出现。

对方案 3 而言，风险窗口分两段：

1. **CPU 交换阶段**（从 framebuffer 读，写暂存区）：这一步本身很快
   （微秒级），且是同一线程内连续执行，不存在并发写者——除非 LVGL 的
   渲染发生在另一个任务里且没有跟这次 flush 同步。当前 velasight 的
   `lv_timer_handler()` 是单线程调用（`vs_app.c` 主循环），渲染和 flush
   在同一个任务上下文顺序执行，不存在这个问题。
2. **DMA 传输阶段**（从暂存区搬到 QSPI 窗口）：暂存区是驱动私有的，
   LVGL 或任何应用层代码都不会碰它，只要驱动自己保证"上一次 DMA 完成
   之前不复用/不覆写这块暂存区"，就没有撕裂风险。这是`bk7258_lcd_spi_write_frame()`
   内部的顺序保证（发起 DMA -> 等待完成 -> 返回），天然满足。

**结论**：只要坚持方案 3（暂存区）和方案 B（同步等待完成才返回），
不需要额外的同步机制；`bk7258_lcd_spi_write_frame()` 对调用者的"调用
返回时数据已经上屏"契约保持不变，撕裂风险因此不适用于本计划的默认
设计——它只会在采用第 3.1 节方案 C（真异步）时出现，记录在此是为了让
后续任何想要移除同步等待的人知道这个前提条件不能丢。

### 3.5 与 CASET/RASET 全窗口刷新的关系

`gc9d01_updatearea()` 目前忽略 LVGL 传入的 `area` 参数，永远整帧推
（第 1.1 节的架构图已经标注这一点不变）。DMA 化不改变这一行为——
`bk7258_gc9d01_window_full()` 仍然在每次推屏前把窗口设成整个 160x160
区域，`bk7258_lcd_spi_write_frame()` 仍然传入整帧长度
（`GC9D01_FBLEN` = 51200）。局部刷新（只 DMA 传输 LVGL 标记的脏矩形）
是一个独立的优化方向，需要窗口命令按脏矩形动态计算 CASET/RASET 参数、
以及 framebuffer 里非连续行的跨步 DMA（`dest_loop_start`/`dest_loop_end`
或多次描述符），复杂度和风险都明显高于"整帧 DMA 替换整帧 CPU 拷贝"，
本计划不做（见第 6 节）。

## 4. 代码落点与实施顺序

### 4.1 目标文件

```text
board/beken/chips/bk7258/bk7258_qspi.c                 修改：
                                                        bk7258_lcd_spi_write_frame()
                                                        改用 DMA；新增内部
                                                        完成信号量、暂存区
                                                        管理
board/beken/chips/bk7258/include/bk7258_qspi.h         可能新增：
                                                        bk7258_lcd_spi_dma_ready()
                                                        查询接口（诊断用，
                                                        可选）
board/beken/chips/bk7258/include/bk7258_dma.h          修改：为
                                                        struct bk7258_dma_cfg_s
                                                        增加突发长度字段
                                                        （见 2.3.2），并按需
                                                        增加 src loop 字段；
                                                        新增 BK7258_DMA_BURST_*
                                                        取值常量。
                                                        不需要新增
                                                        BK7258_DMA_DEV_* 常量
                                                        （复用 MEM，见 2.3.1）
board/beken/chips/bk7258/bk7258_dma.c                  修改：在
                                                        configure_ex() 里写入
                                                        req_mux 的
                                                        bit[25:24]/bit[27:26]
                                                        突发长度；保持零值
                                                        入参等价于今天的复位
                                                        行为，不回归摄像头
                                                        JPEG 通道
board/beken/chips/bk7258/Kconfig                       新增：
                                                        BK7258_LCD_SPI_DMA
                                                        开关及帮助文本
board/beken/boards/bk7258/bk7258-ap/configs/nsh/defconfig       新增开关（默认
                                                                  关闭，见 4.5）
board/beken/boards/bk7258/bk7258-ap/configs/ai_agent/defconfig  同上
```

**不需要修改**（范围收窄的直接体现，重申一遍避免范围蔓延）：

```text
app/velasight/*                                        不改动
board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01_fb.c      不改动
board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01.c         不改动
../../apps/graphics/lvgl/*                              不改动（不采用方案 1）
```

分层边界：`bk7258_qspi.c` 是 chip 层，唯一知道"这个字节序、这个 DMA
通道号、这块暂存区"的地方；board 层（`bk7258_gc9d01_fb.c`）继续只调用
`bk7258_lcd_spi_write_frame(bus, frame, len)`，感知不到内部实现从 CPU
变成了 DMA。这与本 port 现有的 chip/board 分层原则一致（chip 层管寄存器
和硬件时序，board 层管板级语义），也是 ADC 计划第 5.1 节采用的同一套
分层约定。

### 4.2 P0：冻结输入、验证前提假设

不写任何驱动代码。产出：

1. 仓库工作树状态与 commit hash（`git status --short`、`git log -1`）。
2. 用真机确认 `CONFIG_ARMV8M_DCACHE` 在当前两个 defconfig 构建产物里确实
   未启用（读 `.config` 或者从构建日志确认，不能只看源码默认值）——这是
   第 2.5 节"不需要 cache 维护"结论的前提条件。
3. 确认 QSPI0/QSPI1 数据窗口（`0x64000000`/`0x68000000`）在
   `g_bk7258_mpu_regions[]` 中确实没有被任何区域覆盖（第 2.2 节已经用
   地址算术核对过一次，P0 需要独立复核这个结论，不能直接沿用本文档的
   推导），并记录这一状态是否可接受地维持到本计划实施完成，还是需要
   作为前置修复单独提出。
4. 用真机确认 `bk7258_dma_init()` 在 `nsh`/`ai_agent` 两个配置下各自的
   调用时机：当前它只在 `bk7258_camera_imgdata_init()` 里被调用
   （camera 打开时才执行，见 `bk7258_camera_imgdata.c:1427`），而不是在
   `board_late_initialize()` 无条件调用。如果显示路径需要 DMA 而摄像头
   未被打开/未启用，`bk7258_dma_init()` 可能从未执行过，`bk7258_dma.c`
   的 IRQ 未挂载、`prio_mode`/`secure_attr`/`priv_attr` 未初始化——DMA
   引擎处于复位状态。**这是本计划一个必须在 P0 就解决的初始化顺序问题**，
   不能假设"camera 会先把 DMA 引擎叫醒"。
5. **取证实际 SCK 频率**（第 1.3.1 节的前置条件）：`clk_rate = 0` 是
   "不再分频（SCK = 60MHz）"还是"再除以 2（SCK = 30MHz）"。可用的手段
   按可靠性排序：接逻辑分析仪/示波器直接量 SCL 周期；或查工作区根目录
   `/home/mi/vela_competition` 下的 BK7258 数据手册 PDF 的 QSPI 时钟章节
   （注意该文件名里"BK7258"与"Datasheet"之间是一个 **不换行空格**
   U+00A0，不是普通空格，用普通空格拼路径会找不到文件，建议用
   `find . -maxdepth 1 -iname '*Datasheet*'` 定位）；或推 N 帧测总时间
   反推有效位速率。**这一项决定 6.8ms 还是 13.7ms 是理论下限，进而决定
   那 18ms 差距是否真的存在**，不能跳过。
6. 记录一次基线：不开 DMA 的情况下，`gc9d01_fb[0]`/`[1]` 的推屏耗时
   （已有，见第 1.3 节），作为 P2 对比的对照组。

P0 门禁：`CONFIG_ARMV8M_DCACHE` 状态已从真实构建产物确认；QSPI 数据窗口
缺失显式 MPU 覆盖这一事实已独立复核，且已决定是"维持现状（保持 D-Cache
关闭）推进"还是"先修复 `bk7258_start.c` 补一个显式区域"；**SCK 实际频率
已取证**；DMA 引擎初始化时机的依赖关系已经明确（谁在什么条件下调用
`bk7258_dma_init()`，显示路径是否需要独立调用它）；基线耗时数据已记录
在案。

### 4.3 P1：DMA 引擎独立初始化 + 单次探测

**先解决 `bk7258_dma_init()` 的重复调用问题。** 它当前不是安全可重入的：

```c
void bk7258_dma_init(void)
{
  putreg32(BK7258_DMA_PRIO_SOFT_RESET | BK7258_DMA_PRIO_MODE_RR,
           BK7258_DMA_PRIO_MODE);        /* 单元级软复位，影响全部 12 通道 */
  ...
  irq_attach(BK7258_IRQ_DMA, bk7258_dma_isr, NULL);
  up_enable_irq(BK7258_IRQ_DMA);
}
```

`irq_attach()` 重复调用只是覆写同一个向量，无害；但那个**单元级软复位
会影响所有通道**。因此如果摄像头已经在跑 JPEG DMA，显示路径此时再调用
一次 `bk7258_dma_init()`，会打断摄像头正在进行的传输。反过来同理：
`bk7258_camera_imgdata_init()` 在摄像头打开时调用它，会打断显示路径
正在进行的推屏。这不是"幂等"，是**互相破坏**。

```text
给 bk7258_dma_init() 加一次性保护（静态 bool，或拆出
  bk7258_dma_init_once()），使单元级软复位只在第一次执行；后续调用只做
  幂等的部分（或直接返回）。这是对 bk7258_dma.c 的改动，与 2.3.2 节的
  突发长度改动同属"通用 DMA 驱动的必要修补"，一并纳入范围。
把这次一次性初始化提前到显示与摄像头都还没开始用 DMA 的时点（board
  bring-up 早期），使两个子系统都只是"使用者"，不再各自负责唤醒引擎。
验证：摄像头持续取帧的同时反复推屏，JPEG 帧不因为推屏而损坏或短帧；
  反之推屏不因为摄像头启动而花屏。
```

**然后做可达性探测。**

```text
用一次已知内容的小块内存（例如 256 字节）做 mem-to-mem 到另一块内存的
  DMA 传输，验证 bk7258_dma_configure_ex() + 完成中断的基本链路可用
  （不涉及 QSPI 窗口，纯粹验证 DMA 引擎本身与新增的突发长度字段）
用同样的配置，把目的地址换成 QSPI 数据窗口，传输一小段已知图案（例如
  纯色填充的一部分），在面板上肉眼确认颜色和位置正确，等价于
  bk7258_gc9d01_fb.c 现有的 "无 ID 寄存器，只能靠画面验证" 方法论
若出现"配置回读正确但数据不到达"，按 2.3.2 节第（2）条，第一顺位假设是
  loop 地址寄存器未按原厂那样写全，而不是地址映射或 MPC 问题
```

P1 门禁：DMA 引擎初始化时机确定且不与摄像头路径冲突（若摄像头也在用）；
小块 mem-to-mem 传输验证 DMA 完成中断触发且数据正确；对 QSPI 窗口的探测
性 DMA 写入在面板上产生预期的可见结果。

### 4.4 P2：接入 `bk7258_lcd_spi_write_frame()`（首个可用版本）

```c
/* 概念性结构，具体寄存器细节按 bk7258_qspi.c 现有风格实现 */
bool bk7258_lcd_spi_write_frame(int bus, const void *frame, size_t len)
{
  /* ... RAMWR、DC、quad_write_start 前半段完全不变 ... */

  if (g_swap_bytes)
    {
      /* 方案 3：CPU 把 frame 交换字节序写入本 bus 私有的暂存区 */
      gc9_swap_into_scratch(b, frame, words);
      dma_src = (uint32_t)b->scratch;
    }
  else
    {
      dma_src = (uint32_t)frame;
    }

  cfg.channel        = b->dma_channel; /* 每条 bus 各自的通道，见 2.4 */
  cfg.src_addr       = dma_src;
  cfg.dest_addr      = b->window;
  cfg.transfer_len   = len;
  cfg.src_dev        = BK7258_DMA_DEV_MEM;
  cfg.dest_dev       = BK7258_DMA_DEV_MEM;
  cfg.src_inc        = true;
  cfg.dest_inc       = true;
  cfg.repeat         = false;
  cfg.data_width     = BK7258_DMA_WIDTH_32BITS;
  cfg.src_burst_len  = BK7258_DMA_BURST_INC16;  /* 新增，见 2.3.2 */
  cfg.dest_burst_len = BK7258_DMA_BURST_INC16;

  /* 回调必须在 start 之前注册，否则可能丢掉完成信号 */
  bk7258_dma_set_channel_callback(b->dma_channel, lcd_dma_done, b);
  nxsem_reset(&b->dma_done_sem, 0);

  if (bk7258_dma_configure_ex(&cfg) < 0)
    {
      lcd_spi_quad_write_stop(b);   /* 必须收尾，否则 CS 一直被拉低 */
      return false;
    }

  bk7258_dma_start_channel(b->dma_channel);

  /* 有界等待。无界的 nxsem_wait_uninterruptible() 一旦丢掉完成中断就会
   * 永久挂死显示线程；现有 CPU 路径的 lcd_wait_done() 也是有界轮询
   * (QSPI_WAIT_DONE_MAX_ITER)，原厂同样用
   * rtos_get_semaphore(&dma_sema, 5000)。超时按失败处理并停通道，
   * 让上层看到一次失败的推屏，而不是整个 UI 卡死。
   */
  if (nxsem_tickwait_uninterruptible(&b->dma_done_sem,
                                     MSEC2TICK(QSPI_DMA_TIMEOUT_MS)) < 0)
    {
      bk7258_dma_stop_channel(b->dma_channel);
      lcd_spi_quad_write_stop(b);
      return false;
    }

  up_udelay(QSPI_FRAME_TAIL_DELAY_US);

  /* ... quad_write_stop 后半段完全不变 ... */
}
```

其中 `lcd_dma_done()` 是运行在 DMA 中断上下文的回调，只做
`nxsem_post(&b->dma_done_sem)`，不做其他任何事（对照第 0 节安全规则 4）。
`QSPI_DMA_TIMEOUT_MS` 取一个远大于正常帧耗时又不至于让 UI 长时间无响应
的值（例如 200ms，正常帧约 25ms，留 8 倍余量；原厂用 5000ms 偏保守）。

**所有提前返回的分支都必须先执行 `lcd_spi_quad_write_stop()`**：
`FORCE_SPI_CS_LOW`/`IO_CPU_MEM_SEL`/`DISABLE_CMD_SCK` 三个位在函数
前半段被置起，如果直接 `return` 而不清理，CS 会一直保持拉低、命令通道
仍被旁路，**下一次任何命令（包括 CASET/RASET）都会失败**，故障表现为
"第一次 DMA 超时之后屏幕永久不再更新"。现有 CPU 实现没有中途返回的
分支，所以不存在这个问题；DMA 版本引入了失败路径，这一点必须成对处理。

暂存区通过 `bk7258_media_pool_alloc(BK7258_PSRAM_POOL_DISPLAY, 32,
GC9D01_FBLEN)` 在 `bk7258_lcd_spi_init()` 里一次性分配（**不用
`kmm_zalloc()`**，理由见第 3.3 节方案 3 的内存预算），两块面板各自一份
（不能共享，两条总线可能被要求并发，见 3.2 节的独立通道设计——即使本
阶段不做真正并行，也不应该为省内存引入一个后续想做并行时又要拆开的
共享暂存区）。分配失败时必须退回 CPU 拷贝路径，不能让推屏直接失败。

**P2 必须分别打点两个耗时**（第 3.3 节方案 3 的第二个要点）：字节交换
写入暂存区的耗时、以及 DMA 传输的耗时。合成一个总数无法回答"交换是否
成为新瓶颈"，也无法回答第 1.3.1 节那个 18ms 差距是否被突发传输改善。

P2 门禁：与 CPU 版本对比，画面内容（颜色、位置、无花屏）在肉眼和/或
已有的自动化对比脚本下完全一致；`bk7258_lcd_spi_write_frame()` 对外
行为不变（返回值语义、调用者无需修改）；1000 次连续推屏无卡死、无 DMA
完成信号丢失（无一次超时）；交换耗时与 DMA 耗时已分别记录。

### 4.5 P3：开关与灰度

新增 `CONFIG_BK7258_LCD_SPI_DMA`，默认 `n`，与现有 CPU 路径共存
（编译期二选一，不是运行期开关，避免同时维护两套状态机的复杂度）：

```kconfig
config BK7258_LCD_SPI_DMA
	bool "Use DMA for GC9D01 pixel push"
	default n
	depends on BK7258_GC9D01_FB
	help
		Replace the CPU putreg32() loop in bk7258_lcd_spi_write_frame()
		with a bk7258_dma.c mem-to-mem transfer into the QSPI data
		window.

		What this reliably buys is CPU time: the pushing thread sleeps
		on a semaphore instead of busy-writing for ~25ms per frame.
		Whether the frame also lands faster depends on how much of that
		25ms is per-word AHB transaction overhead rather than panel or
		controller rate; the DMA burst configuration is what could
		close that gap. Treat any wall-clock speedup as measured, not
		assumed -- see
		docs/plans/BK7258_OPENVELA_LCD_DMA_PORTING_PLAN.md section 1.3.

		Costs one PSRAM byte-swap scratch buffer per panel
		(51200 bytes each).
```

先在 `nsh` 配置下默认开启验证（`nsh` 是底层门禁配置，改动影响范围小），
`ai_agent`（产品配置）在 P2/P3 门禁都通过后才切换默认值。这与 ADC 计划
第 5.8 节"先 nsh 再 ai_agent"的推进方式一致。

### 4.6 P4：CPU 占用率验证（对应第 1.3 节的真实目标）

**已核实 `CONFIG_SCHED_CPULOAD` 在两个 defconfig 里都没有启用**，所以不能
直接用 `/proc` 的 CPU 占用率读数。两条可行路径：

```text
方案 i（推荐，不改配置）：计数探针
  建一个最低优先级的 kthread，循环里只做 ++counter 然后 sched_yield()。
  在推屏前后读取 counter 差值：
    CPU 路径：推屏 25ms 期间该 counter 几乎不增长（CPU 被忙等占满）
    DMA 路径：该 counter 应有明显增长（调用线程在信号量上睡眠）
  这是一个相对比较，不需要绝对占用率，足以证明"时间片被释放"。
  探针代码只在验证期存在，不进入产品配置。

方案 ii：临时打开 CONFIG_SCHED_CPULOAD 做一次测量
  能拿到绝对百分比，但改动了配置本身，测量结果与产品配置不完全等价；
  只作为方案 i 的交叉验证，不作为唯一依据。
```

P4 门禁：有真机数据证明 DMA 路径下 CPU 在推屏期间可以被其他任务使用，
不是"看起来应该如此"的推断。同时记录第 1.3.2 节那个开放问题的实测结论
（挂钟耗时是否改善、改善多少），无论结果如何都要写进交付物——这是本
计划唯一能闭合 18ms 差距之谜的机会。

## 5. 验证矩阵

### 5.1 构建与静态检查

```text
-Werror 构建通过（CONFIG_BK7258_LCD_SPI_DMA 开与关两种配置都要过）
关闭该选项时，显示行为与改动前逐项一致；nuttx.bin 体积增量记录在案。
  理想情况是逐位一致，但 2.3.2 节对 bk7258_dma.c 的突发长度改动是无条件
  编译的（供摄像头路径共享同一份 configure_ex），所以逐位一致不作为硬
  门禁；若不一致，必须逐项说明差异来源，并确认摄像头 JPEG 通道行为不变
摄像头 JPEG 路径在 DMA 驱动扩展后回归通过（零值突发长度入参等价于扩展
  前的复位行为），这是 2.3.2 节向后兼容要求的验证
Kconfig 帮助文本准确描述收益边界：CPU 释放是承诺项，挂钟加速是待测项，
  不能让后续读者误以为已经承诺"让屏幕刷得更快"
```

### 5.2 正确性（画面层面）

```text
纯色填充（bk7258_gc9d01_fb_fill()）在 DMA 路径下颜色正确，与 CPU 路径
  逐字节比较面板实际收到的数据（如条件允许接线逻辑分析仪；否则至少
  肉眼确认色相和已知的 CPU 路径截图/描述一致）
四象限测试图（bk7258_gc9d01_fb_test_pattern()）在 DMA 路径下位置正确，
  验证 stride 和方向没有因为暂存区拷贝引入错位
LVGL 完整 UI（app/velasight 现有页面）在 DMA 路径下渲染正常，两块面板
  各自内容正确，没有互相污染（验证通道和暂存区没有被两条 bus 混用）
1000 次 open/推屏循环，字节序始终正确（不出现偶发花屏，这种问题通常是
  暂存区复用时序错误的信号）
```

### 5.3 时序与并发

```text
单次推屏耗时（挂钟时间）：DMA 路径不应显著劣于 CPU 路径基线（允许因为
  多一次暂存区拷贝和信号量调度产生的小幅增加，但不能出现数量级劣化）
DMA 完成中断到信号量唤醒的延迟：确认在合理范围内（不是本计划的性能
  瓶颈来源，但需要记录一个数量级供后续参考）
两块面板连续刷新（先 fb1 后 fb0）在 DMA 路径下不互相阻塞对方的通道
  （各自独立通道，验证同时有一个通道在跑时另一个通道的配置不受影响）
DMA 中断服务程序不执行 printf/malloc/命令发送（代码审查 + 运行时确认
  中断上下文耗时极短）
```

### 5.4 CPU 占用（本计划的核心收益指标，见 4.6）

```text
25ms 推屏窗口内，DMA 路径下其他任务的可调度时间显著优于 CPU 路径
长时间运行（例如 VelaSight 正常使用场景下的历史页面切换、状态刷新）
  期间，按键响应延迟或网络任务的及时性有可观察的改善或至少不劣化
```

### 5.5 回归

```text
camera_preview、hello_screen、jpeg_test 等非 LVGL 的 framebuffer 写入
  路径（如果它们与 DMA 开关共享同一份 bk7258_lcd_spi_write_frame()
  实现，就必然共享）在 DMA 路径下同样正确——这些路径不使用
  lv_draw_sw_rgb565_swap()（方案 1 未采用的原因之一），必须靠方案 3
  的驱动层暂存区交换来保证它们不需要修改
双屏初始化时间（~354ms 并行）不因为新增暂存区分配而明显增加
  （暂存区分配发生在 bk7258_lcd_spi_init()，需要确认这个时间点不在
  354ms 的关键路径上，或者影响可忽略）
关闭 CONFIG_BK7258_LCD_SPI_DMA 后，行为与本计划实施前逐项一致
```

### 5.6 长稳

```text
24 小时周期性推屏（模拟 VelaSight 正常使用节奏，不是极限压测），无
  DMA 通道状态卡死、无信号量泄漏（等待永远不返回）、无内存泄漏
  （暂存区是静态分配，理论上不应泄漏，但要验证没有意外的重复分配）
100 次冷启动，DMA 引擎初始化（P1 门禁的时机问题）在每次启动都正确完成
```

## 6. 明确不做（本轮）

```text
局部刷新的脏矩形 DMA（第 3.5 节）——需要动态 CASET/RASET 和跨步传输，
  复杂度明显更高，且当前产品是事件驱动整帧刷新，局部刷新收益有限
真正的异步 flush（方案 C，第 3.1 节）——需要改变 fb_vtable_s 的完成
  通知模型和/或 LVGL 侵入代码，且引入撕裂风险需要双缓冲配合，是独立
  的、更大的项目
方案 1（LVGL 侧 lv_draw_sw_rgb565_swap()，第 3.3 节）——已记录但不采用，
  因为它会把改动范围扩大到 app/velasight 且需要对非 LVGL 写入者做
  额外处理；如果方案 3 的交换开销被证明不可接受，重新评估此项
两块面板的真正并行推屏（第 3.2 节）——本轮只保证独立通道分配不成为
  障碍，不实现调用方的并行调度
音频 DMA 接入——bk7258_audio_dev.c 是独立的、已有文档预告的工作，本
  计划只在第 2.4 节记录通道号分配以避免未来冲突
```

## 7. 风险与明确决策

| 风险 | 性质 | 处置 |
|---|---|---|
| DMA 无法做字节序交换 | 已确认，硬件限制 | 决定方案 3（驱动层暂存区），方案 1 记录为后续选项 |
| `bk7258_dma_init()` 当前依赖摄像头路径触发 | 已确认，初始化顺序问题 | P1 必须显式调用，不能假设摄像头会先跑 |
| DMA 目的地址（QSPI 窗口）的可访问性未经 DMA 引擎实测 | 未确定 | P1 用探测性小块传输 + 面板可见性验证，不能只凭寄存器地图推断 |
| QSPI 数据窗口不在任何显式 MPU 区域内，退回后是可缓存 Normal 内存而非 Device | 已确认（写作过程中发现并纠正的错误假设，见第 2.2 节） | 现状下 D-Cache 全局关闭使其暂不显现；P0 决定维持现状推进还是先修复 `bk7258_start.c` |
| `dest_dev` 该填 MEM 还是某个未定义的 QSPI 请求线 | 已用原厂参考代码交叉验证 | 决定填 `BK7258_DMA_DEV_MEM`，与原厂 `lcd_spi_dma_single_mode_config()` 一致 |
| 暂存区引入撕裂窗口 | 已分析，当前设计下不成立 | 见 3.4 节：同步等待完成 + 驱动私有暂存区，两个前提都满足则无风险 |
| DMA 化被误解为"让单帧变快" | 认知风险，非技术风险 | 第 1.3.2 节把承诺项（CPU 释放）与待测项（挂钟加速）分开，Kconfig 帮助文本同步 |
| 现有 DMA 驱动无突发长度字段，DMA 化后可能吞吐毫无改善 | 已确认（第 2.3.2 节），且是最初范围设想的遗漏 | 扩展 `bk7258_dma_cfg_s`，向后兼容；纳入本计划范围 |
| SCK 实际频率未取证（`clk_rate = 0` 语义不明） | 未确定 | P0 必须定出 60MHz 还是 30MHz，否则 6.8ms/13.7ms 下限无从判断 |
| 暂存区 100KB 若从 SRAM 出会挤掉 `ai_agent` | 已确认（SRAM arena 仅 158KB，`ai_agent` 需约 200KB+32KB 栈） | 强制走 `BK7258_PSRAM_POOL_DISPLAY`，禁止 `kmm_zalloc()` |
| PSRAM 暂存区的写入耗时未知，可能成为新瓶颈 | 未确定 | P2 分别打点交换耗时与 DMA 耗时；若交换过慢则重估方案 1 |
| DMA 超时/配置失败路径未清理 QSPI 窗口模式位 | 设计缺陷（若不处理） | 所有提前返回必须先 `lcd_spi_quad_write_stop()`，见 4.4 节 |
| `bk7258_dma_init()` 重复调用会软复位整个单元，打断另一子系统的传输 | 已确认（第 4.3 节），不是"幂等" | 加一次性保护并提前到 bring-up 早期 |
| CPU 占用收益无法量化验证 | 已确认 `CONFIG_SCHED_CPULOAD` 未启用 | 第 4.6 节给出计数探针方案，不依赖未启用的配置 |
| 双通道分配与未来音频 DMA 冲突 | 尚未发生，提前记录 | 第 2.4 节写明通道表，要求后续者核对 |

## 8. 交付物

每个阶段提交必须包含：

```text
源代码与 Kconfig/defconfig
构建命令与最终 .config
nuttx、nuttx.bin、System.map 的 sha256
原厂取证（文件路径 + 函数名）与实板日志
测试命令、输出、CPU 占用对比数据
失败场景与恢复结果
nuttx.bin 体积增量
```

推荐拆分提交：

```text
docs: add BK7258 LCD QSPI DMA porting plan
docs: record BK7258 QSPI SCK rate and MPU coverage findings       (P0)
fix: make bk7258_dma_init() safe to call from two subsystems      (P1)
feat: add DMA burst length to bk7258_dma_configure_ex()           (P1)
feat: add BK7258 LCD SPI DMA byte-swap scratch buffer             (P2)
feat: switch bk7258_lcd_spi_write_frame() to DMA transfer         (P2)
feat: add CONFIG_BK7258_LCD_SPI_DMA build switch                  (P3)
docs: record BK7258 LCD DMA CPU-load and timing measurements      (P4)
```

## 9. 完成标准

只有同时满足以下条件才称为"LCD DMA 适配完成"：

1. `bk7258_lcd_spi_write_frame()` 对调用者的返回值语义和同步行为不变，
   `bk7258_gc9d01_fb.c`、`app/velasight`、以及所有其他 framebuffer 写入
   者不需要任何改动。
2. 字节序交换在 DMA 路径下产生与 CPU 路径逐像素一致的结果，已用至少
   一种可重复的方法验证（自动比较或有记录的肉眼验证步骤）。
3. `bk7258_dma_init()` 已加一次性保护，DMA 引擎初始化不依赖摄像头路径
   是否启用；显示与摄像头并发运行时互不打断对方的传输（双向验证）。
4. `bk7258_dma_cfg_s` 的突发长度扩展向后兼容：摄像头 JPEG 通道行为与
   扩展前一致，有回归证据。
5. 两块面板各自使用独立 DMA 通道，通道号已登记，不与现有 JPEG 通道
   （0 号）冲突。
6. DMA 完成中断服务程序符合第 0 节安全规则 4（不阻塞、不分配、不打印）。
7. 所有失败路径（配置失败、DMA 超时）都清理了 QSPI 窗口模式位，一次
   失败不会导致后续所有推屏永久失效，有故障注入验证。
8. 暂存区来自 PSRAM 池而非 kmm 堆，`ai_agent` 仍能正常启动（SRAM 余量
   未被挤占）。
9. 有真机数据证明 DMA 路径下 CPU 在推屏窗口内可以被其他任务使用，量化
   优于纯 CPU 路径基线（第 4.6 节的计数探针）。
10. 第 1.3.2 节的开放问题已闭合并记录：SCK 实际频率已取证；字节交换
    耗时与 DMA 传输耗时已分别测得；挂钟耗时相对基线的变化已如实记录，
    无论是改善、不变还是劣化。
11. `CONFIG_BK7258_LCD_SPI_DMA` 关闭时的显示行为与实施前逐项一致。
12. 1000 次连续推屏、100 次冷启动、24 小时长稳全部通过，无花屏、无卡死、
    无信号量超时。
13. Kconfig 帮助文本和本计划都把"CPU 释放（承诺）"与"挂钟加速（待测）"
    分开表述，避免后续维护者误解验收标准。

## 10. 稳定引用

本仓库内的既有实现与结论：

```text
board/beken/chips/bk7258/bk7258_qspi.c                  bk7258_lcd_spi_write_frame()，
                                                         现有 CPU 拷贝循环与
                                                         g_bus[] 窗口地址表
board/beken/chips/bk7258/include/bk7258_qspi.h          对外 API 契约
board/beken/chips/bk7258/bk7258_dma.c                   通用 DMA 引擎实现，
                                                         bk7258_dma_configure_ex()、
                                                         bk7258_dma_init() 的单元级
                                                         软复位（第 4.3 节）、
                                                         "burst 留在复位值" 的
                                                         注释（第 2.3.2 节）
board/beken/chips/bk7258/include/bk7258_dma.h           struct bk7258_dma_cfg_s，
                                                         BK7258_DMA_DEV_* 常量，
                                                         缺失的突发长度字段
board/beken/chips/bk7258/bk7258_psram.c                 kmm_addregion() 把 PSRAM
                                                         并入系统堆（解释 fb1 为何
                                                         落在 PSRAM），以及
                                                         SRAM arena 158KB /
                                                         ai_agent 200KB 的预算记录
board/beken/chips/bk7258/bk7258_camera_imgdata.c        唯一现存的
                                                         bk7258_dma_configure_ex()
                                                         使用者（通道 0），
                                                         初始化时机的先例
board/beken/chips/bk7258/bk7258_start.c                 MPU 区域表，
                                                         framebuffer 与 QSPI
                                                         窗口的缓存属性
board/beken/chips/bk7258/hardware/bk7258_memorymap.h    QSPI0/1 数据窗口基址
board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01_fb.c  fb_vtable_s、
                                                         gc9d01_push()，
                                                         "是否需要 DMA" 的
                                                         既有决策注释
board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01.c CASET/RASET 窗口设置，
                                                         不受本计划影响
board/beken/boards/bk7258/bk7258-ap/src/bk7258_status_screen.c  事件驱动刷新
                                                         策略，说明当前
                                                         25ms 忙等的实际
                                                         触发频率
board/beken/chips/bk7258/include/bk7258_psram.h         BK7258_PSRAM_POOL_DISPLAY，
                                                         暂存区可复用的
                                                         既有内存池
app/velasight/vs_display.c                              LVGL 双屏创建与
                                                         lv_refr_now()
                                                         同步刷新调用点，
                                                         确认不需要改动
docs/plans/VELASIGHT_MCU_APP_DEVELOPMENT_PLAN.md        VelaSight 显示链路
                                                         的既定设计，本
                                                         计划不重新讨论
docs/plans/BK7258_OPENVELA_ADC_PORTING_PLAN.md          文档结构与分层
                                                         约定的参考模板
摄像头取帧接口与显示参数.md                              已测得的推屏耗时
                                                         与局部刷新限制
logs/runtime/velasight-final-boot-*.log                 实测推屏耗时日志
```

NuttX/LVGL 框架参考：

```text
contest/nuttx/drivers/video/fb.c                        FBIO_UPDATE ->
                                                         vtable->updatearea()
                                                         的同步调用路径
contest/nuttx/include/nuttx/video/fb.h                  struct fb_vtable_s，
                                                         CONFIG_FB_SYNC/
                                                         waitforvsync（本
                                                         计划未使用）
contest/apps/graphics/lvgl/lvgl/src/drivers/nuttx/lv_nuttx_fbdev.c
                                                         flush_cb()，mmap +
                                                         LV_DISPLAY_RENDER_MODE_DIRECT，
                                                         确认不需要改动
contest/apps/graphics/lvgl/lvgl/src/draw/sw/lv_draw_sw.c
                                                         lv_draw_sw_rgb565_swap()，
                                                         方案 1（未采用）
                                                         的实现来源
contest/apps/graphics/lvgl/lvgl/src/display/lv_display.h
                                                         关于 RGB565 swap
                                                         的官方说明注释
```

BK7258 原厂参考（LCD DMA 配置）：

```text
bk_avdk_smp/ap/middleware/driver/lcd/lcd_spi_driver.c   lcd_spi_dma_single_mode_config()，
                                                         lcd_spi_dma_repeat_mode_config()，
                                                         dst.addr_inc_en 的
                                                         依据来源
bk_avdk_smp/ap/include/driver/hal/hal_dma_types.h       dma_config_t，
                                                         dma_pixel_trans_type_t
                                                         （已排除的方案 2）
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/dma_reg.h   DMA_V_REQ_MUX_* 请求线
                                                         编码，确认无 QSPI
                                                         专用请求线
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/dma_struct.h dma_hw_t 寄存器布局，
                                                         req_mux.pixel_trans_type
                                                         字段位置
bk_avdk_smp/ap/components/bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c
                                                         面板设备表与
                                                         lcd_spi_display_fill_pure_color()
                                                         的大端填充惯例
                                                         （方案 3 决策依据）
```

行号不是稳定接口；后续引用优先使用函数名、宏名和文件路径，并在每次原厂
SDK 更新后重新执行 P0 取证。
