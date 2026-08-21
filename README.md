# VelaSight 随行 AI 智能眼镜：BK7258 OpenVela 适配

## 一、作品简介

本项目将 OpenVela/NuttX 移植到 BK7258 三核平台，以物理 CPU1/CPU2 运行
OpenVela AP，保留物理 CPU0 上的 Armino CP、Bootloader、无线控制器和标准打包
流程。在此基础上接入 Mailbox 控制台、Wi-Fi、相机、音频、显示、振动反馈和板载
SD-NAND，为随行 AI 智能眼镜提供可复现的系统底座。

项目当前已完成 AP 双核启动、单 UART0 双向控制台、NuttX `wlan0` 网络链路、
Camera/Audio 基础门禁，以及板载 SD-NAND 的可写 MMCSD/VFAT 链路。SD-NAND
保持 1-bit PIO 和 CMD24 单块写，不启用 DMA、4-bit 或 CMD25 多块写。

## 二、选题方向

本作品属于 **新硬件适配** 方向，同时承载 AI 硬件产品验证。主要工作不是将应用
简单部署到现成开发板，而是完成 BK7258 Cortex-M33 AP 的启动、中断、内存、SMP、
跨核 Mailbox、板级外设和 OpenVela 子系统接入，并保留可重复构建和烧录的 CP/AP
组合固件流程。

主要技术特点：

- 物理 CPU0 运行 Armino CP，物理 CPU1/CPU2 运行 OpenVela/NuttX SMP。
- AP 日志和 NSH 输入输出通过 Mailbox V2 转发到 CP UART0，无需额外 USB-TTL。
- Wi-Fi controller 保留在 CP，OpenVela AP 提供 NuttX `wlan0` 和网络协议栈。
- SD-NAND 通过 BK7258 SDIO Host 接入 NuttX MMCSD，提供 `/mnt/sdnand` 持久 VFAT。
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

`nsh` 和 `ai_agent` 是两个不同的板级配置。`nsh` 是最小 NSH 启动和底层
资源对照配置；`ai_agent` 才是 VelaSight 产品配置，包含 VelaSight App、
LVGL 双屏界面、UTF-8 中文和英文显示以及 `packages/ai_agent`。早期基础
移植使用 `nsh` 是为了验证 AP 启动链，不能据此构建最终产品镜像。最终烧录
必须使用 `ai_agent`，否则会得到能够启动但不包含目标 UI 的最小固件。

```bash
cd /home/mi/vela_competition/contest

./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent \
  --cmake distclean

./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent \
  -e -Werror \
  --cmake \
  -j8
```

### 4.3 打包最终固件

```bash
cp /home/mi/vela_competition/contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin \
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
5 秒初始化，并将已有 FAT 自动挂载到 `/mnt/sdnand`；挂载失败只报错，不自动
格式化。`sdnand_init status` 可查看状态，`sdnand_init provision --confirm` 显式执行
破坏性 raw 写门禁并整盘重建 FAT32。设备节点为 `/dev/mmcsd0`；已有 MBR 时优先
使用 `/dev/mmcsd0p0`，整盘 FAT32 使用前者。

### 4.5 VelaSight 按键操作

设备使用三个实体按键，屏幕底部会根据当前页面显示对应的软键提示。短按为按下后
`500 ms` 内松开；需要确认的长按和双键组合均需持续约 `2 s`。

| 实体按键 | 对应软键 | 主页面短按 | 长按/组合操作 |
| --- | --- | --- | --- |
| 电源键 | 确认 | 历史页发起语音询问；照片问答页拍照后提问 | 在历史或照片问答页长按，进入社交辅助模式 |
| 音量加 | 返回 | 查看上一条历史记录；在语音、拍照或结果页取消/返回 | 社交模式中长按结束本次交流并生成摘要；设备热点页长按切回已保存的 Wi-Fi |
| 音量减 | 下一条 | 查看下一条历史记录 | 与音量加同时长按，切换已保存 Wi-Fi STA 与设备热点 AP 模式 |

社交辅助模式中，电源键短按可暂停或继续采集；结束后，按电源键或音量加可关闭交流
摘要并回到历史页面。设备热点模式会在屏幕上显示热点名称、密码和配网网页地址；手机
连接热点后按页面提示完成配网，再长按音量加返回 STA 模式。

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
- SD-NAND 已通过 CMD24、FAT32 格式化、0/512/4096-byte 文件、rename/unlink 和
  重启持久化实板门禁；10,000 次随机写、断电、24 小时长稳、DMA、4-bit 和
  CMD25 multiblock 尚未验收或启用。
- Secure alias 和 CMSE 是当前功能基线，不等于量产 secure boot 已闭环。
- 构建成功、打包成功、实板启动和功能验收分别记录，不互相替代。

提交、rebase、PR、CLA 和 Rebase and merge 要求见
[docs/github开发指南.md](docs/github开发指南.md)。
