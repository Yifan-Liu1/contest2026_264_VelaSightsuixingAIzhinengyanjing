# VelaSight 单片机 App 开发计划

> 双屏内容、历史记录空白页、单张照片多模态问答、软键栏和长按进度的产品交互以
> [VELASIGHT_UI_DESIGN_INSTRUCTION.md](VELASIGHT_UI_DESIGN_INSTRUCTION.md)
> 为准。本开发计划保留工程状态、接口和验收信息；发生交互冲突时以后者为准。

> 文档状态：2026-08-18 会议对齐稿，已按当前源码核查更新。本文结合工作区根目录 `app_feature.md`、
> 2026-08-18《OpenVela 竞赛功能梳理和流程拆解》会议记录、当前 BK7258/OpenVela
> 源码和实板记录制定，只覆盖设备端 App。发生冲突时，本稿以 2026-08-18 会议明确结论
> 为当前产品和演示口径，具体 HTTP 路径与 JSON 字段仍以云端团队后续接口文档为准。
> 当前源码已实现 STA/SoftAP 互斥切换、DHCP 和 AP 配网 Web Server。配网组件提供
> HTTP 表单和 NAND 单文件凭据存储，VelaSight 负责启动/停止热点、展示热点凭据并在
> 配网成功后切回 STA；文件下载仍不属于该服务。

## 1. 目标与范围

本 App 面向 BK7258 上的 OpenVela/NuttX，使用三枚物理按键和两块 160x160
GC9D01 圆形 RGB565 显示器，完成以下设备端闭环：

- 上电默认进入 Wi-Fi STA 模式和历史记录索引界面。
- 浏览本地轻量历史索引，一屏显示记录索引信息，另一屏显示摘要或情绪比例。
- 在历史记录页短按确认键启动闲时语音助手；助手只附带当前屏幕选中的一条记录。
- 历史索引末尾固定增加一张空白页；短按确认键先拍摄一张照片，将照片和本轮语音问题交给通用
  多模态 Agent，不创建社交会话，也不保存为社交历史。
- 语音录制由端侧音量检测自动判断尾静音并截断，也可再次短按确认键手动截断；
  返回键短按可在录音、云端处理或 TTS 播放期间取消。
- 语音助手期间索引屏改为显示“聆听、识别、思考、播报、已取消、失败”等状态，
  收到回答后以 TTS 为主要输出，屏幕只保留一至两行摘要。
- 在历史界面长按确认键进入社交辅助模式，显示进入进度和操作提示。
- 社交辅助期间采集 Camera/Audio、与统一云端接口通信，并显示实时情绪和简短建议。
- 社交模式演示基线按约 20 至 30 秒的一次明确会话设计：历史页长按 Power 开始、
  社交中长按返回结束；
  代码不写死 20 秒上限，长会话扩展留到演示闭环之后。
- 社交图像起步目标为可信 3 FPS，采一张即上传一张；能力允许后挑战 5 FPS。音频按
  2 秒一块持续上传，由云端按 `session_id` 和时间戳拼接。
- 短按确认键暂停/继续社交采集；长按返回键结束会话、等待最终结果、落盘后回到最新记录。
- 在历史界面同时长按返回键和下一个键，请求在 STA 与 SoftAP 间切换。
- SoftAP 界面显示热点名和密码，长按确认键请求随机重置凭据。
- 历史、空白页、社交、语音和错误页面底部显示当前有效的三键软键名称；短按显示响应态，进入长按进度后隐藏软键名称。
- 错误页面只支持短按重试或返回，不提供长按失败返回、长按清除或其他长按失败界面。
- 以有限分辨率下的简洁、可扫视、低刷新交互为首要设计原则。

首版实现闲时语音助手，但不在设备端运行 ASR、大模型或复杂视觉推理。设备只完成
PCM 采集、轻量音量检测、上下文选择、可选单帧拍摄、请求编排和 TTS 播放；ASR、
通用多模态理解和回答生成由设备上的现有 `ai_agent` 直接调用获准的模型 API 完成，
不经过自建社交会话云。这里的“不经过云端”特指不经过团队自建中转云，并不表示端侧
离线推理。系统不做人脸身份识别、人格判断、
心理或医学诊断。云端内容必须使用“可能”等非确定性表达；数据不足时显示“无法判断”。

## 2. 当前代码事实与约束

| 项目 | 当前事实 | App 约束 |
| --- | --- | --- |
| MCU/系统 | BK7258，OpenVela/NuttX AP；CP 保留 Wi-Fi controller 等职责 | App 使用 NuttX API 和已有设备节点，不直接访问 Wi-Fi/显示寄存器 |
| 按键 | Power GPIO12、Volume Up GPIO13、Volume Down GPIO8；语义为确认/取消、返回、下一个，`board_buttons()` 已可读取 | 轮询读取，不在 GPIO ISR 内执行业务或刷新屏幕 |
| 显示 | 物理右屏为 `/dev/fb0`，物理左屏为 `/dev/fb1`；均为 160x160、RGB565、51200 B | 右屏负责索引、状态和动画，左屏负责摘要、建议和操作说明；无局部刷新，状态变化后整帧提交 |
| Camera | `/dev/video0`，GC2145；可信图像暂应走 UYVY 加软件 JPEG | 演示先按可信 3 FPS，采一张上传一张；达到 5 FPS 前不宣称 5 FPS 完成 |
| Audio | 已有基础门禁，目标 16 kHz/16 bit/mono | 音频优先于图片，不允许图像上传阻塞音频 |
| Agent 语音代码 | `packages/ai_agent` 已有录音、ASR、视觉调用、流式 TTS 和 PCM 播放实现 | 优先增加稳定适配接口并复用，不在 App 内复制第二套 ASR/TTS 协议栈 |
| 网络 | `vs_network.c` 已实现 `wlan0` 的 STA/SoftAP 互斥切换、IPv4/DHCP、配网 Web 服务启停和保存后切回 STA | 默认 STA；切换失败进入错误页，不伪装成功；目标 STA 凭据统一读取 NAND 配网文件，热点凭据来自 Kconfig |
| 存储 | SD-NAND 的系统读写链路已有实板验收；VelaSight 当前仍使用 `vs_app.c` 静态历史数据，尚未接入记录持久化 | 必须补齐记录文件、索引、掉电恢复和重启测试；未完成前仍按 App 持久化阻断项处理 |
| 图形库 | 当前业务界面已迁移到 LVGL，使用 NuttX framebuffer backend | VelaSight 产品构建取消板级开机动画；面板初始化保持背光关闭，双 framebuffer 注册后先 push 纯黑保护帧并启动 LVGL，待 LVGL 同步完成双屏首帧后再开背光。启用 Arc、Label、UTF-8 和完整 `U+4E00-U+9FFF` CJK 字体；`nsh` 仅为最小启动/对照配置 |
| 既有原型 | `app/social_cue` 已有相机、mock 云结果和振动闭环 | 保留为验证命令；产品 App 复用思路，不直接改写该原型 |

