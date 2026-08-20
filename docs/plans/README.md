# 移植方案索引

本目录保存当前仍用于实现、验证或作品规划的 9 份团队原创方案。方案描述目标、
协议和阶段门禁；当前实现状态以源码、最终 `.config` 和实板记录为准。

| 文档 | 用途 |
| --- | --- |
| `BK7258_OPENVELA_AP_PORTING_PLAN.md` | AP 启动、SMP、内存、Mailbox/PWC 总体方案 |
| `BK7258_OPENVELA_PSRAM_PORTING_PLAN.md` | PSRAM 地址、所有权、MPU 和 allocator |
| `BK7258_OPENVELA_UART0_MAILBOX_V2_PORTING_PLAN.md` | 单 UART0 双向控制台和 Mailbox V2 |
| `BK7258_OPENVELA_WIFI_PORTING_PLAN.md` | CP-backed Wi-Fi 与 NuttX `wlan0` |
| `BK7258_OPENVELA_WIFI_AP_MODE_PORTING_PLAN.md` | Wi-Fi SoftAP、IPv4/DHCP 所有权与 STA/AP 模式切换 |
| `BK7258_OPENVELA_BLUETOOTH_PORTING_PLAN.md` | CP controller 与 NuttX BLE Host 方案 |
| `BK7258_OPENVELA_SDIO_PORTING_PLAN.md` | SDIO、SD-NAND、MMCSD 和 VFAT |
| `BK7258_OPENVELA_MIMO_NETWORK_PORTING_PLAN.md` | MiMo 多模态云端网络与 TLS |
| `VELASIGHT_MCU_APP_DEVELOPMENT_PLAN.md` | 三键双圆屏、历史记录、社交辅助和 STA/AP 的 MCU App 计划 |
| `VELASIGHT_UI_DESIGN_INSTRUCTION.md` | 历史记录空白页、单张照片多模态问答、社交辅助、闲时助手与双屏软键 UI 设计规范 |
CPU2 SMP 的独立历史方案已归档到 `docs/archive/`；当前 SMP 结论已并入 AP 总体
方案和实际代码，不再维护第二份活跃方案。
