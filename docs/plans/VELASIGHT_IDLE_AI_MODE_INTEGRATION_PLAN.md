# VelaSight 闲时 AI 模式接入实施方案

> 文档版本：V1
>
> 文档状态：2026-08-21 设计基线，**业务层尚未实现**。`app/velasight/vs_voice.c`、
> `vs_media.c`、`vs_history.c` 及对应头文件当前为 **0 字节空文件**，均未加入
> `CMakeLists.txt` 的 `SRCS`。`include/vs_app.h` 已定义 15 个异步事件，但全仓库
> 除网络 worker 外**没有任何地方调用 `vs_app_post_event()`**，因此
> `VS_APP_EVENT_PHOTO_READY` / `VOICE_REPLY` 等事件永远没有生产者。本方案是把
> 胶水层写进这些空文件，**不改动 `vs_app.c` 的页面状态机**。
>
> 适用范围：BK7258 AP/CPU1 上 `app/velasight` 的「闲时语音助手」两条子路径 ——
> 历史记录页短按 Power 的**单条记录问答**，与空白页短按 Power 的**单张照片多模态
> 问答**。社交辅助模式见
> [VELASIGHT_SOCIAL_MODE_INTEGRATION_PLAN.md](VELASIGHT_SOCIAL_MODE_INTEGRATION_PLAN.md)。
>
> 交互与显示口径以
> [VELASIGHT_UI_DESIGN_INSTRUCTION.md](VELASIGHT_UI_DESIGN_INSTRUCTION.md) 为准；
> 工程状态与阶段门禁以
> [VELASIGHT_MCU_APP_DEVELOPMENT_PLAN.md](VELASIGHT_MCU_APP_DEVELOPMENT_PLAN.md)
> 为准。发生冲突时以本文的取证结论为当前事实。
>
> 本文所说「闲时 AI 模式」等价于开发计划中的「闲时语音助手」。它**不经过团队自建
> 社交会话云**，不创建社交 `session_id`，不写社交历史。「本地 Agent」指编排逻辑在
> 设备侧，**不表示模型在 MCU 上离线推理**。

## 0. 使用规则

### 0.1 路径

工作区根目录：

```text
/home/mi/vela_competition
```

正式代码仓库：

```text
/home/mi/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
```

OpenVela/NuttX 源码：

```text
/home/mi/vela_competition/contest/nuttx
```

`ai_agent` 包（本方案依赖的 ASR/TTS/LLM 实现）：

```text
/home/mi/vela_competition/contest/apps/packages/ai_agent
```

本文中相对路径若不带前缀，均相对正式代码仓库根目录。

### 0.2 安全与纪律规则

1. **UI 与按键热路径禁止读写 SD-NAND。** `vs_snapshot()` 已有明确注释禁止此行为，
   当前存储路径可能产生接近秒级阻塞。所有持久化读取必须在启动期或 worker 线程完成
   后经事件回投。
2. **在 P0 的 8.3 文件名门禁通过之前，禁止假定任何 `/mnt/sdnand` 路径可写。**
   见第 2.4 节。
3. **禁止把网络或模型返回的字节直接当 NUL 结尾字符串使用。** 所有外部输入按显式
   长度处理，进入 `struct vs_ui_snapshot_s` 前必须复制到定长缓冲。
4. **禁止在 Agent 回调或 worker 线程内直接刷屏或播放音频。** 回调只允许转换为
   `struct vs_app_event_s` 投递给 App 事件队列。
5. **单请求互斥。** 闲时助手与社交模式互斥占用 Camera、ADC、DAC 与云请求上下文；
   同一时刻只允许一个在途语音请求。
6. **取消必须在有界时间内完成。** 录音、ASR、模型等待、TTS 播报四个阶段都要能被
   返回键短按取消，取消后不得接受迟到回答。
7. **照片与 PCM 只属于当前这一轮。** 完成、取消或超时后立即释放，不写入历史记录，
   不作为下一轮的隐式上下文。
8. 禁止在日志、URL、历史 JSON 或普通配置文件中出现 API Key、Wi-Fi 密码或用户
   语音正文。
9. 所有阶段提交必须记录构建目录、`.config`、`nuttx.bin` 体积增量与 sha256。

## 1. 目标与当前状态

### 1.1 目标架构

```text
Power 短按（VS_INPUT_SHORT / VS_KEY_CONFIRM）
        |
        v
vs_app.c 页面状态机（已实现，本方案不改）
   vs_begin_request() -> request_id
   VS_PAGE_HISTORY       -> photo_context=false -> VS_PAGE_VOICE_LISTENING
   VS_PAGE_HISTORY_BLANK -> photo_context=true  -> VS_PAGE_PHOTO_CAPTURE
        |
        v
vs_voice.c（新增）voice worker pthread，持 request_id
        |
        +-- 空白页路径：vs_media_capture_jpeg() 拍一张即关
        |       -> VS_APP_EVENT_PHOTO_READY -> 主循环切 VOICE_LISTENING
        |
        +-- 历史路径：vs_history_read_one() 读一条 -> 白名单复制 -> 预算裁剪
        |
        v
audio_capture + voice_vad（ai_agent 已编入）
   300..500 ms 噪声底 -> 20 ms RMS 窗 -> 迟滞阈值 -> 800 ms 尾静音自动截断
   或 Power 短按手动截断；30 s 上限
        |
        v
voice_channel_stop_with_text()  ->  question 文本（火山 ASR）
        |
        v
   +-- 历史路径：  llm_chat(prompt, resp, cap)
   +-- 空白页路径：llm_chat_vision_raw(prompt, jpeg, len, "image/jpeg", resp, cap)
        |
        v
有界 JSON 解析 -> answer / display_text / should_speak
        |
        v
vs_app_post_event({VOICE_REPLY, request_id, text})
        |
        v
主循环 vs_handle_app_event()（已实现）
   request_id 门禁 -> VS_PAGE_VOICE_SPEAKING
        |
        v
voice_channel_speak(answer) -> volc_tts 流式 -> audio_playback
```

关键点：**从 `vs_voice` 到模型是直连 `llm_chat_*`，不经过 `agent_loop`**。理由见
第 3.1 节，这是本方案的核心决策。

### 1.2 当前基线（逐项取证）

截至 2026-08-21：

| 项目 | 当前状态 | 证据 |
|---|---|---|
| `vs_voice.c` / `vs_voice.h` | 0 字节空文件 | `ls -la app/velasight/` 与 `include/` |
| `vs_media.c` / `vs_media.h` | 0 字节空文件 | 同上 |
| `vs_history.c` / `vs_history.h` | 0 字节空文件 | 同上 |
| CMake 源列表 | 仅 6 个源文件 + 字体 | `app/velasight/CMakeLists.txt` 的 `SRCS` |
| Make 源列表 | 与 CMake 一致 | `app/velasight/Makefile` 的 `CSRCS` |
| 异步事件契约 | 已定义，未被使用 | `include/vs_app.h` `enum vs_app_event_e` 15 项 |
| 事件生产者 | 仅网络 worker | `vs_network_start_worker()` 是唯一调用 `vs_app_post_event()` 处 |
| 语音页面状态机 | 已实现 | `vs_app.c` 中 `VS_PAGE_VOICE_LISTENING/THINKING/SPEAKING`、`VS_PAGE_PHOTO_CAPTURE` 的 snapshot 与事件分支齐全 |
| `request_id` 门禁 | 已实现 | `vs_begin_request()` / `vs_cancel_request()` / `vs_handle_app_event()` 首段校验 |
| 页面等待校验 | 已实现 | 每个事件分支都校验「当前页是否在等该事件」 |
| 历史数据 | 3 条静态假数据 | `vs_app.c` 的 `g_history[]` |
| `velaclaw_client` | 打开后从未使用 | `vs_app_run()` 中 `velaclaw_client_open("velasight")` 与 `velaclaw_client_close()`，中间无任何调用 |
| `velaclaw_client_local.c` 构建 | **已进 CMake 与 Make** | `packages/ai_agent/CMakeLists.txt:72`、`Makefile:127`、`Makefile:161` |
| ASR/TTS 源码 | **已编入镜像** | `packages/ai_agent/CMakeLists.txt` 含 `volc_asr.c` `volc_tts.c` `volc_tts_ws.c` `voice_asr.c` `voice_tts.c` `voice_channel.c` `voice_vad.c` `audio_capture.c` `audio_playback.c` |
| ASR/TTS 实板 | **未实测** | `使用说明-camera-display-ai_agent.md` 第六节命令表无 ASR/TTS 条目 |
| 火山 ASR/TTS 凭据 | 未配置 | `cmd_set_volc_asr()` 存在（`set_volc_asr <app_id> <token> <cluster>`），无配置记录 |
| MiMo 文本/视觉 | TLS 握手通过，缺真 key | `使用说明-camera-display-ai_agent.md` 6.2 节 |
| 模型名 | 必须 `mimo-v2.5` | 同上；内置两个预设已于 2026-06-30 下线 |
| 单帧 JPEG | 可出，约 316 ms/张 | 同上；copy 27 ms + 软件编码 289 ms |
| V4L2 单帧序列 | 已有两处可复用实现 | `app/agent_camera/agent_camera_main.c`、`app/social_cue/social_cue_main.c` 的 `sc_capture()` |
| 硬件 JPEG M2M 封装 | 已有 | `app/camera_preview/preview_jpeg.h` |
| `/dev/urandom` | 可用 | 生成配置 `CONFIG_DEV_URANDOM=y`、`CONFIG_DEV_URANDOM_RANDOM_POOL=y`；`bk7258_trng.c` 已注册 `/dev/random` |
| PSRAM 分配 | 可用 | `arch/chip/bk7258_psram.h` 的 `bk7258_psram_malloc()`，`audio_test_ogg.c` 已在用 |
| `mbedtls_base64_encode` | 可用 | `llm_vision.c`、`volc_asr.c` 等已在用 |
| SD-NAND 长文件名 | **不可用** | 生成配置 `# CONFIG_FAT_LFN is not set`；`/mnt/sdnand` 为 `vfat` |
| `ai_agent_main()` 自启动 | **不存在** | 全仓库 `board/` 下 grep `ai_agent_main`/`"ai_agent"` 零命中；`bk7258_bringup.c` 只自动拉起 `velasight_autostart()` |
| `llm_proxy_init/voice_channel_init` 调用点 | **仅在 `ai_agent_main()` 内** | `agent_main.c` 的 Phase 1/3；没有第二个调用点 |

