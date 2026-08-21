# BK7258 OpenVela SARADC（ADC）移植适配计划

> 文档版本：V1
>
> 文档状态：2026-08-20 设计基线，**尚未实现**。当前仓库不存在任何 SARADC
> 驱动、寄存器映射、Kconfig 选项或 defconfig 条目；`CONFIG_ANALOG`、
> `CONFIG_ADC` 在两个 defconfig 中均未设置。本计划为全新移植，不是对已有
> 实现的补充说明。
>
> 适用范围：BK7258 AP/CPU1 上的 OpenVela/NuttX 通过 NuttX 原生
> `struct adc_dev_s` 上半部暴露 `/dev/adc0`，用于板级模拟量采集（电池电压、
> 芯片温度、外部模拟输入）。
>
> 本文所说的 ADC 一律指通用 **SARADC**（`SOC_SADC_REG_BASE 0x45890000`），
> 与已经移植完成的**音频 ADC**（AUD 块，`0x47800000`，见 `bk7258_aud.c`）
> 是两个互不相关的外设。仓库中现有的全部 `ADC` 字样都属于后者。

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

原厂 BK7258 参考源码为：

```text
/home/mi/vela_competition/bk_avdk_smp
```

仓库内的 `external/bk_avdk_smp/` 只是 CP 侧裁剪镜像，**不含任何 ADC 文件**；
所有原厂 ADC 取证必须回到上面的完整 SDK 路径。

执行期间遵守以下安全规则：

1. 在 `dev_id`/`dev_version` 探测通过之前，禁止对 SARADC 数据通路寄存器做
   任何写入。
2. 在通道所有权（第 3 节）结论确定之前，禁止在产品配置 `ai_agent` 中默认
   打开 ADC。
3. 任何 ADC 通道启用前必须先完成本板 pinmux 冲突核对（第 2.5 节），禁止在
   已被按键、双屏 QSPI、摄像头或 bit-bang I2C 占用的 GPIO 上启用模拟功能。
4. 模拟寄存器 `ana_reg2` 的写入必须走 analog-SPI busy 轮询，禁止裸
   `putreg32()`（第 2.3 节）。
5. 不在 ADC ISR 中执行 PWC 请求、mailbox 同步等待、动态内存分配或逐字打印。
6. CP 侧原厂 SARADC 实现和 `CONFIG_SARADC_MB` 现状是既有结论，不得为了让
   AP 取数而随手改回（第 3.3 节）。
7. 所有生成配置、ELF、map 和测试日志必须记录构建目录、时间、源码状态和 hash。

## 1. 目标与当前状态

### 1.1 目标架构

```text
NSH / VelaSight App（电量、温度）
        |
        v
NuttX ADC 上半部字符设备 /dev/adc0（struct adc_msg_s，ANIOC_TRIGGER）
        |
        v
BK7258 ADC 下半部 struct adc_dev_s / struct adc_ops_s
        |
        v
        +---- 路线 A：直接寄存器（SARADC 0x45890000）
        |
        +---- 路线 B：mb_ipc socket -> CP0 SARADC_SERVER -> CP 原厂 adc_driver.c
        |
        v
BK7258 SARADC 模拟前端（ana_reg2 gadc_*）+ 通道 pinmux
```

路线选择不是实现细节，而是本计划的核心决策，见第 3 节。CPU2 不参与 ADC。

### 1.2 当前基线（逐项取证）

截至 2026-08-20：

| 项目 | 当前状态 | 证据 |
|---|---|---|
| chip 层 SARADC 驱动 | 不存在 | `board/beken/chips/bk7258/` 无 `bk7258_adc.c` |
| SARADC 寄存器映射 | 不存在 | `hardware/` 无 `bk7258_adc.h`/`bk7258_saradc.h` |
| `CONFIG_BK7258_ADC` | 不存在 | chips/boards 两个 `Kconfig` 均无 |
| `CONFIG_ANALOG` / `CONFIG_ADC` | 未设置 | `configs/{nsh,ai_agent}/defconfig` 均无 |
| NuttX ADC 上半部 | 已具备，未启用 | `contest/nuttx/drivers/analog/adc.c`，`adc_register()` |
| SARADC IRQ 定义 | 不存在 | `chips/bk7258/include/irq.h` 无 SARADC 条目 |
| mailbox `0x4c` SARADC 通知 | 已应答，但当前构建中不会触发 | `bk7258_mailbox_channel.c:796-805`，第 3.3 节 |
| 音频 ADC | 已实现，与本计划无关 | `bk7258_aud.c`、`bk7258_audio_dev.c` |
| CP 侧 SARADC | 原厂 server 全量启用，运行期在用 | 第 3.1、3.2 节 |

仓库中出现的 `ADC` 命中项已全部核对：除 mailbox `0x4c` 通道外，其余均属
AUD 音频链路（`bk7258_aud.c`、`bk7258_audio_dev.c`、`hardware/bk7258_aud.h`、
`include/bk7258_aud.h`）。因此本移植是零基线起步。

### 1.3 工作区状态记录

在引用原厂寄存器定义和配置之前执行：

```bash
cd /home/mi/vela_competition
git -C bk_avdk_smp status --short
git -C contest/contest2026_264_VelaSightsuixingAIzhinengyanjing status --short
git -C contest/contest2026_264_VelaSightsuixingAIzhinengyanjing log -1 --oneline
```

若原厂或目标仓库有修改，不得把当前产物称为“未修改原厂最终固件”。应记录
仓库 commit 或工作树状态、构建时间、config 与 ELF/map/bin 的 sha256。

底层门禁使用 `contest/cmake_out/bk7258-ap_nsh`；产品集成验证使用
`contest/cmake_out/bk7258-ap_ai_agent`。

## 2. 硬件事实（已取证）

### 2.1 寄存器块与基地址

```text
SOC_SADC_REG_BASE = 0x45890000 + SOC_ADDR_OFFSET
```

证据：`bk_avdk_smp/ap/include/soc/bk7258/reg_base.h:96`。AP 以
`CONFIG_ARCH_TRUSTZONE_SECURE` 构建，对应原厂 `CONFIG_SPE` 分支，
`SOC_ADDR_OFFSET` 为 `0`（同文件 `:22`），因此正式代码使用：

```text
BK7258_SARADC_BASE = 0x45890000
```

这与本 port 既有约定一致（TRNG `0x458c0000`、UART1 `0x45830000`、
I2C1 `0x45860000`、AUD `0x47800000`），并且 `reg_base.h` 中**没有** SADC 的
MPC 条目，即该块不在 PSRAM/QSPI/FLASH/SMEM 那一类内存保护控制器后面。
“AP 总线可达”仍必须由 P1 的 `dev_id` 探测证明，不能由缺少 MPC 推断。

### 2.2 寄存器映射

