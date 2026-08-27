# BK7258 OpenVela 项目技能规范

> 本文件是比赛仓内的项目级交付规范，路径以
> `/home/mi/vela_competition` 工作区根目录为基准；它在工作区根通用规则之上补充
> `external` 完整文件 overlay、构建与验收要求，以本副本为比赛仓内权威说明。

本文件是 AI 在本工作区进行 BK7258 OpenVela 移植开发时必须遵守的项目级操作规范。
它不复制各移植方案的具体设计，只固定稳定规则、当前状态、任务路由和验证门禁。
具体设计、寄存器表、协议结构和测试矩阵由比赛仓 `docs/plans/` 中的方案维护。

## 1. 证据与文档优先级

遇到文档与代码冲突时，按以下优先级判断，不得自行挑选更方便的结论：

```text
1. 当前源码和最终 .config
2. 生成的分区、布局、ELF、map、package manifest 和二进制哈希
3. 当前实板日志与测试记录
4. 比赛仓 docs/固件构建步骤.md
5. 最新的子系统移植方案
6. AP 总体移植方案
7. bk_avdk_smp / bk_idk / openvela / vendor_beken / 网页文档 / 官方 Markdown
8. 文档中的历史状态、建议代码和历史哈希
```

移植方案描述设计目标和某一时点的状态，不得覆盖当前源码、最终配置、生成文件或实板证据。

## 2. 工作区与仓库职责

```text
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/autoflash.sh
    自动烧录脚本，当单片机连接到电脑，可使用脚本自动烧录固件验证。当cutecom占用串口，则kill cutecom
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
    BK7258 正式移植代码的唯一所有权目录，提交到 dev-ai-contest-2026 分支
contest/vendor/beken/
    构建入口符号链接，不是源码所有权目录，不要在此直接编辑
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/固件构建步骤.md
    已验证的 OpenVela AP + Armino CP 构建和打包流程
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/README.md
    apps、nuttx、packages/ai_agent、bk_avdk_smp 四棵完整目标文件 overlay 的权威流程
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/manifest.tsv
    四个目标仓固定基线、Armino 镜像 ID、官方来源和离线包 SHA-256
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/prepare.sh
    overlay 安全安装与只读检查的唯一入口
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/plans/
    各子系统移植方案，按需阅读
bk_avdk_smp
    BK7258 原厂 1CP2AP 目标 checkout，提供 CP/bootloader/分区/打包；外部改动只由完整文件 overlay 安装和验证
bk_avdk_smp_original_backup
    bk_avdk_smp工程的原厂备份，读取作为对比参考，当“分析/对比原厂实现”时使用
bk_idk
    BK7258 基础示例和底层驱动参考工程
bk_solution_ai
    开发板外设移植参考工程
vendor_beken
    BK7236N 的 OpenVela 参考实现，仅参考目录组织和 NuttX 接入方式
openvela / openvela_official_md
    OpenVela 原始源码和官方文档参考
博通网页文档
    寻找技术细节时阅读
```

不得把 BK7258 代码放在 `vendor_beken/`，也不得直接修改 `bk_avdk_smp/ap` 来实现 OpenVela 内核移植。

## 3. 架构和资源所有权

### 3.1 固定架构结论

- BK7258 AP 是 Cortex-M33 / ARMv8-M Mainline / Thumb / hard-float，不是 Cortex-A / ARMv8-A。
- 不得引入 `arch/arm64`、GIC、AArch64、generic timer、MMU/页表、TF-A/BL31 或 `OUTPUT_ARCH(aarch64)`。
- BK7258 是三核 AMP：物理 CPU0 运行 Armino CP，CPU1 是 OpenVela AP primary，CPU2 是 AP secondary。
- AP 内部 SMP 时 CPU1/CPU2 运行同一个 `nuttx.bin`，映射为 NuttX 逻辑 CPU0/CPU1。
- 当前运行基线是 CPU1 单核，CPU2 SMP 尚未实现。

### 3.2 CPU0/CP 保留职责

CP 继续拥有并负责：

- bootloader、CP 固件和 FreeRTOS 运行环境
- 中央电源/时钟管理、硬件 WDT
- PSRAM PHY、电压、时钟、控制器和器件 ID 检测
- Flash 擦写编程
- Wi-Fi controller、RWNX/UMAC/LMAC/MAC/PHY/RF
- Bluetooth BTDM controller-only
- CP UART0 物理日志口

AP 不得重复执行与 CP 冲突的 PHY、时钟、电源或控制器初始化。

### 3.3 内存地址来源

Flash、SRAM、PSRAM 和保留区地址来自目标 `app_ab` 分区生成结果，不是 BK7258 芯片常量：

```text
AP Flash XIP:    0x02150000 (当前 app_ab 布局)
AP 私有 SRAM:    0x28010000..0x28064000 (336 KiB)
AP_SPINLOCK:     0x28000000..0x2800ffff
CP_RAM:          0x28064000..0x2809f6ff
PWR_MNG:         0x2809f700..0x2809f7ff
PSRAM 窗口:      0x60000000..0x60ffffff (当前 16 MiB 布局)
```