**上一条是本方案最关键的一条基线事实，第 3.5 节单独展开**：`llm_chat()`、
`llm_chat_vision_raw()`、`voice_channel_start/stop_with_text/speak()` 依赖的全局状态
（`s_api_key`、`s_llm_host`、`s_backends_registered` 等）只在 `ai_agent_main()` 跑过
`llm_proxy_init()`/`voice_channel_init()` 之后才被填充。这些函数不是库函数，是某个
必须先运行过的任务留下的状态。

### 1.3 工作区状态记录

在开始任何阶段之前执行并记录：

```bash
cd /home/mi/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
git status --short
git log -1 --oneline
git -C /home/mi/vela_competition/contest/packages/ai_agent log -1 --oneline
```

若 `packages/ai_agent` 有产品所需的本地修改，必须先在真实目标仓实现和测试，再把
完整最终文件复制到比赛仓 `external/packages/ai_agent/` 的相同相对路径，并通过
`external/prepare.sh install` 与只读 `check`。不得以未受管的直接修改或旧 patch
作为可复现构建输入。

## 2. 已取证的可复用能力

本节列出**已在镜像内、可直接调用**的能力，避免重复实现。

### 2.1 语音链路（`packages/ai_agent/src/voice/`）

```c
/* voice_channel.h */
int voice_channel_init(void);
int voice_channel_start(void);
int voice_channel_stop(void);
int voice_channel_stop_with_text(char *text_out, size_t text_cap);
int voice_channel_speak(const char *text);
int voice_channel_test_tts(const char *text, const char *out_path);
int voice_channel_test_asr(const char *pcm_path);
```

- ASR 后端是**火山引擎**（`volc_asr.c`），凭据键
  `AGENT_CFG_KEY_VOLC_APPKEY` / `VOLC_TOKEN` / `VOLC_ASR_CLUSTER`，运行期命令
  `set_volc_asr <app_id> <token> <cluster>`。
- TTS 后端同为火山（`volc_tts.c` / `volc_tts_ws.c`），发音人命令
  `set_volc_speaker <speaker_id>`。
- VAD 已实现噪声底校准、迟滞阈值、800 ms 尾静音与 30 s 上限
  （`voice_vad.c` 的 `voice_vad_init()` / `voice_vad_process()`）。
- `src/stubs.c` 内有同名 weak 实现，真实强符号会覆盖它。**验收必须确认调用到的是
  真实实现而非 stub**，判据是 ASR 返回非空文本、TTS 有实际音频输出。

`stubs.c` 中 `voice_channel_test_tts()` 的 weak 原型只有一个参数，而
`voice_channel.h` 声明两个参数，`cmd_voice.c` 按两个参数调用。这说明 stub 已与真实
接口漂移，是 stub 未被链接的间接证据，但**不能作为门禁**。

### 2.2 模型链路（`packages/ai_agent/src/llm/`）

```c
/* llm_proxy.h */
int llm_chat_vision_raw(const char* prompt,
                        const unsigned char* raw_image, size_t raw_len,
                        const char* mime_type,
                        char* response_buf, size_t buf_size);
int llm_chat_vision(const char* prompt, const char* image_b64,
                    const char* mime_type,
                    char* response_buf, size_t buf_size);
int llm_set_vision_model(const char* host, const char* model,
                         const char* api_key);
```

`llm_chat_vision_raw()` 内部一次完成 base64 编码与 JSON 组装，调用返回后即可释放
原始图片，是空白页路径的首选入口。`llm_vision.c` 拼的正是
`data:%s;base64,%s`，与 MiMo《图片理解》要求一致；`llm_proxy.c` 已把
`xiaomimimo.com` 判为 OpenAI 兼容并自动使用 `max_completion_tokens`。

### 2.3 相机单帧

两条已验证路径，本方案选前者：

| 路径 | 特征 | 适用 |
|---|---|---|
| `open /dev/video0` → `S_FMT JPEG 480x480` → `REQBUFS/QUERYBUF/mmap` → `QBUF/STREAMON` → `poll` → `DQBUF` → 关闭 | 一次约 316 ms，独占期短 | **闲时助手（一次一张）** |
| 常驻持有 `/dev/video0` 预览 + `/dev/video1` M2M 编码 | 吞吐高但独占相机 | 社交模式 |

`app/social_cue/social_cue_main.c` 的 `sc_capture()` 是可直接移植的最小实现：
`SC_WIDTH/SC_HEIGHT = 480`、`SC_SIZEIMAGE = 160 KiB`、`SC_NBUFFERS = 2`、
`poll` 超时 5 s。驱动 `bk7258_gc2145_find_mode()` 是精确匹配，**只支持
480x480 / 640x480 / 864x480**，其他几何一律失败。

### 2.4 存储与 8.3 命名约束

`/mnt/sdnand` 由 `bk7258_mmcsd.c` 以 `nx_mount(source, "/mnt/sdnand", "vfat", 0, NULL)`
挂载，而生成配置中 `# CONFIG_FAT_LFN is not set`。NuttX 的 `fat_parsesfname()`
（`nuttx/fs/fat/fs_fat32dirent.c`）在名字超过 8 字符或扩展名超过 3 字符时走
`errout` 返回 `-EINVAL`。

由此推导出的受影响路径（**P0 必须实测确认**）：

| 路径 | 名/扩展 | 推导结论 |
|---|---|---|
| `/mnt/sdnand/ai_agent/config/config.json` | 6 / **4** | 打不开 |
| `/mnt/sdnand/ai_agent/sessions/tg_velasight.jsonl` | **12** / **5** | 打不开 |
| `/mnt/sdnand/ai_agent/memory/MEMORY.md` | 6 / 2 | 可用 |
| `/mnt/sdnand/ai_agent/memory/2026-08-21.md` | **10** / 2 | 打不开 |
| `/mnt/sdnand/ai_agent/skills/social-cue-assistant.md` | 超长 | 打不开 |

`使用说明-camera-display-ai_agent.md` 把
`[cfgstore] Config store ready at .../config/config.json` 记为实测通过，但
`config_store_init()` 只执行 `mkdirs()` 后**无条件打印该行并 `return OK`**，从不打开
文件；真正的写入在 `save_json()` 的 `open(AGENT_CONFIG_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0600)`。
因此那条日志不能作为「文件可写」的判据。

`conv` 应用为此专门使用 8.3 命名（`C%04u.TXT`、`INDEX.TXT`、`LLM.JSN`），
`velasight_provisioning` 使用 `vela.cfg`。本方案的所有新增路径必须遵守同一约定。

### 2.5 事件与请求门禁（`vs_app.c`，已实现）

```c
/* vs_app.h */
int      vs_app_post_event(const struct vs_app_event_s *event);
uint32_t vs_app_current_request_id(void);

struct vs_app_event_s
{
  enum vs_app_event_e type;
  uint32_t            request_id;
  enum vs_emotion_e   emotion;
  uint32_t            color;
  int                 error;
  char                text[VS_TEXT_LONG];   /* VS_TEXT_LONG = 128 */
};
```

已实现的语义，worker 必须依赖而不是重造：