原厂 `adc_hw_t` 为逐字排列的结构体，字索引即字节偏移的 1/4。来源：
`bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/adc_struct.h`，访问器见
`.../hal/adc_ll.h`。

| 偏移 | 名称 | 说明 |
|---:|---|---|
| 0x00 | `DEV_ID` | 只读器件 ID |
| 0x04 | `DEV_VERSION` | 只读版本 |
| 0x08 | `GLOBAL_CTRL` | bit0 `soft_reset`，bit1 `clk_gate_bypass` |
| 0x0c | `DEV_STATUS` | 只读状态 |
| 0x10 | `CTRL` | 主控制/状态，见下表 |
| 0x14 | `RAW_DATA` | 未经饱和处理的原始输出 |
| 0x18 | `STEADY_CTRL` | FIFO 水位、稳定时间、校准 |
| 0x1c | `SAT_CTRL` | 饱和处理与溢出标志 |
| 0x20 | `ADC_DATA` | bit[15:0] 饱和处理后数据 |
| 0x24 | `FIFO_DATA` | bit[15:0] FIFO 读口，校准后输出 |

`CTRL`（0x10）位域：

```text
[1:0]   adc_mode      0 power down / 1 single step / 2 software control / 3 continuous
[2]     adc_en
[6:3]   adc_channel   0-5 数字通道，其余为模拟通道
[7]     adc_setting   采样等待周期 0=4 cycles，1=8 cycles
[8]     adc_int_clear
[14:9]  adc_div       adc_clk = clk / [2 * (adc_div + 1)]
[15]    adc_32m_mode  置 1 时 adc_div 失效
[21:16] adc_samp_rate 仅连续模式有效，period = (16 + adc_samp_rate) * adc_clk
[28:22] adc_filter    输出速率 = period / (adc_filter + 1)；>64 时累加值除 2 输出
[29]    adc_busy      只读
[30]    fifo_empty    只读，1 = 空
[31]    fifo_full     只读，1 = 满
```

`STEADY_CTRL`（0x18）位域：

```text
[4:0]   fifo_level            仅连续模式有效，FIFO 数量超过该值产生中断
[7:5]   steady_ctrl           上电后等待 (steady_ctrl + 1) * 8 个 adc_clk
[8]     calibration_triggle   写 1 启动自动校准
[9]     calibration_done      校准完成置位并产生中断
[10]    bypass_calibration    绕过校准结果，不对 RAW_DATA 做偏移修正
[11]    bypass_window
[12]    auto_calib
[13]    algoth_sel
[14]    soft_pwd
```

`SAT_CTRL`（0x1c）位域：`[1:0] sat_ctrl`、`[2] sat_enable`、`[3] over_flow`。
12 位精度下 `sat_ctrl` 的取位规则为 `00: [18:3]+round[2:0]`、
`01: [17:2]+round[1:0]`、`10: [16:1]+round[0]`、`11: [15:0]`；原厂初值为
`ADC_SATURATE_MODE_1`（`adc_ll.h` 末尾 `ADC_INIT_SATURATE_MODE`）。

**可直接复用的结论**：`DEV_ID / DEV_VERSION / GLOBAL_CTRL{soft_reset,
clk_gate_bypass} / DEV_STATUS` 这一段与 TRNG 完全同构
（对照 `hardware/bk7258_trng.h`）。SARADC 与 TRNG 共用同一种 PRRO 外设包装，
因此 `bk7258_trng.c` 的“读 ID + 版本、复位、旁路时钟门、探测失败返回
`-ENODEV` 且不阻断 bring-up”那一整套写法可以照搬，包括
`hardware/bk7258_trng.h:5-17` 的出处注释格式。

### 2.3 时钟、电源与模拟域

三件事必须同时成立，缺一项都会得到“寄存器可写、读数恒定”的静默失败。

**（1）模块时钟门与源选择。** 两个寄存器在本 port 中**已经存在定义**，见
`bk7258_pwm.c:22-23`：

```text
BK7258_SYS_CLKSEL     = 0x44010020   bit17 = cksel_sadc
BK7258_SYS_CLK_ENABLE = 0x44010030   bit5  = sadc_cken
```

证据：`ap/middleware/soc/bk7258_ap/hal/sys_ll.h` 中
`sys_ll_set_cpu_clk_div_mode1_cksel_sadc()` 落在 `SOC_SYS_REG_BASE + (0x8<<2)`，
`sys_ll_set_cpu_device_clk_enable_sadc_cken()` 落在 `SOC_SYS_REG_BASE + (0xc<<2)`；
位号由 `soc/sys_struct.h` 的 `cksel_sadc: bit[17]`、`sadc_cken: bit[5]` 给出。
PWM 用的 `cksel_pwm0` 紧邻 `cksel_sadc`（bit18）、`pwm0_cken` 紧邻
`sadc_cken`（bit3），两侧互为交叉验证。这意味着 `bk7258_pwm_setup()` 的
“时钟选择 -> 时钟使能 -> 模块复位 -> pinmux”序列可以原样作为
`ao_setup()` 的开场。

**（2）模拟前端 `ana_reg2`。** 地址 `SOC_SYS_REG_BASE + (0x42 << 2)` =
`0x44010108`，与本 port 已有的 `BK7258_ANA_REG(2)`
（`hardware/bk7258_aud.h:229`，`SYSCTRL_BASE + 0x100 + N*4`）完全一致。位域：

```text
[7:0]   xtalh_ctune
[8]     force_26mpll
[10:9]  gadc_cmp_ictrl
[12:11] gadc_inbuf_ictrl
[14:13] gadc_refbuf_ictrl
[15]    gadc_nobuf_enable
[16]    vref_scale
[17]    scal_en
[18]    gadc_capcal_en
[24:19] gadc_capcal
[31:25] sp_nt_ctrl
```

原厂上电与配置序列（`ap/.../hal/sys_hal.c` `sys_hal_sadc_pwr_up()` 与
`sys_hal_set_saradc_config()`，CP 侧同名函数一致）：

```text
上电：  sadc_cken = 1；gadc_nobuf_enable = 1
配置：  sp_nt_ctrl = 0x3；gadc_capcal = 0；gadc_nobuf_enable = 1；
        gadc_refbuf_ictrl = 0x2；gadc_inbuf_ictrl = 0x2；gadc_cmp_ictrl = 0x2
掉电：  gadc_nobuf_enable = 0；sadc_cken = 0
```

`adc_ll_init()` 自身还会再写一次 `sp_nt_ctrl = 0x3` 和
`gadc_refbuf_ictrl = 0x2`，说明这两项是 SARADC 正常工作的必要条件而不是调优项。

