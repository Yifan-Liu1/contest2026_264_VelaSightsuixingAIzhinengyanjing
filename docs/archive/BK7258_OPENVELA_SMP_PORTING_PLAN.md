# BK7258 OpenVela AP 双核 SMP、480 MHz 与电源管理方案

> 归档状态：早期独立方案，未按本文件单独启动。当前 SMP 实现、状态和门禁以
> `docs/plans/BK7258_OPENVELA_AP_PORTING_PLAN.md`、当前源码和实板记录为准。
>
> 文档状态：2026-08-05 源码审计后的实施基线。本文以当前比赛仓库、最终
> OpenVela `.config`、`bk_avdk_smp` CP/AP 源码和实板长期运行日志为依据。
> 本文描述目标实现，不表示 CPU2、480 MHz 或低压睡眠已经完成实板验收。
> 任何与本文冲突的历史设计，以当前源码、最终配置和实板证据为准。

## 1. 目标与固定结论

目标是在 BK7258 上运行一个 NuttX SMP 实例：物理 CPU1 和 CPU2 共同执行同一个
OpenVela `nuttx.bin`，目标 AP 核频率为 480 MHz。物理 CPU0 继续运行 Armino CP，
并继续拥有全芯片电源、DPLL、核心/总线时钟、电压、PSRAM PHY、硬件 WDT 和
CPU1 电源生命周期。

正式架构固定为：

| 物理核 | NuttX 编号 | 角色 | 目标频率 |
| --- | ---: | --- | ---: |
| CPU0 | 不可见 | Armino CP，电源/频率执行者 | 240 MHz（480 MHz档位下） |
| CPU1 | 0 | OpenVela AP primary、系统 tick、CP transport owner | 480 MHz |
| CPU2 | 1 | OpenVela AP secondary、通用 SMP 调度 | 480 MHz |

BK7258 的 `PM_CPU_FRQ_480M` 不是三核都运行 480 MHz。原厂档位意图是：

```text
core source: 480 MHz
CPU0:       240 MHz
CPU1:       480 MHz
CPU2:       480 MHz
bus:        240 MHz
```

当前 HAL 已明确写入 480 MHz source、CPU0 `/2` 和 CPU1/CPU2 `/1`，但
`sys_hal_core_bus_clock_ctrl()` 虽接收 `ckdiv_bus`，实际只改 `CLK_DIV_REG0[3:0]`
的 core divider，没有写 bit 6 的 bus divider。因此 CPU0/CPU1/CPU2 目标值有源码
依据，bus 240 MHz 仍是必须通过寄存器和实测确认的目标，不能只凭档位注释验收。

依据为：

- `bk_avdk_smp/cp/include/modules/pm.h:346-353`
- `bk_avdk_smp/cp/middleware/soc/bk7258/hal/sys_hal.c:571-658`
- `bk_avdk_smp/cp/components/bk_pm/pm.c:2218-2283`

必须同时接受以下结论：

1. 不能只打开 `CONFIG_SMP`。CPU2 reset/vector、核 ID、每核中断栈、IPI、
   SMP atomic 和驱动并发都必须实现。
2. 不能只把 AP `CONFIG_BK7258_CPU_FREQ_HZ` 改为 480 MHz。CP 是共享核心时钟的
   唯一执行者，必须在释放 CPU1 reset 前建立 480 MHz vote，并在 AP 停止后释放。
3. 首版 480 MHz SMP 运行期间禁止 runtime DVFS。CPU1/CPU2 的核心时钟变化会直接
   改变 SysTick；没有 pre-change/post-change 协议和时间补偿时，运行中改频会破坏
   timeout、sleep、scheduler tick 和 heartbeat。
4. `CONFIG_SYSTEM_TIME64=y` 是必需配置，不是可选优化。当前 32 位配置已经造成
   可重复的长期静置重启。
5. CPU2 不需要独立镜像或分区。CPU1/CPU2 共享 `app1.bin`、地址空间和调度器；
   CPU2 只需要同一镜像内的 secondary boot vector。
6. CPU2 启动失败时不得静默降级为单核。`CONFIG_SMP_NCPUS=2` 下必须 fail-stop，
   否则 NuttX per-CPU 和调度器状态不完整。
7. 首版只支持普通 idle WFI。CP coordinated low-voltage、CPU1/CPU2 掉电、CPU2
   hotplug 和 runtime DVFS 必须在独立阶段完成，不得和首次 SMP bring-up 混合。

## 2. 当前源码事实与历史方案修正

### 2.1 当前可复用基线

正式 OpenVela 代码所有权目录是：

```text
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
```

当前代码已经具备：

- CPU1 reset、C runtime、SAU、FPU、MPU 和 RAM runtime vector。
- Mailbox v2 channel 1、transport ACK/reset/probe、Mailbox UART 全双工 console。
- AP power-up indication、2 秒 heartbeat、PWC ready 和 PSRAM power vote。
- 16 MiB PSRAM non-cacheable/XN 映射、AP 独立 heap 和四个媒体 pool。
- GPIO、PWM、I2C、QSPI、camera、基础 Wi-Fi IPC 代码。
- `EXTERNAL_AP_BIN` 打包链和单个 `app1.bin`。

当前还没有：

- `CONFIG_SMP=y`、CPU2 secondary entry、`up_cpu_start()`。
- 逻辑 CPU ID、每核中断栈、CPU2 私有 MPU/NVIC 初始化。
- AP 内部 channel 1/2 raw Mailbox IPI。
- 适合 BK7258 SRAM exclusive 限制的 NuttX atomic backend。
- CP 侧 OpenVela AP 480 MHz 生命周期 vote。
- OpenVela 双核 coordinated low-voltage 和恢复时间补偿。

### 2.2 对旧版方案的修正

| 旧说法 | 当前结论 |
| --- | --- |
| “不改变 CPU0/CP 固件” | 错误。480 MHz 生命周期、CPU2 电源域和低压策略必须修改 CP。协议兼容保持不变不等于 CP 源码不改。 |
| “首版固定 120 MHz” | 已被实板否定。当前 CP 将共享核心时钟降至 60 MHz；正式目标改为 CP 保证的 480 MHz。 |
| “Mailbox IRQ 到 board late 才注册” | 已过时。当前 `arm_serialinit()` 初始化 logical transport、Mailbox UART，并启动 physical mailbox；SMP 应在此基础上增加 channel 2。 |
| “PSRAM allocator/PWC 是占位分支” | 已过时。当前 `bk7258_psram.c` 已建立独立 allocator 和 shutdown 状态机，`bk7258_pm_pwc.c` 已处理 power/query/recovery。SMP 仍需审计其跨核锁和对象生命周期。 |
| “当前功能不含 Wi-Fi” | 已过时。当前工作树已有 Wi-Fi IPC 代码和配置；首次 SMP 调试仍应关闭 Wi-Fi 以缩小故障面，之后恢复。 |
| “AP_SPINLOCK 是硬件 spinlock” | 不准确。它是为 exclusive monitor 与 DMA/Audio 并发约束保留的 SRAM 区；锁仍由 LDAXR/STXR 实现。 |
| “CPU2 可由 CP start_cpu2_core() 启动” | 对单镜像 NuttX SMP 不适用。CP 不知道运行时 secondary vector；CPU1 在 `up_cpu_start(1)` 中写 CPU2 vector 并释放 reset。 |
| “低压睡眠可沿用原 AP” | 不成立。原实现依赖 FreeRTOS SMP idle、AON WFI bits、tick 补偿和 mailbox yield；NuttX 必须重建等价状态机。 |

## 3. 已确认的长期重启与频率缺陷

### 3.1 32 位时间乘法溢出导致 heartbeat 永久阻塞

AP heartbeat worker 位于：

```c
for (;;)
  {
    nxsig_usleep(2000000);
    bk7258_mbox_send_message(... IPC_CPU1_HEART_BEAT_INDICATION ...);
  }
```

来源：

- `contest/.../chips/bk7258/bk7258_pm_pwc.c:70-82`

当前最终配置同时满足：

```text
CONFIG_TIMER_ARCH=y
CONFIG_USEC_PER_TICK=1000
# CONFIG_SYSTEM_TIME64 is not set
```

NuttX generic architecture timer 使用：

```c
static uint64_t current_usec(void)
{
  ...
  return TICK2USEC(timebase) + (status.timeout - status.timeleft);
}
```

`timebase` 是 32 位 `clock_t`，`TICK2USEC` 为：

```c
#define TICK2USEC(tick) ((tick) * USEC_PER_TICK)
```

`1000L` 在 32 位 ARM 上仍为 32 位，因此乘法在转换为 `uint64_t` 之前已经溢出。
编译产物也显示只有低 32 位 `mla`，返回值高 32 位被置零。AP 时间会在：

```text
2^32 us = 4,294,967.296 ms AP time
```

提前跳回零，而不是在正常的 32 位 tick 边界回卷。跨界前创建的两秒 sleep deadline
大于这个人为最大值，回卷后的时钟永远到不了 deadline，heartbeat 线程永久阻塞。

CP 的 heartbeat fallback timeout 为 6000 ms。实板多次日志均满足：

```text
current - last_heartbeat = 6000 ms
Assert at: mb_ipc_task:298
```

所以重启是 CP 在 AP heartbeat 停止后执行的确定性断言，不是随机复位。

必需修复：

```text
CONFIG_SYSTEM_TIME64=y
```

不能只把 heartbeat 改成 busy wait、分段 sleep 或增大 CP timeout。这些只能隐藏一个
调用者，Mailbox worker、semaphore timeout 和其他 watchdog deadline 仍会失效。

### 3.2 CP 60 MHz 与 AP 120 MHz 配置不一致

当前 AP SysTick 使用：

```c
systick_initialize(true, CONFIG_BK7258_CPU_FREQ_HZ, -1);
```

AP 配置是 120 MHz，但 CP 配置和初始化代码会执行：

```text
CONFIG_CPU_DEFAULT_FREQ_60M=y
bk_pm_module_vote_cpu_freq(PM_DEV_ID_DEFAULT, PM_CPU_FRQ_60M)
```

而 60 MHz 档位同时影响 CPU0、CPU1、CPU2 和 bus。结果是 AP 以 120 MHz 计算的
1 ms SysTick 实际约为 2 ms，2 秒 heartbeat 实际约为 4 秒。

这也精确解释了重启为何发生在约 2 小时 23 分，而不是约 1 小时 11 分：

```text
4,294.967296 s * (120 / 60) = 8,589.934592 s
```

实板在约 `8,594.5 s` assert，差值位于最后一帧 heartbeat 到 6 秒 timeout 的窗口。

### 3.3 CP heartbeat 配置符号错误

CP heartbeat 代码检查 `CONFIG_WDT_EN`，当前生成配置实际使用 `CONFIG_INT_WDT` 和
`CONFIG_INT_WDT_PERIOD_MS=8000`。所以最终固件走的是 `2000 * 3 = 6000 ms` fallback。

应将 CP 条件改为实际 watchdog 配置符号，并明确 heartbeat timeout 策略。但即使改成
8 秒，也不能替代 AP 时间和频率修复。

## 4. 电源、频率与核生命周期所有权

### 4.1 所有权表

