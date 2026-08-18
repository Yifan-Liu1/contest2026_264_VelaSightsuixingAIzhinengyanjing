# VelaSight 单片机 App 开发计划

> 文档状态：2026-08-17 规划稿。本文结合工作区根目录 `app_feature.md`、当前
> BK7258/OpenVela 源码和实板记录制定，只覆盖设备端 App。Wi-Fi SoftAP 尚未完成，
> Web Server 不属于本 App 的实现工作；相关模块只保留状态、接口和 `TODO(web-owner)`
> 注释，不提供 HTTP、页面、文件下载或配置接口实现。

## 1. 目标与范围

本 App 面向 BK7258 上的 OpenVela/NuttX，使用三枚物理按键和两块 160x160
GC9D01 圆形 RGB565 显示器，完成以下设备端闭环：

- 上电默认进入 Wi-Fi STA 模式和历史记录索引界面。
- 浏览本地轻量历史索引，一屏显示记录索引信息，另一屏显示摘要或情绪比例。
- 在历史记录页短按确认键启动闲时语音助手；助手只附带当前屏幕选中的一条记录。
- 在无记录的空白页短按确认键时先拍摄一张照片，将照片和本轮语音问题交给通用
  多模态 Agent，不创建社交会话，也不保存为社交历史。
- 语音录制由端侧音量检测自动判断尾静音并截断，也可再次短按确认键手动截断；
  返回键短按可在录音、云端处理或 TTS 播放期间取消。
- 语音助手期间索引屏改为显示“聆听、识别、思考、播报、已取消、失败”等状态，
  收到回答后以 TTS 为主要输出，屏幕只保留一至两行摘要。
- 在历史界面长按确认键进入社交辅助模式，显示进入进度和操作提示。
- 社交辅助期间采集 Camera/Audio、与统一云端接口通信，并显示实时情绪和简短建议。
- 短按确认键暂停/继续社交采集；长按返回键结束会话、等待最终结果、落盘后回到最新记录。
- 在历史界面同时长按返回键和下一个键，请求在 STA 与 SoftAP 间切换。
- SoftAP 界面显示热点名和密码，长按确认键请求随机重置凭据。
- 以有限分辨率下的简洁、可扫视、低刷新交互为首要设计原则。

首版实现闲时语音助手，但不在设备端运行 ASR、大模型或复杂视觉推理。设备只完成
PCM 采集、轻量音量检测、上下文选择、可选单帧拍摄、请求编排和 TTS 播放；ASR、
通用多模态理解和回答生成由获准的云端 Agent 完成。系统不做人脸身份识别、人格判断、
心理或医学诊断。云端内容必须使用“可能”等非确定性表达；数据不足时显示“无法判断”。

## 2. 当前代码事实与约束

| 项目 | 当前事实 | App 约束 |
| --- | --- | --- |
| MCU/系统 | BK7258，OpenVela/NuttX AP；CP 保留 Wi-Fi controller 等职责 | App 使用 NuttX API 和已有设备节点，不直接访问 Wi-Fi/显示寄存器 |
| 按键 | Power GPIO12、Volume Down GPIO8、Volume Up GPIO13；`board_buttons()` 已可读取 | 轮询读取，不在 GPIO ISR 内执行业务或刷新屏幕 |
| 显示 | `/dev/fb0`、`/dev/fb1`，160x160，RGB565，各 51200 B | 无局部刷新，状态变化后整帧提交；避免持续动画 |
| Camera | `/dev/video0`，GC2145；可信图像暂应走 UYVY 加软件 JPEG | 固定有界缓冲；最终路线需先解决硬件 JPEG 错帧或接受低帧率软件编码 |
| Audio | 已有基础门禁，目标 16 kHz/16 bit/mono | 音频优先于图片，不允许图像上传阻塞音频 |
| Agent 语音代码 | `packages/ai_agent` 已有录音、ASR、视觉调用、流式 TTS 和 PCM 播放实现 | 优先增加稳定适配接口并复用，不在 App 内复制第二套 ASR/TTS 协议栈 |
| 网络 | STA 已实板通过；SoftAP 只有移植方案，尚未实现 | 默认 STA；AP 请求必须显式显示“不支持/启动失败”，不得伪装成功 |
| 存储 | SD-NAND 当前只读；`/mnt` PSRAM ramdisk 掉电丢失 | 持久历史依赖可写存储，未完成前只允许易失 demo 记录并明确标记 |
| 图形库 | 当前直接使用 NuttX framebuffer，未引入 LVGL/NX | 首版沿用轻量 RGB565 软件绘制，避免新增大型 GUI 依赖 |
| 既有原型 | `app/social_cue` 已有相机、mock 云结果和振动闭环 | 保留为验证命令；产品 App 复用思路，不直接改写该原型 |