**这里有一个已知会静默失败的陷阱。** `ana_regN` 不是普通 MMIO：SoC 把每次
写入通过内部 SPI 链路转发给模拟块，软件必须轮询 per-register busy 位。本
port 已经踩过这个坑并把结论写进了 `hardware/bk7258_aud.h:200-227`：状态寄存器
为 `BK7258_ANA_SPI_STATE = SYSCTRL_BASE + 0xe8`，等待位就是寄存器号
（本 AP 配置未设 `CONFIG_ANA_REG_WRITE_POLL_REG_B`）。裸 `putreg32()` 会让
连续写互相覆盖，而事后读回正常，故障只表现为“没有音频”或此处的“ADC 读数
不动”。

现成实现是 `bk7258_aud.c:198` 的 `aud_ana_write()`，但它是 `static`，
`include/bk7258_aud.h` 未导出（已核对）。因此 P1 需要做一次小的结构决定：

```text
方案 1：把 ana 读改写提升为共享 helper（例如 bk7258_anareg.c），
        AUD 与 ADC 同时改用它，并在其中加一把锁；
方案 2：ADC 内部再实现一份同样的轮询写。
```

推荐方案 1。理由不是去重：`ana_reg2` 与 AUD 用的 `ana_reg18/19/20/21/27`
是不同寄存器，字段本身不冲突，但 **busy 状态寄存器 `0x440100e8` 是共享的**，
音频与 ADC 并发写模拟块时需要串行化。这一点在只有 AUD 一个使用者时不存在，
加入 ADC 后才成立。

**（3）AON 电源域投票。** CP 的 `adc_driver.c:424` 在
`bk_adc_driver_init()` 中执行：

```c
bk_pm_module_vote_power_ctrl(PM_POWER_SUB_MODULE_NAME_BAKP_SADC,
                             PM_POWER_MODULE_STATE_ON);
```

这是 AON/PMU 子模块电源投票，AP 侧没有对应通路。是否为 AP 取数的必要条件、
以及能否/需否经 PWC mailbox 请求，是 **P1 的待取证项**，不能假设“时钟开了
就够”。参照 `bk7258_motor.c:55-74` 的 PWM 时钟投票写法作为格式模板，但
module id 与语义必须单独取证。

### 2.4 中断

```text
INT_SRC_SARADC 外部索引 = 8，GROUP0，IQR_PRI_DEFAULT
```

证据：`ap/middleware/soc/bk7258_ap/soc/icu_map.h:38`。按本 port 既有换算
（`include/irq.h:24` `BK7258_IRQ_FIRST = 16`）：

```c
#define BK7258_EXTIRQ_SARADC 8
#define BK7258_IRQ_SARADC    (BK7258_IRQ_FIRST + BK7258_EXTIRQ_SARADC)  /* 24 */
```

必须同时确认 CPU1 路由位 8 写在 `BK7258_CPU1_IRQ_EN0`（不是 CPU0 的同名
寄存器），这一点 `hardware/bk7258_aud.h` 关于 AUDIO IRQ 的注释已有明确说明，
照同样方式处理。单次转换（P2）不需要中断；中断只在连续模式（P4）才引入。

### 2.5 通道到 GPIO 映射与本板冲突分析

原厂通道表（`ap/middleware/soc/bk7258_ap/soc/adc_map.h:27-44`）与 pinmux
功能索引（`ap/middleware/soc/bk7258_ap/soc/gpio_map.h`，每个 pin 有 8 个功能
槽，索引即写入 sys gpio_config 的 4 位 perial mode）交叉后，对照本板已占用
引脚，结果如下。**这是本计划最重要的一张表**：

| 通道 | 引脚 | 功能索引 | 本板现有用途（证据） | 可用性 |
|---|---|---:|---|---|
| ADC0 | 内部 Vbat | — | 无需引脚 | 受 `adc_cap.h` 下限排除 |
| ADC1 | GPIO25 | 2 | GC9D01 背光 `GC9D01_BACKLIGHT_PIN 25` | 占用 |
| ADC2 | GPIO24 | 2 | 双屏 LCD2 `FL_QSPI_D0 = P24`（`bk7258_qspi.c`） | 占用 |
| ADC3 | GPIO23 | 2 | 双屏 LCD2 `FL_QSPI_CS = P23` | 占用 |
| ADC4 | GPIO28 | 2 | 摄像头 `DVP_RESET_PIN 28` | 占用 |
| ADC5 | GPIO22 | 2 | 双屏 LCD2 `FL_QSPI_CLK = P22` | 占用 |
| ADC6 | GPIO21 | 2 | 未发现任何软件使用者 | **唯一候选** |
| ADC7 | 内部 vtemp | — | 无需引脚 | 受 `adc_cap.h` 上限排除 |
| ADC8/9/11 | — | — | `adc_map.h` 标 `INVALID` | 不可用 |
| ADC10 | GPIO8 | 4 | `BOARD_KEY_VOLUME_DOWN_GPIO 8` | 占用 |
| ADC12 | GPIO0 | 4 | `I2C_SIM_SCL_PIN 0`（bit-bang I2C） | 占用 |
| ADC13 | GPIO1 | 4 | `I2C_SIM_SDA_PIN 1` | 占用 |
| ADC14 | GPIO12 | 4 | `BOARD_KEY_POWER_GPIO 12` | 占用 |
| ADC15 | GPIO13 | 4 | `BOARD_KEY_VOLUME_UP_GPIO 13` | 占用 |

结论有三条，都会直接改变实施范围：

1. **本板几乎没有空闲的外部 ADC 引脚。** 15 个通道里 12 个落在已被三键、
   双屏 QSPI0、摄像头复位或模拟 I2C 占用的 pad 上，唯一软件层面空闲的是
   ADC6/GPIO21。GPIO21 的实际网络必须在 P0 用原理图
   （`AIDK_AI玩具开发板_原理图.pdf`，仓库内未随附）确认；软件没人用不等于
   焊盘悬空可用。
2. **产品真正需要的两路（电池电压、芯片温度）恰好是内部通道** ADC0(Vbat)
   与 ADC7(vtemp)，不占引脚，因此不受上面的冲突表限制。
3. **但这两路被原厂能力上限挡住**：`ap/include/soc/bk7258/adc_cap.h` 声明
   `SOC_ADC_CHAN_MIN 1`、`SOC_ADC_CHAN_MAX 6`，即原厂自己的参数校验不接受
   0 和 7。同一文件顶部还留着原厂的未决注释（大意为通道是否从 1 开始、
   模拟通道是否从 7 开始两项待确认）。而 `adc_ll.h` 的
   `adc_ll_is_analog_channel()` 注释又明确写出 ADC0=Vbat、ADC7=vtemp，且
   `ANALOG_CHANNEL = 0x0B81`（bit0、7、8、9、11）把 0 和 7 归为模拟通道。
   两处互相矛盾，**这个矛盾必须在 P0 用 CP 侧实测数据判定，不能选一边相信**。
   有利的一点是：CP 的 `volt_detect.c` 实际就用
   `ADC_VOLT_SENSER_CHANNEL = 0`（`temp_detect_pub.h:36`）在跑，说明通道 0
   在真机上是可用的，`adc_cap.h` 的下限更像是校验宏没跟上。

