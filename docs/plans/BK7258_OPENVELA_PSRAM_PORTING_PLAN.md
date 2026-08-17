# BK7258 OpenVela 16 MB PSRAM 移植实施与验证方案

> 文档状态：基于当前已成功启动的 OpenVela AP、Mailbox 日志链路、
> `BK7258_OPENVELA_AP_PORTING_PLAN.md`、`bk_avdk_smp` PSRAM 实现、
> `vendor_beken` BK7236N 参考适配和当前 NuttX 内存管理源码形成的实施基线。
> 本文目标是让 OpenVela AP 按原厂 16 MB 布局管理其 AP侧 PSRAM区域，为后续音频、视频、
> 图形和 AI 数据面移植提供内存。PSRAM 器件型号、地址线完整性、电源时序、
> cache 一致性和低功耗恢复必须通过实板测试闭环，不能仅凭链接成功宣称完成。

## 1. 目标与结论

当前BK7258 OpenVela AP已完成物理CPU1启动、Mailbox日志传输、16 MiB PSRAM硬件
映射、运行时allocator和媒体pool代码基线：

```text
AP 私有 SRAM: 0x28010000..0x28064000, 336 KiB
当前 NuttX SRAM heap: 0x28014b28..0x28064000，约317.21 KiB
OpenVela PSRAM allocator: AP heap及四个媒体pool已接入，待实板验收
```

本文沿用原厂“16 MB”产品/配置命名，但容量数值`0x01000000`严格等于16 MiB
（16,777,216字节），不是十进制16,000,000字节。目标板PSRAM在CPU地址空间中的
memory-mapped半开区间固定为：

```text
PSRAM_BASE = 0x60000000
PSRAM_SIZE = 0x01000000
PSRAM_END  = 0x61000000
```

本方案采用以下正式架构：

1. 物理 CPU0/CP 继续作为 PSRAM PHY、LDO、电压、时钟、控制器、器件 ID 检测
   和全局电源状态的唯一 owner。
2. OpenVela AP 不调用 `bk_psram_init()`、`bk_psram_deinit()` 或重新配置 PSRAM
   控制器，避免与 CP 冲突。
3. PSRAM继续按原厂 `lvgl/img_decode`和`lvgl/freetype_font`的 16 MB布局分区：
   四个媒体 slab、128 KiB CP heap、2.875 MiB AP heap和6 MiB AP section。
4. OpenVela接管原厂 AP侧合计15.875 MiB区域；CP保留128 KiB
   `CP_PSRAM_HEAP`，不得与Vela allocator重叠。
5. OpenVela使用独立 `mm_heap_s`管理 `AP_PSRAM_HEAP`，并为四个媒体 slab建立
   保持原用途边界的独立pool；不通过 `arm_addregion()`、
   `umm_addregion()` 或 `kmm_addregion()`并入 NuttX 通用 heap。
6. `AP_PSRAM_SECTION`保留给后续经过审计的静态 `.psram.data/.psram.bss`，不得
   偷偷并入AP动态heap或媒体pool。
7. NuttX 内核对象、TCB、调度器、Mailbox 控制块、PWC 状态、关键线程栈和
   故障恢复状态继续驻留片内 SRAM。
8. 音视频帧、编解码工作区、显示 buffer和音频 ring通过对应媒体pool分配；
   一般AP大对象通过 `AP_PSRAM_HEAP`分配。
9. 首版 PSRAM 配置为 Normal、RW、XN、non-cacheable；在 DMA 和双核一致性
   完成专项验证前不启用 cacheable PSRAM。
10. CP 必须检测到 16 MB 器件，容量不匹配时 OpenVela 禁止建立 allocator，
   不允许当前参考驱动的“只告警后继续运行”。
11. PSRAM默认在AP生命周期内保持上电。动态掉电只在AP和CP heap、四个媒体
    pool、静态section及DMA owner全部空闲，且所有outstanding allocation均为0
    时允许；音视频调试阶段建议关闭自动掉电。
12. 板载容量为16 MB不表示Vela拥有一个16 MB连续heap。原厂布局中的Vela/AP
    总区域为15.875 MiB，且被128 KiB CP heap分隔为多个有用途边界的区域。

### 1.1 完成定义

只有同时满足以下条件，才能称为“OpenVela按原厂布局可用16 MB（16 MiB）板载PSRAM”：

- CP 实板日志确认器件 ID 对应 16 MB 型号。
- 生成布局中 `CONFIG_PSRAM_CAPACITY == 0x01000000`。
- OpenVela只管理原厂AP侧区域，总计 `0x00fe0000`，不覆盖128 KiB CP heap。
- CP map只声明 `0x60700000..0x6071ffff`为 `CP_PSRAM_HEAP`，CP运行时allocator不得越界。
- OpenVela MPU 明确覆盖 16 MB，属性为 RW/XN/non-cacheable。
- 全地址地址线、alias、walking-bit、随机读写和边界测试通过。
- AP heap和四个媒体pool可分别分配、释放、对齐分配并报告统计。
- AP section边界和静态对象加载时序通过验证。
- Vela/AP侧总承载量可达到接近15.875 MiB，但其中只有7 MiB媒体pool和2.875 MiB
  AP heap是动态分配区，另6 MiB是只允许经过审计的静态section对象使用的保留容量；任何区域
  都不会跨越CP heap。
- Vela测试不会读写CP heap；CP heap由CP自身测试和统计。
- Mailbox heartbeat、PWC 和日志压力下 PSRAM 数据不损坏。
- 音视频 DMA 测试通过，buffer 地址、长度、对齐和 cache 维护规则明确。
- AP reset、全芯片 reset 和 PSRAM recovery 不产生旧指针复用或静默损坏。

### 1.2 当前状态

| 项目 | 当前状态 | 说明 |
| --- | --- | --- |
| 板载容量 | 用户确认为 16 MB | 仍需 CP 器件 ID 和地址线测试确认 |
| `app_ab` 配置容量 | 16 MB | `ram_regions.csv` 当前写 `PSRAM_CAPCAITY_SIZE=16M`，已生成统一七区域布局 |
| `app_ab` 干净构建 | 已验证 | 顶层、CP核内、AP核内临时CSV、生成头和链接脚本均为同一16 MB布局 |
| 最终包注入 | 已验证 | 当前`nuttx.bin`、`build/openvela-ap.bin`和`package/tmp/app1.bin` SHA256一致，`all-app.bin`已重建 |
| CP PHY 初始化 | 已存在 | CP 驱动负责电压、时钟、ID 探测和控制器配置 |
| AP PHY 初始化 | 明确禁止 | AVDK AP 的初始化 API 返回 `BK_FAIL` |
| OpenVela MPU | 已构建 | `bk7258_start.c` 已覆盖 `0x60000000..0x60ffffff`，RW/XN/non-cacheable |
| OpenVela AP侧PSRAM | allocator基线已实现 | 手写16 MB布局、MPU、2.875 MiB独立AP heap、四个隔离媒体pool、保存/恢复型AP owner probe和统计API已接入；尚未自动消费`app_ab`生成布局，CP ID/容量query和实板验证仍未完成 |
| 跨工程布局单一来源 | 未实现 | 当前靠权威CSV、干净构建和人工逐层核对保证一致，仍需第10章转换脚本/构建门禁 |
| PWC PSRAM语义 | 部分实现 | 已按当前SMP原厂协议完成`0x7`常驻vote、`0x8`动态heap计数/掉电通知、`0x9`统计和`0xa`freeze/busy响应；现有CP协议仍不提供ID/容量query，完整lease/DMA recovery尚未完成 |
| cache | 保守关闭/未用于 PSRAM | 适合作为首版 non-cacheable 基线 |
| 低功耗掉电 | 不可安全使用 | 已有allocator freeze和动态分配outstanding基线，但媒体/DMA lease、掉电前veto及完整recovery闭环尚未完成 |

## 2. 资料与源码依据

### 2.1 OpenVela AP 当前实现

当前适配源码位于：

```text
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
├── chips/bk7258/
└── boards/bk7258/bk7258-ap/
```

关键文件：

- AP SRAM/静态PSRAM链接布局：`boards/bk7258/bk7258-ap/scripts/ld.script`
- SRAM heap：`chips/bk7258/bk7258_allocateheap.c`
- MPU region：`chips/bk7258/bk7258_start.c`
- PWC worker：`chips/bk7258/bk7258_pm_pwc.c`
- PSRAM allocator/API：`chips/bk7258/bk7258_psram.c`、
  `chips/bk7258/include/bk7258_psram.h`
- 当前配置：`boards/bk7258/bk7258-ap/configs/nsh/defconfig`
- 现有总体方案：`BK7258_OPENVELA_AP_PORTING_PLAN.md:295-313`