三个关键阻断项必须在完整产品验收前关闭：

1. 本地历史持久化尚未接入 App。虽然 SD-NAND 系统读写链路已通过基础实板验收，当前
   `vs_app.c` 仍只使用静态记录，尚未实现记录文件、索引和掉电恢复。
2. 会议已把演示起点调整为可信 3 FPS，并保留 5 FPS 挑战目标。当前可信软件 JPEG
   约 3 FPS，硬件 JPEG 有错帧缺陷；演示可按 3 FPS 验收，但 5 FPS 只能在可信画面实测
   达到后单独标记完成。
3. 自建社交云的上传接口、结果拉取方式、异常情绪字段、ASR/TTS 确切端点和鉴权仍待
   云端接口文档冻结。App 先依据本文抽象接口和 mock 包开发，不能把猜测路径固化为协议。

### 2.1 2026-08-18 会议冻结项

| 主题 | 当前冻结结论 |
| --- | --- |
| 社交会话边界 | 历史页长按 Power 开始，社交中长按返回结束；演示约 20 至 30 秒，但设备端不写死时长 |
| `session_id` | 由设备在会话开始前生成，所有图片、音频、控制和结果请求携带同一 ID |
| 图片 | 可信 3 FPS 起步、目标 5 FPS；一张一请求/一次上传，不在设备聚合多张 |
| 音频 | 16 kHz/16 bit/mono，2 秒一块上传；云端缓存和拼接，设备不落原始音频 |
| 上传接口 | 图片和音频可共用一个媒体上传入口，以媒体类型和元数据区分；最终路径由云端定稿 |
| 平时返回 | 无强烈情绪时保持静默，不要求设备每帧解析普通情绪结果 |
| 强烈情绪 | 先返回固定的情绪变化标志、情绪/颜色字段和短文本，再异步提供大模型建议 |
| 会话结束 | 云端基于完整字幕、时间轴和情绪记录生成最终结果，返回摘要、建议、原始结构化记录和最终 TTS |
| 云端保存 | 只做会话期缓存；设备确认结果后删除原始图片、音频和中间缓存 |
| 本地保存 | 只保存处理后的文本、字幕、情绪、置信度、时间戳、摘要和建议，不保存原始音视频 |
| 闲时助手 | 设备端 `ai_agent` 直接访问模型 API，不经过自建社交云；选中记录可原样作为白名单上下文 |
| Web/AP | SoftAP、DHCP、手机配网页面和 NAND 单文件凭据持久化已接入；热点随机凭据重置仍未实现 |

### 2.2 未冻结项

- 媒体上传究竟使用 HTTP 还是 HTTPS。局域网联调可先 HTTP，正式外网交付仍以 TLS
  证书校验为安全基线，不能将开发期 HTTP 默认为最终方案。
- 统一上传、结束、事件状态查询、结果拉取和 ACK 的确切 URL、HTTP 方法与错误码。
- 强烈情绪的枚举、颜色值、变化去抖阈值、事件序号和大模型建议就绪通知字段。
- 社交最终 TTS 是云端直接返回二进制、返回受控下载 URL，还是设备按最终文本另调 TTS。
- 长会话中是否每几十秒返回一次阶段摘要。会议要求文档保留设计，但演示首版先不实现。

## 3. 按键定义

### 3.1 物理映射

| 语义键 | 板级按键 | GPIO | 统一名称 |
| --- | --- | --- | --- |
| 确认/取消 | Power | 12 | `VS_KEY_CONFIRM` |
| 返回 | Volume Up | 13 | `VS_KEY_BACK` |
| 下一个 | Volume Down | 8 | `VS_KEY_NEXT` |

### 3.2 识别参数

- 扫描周期：10 ms。
- 稳定消抖：连续 30 ms 状态一致。
- 短按：稳定按下至少 `CONFIG_VS_SHORT_PRESS_MIN_MS`（当前 80 ms）且不超过
  `CONFIG_VS_SHORT_PRESS_MAX_MS`（当前 500 ms）后释放；超过上限按取消处理。
- 长按：稳定按下达到 2000 ms，只发送一次 `LONG_PRESS`；释放不再补发短按。
- 组合长按：返回和下一个均稳定按下后开始计时，持续 2000 ms；当前代码没有独立的 150 ms 错峰窗口。
- 组合键成立后吞掉两个单键事件，防止同时触发返回或翻页。
- Power 和返回键的长按进度按 4% 粒度上报；组合键使用独立的 `VS_INPUT_COMBO_PROGRESS`，提前释放上报
  `VS_INPUT_COMBO_CANCEL`，达到阈值上报 `VS_INPUT_NET_TOGGLE`。

阈值最终放入 Kconfig，以上数值作为首版默认值。按键 worker 只产生事件，不直接改变
App 状态。

### 3.3 各界面行为

| 状态 | 确认短按 | 确认长按 | 返回短按 | 返回长按 | 下一个短按 | 返回+下一个长按 |
| --- | --- | --- | --- | --- | --- | --- |
| 历史索引 | 进入 `VOICE_LISTENING` 占位页 | 显示进度，满圈后进入社交占位页 | 保持历史页 | 保持历史页 | 下一条静态记录 | STA/AP 切换请求 |
| 历史空白页 | 返回历史页 | 短按拍照并进入多模态问答 | 返回上一条记录 | 返回上一条记录 | 循环回第一条记录 | STA/AP 切换请求 |
| 语音聆听 | 手动截断并提交 | 无操作 | 立即取消 | 立即取消 | 无操作 | 禁止模式切换 |
| 语音识别/思考 | 无操作 | 无操作 | 立即取消在途请求 | 立即取消在途请求 | 无操作 | 禁止模式切换 |
| TTS 播放 | 停止播报并返回 | 无操作 | 立即取消播放并返回 | 立即取消播放并返回 | 无操作 | 禁止模式切换 |
| 社交采集中 | 暂停采集 | 无操作 | 无操作 | 结束并等待云结果 | 切换提示页 | 禁止模式切换 |
| 社交暂停 | 继续 | 无操作 | 无操作 | 结束并等待云结果 | 切换提示页 | 禁止模式切换 |
| 等待最终结果 | 无操作 | 无操作 | 无操作 | 可二次长按取消等待并保存 incomplete | 无操作 | 禁止模式切换 |
| SoftAP | 无操作 | 当前保持 SoftAP（凭据重置未接入） | 保持 SoftAP | 显示同样环形进度，长按 2 s 后返回 STA | 无操作 | 禁止模式切换 |
| 错误提示 | 确认重试 | 无操作 | 返回历史索引 | 无操作 | 查看下一条错误信息 | 禁止模式切换 |

