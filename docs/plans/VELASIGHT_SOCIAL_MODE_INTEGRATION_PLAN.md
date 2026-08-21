# VelaSight 社交辅助模式接入实施方案

> 文档版本：V1
>
> 文档状态：2026-08-21 设计基线，**业务层尚未实现**。`app/velasight/vs_social.c`、
> `vs_cloud.c`、`vs_media.c` 及对应头文件当前为 **0 字节空文件**，均未加入
> `CMakeLists.txt` 的 `SRCS`。社交流程当前止步于 `VS_PAGE_SOCIAL_STARTING`：
> `vs_app.c` 只把页面切过去，不生成 `session_id`，不采集媒体，不发任何网络请求。
>
> 适用范围：BK7258 AP/CPU1 上 `app/velasight` 的「社交辅助模式」—— 历史页长按
> Power 开始、社交中长按返回结束的完整会话闭环。闲时 AI 模式见
> [VELASIGHT_IDLE_AI_MODE_INTEGRATION_PLAN.md](VELASIGHT_IDLE_AI_MODE_INTEGRATION_PLAN.md)，
> 两者互斥占用 Camera、ADC、DAC 与云请求上下文，共用第 5.7 节的 `vs_history`。
>
> 交互与显示口径以
> [VELASIGHT_UI_DESIGN_INSTRUCTION.md](VELASIGHT_UI_DESIGN_INSTRUCTION.md) 为准；
> 工程状态与阶段门禁以
> [VELASIGHT_MCU_APP_DEVELOPMENT_PLAN.md](VELASIGHT_MCU_APP_DEVELOPMENT_PLAN.md)
> 为准。云端协议以
> [OpenVela比赛云端技术方案.docx](https://mi.feishu.cn/wiki/Tm8vwCXk7idMi6kl66Hc0rKvn2b)
> 的取证结论为准，但该文档存在第 3 节列出的四处缺口，**必须先关闭 P0 才能开始
> 写代码**。
>
> 本文所说「团队自建社交会话云」区别于闲时模式使用的 MiMo/火山模型服务商，是
> 项目组自己维护的云端接口（`/contest/v1/*`），本方案是设备端与该云端的对接。

## 0. 使用规则

### 0.1 路径

```text
工作区根目录：       /home/mi/vela_competition
正式代码仓库：       /home/mi/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
OpenVela/NuttX 源码： /home/mi/vela_competition/contest/nuttx
ai_agent 包：        /home/mi/vela_competition/contest/apps/packages/ai_agent
```

本文相对路径均相对正式代码仓库根目录。

### 0.2 安全与纪律规则

1. **P0（云端接口对齐）未书面关闭前，禁止编写 `vs_cloud.c` 的正式实现。** 可以
   编写基于本文 mock 契约的骨架，但不得把猜测路径固化为最终协议。
2. **UI 与按键热路径禁止读写 SD-NAND。** 与闲时方案第 0.2 节同一约束，社交模式
   涉及的落盘更多，更要遵守。
3. **`/dev/video0` 只有一个所有者。** 社交模式必须在会话期间常驻持有相机，不得
   像 `agent_camera` 一样逐帧 open/close；这与闲时模式的单帧拍照策略不同，两者
   互斥即可,不需要共用同一个媒体模块的运行方式。
4. **原始图片和音频不落盘、不长期驻留。** 只在有界队列中短暂停留,发送成功或
   会话结束即释放;本地历史只保存云端处理后的文本结果。
5. **禁止在采集线程上做网络 I/O。** 音频/图像采集与云端上传必须分线程,网络拥塞
   不能拖慢 ADC/Camera 的采集节奏,参考 `audio_test_stream.c` 的双线程 + 环形槽
   设计。
6. **`session_id` 由设备生成,使用 TRNG。** 不使用可能尚未授时的时间戳作为唯一
   来源。
7. **强烈情绪事件与最终结果必须是两类不同的事件。** 即时事件只更新颜色/短文本,
   不得写历史、不得触发 TTS;只有最终结果事件才允许落盘和播报。
8. **两套状态码不得共用同一个 C 枚举。** 云端技术方案里「对端侧状态」与「服务端
   状态」码值有重叠但含义不同（如 `30` 在对端侧是失败、在服务端侧是图片无有效
   人脸）,必须落成两个独立类型。
9. 所有阶段提交必须记录构建目录、`.config`、`nuttx.bin` 体积增量与 sha256。

## 1. 目标与当前状态

### 1.1 目标架构

```text
历史页长按 Power 满圈（VS_INPUT_LONG，已实现于 vs_app.c）
        |
        v
vs_begin_request() -> request_id；page = VS_PAGE_SOCIAL_STARTING
        |
        v
vs_social.c（新增）编排层
        |
        +-- vs_cloud_social_open()  --------> PUT /contest/v1/session
        |
        v
   VS_APP_EVENT_SOCIAL_STARTED（已定义于 vs_app.h）
        |
        v
   page = VS_PAGE_SOCIAL_RUNNING
        |
        +------------------------------------------------------------+
        |                                                            |
   vs_media.c 相机 worker                              vs_media.c 音频 worker
   常驻持有 /dev/video0                                  常驻持有音频输入
   约 3 FPS 定速（受限于软件 JPEG 编码 316 ms/张）         2 秒累计一块
        |                                                            |
        v                                                            v
   vs_cloud_social_upload(image)                    vs_cloud_social_upload(audio)
   POST /contest/v1/upload 登记 -> presignedUrl                同一入口，fileType=1
   PUT presignedUrl 直传二进制（FDS）
        |                                                            |
        +------------------------------------------------------------+
                                |
                                v
                 vs_social 低频轮询 vs_cloud_social_poll_event()
                 GET /contest/v1/getResult（按 msgId）
                                |
                    +-----------+-----------+
                    |                       |
              event=0 非极端           event=0 极端 / event=1 建议
              静默,不上屏               VS_APP_EVENT_SOCIAL_ALERT
                                        （颜色 + 短文本，不播 TTS）
                                |
                                v
        社交中长按返回键满圈（已实现于 vs_app.c，切 VS_PAGE_SOCIAL_FINALIZING）
                                |
                                v
                 停止两个媒体 worker，上传尾包
                                |
                                v
                 vs_cloud_social_finalize()  -> DELETE /contest/v1/session
                 （异步：立即拿到 msgId，非最终结果）
                                |
                                v
                 vs_social 轮询 vs_cloud_social_get_result(msgId)
                 GET /contest/v1/getResult，直到 status=50 会话关闭
                                |
                                v
                 vs_history_append()（复用闲时方案 P4 的持久化模块）
                                |
                                v
                 VS_APP_EVENT_SOCIAL_RESULT -> VS_PAGE_SOCIAL_RESULT
                 最终 TTS 播报（复用闲时方案的 voice_channel_speak）
```

### 1.2 当前基线（逐项取证）

截至 2026-08-21：

| 项目 | 当前状态 | 证据 |
|---|---|---|
| `vs_social.c` / `.h` | 0 字节空文件 | `ls -la app/velasight/` |
| `vs_cloud.c` / `.h` | 0 字节空文件 | 同上 |
| `vs_media.c` / `.h` | 0 字节空文件 | 同上（与闲时方案共用同一模块,但社交模式用到的是常驻采集能力,闲时模式只用单帧拍照,两套函数在同一文件内按用途区分） |
| 页面状态机 | 已实现,但无业务 | `vs_app.c` 的 `VS_PAGE_SOCIAL_ENTER/STARTING/RUNNING/ALERT/PAUSING/PAUSED/RESUMING/EXITING/FINALIZING/RESULT` 九个页面的 snapshot 与按键分支齐全,长按进入/退出的进度环、暂停/继续的软键切换都已实现 |
| 异步事件契约 | 已定义,未使用 | `vs_app.h` 的 `SOCIAL_STARTED/START_FAILED/ALERT/ALERT_CLEARED/PAUSED/RESUMED/PAUSE_FAILED/RESULT/FINALIZE_FAILED` 九个事件 |
| `event.text` 上限 | 128 字节 | `VS_TEXT_LONG`,与闲时方案同一约束,`display_text` 和最终摘要都要在此内截断 |
| 云端协议文档完整度 | 有四个接口原型 + 缺口 | 见第 3 节 |
| HTTP/HTTPS 客户端 | app 层无,`ai_agent` 内有可复用实现 | 见第 2.1 节;`app/conv/conv_net.c` 只有裸 socket `GET /api/time`,`inet_pton` 只接受 IPv4 点分地址,不适用 |
| multipart/form-data | 全仓库无实现 | grep `multipart\|form-data\|boundary` 只命中注释和无关代码 |
| DNS 解析 | app 层无,`ai_agent` 内有 | `vela_tls.c` 走 `mbedtls_net_connect()` 内部的 `getaddrinfo()`,支持域名 |
| 相机常驻+编码 | 已有可复用实现 | `app/camera_preview/` 的主循环 + `preview_jpeg.h` 的硬件 M2M 封装 |
| 相机吞吐 | 316 ms/张 ≈ 3.16 FPS | `使用说明-camera-display-ai_agent.md`:copy 27 ms + 软件编码 289 ms;`/dev/video0` JPEG 已收口为软件编码器,无条件生效 |
| 云端要求帧率 | 200 ms/张 = 5 FPS | 云端技术方案「输入」章节 |
| 音频分块编码 | 已有可复用实现 | `app/audio_test/audio_test_stream.c` 的双线程环形槽设计;`audio_test_ogg.c` 的 Opus 编码器 |
| 云端要求音频格式 | 仅写「音频文件」,格式未定 | 云端技术方案未指定采样率/编码;开发计划冻结为 16 kHz/16 bit/mono PCM 2 秒块 |
| 置信度拒答策略 | 已有可复用实现,当前是 mock | `app/social_cue/social_cue_main.c` 的 `sc_notify()` 阈值判断,唯一 mock 的是网络请求 |
| 震动反馈 | 已有可复用实现 | `social_cue_main.c` 的 `sc_buzz()`,`/dev/pwm0` |
| TRNG | 可用 | `/dev/random` 已注册（`bk7258_trng.c`）,`CONFIG_DEV_URANDOM=y` |
| SD-NAND 8.3 命名 | 需先判定 | 与闲时方案共享同一结论,见闲时方案第 2.4 节 P0 |

### 1.3 工作区状态记录

```bash
cd /home/mi/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
git status --short
git log -1 --oneline
```

## 2. 已取证的可复用能力

### 2.1 HTTP/HTTPS 客户端：复用 `vela_tls.c`,不重写

`packages/ai_agent/src/infra/vela_tls.c` 提供的不是 LLM 专用接口,而是通用
HTTP(S) 客户端:

```c
/* vela_tls.h */
int vela_https_request(const char *host, const char *port,
                       const char *method, const char *path,
                       const vela_header_t *headers,
                       const char *body, size_t body_len,
                       char *resp_buf, size_t resp_cap,
                       size_t *out_body_len);
int vela_https_get(const char *host, const char *port, const char *path,
                   char *resp_buf, size_t resp_cap);
int vela_https_post_json(const char *host, const char *port, const char *path,
                         const vela_header_t *extra_headers,
                         const char *json_body,
                         char *resp_buf, size_t resp_cap);
int vela_http_post_json(...);   /* 明文,仅内网调试用 */
```

`vela_https_request()` 的 `method` 是字符串、`body/body_len` 是任意字节,**不局限
于 JSON**。这意味着云端技术方案里的 `PUT /contest/v1/session`、
`DELETE /contest/v1/session`、`GET /contest/v1/getResult` 都可以直接调用,不需要
新写请求组装代码。

已验证的能力(取证于 `vela_tls.c`):

- DNS 解析:走 `mbedtls_net_connect()` 内部的 `getaddrinfo()`,支持域名,不像
  `conv_net.c` 那样只接受 IPv4 点分地址。
- TLS 1.2/1.3 协商、ALPN、连接池(默认 2 个 keep-alive slot)、chunked 与
  Content-Length 响应解码、drain 复用逻辑。
- 授时保护:握手前若 `time(NULL) < 1704067200`(2024-01-01)会强制把系统时钟设为
  一个固定的 2026 时间戳(`vela_tls.c` 第 256 行附近的 `forcing to 2026`)。这是
  **临时且不安全**的兜底,`BK7258_OPENVELA_MIMO_NETWORK_PORTING_PLAN.md` 的 M0/M1
  阶段已规划改为「未校时则硬失败」。社交模式的 TLS 请求会经过同一段代码,详见
  第 3.3 节。
- 证书校验:当前 **`MBEDTLS_SSL_VERIFY_OPTIONAL`**(`vela_tls.c` 第 352 行),即
  加密但不验证。云端团队的服务器证书链未知,不能直接沿用 MiMo 方案已锁定的
  DigiCert 信任锚。

**决策(见第 3.1 节)**:`vs_cloud.c` 直接 `#include "infra/vela_tls.h"` 并调用
`vela_https_request()`,不重新实现 HTTP 客户端、不引入 `netutils/webclient`。

### 2.2 multipart/form-data:先确认是否真的需要

云端技术方案 2.2 节写「上传文件」接口的数据格式是 `multipart/form-data`,但结合
2.1 节场景描述「云端返回一个 FDS 的上传预签名 URL」,更可能的实际流程是:

```text
设备 POST /contest/v1/upload  (小请求,登记 event/fileType/deviceId/sessionId/timestamp)
   -> 云端返回 presignedUrl
设备 PUT presignedUrl         (纯二进制 body,FDS 对象存储的标准直传方式)
   -> 200/204
```

这是对象存储预签名 URL 的行业惯例:登记请求携带的是**描述字段**,真正的二进制
走预签名 URL 的直传,直传本身**不需要 multipart 包装**,只需要设置正确的
`Content-Type`(如 `image/jpeg`)并把文件字节作为请求 body。`vela_https_request()`
的通用 `body/body_len` 参数已经能覆盖这一步。

云端文档没有明确说 `/contest/v1/upload` 这个 POST 请求本身是否要求携带二进制
内容(如果要求,那才是真正的 multipart)。这是 P0 必须问清楚的问题,见第 3.4 节。
本方案按「登记 + 预签名 URL 直传」设计,**不预先实现 multipart writer**;若 P0
确认云端要求 `/upload` 本身携带二进制,再补一个最小 multipart 组装函数(边界字符串
+ 字段头 + 二进制段,工作量约半天,不阻塞其他部分的设计)。

### 2.3 相机:常驻持有 + 硬件 JPEG M2M 封装

社交模式的采集方式与闲时模式完全不同,不能复用闲时模式的单帧 open/close:

```c
/* preview_jpeg.h,已有实现,直接复用 */
struct preview_jpeg_s *preview_jpeg_open(int w, int h, int quality);
int preview_jpeg_encode(struct preview_jpeg_s *ctx,
                        const uint8_t *frame, size_t len,
                        const uint8_t **out, size_t *outlen);
void preview_jpeg_last_ms(struct preview_jpeg_s *ctx,
                          uint32_t *copy_ms, uint32_t *codec_ms);
void preview_jpeg_close(struct preview_jpeg_s *ctx);
```

`app/camera_preview/camera_preview_main.c` 的主循环结构是最小可复用模板:
`VIDIOC_STREAMON` 一次 → 循环 `DQBUF → 处理 → QBUF` → 退出前 `STREAMOFF`。**这条
路径的 `/dev/video0` 格式是 `V4L2_PIX_FMT_UYVY`、几何是 640x480(实测于
`camera_preview_main.c` 的 `CAM_WIDTH/CAM_HEIGHT` 常量),不是 JPEG。** `DQBUF`
拿到的是原始 UYVY 帧,`preview_jpeg_encode()` 才是把这帧拷进 `/dev/video1`
硬件 M2M 编码器换出 JPEG 的那一步 —— 这一点必须在这里说清楚,因为 `/dev/video0`
**同时存在另一条互斥的用法**:`agent_camera`/`social_cue` 把 `/dev/video0` 本身
设成 `V4L2_PIX_FMT_JPEG`,`DQBUF` 直接拿到成品 JPEG,这种用法下 `/dev/video1`
和 `preview_jpeg_encode()` 完全不出现。两条路径在 `VIDIOC_S_FMT` 这一步就分叉,
不能在同一个采集循环里同时使用两者的产物。

**本方案选定第一条路径(UYVY 640x480 + `/dev/video1` M2M 编码)**,理由是:

- 第 1.2 节表格引用的 316 ms/张(copy 27 ms + 编码 289 ms)这个数字,来自
  `使用说明-camera-display-ai_agent.md` 里 `camera_preview ... jpeg=30` 这条命令
  的实测输出,属于这条路径,不是 `/dev/video0` 直出 JPEG 的路径。
- `/dev/video0` 直出 JPEG 这条路径在连续多帧采集(`n>1`)上有一个已确认的缺陷:
  同一份文档 10.13 节记录 480x480 下 `agent_camera 480x480 n=10` 固定失败
  (`sampler_skipped=66` 后 `FAILED (-110)`),**只有 640x480 才能连续出帧**
  (`640x480 n=10` 成功但吞吐只有 `measured=1.26 fps`)。社交模式需要连续采集
  整场会话,不能用一条对连续采集本身就有缺陷的路径。

因此第 3 节全文按 UYVY 640x480 + `/dev/video1` 编码这一条路径设计,不混用两条
路径的产物。

社交模式的 `vs_media.c` 相机 worker 去掉双屏渲染部分,保留:

```text
open /dev/video0 一次,会话期间不关闭
VIDIOC_S_FMT V4L2_PIX_FMT_UYVY 640x480 + REQBUFS + QUERYBUF/mmap + STREAMON 一次
preview_jpeg_open(640, 480, quality) 一次,会话期间不关闭(独立打开 /dev/video1)
循环:
  DQBUF(从 /dev/video0 拿一帧 UYVY)
  若 buf.flags & V4L2_BUF_FLAG_ERROR: 计数并 continue(不当作丢帧上报)
  定速判断:与上一次成功编码的时间差 < 目标周期则跳过编码,仅 QBUF
  preview_jpeg_encode() 把这一帧拷进 /dev/video1 编码,换出 JPEG
  产出的 JPEG 指针 memcpy 到有界队列(不能跨 QBUF 持有,下一次 DQBUF 会复用缓冲)
  QBUF(把 UYVY 缓冲还给 /dev/video0)
  检查停止标志,退出则 STREAMOFF + REQBUFS(count=0) + close(video0)
    + preview_jpeg_close(编码器)
```

**实测吞吐是硬约束**:copy 27 ms + 编码 289 ms ≈ 316 ms/张,理论上限约 3.16 FPS,
达不到云端文档的 5 FPS(200 ms/张)。这是第 3.2 节的核心决策依据。

### 2.4 音频:环形槙 + 独立上传线程的设计可整体复用

`app/audio_test/audio_test_stream.c` 已经解决了「网络拥塞不能拖慢采集」这个
核心问题,设计可以原样搬过来:

```text
STREAM_SLOTS = 4               四槙环形队列,64 KiB/槙(16 kHz/2 秒)
capture 线程:只管把采样填进槙,从不阻塞、从不做编码或网络 I/O
uploader 线程:独立、低于 capture 优先级,编码 + 发送;跟不上时丢最老的
             SLOT_READY 槙,计数上报
STREAM_CONNECT_MS = 2000       connect 快速失败
STREAM_ATTEMPTS = 2            每块最多重试一次
```

这套设计是给裸 TCP 目标写的,社交模式需要改的是发送目的地:从「一个自建 TCP
服务器」换成「`vs_cloud_social_upload()` 走 HTTPS」,环形槙和双线程结构不变。

编码格式待定:`audio_test_ogg.c` 现成 Opus 编码器(38 KiB 状态 + 62 KiB 包
scratch,可放 PSRAM);若云端要求原始 PCM 则跳过编码步骤直接发送 2 秒的
16 kHz/16 bit/mono 数据(64 KiB/块)。**这是 P0 必须确认的项**,见第 3.4 节。

### 2.5 置信度拒答策略与震动:从 `social_cue` 迁移

```c
/* social_cue_main.c 现有阈值,直接复用数值 */
#define SC_CONF_REPORT    0.60f   /* >= 给线索 + 建议 */
#define SC_CONF_WEAK      0.40f   /* [0.40,0.60) 只给线索,不给建议 */
                                   /* < 0.40 拒答 */
/* 冲突线索(两条置信度接近但含义矛盾)强制拒答,见 sc_conflicting() */

#define SC_MOTOR_FREQ     1000                       /* /dev/pwm0,1 kHz */
#define SC_MOTOR_DUTY     ((ub16_t)((7u * 65536u) / 10u))   /* 70% duty */
```

`sc_notify()` 的四分支判断逻辑(拒答/弱提示/强提示+震动模式)可以原样搬进
`vs_social.c` 的强烈情绪事件处理路径,只是把 `printf` 换成 `vs_app_post_event()`。

### 2.6 事件与页面状态机(已实现,不需要改)

```c
/* vs_app.h,已实现的九个社交事件 */
VS_APP_EVENT_SOCIAL_STARTED
VS_APP_EVENT_SOCIAL_START_FAILED
VS_APP_EVENT_SOCIAL_ALERT
VS_APP_EVENT_SOCIAL_ALERT_CLEARED
VS_APP_EVENT_SOCIAL_PAUSED
VS_APP_EVENT_SOCIAL_RESUMED
VS_APP_EVENT_SOCIAL_PAUSE_FAILED
VS_APP_EVENT_SOCIAL_RESULT
VS_APP_EVENT_SOCIAL_FINALIZE_FAILED
```

`vs_app.c` 中对应的九个页面切换分支、长按进度环、Power 短按暂停/继续的软键
切换均已就位。`vs_social.c` 只需要在正确的时机调用 `vs_app_post_event()`,不需要
碰 `vs_app.c` 的状态机本体。**唯一需要新增的是暂停/继续对媒体 worker 的实际
控制**,当前 `VS_APP_EVENT_SOCIAL_PAUSED/RESUMED` 只切页面,不停止采集。

## 3. 核心决策

### 3.1 决策一:复用 `vela_tls.c` 而不是新写 HTTP 客户端

第 2.1 节已说明 `vela_https_request()` 的通用性足够覆盖云端技术方案的四个接口
(`PUT session`、`POST upload` 登记、`GET getResult`、`DELETE session`)加一次
预签名 URL 的 `PUT` 直传。重新实现的成本(DNS、TLS 握手、chunked 解码、连接池)
远高于复用,且 `vela_tls.c` 已在 `ai_agent` 配置下实测握手成功
(`Handshake OK: TLSv1.2` + `HTTP Status: 200`)。

`app/velasight/CMakeLists.txt` 已经把 `${NUTTX_APPS_DIR}/packages/ai_agent/include`
和 `.../src` 加入 include 路径(供闲时方案调用 `llm_chat_vision_raw` 等),
`vs_cloud.c` 可以直接 `#include "infra/vela_tls.h"`。Flat build 下所有符号在
最终链接时全局可见,不需要额外的 `DEPENDS`。

**代价**:`vela_tls.c` 的连接池默认 2 个 slot,是给 LLM/ASR/TTS 场景设计的。社交
模式的云端域名与 MiMo/火山域名不同,连接池按 `host` 匹配(`pool_acquire()`),互不
冲突,但闲时模式与社交模式互斥运行这一前提必须保持,否则两条链路会竞争同一个
2-slot 池。

### 3.2 决策二:向云端申请把演示基线降到 3 FPS,不修硬件 JPEG 通路

**先厘清 316 ms/张这个数字属于哪条路径,避免和 2.3 节修正前的表述一样把两条
互斥的采集架构混在一起。** 社交模式选定的是 2.3 节的 UYVY 640x480 + `/dev/video1`
硬件 M2M 编码路径(理由见 2.3 节),316 ms/张(copy 27 ms + 编码 289 ms,约
3.16 FPS)是这条路径的实测值,来自 `camera_preview ... jpeg=30` 的命令输出。
`/dev/video0` 直出 JPEG 这条路径(`agent_camera`/`social_cue` 用的那条)有自己
独立的、更差的连续采集数据(640x480 下 `n=10 -> measured=1.26 fps`,480x480 下
`n>1` 直接失败,见 `使用说明-camera-display-ai_agent.md` 10.13 节),本方案不用
这条路径,所以这个 1.26 fps 不是本方案的采样上限,列在这里只是为了避免和
3.16 FPS 混淆。

`/dev/video0` 直出 JPEG 那条路径确实经历过"硬件 JPEG 通路因逐帧变化的填充长度
缺陷被禁用,收口为软件编码器"的修复(`使用说明-camera-display-ai_agent.md`
10.10 节),但那次修复和收口都发生在直出 JPEG 这条路径内部,与本方案选用的
UYVY + `/dev/video1` 编码路径是两件不同的事——`/dev/video1` 一直是软件编码器,
不存在"曾经是硬件、后来收口"的历史。云端技术方案要求 200 ms/张(5 FPS),本方案
的 3.16 FPS 上限达不到。