两个关键阻断项必须在完整产品验收前关闭：

1. 本地历史持久化需要可写且掉电保持的存储。当前只读 SD-NAND 和 PSRAM ramdisk
   都不能满足要求。
2. 目标要求固定 5 FPS 上传可信 JPEG，而当前可信软件 JPEG 实测约 3 FPS，硬件 JPEG
   又有错帧缺陷。阶段演示可降低图像频率，但不能把它记作 5 FPS 验收通过。

## 3. 按键定义

### 3.1 物理映射

| 语义键 | 板级按键 | GPIO | 统一名称 |
| --- | --- | --- | --- |
| 确认 | Power | 12 | `VS_KEY_CONFIRM` |
| 返回 | Volume Down | 8 | `VS_KEY_BACK` |
| 下一个 | Volume Up | 13 | `VS_KEY_NEXT` |

### 3.2 识别参数

- 扫描周期：10 ms。
- 稳定消抖：连续 30 ms 状态一致。
- 短按：稳定按下后在 50 至 799 ms 内释放。
- 长按：稳定按下达到 800 ms，只发送一次 `LONG_PRESS`；释放不再补发短按。
- 组合长按：返回和下一个在 150 ms 窗口内同时稳定按下，并持续 1200 ms。
- 组合键成立后吞掉两个单键事件，防止同时触发返回或翻页。
- 进入社交模式的确认键进度从 0 至 800 ms 映射到 0 至 100%，建议每 80 ms
  产生一次进度事件，松手则立即清除。

阈值最终放入 Kconfig，以上数值作为首版默认值。按键 worker 只产生事件，不直接改变
App 状态。

### 3.3 各界面行为

| 状态 | 确认短按 | 确认长按 | 返回短按 | 返回长按 | 下一个短按 | 返回+下一个长按 |
| --- | --- | --- | --- | --- | --- | --- |
| 历史索引 | 启动语音助手并附带当前记录 | 进入社交辅助 | 返回默认摘要页 | 无操作 | 下一条记录/切换摘要与饼图 | STA/AP 切换请求 |
| 空白历史页 | 启动语音助手并拍摄单张照片 | 进入社交辅助 | 无操作 | 无操作 | 无操作 | STA/AP 切换请求 |
| 语音聆听 | 手动截断并提交 | 无操作 | 立即取消 | 立即取消 | 无操作 | 禁止模式切换 |
| 语音识别/思考 | 无操作 | 无操作 | 立即取消在途请求 | 立即取消在途请求 | 无操作 | 禁止模式切换 |
| TTS 播放 | 停止播报并返回 | 无操作 | 立即取消播放并返回 | 立即取消播放并返回 | 无操作 | 禁止模式切换 |
| 社交采集中 | 暂停 | 无操作 | 无操作 | 结束并等待云结果 | 切换提示页 | 禁止模式切换 |
| 社交暂停 | 继续 | 无操作 | 无操作 | 结束并等待云结果 | 切换提示页 | 禁止模式切换 |
| 等待最终结果 | 无操作 | 无操作 | 无操作 | 可二次长按取消等待并保存 incomplete | 无操作 | 禁止模式切换 |
| SoftAP | 无操作 | 随机重置 SSID/密码请求 | 请求返回 STA | 隐藏密码/返回 STA，二选一后固定 | 切换 SSID/密码页 | 请求返回 STA |
| 错误提示 | 确认重试 | 无操作 | 返回历史索引 | 无操作 | 查看下一条错误信息 | 禁止模式切换 |

## 4. 状态机

### 4.1 顶层状态

```text
BOOT
  -> HISTORY_STA
      -> VOICE_PREPARING
      -> VOICE_LISTENING
      -> VOICE_TRANSCRIBING
      -> VOICE_THINKING
      -> VOICE_SPEAKING
      -> HISTORY_STA
      -> SOCIAL_ENTERING
      -> SOCIAL_STARTING
      -> SOCIAL_RUNNING <-> SOCIAL_PAUSED
      -> SOCIAL_FINALIZING
      -> HISTORY_STA
      -> NET_SWITCH_TO_AP -> AP_UNSUPPORTED | AP_READY
  AP_READY
      -> AP_RESETTING_CREDENTIALS
      -> NET_SWITCH_TO_STA
      -> HISTORY_STA
  any operational state -> ERROR -> HISTORY_STA
```