当前链接产物证明NuttX通用动态heap仍只使用AP私有SRAM；PSRAM由运行时创建的五个
独立`mm_heap_s`管理，不进入通用heap。静态`AP_PSRAM_SECTION`已在链接脚本保留，
但reset阶段copy/clear时序完成前由链接断言强制保持0 B：

```text
_sdata       = 0x28010000
_edata       = 0x28010374
_ebss        = 0x28013b1d
__heap_start = 0x28014b28
__heap_end   = 0x28064000
__ap_psram_section_start = 0x60a00000
__ap_psram_section_end   = 0x61000000
```

以上符号是当前构建快照，不是固定ABI；静态对象变化时`_edata/_ebss`可以变化，
但`__heap_end`必须始终等于`AP_RAM`半开终点`0x28064000`。当前
`up_allocate_heap()`只返回这一段AP私有SRAM，`arm_addregion()`为空；PSRAM只能
通过`bk7258_psram_*`或`bk7258_media_pool_*`接口访问。当前最小NSH尚无业务调用
分配包装API，未被引用的包装函数会被`--gc-sections`裁掉；初始化、PWC、统计和
shutdown路径已保留在最终ELF，后续业务首次引用包装API时会自动链接进入。

当前`hardware/bk7258_psram.h`中的七区域地址是经过核对的手写常量，不是从
`app_ab/ram_regions.csv`自动生成。它的编译期断言能发现OpenVela本地常量不连续，
但不能发现SMP工作树仍使用另一份旧CSV；跨工程一致性仍依赖第10章的生成链和门禁。

### 2.2 `bk_avdk_smp` 布局生成

`app_ab` 当前内存表：

- `bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv`
- 生成结果：
  `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/partitions/ram_regions.h`

生成器行为：

- 解析容量：
  `bk_avdk_smp/tools/env_tools/bk_py_libs/bk_ram_region/bk_ram_region.py:104-116`
- 连续区域地址累计：同文件 `118-132`
- 范围与重叠校验：同文件 `90-102`
- 生成 `CONFIG_*_ADDR/SIZE`：同文件 `141-167`

工具当前使用原厂拼写 `PSRAM_CAPCAITY_SIZE`。在不同时升级生成器和所有项目
CSV 的情况下，本项目必须继续使用该拼写，避免容量覆盖失效。

2026-07-31已对`app_ab`执行项目级clean build，并核对以下六层结果一致：

```text
受版本控制的 ram_regions.csv
顶层 generated ram_regions.h
CP armino/partitions/_build/ram_regions.csv
AP armino/partitions/_build/ram_regions.csv
CP 预处理链接脚本
AP 预处理链接脚本
```

构建摘要中的AP项来自打包流程仍会构建的Armino AP占位镜像，只用于验证其生成
region边界；最终包中的AP由`EXTERNAL_AP_BIN`替换，OpenVela静态SRAM/PSRAM占用
必须以`nuttx.map`或`System.map`为准。摘要里PSRAM region显示`0 B used`也仅表示
没有链接期section占用，不能证明运行时heap/slab为空、初始化成功或容量可访问。

### 2.3 CP PSRAM 实现

CP 初始化实现：

- `bk_avdk_smp/cp/middleware/driver/psram/psram_driver.c:249-369`
- `bk_avdk_smp/cp/middleware/soc/common/hal/psram_hal.c`
- 启动调用：
  `bk_avdk_smp/cp/middleware/driver/common/driver.c:553-561`

CP 初始化顺序为：

```text
设置 1.95 V
  -> 打开 LDO、电源和初始时钟
  -> 初始化控制器
  -> 读取/探测器件 ID
  -> 等待器件稳定
  -> 切换默认运行时钟
  -> 标记初始化完成
```

驱动支持的容量包括：

| ID | 容量 |
| --- | ---: |
| `PSRAM_W955D8MKY_5J_ID` | 4 MB |
| `PSRAM_APS6408L_ID` | 8 MB |
| `PSRAM_APS128XXO_OB9_ID` | 16 MB |

当前缺陷是 `psram_driver.c:328-340` 发现器件容量与
`CONFIG_PSRAM_CAPACITY`不一致时只输出 warning。正式 OpenVela 16 MB 构建必须
增加可查询的初始化结果和容量，并由 AP 在建立 allocator 前进行严格确认。

### 2.4 AP 参考实现边界

AVDK AP 侧：

- `bk_psram_id_auto_detect()` 返回 `BK_FAIL`。
- `bk_psram_init()` 返回 `BK_FAIL`。
- `bk_psram_deinit()` 返回 `BK_FAIL`。

见：

`bk_avdk_smp/ap/middleware/driver/psram/psram_driver.c:175-190`。

这证明 AP 不能复制 CP 的 PHY 初始化。可复用的是 AP 拥有独立allocator、通过
PWC请求电源、维护使用计数，以及原厂媒体slab的区域和best-fit算法；不可直接
复用的是 FreeRTOS `heap_4.c`、Beken RTOS mutex/critical section及依赖这些OS
接口的原媒体slab源码。

### 2.5 16 MB 工程参考

当前仓库已有 16 MB 布局例子：

- `projects/lvgl/img_decode/partitions/bk7258/ram_regions.csv`
- `projects/lvgl/freetype_font/partitions/bk7258/ram_regions.csv`
- `projects/wifi/p2p/partitions/bk7258/ram_regions.csv`

这些例子证明生成器和 CP ID 检测支持 16 MB，但其内存用途属于各自项目，不能
只复制容量数字而忽略各区域语义。本方案选择与当前 `app_ab` SRAM布局相同的
`lvgl/img_decode`/`lvgl/freetype_font`七区域PSRAM表作为正式参考；不采用
`wifi/p2p`不同的SRAM和PSRAM比例。

原厂四个媒体slab由以下实现管理：

- 固定映射结构：
  `bk_avdk_smp/ap/components/media_utils/src/frame_buffer_mapping.h:24`
- 四个独立best-fit free-list：
  `bk_avdk_smp/ap/components/media_utils/src/psram_mem_slab.c:77`
- PSRAM上电后重建slab：
  `bk_avdk_smp/ap/components/media_service/src/media_service.c:115`

OpenVela应保持四个slab、best-fit分配、块合并、统计和上电重建语义，但使用NuttX
mutex/spinlock和错误码重写OS适配层，不直接编译依赖FreeRTOS/Beken RTOS API的
原文件。

### 2.6 `vendor_beken` 参考边界

BK7236N 参考配置包含：

```text
CONFIG_PSRAM=y
CONFIG_PSRAM_AS_SYS_MEMORY=y
CONFIG_PSRAM_HEAP_BASE=0x60000000
```

见：

`vendor_beken/chips/bk7236n/bk_idk/armino_as_lib/bk7236n/config/sdkconfig.h:238-245`。

但 `vendor_beken/chips/bk7236n/beken_allocateheap.c:117-121` 的
`arm_addregion()`仍是空实现。该移植通过预编译 Armino 库使用 PSRAM，不是一个
已完成的 NuttX 外部 heap 参考。可参考其 chip/board 构建组织，不得直接复制其
PSRAM 地址、初始化库或把 `CONFIG_PSRAM_AS_SYS_MEMORY`当作 NuttX 已接入的证据。

### 2.7 NuttX 独立 heap 依据

NuttX 提供独立 heap API：

- `mm_initialize()`：`contest/nuttx/include/nuttx/mm/mm.h:274-291`
- `mm_malloc()`：同文件 `328-330`
- `mm_free()`：同文件 `348-351`
- `mm_uninitialize()`：同文件 `304-306`
- 代表实现：
  `contest/nuttx/arch/risc-v/src/esp32c3-legacy/esp32c3_rtcheap.c:50-78`

BK7258 应采用同类独立 heap，而不是将 PSRAM 并入 `g_mmheap`。这样可以显式区分
SRAM 与 PSRAM 指针，提供独立统计，并为 power-off/recovery 保留冻结和重建边界。

## 3. 目标内存所有权

### 3.1 Share SRAM 保持不变

BK7258 的 640 KiB 片内 Share SRAM 是 `0x28000000..0x280a0000`，由下列五个
区域完全划分；它不是 OpenVela 可自由分配的 640 KiB。基础 `app_ab` SRAM继续使用：

| 区域 | 半开地址范围 | 大小 | owner |
| --- | ---: | ---: | --- |
| `AP_SPINLOCK` | `0x28000000..0x28010000` | `0x10000`，64 KiB | AP/CP 同步保留 |
| `AP_RAM` | `0x28010000..0x28064000` | `0x54000`，336 KiB | OpenVela AP 私有 SRAM |
| `CP_RAM` | `0x28064000..0x2809f700` | `0x3b700`，237.75 KiB | Armino CP 私有 SRAM |
| `PWR_MNG` | `0x2809f700..0x2809f800` | `0x100`，256 B | 固定 AP/CP ABI |
| `SWAP` | `0x2809f800..0x280a0000` | `0x800`，2 KiB | 系统保留 |