**决策**:不在本方案范围内修任何一条路径的驱动缺陷(那是
`bk7258_camera_imgdata.c`/`bk7258_jpeg_enc.c` 的独立驱动工作,不是设备端 App
集成的范畴)。设备端按 3.16 FPS 的实测值设计定速器,并把这个数字作为 P0 清单里
第一条需要云端书面确认的事项。如果驱动侵修复后测得可达 5 FPS,再单独升级采样率,
不阻塞本方案。

### 3.3 决策三:先补时钟硬失败,再接入社交云的 TLS 请求

`vela_tls.c` 当前对未校时的系统时钟做的是「强制设为 2026」而不是拒绝连接。这个
行为是为了让 MiMo 请求在没有 SNTP 的环境下也能握手,但会让 `VERIFY_REQUIRED`
(证书有效期校验)失去意义。`BK7258_OPENVELA_MIMO_NETWORK_PORTING_PLAN.md` 的
M0/M1 阶段已经规划了修复路径(先接入 SNTP,再把伪造时间改成硬失败,最后开
`VERIFY_REQUIRED` + CA)。

**决策**:社交模式的 P1 阶段(见第 5.3 节)复用 MiMo 方案的 M0 SNTP 接入结论,
但**不重复实施 M1 的 CA 挂载**,因为云端团队服务器的证书链未知,需要先用
`openssl s_client` 对云端实际域名做一次链取证(参照
`BK7258_OPENVELA_MIMO_NETWORK_PORTING_PLAN.md` 2.2 节的方法),再决定信任锚。
在证书链取证完成前,社交模式的 TLS 请求继续使用现状的 `VERIFY_OPTIONAL`(加密
但不验证),这是一个**已知的、临时的安全折衷**,必须记入第 10 节风险表,不得
静默带入交付镜像而不告知。