- `BOOT`：初始化按键、双屏、存储索引和 STA 状态；任何单项失败都形成可显示错误，
  不因一块屏或一条历史损坏而崩溃。
- `HISTORY_STA`：默认状态。仅加载轻量索引和当前记录摘要，不加载全部 JSON。
- `VOICE_PREPARING`：冻结触发瞬间的页面上下文。有选中记录时按 `record_key` 读取该条
  完整记录的白名单字段；空白页时独占 Camera 拍摄一张可信 JPEG。准备完成后立即释放
  Camera，再启动麦克风，避免 Camera 与 Audio 初始化互相阻塞。
- `VOICE_LISTENING`：采集 16 kHz/16 bit/mono PCM，并在端侧运行轻量音量检测。检测到
  有效语音后的连续尾静音、再次短按确认键或达到最长录音时间都会结束录音并提交；
  返回键短按直接取消，不提交已有 PCM。
- `VOICE_TRANSCRIBING`：上传音频并等待 ASR 文本；空文本、仅噪声或低于最短有效语音
  时回到历史页，不调用大模型。
- `VOICE_THINKING`：有记录时发送“问题 + 当前记录白名单内容”；空白页时发送“问题 +
  单张照片”到通用多模态 Agent。禁止自动附带其他历史、API Key、日志或原始社交媒体。
- `VOICE_SPEAKING`：显示一至两行回答摘要，按显式长度接收 TTS PCM/二进制并流式播放。
  返回键短按立即停止下载和 DAC；播放完成或确认键短按停止后返回触发前页面。
- `SOCIAL_ENTERING`：显示长按进度；按键提前释放则回到 `HISTORY_STA`。
- `SOCIAL_STARTING`：创建云会话，成功后才启动 Camera/Audio 上传；失败清理资源。
- `SOCIAL_RUNNING`：采集、上传、接收实时情绪和建议。
- `SOCIAL_PAUSED`：停止产生新媒体块；保留会话和已确认序号。恢复时使用新时间戳，
  不补造暂停期间数据。
- `SOCIAL_FINALIZING`：先停止 Camera/Audio，再排空音频和控制消息，发送 finalize，
  轮询最终结果，校验、写完整记录、写索引、ACK 云端。
- `AP_UNSUPPORTED`：当前实际分支。显示“AP 未支持”，2 秒后或按返回回到 STA 历史界面。
- `AP_READY`：只有 SoftAP 驱动、DHCP 和网络验收通过后才能进入。

### 4.2 事件模型

所有异步来源写入同一个有界 App 事件队列：

```text
button worker -----> key/progress event ----+
cloud worker ------> cue/result/error event +--> app loop --> state snapshot
media worker ------> started/stopped/error --+                  |
voice worker ------> vad/asr/answer/tts event +                  |
storage worker ----> loaded/stored/error ----+                  +--> display worker
network worker ----> sta/ap/up/down/error ---+
```

- 队列满时不得丢失按键释放、结束、取消、VAD 截断、网络错误和最终结果事件。
- 可合并的低优先级事件只有长按进度、音量电平和连续实时情绪更新，保留最新值即可。
- 状态只由 App 主循环修改；worker 不共享写业务状态。
- 显示 worker 接收不可变快照，刷新时不持有媒体、网络或存储锁。

## 5. 双圆屏交互设计

圆屏边角不可用，核心内容放在中心直径约 136 px 的安全区。首版不显示滚动长文，
不使用细线、复杂图例或需要精确触控的元素。

### 5.1 历史记录索引

`/dev/fb0` 为索引屏：

- 中央显示 `当前序号/总数`，例如 `03/12`。
- 上方显示短日期或“最新”；下方显示最长两行标题。
- 底部以三个简单图标/短词提示确认、返回、下一个。
- 有记录时短按确认启动对当前记录的语音回顾，长按确认进入社交辅助。
- 无记录时显示“暂无记录”，短按确认使用“照片 + 语音”通用多模态助手，长按确认
  进入社交辅助。

`/dev/fb1` 为概略屏：

- 默认显示一至两行摘要和状态标志（完整/incomplete/易失）。
- 摘要与情绪饼图由下一个键按页面顺序切换；切到下一条记录前先完成当前记录的
  “摘要 -> 饼图 -> 图例”页序列。