## 4. 状态机

### 4.1 顶层状态

```text
BOOT
  -> HISTORY_STA
      -> VOICE_LISTENING
      -> VOICE_THINKING
      -> VOICE_SPEAKING
      -> HISTORY_STA
      -> SOCIAL_ENTER
      -> SOCIAL_RUNNING <-> SOCIAL_PAUSED
      -> SOCIAL_EXITING
      -> HISTORY_STA
      -> NET_SWITCHING -> SOFTAP | ERROR
   any operational state -> ERROR -> HISTORY_STA
```

- `BOOT`：初始化按键、双屏、存储索引和 STA 状态；任何单项失败都形成可显示错误，
  不因一块屏或一条历史损坏而崩溃。
- `HISTORY_STA`：默认状态。当前代码使用 `vs_app.c` 中的三条静态记录；轻量索引、完整 JSON
  读取和持久化尚未接入。
- `VOICE_LISTENING`、`VOICE_THINKING` 和 `VOICE_SPEAKING`：当前仍是 VelaSight 页面状态
  占位。`ai_agent` 库侧已经编译接入音频采集、RMS VAD、ASR/TTS 和本地 Agent client；
  VelaSight 顶层的上下文白名单、异步回答和取消生命周期尚未接入 `vs_app.c`。
- `VOICE_THINKING` 和 `VOICE_SPEAKING`：当前仅保留页面枚举，尚未实现问题提交、回答显示或 TTS。
- `SOCIAL_ENTER`：显示 Power 长按进度；短按释放进入 `VOICE_LISTENING`，长按达到 2 s 进入
  `SOCIAL_RUNNING`，提前释放不会启动社交业务。
- `SOCIAL_RUNNING`、`SOCIAL_PAUSED` 和 `SOCIAL_EXITING`：当前为本地 UI 占位状态；Power 短按
  在运行/暂停间切换，返回键长按显示退出环形动画并回到历史页，媒体和云端会话尚未接入。
- `NET_SWITCHING`：组合键进入 AP 或 AP 页面返回 STA 时显示环形进度。提前释放组合键回到历史页；
  AP 返回键提前释放保持 SoftAP，达到 2 s 后调用 `vs_network_request_mode()`。
- AP 切换失败进入 `ERROR` 后，确认短按重试，返回短按回历史；错误页不显示长按进度。
- `SOFTAP`：由 `vs_network_apply_ap()` 配置 `wlan0`、IPv4 和 DHCP，并启动配网 Web
  服务；双屏显示热点名称、密码和网页地址。保存成功后 App 主循环加载 NAND 文件并切回
  STA。热点随机凭据重置仍未实现。

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
- 社交异常情绪即时事件与最终结果必须分成两类事件：即时事件只更新颜色/短文本/拉取标志，
  最终结果事件才允许写历史和启动最终 TTS。

## 5. 双圆屏交互设计

圆屏边角不可用，核心内容放在中心直径约 136 px 的安全区。首版不显示滚动长文，
不使用细线、复杂图例或需要精确触控的元素。

### 5.1 历史记录索引

物理左屏为内容屏，固定对应 `/dev/fb1`：

- 只显示当前记录标题、摘要和日期，不显示历史索引和进度动画。
- 底部显示当前有效软键名称；短按后显示响应态。
- 最后一条记录之后固定进入一张空白页，提示单张照片多模态 AI 问答；空白页不计入记录总数。
- 有记录时短按确认进入当前记录语音助手，长按确认进入社交辅助。

物理右屏为状态屏，固定对应 `/dev/fb0`：

- 显示模式、当前索引、完成状态、情绪状态和动画。
- 历史页显示 `01/03` 等索引；空白页显示“照片问答”，不伪装成历史记录。
- 长按进入、结束和网络切换时只显示贴边进度环，并隐藏全部软键名称。
- 情绪比例、图表和图例属于记录详情的后续显示能力，不得与左屏摘要重复。

### 5.2 社交模式进入与运行

进入进度期间：

- 右侧状态屏显示贴近外环的进度条；长按阶段隐藏软键名称。
- 左侧内容屏显示“进入社交辅助 / 继续按住，松开取消”。
- 长按满圈后当前只进入 `SOCIAL_RUNNING` 占位页；尚未生成 `session_id` 或启动 Camera/Audio。
  运行中短按 Power 暂停/继续，返回键长按显示退出动画并返回历史页。

运行期间：

- 左屏显示云端最新短建议；没有可靠线索时显示“继续交流”。
- 右屏显示社交状态、情绪名称、连接状态和数据新鲜度。
- 普通情绪识别结果不强制逐帧显示；只有云端标记 `emotion_changed`/强烈情绪时，
  才改变颜色并显示短文本。设备可依据标志主动拉取一次异常详情，避免高频返回大包。
- 异常详情优先显示颜色和短文本，不在会话中途播放 TTS，避免打断对话；云端大模型建议
  可以稍后作为结构化文本到达，但演示主输出留给会话结束后的最终 TTS。
- 极端情绪只改变背景强调色和边框，不闪烁，不使用全屏快速反色。
- 极端情绪建议至少连续多个有效窗口且置信度过阈值后才显示，并设置冷却时间，
  防止屏幕颜色频繁跳变。
- 暂停时两屏加明显暂停符号，保留最后内容但降低饱和度。

### 5.3 闲时语音助手

目标设计：语音助手占用两屏期间不再显示可操作的历史索引。当前代码只实现页面占位，以下为后续闭环要求：

- `/dev/fb0` 显示大号状态词：准备、聆听、识别、思考、播报、取消或失败。
- `VOICE_LISTENING` 时显示低频更新的音量环和“Power 结束 / 返回取消”；音量环仅为
  反馈，不显示精确 dB，刷新不高于 10 Hz。
- 后续记录回顾路径的 `/dev/fb1` 可显示当前记录短标题；当前代码不显示照片预览、Prompt、
  回答摘要或 TTS 状态。
- 后续实现应在识别/思考阶段只显示阶段词，不显示完整字幕、Prompt 或历史正文。
- 当前没有独立取消语音请求或迟到回答的实现；失败页只由网络切换失败路径使用。

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