`AP_SPINLOCK` 只用于同步与协议保留，不属于可供 AP 动态分配的普通 SRAM。
这里的“私有”表示布局所有权，不表示其他总线master在硬件上必然无法访问；MPU、
PPC/MPC和软件指针校验仍需分别限制访问。

PSRAM 改造不得改变 AP Flash 起点、AP 私有 SRAM、Mailbox buffer 或 PWR_MNG 地址。
这样可以把 PSRAM 引入与当前已成功的启动和日志基线隔离。

### 3.2 PSRAM 正式布局

目标布局直接采用原厂 `lvgl/img_decode`和`lvgl/freetype_font`的16 MB分配，不
自定义单owner区域。完整 `app_ab/ram_regions.csv`应为：

```csv
# SRAM base addr is 0x28000000, capacity is 640K
# PSRAM base addr is 0x60000000
PSRAM_CAPCAITY_SIZE=16M
#Name,                   Type,     Offset,     Size
AP_SPINLOCK,             SRAM, 0x28000000, 0x010000
AP_RAM,                  SRAM,           , 0x054000
CP_RAM,                  SRAM,           , 0x03b700
PWR_MNG,                 SRAM,           , 0x000100
SWAP,                    SRAM,           , 0x000800
PSRAM_MEM_SLAB_USER,    PSRAM, 0x60000000, 0x019000
PSRAM_MEM_SLAB_AUDIO,   PSRAM,           , 0x019000
PSRAM_MEM_SLAB_ENCODE,  PSRAM,           , 0x15E000
PSRAM_MEM_SLAB_DISPLAY, PSRAM,           , 0x570000
CP_PSRAM_HEAP,          PSRAM,           , 0x020000
AP_PSRAM_HEAP,          PSRAM,           , 0x2e0000
AP_PSRAM_SECTION,       PSRAM,           , 0x600000
```

生成后的准确边界：

| 区域 | 半开地址范围 | 大小 | owner/用途 |
| --- | ---: | ---: | --- |
| `PSRAM_MEM_SLAB_USER` | `0x60000000..0x60019000` | 100 KiB | Vela通用媒体pool |
| `PSRAM_MEM_SLAB_AUDIO` | `0x60019000..0x60032000` | 100 KiB | Vela音频pool |
| `PSRAM_MEM_SLAB_ENCODE` | `0x60032000..0x60190000` | 1400 KiB | Vela编码pool |
| `PSRAM_MEM_SLAB_DISPLAY` | `0x60190000..0x60700000` | 5568 KiB | Vela显示/视频帧pool |
| `CP_PSRAM_HEAP` | `0x60700000..0x60720000` | 128 KiB | Armino CP system heap |
| `AP_PSRAM_HEAP` | `0x60720000..0x60a00000` | 2944 KiB | Vela/AP动态heap |
| `AP_PSRAM_SECTION` | `0x60a00000..0x61000000` | 6144 KiB | Vela/AP静态PSRAM section |

容量汇总：

```text
媒体 slab总量 = 0x00700000 = 7 MiB
AP动态heap     = 0x002e0000 = 2.875 MiB
AP静态section  = 0x00600000 = 6 MiB
Vela/AP总量    = 0x00fe0000 = 15.875 MiB
CP heap        = 0x00020000 = 128 KiB
物理总量       = 0x01000000 = 16 MiB
```

生成头应至少得到原厂宏：

```c
#define CONFIG_PSRAM_BASE                    0x60000000
#define CONFIG_PSRAM_CAPACITY                0x01000000
#define CONFIG_PSRAM_MEM_SLAB_USER_ADDR      0x60000000
#define CONFIG_PSRAM_MEM_SLAB_USER_SIZE      0x00019000
#define CONFIG_PSRAM_MEM_SLAB_AUDIO_ADDR     0x60019000
#define CONFIG_PSRAM_MEM_SLAB_AUDIO_SIZE     0x00019000
#define CONFIG_PSRAM_MEM_SLAB_ENCODE_ADDR    0x60032000
#define CONFIG_PSRAM_MEM_SLAB_ENCODE_SIZE    0x0015e000
#define CONFIG_PSRAM_MEM_SLAB_DISPLAY_ADDR   0x60190000
#define CONFIG_PSRAM_MEM_SLAB_DISPLAY_SIZE   0x00570000
#define CONFIG_CP_PSRAM_HEAP_ADDR             0x60700000
#define CONFIG_CP_PSRAM_HEAP_SIZE             0x00020000
#define CONFIG_AP_PSRAM_HEAP_ADDR             0x60720000
#define CONFIG_AP_PSRAM_HEAP_SIZE             0x002e0000
#define CONFIG_AP_PSRAM_SECTION_ADDR          0x60a00000
#define CONFIG_AP_PSRAM_SECTION_SIZE          0x00600000
```

必须增加构建断言：

```text
第一个媒体slab从CONFIG_PSRAM_BASE开始
每个区域起点等于前一区域终点
CP_PSRAM_HEAP严格为0x60700000..0x60720000
AP_PSRAM_HEAP严格为0x60720000..0x60a00000
AP_PSRAM_SECTION终点等于0x61000000
所有区域总量等于CONFIG_PSRAM_CAPACITY
```

### 3.3 CP 所有权保持原厂实现

CP继续使用：

```text
CONFIG_PSRAM=y
CONFIG_PSRAM_AS_SYS_MEMORY=y
CONFIG_CP_PSRAM_HEAP_ADDR=0x60700000
CONFIG_CP_PSRAM_HEAP_SIZE=0x00020000
```

无需为Vela修改 CP linker和 FreeRTOS `heap_4.c`的基本布局。必须保证：

1. CP只能从自己的128 KiB heap分配。
2. CP不得把AP heap、AP section或媒体slab加入自己的allocator。
3. CP异常dump若读取AP区域必须只读，并在AP停止后执行。
4. CP的PSRAM使用计数和低功耗投票继续有效。
5. CP map中 `PSRAM_HEAP`只能声明为 `0x60700000/0x20000`；map中的`0 B used`
   只表示没有链接期section落入该region，不代表运行时heap未启用或未分配。

### 3.4 OpenVela 按原厂区域建立 allocator

不能跨过CP heap建立一个覆盖多个非连续区域的单一heap。OpenVela应分别管理：

1. 四个媒体slab：保持原厂地址、用途和独立best-fit free-list语义。
2. `AP_PSRAM_HEAP`：使用独立 `mm_heap_s`作为AP通用动态PSRAM heap。
3. `AP_PSRAM_SECTION`：保留给链接期静态对象，不进入动态allocator。

首版逻辑策略：

| 用途 | 首版策略 |
| --- | --- |
| 普通AP大对象 | 从 `AP_PSRAM_HEAP`分配 |
| 通用媒体控制buffer | 从 USER slab分配 |
| 音频 ring/workspace | 从 AUDIO slab分配 |
| 编码输入输出/workspace | 从 ENCODE slab分配 |
| 帧缓冲和显示buffer | 从 DISPLAY slab分配 |
| DMA buffer | 从对应用途pool对齐分配，记录DMA owner和方向 |
| 经审计静态大对象 | 链接到 `AP_PSRAM_SECTION` |
| 内核对象和任务栈 | 默认留在 SRAM |

不得为了提高某一阶段可用量而动态吞并相邻区域。若后续确需调整比例，必须修改
项目 `ram_regions.csv`、重新生成AP/CP统一布局并执行完整回归，不能在运行时改
边界。

## 4. 启动与初始化时序

### 4.1 CP 阶段

目标启动时序：

```text
Bootloader
  -> Armino CP
     -> bk_init()
        -> driver_init()
           -> bk_psram_init()
              -> 电压/LDO/时钟
              -> 控制器初始化
              -> 16 MB 器件 ID 检测
              -> 记录 actual_id 和 actual_capacity
     -> 启动 OpenVela AP
```

CP 在启动 AP 前必须具备可查询状态：

```c
struct bk_psram_status
{
  uint32_t initialized;
  uint32_t device_id;
  uint32_t capacity;
  uint32_t clock_hz;
  uint32_t generation;
  int32_t  last_error;
};
```

结构不应由 AP 直接读取 CP 私有全局变量。优先通过正式 PWC query/response返回；
如使用 `PWR_MNG` 固定 ABI，必须分配版本号、长度、校验和和明确 owner，并与 CP
同步修改，不能覆盖现有固定字。

### 4.2 OpenVela reset 阶段

OpenVela reset handler只做：