- 饼图最多显示占比最高的 4 类，其余合并为“其他”。
- 饼图旁不堆叠文字；下一页用色块加情绪名称、百分比展示图例。
- 颜色必须同时配中文短标签，不能只靠颜色传递语义。

### 5.2 社交模式进入与运行

进入进度期间：

- 索引屏显示环形进度条和“继续按住”。
- 概略屏显示“松开取消 / 满圈开始”。

运行期间：

- `/dev/fb0` 显示云端最新实时情绪名称、连接/暂停状态和数据新鲜度。
- `/dev/fb1` 显示最多两行社交提示；没有可靠线索时显示“继续倾听”或“无法判断”。
- 极端情绪只改变背景强调色和边框，不闪烁，不使用全屏快速反色。
- 极端情绪建议至少连续多个有效窗口且置信度过阈值后才显示，并设置冷却时间，
  防止屏幕颜色频繁跳变。
- 暂停时两屏加明显暂停符号，保留最后内容但降低饱和度。

### 5.3 闲时语音助手

语音助手占用两屏期间不再显示可操作的历史索引：

- `/dev/fb0` 显示大号状态词：准备、聆听、识别、思考、播报、取消或失败。
- `VOICE_LISTENING` 时显示低频更新的音量环和“Power 结束 / 返回取消”；音量环仅为
  反馈，不显示精确 dB，刷新不高于 10 Hz。
- `/dev/fb1` 在记录回顾路径显示当前记录短标题和“仅发送本条记录”；在空白页路径
  显示“已拍摄 1 张照片”，不得显示 Camera 实时预览。
- `VOICE_TRANSCRIBING` 和 `VOICE_THINKING` 期间只显示阶段和超时进度，不显示完整字幕、
  Prompt 或历史正文。
- `VOICE_SPEAKING` 时 `/dev/fb1` 显示回答的一至两行 `display_text`，完整回答仅用于 TTS。
- 取消后立即清屏并恢复触发前的记录和页码；失败页显示短错误码，不能播放上一次回答。

端侧音量检测采用轻量 VAD，不引入神经网络模型：

1. 录音开始先采集 300 至 500 ms 环境声建立 RMS 噪声底，但用户立即说话时不得把明显
   高能量语音吸收到噪声底。
2. 以 20 ms PCM 窗计算去直流后的 RMS，阈值使用“噪声底乘数 + 最小绝对门槛”，并设置
   开始/结束不同阈值形成迟滞。
3. 连续 3 至 5 个窗口超过开始阈值才判定开始说话；说话开始前保留约 300 ms pre-roll，
   避免吞掉首字。
4. 已检测到语音后，连续约 800 ms 低于结束阈值自动截断。录音开始后若 5 秒仍无语音，
   显示“未听清”并取消请求。
5. 最长录音默认 30 秒，到时强制截断；小于 300 ms 的有效语音不提交 ASR。
6. VAD 使用增益前或固定增益 PCM 计算，不允许 AGC 将静音逐步放大成语音；阈值、尾静音、
   无语音和最长时长均放入 Kconfig，并通过实板噪声样本标定。

建议调色板（RGB565 在实现时预计算）：

| 情绪 | 颜色 | 用途 |
| --- | --- | --- |
| 中性/未知 | 深灰 + 白字 | 常态背景 |
| 积极/愉悦 | 绿色 | 温和正向线索 |
| 困惑/不确定 | 黄色 | 注意和确认理解 |
| 悲伤/低落 | 蓝色 | 低唤醒负向线索 |
| 愤怒/强烈压力 | 红色 | 极端情绪强调 |
| 害怕/焦虑 | 洋红 | 高唤醒不安线索 |

### 5.4 Finalizing 与结果返回

- 索引屏显示无无限动画的分段等待指示和“正在整理记录”。
- 概略屏轮换显示“停止采集”“上传尾包”“云端分析”“本地保存”等当前阶段。
- 成功落盘后，将新记录插入索引首位并选中，然后回到历史界面。
- 超时或 Schema 非法时不显示旧结果；记录标为 incomplete，并显示可重试错误码。

### 5.5 SoftAP 界面

目标态下 `/dev/fb0` 显示 SSID，`/dev/fb1` 显示密码。SSID 和密码按固定宽度换行，
默认密码可在 30 秒后遮蔽；长按确认生成随机凭据后，先持久化，再重启 SoftAP。

当前 SoftAP 未支持时，只进入 `AP_UNSUPPORTED`，两屏分别显示“AP 未支持”和
“仍使用 STA”。严禁展示虚构热点名、密码或成功状态。

## 6. 数据结构与本地存储