- 队列深度 8，满时返回 `-EAGAIN`；主循环每帧只消费一个事件。
- `vs_handle_app_event()` 对 `type < VS_APP_EVENT_NETWORK_READY` 的事件校验
  `request_id == runtime->active_request_id`，不匹配直接丢弃。
- 每个事件分支再校验当前页面是否在等待该事件，防止乱序。
- `event.text` 只有 **128 字节**。完整回答不能走这个字段，见第 4.3 节。

## 3. 核心决策

### 3.1 决策一：不使用 `velaclaw_ask()`，`vs_voice` 直连 `llm_chat_*`

`packages/ai_agent/src/sdk/velaclaw_client_local.c` 存在三个与本产品约束冲突的
设计，且都不在 App 侧可修复的范围内：

**（1）`chat_id` 固定，会注入不可控的隐式上下文。**
`velaclaw_ask()` 把 `msg.chat_id` 设为 `velaclaw_client_open()` 传入的 `name`，
`vs_app.c` 传的是 `"velasight"`。于是整台设备共用一个 `chat_id`，`agent_loop` 会按
`AGENT_AI_AGENT_MAX_HISTORY = 10` 把最近 **10 条消息**（user/assistant 各算一条，
约 5 轮）拼入请求，`context_builder.c` 还会额外注入 `MEMORY.md` 全文、最近 3 天
笔记与 skill 摘要。UI 设计文档 2.2 节明确要求「只携带当前选中的一条记录，不带历史
目录、其他会话或隐式长期上下文」，开发计划 6.5.3 节还定了字段白名单。走
`velaclaw_ask()` 无法满足。

**（2）回复与请求无关联标识，迟到回答会污染下一轮，且 App 侧门禁挡不住。**
`tap_callback()` 中 `c->async_cb` 是**单个字段**，协议里没有 request id。序列：

```text
第 1 轮 velaclaw_ask() -> async_cb = cb1
用户按返回取消
第 2 轮 velaclaw_ask() -> async_cb = cb2   （cb1 被静默覆盖）
第 1 轮的回答到达 -> tap_callback() 以旧 content 调用 cb2 与新 cookie
```

因为 cookie 属于新请求，`vs_handle_app_event()` 的 `request_id` 校验会**通过**，
屏幕上显示的是上一轮的答案。这正是开发计划 6.5.4 节警告的「旧回答污染新一轮」，
且只在 `vs_voice` 侧加 generation 无法解决。

**（3）`timeout_ms` 完全未实现。** `velaclaw_ask()` 从不读取该字段，push 完即返回；
Agent 不回时 `async_cb` 永久挂起、cookie 永不释放。

附带限制：`mbus_tap_register()` 用固定字符串 `"local_client"` 注册，同一时刻只能有
一个 tap，voice 与 social 无法各持一个客户端；`g_client_instance` 是无引用计数的
单例，`velaclaw_client_close()` 会直接 `free` 掉其他持有者仍在用的指针。

**决策**：`vs_voice.c` 直接调用 `llm_chat()` / `llm_chat_vision_raw()`，Prompt 完全
由 App 构造，generation 与超时由 `vs_voice` 自己管理。`vs_app.c` 中的
`velaclaw_client_open/close` 一并移除，避免留一个打开却不用的单例。

若后续要把稳定接口补回 Agent SDK，最小要求是：`agent_msg_t` 增加请求 id 字段、
tap 回调按 id 分发、`timeout_ms` 生效、支持按 `chat_id` 关闭历史注入。这属于
`packages/ai_agent` 的独立工作，不作为本方案的前置。

### 3.2 决策二：先做空白页路径，再做历史路径

空白页路径（单张照片 + 问题）不依赖历史持久化，用现有 3 条静态记录即可完成端到端
演示；历史路径依赖 `vs_history` 落盘。因此实施顺序固定为 P2 空白页 → P4 持久化 →
P5 历史问答，不得颠倒。

### 3.3 决策三：8 KB 响应截断是上游缺陷，`vs_voice` 侧的响应缓冲改不了它

`llm_http_direct()` 只做 `resp_buf_init(rb, AGENT_LLM_STREAM_BUF_SIZE)`（8 KB），
随后把 `rb->data` 原样交给 TLS 层，TLS 层写到 `resp_cap - 1` 即停止读取，**不增长**。
`AGENT_LLM_MAX_RESP_SIZE`（512 KB）只在 CONNECT 代理路径生效。超限表现是
`cJSON_Parse` 失败并打印 `Failed to parse API JSON, %zu bytes`，看不出是长度问题。

**这条链路上 `vs_voice` 传给 `llm_chat_vision_raw()` 的 `response_buf/buf_size`
管不到这个 8 KB 上限**，两者是两个不同的缓冲：`buf_size` 只控制
`extract_text()` 把已经解析出来的 `choices[0].message.content` 拷给调用方时的
截断长度（`llm_proxy.c` 里一个 `memcpy` 长度钳制），而实际被 8 KB 截断的是
`llm_http_direct()` 内部的 `rb->data`，这一步发生在 `extract_text()` 执行之前 ——
一旦响应超过 8 KB，`cJSON_Parse(rb.data)` 直接失败，`extract_text()` 根本不会被
调用。`vs_voice` 把自己的 `response_buf` 开到 32 KiB、64 KiB 甚至更大，对这个
截断没有任何影响，因为截断发生的位置不在 `vs_voice` 能触达的参数范围内。

**决策**：这是 `packages/ai_agent` 的缺陷，不是 `vs_voice` 能在自己的调用点上
绕开的问题。处置方式是向 `packages/ai_agent` 提一个独立改动 —— 把
`llm_http_direct()` 的 `raw_cap` 从固定 8 KB 改成可增长（复用 `resp_buf_append()`
已有的倍增逻辑，CONNECT 代理路径已经这么做了，直连路径抄一遍即可），上限仍卡在
`AGENT_LLM_MAX_RESP_SIZE`（512 KB）。**这个改动是 P0/P1 的阻塞依赖，不是可以推迟
的优化项**：闲时助手的视觉问答一旦触发超过 8 KB 的模型响应（长回答、带推理内容
的模型都容易触发），在改动落地前会必然失败且失败原因不可见。`vs_voice` 侧仍然
应该按 `CONFIG_VS_VOICE_RESP_MAX_BYTES`（默认 32 KiB）分配自己的 `response_buf`，
但这只是防止"模型答案本身很长"时 `extract_text()` 的二次截断，与修复 8 KB 网络
读取截断是两件独立的事,前者是本方案的常规配置项,后者必须在 P0 清单里作为对
`packages/ai_agent` 的前置依赖单独列出并跟踪状态。

### 3.4 决策四：请求非流式，UI 用阶段词而非进度百分比

`llm_proxy.c` 的请求体不带 `stream:true`，`tls_read_response()` 按 Content-Length 或
chunked 读完整 body 后才返回。因此「思考」阶段没有可信的百分比。UI 设计文档 6 节
已要求「等待云端使用低频分段状态，不伪造百分比」，实现时右屏只显示
`聆听 / 识别 / 思考 / 播报` 阶段词与不高于 4 Hz 的等待动画。

### 3.5 决策五：`vs_voice` 必须自己触发 `ai_agent` 的初始化序列，不能假设它已经跑过

决策一去掉的是 `velaclaw_ask()` 的会话耦合问题（固定 `chat_id`、回调无 request id、
`timeout_ms` 不生效）。它**不会**让 `vs_voice` 摆脱对 `ai_agent` 子系统的依赖 ——
`llm_chat_vision_raw()` 读取的 `s_api_key`/`s_llm_host`（`llm_proxy.c` 静态变量）和
`voice_channel_start()` 依赖的 `s_backends_registered`（`voice_channel.c` 静态变量）
只在 `llm_proxy_init()`/`voice_channel_init()` 跑过之后才有值，这两个函数只在
`ai_agent_main()` 内部被调用（`agent_main.c` Phase 1/3），全仓库没有第二个调用点。

`ai_agent_main()` 是一个 NSH builtin：人在控制台输入 `ai_agent` 才会执行，跑到
"AI Agent ready" 之后堵在 `while (!g_shutdown_requested) sleep(1);`。`board/` 下没有
任何地方自动拉起它——`bk7258_bringup.c` 只自动拉起 `velasight_autostart()`。这意味着
**在没有人手动敲过 `ai_agent` 的一次开机上，闲时助手会在每一次调用上原地失败**：
`llm_chat*()` 因为 `s_api_key[0] == '\0'` 直接返回 `"Error: No API key configured"`；
`voice_asr_stream_open()` 因为 `s_active` 未注册直接返回 `NULL`。都不是崩溃，是干净的
静默失败——不写日志到用户可见的地方，行为上就是"闲时助手永远答不上来"。