- 建立 SAU/MPU、FPU、C runtime 和 runtime vectors。
- 配置 PSRAM MPU region。首版无论 `AP_PSRAM_SECTION` 是否预留，都不在 reset 阶段建立 allocator、搬移对象或触碰未经确认就绪的静态 PSRAM 数据。
- 不创建 allocator。
- 不清零整颗PSRAM、媒体pool、AP heap或CP heap。
- 不等待 PWC semaphore。

后续启用 `.psram.data/.psram.bss`时，reset阶段只允许在CP已保证PSRAM上电后复制
或清零 `0x60a00000..0x60ffffff`内实际section范围；这属于第9章的独立门禁，
不改变其他区域。

不能在 `bk7258_start()` 中调用 `mm_initialize()`，因为此时 NuttX scheduler、锁和
完整内存基础设施尚未启动。

### 4.3 Board late 阶段

在当前 Mailbox、heartbeat 和 PWC worker完成后执行：

```text
board_late_initialize()
  -> Mailbox/PWC transport ready
  -> 查询 CP PSRAM status
  -> 校验 initialized == 1
  -> 校验 capacity == 0x01000000
  -> 获取 AP PSRAM power lease
  -> 执行 non-destructive 边界/alias probe
  -> 首次开发构建只测试Vela/AP拥有的六个区域
  -> 初始化四个媒体pool和AP动态heap
  -> 注册 /proc 或 NSH 诊断入口
  -> 标记 PSRAM ONLINE
```

PSRAM 初始化失败不应破坏当前 AP 启动和 Mailbox heartbeat。开发阶段允许 AP
进入 `DEGRADED_SRAM_ONLY` 并输出明确错误，但音视频服务必须拒绝启动。正式媒体
构建可配置为 PSRAM 失败即阻止 AP ready。

### 4.4 Ready 语义

定义两种配置：

1. `CONFIG_BK7258_PSRAM_REQUIRED=y`：只有 PSRAM ONLINE 后才认为board bring-up
   完成；CPU1 boot-ready只表示CPU1内核、Mailbox和PWC worker已可服务，先于
   PSRAM vote发送，不等于媒体ready。
2. 调试内核配置：AP 基础 ready不依赖 PSRAM，但单独发送/打印
   `PSRAM unavailable`，媒体服务不得启动。

当前实现已经按以下顺序执行：

```text
Mailbox/PWC worker ready
  -> HW_CTRL command 1已完成原厂power-up存活握手
  -> 发送CPU1 boot-ready（释放CP的OpenVela boot transaction等待）
  -> 发送PSRAM 0x7 power vote
  -> 等待CP语义响应
  -> AP owner alias probe
  -> 初始化AP heap和四个媒体pool
  -> PSRAM ONLINE
```

该顺序匹配原厂AP `bk_pm_cp1_boot_ok_response_set()` 先发送ready、再按需vote
PSRAM的调用路径。CP的CPU1启动函数同步等待boot-ready，若AP先等PSRAM响应再发
ready，会与CP形成死锁；因此`PSRAM_REQUIRED`只门禁board/media bring-up，不再
推迟CPU1 boot-ready的发送。

后续音视频正式配置必须选择`PSRAM_REQUIRED`，并为媒体业务增加独立ONLINE查询或
media-ready协议。CP收到CPU1 boot-ready后只能认定核心存活，不能立即启动媒体业务。

## 5. PWC 协议与状态机

### 5.1 不能只处理 transport ACK

当前`bk7258_pm_pwc.c`已在worker中处理PSRAM power、allocator query、诊断和
recovery命令，并区分：

- Mailbox transport ACK：只表示消息已接收。
- PWC semantic response：表示电源或 allocator操作成功、失败或 busy。

OpenVela Mailbox层必须对CP发来的所有有效`CPU0->CPU1`逻辑通道消息回送transport
ACK，即使当前没有注册该通道的业务handler。原厂`mailbox_channel.c`在`rx_isr`为
NULL时会设置`CHNL_STATE_COM_FAIL`并继续回ACK；若OpenVela对未支持的通道（如
`0x4c`）直接返回不回ACK，CP物理发送通道会永久停留在BUSY，后续更高优先级的PWC
语义响应（包括`0x7`）无法发出。实板日志曾出现：

```text
mailbox: unsupported=2 last unsupported logical channel=0x4c
PWC: PSRAM power response failed, error=-110 state=1
```

修复方式与原厂一致：对有效envelope但未注册handler的通道，设置`COM_FAIL`状态并
回送transport ACK；只对PWC RX和UART0 RX执行业务处理。

现有命令号必须保持兼容：

| command | 当前方向和字段 | OpenVela处理 |
| ---: | --- | --- |
| `0x7 PM_CTRL_PSRAM_POWER_CMD` | AP -> CP请求：`param1=module`、`param2=power_state`；当前CP响应：`param1=power_state` | 保留现有字段，不塞入容量信息；等待CP响应，并在后续扩展中增加明确错误字段而不是只回显状态 |
| `0x8 PM_CP1_PSRAM_MALLOC_STATE_CMD` | CP -> AP查询：`param1=using_psram_type`；AP -> CP响应：`param1=0x8`、`param2=used_count` | 首先兼容`using_psram_type==0`的原厂动态heap计数；媒体pool另由模块vote保护，扩展其他type前同步修改两侧 |
| `0x9 PM_CP1_DUMP_PSRAM_MALLOC_INFO_CMD` | CP -> AP | 只投递 worker生成统计，不在 ISR执行 dump |
| `0xa PM_CP1_RECOVERY_CMD` | CP -> AP | 冻结 allocator、停止 DMA并返回 busy/success |
| `0xc PM_GET_PM_DATA_CMD` | AP -> CP查询一项PM数据；当前CP只支持启动间隔、deep-sleep wake source和low-voltage wake source | 当前不得发送未定义PSRAM selector；后续需同步扩展AP/CP，分别查询initialized、device ID、capacity、clock和generation |

一条PWC logical message只有command和三个`uint32_t`参数，不能假设一次响应可以
携带完整`bk_psram_status`。当前CP没有PSRAM status selector，OpenVela也没有虚构
扩展字段。正式实现可在`PM_GET_PM_DATA_CMD`下同步增加多个PSRAM data type并逐项
查询；如果新增专用status command，必须同步修改AP/CP header、版本和负向兼容
测试，不能只修改OpenVela一侧。

### 5.2 状态定义

OpenVela 维护：

```text
OFF
  -> POWER_REQUESTED
  -> PROBING
  -> ONLINE
  -> FREEZING
  -> OFFLINE
  -> RECOVERING
  -> FAILED
```

状态、指向heap的全局指针、outstanding count、generation和last error放在AP私有
片内SRAM，不能放在PSRAM中。默认`mm_initialize()`建立的`struct mm_heap_s`控制块
本身位于`AP_PSRAM_HEAP`头部；PSRAM掉电或offline后只能检查/清空片内SRAM中的
指针，不能再解引用该控制块。

必须分别记录AP动态heap和四个媒体pool的outstanding。命令`0x8`在
`using_psram_type==0`时保持原厂动态heap计数语义，并按原厂格式用响应`param1=0x8`、
`param2=used_count`返回；不能擅自改成五个allocator的合计值，否则CP现有低功耗
判断含义改变。
媒体pool由对应audio/encode/display等模块vote保护；未映射到现有模块的USER pool
增加Vela专用lease。`AP_PSRAM_SECTION`只要存在运行期有效静态对象，就在整个AP
生命周期持有常驻lease，不允许PSRAM动态掉电。

### 5.3 Power-on

```text
AP -> CP: PM_CTRL_PSRAM_POWER_CMD(ON)
CP:
  -> 增加 OpenVela module vote
  -> 若未初始化则执行 CP owner 初始化
  -> 用命令0x7返回 power semantic status
AP:
  -> [当前协议缺口] 查询 initialized、device ID、actual capacity和generation
  -> [当前临时门禁] 对AP owner区域和4/8/12 MiB点执行保存/恢复型alias probe
  -> memory barriers
  -> 初始化 heap
  -> ONLINE
```

不得依赖未定义的异步广播，也不得在请求发送后立即访问PSRAM。当前`0x7`响应只
回显ON/OFF状态，CP丢弃了`bk_psram_init()`返回值，因此收到ON不能作为PHY初始化
成功或实际容量为16 MiB的充分证据；临时alias probe只能降低误配风险，正式验收
仍需要CP ID/容量query和阶段1整颗器件测试。

### 5.4 Power-off

```text
CP/AP 请求关闭
  -> AP 状态切到 FREEZING
  -> 拒绝新分配
  -> 等待在途 DMA 和媒体 owner 释放
  -> 检查AP heap及四个媒体pool outstanding均为0
  -> 检查CP heap count为0
  -> 检查全部active lease为0
  -> 分别销毁媒体pool和AP mm_heap_s
  -> 清各allocator handle
  -> AP semantic response SUCCESS
  -> CP 关闭控制器/时钟/电源
```