### 3.4 决策四:云端接口对齐是唯一的强制前置阶段

与闲时方案不同,社交模式的阻断项主要在**协议层面**而不是本地能力。以下四类
冲突必须由云端团队书面确认或补充,详细清单见第 5.2 节 P0:

1. 帧率:3 FPS(设备实测上限)vs 5 FPS(云端文档要求)。
2. 上传语义:`/contest/v1/upload` 的 `multipart/form-data` 是否要求本身携带
   二进制,还是只是登记字段后走预签名 URL 直传(见第 2.2 节)。
3. 音频格式:采样率/位深/声道/编码(PCM 还是 Opus)完全未在云端文档中出现。
4. 六元抽象与四个实际接口的映射缺口:开发计划设想的
   `open/upload/poll_event/finalize/get_result/ack` 六个操作,云端技术方案只给了
   四个 HTTP 接口(`PUT session`、`POST upload`、`GET getResult`、
   `DELETE session`)。**`ack` 没有对应的云端接口**,`poll_event` 与
   `get_result` 实际上是同一个 `GET /contest/v1/getResult`,只是传入的 `msgId`
   语义不同(一次 upload 的处理状态 vs 一次 finalize 的处理状态)。这个映射
   缺口如果不澄清,`vs_cloud.c` 的六个函数会有两个是空调用或者错误封装。