社交协议沿用 `app_feature.md` 的 `social-session/v1`、`social-alert/v1`、
`social-finalize/v1` 和 `social-cue/v1`。闲时助手沿用 `voice-assistant/v1` 和
`voice-review/v1`，并增加空白页通用多模态请求 `voice-vision/v1`。设备内部使用定长字段
和显式长度，禁止将网络输入直接当作 NUL 结尾字符串。

上下文选择在 Power 短按触发瞬间冻结：

```text
当前页关联 complete record
  -> voice-review/v1: question + selected record only

当前为空白页或无可用 record
  -> capture one JPEG
  -> voice-vision/v1: question + one image only
```

`voice-review/v1` 只允许上传当前记录的 `record_id`、`title`、`body`、`summary`、
`full_transcript`、`timeline`、`emotion_distribution` 和 `content_truncated`；按总请求上限
裁剪并设置截断标志。`voice-vision/v1` 图片使用原始 JPEG 二进制 multipart 或云接口规定的
二进制方式，不在设备内重复保留 Base64 副本。照片和 PCM 仅属于本轮请求，完成、取消或
超时后立即释放，不写入历史记录。

建议上限在 Kconfig 中配置并由 JSON 解析层强制执行：

| 字段 | 首版上限 |
| --- | --- |
| 标题 | UTF-8 96 B |
| 屏幕摘要 | UTF-8 192 B |
| 实时情绪名 | UTF-8 32 B |
| 实时提示 | UTF-8 160 B |
| 情绪类别 | 8 类 |
| 时间线 | 由完整响应总长度和可写存储容量共同限制 |
| 最终 JSON | 256 KiB，超限拒绝或由协议层明确截断 |
| 闲时录音 | 默认最长 30 s，16 kHz/16 bit/mono，约 960 KiB；应流式 ASR 或使用有界分块，禁止依赖单块 SRAM |
| 闲时照片 | 1 张可信 JPEG，不附带其他 Camera 帧 |
| TTS | 按响应显式长度流式接收和播放，单块缓冲建议 4 至 16 KiB |

持久化顺序固定为：

```text
records/<record_id>.tmp
  -> 完整写入、fsync、重新打开并校验
  -> 原子 rename 为 records/<record_id>.json
  -> 写 index.tmp、fsync、原子替换 index.json
  -> 向云端 ACK stored
```

默认列表只显示 `complete`。若存储仍为 PSRAM ramdisk，所有记录必须带 `volatile=true`
并在 UI 标注“重启后清除”；不得宣称已完成历史持久化。

## 7. 源码目录与文件职责

计划目录：

```text
app/velasight/
├── include/
│   ├── vs_app.h
│   ├── vs_types.h
│   ├── vs_input.h
│   ├── vs_display.h
│   ├── vs_history.h
│   ├── vs_social.h
│   ├── vs_voice.h
│   ├── vs_media.h
│   ├── vs_cloud.h
│   ├── vs_network.h
│   └── vs_config.h
├── velasight_main.c
├── vs_app.c
├── vs_input.c
├── vs_display.c
├── vs_history.c
├── vs_social.c
├── vs_voice.c
├── vs_media.c
├── vs_cloud.c
├── vs_network.c
├── vs_config.c
├── vs_web_stub.c
├── Kconfig
├── CMakeLists.txt
├── Make.defs
└── Makefile
```

本次只创建空 `.c/.h` 占位文件。构建元数据在进入第一阶段实现时再创建，避免空源文件
进入现有构建。