因为本产品是 `CONFIG_BUILD_FLAT=y`（已在 `.config` 确认），`ai_agent` 和 `velasight`
在同一地址空间，静态变量互相可见，所以这不是"进程隔离导致调不到"的问题，是"没人
调用初始化函数"的问题。两种可选修法，二选一，必须在 P1 之前定下来，因为它决定
`vs_voice` 的启动顺序：

1. **board 侧自启动**：`bk7258_bringup.c` 在 `velasight_autostart()` 之后（或之前，
   顺序需要与网络初始化的既有时序一起评估）也 `task_create()` 一次 `ai_agent_main`，
   让它作为常驻服务跑在后台。风险是 `ai_agent_main()` 的 Phase 4/6 会注册 NSH 命令
   并起一个读 `stdin` 的 `cli_thread`，产品固件是否需要这个交互式 shell 需要确认；
   不需要的话这两个 phase 要跳过，不能整段照搬。
2. **`vs_voice` 自己调初始化子集**：不启动 `ai_agent_main()` 整个任务，只在
   `vs_app_run()` 启动时依次调用 `config_store_init()` → `message_bus_init()` →
   `llm_proxy_init()` → `llm_router_init()` → `voice_channel_init()`，跳过
   `nsh_commands_init()`/`nsh_commands_start()`（不需要交互式 shell）和网络/BLE/
   Feishu 等与本产品无关的 Phase 5 分支。风险是这条初始化顺序目前只在
   `agent_main.c` 里存在，拆出子集意味着以后 `agent_main.c` 改了初始化顺序容易
   漏改这边。

**决策**：采用方案 2，在 `app/velasight/vs_voice.c` 的 `vs_voice_open()`（新增，
在 `vs_app_run()` 里 `velaclaw_client_open` 原来的位置调用）里执行上述四个 init
调用，理由是产品不需要交互式 NSH shell 和方案 1 会带来的 phase 5 网络/BLE 分支。
这一步是 **P1 的第一个任务**，不是可选项；P0 里"跑 `ai_agent` 敲 `set_llm` 再 `ask`"
验证的是凭据和链路本身可用，不能替代"闲时助手在没人敲过 `ai_agent` 的一次开机上
能不能工作"这条验证，P1 门禁必须新增一条：**重启后不手动进入 `ai_agent` shell，
直接从历史页触发一次问答，验证不是 `"Error: No API key configured"`**。

## 4. 接口契约

### 4.1 新增头文件

`app/velasight/include/vs_voice.h`：

```c
enum vs_voice_ctx_e
{
  VS_VOICE_CTX_RECORD = 0,   /* 历史记录问答，携带一条 record */
  VS_VOICE_CTX_PHOTO         /* 空白页问答，携带一张 JPEG */
};

struct vs_voice_request_s
{
  enum vs_voice_ctx_e ctx;
  uint32_t            request_id;   /* 来自 vs_begin_request() */
  char                record_key[16];  /* CTX_RECORD 有效，8.3 安全 */
};

/* 启动一轮语音请求。非阻塞：内部创建 worker 并立即返回。
 * 同一时刻只允许一个在途请求，重复调用返回 -EBUSY。 */
int  vs_voice_start(const struct vs_voice_request_s *request);

/* 请求取消当前在途请求。有界时间内生效，不阻塞调用方。
 * 生效后 worker 不再投递任何事件。 */
void vs_voice_cancel(void);

/* 手动截断录音（Power 短按）。仅在录音阶段有效，其他阶段返回 -EINVAL。 */
int  vs_voice_stop_recording(void);

/* 停止 TTS 播报并关闭 DAC。可重复调用。 */
void vs_voice_stop_speaking(void);

/* 释放模块资源，可重复调用。 */
void vs_voice_close(void);
```

`app/velasight/include/vs_media.h`（本方案只需要单帧拍照，其余留给社交模式）：

```c
struct vs_media_frame_s
{
  unsigned char *data;    /* 调用方负责 vs_media_frame_release() */
  size_t         len;
  uint16_t       width;
  uint16_t       height;
};

/* 拍一张 JPEG 后立即关闭 /dev/video0。阻塞约 316 ms。
 * 返回 0 并填充 frame，或负 errno。 */
int  vs_media_capture_jpeg(struct vs_media_frame_s *frame,
                           uint16_t width, uint16_t height);
void vs_media_frame_release(struct vs_media_frame_s *frame);
```

`app/velasight/include/vs_history.h`：见第 5.6 节，P4 阶段定稿。

### 4.2 Prompt 契约

历史路径与空白页路径共用同一段 system prompt，`[CONTEXT]` 段按上下文替换。

```text
[SYSTEM]
你是 VelaSight 的闲时语音助手。
只能依据本次请求提供的记录和问题回答。
记录中的情绪、表情和建议都是可能的辅助线索，不得进行身份识别、人格判断、
心理或医学诊断。
证据不足时明确回答“无法判断”。回答先给结论，再给一条可执行建议。
请返回 JSON，不要返回 Markdown、Shell、寄存器操作或任意工具指令。
字段：answer、display_text、should_speak。
```

历史路径的 `[CONTEXT]` 只允许以下字段进入，逐项复制到 `vs_voice` 自己的有界缓冲，
**禁止让 Prompt 引用文件缓冲或 JSON 解析树中的指针**：

| 字段 | 上限 | 处理规则 |
|---|---|---|
| `record_id` | 16 B | 必须与页面选中项一致，不接受模型或网络修改 |
| `title` | 96 B | 超限截断并置 `content_truncated` |
| `summary` | 192 B | 同上 |
| `body` | 2 KiB | 同上 |
| `full_transcript` | 8 KiB | 优先保留与问题相关的时间段 |
| `timeline` | 64 条 | 每条含起止时间、情绪、置信度 |
| `emotion_distribution` | 8 类 | 比例必须经本地范围校验 |
| `content_truncated` | bool | 任一字段裁剪时强制置 `true` |

裁剪顺序固定：

```text
保留 system prompt + title + summary
  -> 保留 body
  -> 按问题关键词/时间范围筛选 timeline 与 transcript
  -> 保留最后一段 transcript
  -> 设置 content_truncated=true
```

总预算由 `CONFIG_VS_VOICE_PROMPT_MAX_BYTES` 控制，默认 16 KiB。

空白页路径的 `[CONTEXT]` 替换为：

```text
[IMAGE]
<单张 480x480 JPEG，由 llm_chat_vision_raw() 完成 base64 与 JSON 组装>

[USER QUESTION]
<ASR 得到的本轮问题>
```

**只允许一张照片。** 照片不写入历史，不作为下一轮隐式上下文。

### 4.3 回答契约与 128 字节事件字段

模型返回按以下顺序处理：

1. 有界解析 JSON，取 `answer`、`display_text`、`should_speak`。
2. 拒绝空回答、超长回答、非法 UTF-8、含 Shell/Tool 指令的内容。
3. `display_text` 截到 `VS_TEXT_LONG - 1 = 127` 字节后写入
   `struct vs_app_event_s` 的 `text`，投 `VS_APP_EVENT_VOICE_REPLY`。
   **截断必须落在 UTF-8 字符边界上**，不得产生半个汉字。
4. 完整 `answer` 留在 `vs_voice` 自己的缓冲中，由 `voice_channel_speak()` 消费，
   **不经过事件队列**。
5. `should_speak == false` 时只显示不播报，直接从 `VOICE_SPEAKING` 页面按用户按键
   返回。

`vs_app.c` 现有实现对空 `text` 已有处理：`VS_APP_EVENT_VOICE_REPLY` 收到空文本时会
`vs_cancel_request()` 并报「AI未返回回答」。因此第 2 步的拒绝路径应改投
`VS_APP_EVENT_VOICE_FAILED` 并带具体 errno，而不是投空文本。

### 4.4 阶段与页面对应

| `vs_voice` 内部阶段 | 页面 | 有效按键 |
|---|---|---|
| 拍照（仅空白页） | `VS_PAGE_PHOTO_CAPTURE` | 返回取消 |
| 录音 | `VS_PAGE_VOICE_LISTENING` | Power 手动截断、返回取消 |
| ASR + 模型 | `VS_PAGE_VOICE_THINKING` | 返回取消 |
| TTS 播报 | `VS_PAGE_VOICE_SPEAKING` | Power 停止播报、返回取消 |

`vs_app.c` 当前的 `VS_APP_EVENT_PHOTO_READY` 分支会把页面从 `PHOTO_CAPTURE` 切到
`VOICE_LISTENING`，因此空白页路径必须在拍照成功后先投 `PHOTO_READY`，再开始录音。
`VOICE_LISTENING` 到 `VOICE_THINKING` 的切换当前**没有对应事件**，需要在 P1 阶段
确认：要么由 `vs_voice` 在录音结束时新增一个事件，要么复用现有事件。本方案选择
新增 `VS_APP_EVENT_VOICE_LISTENING_DONE`，插入在 `VS_APP_EVENT_VOICE_REPLY` 之前
以保持 `type < VS_APP_EVENT_NETWORK_READY` 的门禁语义。