**决策**:`vs_cloud.c` 的公开接口保留开发计划的六元设计(调用方约定不变),但
`ack` 在云端确认前实现为**本地空操作**(设备侧完整写入历史后即视为完成,不
发任何请求),并在代码注释里明确写明这是因为云端无对应接口,待云端补充后再
接入真实调用。

## 4. 接口契约

### 4.1 状态码:两套独立枚举,不复用数值

云端技术方案 3.1 节图示的两套状态机,按状态持有方拆成两个类型:

```c
/* vs_cloud.h —— 服务端侧状态,来自 getResult 的 status 字段 */
enum vs_cloud_server_state_e
{
  VS_CLOUD_SRV_SESSION_OPEN = 0,      /* 会话初建立 */
  VS_CLOUD_SRV_IMAGE_PENDING = 10,    /* 图片未返回结果 */
  VS_CLOUD_SRV_IMAGE_NO_FACE = 30,    /* 图片无有效人脸 */
  VS_CLOUD_SRV_IMAGE_UNRECOGNIZED = 31, /* 图片无法识别 */
  VS_CLOUD_SRV_IMAGE_CALM = 20,       /* 非极端情绪 */
  VS_CLOUD_SRV_IMAGE_EXTREME = 21,    /* 极端情绪,继续走音频链路 */
  VS_CLOUD_SRV_AUDIO_NO_SPEECH = 32,  /* 音频无有效语音 */
  VS_CLOUD_SRV_AUDIO_UNRECOGNIZED = 33, /* 音频无法识别 */
  VS_CLOUD_SRV_AUDIO_PENDING = 11,    /* 音频未返回结果 */
  VS_CLOUD_SRV_AUDIO_DONE = 22,       /* 音频返回结果 */
  VS_CLOUD_SRV_CLOSING = 40,          /* 关闭会话未返回结果 */
  VS_CLOUD_SRV_CLOSED = 50            /* 会话关闭 */
};

/* vs_cloud.h —— 对端侧状态(供设备理解自身应处的阶段,非直接来自云端字段,
 * 是 vs_cloud.c 依据 server_state 与本地时序推导出的展示层状态) */
enum vs_cloud_peer_state_e
{
  VS_CLOUD_PEER_EMOTION_ANALYZING = 10,
  VS_CLOUD_PEER_EMOTION_DONE = 20,
  VS_CLOUD_PEER_ADVICE_GENERATING = 11,
  VS_CLOUD_PEER_ADVICE_DONE = 21,
  VS_CLOUD_PEER_FAILED = 30,
  VS_CLOUD_PEER_CLOSING = 40
};
```

服务端状态到对端状态的映射(云端技术方案 3.1 节的跨层箭头,取证结论):

| 服务端状态 | 对端状态 |
|---|---|
| `SESSION_OPEN` / `IMAGE_PENDING` | `EMOTION_ANALYZING` |
| `IMAGE_CALM`(含 `NO_FACE`/`UNRECOGNIZED` 组) | `EMOTION_DONE` |
| `AUDIO_PENDING` | `ADVICE_GENERATING` |
| `AUDIO_DONE` | `ADVICE_DONE` |
| `CLOSING` | `CLOSING` |

只有 `IMAGE_EXTREME` 才继续触发音频链路;其余分支(`NO_FACE`/`UNRECOGNIZED`/
`CALM`)均静默,不产生 `VS_APP_EVENT_SOCIAL_ALERT`。

### 4.2 `vs_cloud` 六个抽象操作与 HTTP 映射

```c
/* vs_cloud.h */
struct vs_session_open_s
{
  char device_id[32];
  char session_id[24];     /* "vs-264-" + 8 位十六进制,TRNG 生成 */
};

struct vs_media_packet_s
{
  enum { VS_MEDIA_IMAGE = 0, VS_MEDIA_AUDIO = 1 } type;
  const unsigned char *data;
  size_t               len;
  uint32_t             sequence;   /* 按 media type 分别递增 */
  uint32_t             timestamp_ms;
};

struct vs_social_event_s
{
  char     msg_id[24];
  int      event;                  /* 0 图片 / 1 音频建议 / 2 关闭会话 */
  enum vs_cloud_server_state_e server_state;
  enum vs_emotion_e emotion;       /* 复用 vs_types.h 已有枚举 */
  uint32_t color;
  char     display_text[VS_TEXT_LONG];
  char     suggestion[VS_TEXT_LONG];
};

int vs_cloud_social_open(const struct vs_session_open_s *req);
int vs_cloud_social_upload(const char *session_id,
                           const struct vs_media_packet_s *packet,
                           char *msg_id_out, size_t msg_id_cap);
int vs_cloud_social_poll_event(const char *session_id, const char *msg_id,
                               struct vs_social_event_s *out);
int vs_cloud_social_finalize(const char *session_id, char *msg_id_out,
                            size_t msg_id_cap);
int vs_cloud_social_get_result(const char *session_id, const char *msg_id,
                              char *result_json, size_t cap);
int vs_cloud_social_ack(const char *session_id);   /* 当前本地空操作,见 3.4 */
```

HTTP 映射(域名/端口待 P0 确认,以下用占位 `$CLOUD_HOST`):

| 抽象操作 | HTTP | 路径 | 备注 |
|---|---|---|---|
| `vs_cloud_social_open` | `PUT` | `/contest/v1/session` | body 含 `deviceId/sessionId/timestamp` |
| `vs_cloud_social_upload` | `POST` | `/contest/v1/upload` | 登记,拿 `presignedUrl` |
| (upload 第二步) | `PUT` | `presignedUrl`(云端返回的完整 URL,通常是另一个域名) | 二进制直传,见 2.2 节 |
| `vs_cloud_social_poll_event` | `GET` | `/contest/v1/getResult?msgId=...` | 传 upload 返回的 `msgId` |
| `vs_cloud_social_finalize` | `DELETE` | `/contest/v1/session` | 异步,立即返回新 `msgId` |
| `vs_cloud_social_get_result` | `GET` | `/contest/v1/getResult?msgId=...` | 传 finalize 返回的 `msgId`,轮询到 `status=50` |
| `vs_cloud_social_ack` | 无 | — | 本地空操作,见 3.4 节 |