### 5.4 Finalizing 与结果返回（后续实现）

- 左屏显示“正在整理记录”“正在生成建议”“正在保存记录”等用户可读阶段。
- 右屏显示分段等待状态；不把网络上传字节数伪装成用户进度。
- 成功落盘后，将新记录插入索引首位并选中，然后回到历史界面。
- 超时或 Schema 非法时不显示旧结果；记录标为 incomplete，并显示可重试错误码。

### 5.5 SoftAP 界面

目标态下物理右屏 `/dev/fb0` 显示 SSID，物理左屏 `/dev/fb1` 显示密码。SSID 和密码按固定宽度换行，
默认密码可在 30 秒后遮蔽；长按确认生成随机凭据后，先持久化，再重启 SoftAP。

当前 SoftAP 已通过 `vs_network_apply_ap()` 请求真实模式、地址和 DHCP；如果底层请求失败，进入
`VS_PAGE_ERROR`。页面在右屏显示 SSID，左屏显示密码和网页地址；配网页面保存目标 STA
凭据到 `/mnt/sdnand/prov/wifi.bin`，成功响应关闭后自动退出 AP 并切回 STA。

## 6. 数据结构与本地存储

社交协议沿用 `app_feature.md` 的 `social-session/v1`、`social-alert/v1`、
`social-finalize/v1` 和 `social-cue/v1`。闲时助手沿用 `voice-assistant/v1` 和
`voice-review/v1`，并增加空白页通用多模态请求 `voice-vision/v1`。设备内部使用定长字段
和显式长度，禁止将网络输入直接当作 NUL 结尾字符串。

### 6.1 社交会话设备侧协议抽象

会议倾向图片和音频共用媒体上传入口，但云端尚未冻结实际 URL。App 不直接散落 HTTP
路径，而是只依赖以下抽象操作：

```c
int vs_cloud_social_open(const struct vs_session_open *request);
int vs_cloud_social_upload(const struct vs_media_packet *packet);
int vs_cloud_social_poll_event(const char *session_id,
                               uint64_t after_event_sequence);
int vs_cloud_social_finalize(const struct vs_finalize_request *request);
int vs_cloud_social_get_result(const char *session_id,
                               const char *result_id);
int vs_cloud_social_ack(const struct vs_result_ack *ack);
```

设备生成 `session_id` 时使用 TRNG 随机数和设备短 ID，不只使用可能重复或尚未校时的时间戳。
云端创建接口负责接受或拒绝该 ID，不能静默替换后不给设备返回确认。所有媒体包至少包含：

```json
{
  "schema_version": "social-media/v1",
  "session_id": "vs-264-a1b2c3d4",
  "media_type": "image/jpeg | audio/pcm",
  "sequence": 1,
  "timestamp_ms": 0,
  "payload_length": 32768,
  "image": {"width": 640, "height": 480},
  "audio": {
    "sample_rate": 16000,
    "sample_bits": 16,
    "channels": 1,
    "duration_ms": 2000
  }
}
```

上例是元数据契约，不表示把二进制塞入 JSON。JPEG/PCM 仍以原始二进制正文、multipart
或云端最终规定的零拷贝友好方式发送，不使用 Base64。`sequence` 可按媒体类型分别递增，
但字段定义必须由接口文档明确；重传保持原序号，云端按 `session_id + media_type + sequence`
去重。

设备侧社交流程固定为：

```text
历史页长按 Power 满圈
  -> 生成 session_id
  -> open/握手确认
  -> Camera 每得到一张可信 JPEG 即 upload(image)
  -> Audio 每累计 2 s 即 upload(audio)
  -> 每次上传 ACK 或低频 poll 检查 event_available/emotion_changed
  -> 有强烈情绪时拉取短事件详情
  -> 长按返回：停止采集、上传尾包、finalize
  -> 轮询/拉取最终 JSON 与 TTS
  -> 校验并本地保存处理后记录
  -> ACK stored
```

### 6.2 强烈情绪两阶段返回

会议要求兼顾即时性和建议质量。云端检测到强烈情绪变化后采用两阶段语义：

1. **即时事件**：尽快返回 `emotion_changed=true`、情绪枚举、颜色枚举/值、置信度、
   时间戳和极短文本。设备立即改变屏幕色彩并显示短文本，不播放 TTS。
2. **增强建议**：云端可把截至当前的字幕和情绪时间轴交给大模型，随后返回关联同一
   `event_id` 的短建议。增强建议晚到不阻塞图片和音频上传，也不覆盖更新的事件。

建议事件最小结构如下，最终字段由云端接口文档冻结：

```json
{
  "schema_version": "social-event/v1",
  "session_id": "vs-264-a1b2c3d4",
  "event_id": "evt-7",
  "event_sequence": 7,
  "status": "immediate | advice_ready",
  "emotion_changed": true,
  "emotion": "anger",
  "confidence": 0.86,
  "color": "red",
  "display_text": "情绪明显升高",
  "suggestion": "可以放慢语速并给对方留出回应时间。",
  "timestamp_ms": 8400
}
```

设备不直接接受云端提供的任意 RGB565 作为唯一语义。首版优先接收受控 `color` 枚举并映射
到本地无障碍调色板；即便保留 RGB 字段，也必须同时有情绪和文字，防止颜色成为不可校验
的业务指令。

### 6.3 最终结果与本地历史

最终结果除标题、正文、摘要、建议和情绪比例外，必须返回云端处理后的原始结构化记录：

- 图片侧：每个有效时间点的情绪、置信度、时间戳及无效原因；不返回原始图片。
- 音频侧：带起止时间的 ASR 字幕；不返回原始 PCM。
- 联合侧：按时间对齐后的 timeline、最终摘要和社交建议。
- 演示输出：最终 TTS 音频或受控下载描述；TTS 播放可取消，不能按 C 字符串长度读取。

设备保存这些处理后字段供历史饼图和闲时回顾使用。完整写入并校验前不得 ACK `stored`；
云端收到 ACK 后删除原始图片、PCM 和会话中间缓存。本地不保存社交原始音视频。

### 6.4 长会话扩展（首版不实现）

演示主路径依靠用户在约 20 至 30 秒后明确结束。未来长会话可以每 20 至 30 秒生成一次
阶段摘要或按字幕完整句边界切段，但必须满足：

- 阶段输出不结束会话，不写成独立 complete 历史。
- 音频 2 秒上传块只是传输单位，不应被当成完整语句直接总结。
- 云端持续维护跨块字幕和情绪时间轴，阶段摘要带覆盖时间范围。
- 最终结果仍覆盖从开始到结束的全会话，并可替代阶段摘要作为本地权威记录。