| 资源 | 唯一执行者 | OpenVela AP 职责 |
| --- | --- | --- |
| DPLL/core source/core divider/bus divider | CPU0/CP PM | 只声明固定 480 MHz 契约并校验结果；bus divider 未验证前不得发布 ready |
| VDDD/VDDDIG 高压档 | CPU0/CP PM | 不直接写 analog/PMU 电压寄存器 |
| CPU1 power/reset/boot offset | CPU0/CP | 发送 ready/recovery，不能直接关闭自身 |
| CPU2 power domain | CPU0/CP | 请求域保持 ON；不得直接改 `CPU2_PWR_DW/HALT` 绕过 CP accounting |
| CPU2 reset/boot offset | CPU1/AP primary | `up_cpu_start(1)` 写 secondary vector 并释放 reset |
| CPU2 scheduler/idle/IPI | OpenVela SMP | 完整管理 |
| PSRAM PHY、电压、时钟、容量检测 | CPU0/CP | 通过 PWC vote；只管理 AP allocator |
| SysTick 和 NuttX monotonic time | CPU1/OpenVela | 固定频率下维护；低压阶段用 AON 时间补偿 |
| WDT 和 heartbeat assert | CPU0/CP | AP 持续发送 heartbeat，处理 transport 错误 |

这里的 ID 属于两个不同命名空间：`PM_DEV_ID_CPU1` 是共享 frequency vote 表的 owner
slot，不是 CPU1 power-domain ID，也不存在 `PM_DEV_ID_CPU2` frequency slot。CPU1/CPU2
电源必须分别使用 `PM_POWER_MODULE_NAME_CPU1`、`PM_POWER_MODULE_NAME_CPU2`。当前 power
enum 中 CPU1、APP、CPU2 的值依次为17、18、19；禁止根据公共头文件中的过时数字注释
硬编码 ID。

### 4.2 480 MHz 启动时序

CP 必须在释放 CPU1 reset 之前建立 480 MHz，不能等 AP 启动后再通过 PWC 改频：

```text
CP OpenVela AP boot transaction
  -> sole owner写PM_DEV_ID_CPU1 = PM_CPU_FRQ_480M
  -> 验证owner slot、PM cache和实际clock/voltage寄存器
  -> 用语义化helper确认CPU2处于reset-hold
  -> 获取OpenVela CPU2 power-only hold并确认domain ON
  -> power CPU1 domain ON
  -> 按34-byte CRC physical到32-byte logical规则计算CPU1 boot offset
  -> 用已验证的语义化helper release CPU1 reset
  -> CPU1 OpenVela 按 480 MHz 初始化 SysTick
  -> nx_start()内部调用nx_smp_start()/up_cpu_start(1)
  -> CPU1 设置 CPU2 secondary vector并用语义化helper release reset
  -> CPU2 online和双向IPI自检
  -> 发送IPC power-up并启动heartbeat
  -> 发送PM_CPU1_BOOT_READY_CMD解除CP启动等待
  -> 请求并验证PSRAM PWC
  -> 成功后发布独立的最终AP/PSRAM-ready门禁
```

`PM_CPU1_BOOT_READY_CMD` 必须先于PSRAM请求：当前CP从low-power worker启动CPU1并在同一
worker中等待boot-ready，PSRAM vote也由该worker处理；反向排序会造成双方互等。这里的
最终ready是给上层allocator/driver的独立门禁，不能替换或延后现有boot-ready。

CP 侧建议增加显式配置 `CONFIG_OPENVELA_AP_480M`，只在外部 OpenVela AP 构建中启用，
避免改变原厂 FreeRTOS AP 的默认行为。该配置下修改：

- `PM_DEV_ID_CPU1` 只有一个覆盖式 byte slot，不是引用计数。480 MHz vote 必须只有
  一个生命周期 owner；建议由新的 OpenVela boot transaction 在任何 CPU1/CPU2 power-on
  前写入。`user_app_main()` 只请求事务启动，`pm_module_bootup_cpu1()` 不得重复投同一 slot。
- `PM_DEV_ID_DEFAULT=60M` 可保留。PM 取所有 slot 最大值，AP online 时480M胜出；AP
  vote释放后回到60M是当前CP配置的正常策略。只有明确要求AP关闭后CP回到120M时，才
  条件跳过最终60M写入，该选择不是建立480M契约的必要条件。
- `bk_pm_current_max_cpu_freq_get()` 只是软件 cache。上电前必须同时校验
  `bk_pm_module_current_cpu_freq_get(PM_DEV_ID_CPU1)`、current max、core source、core
  divider、bus divider、CPU0/CPU1/CPU2 speed和VDDDIG；软件状态与寄存器不一致即失败。
- 新增 OpenVela CPU2 power-only hold API。它只做独立 owner accounting、控制
  `PM_POWER_MODULE_NAME_CPU2` 并等待power readback；不得调用 `start_cpu2_core()`、写
  CPU2 offset或release reset。现有boot-vote API会同时启动CPU2，不适用于单镜像NuttX。
- CPU1 boot继续复用 `get_partition_addr(1)` 的34-byte CRC physical到32-byte logical
  转换及SoC address处理，最终寄存器写入转换后地址的 `offset >> 8`，不得写raw partition
  start。
- 裸 `CPU1/2_SW_RST=0/1` 的HAL注释与原厂start/stop helper语义矛盾。实施时必须提供
  `cpu_hold_reset()`/`cpu_release_reset()`，通过run-status和实板启动试验确定极性并修正
  注释；方案和日志不能把未经验证的裸值直接命名为assert/release。
- `cp/middleware/driver/pwr_clk/Kconfig`：增加 `CONFIG_OPENVELA_AP_480M` bool 选项，
  默认关闭；只在 `projects/app_ab/cp/config/bk7258/config` 中由 OpenVela 构建启用。

所有 CP 源码修改必须同时在比赛仓 `external/bk_avdk_smp/` 权威副本中保留，并更新
`external/bk_avdk_smp/README.md` 的覆盖文件表和同步命令，不能只修改
`bk_avdk_smp` 构建树。同步方法见该 README 第 2 节。

按本方案实施时，权威覆盖至少会新增实际修改到的以下路径；若实现落在其他文件，也必须
一并加入，不能把此表当成封闭清单：

```text
projects/app_ab/cp/cp_main.c
projects/app_ab/cp/config/bk7258/config
cp/components/bk_pm/pm.c
cp/components/bk_startup/system_main.c
cp/include/driver/pwr_clk.h
cp/middleware/driver/common/driver.c
cp/middleware/driver/pwr_clk/Kconfig
cp/middleware/driver/pwr_clk/pwr_clk.c
cp/middleware/driver/pwr_clk/low_pwr_core.c
cp/middleware/soc/bk7258/hal/sys_hal.c
cp/middleware/driver/mailbox/mb_ipc_heartbeat.c
```

其中 `common/driver.c`、`pwr_clk.c` 和heartbeat文件已在当前镜像中；其余文件只有发生
修改时才新增。每次构建前，README列出的全部覆盖文件与目标工作树必须逐字节 `cmp -s`
一致。

OpenVela boot transaction 必须返回明确错误，不能沿用当前始终返回 `BK_OK` 的
boot-vote外层接口。首次失败即进入按stage反向释放的rollback；删除当前CPU1 timeout时
未经quiesce就切换PSRAM OFF/ON并 `goto boot_cp1` 的路径。若未来允许重试，每次都必须是
完整reset、power-off、重新获取资源、power-on事务，并记录stage和错误原因。

原厂实现根据当前档和目标档选择high-to-low或low-to-high路径，然后直接执行目标档，
不会遍历中间档位。升频目标分支先写电压再切时钟，降频目标分支先切时钟再写电压。
普通480M分支目标VDDDIG为0.95 V；VDDD 1.05 V helper仅在 `CONFIG_RX_OPTIMIZE` 启用时
生效，当前CP配置下是no-op。OpenVela不得复制这些寄存器序列，必须修正并复用CP PM：

- `cp/components/bk_pm/pm.c:2218-2283`
- `cp/middleware/driver/sys_ctrl/sys_ps_driver.c:243-288`
- `cp/middleware/soc/bk7258/hal/sys_hal.c:571-709`

当前 `sys_hal_switch_cpu_bus_freq()` 丢弃子函数返回值，且bus divider未真正写入；这两处
必须修复，成功后才能更新PM cache。电压仲裁还必须取CPU档位和全部PSRAM owner所需
VDDDIG的最大值：只要PSRAM PHY仍ON，VDDDIG不得低于其0.95 V要求。现有降频路径会直接
写0.875 V，不能仅靠调整关闭调用顺序规避。

PSRAM也必须纳入同一事务约束：CP boot时不得直接 `bk_psram_init()` 却让owner bitmap
保持0；应由明确的CP BOOT/AS_MEM owner建立基础vote。OpenVela使用独立owner；PWC ON
必须透传真实 `bk_psram_init()` 结果，只有成功后才能置owner bit和发布ready。协议定义为
request `param3=PM_PSRAM_PROTO_V1`；response `param1` 回显ON/OFF state、`param2` 携带
真实 `ret`、`param3` 回显version。不支持的version返回错误且不得修改owner bitmap，
AP/CP双方必须同时更新。AP释放自己的owner不能关闭仍被CP owner使用的PHY。

### 4.3 运行期频率规则

首版运行期规则只有一条：AP online 时 `PM_DEV_ID_CPU1` 始终 vote 480 MHz。

- CP 默认60 MHz vote可以保留；PM取所有module slot的最大值，AP生命周期内唯一的480M
  slot会把共享时钟保持在480 MHz。
- AP 不发送 `PM_CPU_FREQ_CTRL_CMD` 降频。
- 其他 CP/AP module 可以投票，但不能把最大值降到 480 MHz以下。
- CP 应提供诊断查询，至少输出 current max vote、全部vote owner、core source、core/bus
  divider、CPU0/CPU1/CPU2 speed、VDDD/VDDDIG、CPU1/CPU2 power/reset和PSRAM owner状态。
- `PM_CPU_FRQ_480M` 被 `CONFIG_CLK_FORCE_MAX_CPU_FREQ_320M` 截断时必须判为启动失败，
  不能继续用 480 MHz SysTick配置运行。

保留 `PM_GET_PM_DATA_CMD` 命令值 `0xc`，同步扩展AP/CP的
`pm_ap_get_cp_data_type_e`，增加只读current-frequency与divider查询类型。扩展必须同时
更新双方header和source override，不能单边复用已有enum数值。

### 4.4 关闭与故障回滚

正常关闭或 AP recovery 的顺序必须是：

```text
AP primary停止新任务和外设提交
  -> quiesce PSRAM/DMA/Mailbox UART
  -> 调度CPU2执行stop callback
  -> 屏蔽CPU2普通IRQ和IPI
  -> CPU2进入确认的park/WFI状态
  -> CPU1用语义化helper将CPU2置reset并确认不再执行
  -> AP释放自己的PSRAM PWC owner并等待CP ACK
  -> AP向CP回复recovery-ready
  -> CP确认CPU2 reset，释放power-only hold并确认CPU2 domain OFF
  -> CP停止/reset CPU1并确认CPU1 domain OFF
  -> CP以PM_CPU_FRQ_DEFAULT释放PM_DEV_ID_CPU1 slot
  -> CP按CPU档位和剩余PSRAM owner重新计算VDDDIG
```