AP 不得清零 CP_RAM、PWR_MNG、SWAP，不得把 `AP_SPINLOCK` 放入 heap，不得覆盖 CP 的 128 KiB PSRAM heap。

### 3.4 AP 镜像注入

AP 镜像只能通过 `EXTERNAL_AP_BIN` 接口注入打包流程，不得手工覆盖 `package/tmp/app1.bin`。

### 3.5 AP 控制台

正式 AP 日志路径是 Mailbox UART -> CP UART0。物理 UART1 不是交付控制台，不接管 GPIO0/1。

### 3.6 安全状态

当前 Secure alias / CMSE 只表示按现有 AP 的 SPE 编译属性建立功能基线，不等于镜像已通过 secure boot。在安全启动、OTP/eFuse 和 CPU1 镜像认证闭环前，输出只能标注为开发构建。

## 4. 当前实现状态

以下状态以当前源码和最终 `.config` 为准，具体日期和提交见各子系统方案：

| 模块 | 状态 |
| --- | --- |
| CPU1 AP 启动、reset、MPU、向量 | 已实现并有实板启动基础 |
| Mailbox v2 channel 1、IRQ79、RX drain | 已实现 |
| transport ACK、超时、重试、CP envelope 校验 | 已实现基础，完整 reset 恢复和压力验证仍有缺口 |
| HW_CTRL power-up、heartbeat、PWC ready | 已实现基础 |
| Mailbox UART 日志转发到 CP UART0 | 已实现 |
| GPIO、PWM、按键、内核 LED | 已实现 |
| 16 MiB PSRAM MPU、AP heap、四个媒体 pool | 已实现基线，器件容量查询、DMA lease、恢复和完整实测未闭环 |
| Camera GC2145 / LCD | 驱动和框架代码存在，完整显示链仍在开发 |
| CPU2 NuttX SMP | 已实现基础启动、IPI 和双逻辑 CPU 运行基线 |
| Wi-Fi wlan0 STA | 已实现并通过关联、DHCP、DNS、TCP/TLS/HTTPS 实板门禁 |
| Wi-Fi wlan0 SoftAP | WPA2 beacon、手机关联和 NuttX DHCP 启动已实板通过；双向数据面、反复切换和稳定性门禁待完成 |
| Bluetooth NuttX Host | 方案阶段，未实现 |
| SDIO / MMCSD / FAT | 可写 1-bit PIO、CMD24、MMCSD、FAT32、自动挂载和重启持久化实板通过；DMA/4-bit/CMD25 未启用，压力与断电门禁未完成 |
| 量产 secure boot | 未建立 |

构建成功、打包成功、实板启动和功能验收是四个不同状态，不得混写成"完成"。

## 5. 任务路由

开始任何任务前，先读取对应方案：

| 任务 | 必读文档 |
| --- | --- |
| 启动、IRQ、内存、Mailbox/PWC、AP 所有权 | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/plans/BK7258_OPENVELA_AP_PORTING_PLAN.md` |
| 构建和最终打包 | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/固件构建步骤.md` |
| PSRAM、allocator、媒体内存 | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/plans/BK7258_OPENVELA_AP_PORTING_PLAN.md` |
| CPU2 / NuttX SMP | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/plans/BK7258_OPENVELA_AP_PORTING_PLAN.md`；历史独立方案在 `docs/archive/` |
| UART0 / Mailbox V2 控制台 | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/plans/BK7258_OPENVELA_UART0_MAILBOX_V2_PORTING_PLAN.md` |
| Wi-Fi STA | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/plans/BK7258_OPENVELA_WIFI_PORTING_PLAN.md` |
| Wi-Fi SoftAP、DHCP 和 STA/AP 切换 | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/plans/BK7258_OPENVELA_WIFI_AP_MODE_PORTING_PLAN.md`；操作命令见 `docs/WiFi使用说明.md` |
| Bluetooth | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/plans/BK7258_OPENVELA_BLUETOOTH_PORTING_PLAN.md` |
| SDIO / SD-NAND / FAT | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/plans/BK7258_OPENVELA_SDIO_PORTING_PLAN.md` |
| MiMo 多模态网络、TLS、ASR/TTS | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/plans/BK7258_OPENVELA_MIMO_NETWORK_PORTING_PLAN.md` |
| Git、rebase、提交和推送 | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/github开发指南.md` |
| OpenVela 通用驱动开发 | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/reference/driver_development.md` |
| 最小 NSH bring-up | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/reference/minimum_nsh_baseline.md` |
| 芯片移植规范 | `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/reference/porting_guide.md` |
| defconfig 编写参考 | vendor_beken/boards/bk7236n/bk7236n-evb/configs/nsh/defconfig |

## 6. 开发工作流

1. 读取任务路由中对应方案和 GitHub 指南。
2. 比赛仓自有 AP/board 修改应位于 `board/beken/`，不在参考目录；公共仓修改先在真实
   目标仓实现和测试，再以完整文件同步到对应 `external/<repository>/` 相对路径。
3. 确认改动不违反第 3 节架构约束。
4. 实现改动，Make 和 CMake 源列表必须一致。
5. 按第 7 节构建和验证。
6. 通过验证后按第 9 节提交。
7. 每次完成更改后都需 commit 到 `dev-ai-contest-2026` 分支。

## 7. 构建与打包门禁

任何当前构建命令之前都必须阅读 `external/README.md`，并从比赛仓根执行只读检查：

```bash
cd /home/mi/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
./external/prepare.sh check
```

尚未安装 overlay 时，使用 `./external/prepare.sh install`，或直接走推荐产品入口
`./build_and_flash.sh --prepare-overlay`。Armino 镜像的 Docker Hub/Beken 官方来源、固定
image ID 与离线包 SHA-256 只以 `external/manifest.tsv` 为准；tag 或历史日志不能替代
该门禁。

### 7.1 OpenVela AP 构建

本项目有两个不同的板级配置：`nsh` 仅用于最小启动/底层对照，`ai_agent`
用于 VelaSight 产品固件。早期门禁命令保留 `nsh` 是因为它先完成了 AP
基础启动验证；当前包含 VelaSight、LVGL 和中文界面的最终镜像必须使用
`ai_agent`。两者的 `.config`、输出目录和固件内容不能混用。

```bash
cd /home/mi/vela_competition/contest