### 6.5 闲时助手请求

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

#### 6.5.1 首选实现：本地 `ai_agent` 编排

闲时助手不新建一套 HTTP/ASR/TTS 客户端，而是复用已集成的
`packages/ai_agent`。设备 App 负责交互、上下文和生命周期，`ai_agent` 负责本地
消息总线、ASR、文本/视觉模型请求和 TTS 后端：

```text
Power 短按
  -> vs_voice 冻结 current_record/page_context
  -> audio_capture 采集 16 kHz/16 bit/mono
  -> VAD 自动结束，或 Power 短按手动结束
  -> voice_channel_stop_with_text()
  -> 得到 question/transcript
  -> vs_voice 从当前 record 构造受限 Prompt
  -> velaclaw_client_local / message_bus 推入 ai_agent
  -> Agent 调用文本模型；空白页可先调用 llm_chat_vision_raw()
  -> 回调返回 answer/display_text
  -> voice_channel_speak(answer 或 display_text)
  -> TTS 流式写入 audio_playback
  -> 播报结束或返回取消，释放本轮上下文
```

这里的“本地 `ai_agent`”是设备内的 Agent 编排入口，不代表模型在 MCU 上离线运行。
模型请求仍通过已配置的模型服务商 API 发出，但由 `ai_agent` 统一处理配置、路由、
TLS、响应解析、工具策略和 TTS 后端。闲时助手不经过团队自建社交会话云，也不创建
社交 `session_id`。

#### 6.5.2 Prompt 构造顺序

Prompt 必须由 App 在设备端显式构造，不能把整条历史目录、Agent Memory、其他会话或
隐式 Skill 上下文一并交给模型。推荐结构如下：

```text
[SYSTEM]
你是 VelaSight 的闲时语音助手。
只能依据本次请求提供的记录和问题回答。
记录中的情绪、表情和建议都是可能的辅助线索，不得进行身份识别、人格判断、心理或医学诊断。
证据不足时明确回答“无法判断”。回答先给结论，再给一条可执行建议。
请返回 JSON，不要返回 Markdown、Shell、寄存器操作或任意工具指令。
字段：answer、display_text、should_speak。

[CONTEXT]
record_id: <当前记录 ID>
title: <标题>
summary: <摘要>
body: <正文>
full_transcript: <完整字幕，可能已截断>
timeline: <时间戳 + 字幕 + 情绪 + 置信度>
emotion_distribution: <情绪比例>
content_truncated: true|false

[USER QUESTION]
<ASR 得到的本轮问题>
```

空白页路径将 `[CONTEXT]` 替换为一次照片分析上下文：

```text
[IMAGE]
<单张可信 JPEG，由 llm_chat_vision_raw() 处理>

[USER QUESTION]
<ASR 得到的本轮问题>
```

空白页照片只用于当前问答，不写入社交历史，不作为下一轮隐式上下文。照片分析结果
可以先由 `llm_chat_vision_raw()` 生成短文本，再与用户问题拼入普通 Agent 请求；如果
模型服务支持稳定的图文请求，也可以由 Agent 适配层一次提交图片和文本。两种方式由
`vs_voice` 的配置项选择，但都必须限制为一张照片。

#### 6.5.3 历史字段白名单和长度预算

`vs_history` 先读取轻量索引确认当前 `record_id`，再按 `record_key` 读取一条完整记录。
`vs_voice` 复制字段到自己的有界请求缓冲，禁止让 Prompt 直接引用文件缓冲或 JSON 解析
树中的指针。只允许以下字段进入闲时助手：

| 字段 | 用途 | 处理规则 |
| --- | --- | --- |
| `record_id` | 关联当前记录 | 必须与页面选中项一致，不接受模型或网络修改 |
| `title` | 记录主题 | 最多 96 B |
| `summary` | 首选简短证据 | 最多 192 B |
| `body` | 人类可读正文 | 最多 2 KiB，超限设置截断标志 |
| `full_transcript` | 回答语音内容问题 | 最多 8 KiB，优先保留与问题相关的时间段 |
| `timeline` | 对齐情绪和字幕 | 最多 64 条，每条有起止时间、情绪和置信度 |
| `emotion_distribution` | 饼图和比例问题 | 最多 8 类，比例必须经过本地范围校验 |
| `content_truncated` | 告知模型上下文不完整 | 任一字段裁剪时强制置为 `true` |

总 Prompt 预算由 `CONFIG_VS_VOICE_PROMPT_MAX_BYTES` 控制，建议首版不超过 16 KiB，
以免与 Agent 的 8 KiB context buffer、LLM 请求缓冲和 TTS/显示内存叠加。超限裁剪顺序：

```text
保留 system prompt + title + summary
  -> 保留 body
  -> 按问题关键词/时间范围筛选 timeline 和 transcript
  -> 保留最后一段 transcript
  -> 设置 content_truncated=true
```

模型收到的是脱敏后的处理结果，不包含 API Key、Wi-Fi 密码、系统日志、原始 JPEG、
原始 PCM、其他 `record_id` 或 Agent 的长期 Memory。

#### 6.5.4 `ai_agent` 接入层

首选使用 `packages/ai_agent/include/velaclaw/client.h` 提供的本地异步客户端：

```c
velaclaw_client_t *client = velaclaw_client_open("velasight_voice");

velaclaw_ask_req_t request = {
  .text = prompt,
  .timeout_ms = CONFIG_VS_VOICE_AGENT_TIMEOUT_MS,
};

velaclaw_ask(client, &request, vs_voice_agent_reply, request_cookie);
```

该客户端通过 `message_bus_push_inbound()` 将请求送入 Agent，并通过
`mbus_tap_register()` 接收异步回答，避免 App 自己重复实现模型 HTTP 协议。回调函数
不得直接刷新屏幕或播放音频，只转换为 `VS_EVENT_VOICE_ANSWER` 投递给 App 事件队列。

当前源码集成状态必须作为实现门禁处理：

- `velaclaw_client_local.c` 已存在，但当前只加入传统 Makefile，没有加入
  `packages/ai_agent/CMakeLists.txt`。
- BK7258 实际采用 CMake 构建，因此接入 VelaSight 前必须把该源文件加入 CMake 的
  `AGENT_SRCS`，并确认最终 `System.map` 出现 `velaclaw_client_open` 和 `velaclaw_ask`。