任一引用存在时返回 `BUSY`，禁止强制断电。`AP_PSRAM_SECTION`存在有效静态对象时
直接视为常驻busy。首个音视频调试版本建议持续持有一个OpenVela system lease，
使CP不执行自动掉电。

### 5.5 Recovery

CP 发出 AP recovery 或 PSRAM掉电通知时：

1. 停止音视频 producer。
2. 停止 DMA，等待硬件 idle。
3. 冻结 allocator。
4. 将全部现存 PSRAM 指针视为失效。
5. 增加 generation。
6. allocator outstanding 非零时记录泄漏并拒绝无损恢复。
7. CP 重新上电后重新 probe 和 `mm_initialize()`。
8. 上层服务必须重建 buffer，不得复用旧地址。

generation 应随诊断 API 暴露。调试版本可以在 allocation header 或 owner表记录
generation，帮助发现跨 recovery 的 stale pointer。

## 6. OpenVela 驱动与 API

### 6.1 文件结构

在现有 BK7258 chip port 最小新增：

```text
chips/bk7258/
├── bk7258_psram.c
├── bk7258_psram.h
└── hardware/
    └── bk7258_psram.h
```

修改：

```text
chips/bk7258/Kconfig
chips/bk7258/CMakeLists.txt
chips/bk7258/Make.defs
chips/bk7258/bk7258_start.c
chips/bk7258/bk7258_pm_pwc.c
chips/bk7258/hardware/bk7258_memorymap.h
boards/bk7258/bk7258-ap/src/bk7258_bringup.c
boards/bk7258/bk7258-ap/configs/nsh/defconfig
```

若生成布局头由 board 层提供，chip 层通过一个最小稳定接口获取
`BOARD_PSRAM_START/SIZE`，不要直接包含整个 Armino `sdkconfig.h`。

### 6.2 Kconfig

建议增加：

```kconfig
config BK7258_PSRAM
    bool "BK7258 CP-managed PSRAM"
    default n

config BK7258_PSRAM_REQUIRED
    bool "Require PSRAM before AP ready"
    depends on BK7258_PSRAM

config BK7258_PSRAM_SIZE
    hex "PSRAM size"
    default 0x01000000
    depends on BK7258_PSRAM

config BK7258_PSRAM_MEMORY_TEST
    bool "Run destructive PSRAM test before heap init"
    depends on BK7258_PSRAM

config BK7258_PSRAM_KEEP_POWERED
    bool "Keep PSRAM powered while AP runs"
    default y
    depends on BK7258_PSRAM
```

最终配置：

```text
CONFIG_BK7258_PSRAM=y
CONFIG_BK7258_PSRAM_REQUIRED=y
CONFIG_BK7258_PSRAM_SIZE=0x01000000
CONFIG_BK7258_PSRAM_KEEP_POWERED=y
CONFIG_MM_REGIONS=1
```

保持 `CONFIG_MM_REGIONS=1` 是有意设计：PSRAM 不进入通用 heap。

### 6.3 Public API

建议提供：

```c
int bk7258_psram_initialize(void);
int bk7258_psram_shutdown(void);
bool bk7258_psram_is_online(void);

void *bk7258_psram_malloc(size_t size);
void *bk7258_psram_zalloc(size_t size);
void *bk7258_psram_calloc(size_t n, size_t elem_size);
void *bk7258_psram_realloc(void *ptr, size_t size);
void *bk7258_psram_memalign(size_t alignment, size_t size);
void bk7258_psram_free(void *ptr);

void *bk7258_media_pool_alloc(enum bk7258_psram_pool pool,
                              size_t alignment, size_t size);
void bk7258_media_pool_free(enum bk7258_psram_pool pool, void *ptr);

int bk7258_psram_info(struct bk7258_psram_info *info);
bool bk7258_psram_contains(const void *ptr, size_t size);
```

接口约束：

- `size == 0`行为与 NuttX `mm_*`保持一致并写入接口文档。
- `calloc`检查乘法溢出。
- `contains`检查指针加法溢出和半开区间上限。
- allocator 不在线或冻结时分配返回 `NULL`并设置明确 errno。
- 通用 `free`只接受 `AP_PSRAM_HEAP`指针；媒体pool指针必须交给对应pool释放。
- 任一API都检查指针所属原厂区域，禁止跨过CP heap释放或合并块。
- `memalign`支持至少 32 字节 cache line、DMA 和 framebuffer要求。
- 所有 API 在 SMP 下必须依赖 NuttX `mm_heap_s`内部锁和额外状态锁，不能只用
  `up_irq_save()`保护全局状态。

### 6.4 allocator 实现

核心流程：

```c
g_psram_heap = mm_initialize("bk7258-ap-psram",
                             (void *)CONFIG_AP_PSRAM_HEAP_ADDR,
                             CONFIG_AP_PSRAM_HEAP_SIZE);
```

四个媒体slab不加入 `g_psram_heap`。按原厂 `psram_mem_slab.c`移植四个独立
best-fit free-list，保留块拆分、相邻空闲块合并、使用量统计和
`USER/AUDIO/ENCODE/DISPLAY`类型检查；锁和初始化生命周期改用NuttX机制。禁止
把四段交给一个可跨区域分配的heap。

包装 API 使用：

```text
mm_malloc
mm_zalloc
mm_calloc
mm_realloc
mm_memalign
mm_free
mm_mallinfo
mm_malloc_size
mm_uninitialize
```

`struct mm_heap_s`控制块由`mm_initialize()`放在`AP_PSRAM_HEAP`头部，因此allocator
offline后不能再解引用它。指向它的全局指针和状态保存在AP私有SRAM；shutdown时
必须在PSRAM仍上电且allocator已冻结时完成统计和泄漏检查，再调用
`mm_uninitialize()`，最后清空片内SRAM中的指针。

### 6.5 统计与诊断

`bk7258_psram_info` 至少包含：

```c
struct bk7258_psram_info
{
  uintptr_t base;
  size_t capacity;
  size_t arena;
  size_t allocated;
  size_t free;
  size_t largest_free;
  size_t peak_allocated;
  uint32_t allocation_count;
  uint32_t failed_allocations;
  uint32_t generation;
  uint32_t state;
  uint32_t device_id;
  int last_error;
};
```

提供 NSH 或 procfs 节点，例如：

```text
psram info
psram test quick
psram test full
psram stress <seconds>
```

不得在生产运行期间执行破坏性AP区域测试；Vela任何时候都不得测试CP heap。

## 7. MPU、SAU 与执行权限

### 7.1 MPU region

在 `bk7258_start.c` 增加：

```c
{
  0x60000000u,
  0x01000000u,
  MPU_RBAR_XN | MPU_RBAR_AP_RWRW | MPU_RBAR_SH_INNER,
  MPU_RLAR_NONCACHEABLE
}
```

要求：

- 精确覆盖 16 MB，不覆盖控制器的 64 MB映射窗口。
- RW、XN，禁止默认从 PSRAM执行代码。
- 首版 non-cacheable。
- CPU1+CPU2 SMP 时两个核配置一致。
- 所有正式 API和测试工具必须拒绝 `0x61000000`及之后地址。

当前 `mpu_initialize(..., false, true)`启用了 privileged default map，因此“没有
显式 PSRAM region”不等于 privileged代码越界必然触发 MemManageFault。若验收
要求硬件 fault隔离，必须关闭 `PRIVDEFENA`并补齐所有必要 region，或增加一个
更高优先级的 `0x61000000..0x63ffffff` no-access region；具体 no-access编码和
region优先级需按当前 ARMv8-M MPU公共实现验证。首版至少由 API边界、DMA长度和
测试命令三层阻止越界，不能把未验证的 fault写成既定行为。

当前配置声明 `CONFIG_ARM_MPU_NREGIONS=8`，启用PSRAM后的现有代码使用4个region：
Flash、AP私有SRAM、PSRAM和外设窗口，仍在配置数量范围内。但必须从MPU TYPE
寄存器实板确认硬件region数量；SAU region不计入MPU region数量。

### 7.2 SAU 风险门禁

当前 `bk7258_start.c:77-83` 的 SAU写值与注释存在疑点：region 0的 RLAR bit 1
会设置 NSC，region 1 又覆盖 AP RAM和 PSRAM地址范围。PSRAM接入前必须：

1. 导出 golden AVDK AP 的 SAU RNR/RBAR/RLAR/CTRL。
2. 导出 OpenVela 同一寄存器。
3. 结合 BK7258 IDAU/PPC/MPC定义确认 Secure alias实际属性。
4. 确认 `0x60000000..0x60ffffff` 对 CPU1和媒体 DMA master可访问。
5. 对错误安全属性执行 SecureFault负向测试。

