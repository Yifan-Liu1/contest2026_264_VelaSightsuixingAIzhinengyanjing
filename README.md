# VelaSight 随行 AI 智能眼镜：BK7258 OpenVela 适配

## 一、作品简介

本项目将 OpenVela/NuttX 移植到 BK7258 三核平台，以物理 CPU1/CPU2 运行
OpenVela AP，保留物理 CPU0 上的 Armino CP、Bootloader、无线控制器和标准打包
流程。在此基础上接入 Mailbox 控制台、Wi-Fi、相机、音频、显示、振动反馈和板载
SD-NAND，为随行 AI 智能眼镜提供可复现的系统底座。

项目当前已完成 AP 双核启动、单 UART0 双向控制台、NuttX `wlan0` 网络链路、
Camera/Audio 基础门禁，以及板载 SD-NAND 的只读 MMCSD/VFAT 链路。SD-NAND
保持 1-bit、PIO、只读模式，不启用 DMA、4-bit、格式化或写操作。

## 二、选题方向

本作品属于 **新硬件适配** 方向，同时承载 AI 硬件产品验证。主要工作不是将应用
简单部署到现成开发板，而是完成 BK7258 Cortex-M33 AP 的启动、中断、内存、SMP、
跨核 Mailbox、板级外设和 OpenVela 子系统接入，并保留可重复构建和烧录的 CP/AP
组合固件流程。

主要技术特点：

- 物理 CPU0 运行 Armino CP，物理 CPU1/CPU2 运行 OpenVela/NuttX SMP。
- AP 日志和 NSH 输入输出通过 Mailbox V2 转发到 CP UART0，无需额外 USB-TTL。
- Wi-Fi controller 保留在 CP，OpenVela AP 提供 NuttX `wlan0` 和网络协议栈。
- SD-NAND 通过 BK7258 SDIO Host 接入 NuttX MMCSD，提供只读 VFAT 访问。
- 最终固件通过 Beken 标准分区和 packager 生成 `all-app.bin`。

## 三、目录结构

```text
app/                         比赛应用与板级验证命令
board/beken/                 BK7258 OpenVela 芯片层和板级适配
external/bk_avdk_smp/        可复现的 Armino CP 覆盖文件
docs/                        构建说明、移植方案、验收记录和参考资料
docs/plans/                  7 份活跃实施方案
docs/archive/                未启动或已归档的历史方案与工具说明
docs/reference/              OpenVela 官方移植文档的本地参考副本
logs/                        按大赛格式导出的 AI Coding 日志
.claude/skills/autoflash/    自动烧录操作技能
autoflash.sh                 BK7258 固件自动烧录脚本
```

文档入口：

- [固件构建步骤](docs/固件构建步骤.md)
- [项目技能规范](docs/SKILLS.md)
- [GitHub 开发指南](docs/github开发指南.md)
- [基础适配门禁验收记录](docs/8.16基础适配门禁验收记录.md)
- [Wi-Fi 使用说明](docs/WiFi使用说明.md)
- [移植方案索引](docs/plans/README.md)

## 四、构建、烧录与运行

### 4.1 工作区布局

本项目需要比赛仓、OpenVela 工作树和 Beken AVDK SMP 工程：

```text
/home/mi/vela_competition/
├── contest/
│   └── contest2026_264_VelaSightsuixingAIzhinengyanjing/
└── bk_avdk_smp/
```

先按 [external/bk_avdk_smp/README.md](external/bk_avdk_smp/README.md) 将比赛仓中的
CP 权威覆盖文件同步到 `bk_avdk_smp`，并执行逐字节校验。

### 4.2 构建 OpenVela AP

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

### 4.3 打包最终固件

```bash
cp /home/mi/vela_competition/contest/cmake_out/bk7258-ap_nsh/nuttx.bin \
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

完整步骤、产物路径和哈希校验见 [docs/固件构建步骤.md](docs/固件构建步骤.md)。

### 4.4 烧录和控制台

```bash
cd /home/mi/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
./autoflash.sh -p /dev/ttyUSB1 -n 1
```

连接开发板 UART0 后，在 CP 命令行进入 AP NSH：

```text
ap_console status
ap_console open
```

退出 AP 控制台时先按 `Ctrl-]`，松开后再按 `.`。SD-NAND 会在系统启动后延迟
5 秒初始化，也可在 NSH 中执行 `sdnand_init` 手动触发。成功后设备节点为
`/dev/mmcsd0`，MBR 分区为 `/dev/mmcsd0p0`。

## 五、AI Coding 使用说明

AI 参与了需求拆解、源码取证、移植方案设计、驱动实现、构建错误定位、串口日志
分析、实板验证和文档维护。项目使用 [docs/SKILLS.md](docs/SKILLS.md) 固定架构约束、
仓库职责、任务路由和验证门禁，再由 `docs/plans/` 中的子系统方案维护协议细节和
测试矩阵，避免让历史结论覆盖当前源码、最终配置和实板证据。

AI 生成或建议的改动均需经过源码比对、`-Werror` 构建、最终镜像哈希校验和对应
实板门禁。按大赛格式导出的完整对话记录放在 `logs/`；运行期串口日志不冒充
AI Coding 日志。

## 六、当前边界

- Bluetooth Host 仍处于方案阶段，未作为已完成功能声明。
- SD-NAND 当前只读，未启用写入、格式化、DMA、4-bit 或 multiblock。
- Secure alias 和 CMSE 是当前功能基线，不等于量产 secure boot 已闭环。
- 构建成功、打包成功、实板启动和功能验收分别记录，不互相替代。

提交、rebase、PR、CLA 和 Rebase and merge 要求见
[docs/github开发指南.md](docs/github开发指南.md)。