- `timeout_ms` 当前 API 只有字段定义，客户端实现尚未真正执行超时控制。
- 首版必须增加 request generation/request ID、取消标志、迟到回调隔离和单请求互斥；
  否则上一轮回答可能在用户取消后污染下一轮语音助手。
- `velaclaw_client_close()` 只能在回调注销、在途请求取消或超时回收后调用，不能在
  App 返回键回调中直接释放仍被 Agent worker 使用的 cookie。

如果短期不能修改公共 `packages/ai_agent`，可以在 `vs_voice.c` 内直接调用已有
`message_bus_push_inbound()`，但这只是临时适配；正式方案仍应把稳定接口补到 Agent SDK，
并同步 CMake 和 Make 两套源文件列表。

#### 6.5.5 语音回答接收和 TTS 播放

Agent 回调收到回答后按以下顺序处理：

1. 有界解析 JSON；`answer` 用于完整回答，`display_text` 用于两块屏幕的一至两行摘要，
   `should_speak=false` 时只显示不播报。
2. 拒绝空回答、超过长度上限的回答、非法 UTF-8 和包含 Shell/Tool 指令的内容。
3. App 将 `display_text` 写入不可变显示快照，索引屏显示“播报”，概略屏显示摘要。
4. 若允许播报，调用 `voice_channel_speak(answer)`；该接口内部复用流式 TTS 和
   `audio_playback_write()`，不要求把完整音频一次装入 SRAM。
5. 播报期间返回键设置取消 generation，调用 `audio_playback_stop()`，关闭下载和 DAC，
   清空本轮 TTS 缓冲；不能继续播放上一次缓存。
6. 播报结束后释放 Prompt、ASR 文本、模型回答、照片和临时 JSON，恢复触发前的历史页。

当前 Agent 已编译 `voice_channel_start/stop/stop_with_text/speak` 和 ASR/TTS 源码，
但 BK7258 产物仍需实板确认真实 `media_recorder`/`media_player` backend，而不是 weak
stub。首版验收必须包含“录音 -> ASR -> Prompt -> Agent -> TTS -> 扬声器”的完整闭环。

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

当前 CMake 实际编译 `velasight_main.c`、`vs_app.c`、`vs_display.c`、`vs_input.c`、
`vs_config.c` 和 `vs_network.c`。`vs_history.c`、`vs_social.c`、`vs_voice.c`、`vs_media.c`、
`vs_cloud.c` 和 `vs_web_stub.c` 虽已作为接口/占位文件保留，但未加入当前 CMake 目标。

| 源码文件 | 对应功能与实现边界 |
| --- | --- |
| `velasight_main.c` | NSH/builtin 入口；解析仅供调试的启动参数，初始化上下文并运行 App；不承载业务状态 |
| `vs_app.c/.h` | 顶层状态机、事件队列、资源生命周期和状态快照；是唯一可修改顶层状态的模块 |
| `vs_types.h` | 状态、事件、错误码、记录索引、情绪分布、显示快照等定长公共类型 |
| `vs_input.c/.h` | 读取 `board_buttons()`，完成消抖、短按、长按、组合长按和进度事件生成 |
| `vs_display.c/.h` | 打开物理右屏 `/dev/fb0` 和物理左屏 `/dev/fb1`，实现 RGB565 framebuffer、圆屏布局、ASCII 文字、进度环和整帧提交；饼图/图例尚未实现 |
| `vs_history.c/.h` | 轻量索引分页、按需读取完整 JSON、完整记录先于索引的事务写入、启动恢复与损坏隔离 |
| `vs_social.c/.h` | 社交业务子状态、开始/暂停/继续/finalize/取消编排，以及实时提示和最终记录归一化 |
| `vs_voice.c/.h` | 闲时助手子状态、当前记录/空白页上下文冻结、轻量 VAD、白名单 Prompt 拼接、本地 `ai_agent` 请求、手动截断、ASR/Agent/TTS 编排和全阶段取消 |
| `vs_media.c/.h` | Camera/Audio 的启动停止、有界队列、时间戳和序号；音频优先、图片丢旧帧策略 |
| `vs_cloud.c/.h` | 自建社交会话云适配；开发期 HTTP/正式 TLS、请求序号、响应限长、Schema 校验、事件/结果拉取和 ACK |
| `vs_network.c/.h` | STA/AP 互斥切换、WAPI 模式配置、IPv4/DHCP、配网 Web 服务生命周期和保存事件处理 |
| `vs_config.c/.h` | 等待 SD-NAND 挂载，优先从配网单文件读取 STA 凭据，无记录时使用 Kconfig 默认值 |
| `app/provisioning_web` | AP 配网 HTTP 表单、输入校验、NAND 单文件记录、CRC、保存回调和独立调试命令 |

### 7.1 构建文件计划

- `Kconfig`：总开关、主线程栈、worker 栈、事件队列深度、按键阈值、协议长度、
  设备节点、Prompt 总长度、Agent 超时、VAD 参数、语音回答上限和 mock 开关。
- `CMakeLists.txt`：使用 `nuttx_add_application(NAME velasight ...)`，当前仅列出已接入的六个源文件。
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

当前 `vs_network_request_mode()` 已实现 STA/AP 请求；AP 路径使用 Kconfig 中的 SSID、密码和信道，
并启动 NuttX DHCP 服务。凭据随机化、持久化和 Web Server 仍属于后续工作。

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

### 9.4 两条 AI 网络链路

设备必须区分两条逻辑和配置均独立的网络链路：

```text
社交辅助：vs_social -> vs_cloud -> 团队自建会话云 -> 表情/ASR/文本模型/TTS
闲时助手：vs_voice -> 本地 ai_agent -> 模型服务商 ASR/通用模型/TTS API
```

- 社交链路只处理社交会话、媒体块、异常事件和最终记录，云端负责缓存和拼接。
- 闲时链路不调用自建会话云，不创建社交 `session_id`，不把问答写成社交记录。
- 两条链路可使用不同域名、鉴权和超时，不能把自建云 Token 传给模型服务商，反之亦然。
- “本地 Agent”表示编排逻辑在设备，并不表示模型离线运行；UI 和文档不得显示成端侧推理。
- 两条链路共享 `wlan0`、TLS 内存和 Audio/Camera 时，由 App 状态机保证互斥。

### 9.5 团队接口与责任边界