| 源码文件 | 对应功能与实现边界 |
| --- | --- |
| `velasight_main.c` | NSH/builtin 入口；解析仅供调试的启动参数，初始化上下文并运行 App；不承载业务状态 |
| `vs_app.c/.h` | 顶层状态机、事件队列、资源生命周期和状态快照；是唯一可修改顶层状态的模块 |
| `vs_types.h` | 状态、事件、错误码、记录索引、情绪分布、显示快照等定长公共类型 |
| `vs_input.c/.h` | 读取 `board_buttons()`，完成消抖、短按、长按、组合长按和进度事件生成 |
| `vs_display.c/.h` | 打开 `/dev/fb0/1`、RGB565 双缓冲、圆屏布局、文字/图标、饼图、进度环和整帧提交 |
| `vs_history.c/.h` | 轻量索引分页、按需读取完整 JSON、完整记录先于索引的事务写入、启动恢复与损坏隔离 |
| `vs_social.c/.h` | 社交业务子状态、开始/暂停/继续/finalize/取消编排，以及实时提示和最终记录归一化 |
| `vs_voice.c/.h` | 闲时助手子状态、当前记录/空白页上下文冻结、轻量 VAD、手动截断、ASR/Agent/TTS 编排和全阶段取消 |
| `vs_media.c/.h` | Camera/Audio 的启动停止、有界队列、时间戳和序号；音频优先、图片丢旧帧策略 |
| `vs_cloud.c/.h` | 统一 HTTPS 会话接口、TLS 校验、请求序号、响应限长、JSON Schema 校验、轮询和 ACK |
| `vs_network.c/.h` | STA 状态查询、STA/AP 互斥切换请求、SoftAP 凭据生成与状态；当前 AP 返回 `-ENOTSUP` |
| `vs_config.c/.h` | 设备端非敏感配置的默认值、读取、校验和原子保存；API Key 不进入该模块或仓库 |
| `vs_web_stub.c` | 仅为未来 Web 所有者保留编译边界；本 App 不实现服务器。实现阶段只允许留下 `TODO(web-owner)` 注释和 `-ENOSYS` stub |

### 7.1 构建文件计划

- `Kconfig`：总开关、主线程栈、worker 栈、事件队列深度、按键阈值、协议长度、
  设备节点和 mock 开关。
- `CMakeLists.txt`：使用 `nuttx_add_application(NAME velasight ...)`，列出全部正式源文件。
- `Makefile`：与 CMake 源列表逐项一致，禁止两套构建遗漏模块。
- `Make.defs`：注册到比赛应用包路径。
- 在上层比赛 App 清单或 manifest 中增加 `app/velasight` 映射；具体映射方式沿用当前
  `camera_preview` 和 `social_cue`。

## 8. 线程、优先级与内存路线

首版建议 6 个执行上下文，具体优先级按最终系统负载实测调整：

| 上下文 | 职责 | 规则 |
| --- | --- | --- |
| App 主线程 | 状态机和事件消费 | 不做阻塞网络、文件大读写或整帧转换 |
| Input worker | 10 ms 按键扫描 | 小栈、固定周期，不打印高频日志 |
| Media worker | Camera/Audio 采集和排队 | 音频路径优先，图片队列满时丢最旧帧 |
| Voice worker | VAD、ASR/Agent 请求和流式 TTS 编排 | 与社交模式互斥；每个阻塞步骤都检查取消 generation |
| Cloud worker | TLS、上传、接收和 finalize 轮询 | 所有请求有超时和取消令牌 |
| Display worker | 生成两块帧缓冲并提交 | 事件驱动；同屏短时间多次更新合并为一次 |

Storage 可先由 Cloud worker 在 finalizing 阶段串行执行，只有实测写入阻塞影响音频或 UI
时才拆出第七个 worker。闲时语音助手和社交辅助模式互斥占用 Camera、ADC、DAC 与云请求
上下文；切换前必须先停止并释放上一模式的全部媒体资源。

内存原则：

- 两块显示前台 framebuffer 已由驱动持有；App 最多再持有一块 51200 B 绘制缓冲并串行
  更新两屏，优先于额外保留两块后台缓冲。
- JPEG 和 PCM 使用 PSRAM 有界池，不用无上限 `malloc` 链表。
- 语音问答优先使用流式 ASR；若首版云接口只能整包上传，则 PCM 使用 PSRAM 有界 ring，
  达到上限自动截断，绝不扩容越界。
- ADC 录音与 DAC/TTS 默认不并发。开始播报前关闭录音；取消播报调用
  `audio_playback_stop()` 并关闭仍在下载的响应。
- JSON 使用流式或有上限解析；最终记录允许落临时文件，不要求整包永久驻留 SRAM。
- UI 快照使用定长短文本，不引用网络接收缓冲生命周期之外的指针。
- Camera、Audio、网络和显示关闭流程必须可重复调用，以便错误回滚。

## 9. 网络、AP 与 Web 边界

### 9.1 STA

- 开机查询 `wlan0` 的真实 RUNNING/carrier/地址状态，不能只以“存在 IP 配置”判断联网。
- 未联网时历史浏览仍可用；进入社交模式前显示网络错误并允许重试。
- 未联网时短按 Power 不启动录音或拍照，先显示网络不可用，避免采集无法提交的隐私数据。
- TLS 必须校验证书；Token 不写日志、URL、历史 JSON 或普通配置文件。

### 9.2 SoftAP