当前 CP shutdown 代码仅在特定低压配置下才停止 CPU2，而且顺序是先停止 CPU1、后停止
CPU2。OpenVela SMP 模式必须改成上述顺序。CPU2仍在执行共享NuttX状态时，CP不能
直接关闭CPU2 power；CPU1已停止后也不能再依赖CPU1去park CPU2。

所有失败必须达到相同后置条件，但不能强制走同一callback路径：CPU1/CPU2不执行、只对
本事务已获取的CPU2/CPU1 domain、OpenVela PSRAM owner和480M slot按反序释放。

- 480M/readback失败发生在CPU1 release前：CP直接撤销已获取资源，不等待AP。
- CPU1 boot超时：CP强制CPU1/CPU2 reset并关闭domain，不等待AP recovery。
- CPU2 online超时：CPU1若仍可信就强制CPU2 reset，不能依赖未online的stop callback。
- 只有正常recovery才使用完整AP quiesce、park和ready握手。

## 5. 睡眠与低功耗分阶段方案

### 5.1 阶段 P0：普通 WFI，禁止 coordinated low-voltage

首次 SMP 和 480 MHz 验收只启用 per-core 普通 WFI：

- 增加BK7258 `up_idle()`；当前公共ARM `up_idle()`中的WFI被 `#if 0` 禁用，不能把
  “空闲任务在运行”误判为已经WFI。chip hook必须清 `SCB_SCR_SLEEPDEEP`，执行
  `DSB; WFI; ISB`，且不设置coordinated low-voltage ready bit。
- CP在每次OpenVela启动和recovery时显式调用
  `bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_CPU1, 0, 0)` 并检查 `BK_OK`。
  state 0表示AP active/not-ready，因此全芯片low-voltage条件不成立。
- 保持 `CONFIG_PM_AP_POWERDOWN_WHEN_LV` 关闭。
- 不支持 CPU1/CPU2 power-down、retention、CPU2 hotplug 或 tickless。
- heartbeat、Mailbox 和 SysTick 在普通 WFI 下仍可唤醒 CPU1。

CP 的 `PM_ENTER_LOW_VOL_MODULES_CONFIG` 包含 `PM_SLEEP_MODULE_NAME_CPU1`。启动 OpenVela
AP 时应显式写exit-sleep，不能依赖BSS初值；PM初始化实际上会把CPU1置为enter-sleep，
而当前清除代码又受已关闭的 `CONFIG_PM_AP_POWERDOWN_WHEN_LV` 条件控制。OpenVela必须
独立执行上述调用，保证未实现低压协议时CP只能进入normal sleep。

### 5.2 阶段 P1：双核 coordinated low-voltage

只有 P0 的 24 小时稳定性通过后，才能实现原厂 AP 的等价低压协议。原厂事实为：

- CP 要求 CPU1 和 CPU2 都进入 WFI，并检查 AON PMU 的
  `cp1_enter_wfi_state`、`cp2_enter_wfi_state`。
- AP 两个核分别屏蔽普通 IRQ，只保留本核 Mailbox wakeup。
- CP 保存并切换核心/Flash 时钟，恢复 DPLL 后再唤醒 AP。
- 原 AP 依赖 FreeRTOS SMP idle 和 `vPortYieldCore()`；这些调用不能直接移植到 NuttX。

NuttX实现必须复用现有SMP PM框架，而不是另建平行barrier：

1. 启用 `CONFIG_PM=y`、选定governor、`CONFIG_PM_NDOMAINS=3` 和
   `CONFIG_PM_SMP_LAST_CPU_INDEX=0`；这里的0是NuttX logical CPU0/物理CPU1，不是CP。
2. BK7258 `up_idle()` 调用 `pm_idle(handler)`；驱动通过 `pm_register()` 注册
   `struct pm_callback_s` prepare/notify callback。
3. logical CPU0作为last CPU发起全局transition，禁止新任务迁移、DMA和mailbox
   transaction；两核运行per-CPU prepare并保存本核NVIC/SysTick状态。
4. CPU2设置 `cp2_enter_wfi_state` 后进入WFI；CPU1确认CPU2已就绪。
5. CPU1停止SysTick，设置 `cp1_enter_wfi_state`，通过既有sleep vote允许CP进入低压。
6. CP只在两个AON WFI bit、AP sleep/clock vote和mailbox idle都满足后切换低压。
7. CP恢复DPLL、经readback验证的480 MHz档位和bus后，通过Mailbox唤醒AP。
8. CPU1读取AON elapsed time，恢复monotonic time和SysTick；再唤醒CPU2。
9. 两核恢复NVIC和驱动callback，`pm_idle()`完成restore并恢复heartbeat。

低压时间基准不能继续完全依赖core-clock SysTick。增加BK7258 strong
`up_timer_gettick()`/`up_timer_gettime()`，以AON RTC构建64位单调epoch；当前原厂启用
64-bit AON counter，应使用稳定的low/high重读算法，不需要软件扩展32-bit wrap。必须由
CP契约确认32 kHz源是32000还是32768 Hz，并用64位算术换算、校准和验证回卷单调性。

时间读取本身不是唤醒源。P1必须提供下一NuttX deadline的AON/CP wake：推荐启用
`CONFIG_SCHED_TICKLESS=y`，实现完整的 `up_timer_tick_cancel()`/
`up_timer_tick_start()` 或 `up_alarm_cancel()`/`up_alarm_start()` AON compare backend。
若暂时保持periodic语义，则必须编程AON wake不晚于下一1 ms tick，收益有限。恢复时先
确认clock稳定，再重启SysTick；解除PM barrier前必须让logical CPU0至少执行一次timer
processing，使低压期间到期的sleep和watchdog立即得到处理。

在 AON-backed monotonic time、双核 barrier和驱动 PM callback完成前，禁止打开 P1。

### 5.3 Deep sleep

Deep sleep定义为 AP 上下文丢失后的完整重启，不是 NuttX suspend-to-RAM：

- CPU1/CPU2、NuttX heap、scheduler、Mailbox transport和PSRAM对象均视为丢失。
- CP/bootloader重新启动 CPU1，CPU1重新启动 CPU2。
- PSRAM generation必须变化，旧对象不可复用。
- 不承诺从原 PC/任务现场继续运行。

## 6. NuttX SMP 芯片层实现

### 6.1 配置硬门禁

AP defconfig目标配置：

```text
CONFIG_ARCH_HAVE_MULTICPU=y
CONFIG_SMP=y
CONFIG_SMP_NCPUS=2
CONFIG_SMP_DEFAULT_CPUSET=0x3
CONFIG_ARCH_INTERRUPTSTACK=2048
CONFIG_SYSTEM_TIME64=y
# CONFIG_TIMER_ARCH is not set
CONFIG_ARMV8M_SYSTICK=y
CONFIG_USEC_PER_TICK=1000
CONFIG_BK7258_CPU_FREQ_HZ=480000000
CONFIG_TICKET_SPINLOCK=y
# CONFIG_SCHED_TICKLESS is not set
```

上述是S0-S4/P0配置。P1另增加：

```text
CONFIG_PM=y
CONFIG_PM_NDOMAINS=3
CONFIG_PM_SMP_LAST_CPU_INDEX=0
CONFIG_PM_GOVERNOR_GREEDY=y
CONFIG_PM_GOVERNOR_EXPLICIT_RELAX=0
CONFIG_SCHED_TICKLESS=y
```

`ARCH_CHIP_BK7258` 增加：

```kconfig
select ARCH_HAVE_MULTICPU
select ARCH_IDLE_CUSTOM
select LIBC_ATOMIC_HWSPINLOCK if SMP
```

`ARCH_IDLE_CUSTOM` 必须同时对Make和CMake生效，使公共 `arm_idle.c` 不进入构建；否则
`bk7258_idle.c` 与公共实现会重复定义 `up_idle()`。

`board/beken/chips/bk7258/Kconfig:15-17` 的 `CONFIG_BK7258_CPU_FREQ_HZ` 默认值从
`120000000` 改为 `480000000`。当前 `bk7258_timerisr.c`由logical CPU0直接配置并拥有
SysTick，CPU2不调用`up_timer_initialize()`；不能恢复通用timer lower-half路径。

最终 `.config` 必须确认 `clock_t` 为64位、toolchain atomic没有绕过 BK7258 backend、
CPU数为2。禁止只修改生成的 `.config`；必须修改项目 defconfig并执行 distclean。

### 6.2 文件与构建接入

建议最小新增：

```text
chips/bk7258/
  bk7258_cpustart.c       # secondary vector、reset、online handshake
  bk7258_cpuindex.c       # logical CPU ID
  bk7258_cpuidlestack.c   # secondary idle stack
  bk7258_idle.c           # P0 WFI；P1调用NuttX pm_idle
  bk7258_smpcall.c        # raw Mailbox IPI
  bk7258_hwspinlock.c     # AP_SPINLOCK software gate
  bk7258_atomic.c         # g_atomic_hwspinlock
```

修改：

```text
Kconfig
CMakeLists.txt
Make.defs
bk7258_start.c
bk7258_vectors.c
bk7258_irq.c
bk7258_timerisr.c
bk7258_mbox0.c
bk7258_mailbox_channel.c
hardware/bk7258_sysctrl.h
hardware/bk7258_mbox.h
board scripts/ld.script
board configs/nsh/defconfig
```

Make和CMake必须包含同一组SMP源文件，并仅在 `CONFIG_SMP` 下编译专用对象。

### 6.3 CPU ID

原厂 AP 使用每核私有 DTCM地址 `0x20000000` 保存 AP逻辑核 ID。OpenVela采用：

```text
物理CPU1私有 0x20000000 = NuttX CPU0
物理CPU2私有 0x20000000 = NuttX CPU1
```

`up_cpu_index()` 直接读取该word。primary在任何 per-CPU/NuttX atomic调用前写0；
secondary在任何NuttX API前写1。必须实板验证两核看到的是私有实例，不是共享word。

### 6.4 内存、secondary vector和栈

分区保持：

```text
AP_SPINLOCK  0x28000000..0x2800ffff
AP_RAM       0x28010000..0x28063fff
CP_RAM       0x28064000..0x2809f6ff
PWR_MNG      0x2809f700..0x2809f7ff
SWAP         0x2809f800..0x2809ffff
```

CPU2不增加Flash/RAM分区。链接脚本增加：

- `SPINLOCK` MEMORY，绝不加入heap。
- 512字节对齐的 `.bk_secondary_vectors`。
- 只用于reset早期的CPU2 bootstrap stack。
- 两份静态 interrupt stack，或等价的每核固定中断栈区域。
- 全部边界和对齐 `ASSERT`。

secondary vector：

```text
word 0: CPU2 bootstrap MSP top
word 1: bk7258_secondary_start | 1
```

CPU2 boot offset寄存器只保存地址高24位，至少要求256字节对齐；统一采用512字节以
满足VTOR约束。secondary vector不包含primary image offset `0x100` 的BK legacy magic。