| 参与方 | 负责内容 | 向设备 App 交付 |
| --- | --- | --- |
| 设备 App | 按键/UI、设备 `session_id`、Camera/Audio 分块、上传、拉取、记录保存和 TTS 播放 | 本文状态机、抽象接口实现、实板日志 |
| 自建会话云 | 会话缓存、媒体接收、音频拼接、表情识别、ASR、时间轴、异常事件、最终模型编排和缓存清理 | 接口文档、Schema、错误码、mock server/固定样例包 |
| 表情/算法 | 单帧表情、置信度、无效原因和强烈情绪判定 | 稳定枚举、阈值建议、输入尺寸和性能数据 |
| 模型服务 | 文本建议、摘要和 TTS；闲时助手的 ASR/通用多模态/TTS | 确切端点、鉴权、格式、长度限制和错误语义 |
| Web 负责人 | SoftAP 下配置和完整记录读取服务 | 后续独立接口；不修改 App 内部状态和媒体所有权 |

云端接口文档至少要在联调前提供：

1. `open/upload/event/finalize/result/ack` 六类操作的 URL、方法、Header、正文格式和返回码。
2. 图片与音频是否共用入口，以及二进制正文和元数据如何同时传递。
3. 每类 sequence 的作用域、幂等键、重传规则、最大包长、超时和限流。
4. 强烈情绪即时事件与增强建议的关联方式，以及设备是从上传 ACK 得到标志还是低频 poll。
5. 最终结果 JSON Schema、TTS 交付方式和设备 ACK 后的云端删除时机。
6. 一组不依赖真实模型的固定 mock：普通会话、强烈情绪、延迟建议、非法包、超时和最终成功。

## 10. 实现阶段与技术路线

### 阶段 0：门禁与契约冻结

- 确认三个按键的最终丝印映射、长按阈值和组合键人体工学。
- 冻结云端实时提示和最终结果 JSON Schema、长度上限、错误码和超时。
- 冻结 ASR、通用 Agent、记录回顾和 TTS 的端点、鉴权、音频/图片格式、取消语义及响应长度；
  现有 MiMo 网络计划已指出 ASR/TTS 端点仍需从平台控制台确认，该项是首版阻断项。
- 决定可写持久存储方案，记录挂载点、容量、原子 rename/fsync 行为。
- 固化可信 3 FPS 演示路线，并将修复硬件 JPEG 或其他可达 5 FPS 的路线作为独立挑战项。

完成判据：阻断项均有负责人、接口和可测方案，不以 mock 代替硬件结论。

### 阶段 1：离线 UI 与按键闭环

- 添加 Kconfig、CMake、Make 文件和最小 `velasight` 入口。
- 实现事件队列、顶层状态机、按键识别和 mock 历史数据。
- 实现双屏索引、摘要、长按进度、暂停和错误页；饼图与颜色图例仍属后续 UI 工作。
- 实现语音助手各阶段状态页、音量环和全阶段取消的本地 mock。
- AP 组合键进入 `VS_PAGE_NET_SWITCHING`，完成后进入 `VS_PAGE_SOFTAP`；失败进入 `VS_PAGE_ERROR`。

当前实现进展：`app/velasight` 已接入 CMake/Make 和 manifest，使用 LVGL NuttX
framebuffer backend 打开 `/dev/fb0`、`/dev/fb1`。产品配置跳过板级问候动画和 idle 页面，
`board_early_initialize()` 先显式拉低 GPIO25，面板初始化期间继续保持背光关闭；双 framebuffer 注册完成后先各 push 一帧 RGB565 纯黑保护帧，再由板级弱钩子创建独立 `velasight` 任务；LVGL 创建双屏组件树后分别执行同步首帧刷新，两次 `FBIO_UPDATE` 完成后通过板级 reveal 接口开启背光并接管两屏；
Camera、Audio、JPEG、Bluetooth、KVDB 和网络继续在后台初始化。首版已实现历史索引、摘要占位、语音/社交
  状态占位、错误页和按键扫描；单键和组合长按默认均为 2000 ms，短按窗口为
  80..500 ms。显示已迁移到 LVGL NuttX framebuffer backend，使用 16 px
  SimHei 16 px、4 bpp 字体覆盖完整 `U+4E00-U+9FFF` CJK Unified Ideographs
  基本区和 ASCII；启用 `LV_FONT_FMT_TXT_LARGE` 支持完整字形索引，不再依赖旧的
  1166 字 SimSun 稀疏子集。

实板第一轮 UI 反馈后，显示职责收敛为：物理右屏 `fb0` 只显示大号历史序号和 `HISTORY` 状态，
物理左屏 `fb1` 只显示当前历史记录的短标题、日期和完成状态；内容按圆形安全区居中裁剪，不再
在两屏重复显示完整历史字段。Power 短按从历史页进入 `VOICE_LISTENING` 占位页，Power
按住时从首次进度事件进入 `SOCIAL_ENTER`；满 100% 后进入社交占位页，提前释放时取消进入。
返回键在 AP 页面按住时进入 `NET_SWITCHING`，短按或提前释放保持 SoftAP，满 100% 后切回 STA。

SoftAP 页面由 `vs_network` 负责调用 WAPI、IPv4 和 DHCP；服务器、SSID/密码持久化
和随机凭据程序尚未实现。STA/AP 切换由 `vs_network` 负责调用 WAPI、DHCP 客户端/服务端
和接口地址配置，开发默认凭据来自 Kconfig，正式版本必须改为 SoftAP 配置程序提供的
持久化配置，不能继续依赖 Kconfig 密码。

完成判据：无需网络即可连续操作 30 分钟；无重复短按、组合键串扰、屏幕越界或堆增长。

### 阶段 2：历史存储

- 实现轻量索引启动加载、分页和完整记录按需读取。
- 实现 `.tmp -> fsync -> rename -> index` 的事务写入和掉电恢复。
- 注入截断 JSON、坏 UTF-8、超长字段、断电残留 tmp 和索引悬挂项。

完成判据：100 条记录下浏览内存保持有界；任意单条损坏不影响其他记录；重启后记录存在。

### 阶段 3：闲时语音助手闭环

- 基于 `configs/ai_agent` 构建，并将 `velaclaw_client_local.c` 同时加入 Agent 的
  CMake/Make 源列表；已确认 `ai_agent_main`、本地客户端和语音符号进入最终镜像。
- 为 `packages/ai_agent` 的录音、ASR、`llm_chat_vision_raw()`、Agent 消息和
  `voice_channel_speak()` 增加 App 可调用、可取消、长度安全的适配接口。
- 实现“读取当前 `record_key` -> 白名单字段复制 -> Prompt 拼接 -> 本地 Agent 请求
  -> JSON 回答 -> display_text + TTS”的单条记录回顾路径。