### 2.6 采样参数与已验证的可用配置

CP 的 `volt_detect.c:105-140` 是这颗硅片上**已经在跑**的一组配置，作为首版
参考基线远优于自己推导：

```text
adc_mode      = ADC_CONTINUOUS_MODE
src_clk       = ADC_SCLK_XTAL_26M
clk           = 750000          （TEMP_DETEC_ADC_CLK）
sample_rate   = 32              （TEMP_DETEC_ADC_SAMPLE_RATE）
steady_ctrl   = 7               （TEMP_DETEC_ADC_STEADY_CTRL）
adc_filter    = 0
saturate_mode = ADC_SATURATE_MODE_3（BK7258 属 CONFIG_SOC_BK7236XX 分支）
校准          = bk_adc_enable_bypass_clalibration()
采样条数      = ADC_TEMP_BUFFER_SIZE = 5 + 5，前 5 条丢弃
```

`adc_div` 由 `adc_hal_set_clk()` 推导：`pre_div = 26000000 / 2 / adc_clk - 1`，
对 750 kHz 得 `pre_div = 16`。`SOC_ADC_SAMPLE_CNT_MAX = 32`。

注意“前 5 条丢弃”不是玄学：`steady_ctrl` 只保证模拟前端稳定时间，FIFO 头部
仍会带上电瞬态。首版实现必须保留同样的丢弃策略，否则第一个 `read()` 的值
不可用。

## 3. 所有权：本计划的核心决策

### 3.1 原厂架构是 CP 拥有 SARADC，AP 只是 IPC 客户端

这一点与 PWM/TRNG/SDIO 都不同，必须先讲清楚，否则整个实施顺序会走错。

```text
CPU0 / CP                                CPU1 / AP
---------                                ---------
adc_driver.c   （真正的寄存器驱动）
adc_ll.h / adc_hal.c
saradc_server.c ── mb_ipc socket ──────── saradc_client.c（纯转发，无寄存器路径）
   ^
   |
temp_detect.c / volt_detect.c（CP 本地使用者）
```

关键取证：

- AP 侧 `middleware/driver/saradc/` 下**只有** `saradc_client.c`，没有
  `adc_driver.c`。`saradc_client.c` 的每个入口都是打包
  `saradc_cmd_t` 后 `mb_ipc_send()` + `mb_ipc_recv()`，例如
  `bk_adc_single_read()` 发 `SARADC_CMD_SINGLE_READ` 并从 `cmd_buff.buff[0]`
  取回结果，全文没有任何寄存器访问。
- AP 侧确实带了 `adc_ll.h`、`adc_hal.c`、`adc_struct.h`、`adc_map.h`，
  所以**寄存器信息是齐的**；缺的是驱动主体，不是硬件描述。
- 仲裁点是 CP 的一把互斥锁。`adc_driver.c:359-372`：

  ```c
  bk_err_t bk_adc_acquire(void)
  {
      ...
      ret = rtos_lock_mutex(&s_adc_dev.adc_mutex);
      mb_saradc_op_prepare();
      return ret;
  }
  ```

  AP 客户端的 `acquire` 经 IPC 映射到这把锁。这是全系统唯一的 SARADC 仲裁
  机制，没有第二个。

### 3.2 CP 在运行期确实持续占用 SARADC

这不是理论风险。本项目 CP 配置：

```text
projects/app_ab/cp/config/bk7258/config:602   CONFIG_TEMP_DETECT=y
projects/app_ab/cp/config/bk7258/config:604   CONFIG_VOLT_DETECT=y
                                       :887   CONFIG_SARADC=y
                                       :888   CONFIG_SARADC_SERVER=y
                                       :891   CONFIG_SARADC_PM_CB_SUPPORT=y
                                       :895   # CONFIG_SARADC_MB is not set
                                       :596   # CONFIG_SARADC_CALI is not set
```

生成配置一致（`build/bk7258/app_ab/bk7258/config/sdkconfig.h:167,168,253-255`）。

两个探测任务各自的一次测量都是完整占用序列
`acquire -> init -> set_config -> bypass_calib -> start -> read_raw(10) ->
stop -> deinit -> release`，其间会重写 `CTRL`、切换通道、清空 FIFO。周期为
（`temp_detect.h:75-77`）：

```text
开机后前 30 秒：每 1 秒一次   （ADC_TMEP_DETECT_INTERVAL_INIT = 1，
                                ADC_TMEP_DETECT_INTERVAL_CHANGE = 30）
之后：          每 15 秒一次 （ADC_TMEP_DETECT_INTERVAL = 15）
```

温度与电压两路独立计时，即启动阶段每秒两次占用。**AP 侧任何不经 CP 锁的
SARADC 访问，都是与之无仲裁的并发。**

### 3.3 `CONFIG_SARADC_MB` 已关闭，以及由此产生的后果

`MB_CHNL_SARADC`（本 port 的 `BK7258_MB_CHAN_SARADC_RX = 0x4c`）不是数据
通道，而是 SARADC 操作起止广播。本项目在 2026-08-10 明确关掉了它，原因和
结论记录在 `docs/archive/BK7258_OPENVELA_SMP_PORTING_PLAN.md`：启动期反复出现
`0x4c` transaction timeout 与 transport recovery，抢占了唯一的 physical
transaction，牵连 UART0/PWC `0x5` 链路；关闭后 CP 本地 ADC、温度和 RF 校准
保持启用，`saradc_notify.c` 恢复原厂状态不再作为比赛仓覆盖文件。

由此产生两个必须写进计划的推论：

1. `bk7258_mailbox_channel.c:796-805` 那段 `0x4c` 应答分支，在当前 CP 构建
   下**不会被触发**（`mb_saradc_op_prepare()` 退化为本地回调，不再
   `mb_chnl_write`）。它是防御性保留代码，不是可用的“CP 会告诉我它在用
   ADC”的通知机制。任何依赖“监听 0x4c 就能避让 CP”的设计都是错的。
2. 想通过重新打开 `CONFIG_SARADC_MB` 来获得避让，等于回退一个已经用实板
   定位过的问题。不允许作为默认方案；若确有必要，必须作为独立阶段、带
   启动期 100 次冷启动门禁重新验证。

### 3.4 三条可行路线

**路线 A：AP 直接驱动 0x45890000。**

- 优点：与本 port 全部既有外设风格一致（TRNG/PWM/AUD/PSRAM 都不链接原厂
  SDK 代码，`chips/bk7258/libs/*.a` 中也没有 ADC 驱动）；寄存器信息已在
  第 2 节完整取证；无新增跨核协议。
- 缺点：**没有任何仲裁**。与 3.2 的 CP 周期占用直接冲突，且冲突表现为读数
  偶发错通道或错值，不是干净的报错。此外 AP 无法做 2.3(3) 的 AON 电源投票。