`up_cpu_idlestack()` 应像RP2040一样调用 `up_create_stack()`，初始化logical CPU1 idle
TCB。还必须分配 `CONFIG_SMP_NCPUS * INTSTACK_SIZE` 对齐空间并实现
`uintptr_t up_get_intstackbase(int cpu)`；公共ARM实现只覆盖UP构建，ARMv8-M任务初始状态
会调用该hook。CPU2 reset后先用bootstrap MSP；进入NuttX前将PSP切到idle TCB stack
top，调用 `arm_initialize_stack()` 安装本核MSP/MSPLIM interrupt stack。两个核不得共用
MSP。

### 6.5 CPU2启动

`up_cpu_start(1)`：

```text
校验cpu==1且当前为logical CPU0
  -> 确认CP的OpenVela CPU2 power-only hold和domain readback均为ON
  -> 确认480 MHz契约和channel 2已配置
  -> 发布idle PSP、interrupt MSP、runtime VTOR
  -> 清boot stage/online flag，DMB/DSB
  -> CPU2保持reset时清route/pending
  -> CPU2_RXEVT_SEL=1
  -> CPU2_OFFSET=secondary_vector>>8
  -> cpu2_release_reset()（极性已由run-status/实板验证）
  -> 有界等待online和双向IPI自检
```

超时建议500 ms，输出CPU2 control、run status、route、NVIC、Mailbox channel 2、boot
stage和当前频率readback，然后panic。不能只返回错误后继续 `nx_bringup()`。

`bk7258_secondary_start()` 只初始化每核私有状态：

- logical ID、FPU CP10/11和barrier。
- 与primary一致的SAU/MPU/VTOR。
- CPU2私有NVIC enable/pending/priority。
- CPU2 Mailbox route和IRQ79。
- PSP/MSP/stack limit。
- release-ordered online flag、可选的 `sched_note_cpu_started(this_task())`、
  `nx_idle_trampoline()`。

primary以acquire语义等待online flag。`sched_note_cpu_started()`只是可选trace，在note
driver未启用时为空，不能承担online handshake。

secondary禁止复制 `.data`、清 `.bss`、初始化heap/PSRAM/driver、调用 `nx_start()`、
初始化第二个SysTick或重置全局Mailbox FIFO。

### 6.6 每核MPU/NVIC

MPU、SAU、NVIC、VTOR、SysTick和FPU控制均为每核私有。CPU2必须安装与CPU1相同的：

- AP Flash RO/X。
- AP RAM RW/XN/shareable。
- `AP_SPINLOCK` RW/XN/non-cacheable/shareable。
- CP共享窗口和Mailbox UART override。
- PSRAM 16 MiB RW/XN/non-cacheable。
- peripheral Device/XN。

当前 CPU1 使用7个 MPU region；增加 AP_SPINLOCK 后至少需要8个，启动时继续读
`MPU_TYPE.DREGION`并 fail-stop。CPU2不能重复probe或初始化PSRAM allocator。

全局NuttX IRQ table只由primary初始化一次。CPU2只初始化私有NVIC和CPU2 route：

```text
CPU1 IRQ EN0/EN1: 0x44010088 / 0x4401008c
CPU2 IRQ EN0/EN1: 0x44010090 / 0x44010094
```

首版只有CPU2 Mailbox IPI路由到CPU2。GPIO、PWM、I2C、QSPI、camera、Wi-Fi和其他
有状态外设仍由logical CPU0 owner。

## 7. Mailbox IPI与CP transport共存

### 7.1 物理channel

| channel | owner | FIFO |
| ---: | --- | --- |
| 0 | CPU0/CP | start 0, length 2 |
| 1 | CPU1/AP primary | start 2, length 3 |
| 2 | CPU2/AP secondary | start 5, length 3 |

沿用原厂消息分类：

```text
data[1] != 0: pointer + length envelope，进入CP/AP logical transport
data[1] == 0: AP内部raw command，进入SMP IPI handler
```

CPU2不处理CP PWC、heartbeat、Wi-Fi或Mailbox UART envelope。CP transport始终由
logical CPU0负责。

### 7.2 当前初始化路径的改造

当前 physical Mailbox 已在 `arm_serialinit()` 中经：

```text
bk7258_mailbox_init()
bk7258_mb_uart_init()
bk7258_mailbox_start()
```

启动。这发生在 `nx_smp_start()` 之前，生命周期适合SMP。改造要求：

1. primary physical start一次性配置channel 1和2 FIFO，不能在CPU2 online后soft reset。
2. 全局 `irq_attach(79, ...)` 只执行一次；CPU2复用共享NuttX IRQ handler table，但启用
   自己的route和私有NVIC line 63。
3. ISR按 `up_cpu_index()` 选择本核physical channel。
4. channel 1可处理CP envelope和raw IPI；channel 2只接受raw IPI。
5. shared transport状态从仅 `up_irq_save()` 改为SMP spinlock，或严格固定到CPU0。

### 7.3 NuttX SMP hooks

实现：

```c
int up_send_smp_sched(int cpu);
void up_send_smp_call(cpu_set_t cpuset);
```

raw command采用pending bitmap合并 `SCHED`、`CALL`和 `PM`。发送前DMB，ISR读取并清
pending后DMB，然后调用：

```c
nxsched_smp_call_handler(irq, context, arg);
nxsched_process_delivered(this_cpu());
```

FIFO full不能静默丢失。采用“shared pending bit + kick”语义：消息只负责唤醒，真实
工作由pending bit保持；如果目标已有未处理kick，不重复占FIFO。发送失败需要有界重试、
计数和fatal门限，禁止持有目标ISR需要的spinlock时无限等待。

## 8. Atomic、spinlock和共享驱动

### 8.1 BK7258约束

原厂注释明确：BK7258 AP SMP只有两个exclusive monitor；两个CPU同时对普通SRAM做
exclusive访问时，第三方DMA/Audio写SRAM可能无提示失败。因此原厂把所有spinlock对象
集中在 `AP_SPINLOCK` 特殊SRAM区。

`AP_SPINLOCK`不是寄存器型硬件spinlock。首版使用一个位于该区的software gate，
通过两个AP逻辑核之间的Peterson协议仲裁NuttX `atomic_*` 和libc out-of-line atomic
helper：

```text
CONFIG_LIBC_ATOMIC_HWSPINLOCK=y
g_atomic_hwspinlock -> struct hwspinlock_dev_s -> BK7258 software gate
```

gate owner使用 `logical_cpu + 1`，0表示free，避免CPU0 owner和free混淆；depth支持
libc atomic helper的同核递归进入。每次trylock都重新发布本核intent和turn，失败时清除
intent后返回，不能把一次失败尝试遗留为永久竞争状态。成功路径设置owner/depth，unlock
只允许owner执行并在最后一层释放intent，使用DMB、DSB和SEV完成发布。primary在进入
`nx_start()`前初始化gate；CPU2启动后禁止再次清零。

这不会自动拦截编译器内联 `__atomic_*`、`__sync_*` 或手写exclusive loop。SMP源码禁止
直接使用这些路径，构建时反汇编全部SMP对象，发现未批准的LDREX/STREX或LDAXR/STXR
循环即失败。普通 `spinlock_t` 固定启用 `CONFIG_TICKET_SPINLOCK`，其ticket状态使用
NuttX `atomic_cmpxchg`，再由 `CONFIG_LIBC_ATOMIC_HWSPINLOCK`统一进入本AP gate；每核
递归 `g_schedlock`仍按NuttX rspinlock协议使用同一atomic backend。只有DMA/Audio/
exclusive压力通过后才能评估更细粒度方案。

### 8.2 驱动收敛

首版将以下worker和IRQ固定到logical CPU0：

- Mailbox transport/UART/syslog/PWC/heartbeat。
- Wi-Fi、camera、QSPI、DMA和媒体worker。
- GPIO、button/motor、PWM、I2C。
- 所有未完成SMP审计的board driver。

worker入口在访问任何driver状态前先构造 `CPU_ZERO/CPU_SET(0)` mask，并调用
`sched_setaffinity(0, sizeof(cpu_set_t), &cpuset)`；成功后才通知creator ready。creator
等待该ready，affinity失败则driver bring-up失败。不能在 `kthread_create()` 返回后才pin，
因为worker可能已经运行。

即使worker固定CPU0，CPU2任务仍可能调用公共API。以下共享状态必须使用
`spin_lock_irqsave()` 或mutex：

- Mailbox active transaction、ACK pool、recovery epoch和ring index。
- syslog/serial TX/RX ring。
- GPIO/PWM/I2C寄存器RMW。
- PSRAM allocator状态、allocation counters和generation。
- Wi-Fi command/data slot和pbuf ownership。

ISR共享短状态使用spinlock；会等待ACK、semaphore、内存或格式化日志的路径使用mutex，
禁止在spinlock内sleep。

## 9. 系统tick、heartbeat与时间正确性

首版只有logical CPU0启动SysTick。CPU2不得调用 `up_timer_initialize()`，否则系统时间
和RR accounting加倍。CPU0每tick调用NuttX timer处理，调度器通过IPI要求CPU2切换。

必须满足：

- `CONFIG_SYSTEM_TIME64=y`。
- CPU1实际频率和 `CONFIG_BK7258_CPU_FREQ_HZ=480000000` 一致。
- 60秒wall clock测试误差在晶振/调度允许范围内，不能出现2倍或1/2倍。
- heartbeat实际周期约2秒，不是4秒。
- heartbeat发送返回值必须检查并统计 `-EBUSY`、`-ENOLINK`、timeout。
- CP heartbeat使用实际 `CONFIG_INT_WDT` 配置；timeout至少容纳调度抖动和一次transport
  recovery，但不能用增大timeout掩盖lost heartbeat。

长期测试必须跨越旧故障边界：至少运行3小时；正式验收运行24小时。还应通过调试构建
把timer timebase置于旧的 `UINT32_MAX / 1000` 边界前，快速验证sleep跨界。

## 10. 分阶段实施

### S0：先修复单核时间和480 MHz契约

1. AP defconfig 增加 `CONFIG_SYSTEM_TIME64=y`，保持单核 `CONFIG_SMP` 关闭。
2. AP `Kconfig` 将 `CONFIG_BK7258_CPU_FREQ_HZ` 默认值改为 `480000000`。
3. CP增加 `CONFIG_OPENVELA_AP_480M` 和单一boot transaction owner；在任何AP power-on
   前写 `PM_DEV_ID_CPU1=PM_CPU_FRQ_480M`，保留默认60M slot，并显式写CPU1 exit-sleep。
4. 修复DVFS子函数错误传播、bus divider写入/readback和CPU/PSRAM联合VDDDIG下限；把CP
   boot PSRAM和OpenVela PWC纳入独立owner accounting并透传真实初始化错误。
5. CP 修正 `mb_ipc_heartbeat.c` 的 watchdog 配置符号从 `CONFIG_WDT_EN` 改为实际
   `CONFIG_INT_WDT`，使 timeout 正确反映 `CONFIG_INT_WDT_PERIOD_MS`。
6. 增加CP全部frequency owner、source/divider/speed/voltage/power/PSRAM readback诊断。
7. AP heartbeat worker 检查 `bk7258_mbox_send_message()` 返回值，记录 `-EBUSY`、
   `-ENOLINK` 和 transport timeout 计数。