SoftAP 的驱动、CP/AP wire role、DHCP 和 L3 所有权按
`BK7258_OPENVELA_WIFI_AP_MODE_PORTING_PLAN.md` 实施。App 只调用以下抽象能力：

```c
int vs_network_request_mode(enum vs_net_mode mode);
int vs_network_get_ap_credentials(struct vs_ap_credentials *out);
int vs_network_reset_ap_credentials(struct vs_ap_credentials *out);
```

在底层验收前，三个 AP 路径返回 `-ENOTSUP`，状态机进入 `AP_UNSUPPORTED`。

随机凭据要求：使用 `/dev/urandom` 或 BK7258 已接入的 TRNG；SSID 使用设备短 ID 加随机后缀，
WPA2 密码至少 12 个可辨识字符。随机失败时保留旧凭据，不使用时间戳或固定默认密码降级。

### 9.3 Web 预留

`vs_web_stub.c` 不注册 socket、不监听端口、不嵌入网页资源。未来 Web 工作仅可通过已定义的
配置快照和只读记录访问接口与本 App 交互，不能直接访问 App 内部状态、媒体队列或 API Key。
预留注释格式统一为：

```c
/* TODO(web-owner): Implement the AP-only configuration and record service. */
```

Web 功能不纳入本计划里程碑和完成判据。

## 10. 实现阶段与技术路线

### 阶段 0：门禁与契约冻结

- 确认三个按键的最终丝印映射、长按阈值和组合键人体工学。
- 冻结云端实时提示和最终结果 JSON Schema、长度上限、错误码和超时。
- 冻结 ASR、通用 Agent、记录回顾和 TTS 的端点、鉴权、音频/图片格式、取消语义及响应长度；
  现有 MiMo 网络计划已指出 ASR/TTS 端点仍需从平台控制台确认，该项是首版阻断项。
- 决定可写持久存储方案，记录挂载点、容量、原子 rename/fsync 行为。
- 对可信 5 FPS 图像路线作出选择：修复硬件 JPEG 边界，或采用可达 5 FPS 的其他编码路线。

完成判据：阻断项均有负责人、接口和可测方案，不以 mock 代替硬件结论。

### 阶段 1：离线 UI 与按键闭环

- 添加 Kconfig、CMake、Make 文件和最小 `velasight` 入口。
- 实现事件队列、顶层状态机、按键识别和 mock 历史数据。
- 实现双屏索引、摘要、饼图、颜色图例、长按进度、暂停和错误页。
- 实现语音助手各阶段状态页、音量环和全阶段取消的本地 mock。
- AP 组合键走 `-ENOTSUP` 页面。

完成判据：无需网络即可连续操作 30 分钟；无重复短按、组合键串扰、屏幕越界或堆增长。

### 阶段 2：历史存储

- 实现轻量索引启动加载、分页和完整记录按需读取。
- 实现 `.tmp -> fsync -> rename -> index` 的事务写入和掉电恢复。
- 注入截断 JSON、坏 UTF-8、超长字段、断电残留 tmp 和索引悬挂项。

完成判据：100 条记录下浏览内存保持有界；任意单条损坏不影响其他记录；重启后记录存在。

### 阶段 3：闲时语音助手闭环

- 为 `packages/ai_agent` 的录音、ASR、`llm_chat_vision_raw()`、Agent 消息和
  `voice_channel_speak()` 增加 App 可调用、可取消、长度安全的适配接口。
- 实现自适应 RMS VAD、pre-roll、尾静音、无语音超时、最长录音和 Power 手动截断。
- 实现“当前记录 + 问题”的回顾路径，以及“单张照片 + 问题”的通用多模态路径。
- TTS 使用小块流式播放；返回键在录音、请求和播放阶段均能在有界时间内取消。

完成判据：记录回顾、空白页拍照、VAD 自动截断、Power 手动截断、返回取消和 TTS 播放
均完成真机往返；取消后无 Camera/ADC/DAC/TLS 资源残留。

### 阶段 4：社交媒体与云端闭环

- 将 `social_cue` 原型中的 Camera、PWM 和拒绝策略经验迁入正式模块。
- 接入 16 kHz PCM 分块、可信 JPEG、有界队列和统一 HTTPS 接口。
- 实现实时 `social-alert/v1` 更新、短按暂停/继续、长按结束、finalize 轮询和 ACK。
- 结果先校验再显示，先完整记录再写索引，ACK 后才允许云端清理。

完成判据：正常、暂停恢复、网络拥塞、无人脸、Schema 非法、云端超时和用户结束路径均实板通过。

### 阶段 5：SoftAP App 接口接入