./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent \
  --cmake \
  -j8
```

修改过 `Kconfig` 或 `defconfig` 时先执行：

```bash
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent \
  --cmake distclean
```

输出：

```text
contest/cmake_out/bk7258-ap_ai_agent/nuttx       # ELF
contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin   # AP raw binary
contest/cmake_out/bk7258-ap_ai_agent/System.map
```

### 7.2 准备外部 AP 输入

```bash
cp \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin \
  /home/mi/vela_competition/bk_avdk_smp/build/openvela-ap.bin
```

### 7.3 构建 CP 并打包最终固件

```bash
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

仅安装 Podman 时不要使用会在内部调用 Docker 的 `dbuild.sh`。

### 7.4 强制 clean 条件

以下任一改动必须先执行 `make -C projects/app_ab clean` 再打包：

- 分区、RAM/PSRAM CSV
- AP/CP 配置
- Kconfig 默认值
- 分区生成脚本
- SoC 链接脚本

### 7.5 输出与哈希校验

最终固件：

```text
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/all-app.bin
```

OTA 固件：

```text
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/app_ab_crc.rbl
```

三个 AP 文件 SHA256 必须一致且 `cmp` 返回 0：

```bash
sha256sum \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin \
  /home/mi/vela_competition/bk_avdk_smp/build/openvela-ap.bin \
  /home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin

cmp -s \
  /home/mi/vela_competition/contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin \
  /home/mi/vela_competition/bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
```

## 8. 静态与实板验证

构建和哈希一致不能替代以下验证：

- 检查最终 `.config` 中关键符号与预期一致。
- 检查 ELF attributes：Cortex-M33、hard-float、`-mcmse`。
- 检查 `System.map` 和 section 边界不越界。
- 检查 `bk_package.json` 中 AP 角色和文件名。
- 当前产品使用完整 CJK 字体；因 Beken code 分区必须按 `34K` 对齐，固定数据
  分区前的最大合法值为 `primary_ap_app=4148K`，AP linker raw `FLASH` 区域为
  `0x3d0000`（3904K）。分区源和 OpenVela linker 不能只改一侧。
- 实板复位、NSH 交互、CP 不出现 `IPC retry to start core1` 或 heartbeat timeout。
- 子系统功能、压力、恢复和安全测试按对应方案执行。

## 9. Git 与交付要求

- 开发分支：`dev-ai-contest-2026`。
- 仓库：`contest/contest2026_264_VelaSightsuixingAIzhinengyanjing`。
- Fork 同步使用 `--force-with-lease`，rebase 到 `openvela/dev-ai-contest-2026`。
- 避免 merge commit，保持线性历史。
- PR 必须通过 `cla/signature`。
- 提交信息遵循仓库风格。
- 详细 Git 操作阅读 `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/docs/github开发指南.md`。

## 10. 按需参考资料

以下材料不在常规工作流中加载，只在对应任务时读取：

- OpenVela 驱动子系统文档：`openvela/docs/zh-cn/device_dev_guide/`
- OpenVela 内核文档：`openvela/docs/zh-cn/device_dev_guide/kernel/`
- AVDK 媒体示例：`bk_avdk_smp/projects/*/README.md`
- AI 解决方案：`bk_solution_ai/projects/*/README.md`
- BK7236N vendor 参考：`vendor_beken/chips/bk7236n/`
- Armino 构建工具：`bk_idk/tools/build_tools/`
- OpenVela 测试框架：`openvela/tests/`
- 博通网页文档：`博通网页文档/`