未完成前只称“功能性 PSRAM访问”，不能声称 TrustZone隔离正确。

### 7.3 不执行代码

首版禁止：

- `.psram.text`、`.ramfunc`指向 PSRAM。
- 从 PSRAM加载插件或 codec代码。
- 将函数指针跳转到 PSRAM。
- 将整个 PSRAM MPU region改为可执行。

若后续确需 PSRAM code，必须建立独立小型 RO/RX region、签名/完整性校验、copy
table、cache维护和链接断言，不改变数据 heap的 XN属性。

## 8. Cache、DMA 与 SMP 一致性

### 8.1 首版 non-cacheable

AVDK AP 的 `mpu_cfg.c:121-124` 将 PSRAM设为 Normal non-cacheable、XN。OpenVela
首版沿用该属性，原因是：

- 音视频 DMA 会直接读写 PSRAM。
- AP/CP和未来 CPU1/CPU2可能共享数据。
- 当前没有完整 cache clean/invalidate ownership协议。
- Mailbox和媒体驱动尚未统一 cache line对齐规则。

性能可能低于 cacheable模式，但正确性优先。

### 8.2 DMA API

媒体驱动不能把普通 `bk7258_psram_malloc()` 返回值无条件交给 DMA。建议提供：

```c
void *bk7258_psram_dma_alloc(size_t size, size_t alignment,
                             enum dma_data_direction dir);
void bk7258_psram_dma_free(void *ptr);
```

首版 non-cacheable时 sync操作为空 barrier，但 API仍保留：

```c
bk7258_psram_sync_for_device(ptr, size, dir);
bk7258_psram_sync_for_cpu(ptr, size, dir);
```

这样后续启用 cache时不需要修改全部音视频调用者。

必须验证每个硬件 master：

- 是否能访问 Secure PSRAM alias `0x60000000`。
- 地址是否需要其他 bus alias。
- 最大传输长度和边界限制。
- 对齐、burst和二维 stride要求。
- DMA completion后 CPU可见性。

### 8.3 Cacheable 后续阶段

只有满足以下条件后才评估 cacheable PSRAM：

- CPU1/CPU2共享属性确认。
- D-cache line size实测并固定。
- allocator至少按 cache line对齐关键对象。
- DMA map/unmap和双向 sync接口全部接入。
- AP/CP共享对象有明确 owner handoff。
- 24小时音视频压力无 stale data、tearing或随机 codec错误。

不能只把 MPU attribute从 non-cacheable改为 write-back而不修改驱动。

## 9. 链接与静态 section

### 9.1 AP PSRAM section

OpenVela链接脚本必须保留原厂 `AP_PSRAM_SECTION`边界：

```ld
PSRAM_SECTION (rw) : ORIGIN = CONFIG_AP_PSRAM_SECTION_ADDR,
                     LENGTH = CONFIG_AP_PSRAM_SECTION_SIZE

__ap_psram_section_start = ORIGIN(PSRAM_SECTION);
__ap_psram_section_end = ORIGIN(PSRAM_SECTION) + LENGTH(PSRAM_SECTION);
```

首次allocator提交可以先不放对象，但不得将这6 MiB并入AP heap。后续静态对象只
允许显式标注到：

```text
.psram.data
.psram.bss
.psram.noinit
```

`.psram.text`首版仍禁止。启用静态section前必须确认CP在释放AP reset前已完成
PSRAM初始化；OpenVela reset handler先建立PSRAM MPU属性，再复制 `.psram.data`、
清零 `.psram.bss`。如果启动链不能保证PSRAM已上电，则静态section保持为空，
不能在board late阶段补初始化已经可能被C代码访问的全局对象。

### 9.2 链接断言

增加：

```ld
ASSERT(CONFIG_CP_PSRAM_HEAP_ADDR == 0x60700000, "CP PSRAM heap base mismatch")
ASSERT(CONFIG_CP_PSRAM_HEAP_SIZE == 0x00020000, "CP PSRAM heap size mismatch")
ASSERT(CONFIG_AP_PSRAM_HEAP_ADDR == 0x60720000, "AP PSRAM heap base mismatch")
ASSERT(CONFIG_AP_PSRAM_HEAP_SIZE == 0x002e0000, "AP PSRAM heap size mismatch")
ASSERT(__ap_psram_section_start == 0x60a00000, "AP PSRAM section base mismatch")
ASSERT(__ap_psram_section_end == 0x61000000, "16 MB layout end mismatch")
```

构建后扫描所有alloc section：只有经过批准的 `.psram.data/.psram.bss/.psram.noinit`
可以落入 `AP_PSRAM_SECTION`；媒体slab、CP heap和AP动态heap不能出现链接期对象。

## 10. 构建系统与布局单一来源

### 10.1 目标依赖链

正式依赖链：

```text
projects/app_ab/partitions/bk7258/ram_regions.csv
  -> BK ram region generator
  -> ram_regions.h
  -> validate/convert_bk7258_layout
  -> bk7258_openvela_layout.h
  -> OpenVela C/MPU配置和 ld.script预处理
  -> OpenVela AP ELF/raw
  -> EXTERNAL_AP_BIN
  -> Beken packager
  -> all-app.bin
```

`bk7258_openvela_layout.h` 只导出：

```c
BK7258_AP_RAM_BASE/SIZE
BK7258_CP_RAM_BASE/SIZE
BK7258_PWR_MNG_BASE/SIZE
BK7258_PSRAM_CAPACITY
BK7258_PSRAM_MEM_SLAB_USER_BASE/SIZE
BK7258_PSRAM_MEM_SLAB_AUDIO_BASE/SIZE
BK7258_PSRAM_MEM_SLAB_ENCODE_BASE/SIZE
BK7258_PSRAM_MEM_SLAB_DISPLAY_BASE/SIZE
BK7258_CP_PSRAM_HEAP_BASE/SIZE
BK7258_AP_PSRAM_HEAP_BASE/SIZE
BK7258_AP_PSRAM_SECTION_BASE/SIZE
```

不要把整个 `ram_regions.h` 或 Armino `sdkconfig.h`直接 include进 NuttX，避免宏
冲突和将 CP配置泄漏到 AP构建。

### 10.2 当前两步构建的处理

当前流程先构建 OpenVela，再构建 `app_ab`。这会导致 OpenVela无法自然消费本次
Armino构建生成的 layout。实施期有两种方式：

1. 推荐：增加独立 `generate-layout`步骤，在两个工程构建前生成并校验 header。
2. 过渡：从受版本控制的 `ram_regions.csv`运行转换脚本生成 OpenVela header，
   CP构建后再比较其 `ram_regions.h`，不一致则失败。

禁止直接复制上一次 `build/.../ram_regions.h`，因为增量构建可能使用旧的 8 MB
结果。

`projects/app_ab/build/.../armino/partitions/_build/ram_regions.csv`、`ram_regions.h`
和预处理后的链接脚本都属于构建产物，不是布局单一来源；当 `ram_regions.csv`
或配置文件变化时，必须执行项目级 clean 重新生成。

### 10.3 构建门禁

以下任一条件必须使构建失败：

- CSV容量不是 `16M`。
- 生成 `CONFIG_PSRAM_CAPACITY`不是 `0x01000000`。
- 七个原厂区域任一地址或大小与第3.2节不一致。
- 区域不连续、重叠，或总终点不是 `0x61000000`。
- CP未启用128 KiB `CP_PSRAM_HEAP`，或CP map越过该区域。
- OpenVela动态heap或媒体pool覆盖 `0x60700000..0x6071ffff`。
- OpenVela的非PSRAM section或未经批准对象落入PSRAM。
- `.psram.*`对象超出 `0x60a00000..0x60ffffff`。
- OpenVela MPU配置长度不是 16 MB。
- OpenVela配置意外设置 `CONFIG_MM_REGIONS > 1`并把 PSRAM加入通用 heap。
- AP/CP生成头的容量、base或size不一致。
- 顶层、CP核内和AP核内`_build/ram_regions.csv`或预处理链接脚本任一仍为旧布局。
- 最终`package/tmp/app1.bin`与指定的OpenVela `nuttx.bin`不一致。

## 11. 实施步骤

### 阶段 0：冻结现有 SRAM-only 基线

保存：

- 当前 OpenVela `nuttx`、`nuttx.bin`、`System.map`和 `.config`。
- 当前 `all-app.bin`、CP map和 generated `ram_regions.h`。
- 20次启动日志、Mailbox heartbeat和 NSH日志。
- 当前 SRAM `free`、任务列表和栈使用情况。

退出条件：现有 AP启动和日志连续复位20次通过。

### 阶段 1：建立 16 MB CP 硬件基线