- 适用前提：能证明 SARADC 在运行期为 AP 独占。当前配置下**不成立**。

**路线 B：AP 实现 mb_ipc socket 客户端，走 CP 的 `SARADC_SERVER`。**

- 优点：仲裁天然正确，AP 的 `acquire` 就是 CP 的那把锁；不改 CP 源码，
  不动 `CONFIG_SARADC_MB`；`SARADC_CMD_*` 协议与 `saradc_cmd_t` 载荷已定义
  （`ap/middleware/driver/saradc/saradc_ipc.h`，30 个命令，
  `SARADC_IPC_READ_SIZE/WRITE_SIZE = 0x400`）；服务端口号由
  `mb_ipc_port_cfg.h` 固定为 `SARADC_SERVER`（CPU0）/`SARADC_CLIENT`（CPU1）。
- 缺点：需要在 AP 上实现 mb_ipc 的 socket/connect/send/recv 语义，而不只是
  现有的单事务 mailbox 通道。本仓库为 flash 服务做过一次并已移除
  （`bk7258_kvdb.c:219-223`：“the AP has no flash controller of its own, and
  reaching the CP's flash service over the mailbox is not something this
  board does -- that client was removed along with the CP-side changes it
  depended on”）。
- 现存资产比看起来多：`BK7258_MB_CHAN_IPC_TX 0x11` / `IPC_RX 0x41` 已在允许
  通道表内（`bk7258_mailbox_channel.c:119`），SWAP 内的
  `BK7258_IPC_TX_ADDRESS 0x2809f900`、`BK7258_FLASH_IPC_ADDRESS 0x2809f980`、
  `BK7258_FLASH_DATA_ADDRESS 0x2809fa00` 及其 `_Static_assert` 齐备，
  `bk7258_mb_ipc_tag()`/`_is_response()`/`_make_response()` 三个 ABI helper
  齐备，`bk7258_mailbox_channel.c:853-880` 还留着 IPC 通道必须回显 `param1`
  和 `ack_state` 的血泪注释（不回显会让 CP 路由 200ms 后拆链）。也就是说，
  这条路线的难点集中在 socket 状态机，物理层和 ABI 细节已经交过学费。
- 附带约束：描述符和载荷必须放在 SWAP，不能放 AP 自己的堆
  （`bk7258_mbox.h:128-146` 记录过“描述符在 AP RAM 时服务端静默丢弃请求”）。

**路线 C：不做 NuttX ADC，让 CP 发布它已经算好的电压和温度。**

- CP 的 `volt_detect`/`temp_detect` 已经在测本产品需要的两个量。若只为
  “屏幕上显示电量”，把这两个标量经现有 mailbox 通道推给 AP，成本远低于
  A 或 B，且完全没有并发问题。
- 缺点：需要 CP 侧新增发布代码，与本项目“尽量不覆盖原厂 CP 文件”的既有
  取向冲突；且不产生通用 ADC 能力，外部模拟输入仍然做不到。

### 3.5 决策

```text
若目标仅为电池电量/温度显示            -> 路线 C，并明确不交付 /dev/adc0
若目标为通用 ADC 且必须与 CP 共存      -> 路线 B
路线 A 仅在 P0 证明 SARADC 为 AP 独占时才允许，当前配置下不成立
```

默认推进路线 B，P0 的产物之一就是这个选择的书面依据。**不允许**先按路线 A
出一个“能读到数”的版本再补仲裁：无仲裁的读数在开机前 30 秒每秒与 CP 相撞，
而这恰好是最容易被当成“驱动基本能用”的观察窗口。

## 4. NuttX 接口基线

### 4.1 上半部与设备节点

上半部已在 `contest/nuttx` 中就绪，无需改动框架：

```text
nuttx/drivers/analog/adc.c            上半部，adc_register() 在 :728
nuttx/include/nuttx/analog/adc.h      adc_ops_s :157，adc_dev_s :205，adc_register :267
nuttx/include/nuttx/analog/ioctl.h    ANIOC_* 命令
nuttx/drivers/analog/Kconfig          menuconfig ANALOG（default n）-> config ADC
```

节点为 `/dev/adc0`，由 board 层注册。用户侧读出的是 `struct adc_msg_s`
（通道号 + 采样值），FIFO 深度由 `CONFIG_ADC_FIFOSIZE`（默认 8）决定，实际
可存条数为 `ADC_FIFOSIZE - 1`。

### 4.2 必须实现的 vtable

```c
struct adc_ops_s
{
  int  (*ao_bind)(struct adc_dev_s *dev,
                  const struct adc_callback_s *callback);
  void (*ao_reset)(struct adc_dev_s *dev);
  int  (*ao_setup)(struct adc_dev_s *dev);
  void (*ao_shutdown)(struct adc_dev_s *dev);
  void (*ao_rxint)(struct adc_dev_s *dev, bool enable);
  int  (*ao_ioctl)(struct adc_dev_s *dev, int cmd, unsigned long arg);
};
```

各钩子在本芯片上的落点：

| 钩子 | 路线 A 内容 | 路线 B 内容 |
|---|---|---|
| `ao_bind` | 保存上半部回调 | 同 |
| `ao_reset` | `GLOBAL_CTRL` soft_reset + `CTRL=0`，不碰 ana_reg2 | 无硬件动作，清本地状态 |
| `ao_setup` | cksel/cken -> 复位 -> ana_reg2 上电配置 -> pinmux | socket + connect + `SARADC_CMD_ACQUIRE/INIT/SET_CONFIG` |
| `ao_shutdown` | 停转换 -> `gadc_nobuf_enable=0` -> `sadc_cken=0` -> 还原 pinmux | `STOP/DEINIT/RELEASE` + close |
| `ao_rxint` | 使能/关闭 `BK7258_IRQ_SARADC`（P4 才实现） | 关联 CP ISR 回调（P4） |
| `ao_ioctl` | `ANIOC_TRIGGER` 单次；`ANIOC_GET_NCHANNELS` | 同，映射到 `SINGLE_READ` |

`ao_setup()` 会做 mailbox 同步等待（路线 B）或 analog-SPI 轮询（路线 A），
两者都只能在任务上下文执行。上半部在 `open()` 中调用 `ao_setup()`，满足这一
约束；但**禁止**从 `ao_rxint()` 或 ISR 内触发上述任何一项。

### 4.3 ioctl 与数据流

```text
ANIOC_TRIGGER         _ANIOC(0x0001)  触发一次转换（首版必须支持）
ANIOC_WDOG_UPPER      _ANIOC(0x0002)  不实现，返回 -ENOTTY
ANIOC_WDOG_LOWER      _ANIOC(0x0003)  不实现，返回 -ENOTTY
ANIOC_GET_NCHANNELS   _ANIOC(0x0004)  返回本驱动通道数（首版 1）
ANIOC_RESET_FIFO      _ANIOC(0x0005)  透传上半部
ANIOC_SAMPLES_ON_READ _ANIOC(0x0006)  透传上半部
```