- 等 SoftAP 驱动方案独立通过实板验收后，将 `vs_network` 的 `-ENOTSUP` 替换为真实调用。
- 实现凭据显示、TRNG 随机重置、AP 返回 STA，以及切换期间共享资源清理。
- 不实现 Web Server，只为 Web 所有者提供稳定接口。

完成判据：STA/AP 可运行时互斥切换且无需复位；失败能回滚 STA；UI 与真实热点状态一致。

### 阶段 6：资源、异常与演示验收

- 运行长会话、快速按键、弱网、断网、存储满、单屏失败和低内存压力测试。
- 检查音频连续性、图片丢帧策略、显示刷新耗时、线程栈余量、堆峰值和文件恢复。
- 使用 `-Werror` 完整构建，执行 AP/CP 打包、镜像哈希一致和实板回归。

## 11. 测试矩阵

| 类别 | 必测场景 | 通过条件 |
| --- | --- | --- |
| 按键 | 抖动、快速短按、按住、两键错峰、三键同时按 | 每次只产生规定事件，无幽灵短按 |
| 历史 | 0/1/100 条、坏索引、坏记录、超长 UTF-8 | 页面可用，内存有界，坏记录隔离 |
| 显示 | 最长标题、最大百分比、双屏一块打开失败 | 文本不越安全区；单屏失败不阻塞业务 |
| VAD | 安静、恒定风噪、突发噪声、远讲、连续说话、无语音、30 s 上限 | 不吞首字，不把稳定噪声当语音，不无限录音 |
| 语音回顾 | 每个历史子页触发、超长记录、记录读取失败 | 只上传触发时选中的一条记录，截断有标志 |
| 多模态助手 | 空白页触发、拍照失败、照片上传失败 | 仅一张照片；失败不退化为上传其他历史或旧照片 |
| TTS | 正常播放、分块中断、返回取消、Power 停止 | 按长度播放；停止后 DAC 和下载均关闭，不播放旧缓存 |
| 社交 | 开始、暂停、恢复、长按结束、finalize 成功 | Camera/Audio 生命周期和云序号正确 |
| 云端 | 401/409/413/429/5xx、TLS 失败、非法 JSON、超时 | 明确错误，不展示旧数据，不泄漏缓冲 |
| 媒体 | 图片队列满、音频压力、取消时有在途包 | 丢旧图保音频，取消后资源全部释放 |
| 存储 | 写满、掉电 tmp、rename 失败、ACK 前重启 | 不出现指向不完整 JSON 的 complete 索引 |
| AP | 当前未支持、启动失败、凭据随机失败 | 明确显示真实状态；旧凭据不被破坏 |
| 隐私 | 取消、超时、最终 ACK、日志检查 | 原始媒体离开会话即释放，日志无密钥/正文 |

## 12. 构建与验收命令

修改 Kconfig 后先 clean，再按项目门禁构建：

```bash
cd /home/mi/vela_competition/contest
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh --cmake distclean
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh -e -Werror --cmake -j8
```

构建通过只说明静态集成成功。最终还需按 `docs/固件构建步骤.md` 生成 CP/AP 组合固件、
核对三个 AP 二进制哈希，并在实板上逐项执行第 11 节门禁。

## 13. 首版完成定义

以下条件同时满足，才能把 MCU App 标记为完成：

- 上电默认 STA 历史索引，三键全路径稳定，长按和组合键无串扰。
- 双屏历史索引、摘要、饼图、图例、实时情绪、提示、暂停和 finalizing 页面实板可读。
- 历史页短按 Power 能只携带当前记录完成语音回顾；空白页能以单张照片和语音问题调用
  通用多模态 Agent。
- 端侧 VAD 能自动截断，也能由 Power 短按手动截断；返回键可取消录音、云请求和 TTS，
  TTS 回答可真机播放且不依赖字符串结束符判断长度。
- 社交模式 Camera/Audio/统一云接口/最终记录形成真实闭环，不依赖 mock。
- 历史记录写入掉电保持存储，完整 JSON 先于轻量索引，重启恢复通过。
- 极端情绪需满足持续和置信度门槛，颜色之外始终有文字标签。
- 取消、断网、超时、非法响应和存储失败都停止采集并释放资源。
- SoftAP 未完成时只显示“不支持”；完成后 UI 必须与真实热点状态一致。
- Web Server 保持未实现并明确移交，不计入 MCU App 完成范围。
- `-Werror` 构建、最终固件打包、哈希核对和实板测试记录全部通过。