1. 暂时保持原 AVDK AP，使用一个已存在的 16 MB工程验证器件。
2. 记录 CP 检测到的 PSRAM device ID和 capacity。
3. 执行0、4 MiB、8 MiB、12 MiB边界和最后有效word测试；16 MiB偏移
   `0x61000000`只做受控非法边界/alias负向验证，不作为器件内地址写入。
4. 执行地址线 alias测试，证明后 8 MB不是前 8 MB镜像。
5. 记录 80/120 MHz下读写稳定性和错误率。

退出条件：实板确认可稳定访问 `0x60000000..0x60ffffff`，且
`0x61000000`不属于器件。

### 阶段 2：切换 `app_ab` 原厂16 MB布局

1. 将容量改为 16 MB。
2. 按 `lvgl/img_decode`复制七个PSRAM区域的大小和顺序。
3. 保留CP 128 KiB heap及 `PSRAM_AS_SYS_MEMORY`。
4. 生成并校验全部AP/CP区域宏。
5. 保持CP PHY初始化、heap和PWC电源服务。

退出条件：CP构建成功，顶层/CP/AP核内生成布局一致，CP map只声明
`0x60700000..0x6071ffff`为`PSRAM_HEAP`，CP运行时heap统计也不越界，CP检测
16 MB并启动OpenVela AP。

### 阶段 3：OpenVela MPU 与只读探测

当前进度：MPU和保存/恢复型AP owner alias probe已实现；生成布局头、CP status
query和fault-safe异常捕获尚未实现。

1. 导入生成布局头。
2. 增加精确 16 MB MPU region。
3. 实现 PWC status query。
4. 只做边界、容量和保存/恢复型 alias probe，不建立 allocator。
5. 增加 fault-safe probe和错误日志。

退出条件：OpenVela能确认 CP已初始化 16 MB；非法边界请求能被 API拒绝，启用
硬件 guard时能够明确捕获 fault；
Mailbox和 SRAM功能无回归。

### 阶段 4：分owner内存测试

整颗16 MB的地址线和alias测试必须在阶段1的原厂测试固件中完成，并确保测试时
没有活跃CP/AP allocation。OpenVela运行时不得写入CP heap。

OpenVela在自己的allocator建立前，只对四个媒体slab、`AP_PSRAM_HEAP`和
`AP_PSRAM_SECTION`执行：

- data bus walking 1/0。
- address bus walking bit。
- 固定模式 `0x00000000`、`0xffffffff`、`0xaaaaaaaa`、`0x55555555`。
- 每 4 KiB页首尾校验。
- 各Vela区域伪随机写入/读回。
- 保存/恢复型跨区域alias对照，明确跳过 `0x60700000..0x6071ffff`。

CP对自己的128 KiB heap执行独立测试和统计。Vela只能通过PWC查询结果，不能读取
或扫描CP heap。

测试失败时保持 PSRAM `FAILED`，不发送媒体 ready，不建立 heap。

退出条件：原厂测试固件完成整颗容量验证；OpenVela连续冷启动100次AP区域测试
无错误；CP heap统计无损坏。破坏性测试只用于开发固件，后续量产启动改为快速
non-destructive probe。

### 阶段 5：独立 allocator

当前进度：AP heap、四个媒体pool、分配包装、state/generation/outstanding和统计
API已实现并通过`-Werror`构建；NSH/procfs命令及allocator负向/压力测试尚未完成。

1. 使用 `mm_initialize()`建立2.875 MiB `AP_PSRAM_HEAP`。
2. 为四个媒体slab分别建立pool。
3. 实现通用和媒体pool的分配、释放及对齐包装。
4. 实现 state、generation、outstanding和分区统计。
5. 增加 procfs/NSH诊断。
6. 增加跨pool free、CP heap指针、double free和越界负向测试。

退出条件：随机分配压力、碎片恢复、对齐和统计测试通过；SRAM `free`不因大块
PSRAM对象显著下降。

### 阶段 6：PWC 电源与 recovery

当前进度：已按当前SMP协议完成`0x7`power-on等待、KEEP_POWERED CPU1 vote、
`0x8`动态heap计数/掉电通知、`0x9`统计和`0xa`allocator freeze/busy基线；DMA/media
lease、掉电前veto、CP拒绝/timeout注入和完整服务recovery尚未完成。

1. 完成 power-on semantic response。
2. 增加 KEEP_POWERED lease。
3. 实现 freeze、busy和 power-off。
4. 实现 AP/PSRAM recovery generation。
5. 注入 CP拒绝、timeout、Mailbox reset和 PSRAM掉电。

退出条件：有活跃分配时 CP不能关闭 PSRAM；无分配时可安全关闭和重建；旧指针
不会被静默继续使用。

### 阶段 7：音频数据面

优先迁移风险较低的音频 buffer：

- PCM capture/playback ring。
- codec input/output buffer。
- AEC/NS/AGC workspace。
- 音频文件和网络 jitter buffer。

要求所有大块分配显式使用 PSRAM API；控制结构和 ISR小对象留 SRAM。

退出条件：双向音频运行4小时，无 underrun、数据错误、heap泄漏和 heartbeat超时。

### 阶段 8：视频数据面

迁移：

- camera frame buffer。
- JPEG/H264输入输出帧。
- scaler/DMA2D workspace。
- LCD/display buffer。
- UVC和网络发送队列。

退出条件：目标分辨率和帧率连续运行8小时，无 tearing、codec错误、DMA越界、
heap碎片耗尽和 CP/AP失联。

### 阶段 9：SMP 与 cache 后续

PSRAM单核稳定后再与 `BK7258_OPENVELA_SMP_PORTING_PLAN.md`合并：

- 两核使用同一 allocator。
- 状态锁和统计使用 SMP-safe primitive。
- 两核 MPU属性一致。
- CPU2不重复初始化 heap。
- DMA和两核并发分配压力通过。

cacheable PSRAM是再后一阶段，不与首次 16 MB启用同时提交。

## 12. 测试矩阵

### 12.1 静态检查

| 检查 | 通过条件 |
| --- | --- |
| CSV容量 | 16 MB |
| 原厂区域 | 七个区域地址、大小和顺序与第3.2节一致 |
| CP map | 只使用`0x60700000..0x6071ffff` |
| AP ELF | 仅批准的静态对象位于AP section |
| MPU | 精确 16 MB、RW/XN/non-cacheable |
| NuttX heap | SRAM heap与 PSRAM heap独立 |
| API | 所有大小计算有溢出检查 |
| 构建头 | AP/CP base、size、capacity一致 |

### 12.2 容量与地址线

| 测试点 | 地址 |
| --- | ---: |
| 首字 | `0x60000000` |
| 4 MB边界 | `0x60400000` |
| 8 MB边界 | `0x60800000` |
| 12 MB边界 | `0x60c00000` |
| 最后一个 word | `0x60fffffc` |
| 非法首地址 | `0x61000000` |

整颗器件测试固件在每个点写不同模式并交叉读回，避免只做线性 memcpy而漏掉
地址alias。OpenVela常规测试遇到CP heap地址必须跳过，不能按该表直接写入。

### 12.3 allocator

- 1 B到多 MB不同尺寸分配。
- 8/16/32/64/128/4096字节对齐。
- `calloc`清零和乘法溢出。
- `realloc`扩大、缩小和失败保持原块。
- AP动态heap接近2.875 MiB的组合分配。
- 四个媒体pool分别达到各自容量边界。
- 四个媒体pool与AP动态heap组合使用接近9.875 MiB；若静态section另有经过审计的
  实际对象，再验证动态区与静态区合计接近15.875 MiB，同时CP heap保持完整。
- 随机分配/释放100万次。
- 最大碎片场景和全部释放后的最大块恢复。
- double free、SRAM指针 free、越界指针和 stale generation负向测试。

### 12.4 并发与系统稳定性

- Mailbox日志洪泛同时进行 PSRAM随机读写。
- heartbeat、PWC命令与 allocator压力并发。
- 音频 DMA、视频 DMA和 CPU memcpy并发。
- CPU1 100% load持续1小时。
- 后续 SMP两核并发 malloc/free。
- 连续复位100次。
- 24小时基础压力和72小时音视频 soak。

### 12.5 故障注入

- CP报告 8 MB而构建要求16 MB。
- CP PSRAM初始化失败。
- power-on semantic response超时。
- Mailbox transport ACK丢失。
- allocator仍有对象时请求 power-off。
- DMA仍在运行时请求 recovery。
- AP reset而 CP保持 PSRAM上电。
- CP重置 PSRAM而 AP仍持有旧指针。
- 修改布局使终点超过 `0x61000000`，确认构建失败。

## 13. 构建与校验命令

OpenVela构建：

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

最终打包继续使用：