### 4.3 图片/音频结果的 JSON 契约(按 event 分支)

取证于云端技术方案 4.4 节响应示例:

```json
// event=0 图片结果
{"response": {"emotionColor": "red", "emotionDetail": "生气", "confidence": "0.95"}}

// event=1 音频结果(仅极端情绪之后才会有)
{"response": {"advice": "用户真的怒了"}}

// event=2 关闭会话结果
{"response": {
  "ttsMinutes": "xxx/xxx",
  "txtMinutes": "xxx",
  "audioTimeline": [{"sentence": "...", "emotionColor": "...",
                     "emotionDetail": "...", "confidence": "0.95",
                     "timestampBegin": "xxx", "timestampEnd": "xxx"}],
  "emotionTimeline": [{"emotionColor": "...", "emotionDetail": "...",
                       "confidence": "0.95", "timestamp": "xxx"}]
}}
```

情绪颜色分类(云端技术方案 4.4 节 Detail 集合,直接复用不做二次映射):

| `emotionColor` | `emotionDetail` 取值 |
|---|---|
| `red` | 生气、反感 |
| `blue` | 害怕、伤心、疑惑、惊讶 |
| `green` | 愉悦、中立 |

`vs_social.c` 解析这段 JSON 时同样遵守「有界解析,禁止把字段当无限长字符串」的
规则,`emotionDetail`/`advice`/`suggestion` 截到 `VS_TEXT_LONG - 1` 并保证落在
UTF-8 字符边界。

### 4.4 强烈情绪两阶段事件的设备侧落地

云端技术方案设计的是「先给情绪,建议异步补上」,`vs_social.c` 落地为:

```text
poll_event() 返回 event=0 且 server_state == IMAGE_EXTREME:
  -> 立即投 VS_APP_EVENT_SOCIAL_ALERT(emotion/color/display_text)
  -> 不播 TTS,不写历史
  -> 记录 event_id,继续正常采集

后续 poll_event() 返回 event=1(音频建议),且 msgId 关联同一 IMAGE_EXTREME 触发点:
  -> 更新 alert 的 suggestion 字段(仍是 VS_APP_EVENT_SOCIAL_ALERT,不是新事件类型)
  -> 若此时新的一帧图片已经把情绪判回非极端(IMAGE_CALM),旧的迟到建议丢弃
     (对照 vs_app.c 的 request_id 门禁思路,vs_social 内部维护一个
     "当前有效 alert 世代号",迟到建议世代号不匹配则丢弃)

VS_APP_EVENT_SOCIAL_ALERT_CLEARED 由本地判断触发:
  连续 N 次(建议 3 次)poll_event 都回到 IMAGE_CALM,且已过冷却时间,才清除,
  避免情绪在阈值附近来回跳变导致屏幕颜色频繁闪烁
```

冷却时间与去抖窗口放入 Kconfig(见第 6.1 节),UI 设计文档已要求「极端情绪建议
至少连续多个有效窗口且置信度过阈值后才显示,并设置冷却时间」。

### 4.5 会话时序与暂停/继续的媒体控制

`vs_app.c` 当前的 `VS_PAGE_SOCIAL_PAUSING/PAUSED/RESUMING` 只切页面。本方案要求
`vs_social.c` 在收到暂停请求时:

```text
Power 短按暂停(vs_app.c 已有分支触发 -> 需新增调用 vs_social_pause())
  -> vs_media 相机/音频 worker 收到暂停标志,停止 QBUF 循环但不 STREAMOFF
     (避免重新协商格式的开销),音频侧停止填充环形槙
  -> 不再调用 vs_cloud_social_upload()
  -> 投 VS_APP_EVENT_SOCIAL_PAUSED

Power 短按继续
  -> vs_media worker 恢复 QBUF 循环与音频填充
  -> 投 VS_APP_EVENT_SOCIAL_RESUMED
```

暂停期间 `vs_social` 仍然继续 `poll_event()`(低频,不因暂停而停止),以便拿到
暂停前已上传数据的迟到结果。

## 5. 代码落点与实施顺序

### 5.1 目标文件

```text
app/velasight/include/vs_cloud.h      填充:六个抽象操作 + 两套状态枚举
app/velasight/include/vs_social.h     填充:会话生命周期 API
app/velasight/include/vs_media.h      填充:常驻采集 API(与闲时方案共享文件,
                                       但函数不同,见闲时方案 4.1 节)
app/velasight/include/vs_app.h        修改:暂停/继续分支调用 vs_social_pause/resume
app/velasight/vs_cloud.c              填充:HTTP 映射、JSON 解析、状态机映射
app/velasight/vs_social.c             填充:会话编排、置信度策略、震动
app/velasight/vs_media.c              填充:相机/音频常驻 worker(与闲时方案的
                                       单帧拍照函数共存于同一文件)
app/velasight/vs_app.c                修改:调用 vs_social_start/pause/resume/finalize
app/velasight/CMakeLists.txt          修改:SRCS 增加三个源文件
app/velasight/Makefile                修改:CSRCS 同步
app/velasight/Kconfig                 修改:新增第 6.1 节选项
```

### 5.2 P0:云端接口对齐(不写代码,纯沟通与文档)

产出一份云端团队签字确认的补充说明,逐条对应下表:

| # | 待确认事项 | 当前设备端事实 | 需要云端给出 |
|---|---|---|---|
| 1 | 图像帧率 | 实测软件 JPEG 编码 316 ms/张 ≈ 3.16 FPS,硬件通路已禁用 | 是否接受 3 FPS 起步,云端缓存/拼接逻辑是否假设了固定的 200 ms 间隔 |
| 2 | `/contest/v1/upload` 语义 | 不确定是「登记后走预签名 URL 直传」还是「本身要求 multipart 携带二进制」 | 明确二选一;若后者,给出 multipart 字段名规范 |
| 3 | 音频格式 | 未定,现有可复用实现是 16 kHz/16bit/mono PCM 或 Opus | 采样率、位深、声道、编码格式、`fileType=1` 对应的具体二进制格式 |
| 4 | `ack` 接口缺口 | 云端文档只有 4 个接口,没有 ACK | 是否需要设备显式确认结果已保存,还是云端以「设备侧完成 finalize 轮询」为准自行清理 |
| 5 | `getResult` 的 `msgId` 语境 | 不确定同一 `msgId` 能否反复轮询、多个 in-flight 的 `msgId` 如何区分 poll 目标 | 一次 upload 产生几个 `msgId`;多张图片并发上传时如何各自查询结果 |
| 6 | 强烈情绪判定阈值 | 云端 4.4 节只给了颜色分类,没给触发阈值 | 极端情绪的置信度门槛、连续窗口数、去抖/冷却时间的建议值(设备侧仍会做本地二次去抖,但需要云端的基准) |
| 7 | 最终 TTS 交付方式 | 云端 4.4 节写 `ttsMinutes` 字段但格式是「音频 URL 还是二进制」表述不一致(3.获取结果小节写"xxx/xxx"疑似 URL,5.关闭会话小节注释写"音频 URL");若是 URL,时效性未知 | 明确 `ttsMinutes` 是下载 URL 还是需要设备自行调用另一个 TTS 接口;若是 URL,是否像上传用的预签名 URL 一样有过期时间——`vs_cloud_social_get_result()` 轮询可能跨越较长时间,需要知道拿到 URL 后能拖多久再下载 |
| 8 | 六种 mock 场景 | 无 | 提供不依赖真实模型的固定响应集:普通会话、强烈情绪、延迟建议、非法包、超时、最终成功,供 P4/P6 阶段离线联调 |
| 9 | 域名与证书 | 未知 | `$CLOUD_HOST` 实际域名、端口,以及是否可以先用 `openssl s_client` 取证书链(参照 MiMo 方案 2.2 节方法) |
| 10 | DELETE 异步性质 | 云端 4.1 节接口说明的措辞像同步返回,但 4.4 节响应参数只有 `msgId`,时序图也显示异步 | 书面确认 `DELETE /session` 是否立即返回,最终结果必须靠轮询 |

**P0 门禁**:以上 10 项全部有书面回复(哪怕是「暂定 X,后续可能调整」也算,只要
可执行),且第 1、2、3、4 项没有得到「无法确认」的回复 —— 这四项直接决定
`vs_media.c` 和 `vs_cloud.c` 的函数签名,拖到实现中期改会牵动大量代码。

### 5.3 P1:HTTP 客户端接入与连通性验证