## 5. 代码落点与实施顺序

### 5.1 目标文件

```text
app/velasight/include/vs_voice.h      填充：第 4.1 节接口
app/velasight/include/vs_media.h      填充：单帧拍照接口
app/velasight/include/vs_history.h    填充：P4 定稿
app/velasight/include/vs_app.h        修改：新增 VOICE_LISTENING_DONE 事件
app/velasight/vs_voice.c              填充：worker、VAD 编排、Prompt、取消
app/velasight/vs_media.c              填充：V4L2 单帧
app/velasight/vs_history.c            填充：8.3 索引与记录事务写入
app/velasight/vs_app.c                修改：调用 vs_voice_start/cancel，移除 velaclaw
app/velasight/CMakeLists.txt          修改：SRCS 增加三个源文件
app/velasight/Makefile                修改：CSRCS 同步
app/velasight/Kconfig                 修改：新增第 6.1 节选项
```

分层边界：`vs_app.c` 是唯一可以修改顶层状态的模块；`vs_voice.c` 只产生事件，
不刷屏、不读 `runtime`；`vs_media.c` 只做设备 I/O，不知道页面；`vs_history.c` 只做
存储事务，不知道 Prompt。

**构建文件注意**：本项目以 `--cmake` 构建，`CMakeLists.txt` 是权威路径；`Makefile`
的 `CSRCS` 必须逐项同步，禁止两套构建遗漏模块。

### 5.2 P0：凭据、实板门禁与 8.3 判定（不写代码）

这一阶段全部是配置与实测，产出结论而不是代码。

**（0）确认 `ai_agent` 自启动缺口，且不要用这一步的验证方式掩盖它。** 本节
(2)-(6) 都要求先手动 `nsh> ai_agent` 进入 shell。这只验证凭据和链路本身可用，
**不验证**闲时助手在产品真实使用场景（重启后没人手动进过 `ai_agent` shell）下
能不能工作 —— 按第 3.5 节的取证，答案是不能，`llm_chat*()` 会静默返回
`"Error: No API key configured"`。这一条不需要现在解决（解决方式是 3.5 节的
决策五，落在 P1），但必须现在记录在验收结论里，避免 P0 通过后被误读为"闲时助手
已经能跑"。

**（1）8.3 文件名判定。** 在板上执行：

```sh
nsh> ls /mnt/sdnand
nsh> echo hi > /mnt/sdnand/ai_agent/config/config.json ; echo $?
nsh> echo hi > /mnt/sdnand/T.TXT ; echo $?
nsh> cat /mnt/sdnand/T.TXT
```

记录两个 `$?`。若长名失败、短名成功，则 2.4 节推导成立，`vs_history` 与所有新增
路径必须用 8.3 命名，同时把 `ai_agent` 配置持久化列为已知缺陷（见第 10 节）。

**（2）凭据配置。** 两家服务都要配，缺一条链路不通：

```sh
nsh> ai_agent
vela> set_llm        api.xiaomimimo.com mimo-v2.5 <mimo-key>
vela> set_vision_llm api.xiaomimimo.com mimo-v2.5 <mimo-key>
vela> set_volc_asr   <volc-app-id> <volc-token> <volc-cluster>
vela> set_volc_speaker <speaker-id>
vela> config_show
```

`config_show` 必须显示 Host / Model / Vision Model 均为 `mimo-v2.5`。若（1）判定
长名不可写，则这些配置**重启即丢**，每次上电都要重配；这一事实必须写进验收记录。

**（3）文本链路验证。**

```sh
vela> net_test
vela> ask 你好
```

期望 `Handshake OK: TLSv1.2` 与非空回答。

**（4）ASR/TTS 实板往返验证（本阶段最关键）。**

```sh
vela> voice_test_tts 你好，我是 VelaSight /mnt/ram/t.pcm
nsh> ls -l /mnt/ram/t.pcm
vela> voice_test_asr /mnt/ram/t.pcm
```

必须同时满足：`t.pcm` 非空且长度与文本长度量级相符；`voice_test_asr` 打印出可读
中文，不是空串、不是 `-1`。返回 `-1` 说明链接到了 `stubs.c` 的 weak 实现，属于阻断
项，必须先解决再进入 P1。

**（5）录音与播放底层确认。**

```sh
nsh> audio_test rec
nsh> audio_test play
nsh> audio_test recplay
```

`rec` 报出的 DC/RMS/peak 必须随环境声变化；`recplay` 必须能听到回放。这一步确认
ADC/DAC 通路本身可用，把 P0(4) 的失败范围收窄到 ai_agent 的 media backend。

**（6）拍照确认。**

```sh
nsh> agent_camera auto out=/mnt/ram/C.JPG
nsh> ls -l /mnt/ram/C.JPG
```

**P0 门禁**：8.3 结论有实测 `$?`；两套凭据已配且 `config_show` 一致；`net_test` 与
`ask` 通过；`voice_test_tts` 与 `voice_test_asr` 均返回真实结果；`audio_test recplay`
可听；`agent_camera` 出图。任一项不通过不得进入 P1，**不得用 mock 代替硬件结论**。

### 5.3 P1：`ai_agent` 初始化、`vs_voice` 骨架、构建接入与取消框架

只搭骨架，模型调用先返回固定文本。**第一个任务是第 3.5 节决策五**，其余任务
在它之后才有意义。

```text
第一步（决策五，阻塞本阶段其余任务）：
  新增 vs_voice_open()，在 vs_app_run() 里 velaclaw_client_open 原来的位置调用：
    config_store_init() -> message_bus_init() -> llm_proxy_init()
      -> llm_router_init() -> voice_channel_init()
  不调用 nsh_commands_init/nsh_commands_start（不需要交互式 shell）
  不调用 Phase 5 的网络/BLE/Feishu 分支（与本产品无关）
  确认这四个 init 函数的返回值都被检查并记录日志，任一失败不阻塞 velasight 启动
  但要在 UI 可达的地方留痕（例如 runtime.api_ready 复用现有字段）

填充 vs_voice.h（第 4.1 节）与 vs_media.h
vs_voice.c 实现：
  - 单请求互斥：static bool g_busy + 互斥锁，重复 start 返回 -EBUSY
  - generation：static uint32_t g_generation，start 时自增并与 request_id 绑定
  - 取消令牌：static volatile bool g_cancel，每个阻塞步骤前后检查
  - worker pthread：8192 栈，detach
  - 自实现超时：CONFIG_VS_VOICE_AGENT_TIMEOUT_MS，用 clock_gettime 差值判定
  - 投递封装：仅当 generation 未变时才调用 vs_app_post_event()
vs_app.h 新增 VS_APP_EVENT_VOICE_LISTENING_DONE，插在 VOICE_REPLY 之前
  注意：这会使该值之后的所有 enum vs_app_event_e 常量的整数值后移一位；
  当前没有其他文件依赖这些整数的具体数值（均通过符号名引用），但改动时要
  重新确认这一点仍然成立
vs_app.c：
  - VS_PAGE_HISTORY_BLANK 的 CONFIRM 短按后调用 vs_voice_start(CTX_PHOTO)
  - VS_PAGE_HISTORY 的 CONFIRM 短按后调用 vs_voice_start(CTX_RECORD)
  - VOICE_LISTENING 的 CONFIRM 短按 -> vs_voice_stop_recording()
  - VOICE_SPEAKING 的 CONFIRM 短按 -> vs_voice_stop_speaking()
  - 任意语音页 BACK 短按 -> vs_voice_cancel() + vs_cancel_request()
  - 处理 VOICE_LISTENING_DONE -> VS_PAGE_VOICE_THINKING
  - 移除 velaclaw_client_open/close
CMakeLists.txt / Makefile 同步加入 vs_voice.c vs_media.c
```

`vs_app_post_event()` 队列深度只有 8 且满时返回 `-EAGAIN`。worker 的投递封装必须
按现有网络 worker 的写法重试：

```c
while (vs_app_post_event(&event) == -EAGAIN)
  usleep(10000);
```

但重试循环中必须检查取消标志，否则取消后仍会阻塞在重试上。