```bash
cp \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_nsh/nuttx.bin \
  /home/mi/vela_competition/bk_avdk_smp/build/openvela-ap.bin

cd /home/mi/vela_competition/bk_avdk_smp

make -C projects/app_ab clean

podman run --rm \
  --userns=keep-id \
  -v "$PWD:/armino" \
  -w /armino \
  localhost/bekencorp/armino-idk:1.5 \
  make -C projects/app_ab bk7258 \
  SDK_DIR=/armino \
  EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin
```

修改`ram_regions.csv`、AP/CP配置、Kconfig默认值、分区生成脚本或SoC链接脚本后，
上述项目级clean是强制步骤，不能依赖增量构建刷新核内临时分区文件。

构建后必须检查：

```text
顶层、CP核内、AP核内 ram_regions.csv/ram_regions.h:
  PSRAM_CAPCAITY_SIZE              = 16M
  CONFIG_PSRAM_CAPACITY             = 0x01000000
  CONFIG_CP_PSRAM_HEAP_ADDR         = 0x60700000
  CONFIG_CP_PSRAM_HEAP_SIZE         = 0x00020000
  CONFIG_AP_PSRAM_HEAP_ADDR         = 0x60720000
  CONFIG_AP_PSRAM_HEAP_SIZE         = 0x002e0000
  CONFIG_AP_PSRAM_SECTION_ADDR      = 0x60a00000
  CONFIG_AP_PSRAM_SECTION_SIZE      = 0x00600000

OpenVela System.map:
  .data/.vectors/.bss/.noinit/idle stack及SRAM heap全部位于AP_RAM
  AP私有SRAM终点严格为0x28064000，不占AP_SPINLOCK或CP_RAM
  仅批准的.psram.*位于0x60a00000..0x60ffffff

CP app.map:
  PSRAM_HEAP严格为0x60700000/0x20000

AP占位 app.map:
  四个媒体slab、AP_PSRAM_HEAP、AP_PSRAM_SECTION边界与第3.2节一致
  只用于布局检查，不代表最终OpenVela AP静态占用

最终包:
  nuttx.bin、openvela-ap.bin、tmp/app1.bin哈希和逐字节比较一致
```

## 14. 音视频移植使用规则

后续组件必须遵循：

1. 小型控制对象、锁、semaphore、ISR上下文和硬件状态留在 SRAM。
2. 大于可配置阈值的数据 buffer优先使用 PSRAM，但禁止偷偷替换全局 `malloc`。
3. 所有 DMA buffer使用 DMA专用 PSRAM API。
4. 每个媒体服务持有明确 power lease。
5. 服务停止顺序固定为 producer停止、DMA停止、consumer drain、释放 buffer、
   释放 lease。
6. codec预编译库若内部调用 `psram_malloc`，通过明确 OS ABI wrapper映射到
   `bk7258_psram_*`，不得映射到 `kmm_malloc`。
7. 对每个预编译库检查 `.psram.*`、固定地址和 FreeRTOS符号，禁止未审计接入。
8. framebuffer和媒体代码不直接散落硬编码地址；各pool base/size来自生成布局，
   对象地址来自对应pool allocator。
9. 驱动不得缓存跨 recovery有效的裸 PSRAM指针。
10. 内存不足时返回明确错误并降级分辨率、buffer数量或 codec配置，不能回退到
    片内 SRAM大块分配导致内核失稳。

## 15. 主要风险与决策

| 风险 | 后果 | 处理 |
| --- | --- | --- |
| 实物不是16 MB或ID异常 | 后8 MB访问镜像/损坏 | ID、地址线和全容量实测，失败禁用 heap |
| 只改CSV容量或只看顶层生成头 | CP/AP核内仍按旧heap宏构建 | 项目级clean，逐层核对临时CSV、生成头和链接脚本 |
| Vela覆盖CP 128 KiB heap | AP/CP allocator互相破坏 | 保留原厂边界并做map/运行时指针检查 |
| AP重复初始化PHY | 电压/时钟冲突 | CP唯一owner，AP只通过PWC请求 |
| PSRAM加入通用heap | 掉电时无法证明无引用 | 使用独立 `mm_heap_s` |
| reset阶段访问AP section时电源未就绪 | 启动立即fault | 确认CP先初始化；否则静态section保持为空 |
| 容量不匹配只告警 | 越过真实器件边界 | semantic query严格校验，失败不上线 |
| cache过早启用 | DMA/双核读取旧数据 | 首版non-cacheable，预留sync API |
| DMA安全属性不匹配 | SecureFault或静默不工作 | 逐master验证SAU/PPC/MPC和alias |
| active对象时掉电 | 音视频随机崩溃 | lease、freeze、outstanding和busy响应 |
| AP/CP reset不同步 | stale pointer和旧数据 | generation与服务重建 |
| 错把分区布局当成单heap | 跨CP区合并和内存破坏 | AP heap与四个媒体pool分别管理 |
| SMP只关闭本地IRQ | allocator状态竞争 | NuttX heap锁和SMP-safe状态锁 |
| SAU当前配置可疑 | 访问属性和安全边界错误 | golden寄存器对比后再验收 |

## 16. 不采用的方案

以下做法不作为正式交付方案：

- 在 OpenVela 中调用 CP版 `bk_psram_init()`。
- 直接链接 AVDK FreeRTOS `heap_4.c`。
- 使用 `CONFIG_PSRAM_AS_SYS_MEMORY`宣称 NuttX已支持 PSRAM。
- 在 `arm_addregion()`中把 16 MB加入通用 NuttX heap。
- 保留 CP heap同时让 OpenVela allocator覆盖完整 16 MB。
- 删除或移动原厂128 KiB CP heap而不完成CP依赖审计。
- 仅把 `PSRAM_CAPCAITY_SIZE`从8M改成16M而不采用原厂16 MB区域大小。
- 使用控制器64 MB映射窗口作为物理容量。
- 容量 mismatch后仅打印 warning继续运行。
- 启动时无条件清零完整16 MB，延长 ready并破坏保留数据。
- 首次启用同时打开 cache、SMP和音视频全功能。
- 将整个 PSRAM标记为 executable。
- 在媒体代码中散落固定地址或直接强转裸物理地址。

## 17. 推荐提交拆分

为便于回归和 review，建议按以下提交拆分：

1. `app_ab`切换到原厂16 MB七区域布局。
2. 布局转换脚本、生成头和构建门禁。
3. OpenVela PSRAM MPU region及 CP status query。
4. 整颗器件golden测试、Vela分owner测试和诊断命令。
5. 独立 `mm_heap_s` allocator及统计 API。
6. PWC power lease、freeze、busy和 recovery。
7. 音频 buffer迁移。
8. 视频/DMA buffer迁移。
9. SMP并发和可选 cache优化。

每个提交都必须保持当前 OpenVela AP启动、Mailbox日志、heartbeat和最终打包可用，
不得把所有改动合并成无法定位回归的一次性大提交。

## 18. 最终验收日志示例

正式媒体配置启动至少输出：

```text
ap0: psram: CP owner ready, id=0x........, capacity=16777216
ap0: psram: device range 0x60000000-0x60ffffff, non-cacheable, XN
ap0: psram: AP-owned region/alias test passed, CP heap skipped
ap0: psram: CP heap 0x60700000-0x6071ffff reserved
ap0: psram: AP heap region online, base=0x60720000, size=3014656, arena=<allocator-usable-bytes>
ap0: psram: media pools user/audio/encode/display online
ap0: psram: AP section 0x60a00000-0x60ffffff ready
ap0: OpenVela AP ready
ap0: nsh>
```

CP不应出现：

```text
psram type(16MB) not match CONFIG_PSRAM_CAPACITY
IPC retry to start core1
heartbeat timeout
PM CPU1 boot ready timeout
```

验收时还必须证明：

- CP map只占用原厂128 KiB `CP_PSRAM_HEAP`。
- OpenVela能使用后8 MB，而不是只使用原8 MB。
- `free`显示片内 SRAM仍独立稳定。
- `psram info`分别显示四个媒体pool、AP heap、AP section和CP保留区；AP section
  报告链接期实际占用与保留容量，不能伪装成动态free heap。
- Vela/AP区域总量为15.875 MiB，任何allocator都不跨CP heap。
- 音视频压力不会导致 Mailbox/PWC失效。
- power/recovery故障不会产生静默数据损坏。

完成本方案后，BK7258 OpenVela将按原厂16 MB布局获得15.875 MiB AP侧外部PSRAM
承载区域，其中7 MiB为媒体pool、2.875 MiB为AP动态heap、6 MiB为AP静态section；
CP保留128 KiB PSRAM heap并继续控制PSRAM硬件和全局电源。NuttX关键内核状态仍
驻留AP私有片内SRAM，媒体pool、AP动态heap、AP静态section和CP heap之间保持明确
隔离。