```text
vs_cloud.c 骨架:
  #include "infra/vela_tls.h"
  封装 vs_cloud_http_call(method, path, body, body_len, resp, resp_cap)
    内部调 vela_https_request(),host/port 来自 Kconfig
接入 SNTP(复用 MiMo 方案 M0 阶段结论,若该阶段已在其他配置分支完成则跳过):
  defconfig 追加 CONFIG_NETUTILS_NTPCLIENT / CONFIG_SYSTEM_NTPC
  vs_network.c 的 STA 联网成功回调后追加 ntpcstart(参考
  network_wifi_reconnect() 的写法,或直接 system("ntpcstart"))
实板连通性验证(不依赖真实云端功能,只验证链路):
  nsh> ifup wlan0; wapi essid ...; renew wlan0
  nsh> ntpcstart && ntpcstatus && date
  nsh> velasight_cloud_probe $CLOUD_HOST 443 /contest/v1/session
       (P1 阶段临时加一个诊断子命令,或直接在 nsh 里用现成 net_test 的域名换成
       云端域名验证握手,验证后可删除)
```

**P1 门禁**:`date` 输出真实日期;对 `$CLOUD_HOST` 的 TLS 握手成功(不要求业务
返回 200,只要求握手不报 `VELA_TLS_ERR_HANDSHAKE`/`ERR_CONNECT`);若云端证书链
已通过 P0 第 9 项取证,记录证书链细节备后续接入 `VERIFY_REQUIRED` 用。

### 5.4 P2:`vs_media.c` 常驻采集(先只对着本地 mock,不接云端)

```text
相机 worker(按 2.3 节修正后的循环模板,UYVY 640x480 + /dev/video1 编码,
两个设备节点独立打开,不是 /dev/video0 直出 JPEG):
  open /dev/video0 一次,VIDIOC_S_FMT V4L2_PIX_FMT_UYVY 640x480 / REQBUFS 2 / mmap / STREAMON
  preview_jpeg_open(640, 480, quality) 一次(打开 /dev/video1)
  循环:DQBUF(video0,拿 UYVY 帧) -> 定速判断(316 ms 最小间隔)
       -> preview_jpeg_encode 或跳过 -> memcpy 到有界队列(2 槙即可,
       编码本身已经是天然限速器)-> QBUF(video0)
  收到停止标志:STREAMOFF -> REQBUFS(count=0) -> close(video0)
       + preview_jpeg_close(编码器)

音频 worker(照搬 audio_test_stream.c 的双线程环形槙结构):
  STREAM_SLOTS = 4,槙大小按 P0 确认的格式计算(PCM 16kHz/16bit/mono/2s = 64 KiB)
  capture 线程:只填槙,不做编码和网络
  独立的"编码/发送"线程:P2 阶段先只做编码(若 P0 确认要 Opus)或直接搬运
    PCM,发送目标先指向本地 mock(见下)

本地 mock 目标(P2 专用,验证采集与队列本身,不等云端接口对齐完成也能推进):
  相机队列产出的 JPEG 只做长度和 JPEG marker 校验后丢弃并计数
  音频队列产出的块只做长度校验后丢弃并计数
  两个 worker 独立跑 60 秒,统计:相机实际 FPS、丢帧数、音频丢块数、
  两个 worker 与 UI 主循环并发时的按键响应延迟
```

**P2 门禁**:相机 worker 连续运行 5 分钟不崩溃、`preview_jpeg_encode` 无
`VIDIOC_DQBUF failed`;实测 FPS 记录在案(预期接近 3.16,允许因并发音频而略低,
但不接受腰斩);音频 worker 连续运行 5 分钟丢块数为 0(在无网络拥塞的 mock 场景
下不应该丢);按键响应延迟(短按到软键高亮)不超过设计文档要求的 200 ms 窗口,
证明两个采集 worker 没有饿死 UI 主循环。

### 5.5 P3:`vs_cloud.c` 六个操作接云端(P0 完成后才能开始)

```text
按第 4.2 节的 HTTP 映射逐个实现:
  vs_cloud_social_open()      PUT,解析响应拿 status/result
  vs_cloud_social_upload()    POST 登记 + PUT presignedUrl 直传
                              (若 P0 第 2 项确认需要 multipart,这里插入
                               multipart 组装,工作量约半天)
  vs_cloud_social_poll_event() GET,解析 event 分支(4.3 节三种 JSON 形态)
  vs_cloud_social_finalize()  DELETE,只取 msgId
  vs_cloud_social_get_result() GET,轮询直到 server_state==CLOSED
  vs_cloud_social_ack()       本地空操作(3.4 节决策),打日志说明原因

所有响应缓冲从 PSRAM 分配,大小按 P0 确认的最大 payload estimate;关闭会话结果
(4.3 节 event=2)含 audioTimeline/emotionTimeline 数组,可能明显大于闲时模式的
32 KiB,建议起始值 64 KiB 并在 P6 压测阶段按实测调整。

若 P0 已提供 mock 场景(第 8 项),先对 mock 服务器跑通全部六个函数,再切换到
真实云端域名。
```

**P3 门禁**:六个函数在 mock 服务器上全部返回预期结构;对真实云端域名,
`open/upload/poll_event/finalize/get_result` 五个(ack 除外)每个至少有一次
成功往返的实板日志;JSON 解析对「字段缺失」「多一个未知字段」「数组为空」
三类输入均不崩溃。

### 5.6 P4:`vs_social.c` 会话编排

```text
vs_social_start(request_id):
  TRNG 生成 session_id(/dev/urandom 读 8 字节转十六进制)
  vs_cloud_social_open() -> 成功投 SOCIAL_STARTED,失败投 SOCIAL_START_FAILED
  启动 vs_media 相机 + 音频 worker
  启动低频 poll 线程(建议 1~2 秒一次,具体值等 P0 第 6 项后调整)

采集回调(vs_media 产出一帧/一块时调用):
  vs_cloud_social_upload() 拿 msg_id
  记录 msg_id 供 poll_event 关联(有界映射表,建议 16 项环形数组,
  与 audio_test_stream 的槙位数量级一致)

poll 线程:
  遍历待查 msg_id 调 vs_cloud_social_poll_event()
  按 4.1 节状态映射与 4.4 节两阶段事件逻辑投递 SOCIAL_ALERT / ALERT_CLEARED

vs_social_pause() / vs_social_resume():
  按 4.5 节控制 vs_media worker,投递 PAUSED/RESUMED
  (失败投 SOCIAL_PAUSE_FAILED,vs_app.c 已有对应分支)

vs_social_finalize(request_id):
  停止两个 vs_media worker(STREAMOFF,音频线程退出)
  上传尾包(可能不满 2 秒的最后一块音频,不丢弃)
  vs_cloud_social_finalize() 拿新 msg_id
  轮询 vs_cloud_social_get_result() 直到 CLOSED 或超时
  成功:解析 4.3 节 event=2 JSON -> vs_history_append()(复用闲时方案 P4)
       -> 投 SOCIAL_RESULT,text 放最终摘要(128 字节内,完整文本走
       voice_channel_speak,复用闲时方案的 TTS 播报路径)
  失败/超时:投 SOCIAL_FINALIZE_FAILED
  vs_cloud_social_ack()(当前空操作)
```

**P4 门禁**:见第 7 节验证矩阵的「社交」分组全部通过。

### 5.7 P5:与闲时模式共享 `vs_history`

社交模式的最终结果落盘复用闲时方案 P4 阶段建立的 `vs_history_append()`,不重复
实现事务写入逻辑。区别只在于 `vs_history_index_s` 的 `calm/happy/tense` 三个
比例字段由 `emotionTimeline` 统计得出,而不是闲时问答场景下的默认值。

若本方案先于闲时方案交付,`vs_history.c` 由本方案负责按闲时方案 P4 节的契约
实现,闲时方案后续直接复用,不重复开发;文档层面两份方案互相引用即可。

**同样需要交叉引用的是 `voice_channel_speak()`。** 最终 TTS 播报(第 5.6 节)
复用的是闲时方案的播报路径,而闲时方案 3.5 节已确认 `voice_channel_speak()`
依赖 `voice_channel_init()` 跑过 —— 这个 init 只在 `ai_agent_main()` 内被调用,
而 `ai_agent_main()` 没有任何自启动路径(取证见闲时方案 3.5 节,`board/` 下
`ai_agent_main`/`"ai_agent"` 零命中)。**本方案不需要重新分析这个问题**,但
落地顺序上,若社交模式先于闲时方案交付,`vs_voice_open()`(闲时方案 P1 的
决策五)必须提前实现并在 `vs_app_run()` 启动时调用,否则社交会话结束后
`voice_channel_speak(final_text)` 会因为 `ai_agent` 全局状态未初始化而静默
失败——不影响历史落盘(`vs_history_append()` 不依赖这条初始化),但用户听不到
最终 TTS。`vs_cloud.c` 复用的 `vela_tls.c`(第 2.1 节)不受这个问题影响,它用
`pthread_once` 自行完成一次性初始化,不依赖 `ai_agent_main()` 跑过。