首版数据流固定为“应用 `ioctl(ANIOC_TRIGGER)` -> 驱动完成一次采集 ->
`au_receive()` 入上半部 FIFO -> 应用 `read()`”。不实现自由运行的连续采样，
理由是 P4 之前没有中断，轮询式连续采集会在 480MHz CPU 上白烧周期，而本产品
的采样需求是秒级。

## 5. 代码落点与实施顺序

### 5.1 目标文件

```text
board/beken/chips/bk7258/hardware/bk7258_adc.h        新增：寄存器映射与出处
board/beken/chips/bk7258/bk7258_adc.c                 新增：adc_ops_s 下半部
board/beken/chips/bk7258/include/bk7258_adc.h         新增：initialize 原型
board/beken/chips/bk7258/include/irq.h                新增：EXTIRQ/IRQ_SARADC
board/beken/chips/bk7258/Kconfig                      新增：BK7258_ADC 及子选项
board/beken/chips/bk7258/CMakeLists.txt               条件编译（权威构建路径）
board/beken/chips/bk7258/Make.defs                    同步（见下）
board/beken/boards/bk7258/bk7258-ap/src/bk7258_adc_dev.c  新增：adc_register
board/beken/boards/bk7258/bk7258-ap/src/CMakeLists.txt
board/beken/boards/bk7258/bk7258-ap/src/bk7258_bringup.c
board/beken/boards/bk7258/bk7258-ap/configs/nsh/defconfig
board/beken/boards/bk7258/bk7258-ap/configs/ai_agent/defconfig
```

路线 B 额外需要：

```text
board/beken/chips/bk7258/bk7258_mb_ipc.c              新增：mb_ipc socket 客户端
board/beken/chips/bk7258/hardware/bk7258_mbox.h       新增 SARADC 描述符/载荷窗口
```

分层边界：chip 层负责寄存器、时钟、模拟域、IRQ、`adc_dev_s`；board 层负责
pinmux 选择、通道到板级语义的映射、`adc_register()`；PWC 层只负责
CP-compatible 的时钟/电源请求。

**构建文件注意**：`Make.defs` 已与 `CMakeLists.txt` 漂移（例如
`bk7258_trng.c` 只在后者中）。本项目以 `--cmake` 构建，`CMakeLists.txt` 是
权威路径，必须正确；`Make.defs` 同步添加以保持一致，但不作为验证依据。

### 5.2 P0：冻结输入、判定路线

不写任何驱动代码。产出：

1. 两个仓库工作树状态与 commit/hash。
2. 原理图核对 GPIO21 实际网络、是否引出可测焊盘、有无外部分压；同时确认
   GPIO0/1/8/12/13/21/22/23/24/25/28 的当前用途与第 2.5 节表一致。
3. 用 CP 侧现有能力读出通道 0（Vbat）与通道 7（vtemp）的真机数值，判定
   2.5(3) 中 `adc_cap.h` 与 `adc_ll.h` 的矛盾该信哪一边，并记录原始值。
4. 记录 CP `temp_detect`/`volt_detect` 的实际占用节奏（启动期与稳态各抓一段），
   作为 3.2 的实测证据而不只是配置推断。
5. 书面确定路线 A/B/C，并说明依据。若选 C，本计划到此转为 CP 发布通道设计，
   不再进入 P1。

P0 门禁：路线已定；通道 0/7 可用性有真机数据；引脚冲突表有原理图背书。

### 5.3 P1：可达性、时钟与模拟域（路线 A/B 共同）

先只做探测，不做转换：

```text
读 DEV_ID / DEV_VERSION，确认 AP 对 0x45890000 的读权限
按 2.3(1) 开 cksel_sadc / sadc_cken，复位后再读一次 DEV_STATUS
按 2.3(2) 用带 busy 轮询的 ana 写入设置 gadc_* 与 sp_nt_ctrl，回读确认
取证 2.3(3)：AON BAKP_SADC 投票是否为必要条件，若是则定出 PWC 请求方式
```

参照 `bk7258_trng_initialize()` 的探测风格：报告 `dev_id`/`version`，探测
失败返回 `-ENODEV` 并让 bring-up 打印警告而不中止。

P1 门禁：100 次冷启动均得到确定的 ID 读数或确定的失败；ana_reg2 写入可回读；
在时钟或电源请求失败时不访问数据通路寄存器。

### 5.4 P2：单次转换与 `/dev/adc0`（首个可用版本）

```text
实现 adc_ops_s 六个钩子的最小集（ao_rxint 先返回、不使能中断）
采用 2.6 的已验证配置，连续模式 + 丢弃前 5 条 + 取后 5 条
ANIOC_TRIGGER -> 完成一次采集 -> au_receive() -> read()
board 层 adc_register("/dev/adc0", dev)，bringup 中 best-effort 调用
```

首版只暴露一个通道，先用 P0 判定可用的内部通道（预期 Vbat）。

P2 门禁：`/dev/adc0` 稳定出现；连续 1000 次 TRIGGER+read 无卡死、无
`over_flow` 置位；读数在已知输入下落在合理区间且与 CP 侧读数量级一致。

### 5.5 P3：外部通道与 pinmux

仅在 P0 确认 GPIO21 可用时进行：

```text
bk7258_gpio_set_function(21, 2)   选中 ADC6
随后按原厂 adc_init_gpio() 的顺序：关 pull、关 input、关 output
```

现有 `bk7258_gpio_set_function()` 已经会置 `OUTPUT_DISABLE` 与
`SECOND_FUNCTION` 并清 `INPUT_ENABLE`/`PULL_ENABLE`，与原厂
`gpio_dev_map + disable_pull/input/output` 语义一致，因此无需新增 GPIO API。
`ao_shutdown()` 必须还原引脚，禁止把 pad 留在模拟功能上。

P3 门禁：外部已知电压（分压后）读数误差在标定范围内；关闭后引脚回到
原状态，双屏、按键、摄像头功能无回归。

### 5.6 P4：中断与连续采样（可选）

引入 `BK7258_IRQ_SARADC`、`fifo_level` 水位中断与 `ao_rxint()`。ISR 只做
“搬 FIFO + 调 `au_receive_batch()` + 清中断”，不做其他任何事。仅在确有连续
采样需求时才做；本产品的秒级需求不需要。

### 5.7 P5：校准与工程量换算

首版按 CP 现状使用 `bypass_calibration`。若需要绝对精度，再评估
`calibration_triggle`/`auto_calib` 与 `bk_adc_data_calculate()` 的换算系数。
原始码值到毫伏/摄氏度的换算放在应用层或 board 层，**不放进 chip 层驱动**：
换算依赖板级分压网络，属于板级知识。