- `ai_agent` 已实现 PCM RMS 噪声底校准、800 ms 尾静音和 30 s 最大录音 VAD，并复用
  `audio_capture_*` API；pre-roll、VelaSight Power 手动截断和顶层事件编排仍待接入。
- 实现“当前记录 + 问题”的回顾路径，以及“单张照片 + 问题”的通用多模态路径。
- TTS 使用小块流式播放；返回键在录音、请求、Agent 回调等待和播放阶段均能在有界时间内取消。
- 增加 Agent 请求 generation、迟到回调隔离、单请求互斥和超时回收，禁止旧回答污染新一轮。

完成判据：记录回顾、空白页拍照、VAD 自动截断、Power 手动截断、返回取消和 TTS 播放
均完成真机往返；取消后无 Camera/ADC/DAC/TLS/Agent callback 资源残留；CMake 构建的
最终 `System.map` 能证明本地 SDK 已进入镜像。

### 阶段 4：社交媒体与云端闭环

- 将 `social_cue` 原型中的 Camera、PWM 和拒绝策略经验迁入正式模块。
- 接入 16 kHz PCM 2 秒分块、可信 JPEG 约 3 FPS、有界队列和社交云抽象接口；局域网
  HTTP 仅用于前期联调，正式外网版本恢复 TLS 证书校验。
- 实现实时 `social-alert/v1` 更新、短按暂停/继续、长按结束、finalize 轮询和 ACK。
- 实现强烈情绪“即时颜色/短文本 -> 关联大模型建议”的两阶段事件，普通时段保持静默。
- 最终结果先校验再显示和播放 TTS，先完整记录再写索引，ACK 后才允许云端清理。

完成判据：正常、暂停恢复、网络拥塞、无人脸、Schema 非法、云端超时和用户结束路径均实板通过。

### 阶段 5：SoftAP App 接口接入

- 在当前基础上补齐凭据显示/重置、持久化和 Web 服务，并继续验证切换失败回滚。
- 实现凭据显示、TRNG 随机重置、AP 返回 STA，以及切换期间共享资源清理。
- 不实现 Web Server，只为 Web 所有者提供稳定接口。

完成判据：STA/AP 可运行时互斥切换且无需复位；失败能回滚 STA；UI 与真实热点状态一致。

### 阶段 6：资源、异常与演示验收

- 运行长会话、快速按键、弱网、断网、存储满、单屏失败和低内存压力测试。
- 检查音频连续性、图片丢帧策略、显示刷新耗时、线程栈余量、堆峰值和文件恢复。
- 使用 `ai_agent` 配置完成构建、执行 AP/CP 打包、镜像哈希一致和实板回归；该配置当前不能使用 `-Werror`，
  因为上游 `packages/ai_agent` 仍有告警。

## 11. 测试矩阵

| 类别 | 必测场景 | 通过条件 |
| --- | --- | --- |
| 按键 | 抖动、快速短按、按住、两键错峰、三键同时按 | 每次只产生规定事件，无幽灵短按 |
| 历史 | 0/1/100 条、坏索引、坏记录、超长 UTF-8 | 页面可用，内存有界，坏记录隔离 |
| 显示 | 最长标题、最大百分比、双屏一块打开失败 | 文本不越安全区；单屏失败不阻塞业务 |
| VAD | 安静、恒定风噪、突发噪声、远讲、连续说话、无语音、30 s 上限 | 不吞首字，不把稳定噪声当语音，不无限录音 |
| 语音回顾 | 每个历史子页触发、超长记录、记录读取失败 | 只上传触发时选中的一条记录，截断有标志 |
| 多模态助手 | 空白页触发、拍照失败、照片上传失败 | 仅一张照片；失败不退化为上传其他历史或旧照片 |
| Prompt/Agent | 正常记录、字段截断、空记录、重复问题、Agent 超时、迟到回调 | 仅白名单字段入 Prompt；超时/取消后的回答被丢弃 |
| TTS | 正常播放、分块中断、返回取消、Power 停止 | 按长度播放；停止后 DAC 和下载均关闭，不播放旧缓存 |
| 社交 | 开始、暂停、恢复、长按结束、finalize 成功 | Camera/Audio 生命周期和云序号正确 |
| 社交采样 | 3 FPS 图片逐张上传、2 s PCM 分块、尾包不足 2 s | 时间戳单调，尾包不丢，网络阻塞不拖死 Audio |
| 异常情绪 | 无变化、连续变化、即时事件、建议晚到/乱序 | 常态静默；颜色和短文本及时；旧建议不覆盖新事件 |
| 最终结果 | 字幕、情绪/置信度时间轴、摘要、建议、TTS | 只保存处理后记录；TTS 可播放和取消；ACK 后清理 |
| 云端 | 401/409/413/429/5xx、TLS 失败、非法 JSON、超时 | 明确错误，不展示旧数据，不泄漏缓冲 |
| 媒体 | 图片队列满、音频压力、取消时有在途包 | 丢旧图保音频，取消后资源全部释放 |
| 存储 | 写满、掉电 tmp、rename 失败、ACK 前重启 | 不出现指向不完整 JSON 的 complete 索引 |
| AP | STA/AP 切换、启动失败、提前松开、返回键短按/长按 | 非错误页长按 2 s 后切换；错误页只支持短按重试/返回 |
| 隐私 | 取消、超时、最终 ACK、日志检查 | 原始媒体离开会话即释放，日志无密钥/正文 |

## 12. 构建与验收命令

修改 Kconfig 后先 clean，再按项目门禁构建：

```bash
cd /home/mi/vela_competition/contest
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake distclean
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake -j8
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
- 社交演示至少以可信约 3 FPS 图片逐张上传和 2 秒音频块运行约 20 至 30 秒；长按返回后
  收到完整字幕、情绪/置信度时间轴、摘要、建议和最终 TTS。
- 强烈情绪即时事件使用受控颜色、情绪枚举和短文本，中途不播放 TTS；关联建议晚到时
  不阻塞媒体上传，也不覆盖更新的情绪事件。
- 历史记录写入掉电保持存储，完整 JSON 先于轻量索引，重启恢复通过。
- 极端情绪需满足持续和置信度门槛，颜色之外始终有文字标签。
- 取消、断网、超时、非法响应和存储失败都停止采集并释放资源。
- SoftAP 基础切换已实现；凭据显示/重置、Web Server 和持久化仍未完成，UI 不能宣称这些能力已完成。
- Web Server 保持未实现并明确移交，不计入 MCU App 完成范围。
- `ai_agent` 配置构建、最终固件打包、哈希核对和实板测试记录全部通过；上游告警清理后再增加 `-Werror` 门禁。