**P1 门禁**：`--cmake` 构建通过且 `CMakeLists.txt` 与 `Makefile` 源列表一致；四个
语音页面可用假回答走通一遍；返回键在每个阶段都能在 500 ms 内回到触发前页面；连续
50 次「进入 → 取消」无内存增长（`heap_info` 前后对比）；`System.map` 中不再出现
`velaclaw_ask`；**新增（决策五）：完整重启一次，不手动进入 `ai_agent` shell、不敲
任何 `set_llm`/`set_volc_asr`，直接从历史页触发一次语音问答，确认走的是 P0(2) 配置
过的凭据而不是失败——若此时仍返回 `"Error: No API key configured"`，说明
`vs_voice_open()` 里的初始化调用没有生效或凭据没有持久化（8.3 结论若显示长名不可写，
凭据本来就重启即丢，这一步会必然失败，属已知限制而非本条门禁的失败，但仍要跑一次
确认失败原因确实是"配置丢失"而不是"初始化没跑"）**。

### 5.4 P2：空白页单张照片问答闭环

```text
vs_media.c：移植 social_cue 的 sc_capture()
  - 480x480 / V4L2_PIX_FMT_JPEG / sizeimage 160 KiB / 2 buffers / poll 5 s
  - 成功后 memcpy 到 PSRAM 缓冲（bk7258_psram_malloc）并立即 close(fd)
  - 校验 JPEG SOI/EOI marker，不合法返回 -EBADMSG
vs_voice.c 空白页路径：
  1. vs_media_capture_jpeg() -> 成功投 PHOTO_READY，失败投 PHOTO_FAILED
  2. voice_channel_start() 起录音；voice_vad 自动截断或 vs_voice_stop_recording()
  3. voice_channel_stop_with_text(question, sizeof(question))
     - 空文本视为“未听清”，投 VOICE_FAILED 带 -ENODATA
  4. 投 VOICE_LISTENING_DONE
  5. 拼 Prompt（第 4.2 节空白页格式）
  6. llm_chat_vision_raw(prompt, frame.data, frame.len, "image/jpeg",
                         resp, CONFIG_VS_VOICE_RESP_MAX_BYTES)
  7. vs_media_frame_release() 立即释放照片
  8. 有界解析 JSON -> display_text（UTF-8 边界截断到 127 B）
  9. 投 VOICE_REPLY
 10. 主循环切 VOICE_SPEAKING 后，vs_voice 调 voice_channel_speak(answer)
 11. 播报结束释放 Prompt / question / answer / resp
```

响应缓冲从 PSRAM 分配，默认 32 KiB。480x480 JPEG 约 44 KB，`llm_chat_vision_raw()`
内部 base64 后约 59 KB 再加 JSON 外壳，峰值约 110 KB；当前 `heap_info` 实测
`arena=6448384 free=6104768`，余量充足，但仍必须记录每阶段的堆峰值。

**P2 门禁**：空白页短按 Power 完成「拍照 → 录音 → ASR → 视觉模型 → 显示 → TTS」
真机往返；拍照失败、ASR 空文本、模型超时三条失败路径分别进入统一错误页且文案可读；
照片在模型调用返回后立即释放（用 `heap_info` 证明）；本轮照片未写入任何文件。

### 5.5 P3：全阶段取消与迟到隔离

```text
四个阶段的取消实现：
  录音   -> voice_channel_stop() + 丢弃 PCM
  ASR    -> 设置 g_cancel，ASR 返回后不投事件
  模型   -> 设置 g_cancel，llm_chat_* 返回后不投事件、立即 free 响应
  播报   -> voice_channel_stop_speaking 语义：audio_playback_stop() + 关 DAC
            + 清空本轮 TTS 缓冲
超时：每个阻塞步骤记录起始时刻，超过 CONFIG_VS_VOICE_AGENT_TIMEOUT_MS 投
      VOICE_FAILED 带 -ETIMEDOUT
```

注意 `llm_chat_*` 是同步阻塞调用，**无法从外部中断**；取消只能做到「返回后丢弃
结果」。因此 UI 上取消是立即生效的（页面立刻回退），但底层 TLS 请求仍会跑完。
这一点必须写进代码注释，避免后续误以为可以在取消后立刻发起下一轮。为此
`vs_voice_start()` 在上一轮 worker 未退出时必须返回 `-EBUSY`，而不是并发两个请求。

**P3 门禁**：录音/ASR/模型/播报四个阶段各取消 20 次，均在 500 ms 内回到触发前页面；
取消后 20 s 内不出现任何迟到回答上屏；取消后 Camera、ADC、DAC 均已释放
（`audio_test rec` 能立即成功即为证据）；连续「取消 → 立即再触发」20 次，第二次
必定得到 `-EBUSY` 提示或正常排队，不得出现两个 worker 并存。

### 5.6 P4：`vs_history` 持久化

路径全部 8.3：

```text
/mnt/sdnand/VSREC/INDEX.TXT      轻量索引，每行一条
/mnt/sdnand/VSREC/R%04u.JSN      完整记录
/mnt/sdnand/VSREC/R%04u.TMP      写入中间态
```

```c
/* vs_history.h */
struct vs_history_index_s
{
  char     record_key[16];    /* "R0001" */
  char     date[16];          /* "08/18 09:20" */
  char     title[VS_TEXT_SHORT];
  char     summary[VS_TEXT_LONG];
  uint8_t  calm, happy, tense;
  bool     incomplete;
  bool     volatile_store;    /* 存储非掉电保持时为 true */
};

int vs_history_open(void);                       /* 启动期调用，可能阻塞 */
int vs_history_count(void);
int vs_history_get_index(int i, struct vs_history_index_s *out);
int vs_history_read_full(const char *record_key, char *buf, size_t len);
int vs_history_append(const struct vs_history_index_s *index,
                      const char *full_json);
void vs_history_close(void);
```

事务顺序固定，不得简化：

```text
写 R%04u.TMP -> fsync -> 关闭 -> 重新打开并校验 -> rename 为 R%04u.JSN
  -> 写 INDEX.TMP -> fsync -> 原子替换 INDEX.TXT
```

`vs_history_open()` 必须复用 `vs_config.c` 已有的等待逻辑：SD-NAND 挂载可能延迟
60 s 以上，`vs_config_wait_for_store()` 轮询 700 × 100 ms。**只能在启动期或 worker
中调用，禁止出现在 `vs_snapshot()` 路径上。**

`vs_app.c` 中的 `g_history[]` 静态数组替换为 `vs_history_get_index()`，
`vs_snapshot()` 只读一份已缓存在内存的索引数组，不触发文件 I/O。

若最终确认存储为非掉电保持，所有记录必须带 `volatile_store = true` 并在 UI 标注
「重启后清除」，**不得声称已完成历史持久化**。

**P4 门禁**：0/1/100 条记录下浏览内存有界；注入截断 JSON、坏 UTF-8、超长字段、
断电残留 `.TMP`、索引悬挂项，均只隔离坏记录不影响其他；重启后记录仍存在；
`vs_snapshot()` 路径上 `strace` 或插桩确认无文件 I/O。

### 5.7 P5：历史记录问答

```text
vs_voice.c 历史路径：
  1. vs_history_read_full(record_key, buf, len)
  2. 白名单字段逐项复制到 vs_voice 自己的有界缓冲（第 4.2 节表格）
  3. 按 CONFIG_VS_VOICE_PROMPT_MAX_BYTES 裁剪，任一字段被裁则置 content_truncated
  4. 录音 + ASR（与空白页路径共用同一段实现）
  5. llm_chat(prompt, resp, CONFIG_VS_VOICE_RESP_MAX_BYTES)
  6. 后续与空白页路径一致
```

上下文在 Power 短按触发瞬间冻结：`vs_app.c` 在调用 `vs_voice_start()` 时把当前
`record_key` 一并传入，之后用户翻页不影响本轮请求。

**P5 门禁**：每条历史记录都能触发问答且只携带该条；超长记录裁剪后
`content_truncated` 为 `true` 且模型回答中体现了「上下文不完整」；记录读取失败进入
错误页而不是退化为发送其他记录；Prompt 中不出现 API Key、Wi-Fi 密码、其他
`record_id`、原始 JPEG 或原始 PCM（用一次 Prompt dump 到串口逐项人工核对，核对后
删除该调试代码）。

### 5.8 P6：资源与长稳验收

```text
连续 30 分钟随机按键操作
弱网、断网、模型 401/429/5xx、TLS 失败、非法 JSON、超时六类注入
存储写满、单屏打开失败
每阶段前后 heap_info 对比，线程栈余量检查
```

**P6 门禁**：见第 7 节验证矩阵全部通过。

## 6. 配置与构建执行清单

### 6.1 Kconfig 新增

在 `app/velasight/Kconfig` 中追加，全部 `depends on
LVX_USE_DEMO_CONTEST2026_264_VELASIGHT`：