P5 门禁：换算公式有出处，且在至少三点输入上与外部万用表/温度计比对。

### 5.8 P6：产品接入

VelaSight App 侧读取电量并显示。此阶段才允许在 `ai_agent/defconfig` 中默认
开启 ADC；在此之前只在 `nsh` 配置中开启。

## 6. 配置与构建执行清单

### 6.1 配置修改

`nuttx/drivers/analog/Kconfig` 中 `ANALOG` 是 gating menuconfig，只设
`CONFIG_ADC=y` 不会生效。需要同时加入（defconfig 内按块内字母序插入）：

```text
CONFIG_ANALOG=y
CONFIG_ADC=y
CONFIG_BK7258_ADC=y
```

`CONFIG_ADC_FIFOSIZE` 默认 8（可存 7 条），首版取样 5 条够用，不改。
`CONFIG_ADC_NPOLLWAITERS` 默认 2，不改。

chip 层 `Kconfig` 新增选项放在 `BK7258_TRNG` 附近，保持风格一致：

```text
config BK7258_ADC
	bool "BK7258 SARADC"
	default n
	depends on ARCH_CHIP_BK7258
	select ANALOG
	select ADC
	help
		（说明用途、与音频 ADC 的区别、与 CP 的所有权关系、
		 以及本板可用通道的限制）
```

通道选择用独立 int 选项表达，默认值必须是 P0 判定可用的那一个，不能默认
落在被占用的引脚上。

### 6.2 源码加入

```cmake
if(CONFIG_BK7258_ADC)
  list(APPEND SRCS bk7258_adc.c)
endif()
```

board 层 `src/CMakeLists.txt` 中加入 `bk7258_adc_dev.c`。bring-up 调用位置
参照 `bk7258_bringup.c` 中 TRNG 的写法：best-effort，失败只打印不返回错误。

### 6.3 构建与打包命令

底层门禁使用 `nsh`，产品验证使用 `ai_agent`。改过 `Kconfig`/`defconfig` 后
必须先 `distclean`：

```bash
cd /home/mi/vela_competition/contest

./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh --cmake distclean
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh --cmake -j8
```

产物：

```text
contest/cmake_out/bk7258-ap_nsh/nuttx
contest/cmake_out/bk7258-ap_nsh/nuttx.bin
contest/cmake_out/bk7258-ap_nsh/System.map
```

产品镜像与打包按 `docs/固件构建步骤.md`：AP `nuttx.bin` 复制为
`bk_avdk_smp/build/openvela-ap.bin`，再在 Podman 内
`make -C projects/app_ab bk7258 SDK_DIR=/armino
EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin`，最终得到
`projects/app_ab/build/bk7258/app_ab/package/all-app.bin`。
`build_and_flash.sh` 已把整套流程自动化。

AP raw linker 区域为 `3904K`（`0x3d0000`，见
`boards/bk7258/bk7258-ap/scripts/ld.script`），ADC 驱动的体积增量必须记录在
每次提交中。

应用侧验证可直接启用 `apps/examples/adc`（读 `/dev/adc0` 并驱动
`ANIOC_TRIGGER`），无需先写自研测试程序；若需要 PASS/FAIL 形式的门禁输出，
按 `app/periph_selftest/` 的四件套（`Kconfig`/`CMakeLists.txt`/`Make.defs`/
`Makefile`）加 `apps/packages/demos/contest2026_264_*` 符号链接。

## 7. 验证矩阵

### 7.1 构建与静态检查

```text
-Werror 构建通过
CMakeLists.txt 与 Make.defs 均已加入 bk7258_adc.c
CONFIG_ANALOG 与 CONFIG_ADC 同时为 y
未启用 ADC 时 nuttx.bin 体积与基线一致（条件编译真的生效）
ai_agent 配置在 P6 之前不得默认开启 ADC
路线 A：ELF 中不存在任何原厂 bk_adc_* 符号
路线 B：ana_reg2 相关写入不存在于 AP 侧
```

### 7.2 可达性与初始化

```text
100 次冷启动：DEV_ID/DEV_VERSION 读数稳定一致
时钟未使能时的读数与使能后的读数可区分（证明 cken 真的起作用）
ana_reg2 写入后回读一致；连续写多个字段不互相覆盖
时钟或电源请求失败时不访问数据通路寄存器
探测失败时 bring-up 继续，双屏、按键、摄像头、音频、Wi-Fi 无回归
```

### 7.3 采集正确性

```text
1000 次 TRIGGER + read：无卡死、无超时、over_flow 始终为 0
丢弃前 5 条策略生效：首次 read 的值与后续稳态值同量级
同一输入下 100 次采样的极差在噪声预期内
与 CP 侧同通道读数量级一致（跨核交叉验证）
读数不随双屏刷新、摄像头取帧、音频播放、Wi-Fi 收发而系统性偏移
```

### 7.4 并发与所有权（本移植最关键的一组）

```text
开机后 0-30 秒窗口内持续采样：与 CP 每秒两次的占用重叠时不得出现
    错通道值、跳变值或 FIFO 状态异常
稳态 15 秒周期窗口内同样验证
路线 B：acquire 竞争时返回有界的失败或有界等待，不无限阻塞
路线 B：socket 断链后可重连，不需要重启
路线 A（若获批）：必须给出独占性证据，否则本项判定为不通过
mailbox 统计：ADC 引入后 unsupported/timeout/recovery 计数不增长
```

### 7.5 引脚与回归

```text
启用外部通道后：三键、双屏、摄像头、音频、SD-NAND 全部功能回归通过
ao_shutdown 后引脚回到原配置，可用寄存器回读证明
未启用外部通道时，GPIO 配置与基线逐位一致
```

### 7.6 长稳

```text
24 小时周期采样，无泄漏、无计数器溢出、无读数漂移超限
100 次冷启动，/dev/adc0 每次都出现
1000 次 open/close 循环
```

## 8. 交付物与回滚

每个阶段提交必须包含：

```text
源代码与 Kconfig/defconfig
构建命令与最终 .config
nuttx、nuttx.bin、System.map 的 sha256
原厂取证（文件路径 + 函数/宏名）与实板日志
测试命令、输出、统计计数器
失败场景与恢复结果
nuttx.bin 体积增量
```

推荐拆分提交：

```text
docs: add BK7258 SARADC porting plan
docs: record BK7258 SARADC ownership and channel decision   （P0）
feat: add BK7258 SARADC register map and probe              （P1）
feat: add BK7258 SARADC lower-half and /dev/adc0            （P2）
feat: enable BK7258 SARADC external channel pinmux          （P3）
feat: add BK7258 SARADC interrupt-driven continuous mode     （P4，可选）
feat: expose battery level to VelaSight App                  （P6）
```

出现以下任一情况立即回退到最近一个通过的阶段，不得用提高采样次数、
换通道或加延时掩盖：读数无法解释地跳变、`over_flow` 置位、与 CP 并发时
出现错通道数据、ana_reg2 回读不一致、AON 电源语义不确定、mailbox
recovery 计数增长。