退出条件：20次冷启动、60秒时间校准、3小时静置无heartbeat timeout；PM slot/cache和
实际寄存器一致，CPU0=240 MHz、CPU1=480 MHz、CPU2 speed字段为480 MHz目标，bus经
divider readback和外部测量确认为240 MHz，无320M clamp；PSRAM ON/OFF故障注入不会发布
虚假ready或把VDDDIG降到有效owner要求以下。

### S1：SMP编译与内存骨架

1. 先增加CPU ID、每核interrupt stack、`up_get_intstackbase()` 和secondary idle stack，
   并提供可链接的 `up_cpu_start()` fail-stop占位及 `up_send_smp_sched()`/
   `up_send_smp_call()` 占位，再打开SMP配置。
2. 增加SPINLOCK MEMORY、ticket spinlock和atomic backend。
3. 增加secondary vector和链接断言，暂不release CPU2。
4. 增加P0 `up_idle()` 普通WFI；两核MPU表增加AP_SPINLOCK和相同PSRAM属性。

退出条件：clean build通过但该产物不启动；map、vector、stack、gate和heap边界全部正确；
反汇编确认未批准的exclusive loop不存在、普通spinlock使用ticket字段，P0 idle object确实
包含 `DSB/WFI/ISB`。
S2/S3分别替换上述fail-stop和IPI占位实现后才允许烧录启动。

### S2：CPU2启动到idle

1. CP通过OpenVela power-only hold在AP生命周期内保持CPU2 domain ON，并在handoff前
   保持经验证的reset状态；CP不调用原厂 `start_cpu2_core()`。
2. AP实现语义化CPU2 reset helper、private arch init、stack切换和release/acquire online
   handshake，且不写CPU2 power-domain控制位。
3. CPU2只运行NuttX idle，不路由普通外设。

退出条件：100次冷启动，CPU2每次500 ms内online；任何失败都fail-stop且有寄存器诊断。

### S3：IPI与调度

1. 配置channel 2 FIFO和per-core IRQ79。
2. 实现pending-bit raw IPI、`up_send_smp_sched/call`。
3. 完成双向call、task migration、semaphore/mutex和atomic压力。

退出条件：两核100%负载1小时，无lost IPI、死锁、counter mismatch和heartbeat timeout。

### S4：恢复驱动

1. worker/IRQ固定CPU0。
2. 审计Mailbox、syslog、GPIO/PWM/I2C、PSRAM、Wi-Fi和camera共享状态。
3. 依次恢复console、PWC/heartbeat、PSRAM、GPIO/PWM、Wi-Fi和camera。

退出条件：单核已有功能全部回归，CPU2压力不影响CP transport和外设owner。

### P1：低压睡眠

1. 启用NuttX PM SMP框架、三个domain、logical CPU0 last-CPU和driver callback。
2. 实现AON-backed 64位monotonic time及tickless AON compare/deadline wake backend。
3. 在 `pm_idle(handler)` 内实现双核AON WFI bit和CP sleep vote协议。
4. CP只在AP双方确认后进入low-voltage，寄存器确认恢复480 MHz/bus后再唤醒AP。
5. 完成expired timer处理、heartbeat宽限和Mailbox恢复。

退出条件：10000次低压进出、短/长timer deadline、RTC/GPIO wakeup、双核负载与故障
注入全部通过；sleep/watchdog不晚触发，monotonic time不倒退。

## 11. 构建与产物门禁

修改Kconfig/defconfig或CP config后必须clean：

构建前先按比赛仓 `external/bk_avdk_smp/README.md` 第2节同步全部CP覆盖并完成逐文件
`cmp -s`。然后执行：

```bash
cd /home/mi/vela_competition/contest
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh --cmake distclean
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  -e -Werror --cmake -j8

cp cmake_out/bk7258-ap_nsh/nuttx.bin \
  /home/mi/vela_competition/bk_avdk_smp/build/openvela-ap.bin

cd /home/mi/vela_competition/bk_avdk_smp
make -C projects/app_ab clean
podman run --rm --userns=keep-id \
  -v "$PWD:/armino" -w /armino \
  localhost/bekencorp/armino-idk:1.5 \
  make -C projects/app_ab bk7258 SDK_DIR=/armino \
  EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin
```

检查最终配置：

```bash
rg 'CONFIG_(SMP|SMP_NCPUS|SYSTEM_TIME64|TIMER_ARCH|USEC_PER_TICK|BK7258_CPU_FREQ_HZ|LIBC_ATOMIC)' \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_nsh/.config

rg 'CONFIG_OPENVELA_AP_480M|CONFIG_CPU_DEFAULT_FREQ_60M|CONFIG_CLK_FORCE_MAX_CPU_FREQ_320M' \
  projects/app_ab/cp/config/bk7258/config \
  projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.h
```

检查：

- `CONFIG_SMP_NCPUS=2`、`CONFIG_SYSTEM_TIME64=y`、CPU频率480000000。
- `current_usec()` 不再是低32位乘1000的 `mla`。
- primary vector位于 `0x02150000`；secondary vector位于同一Flash且512字节对齐。
- 两个中断栈、bootstrap stack、atomic gate不重叠。
- atomic gate位于 `0x28000000..0x2800ffff`；AP heap不进入该区。
- AP SRAM不越过 `0x28064000`。
- CPU2没有独立loadable image。
- CP project config和生成 `sdkconfig.h` 均启用OpenVela事务配置；320M clamp未定义或为0。
- CP最终ELF反汇编确认480 MHz vote位于CPU1/CPU2 power-on之前，heartbeat读取实际
  `CONFIG_INT_WDT_PERIOD_MS`，并且timeout rollback不再切换PSRAM后 `goto` 重试。
- PM诊断证明软件slot/cache与clock、voltage、power和PSRAM owner寄存器实读一致。
- `sys_hal_core_bus_clock_ctrl()` 反汇编/源码确认写入bus divider，且切频错误能传回事务。
- 比赛仓 `external/bk_avdk_smp/README.md` 已列出全部新增覆盖；README同步命令执行后每个
  文件 `cmp -s` 返回0。

三个AP文件必须哈希一致：

```bash
sha256sum \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_nsh/nuttx.bin \
  /home/mi/vela_competition/bk_avdk_smp/build/openvela-ap.bin \
  /home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
```

不得复用早于当前source的CP object。当前曾出现CP source晚于heartbeat object的情况，
所以CP config/source变化必须 `make clean`，并记录最终ELF、map和hash。

## 12. 实板验证矩阵

| 测试 | 方法 | 通过标准 |
| --- | --- | --- |
| 480 MHz契约 | CP读取全部vote、source、core/bus divider、三个CPU speed、VDDD/VDDDIG | slot/cache/寄存器一致；CPU0=240、AP=480、bus实测240；无320M clamp |
| wall clock | 对比外部计时60秒/1小时 | 无2倍或1/2倍走时 |
| heartbeat | 记录1000个间隔 | 约2秒，发送失败有统计，无CP timeout |
| 旧回卷边界 | debug seed + 3小时静置 | sleep继续唤醒，无时间倒退和assert |
| CPU ID | affinity任务读取 `sched_getcpu()` | 分别稳定为0、1 |
| CPU2启动 | 100次冷启动 | 全部500 ms内online |
| 双向IPI | CPU0->CPU1和CPU1->CPU0 | callback CPU和次数正确，无lost kick |
| task migration | mask 0x3计算任务 | 可迁移且状态完整 |
| atomic | 两核各加100万次 | 精确200万 |
| DMA+atomic | DMA写普通SRAM并发lock压力 | 无silent DMA write loss |
| interrupt stack | 中断嵌套和stack coloration | 两核MSP不重叠、不越界 |
| PSRAM | owner/error/电压故障注入及两核并发alloc/free | init失败不置bit/ready；CP owner不被AP OFF误关；VDDDIG不低于0.95 V；generation和计数一致 |
| Mailbox | log、PWC、Wi-Fi与IPI并发 | 无ACK覆盖、FIFO死锁、heartbeat丢失 |
| 外设owner | GPIO/PWM/I2C/camera/Wi-Fi回归 | IRQ只在logical CPU0，行为与单核一致 |
| P0 idle | 两核空闲WFI | 可由SysTick/IPI/Mailbox唤醒，CP不进LV |
| P1低压 | 短/长timer deadline及RTC/GPIO唤醒循环 | deadline唤醒不丢失，时间不倒退，480 MHz/bus恢复后再运行AP |
| shutdown | 请求AP recovery | park/reset CPU2、释放AP PSRAM owner、关CPU2、关CPU1、最后释放频率slot |

故障注入：

- CPU2保持reset、secondary vector错位、CPU2 IRQ route屏蔽。
- channel 2 FIFO full、丢kick、重复kick、pending bit竞态。
- CP拒绝480 MHz、强制320 MHz clamp、readback不一致。
- bus divider写失败、切频helper返回错误、PSRAM仍ON时请求降到60 MHz。
- heartbeat send返回 `-EBUSY`/`-ENOLINK`。
- AP在CPU2未park时请求shutdown。
- PSRAM OFFLINE/recovery期间CPU2发起分配。
- P1阶段只让一个AP核设置WFI bit，确认CP拒绝进入low-voltage。
- P1恢复时暂不恢复480 MHz，确认AP不解除PM barrier。

## 13. 完成定义

只有同时满足以下条件才算完成：

1. 最终包仍只有一个AP `app1.bin`，CPU1/CPU2共享同一个NuttX实例。
2. CP在CPU1 reset前建立并验证480 MHz AP档位，运行期不发生未协调DVFS。
3. `CONFIG_SYSTEM_TIME64=y`，旧的长期静置重启不可复现。
4. CPU2使用独立bootstrap、idle和interrupt stack，private arch状态正确。
5. CPU2启动失败fail-stop，不发送虚假PWC ready。
6. 双向schedule/call IPI无丢失，SMP调度和task migration稳定。
7. AP_SPINLOCK atomic gate通过两核、DMA和外设并发压力。
8. 只有logical CPU0产生系统tick，wall clock和heartbeat周期正确。
9. CP transport和普通外设保持logical CPU0 owner且SMP-safe。
10. CPU2先park/reset，CP依次关闭CPU2/CPU1，最后释放480 MHz slot并重新仲裁VDDDIG。
11. S0-S4完成后通过24小时SMP稳定性测试。
12. P1只有在NuttX SMP PM、AON时间、deadline wake、双核WFI barrier和恢复协议完成后
    才标记支持。

## 14. 实施顺序总结

```text
SYSTEM_TIME64 + CP 480 MHz单一生命周期slot
  -> 单核3小时验证
  -> SMP配置、AP_SPINLOCK和每核栈
  -> CPU2 secondary vector/reset/online
  -> channel 2 raw IPI和NuttX SMP hooks
  -> driver affinity与共享锁
  -> PSRAM/Wi-Fi/camera等功能回归
  -> 24小时稳定性
  -> AON-backed time和coordinated low-voltage
```

不要把低压睡眠、runtime DVFS、cacheable PSRAM、CPU2 hotplug和首次SMP启动放进同一
提交。先证明固定480 MHz、64位时间、cache-off、单tick、CPU0外设owner模式下的双核
调度正确，再逐项开放功耗和性能优化。

## 15. SMP 实板调试记录