### 5.8 P6:全链路取消与异常注入

```text
六类注入(对照第 7 节验证矩阵):
  弱网(限速/加延时代理)
  断网(拔 Wi-Fi 或强制 ifdown)
  云端 4xx/5xx(用 mock 服务器返回错误码)
  TLS 握手失败(mock 服务器不响应或证书错误)
  非法 JSON(mock 服务器返回截断/错误结构)
  长时间无响应(mock 服务器 delay 超过设备超时)

用户主动结束路径:
  长按返回键结束时立即停止采集,即使云端 finalize 还没返回
  finalize 轮询超时后的行为:标记 incomplete,仍尝试落盘已收到的部分结果
```

**P6 门禁**:见第 7 节。

### 5.9 P7:长稳与实板验收

```text
完整会话 20~30 秒 × 20 次,记录 heap_info 前后差值
暂停/继续交替 10 次
真实弱网环境(不是限速代理)下跑 5 次完整会话
```

## 6. 配置与构建执行清单

### 6.1 Kconfig 新增

```text
config VS_SOCIAL_CLOUD_HOST
	string "VelaSight social cloud host"
	default ""
	depends on LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
	---help---
		留空表示未配置,vs_social_start() 应直接失败并提示配置缺失,
		不得对空主机名发起连接尝试。

config VS_SOCIAL_CLOUD_PORT
	int "VelaSight social cloud port"
	default 443
	depends on LVX_USE_DEMO_CONTEST2026_264_VELASIGHT

config VS_SOCIAL_IMAGE_MIN_INTERVAL_MS
	int "VelaSight social image capture minimum interval (ms)"
	default 320
	range 200 2000
	depends on LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
	---help---
		按实测软件 JPEG 编码 316ms/张设定,P0 第 1 项确认后可调整。

config VS_SOCIAL_AUDIO_CHUNK_MS
	int "VelaSight social audio chunk duration (ms)"
	default 2000
	range 500 5000
	depends on LVX_USE_DEMO_CONTEST2026_264_VELASIGHT

config VS_SOCIAL_POLL_INTERVAL_MS
	int "VelaSight social event poll interval (ms)"
	default 1500
	range 500 5000
	depends on LVX_USE_DEMO_CONTEST2026_264_VELASIGHT

config VS_SOCIAL_FINALIZE_TIMEOUT_MS
	int "VelaSight social finalize polling timeout (ms)"
	default 30000
	range 5000 120000
	depends on LVX_USE_DEMO_CONTEST2026_264_VELASIGHT

config VS_SOCIAL_ALERT_DEBOUNCE_WINDOWS
	int "VelaSight social alert confirmation windows"
	default 3
	range 1 10
	depends on LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
	---help---
		连续多少次 poll_event 命中 IMAGE_EXTREME 才触发提醒;清除同理,
		避免情绪判定在阈值附近抖动。

config VS_SOCIAL_ALERT_COOLDOWN_MS
	int "VelaSight social alert cooldown (ms)"
	default 8000
	range 2000 60000
	depends on LVX_USE_DEMO_CONTEST2026_264_VELASIGHT

config VS_SOCIAL_RESP_MAX_BYTES
	int "VelaSight social cloud response buffer (bytes)"
	default 65536
	range 8192 262144
	depends on LVX_USE_DEMO_CONTEST2026_264_VELASIGHT

config VS_SOCIAL_MOCK
	bool "VelaSight social mode local mock (no cloud)"
	default n
	depends on LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
	---help---
		P2/P6 阶段联调用,启用后 vs_cloud.c 走本地固定响应而不发真实
		网络请求。交付镜像必须为 n。
```

### 6.2 源码加入

`app/velasight/CMakeLists.txt`:

```cmake
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT)
  nuttx_add_application(
    NAME velasight
     SRCS velasight_main.c vs_app.c vs_display.c vs_input.c vs_config.c
          vs_network.c vs_voice.c vs_media.c vs_history.c
          vs_cloud.c vs_social.c
          velasight_font_16_ui.c
     INCLUDE_DIRECTORIES ${NUTTX_APPS_DIR}/packages/ai_agent/include
                          ${NUTTX_APPS_DIR}/packages/ai_agent/src
                          ${NUTTX_APPS_DIR}/packages/demos/contest2026_264_provision_web/include
      DEPENDS lvgl provision_web_core
    STACKSIZE 8192)
endif()
```

(`vs_voice.c`/`vs_history.c` 来自闲时方案,若两份方案并行开发,以先落地者的
`CMakeLists.txt` 为准,后落地者补齐差集。)

`app/velasight/Makefile` 的 `CSRCS` 同步加入 `vs_cloud.c vs_social.c`。

### 6.3 构建与打包

```bash
cd /home/mi/vela_competition/contest

./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake distclean
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake -j8
```

产物与哈希核对流程同闲时方案第 6.3 节,不重复。

## 7. 验证矩阵

### 7.1 构建与静态检查

```text
--cmake 构建通过
CMakeLists.txt 与 Makefile 源列表逐项一致
System.map 中存在 vs_social_start / vs_cloud_social_open / vs_media 相机与音频入口
CONFIG_VS_SOCIAL_MOCK=n 的交付镜像不含 mock 分支
nuttx.bin 体积增量已记录
```

### 7.2 云端接口对齐(P0)

```text
第 5.2 节表格 10 项均有书面回复
第 1/2/3/4 项没有"无法确认"
```

### 7.3 采集

| 场景 | 通过条件 |
|---|---|
| 相机连续 5 分钟 | 无崩溃,实测 FPS 接近 3.16,记录丢帧数 |
| 音频连续 5 分钟(无网络拥塞) | 丢块数为 0 |
| 相机+音频+UI 并发 | 按键响应延迟不超过 200 ms |
| 暂停 | 两个 worker 停止上传,不 STREAMOFF |
| 继续 | 两个 worker 恢复,不重新协商格式 |

### 7.4 云端往返

| 场景 | 通过条件 |
|---|---|
| `open` | 拿到 `sessionId` 回显一致 |
| `upload`(图片) | 拿到 `msgId`,预签名 URL 直传成功 |
| `upload`(音频) | 同上 |
| `poll_event` 非极端 | 静默,不投 ALERT |
| `poll_event` 极端 | 投 ALERT,颜色+短文本正确,不播 TTS |
| `finalize` | 立即返回,不阻塞采集停止 |
| `get_result` 轮询 | 最终拿到 timeline/摘要/建议/TTS 描述 |
| JSON 异常(缺字段/多字段/空数组) | 不崩溃,记录解析失败但不中断会话 |

### 7.5 情绪两阶段与去抖

| 场景 | 通过条件 |
|---|---|
| 单次极端情绪帧 | 不因单帧触发,等够去抖窗口数 |
| 连续极端 | 达到窗口数后触发,颜色+文字同时出现 |
| 建议晚到 | 关联同一 alert 世代号才更新;世代号已变则丢弃 |
| 冷却期内再次极端 | 不重复触发提醒(除非强度显著更高,by 设计留开放,首版可先不做强度比较) |
| 恢复正常 | 连续窗口数满足后才 CLEARED,不抖动 |

### 7.6 异常注入(P6)

```text
弱网:上传变慢但不阻塞采集,poll 间隔可自适应退避
断网:采集继续,upload 失败计数,恢复后补传或跳过(不阻塞新数据)
4xx/5xx:明确失败事件,不重试风暴(退避策略参考 conv_serve.c 的
        serve_report_failure 抑制思路)
TLS 失败:vs_social_start 直接 SOCIAL_START_FAILED,不进入 RUNNING
非法 JSON:计入解析失败计数,不崩溃,不写坏数据到历史
超时:finalize 轮询超过 VS_SOCIAL_FINALIZE_TIMEOUT_MS 后标记 incomplete 并落盘
```

### 7.7 用户交互路径

```text
历史页长按 Power 满圈 -> 成功进入 RUNNING;提前松开 -> 取消,不生成 session_id
运行中 Power 短按 -> 暂停 -> 短按 -> 继续
运行中长按返回 -> FINALIZING -> 最终结果或失败,均能回到历史页
FINALIZING 期间再次长按返回(计划允许的二次取消) -> 停止等待,标记 incomplete
```

### 7.8 资源与长稳

```text
20~30 秒会话 × 20 次,heap_info 前后差值有界
暂停/继续交替 10 次无资源泄漏
真实弱网 5 次会话完整率记录在案
```

## 8. 交付物与回滚

每个阶段提交必须包含:

```text
源代码与 Kconfig 改动
构建命令与最终 .config
nuttx、nuttx.bin、System.map 的 sha256
nuttx.bin 体积增量
实板串口日志(含 heap_info 前后对比)
测试命令、输出与统计计数
失败场景注入方式与恢复结果
```

推荐拆分提交:

```text
docs: add VelaSight social mode integration plan
docs: record cloud interface alignment answers                    （P0）
feat(velasight): reuse vela_tls for the social cloud client        （P1）
feat(velasight): add resident camera and audio capture workers     （P2）
feat(velasight): implement the six cloud operations against mock   （P3）
feat(velasight): orchestrate the social session state machine      （P4）
feat(velasight): persist social results through vs_history         （P5）
test(velasight): social mode fault injection results                （P6）
test(velasight): social mode soak test results                     （P7）
```

出现以下任一情况立即回退到最近一个通过的阶段,**不得用加延时或放宽超时掩盖**:
原始图片或音频出现在文件系统中;暂停后媒体 worker 仍在上传;迟到的建议覆盖了
更新的情绪状态;`heap_info` 在长会话中单调增长;两套状态枚举被混用;`ack`
被实现为真实网络调用却没有云端接口支撑。

## 9. 完成标准

只有同时满足以下条件,才能称「社交辅助模式接入完成」:

1. 第 5.2 节 P0 清单 10 项全部有书面回复,且关键的第 1~4 项没有「无法确认」。
2. `vs_cloud.c` / `vs_social.c` / `vs_media.c` 均已进入 CMake 与 Make 两套源
   列表,`System.map` 可证明进入镜像。
3. 相机 worker 常驻持有 `/dev/video0` 完成整场会话采集,不逐帧 open/close;
   音频 worker 的环形槙设计与 `audio_test_stream.c` 同构并已验证不阻塞采集。
4. 六个 `vs_cloud` 抽象操作中,除 `ack`(按 3.4 节决策为本地空操作)外,五个
   均完成至少一次对真实云端域名的成功往返。
5. 强烈情绪走「即时颜色+短文本」与「建议异步补充」两阶段,期间不播 TTS;
   去抖与冷却窗口生效,情绪判定不因单帧抖动。
6. 会话正常结束时,`DELETE /session` 的异步语义被正确处理(不当作同步返回),
   最终结果通过轮询 `getResult` 获得。
7. 最终结果(摘要、建议、时间轴、TTS)通过 `vs_history_append()` 落盘,原始
   图片和音频在发送成功或会话结束后立即释放,不落盘、不长期驻留内存。
8. 暂停/继续能实际停止/恢复媒体采集与上传,不只是切换页面。
9. 两套状态码(对端侧/服务端侧)在代码中是两个独立类型,映射关系有注释说明
   出处。
10. `CONFIG_VS_SOCIAL_MOCK=n` 的交付镜像中不含 mock 路径。
11. 六类异常注入(弱网/断网/4xx5xx/TLS失败/非法JSON/超时)全部有明确处理,
    不崩溃、不写坏数据。
12. 20~30 秒会话 × 20 次、暂停继续交替 10 次的长稳测试通过,`heap_info` 无
    单调增长。
13. 日志与历史记录中不出现原始图片、原始音频、API 凭据或用户对话逐字转写
    以外的敏感信息(转写本身是产品要保存的内容,但凭据类信息不得出现)。

## 10. 风险与明确决策

| 风险 | 性质 | 处置 |
|---|---|---|
| 3 FPS vs 5 FPS 帧率冲突 | 文档级冲突,已确认 | 决策二:不修硬件通路,P0 申请降到 3 FPS |
| `/upload` multipart 语义不明 | 云端文档歧义 | P0 第 2 项;先按登记+预签名直传设计 |
| 音频格式未定 | 云端文档缺失 | P0 第 3 项;现有可复用实现是 PCM 或 Opus 两选一 |
| 六元抽象与四接口不对等,`ack` 无云端支撑 | 已确认的设计缺口 | 决策四:ack 暂为本地空操作 |
| `vela_tls.c` 当前 `VERIFY_OPTIONAL` | 已确认,安全折衷 | 决策三:先做 SNTP,证书链取证后再评估 `VERIFY_REQUIRED`;交付前必须在文档中声明现状 |
| `vela_tls.c` 授时失败时伪造 2026 时间 | 已确认,`packages/ai_agent` 现状 | 复用 MiMo 方案 M0/M1 的修复路径,不在本方案重复实现,但依赖其完成 |
| 云端域名证书链未知 | 待取证 | P0 第 9 项,方法参照 MiMo 方案 2.2 节 |
| `DELETE /session` 措辞歧义(同步/异步) | 云端文档内部不一致 | P0 第 10 项;设备侧按异步实现,即使云端确认同步也不会出错(轮询会立刻拿到结果) |
| 相机与音频并发对 UI 响应的影响 | 未知,需实测 | P2 门禁包含按键响应延迟测量 |
| TLS 连接池与闲时模式共享 | 已确认互斥即安全 | 决策一附带说明;若未来两模式并发运行需重新评估池大小 |
| 强烈情绪判定阈值无云端基准 | P0 第 6 项 | 设备侧仍做本地去抖(Kconfig 可调),即使云端阈值后调也不需要改代码结构 |
| `getResult` 的 `msgId` 并发语义不明 | P0 第 5 项 | 设备侧按「每个 msgId 独立查询,不假设服务端会话级聚合」设计,更保守 |

明确不做(本轮):STA+AP 并发情况下的社交模式(与 Wi-Fi SoftAP 移植方案的双网卡
阶段一样延后);硬件 JPEG 通路修复以突破 3 FPS 上限;5 FPS 挑战目标;长会话的
阶段性摘要(开发计划已标注为首版不实现);多设备并发会话的云端压力测试(那是
云端团队的职责);`ack` 接口的真实实现(等云端补充)。

## 11. 稳定引用

VelaSight App:

```text
app/velasight/vs_app.c                        社交九个页面的状态机与事件分支(已实现)
app/velasight/include/vs_app.h                社交九个事件枚举
app/velasight/include/vs_types.h              vs_emotion_e、VS_TEXT_LONG
```

可复用的既有实现:

```text
app/social_cue/social_cue_main.c              置信度阈值、拒答策略、PWM 震动、
                                               sc_capture() 的 V4L2 单帧序列
app/camera_preview/camera_preview_main.c      常驻持有 /dev/video0 的主循环模板
app/camera_preview/preview_jpeg.h             硬件 JPEG M2M 封装(拷贝+编码分离计时)
app/audio_test/audio_test_stream.c            环形槙 + 双线程 + 丢最老槙的设计
app/audio_test/audio_test_ogg.c               Opus 编码器(38 KiB 状态 + 62 KiB scratch)
app/conv/conv_serve.c                         serve_report_failure 的失败退避打印节奏,
                                               可用于 poll_event 失败时的日志抑制
```

`ai_agent` 侧接口与实现:

```text
apps/packages/ai_agent/src/infra/vela_tls.h   通用 HTTPS 客户端,本方案主要复用对象
apps/packages/ai_agent/src/infra/vela_tls.c   连接池、DNS、chunked 解码、授时保护现状
```

云端协议(取证于 2026-08-21 读取的 `OpenVela比赛云端技术方案.docx`):

```text
四、云端对设备端  1.建立会话(PUT) 2.上传文件(POST) 3.获取结果(GET) 4.结束会话(DELETE)
五、云端对 AI 端  1.接收 AI 端识别结果(POST /contest/v1/pushResult)
六、AI 端对云端   1.推送数据(POST /contest/v1/pushData)
三、流程设计      3.1 状态流转图(两套状态码的唯一图示来源)
```

其他移植方案(方法可借鉴):

```text
docs/plans/BK7258_OPENVELA_MIMO_NETWORK_PORTING_PLAN.md   SNTP/CA/VERIFY_REQUIRED
                                                           的阶段化接入路径,2.2 节
                                                           的证书链取证方法可直接
                                                           套用到云端域名
docs/plans/VELASIGHT_IDLE_AI_MODE_INTEGRATION_PLAN.md     vs_history 的契约与 P4
                                                           实现,本方案 P5 直接依赖
```

平台与配置:

```text
board/beken/chips/bk7258/bk7258_trng.c                    /dev/random 注册,session_id 生成用
使用说明-camera-display-ai_agent.md                        10.10 节软件 JPEG 收口结论、
                                                           一节的帧率与延迟实测表
```

行号不是稳定接口。后续引用优先使用函数名、宏名与文件路径。云端协议章节编号
可能随文档修订变化,引用时以最新版 `OpenVela比赛云端技术方案.docx` 的章节标题
重新核对。