## 9. 完成标准

只有同时满足以下条件才称为“ADC 适配完成”：

1. 路线选择有书面依据，且与 CP 的所有权关系明确写入代码注释。
2. SARADC 与音频 ADC 的区别在代码和文档中不产生混淆。
3. 基地址、时钟位、`ana_reg2` 字段、IRQ 号全部经实板回读验证，不依赖注释推断。
4. `ana_reg2` 写入走 busy 轮询，且与音频路径的并发已串行化。
5. AON `BAKP_SADC` 电源语义已取证：或证明 AP 不需要，或有可靠的请求与确认。
6. 通道选择经原理图与实板确认，未占用按键、双屏、摄像头或模拟 I2C 引脚。
7. `adc_cap.h` 与 `adc_ll.h` 关于通道 0/7 的矛盾已用真机数据判定并记录。
8. 下半部使用 NuttX `struct adc_dev_s`，不混入原厂 RTOS glue。
9. `/dev/adc0` 稳定枚举，`ANIOC_TRIGGER` + `read()` 路径通过 1000 次循环。
10. 与 CP `temp_detect`/`volt_detect` 的并发在启动期和稳态两个窗口均通过。
11. `CONFIG_SARADC_MB` 保持关闭；若必须打开，作为独立阶段并重过启动门禁。
12. 引脚在 `ao_shutdown()` 后完全还原，全外设回归通过。
13. 24 小时长稳、100 次冷启动、1000 次 open/close 达标。
14. 工程量换算的系数有出处，并与外部仪表在三点以上比对。

## 10. 风险与明确决策

| 风险 | 性质 | 处置 |
|---|---|---|
| CP 周期占用 SARADC 且不广播 | 已确认，非理论 | 决定路线 B；路线 A 需独占性证据 |
| 本板外部 ADC 引脚几乎全被占用 | 已确认 | 产品优先用内部 Vbat/vtemp；外部仅 ADC6/GPIO21 且需原理图确认 |
| `adc_cap.h` 与 `adc_ll.h` 通道定义矛盾 | 原厂自身未决 | P0 用真机数据判定，记录结论 |
| `ana_reg2` 裸写静默失败 | 已在音频路径踩过 | 复用带轮询的 helper 并加锁 |
| AON `BAKP_SADC` 投票 AP 无通路 | 未确定 | P1 取证；未确定前不进入 P2 |
| 路线 B 需要 mb_ipc socket 状态机 | 工作量风险 | 复用既有 SWAP 布局与 ABI helper；分阶段提交 |
| 重开 `CONFIG_SARADC_MB` 引发启动期回归 | 已有实板历史 | 默认禁止；如需则独立阶段 + 100 次冷启动门禁 |
| AP raw 分区容量 | 固定上限 3904K | 每次提交记录体积增量 |

明确不做（本轮）：多通道并发采集、DMA、看门狗阈值中断（`ANIOC_WDOG_*`）、
低功耗下的 ADC 唤醒、SDMADC（另一个外设，`sys_hal_set_sdmadc_config()` 与
SARADC 共用 `ana_reg2` 的 `sp_nt_ctrl`/`gadc_capcal`，两者不得同时启用）。

## 11. 稳定引用

NuttX ADC 框架：

```text
contest/nuttx/drivers/analog/adc.c
contest/nuttx/drivers/analog/Kconfig
contest/nuttx/include/nuttx/analog/adc.h
contest/nuttx/include/nuttx/analog/ioctl.h
contest/apps/examples/adc/adc_main.c
```

BK7258 原厂参考（寄存器与 HAL）：

```text
bk_avdk_smp/ap/include/soc/bk7258/reg_base.h
bk_avdk_smp/ap/include/soc/bk7258/adc_cap.h
bk_avdk_smp/ap/include/driver/adc.h
bk_avdk_smp/ap/include/driver/adc_types.h
bk_avdk_smp/ap/include/driver/hal/hal_adc_types.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/adc_struct.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/adc_map.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/gpio_map.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/icu_map.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sys_struct.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/adc_ll.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/sys_ll.h
bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/sys_hal.c
bk_avdk_smp/ap/middleware/soc/common/hal/adc_hal.c
```

BK7258 原厂参考（所有权与 IPC）：

```text
bk_avdk_smp/ap/middleware/driver/saradc/saradc_client.c
bk_avdk_smp/ap/middleware/driver/saradc/saradc_ipc.h
bk_avdk_smp/ap/middleware/driver/saradc/saradc_notify.c
bk_avdk_smp/ap/include/driver/mb_ipc_port_cfg.h
bk_avdk_smp/ap/include/driver/mailbox_channel.h
bk_avdk_smp/cp/middleware/driver/saradc/adc_driver.c
bk_avdk_smp/cp/middleware/driver/saradc/saradc_server.c
bk_avdk_smp/cp/components/temp_detect/temp_detect.c
bk_avdk_smp/cp/components/temp_detect/temp_detect.h
bk_avdk_smp/cp/components/temp_detect/temp_detect_pub.h
bk_avdk_smp/cp/components/temp_detect/volt_detect.c
bk_avdk_smp/projects/app_ab/cp/config/bk7258/config
```

本仓库内的模板与既有结论：

```text
board/beken/chips/bk7258/bk7258_trng.c                 探测式 char 设备模板
board/beken/chips/bk7258/hardware/bk7258_trng.h        出处注释与 PRRO 同构布局
board/beken/chips/bk7258/bk7258_pwm.c                  时钟/pinmux 开场与共享资源纪律
board/beken/chips/bk7258/hardware/bk7258_aud.h         ana_reg 访问机制与陷阱
board/beken/chips/bk7258/bk7258_aud.c                  aud_ana_write() 轮询实现
board/beken/chips/bk7258/bk7258_gpio.c                 set_function 语义
board/beken/chips/bk7258/hardware/bk7258_mbox.h        SWAP 布局与 IPC ABI helper
board/beken/chips/bk7258/bk7258_mailbox_channel.c      0x4c 应答与 IPC 回显约束
board/beken/chips/bk7258/bk7258_kvdb.c                 flash IPC 客户端被移除的结论
board/beken/chips/bk7258/include/irq.h                 IRQ 换算约定
board/beken/boards/bk7258/bk7258-ap/include/board.h    按键引脚
board/beken/boards/bk7258/bk7258-ap/src/bk7258_bringup.c  best-effort 调用约定
docs/archive/BK7258_OPENVELA_SMP_PORTING_PLAN.md        CONFIG_SARADC_MB 关闭始末
docs/固件构建步骤.md                                    构建与打包
```

行号不是稳定接口；后续引用优先使用函数名、宏名和文件路径，并在每次原厂
SDK 更新后重新执行 P0 取证。