> 来源：`external/bk_avdk_smp/README.md` 原第 330-755 行。以下内容是连续保留的
> SMP 实板调试历史记录，其中判断和哈希均只对应记录当时的版本；历史 hash 不能视作
> 当前产物，也不得据此将历史判断改写为当前状态。

实板首次联调发现 AP 在 `arm_serialinit()` 的 scheduler-lock 上下文中等待新建 mailbox
worker，导致 worker 无法运行、物理 mailbox 未启动便进入 SMP，最终 CP 在 stage 4
收不到 `0x5`。当前 AP 已拆分初始化：early serial 阶段只初始化逻辑状态、物理 MBOX
IRQ 和 console，`AppBringUp` 再启动 mailbox/MB-UART worker 并验证 CPU0 affinity。
`0x5` 也已移到 PWC callback 注册和worker就绪后。早期版本曾用PWC `0x12`替代原厂
power-up/heartbeat，导致基础liveness依赖UART STATE和PWC `0x5` completion；该迁移方案
现已废止。AP恢复在HW_CTRL `0x10`发送原厂command 1/2，PWC仅保留`0x5`和`0x11`。
CP stage-4等待窗口由3秒放宽为10秒，失败时保留错误和rollback现场，不再由`cp_main`
断言触发整芯片循环复位；最终`0x11` commit条件不变。

第二次实板联调仍停在 stage 4，且 CP 未收到 AP IPC power-up。AP early serial 中
`bk7258_mb_uart_init()` 的初始化 `printf()` 发生在 idle stdio 建立前，现已移除。
同时在 AP_SPINLOCK 尾部 `0x2800ffe0..0x2800ffff` 加入不依赖 mailbox/console 的启动
轨迹；CP timeout 会打印 `AP trace=<magic> primary=<stage> secondary=<stage>` 和5个
detail word。magic 应为 `0x41505452`。primary 1..4 表示 CPU1入口至 `nx_start`，
10..14 表示 early serial，20..24 表示 CPU2启动/IPI，30..34 表示 late bring-up至
`0x5` transport ACK；secondary 1..5 表示 CPU2入口、private、NVIC、mailbox和idle。

共享轨迹首次返回 `primary=2, secondary=0`，证明 CPU1 已进入 `bk7258_start()`，但
未完成主核 private 初始化。根因是启动代码在清零 BSS 前调用 `modifyreg32()` 开启
CPACR，而 NuttX `modifyreg32()` 使用 BSS 中的 spinlock；AP-only reset 后保留 SRAM
可令该锁保持占用，CPU1 永久自旋。当前已改为与 Armino `SystemInitCpu0()` 一致的
无锁 CPACR 直接读改写。`__start` 同时清零全部 detail word；primary 5/6/7/8/9
分别表示 CPACR、data copy、BSS clear、runtime vector copy 和 atomic gate 初始化完成。

下一轮轨迹返回 `primary=22, secondary=5`，证明 CPU1 已释放 CPU2 reset，CPU2 已完成
private/NVIC/mailbox 初始化并进入 secondary idle，但主核没有观察到 online。原实现
在 online 发布前开启 CPU2 全局中断，且 online/boot-test flag 全部使用同一个 libc
atomic hwspinlock；CPU1 以480 MHz紧密轮询该非公平锁，可令 CPU2 的一次 store 永久
饥饿，原厂 Armino secondary scheduler handoff 也不会在发布 ready 前开启中断。当前
bootstrap 已改为专用 raw Mailbox `BOOT_NOTIFY/BOOT_PING` 和 volatile shared flag加
barrier，不再依赖通用 atomic gate；CPU2 顺序固定为interrupt stack、online、notify、
最后开中断。secondary 6/7/8/9分别表示这四个阶段。正常 scheduler/call IPI 仍使用
原 atomic pending 状态机；500 ms诊断轮询中的无界 `wfe` 也已移除。

再下一轮轨迹推进到 `primary=22, secondary=9`，证明 CPU2 已完成 stack、notify 和
全局中断开启，而 CPU1仍未完成启动握手。`primary=22` 还可能表示 CPU1在第一次
读取系统 tick或等待 CPU1 Mailbox IRQ 时停住；该阶段不能要求 arch timer 已可靠
递增，也不应依赖启动期 IRQ 调度或跨核共享 ready flag。当前握手因此改为完全由
硬件 FIFO证明：CPU2发送 `BOOT_NOTIFY`，CPU1屏蔽本核中断并主动 drain channel 1，
收到 notify 后向 CPU2发送 `BOOT_PING`，CPU2的 channel 2 IRQ回 `BOOT_ACK`，CPU1
轮询收到 ACK 后才返回 `up_cpu_start()`。CPU1只更新自己的本地状态，不再读取
CPU2写入的 online flag；DWT cycle counter和纯迭代上限共同保证 AP先于 CP的10秒
门限留下诊断。secondary stage 10表示 CPU2已收到 ping并发出 ACK。

该版本实板轨迹为 `primary=24, secondary=10`，确认双向 hardware FIFO handshake 已
完整成功，停点已移动到 CPU1恢复本核中断或紧随其后的 NuttX SMP启动收尾。CPU1
主动 drain channel 1时硬件仍可能保留 Mailbox NVIC pending；当前在恢复中断前显式
清除该 pending，并把窗口继续拆分：primary 25表示准备恢复中断，26表示首个 IRQ已
进入（detail[2]保存 IRQ号，15是 SysTick、79是 Mailbox），27表示恢复中断调用已
返回，28表示 `up_cpu_start()` 即将成功返回，30仍表示 board late bring-up入口。

细分后的实板轨迹为 `primary=26, secondary=10, detail[2]=0x0f`，精确确认 CPU1
永久停在恢复中断后的首个 SysTick。首 tick 的 scheduler/atomic路径会立即使用 libc
全局 atomic gate，而原 gate以 SRAM `LDAEX/STREX`实现。BK7258原厂 spinlock源码
明确说明芯片只有两个 SRAM exclusive monitor，并对三个活跃核心参与 exclusive
access的配置直接编译报错；当前 CP、AP CPU1和AP CPU2三核同时运行，AP exclusive
序列可能永久失败。当前 AP-only atomic gate已改为两个AP逻辑核之间的 Peterson锁，
只使用位于 inner-shareable non-cacheable AP_SPINLOCK区的普通对齐 word store和
barrier，不再占用 exclusive monitor。NuttX上层 atomic/hwspinlock API保持不变，
`.bk_spinlock`由4字节增至20字节。普通 `spinlock_t` 已启用
`CONFIG_TICKET_SPINLOCK`，其 `atomic_cmpxchg` 也统一经 `CONFIG_LIBC_ATOMIC_HWSPINLOCK`
进入该 gate。Peterson trylock 每次失败都会撤回本核 intent，同核递归进入使用 owner/depth
计数，避免首个 tick 与 CPU2 IPI 并发时把失败状态遗留在 gate 中。

Peterson gate版本实板仍返回相同的 `primary=26, detail[2]=0x0f`，因此 exclusive
monitor是必须消除的独立三核风险，但不是当前首 tick停点的充分解释。继续对照原厂
FreeRTOS SMP发现其固定 `configTICK_CORE=CPU0`，tick owner直接推进内核 tick；当前
NuttX配置却启用了 `CONFIG_TIMER_ARCH`，将SysTick包装为通用 timer lower-half，首 tick
的 watchdog时间查询又通过 `clock_systime_ticks()`回读正在执行的同一 SysTick
lower-half。当前已关闭 `CONFIG_TIMER_ARCH`，改为 logical CPU0专属的直接 SysTick ISR，
CPU2继续保持私有 SysTick关闭；生成配置和最终 map已确认不再包含 `timer_callback`、
`up_timer_gettick`或`systick_initialize`，系统时间恢复为 `g_system_ticks`。首 tick新增
detail[3]诊断：0表示尚未进入BK7258 timer ISR，1表示已进入并正在执行
`nxsched_process_timer()`，2表示timer核心处理已返回。

direct SysTick版本上一轮实板轨迹为
`primary=26, secondary=10, detail[2]=0x0f, detail[3]=1`；随后修复版已推进到
`detail[3]=2, detail[4]=a8000000`。最终反汇编确认
`nxsched_process_timer()`第一条调用是`clock_timer()`，而`clock_timer()`在更新64位
`g_system_ticks`前立即通过`atomic_load_4()`和`atomic_compare_exchange_4()`进入
`g_atomic_hwspinlock`，所以该日志只能确认首 tick停在首次atomic/seqlock之后，不能直接
断言已经进入scheduler的`enter_critical_section()`。修复版的`detail[3]=2`则证明
`nxsched_process_timer()`已完整返回；`detail[4]=a8000000`表示该次atomic gate已完整释放。
本轮对Peterson gate补充owner/depth、
失败intent回滚和首tick状态探针；`detail[4]`高字节含义为A1入口、A2同核递归、A3 owner
忙、A4发布intent、A5双核冲突、A6获得gate、A7递归unlock、A8完整释放。NuttX ticket
`spin_trylock_notrace()`也修复为使用私有expected变量，避免CAS失败改写`lock->owner`。
SMP raw kick继续使用1024次有界FIFO重试，并在发送失败时清除`g_ipi_kicked`；pending
bit仍保留，避免错误状态伪装成已发送。首 tick当前停在`clock_timer()`的首次atomic之前，
尚无证据表明已经运行到round-robin远端IPI路径。

以下是 2026-08-06 本次 CP lifecycle 修改后的 OpenVela AP 打包输入：

```text
cmake_out/bk7258-ap_nsh/nuttx.bin
bk_avdk_smp/build/openvela-ap.bin
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
SHA256 af4334f41c3a8fe125ea3e4201a38c55a2a8905c1125938cb5f56dd8da8d7926
```

本轮使用官方容器、项目级 clean 和 `EXTRA_CFLAGS=-Werror` 完整重建后的最终包：

```text
all-app.bin SHA256
9cb3626ffc41518293a48c3a088185cd38621dbbc0728bb0bda7ac55b69af37e

app_ab_crc.rbl SHA256
2537425fc3ac8d4b80b474db1f16e772d37d9ea097cf23de477402caa225628d
```

本轮已验证 CP `-Werror` 编译、最终打包、生成配置门禁、16 MiB 分区门禁和 AP 输入
逐字节一致性；尚未对新增 `0x11` commit/abort、recovery INIT 取消和 FINISH shutdown
执行实板验证。

2026-08-07 针对实板 `primary=26, secondary=10, detail[2]=0x0f,
detail[3]=2, detail[4]=a8000000` 的修复：

- 首个 SysTick 不再在 `clock_initialize()` 内立即启动；只安装 handler 和
  reload，待 `up_cpu_start()` 完成 CPU2 handshake、`nx_smp_start()` 返回后，
  在 board bring-up 开始时启动，避免首 tick 抢占 `up_cpu_start()` 的 IRQ
  restore/exception return 窗口。
- `up_send_smp_sched()` 和 `up_send_smp_call()` 不再因为 CH2 FIFO 短时满而
  直接 `PANIC()`。pending bit 保留、`g_ipi_kicked` 在发送失败时清零，CPU0
  的后续 SysTick 会重试合并后的 raw kick；同一目标的多个通知仍合并为一个
  FIFO entry。