```text
config VS_VOICE_PROMPT_MAX_BYTES
	int "VelaSight idle assistant prompt budget (bytes)"
	default 16384
	range 2048 65536

config VS_VOICE_RESP_MAX_BYTES
	int "VelaSight idle assistant model response buffer (bytes)"
	default 32768
	range 8192 131072

config VS_VOICE_AGENT_TIMEOUT_MS
	int "VelaSight idle assistant per-step timeout (ms)"
	default 30000
	range 5000 120000

config VS_VOICE_RECORD_MAX_MS
	int "VelaSight idle assistant maximum recording (ms)"
	default 30000
	range 3000 60000

config VS_VOICE_VAD_TAIL_MS
	int "VelaSight idle assistant trailing silence before auto stop (ms)"
	default 800
	range 200 3000

config VS_VOICE_VAD_PREROLL_MS
	int "VelaSight idle assistant pre-roll kept before speech onset (ms)"
	default 300
	range 0 1000

config VS_VOICE_NO_SPEECH_MS
	int "VelaSight idle assistant give up when no speech (ms)"
	default 5000
	range 1000 20000

config VS_PHOTO_WIDTH
	int "VelaSight still capture width"
	default 480

config VS_PHOTO_HEIGHT
	int "VelaSight still capture height"
	default 480

config VS_VOICE_MOCK
	bool "VelaSight idle assistant local mock (no network)"
	default n
	---help---
		Replace ASR and the model call with fixed local text so the
		state machine and cancel paths can be exercised without
		credentials.  Never enable in a delivered image.
```

`VS_PHOTO_WIDTH/HEIGHT` 的取值受驱动限制：`bk7258_gc2145_find_mode()` 是精确匹配，
只接受 480x480、640x480、864x480。其他值会在 `VIDIOC_S_FMT` 阶段失败。

### 6.2 源码加入

`app/velasight/CMakeLists.txt`：

```cmake
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT)
  nuttx_add_application(
    NAME velasight
     SRCS velasight_main.c vs_app.c vs_display.c vs_input.c vs_config.c
          vs_network.c vs_voice.c vs_media.c vs_history.c
          velasight_font_16_ui.c
     INCLUDE_DIRECTORIES ${NUTTX_APPS_DIR}/packages/ai_agent/include
                          ${NUTTX_APPS_DIR}/packages/ai_agent/src
                          ${NUTTX_APPS_DIR}/packages/demos/contest2026_264_provision_web/include
      DEPENDS lvgl provision_web_core
    STACKSIZE 8192)
endif()
```

`app/velasight/Makefile` 的 `CSRCS` 同步加入 `vs_voice.c vs_media.c vs_history.c`。

`${NUTTX_APPS_DIR}/packages/ai_agent/src` 已在 include 路径中，因此可以直接
`#include "voice/voice_channel.h"` 与 `#include "llm/llm_proxy.h"`。

应用目录通过符号链接接入构建树，`app/velasight` 已有
`packages/demos/contest2026_264_velasight` 链接，本方案不新增应用目录。

### 6.3 构建与打包

改过 `Kconfig` 或 `defconfig` 后必须先 `distclean`：

```bash
cd /home/mi/vela_competition/contest

./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake distclean
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake -j8
```

产物：

```text
contest/cmake_out/bk7258-ap_ai_agent/nuttx
contest/cmake_out/bk7258-ap_ai_agent/nuttx.bin
contest/cmake_out/bk7258-ap_ai_agent/System.map
```

`nsh` 配置不包含 `ai_agent`，因此**本方案的功能无法在 `nsh` 上验证**；`nsh` 只用于
确认改动没有破坏基础启动。CP/AP 组合固件与哈希核对按
`docs/固件构建步骤.md` 第 3 至 5 节。

AP raw linker 区域上限 `3904K`（`boards/bk7258/bk7258-ap/scripts/ld.script`），每次
提交记录 `nuttx.bin` 体积增量。

## 7. 验证矩阵

### 7.1 构建与静态检查

```text
--cmake 构建通过
CMakeLists.txt 与 Makefile 源列表逐项一致
System.map 中存在 vs_voice_start / vs_media_capture_jpeg / vs_history_open
System.map 中存在 llm_chat_vision_raw / voice_channel_stop_with_text
System.map 中不存在 velaclaw_ask（决策一已生效）
CONFIG_VS_VOICE_MOCK=n 的交付镜像中不含 mock 分支
nuttx.bin 体积增量已记录
```

### 7.2 凭据与链路

| 场景 | 通过条件 |
|---|---|
| `config_show` | Host / Model / Vision Model 均为 `mimo-v2.5`，key 只显示前 4 位 |
| `net_test` | `Handshake OK: TLSv1.2` 且 `HTTP Status: 200` |
| `ask 你好` | 返回非空中文回答 |
| `voice_test_tts` | 输出 PCM 文件非空，长度与文本量级相符 |
| `voice_test_asr` | 打印可读中文，不是空串或 `-1` |
| 8.3 判定 | 长名写入的 `$?` 与短名写入的 `$?` 均已记录 |

### 7.3 VAD

| 场景 | 通过条件 |
|---|---|
| 安静环境 | 不把底噪当语音，`VS_VOICE_NO_SPEECH_MS` 后显示「未听清」 |
| 恒定风噪 | 噪声底吸收稳定噪声，不误触发 |
| 突发噪声 | 单次尖峰不触发开始 |
| 立即说话 | 噪声底校准阶段不吞首字（pre-roll 生效） |
| 远讲 | 能触发，或明确失败，不静默挂起 |
| 连续说话 | 到 `VS_VOICE_RECORD_MAX_MS` 强制截断 |
| 短于 300 ms | 不提交 ASR |

### 7.4 空白页问答

| 场景 | 通过条件 |
|---|---|
| 正常路径 | 拍照 → 录音 → ASR → 视觉模型 → 显示 → TTS 真机往返 |
| 只拍一张 | 日志中 `open /dev/video0` 与 `close` 各一次 |
| 拍照失败 | 进统一错误页，不使用旧照片、不退化为历史问答 |
| ASR 空文本 | 显示「未听清」，不提交模型 |
| 模型 4xx/5xx | 明确错误，不展示旧数据 |
| 响应超 32 KiB | 明确报错，不是无提示的 JSON 解析失败 |
| 隐私 | 本轮结束后文件系统中无新增图片或 PCM |

### 7.5 历史问答

| 场景 | 通过条件 |
|---|---|
| 每条记录触发 | 只携带当前选中的一条 |
| 超长记录 | 裁剪后 `content_truncated=true` |
| 空记录 | 明确失败，不发送空 Prompt |
| 读取失败 | 进错误页，不退化为发送其他记录 |
| Prompt 白名单 | 人工核对一次 dump，无 Key、密码、其他 `record_id`、原始媒体 |

### 7.6 取消与迟到

| 场景 | 通过条件 |
|---|---|
| 录音阶段取消 ×20 | 500 ms 内回退，ADC 已释放 |
| ASR 阶段取消 ×20 | 回退，20 s 内无迟到上屏 |
| 模型阶段取消 ×20 | 回退，响应缓冲已释放 |
| 播报阶段取消 ×20 | DAC 关闭，不播放上一次缓存 |
| 取消后立即再触发 ×20 | 不出现两个 worker 并存 |
| Power 停止播报 | 播报立即停止并回到触发前页面 |

### 7.7 存储

```text
0 / 1 / 100 条记录浏览内存有界
截断 JSON、坏 UTF-8、超长字段被隔离
断电残留 .TMP 在下次启动被清理
rename 失败不产生指向不完整 JSON 的 complete 索引
重启后记录仍存在（或全部标注 volatile 并在 UI 提示）
UI 快照路径无文件 I/O
```

### 7.8 资源与长稳

```text
连续 30 分钟随机操作无崩溃、无堆单调增长
每阶段前后 heap_info 差值已记录
线程栈余量：voice worker、主循环、input worker 均有余量记录
双屏刷新不超过 10 FPS，语音期间按键响应无可感延迟
```

## 8. 交付物与回滚

每个阶段提交必须包含：

```text
源代码与 Kconfig 改动
构建命令与最终 .config
nuttx、nuttx.bin、System.map 的 sha256
nuttx.bin 体积增量
实板串口日志（含 heap_info 前后对比）
测试命令、输出与统计计数
失败场景注入方式与恢复结果
```

推荐拆分提交：

```text
docs: add VelaSight idle AI mode integration plan
docs: record idle assistant credential and ASR/TTS bring-up results   （P0）
feat(velasight): add vs_voice skeleton with cancel and timeout        （P1）
feat(velasight): capture one still and answer with the vision model   （P2）
feat(velasight): cancel recording, ASR, model and playback            （P3）
feat(velasight): persist history records on SD-NAND                   （P4）
feat(velasight): answer questions about the selected record           （P5）
test(velasight): idle assistant soak and fault injection results      （P6）
```