- CH2 raw kick 发送增加 `detail[4]` 探针：`B1` entry、`B2` 已有 kick、
  `B3` FIFO send 成功（低16位为重试次数）、`B4` 发送失败（低16位为 errno，
  `0xffff` 为 FIFO retry timeout）。
- CPU2 secondary 增加 liveness 阶段：`11` 已完成 `sched_note_cpu_started`、
  `12` 正在等待 CPU0 进入 idle loop、`13` 已开始 idle；Mailbox IRQ 阶段
  `14` 入口、`15` 收到 raw kick、`16` 退出。Mailbox 单次 IRQ 最多处理 8
  个 FIFO entry，避免持续输入造成无限排空循环。

2026-08-07 实板新轨迹为 `primary=31, secondary=16`，说明上述 SMP 修复已生效：
CPU2 已完成 raw-kick Mailbox IRQ，CPU2 启动函数和 `nx_smp_start()` 已返回，
AP 已进入 late bring-up 并完成 mailbox worker 创建。当前新卡点位于 UART0
RESET/STATE link probe 之后、`primary=32` 之前。根因是 CP CPU1 启动等待原先
在 10 秒内忙轮询 AON RTC，抢占了唯一执行 `bk_mb_uart_poll()` 的 `ap_console`
线程，CP 硬件 IRQ 虽然收到了 AP descriptor，但逻辑 STATE/ACK 没有被调度处理。
本轮 CP 等待循环每次迭代加入 1 ms RTOS delay；AP `bk7258_mailbox_send_wire()`
也不再把物理 FIFO `-EAGAIN` 伪装为发送成功，避免 UART transaction 永久 busy。
AP UART link 等待从 1 秒延长为 8 秒，与 CP 的 10 秒 CPU1 boot transaction
deadline 保持余量。

本轮实板进一步推进到：

```text
ap0: mailbox UART0 link ready
CP OpenVela PM stage=5
```

这证明AP/CP RESET/STATE link、AP的`PM_CPU1_BOOT_READY_CMD=0x5`以及CP接收路径均已
成功。随后出现的`IPC[1]heartbeat timeout`证明将`0x5`直接映射为`CORE_POWER_ON`
会在heartbeat source尚未可靠建立时提前武装CP看门狗。随后采用的PWC `0x12`翻译层
仍把基础liveness绑定在更复杂的PWC启动事务上，最新实板继续出现`IPC retry`。
当前协议因此恢复原厂职责：HW_CTRL command 1触发`CORE_POWER_ON`，command 2每2秒
刷新heartbeat；PWC `0x5`只表示SMP/PWC worker milestone，`0x11`表示PSRAM/clock/SMP
最终commit或rollback。

当时实板日志再次返回 `primary=31, secondary=16`，没有到达 `primary=32`；该次
失败发生在 AP UART0 STATE probe，尚未执行后续liveness消息。AP 启动等待现在每
100 ms 主动重发 STATE request，并主动 drain MBOX RX，覆盖初始 RESET/PROBING
跨 worker 唤醒丢失的窗口；probe 超时额外打印 MB-UART 统计，用于区分 ACK 丢失、
远端协议错误和本地 worker 未发送。进一步新增 CP stage-4 timeout 快照，输出
`mbox0_adapter` 的 `bad_sid/bad_length/bad_address/overflow` 和逻辑 channel 的
`busy/sequence/command/stale_ack/tx_fault`，用于区分物理接收丢包和逻辑 ACK 丢包。
CP 侧未改原厂 UART/IPC handler。

最新 timeout 快照为 `rx=33 bad_sid=0 bad_length=0 bad_address=0 overflow=0`，
证明 AP 报文已被 CP adapter 接收；`ch=76 (0x4c)` 解码为原厂
`MB_CHNL_SARADC`，`cmd=1` 是 SARADC operation notification。此前 OpenVela AP
未处理该 CP->AP channel，返回 COM_FAIL/造成物理 transaction 恢复，连带阻塞
UART0 STATE ACK。AP 现增加 `0x4c` SARADC request 的原厂 ACK 兼容：收到 START/END
且 `param1=IPC_SARADC_OP_REQ` 时返回 `ack_data1=IPC_SARADC_OP_ACK`，不改变 CP
原厂 SARADC/IPC 实现。

2026-08-07 针对上述 `MB_CHNL_SARADC` 与 UART0 probe 并发窗口的修复：

- AP logical transport 保留最后一次 ACK、active transaction、command header 和
  发送 errno，并按 bit 记录 ACK 拒绝原因：`1` 无 active transaction、`2` control
  不为 ACK_BOX、`4` channel 不匹配、`8` sequence 不匹配、`16` command 不匹配、
  `32` UART ACK payload 非法、`64` ACK state 非法。timeout/fifo/ACK pool 计数器
  通过 boottrace detail[3] 保留低8位 errno、次8位 timeout、再后两组 fifo-full 和
  ACK-overflow 计数。
- AP 物理 MBOX 的 `bad_source/bad_length/bad_address/descriptor_full` 通过
  boottrace detail[4] 保留，便于 CP stage-4 timeout 时区分 envelope 过滤与逻辑
  ACK 处理失败。
- CP 保存最后一次发出的 physical command header、收到的 ACK header 和 ACK data1，
  stage-4 快照额外打印 `AP logical raw cmd/ack/data1`，可直接比较 channel、sequence
  和 command 编码。
- CP 在释放 CPU1 reset 前 quiesce peer logical channel；CP->AP 的本地 UART STATE
  ACK 不再提前解除 quiesce，只有收到 AP `PM_CPU1_BOOT_READY_CMD=0x5` 才恢复普通
  logical transaction。这样 SARADC 保持原运行期行为，但不会在启动 probe 窗口抢占
  唯一 physical transaction。
- CP 输入 fragment 已按构建规则恢复为权威最小配置；本轮 AP、CP、完整打包均使用
  `EXTRA_CFLAGS=-Werror` 通过。

本轮构建产物哈希：

```text
AP raw SHA256
19ea4269285dfa4605ec70d34e4e239889b087d2c678920daad60b61376e2425

all-app.bin SHA256
d562c8244ea794fb5e03337366304dc7311c5fa2809b646bed70fd76f000d185

app_ab_crc.rbl SHA256
91e6e69739219128050c66b85001fb0a2dc608e5348b6f066daa20c485ff2360
```

上一轮最终包哈希（已过时，仅保留历史对照）：

```text
AP raw SHA256
f2985d218c6e6ae63d9e7f90f81ae3fd32c7e8f70322158dbf6d5bd9f06b97a4

all-app.bin SHA256
84b46d106870e8b817a958a202cf436a7b0e994f828d1ac1404e8d1e49321b94

app_ab_crc.rbl SHA256
364779a083d4366a858ce06059647d9f57aa9771c1c06eab378cf85c9ad5cb54
```

本轮新固件重点观察：

```text
secondary=12: CPU2 已完成 ACK/调度启动，尚未进入 idle loop
secondary=13: CPU2 已进入 idle loop
secondary=15: CPU2 收到首个 raw kick
secondary=16: CPU2 完成首个 raw kick 的 Mailbox IRQ 处理
detail[4]=b1xxxxxx: CPU0 进入 raw kick
detail[4]=b2xxxxxx: kick 已被现有 g_ipi_kicked 合并
detail[4]=b3xxxxxx: FIFO 写入成功
detail[4]=b4xxxxxx: FIFO 写入失败或 1024 次重试超时
```

2026-08-07 针对实板出现 `mailbox UART0 link ready` 但未出现 `nsh>` 的修复：

- `board_late_initialize()` 在 `bk7258_pwc_start()` 返回前不会创建 `nsh_main`；
  因此 `stage=5` 只证明 CP 执行了 `0x5` boot-ready handler，不证明 AP 已收到该
  PWC transport ACK。
- CP mailbox RX adapter 现在在上层 ACK 队列达到 `MB_ACK_QUEUE_LEN - 1` 时停止接收
  普通 command，为当前 command 保留一个 ACK 槽，避免先执行 PWC callback 再因 ACK
  队列满而丢 ACK，形成 CP 已到 stage=5、AP 永久等待 PWC ACK 的状态。
- AP link wait 在重发 RESET/STATE probe 前再次确认 link state，避免 TX worker 刚完成
  STATE ACK 后被等待线程把 READY 链路重新切回 PROBING。
- 后续实板稳定复现 CP `stage=5`、AP 无 PWC 成功或失败日志，说明 CP 已处理 `0x5`，
  但 AP 的同步 PWC wait 没有消费返回 ACK。`wait_channel()` 现在和 UART link wait 一样
  每 1 ms 主动 drain MBOX RX；timeout 前先输出完整 mailbox stats。这样 PWC transport
  不再依赖 `board_late_initialize()` 阶段的 Mailbox IRQ 调度时机。

2026-08-10 实板在上述主动 drain 后仍稳定停在 `stage=5`。AP 的 500 ms PWC timeout
会将 UART link 一并切入 recovery，因此 timeout 文本只进入 AP UART ring、无法转发到
CP；日志中约 8 秒后的 `IPC retry to start core1` 不是 AP 仍在等待 8 秒。进一步确认
CP physical adapter 的 stable ACK slot 只由后续 CP command ACK fence 释放；启动阶段的
流量主要是 AP->CP UART/PWC command，没有该 fence。早期 UART backlog 会耗尽 4 个 ACK
slot，CP 仍先执行 `0x5` callback 到达 `stage=5`，但无法发布对应 transport ACK。

本轮将 CP physical ACK pool 扩为 8 个；CP 收到下一条普通 AP command 时，利用 AP
logical transport 的单 active-transaction 串行约束作为精确 fence，立即释放此前 ACK
slot。目标 FIFO 连续空闲 20 ms 后的 quarantine 回收作为无后续 command 时的兜底。
heartbeat 启动重试现在同时输出 CP physical/logical mailbox diagnostics，若仍失败可直接
查看 `slot_busy`、`ack_slots`、`ack_retry` 和 raw header。AP PWC receive callback 原先在
logical mailbox 已持有 driver lock 时递归获取同一 SMP spinlock；该回调现复用调用者的
临界区，避免后续 PSRAM/ready response 到达时死锁。

2026-08-10 新诊断显示 physical ACK pool 已正常回收：`slot_busy=0`、`ack_full=0`、
`ack_retry=0`、`ack_slots=0`，但启动期仍有多次 `0x4c` SARADC transaction timeout 和
transport recovery。为隔离 PWC `0x5` 链路，app_ab CP 项目明确关闭
`CONFIG_SARADC_MB`，不再初始化或发送跨核 SARADC START/END command；CP 本地 ADC、
温度和 RF 校准保持启用。`saradc_notify.c` 恢复原厂状态，不再作为比赛仓覆盖文件。
CP 诊断保留最后 physical RX header、最后成功发布的 transport ACK header、ACK 计数和
AP boottrace 快照，用于排除 ADC 后继续定位其他 transport 问题。

2026-08-10 后续实板日志显示 AP 已复制到正确的 `0x5` ACK（`12041005`），但仍停在
`primary=33`。对照发送和接收时序后确认 AP transport 存在发送前置状态竞态：
`dispatch_locked()` 原先先发布 physical mailbox 指针，随后才设置 `active->busy=true`；
双核 CP 可能在 `bk7258_mbox_send()` 返回前立即发送 ACK，`handle_ack()` 会把该 ACK
判为 `no active transaction` 并丢弃，事务随后永久等待。现已在发布指针前设置
`active->busy`，并预分配 transaction order；发送失败时回滚 busy 状态。这样 ACK 从
物理发送返回前到达也能正确匹配当前 PWC/UART transaction。

2026-08-10 最新实板仍显示`mailbox UART0 link ready`、CP `stage=5`、AP
`primary=33`，8秒后继续`IPC retry to start core1`。该结果证明PWC `0x12`迁移未解除
legacy `CORE_STARTING`依赖，且基础liveness仍被前置PWC transaction阻断。本轮按
`bk_avdk_smp_original_backup`恢复原厂契约：AP在UART link ready后、PWC `0x5`前，通过
HW_CTRL `0x10`发送16字节command 1并等待600 ms完成ACK；独立worker每2秒发送command 2，
4字节payload固定使用原厂SWAP地址`0x2809f900`。CP仅删除PWC `0x12`定义和翻译handler，
原厂`mb_ipc_cmd.c`、`mb_ipc_heartbeat.c`保持不变。PWC worker在`0x5`前完成有界启动，
`0x5/0x11`继续承担OpenVela SMP/PSRAM/clock生命周期，不再承担power-up门控。

为避免恢复HW_CTRL后仍被PWC并发故障阻断，本轮同时收敛AP PWC可靠性：所有PWC TX
通过单一mutex覆盖send/wait，worker的semantic response不再与`0x5/0x11/0x7`同步请求
竞争同一logical channel；PWC RX软件队列满时返回`-EAGAIN`并暂不回transport ACK，
worker释放queue slot后主动kick physical RX重试，避免“已ACK但语义消息静默丢失”。

本轮clean `-Werror`构建和最终打包产物：

```text
OpenVela AP / build/openvela-ap.bin / tmp/app1.bin SHA256
e9a390dd9e3111193ac6c98692f62c8cda12a69081564c1a49f59346f4fd81ed

all-app.bin SHA256
69c271fe38c59f9c7fde6f491b298665589bdc3e82c2f2f6c8b347aa3eb34b9b

app_ab_crc.rbl SHA256
ebd4ab8638d5e25652f648783fa2b4691ad32023279ee333c467aaac76528c52
```

2026-08-11 实板首次验证原厂HW_CTRL恢复版时，UART link ready后立即出现
`HW_CTRL power-up service failed, error=-1`，而mailbox统计仍为`tx=8/rx=8`、last command
仍是UART `0x19`，没有HW_CTRL `0x10` transaction。`last_tx_error=1`对应`EPERM`：新建
heartbeat worker设置CPU0 affinity后尚未完成迁移，仍在AP logical CPU1上执行；physical
MBOX按设计拒绝CPU2向CP发送16字节descriptor。随后修复版返回`-EXDEV`，证明在该
heartbeat worker内等待迁移仍不可靠。当前实现让`bk7258_mailbox_send_wire()`只登记
logical transaction并唤醒已有CPU0-bound `mbox-v2` TX worker；物理descriptor只由该worker
调用`bk7258_mbox_send()`。创建mailbox worker前，AP将创建者临时固定到logical CPU0，
使所有kernel worker从首次调度起继承CPU0-only affinity。没有放宽CPU2 physical mailbox
权限，也没有修改CP command 1/2 ABI。

本轮最新版本另外完成了AP `distclean`、`-Werror`完整构建、CP clean容器构建和标准打包。
AP raw、`build/openvela-ap.bin`和`package/tmp/app1.bin`逐字节一致；以上SHA-256为本轮
最终产物，仍待实板确认HW_CTRL cmd1 ACK、PWC `0x5/0x11` commit和`nsh>`。

2026-08-11 下一轮实板已确认HW_CTRL cmd1和PWC `0x5`均成功，但CP heartbeat timestamp
仍停在cmd1时刻，约8秒后原厂`mb_ipc_task` assert并重启。进一步核对当前生成配置和
`System.map`后确认：本固件为`CONFIG_SMP=y`且未启用`CONFIG_BMP`，所以
`DEFINE_PER_CPU_BMP`在当前构建中退化为全局单实例；`g_system_ticks`和
`g_wdactivelist`不是此前推断的per-CPU对象。当前修复保持CPU0-only SysTick和完整
`nxsched_process_timer()`不变，删除未经实板证明会持续计数的AON RTC heartbeat时基；
cmd1 ACK后按原厂时序建立2秒NuttX tick deadline，不再紧接cmd1发送cmd2，后续cmd2由
CPU0-only `mbox-v2` TX worker每2秒调度。前三次提交和ACK输出限量诊断。原厂cmd2 ABI、
CP handler、watchdog和16 MiB PSRAM layout均未修改。

2026-08-11 实板验证上述版本时，四次启动均有cmd1 ACK和PWC `0x5` ACK，但完全没有
`heartbeat queued`，同时PWC在发送PSRAM power request后也没有触发1秒timeout，直到CP在
约8秒后复位。这证明故障位于cmd2提交之前，且不是heartbeat单一状态机问题，而是所有依赖
NuttX tick的超时和周期worker均未继续推进。根因是`bk7258_timer_start()`原先作为late
bring-up第一条操作执行，而bring-up线程要到后续`bk7258_mailbox_workers_start()`才设置
CPU0 affinity；非debug构建中的`DEBUGASSERT(up_cpu_index() == 0)`不能形成运行时保证，
因此私有SysTick可能在logical CPU1启动。

当前修复通过`nxsched_smp_call_single(0, ...)`让SysTick寄存器写和使能确定发生在logical
CPU0；bring-up和heartbeat启动线程也显式限制为CPU0。cmd1 ACK后的heartbeat enable和2秒
deadline由CPU0 mailbox完成回调设置，避免跨核发布竞态。AON RTC不属于当前heartbeat
实现，旧的secure地址常量仍未作为协议时基使用。完整clean `-Werror`构建和打包产物：

```text
OpenVela AP / build/openvela-ap.bin / tmp/app1.bin SHA256
987ffd1b0a38a02ce4fa07e75ce5f01d55abdda5d99b93002a9688f30db4edee

all-app.bin SHA256
102d46ac3d7c3c1f554d1070c5281dcc2be7b9167e508d79f9b19bde683c765d

app_ab_crc.rbl SHA256
96ac8b6b6e7f4efd92c5e7f9ee19bc5d2211485445ef2ca222c8727e440ab3bf
```

下一次实板必须先看到前三组`heartbeat queued`/`heartbeat ACK result=0`且30秒内无CP
heartbeat assert，再确认PSRAM response、PWC `0x11`、stage 6、`nsh>`。按键震动worker
在这些步骤和Wi-Fi初始化之后才创建；只有看到`power-key motor worker ready`后，按键/震动
故障才可作为独立GPIO/PWM问题分析。

2026-08-11 对`all-app.bin=102d46ac...`的实板验证显示该版发生启动回归：CP快照为
`primary=31, secondary=10`，UART STATE物理请求/ACK分别为`19020001/19021001`，证明
descriptor和匹配ACK已到AP，但CPU2仍停在boot ACK后的exception-return窗口，未到
secondary scheduler阶段。该版在late bring-up入口先改变初始化线程CPU affinity，再通过
`nxsched_smp_call_single(0, ...)`启动SysTick；两步都会在该敏感窗口触发SMP IPI，复现了
历史上的首tick/调度IPI过早抢占问题。因此`102d46ac...`已废弃，不得继续烧录。

当前修复不再从bring-up线程发送SMP call。在任何worker创建和父线程setaffinity之前，先只读
轮询boottrace直到`secondary >= 12`；因此CPU2从stage 10到scheduler ready的敏感窗口内不会
收到调度IPI。随后父线程固定CPU0，`mbox-v2`从首次指令起继承CPU0-only affinity，并直接
调用`bk7258_timer_start()`写CPU0私有SysTick。timer函数仍运行时检查
`up_cpu_index()==0`，但不再向CPU2发送任何IPI。反汇编已确认secondary stage检查位于
`sched_setaffinity`和`kthread_create`之前，worker中的timer调用也直接写SysTick寄存器。
heartbeat ACK同核发布和`heartbeat armed`诊断继续保留。

当前clean `-Werror`构建和标准打包产物：

```text
OpenVela AP / build/openvela-ap.bin / tmp/app1.bin SHA256
87ac12950fa5a22487c315efb75e0f3dfb40657ce1e1df2623561001ae7a096d

all-app.bin SHA256
1808720ced6480cf7dbdf175308036820877df21b24252749bf99d4ac1949dcd

app_ab_crc.rbl SHA256
02a6e6f8637606564d00b486f0791b41f70c6cd879724099646a10fb932d0354
```

2026-08-12 SMP IPI必达修复及启动回退检查点：

- 代码审计确认普通SysTick无法抢占本核正在执行的`hwspin_lock_irqsave()`：gate获取前
  `BASEPRI`已屏蔽普通中断，且Peterson gate通过`owner/depth`支持同CPU重入。因此
  “SysTick抢占同核atomic gate持有者并永久自锁”不是当前代码可成立的因果链。
- 实板CP在约2秒间隔内分别采到`detail[3]=0x001cc702`和`0x00248902`，tick计数增加
  `0x7c2=1986`；两次`detail[4]`均为`0xa8000000`，表示atomic gate完整释放。这直接
  排除CPU0长期关中断自旋和tick停摆。
- `bk7258_smp_kick()`原先在1024次FIFO重试后保留pending并返回，但只有CPU0 SysTick
  会补发CPU0到CPU1的pending；CPU1到CPU0的scheduler delivery或同步SMP call可能
  永久失去doorbell。当前普通SMP kick与RP2040契约一致，等待mailbox接受doorbell后
  才返回；非`EAGAIN`硬错误不再伪装为成功。SysTick ISR删除非标准
  `bk7258_smp_retry_pending()`，标准`nxsched_process_timer()`保持原位不动。
- PWC增加`primary=54`创建失败阶段和负errno detail，避免`kthread_create("pwc")`
  立即失败与停在创建过程都显示为`primary=48`。
- 修改前镜像`df6958cf...`可进入NSH、UART link ready、heartbeat ACK，并最终停在
  `primary=48, secondary=13`。上述IPI行为修改后的镜像出现启动回退：NSH可见，但
  UART link未ready，CP两次采样均停在`primary=31, secondary=13`，最终仍为
  `CPU1 boot timeout stage=4`。因此IPI必达缺陷真实存在，但尚不能视为当前启动失败
  的充分根因；最直接的回退相关行为差异是SMP kick从有界失败/后续tick补发改为发送者
  等待FIFO接受，以及tick ISR删除补发调用。

本检查点构建和烧录产物：

```text
OpenVela AP / build/openvela-ap.bin / tmp/app1.bin SHA256
d476f72daa453a72931406a1972d21d50a88a0a322659200f9a29bb5979327eb

all-app.bin SHA256
6f0bcb1e75efaa98311b58317455b6c2329452ec15468d0e5891dac11c6ae91c
```

独立NuttX工作树保持基线`dd92bcf4257`原始状态，不包含任何本地修改。后续诊断和修复
不得修改NuttX内核，范围限定在BK7258 board/SoC port与AP/CP集成层。