出现以下任一情况立即回退到最近一个通过的阶段，**不得用加延时、放宽超时或忽略
错误码掩盖**：取消后仍有迟到回答上屏；照片或 PCM 出现在文件系统中；`heap_info`
在循环操作中单调增长；UI 快照路径出现文件 I/O；Prompt 中出现白名单外字段；
两个 voice worker 并存。

## 9. 完成标准

只有同时满足以下条件，才能称「闲时 AI 模式接入完成」：

1. P0 的六项门禁全部有实板记录，其中 `voice_test_asr` 与 `voice_test_tts` 返回真实
   结果而非 stub 的 `-1`。
2. 8.3 文件名结论已实测判定并记录；若长名不可写，`vs_history` 已按 8.3 实现，且
   `ai_agent` 配置不可持久化这一缺陷已单独立项。
3. `vs_voice.c` / `vs_media.c` / `vs_history.c` 均已进入 CMake 与 Make 两套源列表，
   `System.map` 可证明进入镜像。
4. **`vs_voice_open()` 已在 `velasight` 启动时调用决策五列出的四个 `ai_agent`
   初始化函数**，且 P1 门禁新增的"重启后不手动进 shell"验证已通过。
5. 空白页路径完成「拍照 → 录音 → VAD 自动截断 → ASR → 视觉模型 → display_text →
   TTS」真机往返，且照片只拍一张、结束即释放。
6. 历史路径完成「读一条记录 → 白名单复制 → 预算裁剪 → ASR → 文本模型 → TTS」
   真机往返，只携带当前选中记录。
7. Power 短按可手动截断录音，也可停止播报；返回键在录音、ASR、模型、播报四个阶段
   均可在 500 ms 内取消。
8. 取消后无 Camera / ADC / DAC / TLS / callback 资源残留，且迟到回答被丢弃。
9. `System.map` 中不存在 `velaclaw_ask`，决策一已在代码中生效并有注释说明原因。
10. **`packages/ai_agent` 的直连路径响应缓冲已改为可增长（决策三的前置依赖），
    或该改动仍未落地但已作为已知阻断项记录在案，交付前不得声称"响应缓冲问题已
    解决"**——`vs_voice` 自己的 32 KiB `response_buf` 不能替代这项修复。
11. Prompt 白名单经一次人工 dump 核对，调试代码已删除。
12. 历史记录写入掉电保持存储并通过重启恢复；若存储为易失，UI 已明确标注。
13. `CONFIG_VS_VOICE_MOCK=n` 的交付镜像中不含 mock 路径。
14. 30 分钟长稳、四阶段各 20 次取消、六类故障注入全部通过。
15. 日志与历史 JSON 中不出现 API Key、Wi-Fi 密码或用户语音正文。

## 10. 风险与明确决策

| 风险 | 性质 | 处置 |
|---|---|---|
| **`ai_agent` 无自启动，全局状态永不初始化** | **已确认，`board/` 零命中，架构级阻断项** | 决策五：`vs_voice_open()` 自己调四个 init 函数；P1 第一任务，P1 门禁新增重启验证 |
| ASR/TTS 零实板记录 | **最高优先级未知** | P0(4) 作为阻断项，不通过不进 P1 |
| `stubs.c` weak 实现可能被链接 | 已确认签名漂移 | 以 ASR 返回真实文本为判据，不看符号 |
| 8.3 命名使 agent 配置不可持久化 | 代码级推导，待实测 | P0(1) 判定；若成立则每次上电重配凭据，并单独立项修 |
| `velaclaw_ask()` 无 request id | 已确认 | 决策一：不用该 SDK |
| `chat_id` 固定导致隐式上下文注入 | 已确认 | 同上 |
| `timeout_ms` 未实现 | 已确认 | `vs_voice` 自实现超时 |
| 直连路径响应缓冲 8 KB 静默截断 | 已确认，**修复点在 `packages/ai_agent`，`vs_voice` 自带的缓冲改不了它** | 决策三：作为 P0/P1 阻塞依赖提给 `packages/ai_agent`；`vs_voice` 的 32 KiB 缓冲只管 `extract_text()` 的二次截断 |
| `llm_chat_*` 同步不可中断 | 已确认 | 取消只丢弃结果；`vs_voice_start()` 用 `-EBUSY` 串行化 |
| `context_builder.c` 的 `size - off` 下溢 | 已确认，本方案路径不经过 | 记录为 `packages/ai_agent` 缺陷，不阻塞本方案 |
| `agent_loop.c` 三条早退分支泄漏 `image_b64` | 已确认，本方案不设置该字段 | 同上 |
| 火山与 MiMo 两套凭据分属不同厂商 | 已确认 | P0(2) 两套都配；不得把一方 Token 传给另一方 |
| SD-NAND 挂载延迟 60 s+ | 已确认 | `vs_history_open()` 复用 `vs_config` 的等待逻辑，仅启动期调用 |
| 事件 `text` 只有 128 B | 已确认 | 只传 `display_text`，完整回答不过事件队列 |
| 软件 JPEG 约 316 ms/张 | 已确认 | 闲时助手一次一张，可接受 |

明确不做（本轮）：多轮对话上下文（每次触发都是独立一轮，不保留上一轮问答）；
流式 ASR（用整段录音后一次识别）；SSE 流式模型输出；多张照片；照片预览；
`velaclaw_client_local.c` 的修复；`packages/ai_agent` 的响应缓冲改造；
Web 侧记录访问。

## 11. 稳定引用

VelaSight App：

```text
app/velasight/vs_app.c                        页面状态机、事件队列、request_id 门禁
app/velasight/include/vs_app.h                事件枚举与 vs_app_event_s
app/velasight/include/vs_types.h              页面枚举、UI 快照、VS_TEXT_LONG
app/velasight/vs_config.c                     SD-NAND 挂载等待与凭据加载
app/velasight/vs_display.c                    LVGL 双屏渲染
app/velasight/Kconfig                         现有阈值选项风格
app/velasight/CMakeLists.txt                  权威构建路径
app/velasight/Makefile                        必须同步的第二套源列表
```

可复用的既有实现：

```text
app/social_cue/social_cue_main.c              sc_capture() 单帧 V4L2 序列、拒答策略
app/agent_camera/agent_camera_main.c          V4L2 几何协商、JPEG marker 校验、base64 导出
app/camera_preview/preview_jpeg.h             硬件 JPEG M2M 封装（社交模式用）
app/audio_test/audio_test_main.c              rec / play / recplay 底层通路判据
app/conv/conv_store.h                         8.3 命名的索引 + 记录文件范例
app/provisioning_web/include/velasight_provisioning.h   定长记录与 8.3 路径范例
```

`ai_agent` 侧接口与实现：

```text
apps/packages/ai_agent/src/voice/voice_channel.h    start/stop/stop_with_text/speak
apps/packages/ai_agent/src/voice/voice_vad.c        voice_vad_init/process
apps/packages/ai_agent/src/voice/volc_asr.h         火山 ASR 与凭据键
apps/packages/ai_agent/src/voice/volc_tts.h         火山 TTS 与流式回调
apps/packages/ai_agent/src/llm/llm_proxy.h          llm_chat_vision_raw 等
apps/packages/ai_agent/src/llm/llm_vision.c         data URI 组装与 base64
apps/packages/ai_agent/src/infra/vela_tls.h         HTTPS 客户端（社交模式主用）
apps/packages/ai_agent/src/infra/config_store.c     config_store_init 的实际行为
apps/packages/ai_agent/src/core/agent_loop.c        本方案刻意绕开的上下文组装
apps/packages/ai_agent/src/core/session_mgr.c       会话历史与 8 KB 截断点
apps/packages/ai_agent/src/core/context_builder.c   system prompt 组装与下溢隐患
apps/packages/ai_agent/src/sdk/velaclaw_client_local.c   决策一的取证对象
apps/packages/ai_agent/include/agent_config.h       全部容量常量
apps/packages/ai_agent/src/channels/cmd_voice.c     set_volc_asr / set_volc_speaker
```

平台与配置：

```text
board/beken/boards/bk7258/bk7258-ap/src/bk7258_mmcsd.c    /mnt/sdnand vfat 挂载
board/beken/boards/bk7258/bk7258-ap/src/bk7258_agent_config.c  config.json 写入点
board/beken/chips/bk7258/bk7258_trng.c                    /dev/random 注册
contest/nuttx/fs/fat/fs_fat32dirent.c                     fat_parsesfname 的 8.3 判定
contest/cmake_out/bk7258-ap_ai_agent/.config              FAT_LFN 与 DATA_DIR 取证
使用说明-camera-display-ai_agent.md                       命令实测状态表
docs/固件构建步骤.md                                       构建与打包
docs/SD-NAND使用说明.md                                    存储约束
```

行号不是稳定接口。后续引用优先使用函数名、宏名与文件路径，并在每次
`packages/ai_agent` 更新后重新执行第 1.2 节取证。
