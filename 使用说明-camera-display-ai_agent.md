> **2026-08-18 最终策略更新：** 本文中“仅软件 JPEG”“480x480 多帧失败”以及“硬件 JPEG 普遍不可用”的段落是修复前的历史测量。当前固件在 480x480/640x480 使用完整熵校验、fail-closed 的硬件 JPEG；864x480 因解码像素不稳定自动回退软件。最终证据见 `docs/2026-08-18-三项修复实测记录.md`。

# Camera / Display / AI Agent 使用说明

* 更新当前文档时需要同步更新URL：[https://mi.feishu.cn/wiki/IbMQwpO0siolh9kIiiQcNbQIn6d](https://mi.feishu.cn/wiki/IbMQwpO0siolh9kIiiQcNbQIn6d)



板上三条通路的命令与**每条命令的可用状态**。状态不是设计意图，是 2026-08-14 在
BK7258 DevKit 上逐条敲出来的结果。

- 固件：`ai_agent` 配置。FLASH 795456 B / 1088 KB = 71.4%，静态 RAM 187136 B / 336 KB = 54.4%
- 进 AP 控制台：CP 侧 `ap_console open`；退出按 `Ctrl-]`、松手、再按 `.`
- 状态标记：
  - ✅ **实测通过** — 本次跑过并给出预期输出
  - ⚠️ **有前置条件 / 有坑** — 命令可用但有陷阱，写在备注里
  - ❌ **本配置不可用 / 已不适用**
  - ⬜ **未实测** — 存在但本次没跑，不要当成可用

<!-- markdownlint-disable-next-line -->
> **先读这条**：飞书《BK7258 AIDK 上板命令参考》（2026-08-12）有五处已经过时甚至有害，
> 逐条列在第七节。本文是当前事实。

---

## 一、双屏分工：一屏实时预览 + 一屏表情

这是 VelaSight 的目标形态，`camera_preview live+face` 已落地。

| 命令 | 状态 | 说明与实测输出 |
|---|---|---|
| `camera_preview live+face` | ✅ | fb0 实时预览、fb1 表情。实测 **29.22 fps**（`convert 8ms/f, push 24ms/f, 0 errors`） |
| `camera_preview live+face 40` | ✅ | 跑 40 帧后退出 |
| `camera_preview live+face expr=smile` | ⬜ | 指定初始表情；名字错会列出可用值 |
| `camera_preview live+face 40 cycle=1` | ✅ | 每秒轮换表情，实测打印 `preview: expression -> smile` |
| `camera_preview live+face 120 src=/mnt/ram/expr.txt` | ✅ | **表情由文件驱动**——识别方写一个词进去即可：`echo smile > /mnt/ram/expr.txt` → `preview: expression <- smile`。用文件而非 IPC，因为 `/dev/video0` 独占，识别方在预览持有相机时打不开设备 |
| `camera_preview live+face 120 src=/mnt/ram/expr.txt jpeg=60` | ✅ | 双屏 + 外部表情 + 每 60 帧产出上传用 JPEG，实测 **25.07 fps** |

**为什么要分工**，实测对比：

| 配置 | 显示帧率 | 相机帧率 |
|---|---|---|
| 单屏预览（`fb=0`） | 28.7 fps | 29.2 fps |
| 双屏都推相机帧（默认） | **16.8 fps** | 29.4 fps（丢 45/105 帧） |
| `live+face`（一屏相机 + 一屏表情） | **29.2 fps** | 29.6 fps |

一次推屏 51200 字节要 24–26 ms，两屏串行就是 50 ms/帧。表情屏只在表情变化时重画，
所以不进每帧路径，预览就能跑满相机速率。

## 一点五、识别性能：决定"实时"是几秒级

模型两行用真 key + 从本板取回的真帧在主机测，其余板上测：

| 环节 | 实测 |
|---|---|
| 文本 `ask` 往返（板上） | `llm_ms=5091`（主机同 15 KB 请求 4.6 s） |
| 视觉 480x480（43859 B → base64 58480 B） | **14.4–14.8 s**，`image_tokens=225` |
| 视觉 240x240（8941 B → base64 11924 B） | **3.6 s**，`image_tokens=64`，答案仍正确 |
| 预览流出 JPEG（480x480） | 313 ms（copy 27 + 软件编码 286） |
| 表情渲染 + 推屏 | 24–26 ms |

**480x480 到 240x240 是 4 倍差距**，而对占满画面的人脸没有质量损失——这是识别回路上最大的
杠杆。据此端到端刷新周期：480x480 约 **15 s**，240x240 约 **4 s**。预览始终 29 fps 不受影响。

模型的拒绝路径在真实调用中生效：对着空桌子的帧返回 `unable_to_judge: true, reason: no_face`。

## 二、从预览流产出 JPEG（上传用）

`/dev/video0` 只有一个拥有者：预览开着时别的进程打不开相机。所以持有相机的进程必须同时
产出上传用的 JPEG —— 这是 `camera_preview` 的 `jpeg` 选项。

| 命令 | 状态 | 说明与实测输出 |
|---|---|---|
| `camera_preview 90 fb=0 jpeg=30` | ✅ | 每 30 帧编一张：`jpeg #1 35064 bytes (copy 27ms + codec 289ms), SOI=ffd8 EOI=ffd9` |
| `camera_preview jpeg` | ⬜ | 默认每 60 帧一张（≈2 s，与方案文档的 Tool 周期一致） |
| `camera_preview 90 fb=0 jpeg=30 jpegout=/mnt/ram/pv.jpg` | ✅ | 首张落盘：`wrote 35064 bytes to /mnt/ram/pv.jpg`，`ls -l /mnt/ram` 核对一致 |
| `q=<1..100>` | ⬜ | JPEG 质量，默认 80 |

<!-- markdownlint-disable-next-line -->
> **`/dev/video1` 是软件编码器，不是硬件 JPEG 块。** 芯片的硬件 JPEG 由 DVP 直接喂，
> 读不了内存，所以撑不起 M2M 编解码器（`CONFIG_BK7258_JPEG_ENC` 的 Kconfig 帮助里写得
> 很清楚）。硬件那条路只在采集侧，就是 `agent_camera` 用的那条。

实测代价：**copy 27 ms + 编码 289 ms ≈ 316 ms/张**。开销分布很关键：

- 拷贝只有 27 ms（因为用的是 32 位字循环；这个 libc 的 `memcpy` 是逐字节的，会慢一个量级）
- 想去掉这 27 ms 得让编码器直接读相机缓冲，但 `bk7258_jpeg_addr_ok()` 只接受它自己分配的
  池 —— 那是防止错误偏移把 DMA 指向任意内存的保护，放宽到 PSRAM 媒体窗口是一次独立的驱动
  改动，不该顺手塞进应用里
- 编码 289 ms 是软件 libjpeg-turbo 在 Cortex-M33 XIP 上的真实速度，压不下去

对预览的影响：`jpeg=30` 时从 29 fps 掉到 22.2 fps；按方案文档的 2 秒节奏（`jpeg=60`）
每 60 帧掉一次，算术上约 25–26 fps。

## 三、Camera

`/dev/video0`。分辨率**只有** 480x480 / 640x480 / 864x480，驱动精确匹配，其他几何 `EINVAL`。

| 命令 | 状态 | 说明与实测输出 |
|---|---|---|
| `agent_camera` | ✅ | auto 档拍一帧：`auto-selected 480x480`、`bytesused=61347`、`SOF=yes DQT=yes DHT=yes SOS=yes EOI=yes`、`OK` |
| `agent_camera 640x480` | ✅ | 指定几何：`bytesused=28707`，五项全 yes |
| `agent_camera caps` | ✅ | `driver enumerates 6 JPEG size(s)`，UYVY 与 JPEG 各 6 档 |
| `agent_camera n=30` | ✅ | 连拍 30 帧：`timeouts=0 measured=16.55 fps`、`short=0 resets=1 hdr_fail=0 eoi_delta=0` |
| `agent_camera out=/mnt/ram/cap.jpg` | ✅ | `wrote 59851 bytes to /mnt/ram/cap.jpg` |
| `agent_camera b64` | ✅ | 见第五节，取回后主机零警告解码 |
| `agent_camera low` | ✅ | 复现 ai_agent 默认请求：`S_FMT JPEG 320x180 failed: 22` → `320x180 refused, using enumerated 480x480` → `OK` |
| `agent_camera low strict` | ✅ | 不协商，`FAILED (-22)`——这正是未打补丁的 ai_agent 的行为 |
| `nxcamera` | ✅ | 交互式：`input /dev/video0` → `output /mnt/ram/n.jpg 1` → `stream 480 480 30 JPEG` → `q`，产出 18567 字节 |
| `jpeg_test` | ⬜ | 软件 M2M 编解码器自测，本配置已启用（`enc`/`show`/`cam`/`dump`/`info`） |

**五项全 yes 才是标准 JPEG。** 只看 SOI/EOI 不够：硬件写进码流的 AC 霍夫曼表与它实际
编码用的表不一致、且不输出 SOS，驱动会在交付前重写标准头。最终判据是主机上真解码器
渲染出来。

## 四、Display

双屏 GC9D01 160x160 RGB565，开机注册 `/dev/fb0`、`/dev/fb1`。

| 命令 | 状态 | 说明与实测输出 |
|---|---|---|
| （开机自动） | ✅ | 逐笔画 "hello vela"：`greeting written in 1677 ms (20 steps, 330 px path, em 39, pen 3)` |
| `hello` | ✅ | `'hello' on 2 panel(s), 44px em, pen 3px` → `written in 1473ms, 21 frame(s)` |
| `hello ni hao` / `em=` / `thick=` / `ms=` / `hold=` / `loop` / `fb=0` | ⬜ | 任意文字与参数（字形表只有 a-z 与 `. , - !`） |
| `camera_preview fill f800` | ✅ | 纯色，不碰摄像头：`drew 'fill' on 160x160, 2 panel(s)` |
| `camera_preview grid` | ✅ | 单像素棋盘，暴露传输丢字节 |
| `camera_preview pattern` | ✅ | 四象限 |
| `camera_preview bars` | ✅ | 彩条 |
| `camera_preview face` | ✅ | 不带名字时列出表情（`neutral`/`smile`/`sad`/`angry`/`surprise` …） |
| `camera_preview 30 yuyv` | ✅ | 按 YUYV 解析做 A/B：`decode=YUYV` |
| `camera_preview 30 sat=200` | ✅ | `decode=VYUY-R sat=200%` |
| `camera_preview 30 fb=1` | ✅ | 只驱动 fb1：`1 panel(s)`、27.8 fps |
| `camera_preview stats` / `bench` | ⬜ | 通道统计与访存代价表 |

<!-- markdownlint-disable-next-line -->
> **修掉了一个会污染所有 A/B 对比的缺陷。** NuttX flat build 里 builtin 的静态存储跨调用
> 保留，所以 `camera_preview 30 yuyv` 之后再跑 `camera_preview 30 sat=200`，第二次仍然是
> YUYV —— 而且它的 banner 会照实打印出来，没人要求过。任何按这个顺序做的 A/B 都在比较
> 两个同时变了的东西。现在 main 入口把整套选项恢复默认（原先只恢复了 `g_stop`）。

## 五、把图拉回主机

### 5.1 base64（推荐）✅

```bash
./serial_cmd.sh -w 100 -o /tmp/b64.log 'agent_camera b64'

sed -n '/BEGIN AGENT_CAMERA/,/END AGENT_CAMERA/p' /tmp/b64.log \
  | sed 's/^ap0: //' | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' \
  | sed '1d;$d' | tr -d ' \r\n' | base64 -d > /tmp/cap.jpg

identify /tmp/cap.jpg
python3 -c "from PIL import Image; im=Image.open('/tmp/cap.jpg'); im.load(); print(im.size)"
```

实测：围栏声明 43859 字节，取回 43859 字节，`identify` 报
`JPEG 480x480 8-bit sRGB 43859B` **零警告**，PIL 解码 `(480, 480) RGB`。

注意 `sed 's/^ap0: //'` 不能省——控制台在 CP 侧时 AP 的输出带 `ap0: ` 前缀，不去掉解出来
是垃圾而长度看着还合理。

### 5.2 hexdump ⚠️ 实测不可靠

文档里的备用路径**在 115200 控制台上会大量丢数据**。实测取 29419 字节的文件：

```
parsed 434 hexdump lines, 6560 bytes recovered, wrote /tmp/s.jpg (29419 bytes)
WARNING: 22859 bytes missing (zero filled); first gap at 0xd30
```

只收回 22%，其余零填充。**而 `identify` 依然"成功"**（`JPEG 640x480 … 29419B`），因为它
只读文件头——这正是"工具退出 0 不等于数据完整"的典型。要用 hexdump 就必须核对
`hexdump2raw.py` 的 missing 计数，不能只看 `identify`。

## 六、AI Agent

`ai_agent` 启动后进入 `vela>`，`quit` 退出。数据目录 `/mnt/sdnand/ai_agent`，开机自动挂载。

### 6.1 启动与网络

| 命令 | 状态 | 说明与实测输出 |
|---|---|---|
| `ai_agent` | ✅ | 约 2.7 s 就绪；`[cfgstore] Config store ready at /mnt/sdnand/ai_agent/config/config.json`、10 个内置 skill 全部写入、无 `Cannot write skill` |
| `heap_info` | ✅ | `arena=6448384 fordblks(free)=6104768 uordblks(used)=343616` |
| `config_show` | ✅ | 逐项列出配置，未设的显示 `(not set)`，key 只显示前 4 位 |
| `quit` | ✅ | 回到 nsh |
| `net_status` | ⚠️ | **不能当关联判据**：接口 DOWN 时也打印 `Network connected: yes`（只检查有没有配上地址）。看 `ifconfig wlan0` 是否 `RUNNING` |
| `net_test` | ✅ | `Handshake OK: TLSv1.2 / TLS-DHE-RSA-WITH-AES-256-CBC-SHA` → `SUCCESS! HTTP Status: 200` |
| `memory_read` / `session_list` | ✅ | 见前次记录 |
| `set_wifi` / `wifi_reconnect` | ⬜ | agent 侧配网 |
| `show_chat` | ❌ | 未编入本配置：`Unknown command: show_chat` |
| `set_feishu_app` / `set_feishu_user_token` | ❌ | help 里有，但 `CONFIG_AI_AGENT_FEISHU` 未开 |

配网（开机不自动联网，`CONFIG_NETINIT_NETLOCAL`）：

```sh
ifup wlan0
wapi scan wlan0                 # encode=8000 是开放网络
wapi psk wlan0 <密码> 3         # 开放热点跳过
wapi essid wlan0 <SSID> 1
renew wlan0                     # 首次可能失败，重试一次
ifconfig wlan0                  # RUNNING 且地址不是 10.0.0.2 才算成
```

**`ping` 不能当判据**：实测环境屏蔽 ICMP，`ping 8.8.8.8` 与 `ping api.xiaomimimo.com`
都 100% 丢包，而同一时刻 DNS 解析和 TLS 握手都正常。用 DNS 是否解析出 IP，或 `net_test`。

### 6.2 接 MiMo v2.5（文本 + 视觉）

网络侧已验证到 MiMo：DNS 解析 `api.xiaomimimo.com → 220.181.104.191`，用占位 key 发起
请求时 **TLS 握手成功**（`Handshake OK: TLSv1.2`），请求体 15314 字节发出，服务端拒绝。
所以**只缺一个真 key**。

<!-- markdownlint-disable-next-line -->
> ❗ **agent 内置的两个 mimo 预设都是已下线的模型名，不要用。** MiMo-V2 系列已于
> 2026-06-30 下线（官方文档首屏公告）。而 `cmd_llm.c` 的预设写的是文本
> `mimo-v2-flash`、视觉 `mimo-v2-omni`，两个都失效了。图片理解目前**只支持
> `mimo-v2.5`** 这一个模型名（官方《图片理解》文档「支持的模型列表」原话），文本也用
> 同一个名字。所以必须走"host + model + key"的三参数形式，跳过预设。

对齐官方文档的事实：

| 项 | 值 | 依据 |
|---|---|---|
| Base URL | `https://api.xiaomimimo.com/v1` | 官方 OpenAI SDK 示例 |
| 路径 | `/v1/chat/completions` | 同上 |
| 模型 | **`mimo-v2.5`**（文本与图片同一个） | 《图片理解》「当前仅支持 `mimo-v2.5` 模型」 |
| 图片传入 | `image_url.url = data:{MIME};base64,<b64>`，单张 ≤ 50 MB | 《图片理解》「Base64 编码传入」 |
| token 参数 | `max_completion_tokens` | 官方示例；agent 的 `is_openai_compat_host()` 已把 `xiaomimimo.com` 判为 OpenAI 兼容，会自动用这个字段 ✅ |
| 图片格式 | JPEG / PNG / GIF / WebP / BMP | 《图片理解》「图片限制」 |

**好消息是 agent 侧不需要改代码**：`llm_vision.c:99` 拼的就是
`data:%s;base64,%s`，与 MiMo 要求的形式一致；`llm_proxy.c:56-62` 已针对
`xiaomimimo.com` 选择 `max_completion_tokens`。我们 480x480 的帧约 44 KB，base64 后约
59 KB，远低于 50 MB 上限；图片 token 按官方缩放规则约 `(480/16)²/4 = 225` 个。

**方式 A：运行期命令（最快，不用重编）**

```
ai_agent
set_llm        api.xiaomimimo.com mimo-v2.5 <你的-mimo-api-key>
set_vision_llm api.xiaomimimo.com mimo-v2.5 <你的-mimo-api-key>
config_show                      # 确认 Host / Model / Vision Model 都是 mimo-v2.5
ask 你好
```

实测 `set_llm` 的输出形态（当时用的是预设，模型名是旧的）：

```
[llm_router] Backend 0 configured: api.xiaomimimo.com
[llm] LLM config updated atomically: api.xiaomimimo.com/v1/chat/completions (model: ...)
API key saved.
```

<!-- markdownlint-disable-next-line -->
> ⚠️ **保存位置是 `/mnt/sdnand/ai_agent/config/config.json`。** `/mnt/sdnand` 是 SD-NAND
> 上的持久 VFAT，掉电不丢；掉电即失的是 `/mnt/ram`（PSRAM ramdisk，用于放大文件的临时盘）。
> 键名：`api_key` / `model` / `llm_host` / `llm_path` /
> `vision_model` / `vision_host` / `vision_api_key`，也可以直接编辑该 JSON 后重启 agent。

**方式 B：编译期内置（跨重启，要重编重烧）**

`agent_config.h` 有这段：

```c
#if __has_include("agent_secrets.h")
#include "agent_secrets.h"
#endif
```

所以新建 `packages/ai_agent/include/agent_secrets.h`：

```c
/* 本地文件，切勿提交：packages/ai_agent 是公共仓 */
#define AGENT_SECRET_API_KEY "<你的-mimo-api-key>"
#define AGENT_SECRET_MODEL   "mimo-v2.5"
```

`llm_proxy.c:110-113` 在 init 时把它们装进运行期配置。然后重编 + `./autoflash.sh -A`。

<!-- markdownlint-disable-next-line -->
> ⚠️ 两个风险要当真：`packages/ai_agent` 是**公共仓**且没忽略这个文件，`git add` 一不小心
> 就把 key 提上去；key 也会明文进入固件 `.bin`，带 key 的固件不要外发。方案文档 9.2 的
> 要求是"Token 通过配置注入，不写入代码仓、README、串口日志或 AI Coding 日志"。

**如果返回 401**：MiMo 的 curl 示例用的是 `api-key: <key>` 头，而 agent 发的是
OpenAI 风格的 `Authorization: Bearer <key>`（`llm_proxy.c:344-346`）。官方同时提供
OpenAI SDK 示例（SDK 发的就是 Bearer），所以 Bearer 应当可用；万一被拒，就是要在
`llm_proxy.c` 加一个 `api-key` 头，属于上游补丁范畴。

**表情识别这条链的完整形态**：`camera_preview ... jpeg` 产出 JPEG（第二节）→ base64 →
`data:image/jpeg;base64,...` 交给 `mimo-v2.5` → 按 skill 文档
（`ai_agent/skills/social-cue-assistant.md`）要求的 JSON 契约返回线索与置信度 →
`social_cue` 的策略过滤 → 双屏的表情屏（第一节）。目前缺的只有 key。

**当前 HTTPS 只加密、未验证证书**：握手日志里 `UNIX=432` 是开机秒数（无 RTC/SNTP，上游
强行把时钟设成 2026），且 `vela_tls.c` 用 `MBEDTLS_SSL_VERIFY_OPTIONAL`、未装 CA bundle。
往云端传人脸之前应按 SNTP → 根证书 → `VERIFY_REQUIRED` 的顺序补齐。

### 6.3 表情线索闭环 `social_cue`

| 命令 | 状态 | 说明 |
|---|---|---|
| `social_cue` | ✅ | 真机采集 + Mock 分析，走完整状态机 |
| `social_cue mock case 0..4` | ✅ | 0 清晰 / 1 弱 / 2 低于下限 / 3 无法判断 / 4 线索冲突 |
| `social_cue schema` | ⬜ | 打印 JSON 契约与阈值 |
| `social_cue install` | ✅ | 把 skill 文档写进 `/mnt/sdnand/ai_agent/skills/`（4767 字节） |


另有一条环境相关的：`serial_cmd.sh -r` 复位后只等 4 s 就发 `ap_console open`，而当前固件
到 NSH 提示符要约 6 s（开机动画 1.7 s + camera/audio/jpeg 注册），所以 `-r` 之后的命令**可能
被丢弃**（会看到 `AP console input drops`）。稳妥做法是分两步：

```sh
./serial_cmd.sh -r -w 14 >/dev/null            # 只复位
./serial_cmd.sh -w 60 'ap_console open' '你的命令'
```

## 八、构建、打包、烧录

```bash
# 桌面单测（不需要硬件）——实测 all checks passed + test_face + test_jpeg_enc
(cd contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/chips/bk7258/sim_tests && make)

# 构建（必须走 cmake 路径）
# nsh 只是最小启动/对照配置；最终 VelaSight 固件必须使用 ai_agent。
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake -j8   # 不能加 -Werror

# 打包 + 一致性校验（不能省）
cp cmake_out/bk7258-ap_ai_agent/nuttx.bin bk_avdk_smp/build/openvela-ap.bin
(cd bk_avdk_smp && podman run --rm --userns=keep-id -v "$PWD:/armino" -w /armino \
  localhost/bekencorp/armino-idk:1.5 make -C projects/app_ab bk7258 \
  SDK_DIR=/armino EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin)
sha256sum cmake_out/bk7258-ap_ai_agent/nuttx.bin \
  bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin   # 两份必须相同

# 烧录
./autoflash.sh          # 全烧，约 33-40 s
./autoflash.sh -A       # 只烧 AP 分区，约 20 s（只改了 AP 侧代码时用）
./autoflash.sh -t       # 只测握手，0.3 s
```

改了 defconfig 必须 `rm -rf cmake_out/bk7258-ap_<config>` 再构建，否则改动不生效。
烧录波特率别往上加：2000000 不比 1500000 快（瓶颈是 flash 编程），3000000 会在擦除完成
之后失败并把板子留成空的。

`apply.sh` 实测幂等：重复执行报 `already applied`，`--revert` 报 `reverted`，再执行报
`applied`。

## 配置持久化：NAND 配网文件（KVDB 已废弃）

VelaSight 产品不使用 KVDB。配网网页一次提交 Wi-Fi 名称、Wi-Fi 密码和 MiMo API key，统一写入
SD-NAND 文件 `/mnt/sdnand/prov/vela.cfg`。应用和 ai_agent 都从这一个文件读取，避免配置分裂。
提交成功后，应用重新生成 ai_agent 的 `llm_backend_0` 配置；API key 不进入镜像，也不进入公共仓。

```bash
provision_web run --one-shot           # SoftAP 中打开网页完成配置
provision_web show                     # 显示已存 Wi-Fi 名称，不显示密码和 API key
provision_web path                     # 显示统一 NAND 文件路径
```

配网提交后，应用读取同一 `vela.cfg`，异步执行 STA 关联和 DHCP。未成功连接 Wi-Fi 时保持离线
主页，不把普通连接失败显示为错误页面。

**只有 IP 地址还需要一条命令**：DHCP 客户端在 apps 侧，内核侧取不到，所以关联之后仍要
`renew wlan0`（首次可能失败一次，重试即可，见 `docs/WiFi使用说明.md`）。

```
kvdb: associating with <SSID> (WPA2) -- run `renew wlan0` for an address
```

旧 `kvdb` 命令和 AP flash KVDB 代码仅为历史/诊断兼容保留，不属于 VelaSight 配置链路，后续不应
在产品代码中新增调用。

## 十、2026-08-17 复测：一处驱动缺陷已修，六处判据更新

> 本节与飞书《BK7258 AIDK 上板命令参考》第十节同步。全部来自当日实际执行输出。

### 10.1 `agent_camera n>1` 只出一帧（已修）

症状：第 0 帧正常，之后固定 `no frame within 5000 ms` → `FAILED (-110)`。

已排除：WiFi（`ifdown wlan0` 后一样）、`tcp=`（不带也一样）、工作队列被占（`ps` 显示 `lpwork` 空闲）、应用（`poll → DQBUF → QBUF` 闭合，re-QBUF 无报错）。

定责：在 `set_buf()` 加只统计 streaming 期间调用的 `set_buf_live`，实测 `set_buf_live=1 armed=0 encoding=0` —— 框架确实重新 arm 过，责任在驱动的编码 work。

根因：`bk7258_camera_sw_jpeg_work()` 的三条提前返回（`!capturing || frame_buf == NULL || g_sw_jpeg == NULL`、`frame_buf_size == 0`、`jpeg_raw_ready < 0`）既不调 `capture_cb` 也不还回 `jpeg_buf_armed`，而 `frame_done()` 排入 work 前已清掉 arm，于是握手永久断开。

修法：每条不完成 buffer 的出口都还回 arm（`priv->jpeg_buf_armed = priv->capturing;`）。

| 指标 | 改前 | 改后 |
|---|---|---|
| 交付帧（`n=8`） | 1，然后 `FAILED (-110)` | 8/8，`agent_camera: OK` |
| `set_buf_live` / `armed` | 1 / 0 | 8 / 1 |
| `sampler_skipped` | 0 | 67 |

画质：`n=6 rec=6`，6/6 通过长度 + FNV-1a；1–5 帧 luma 113.1–113.3、Cr 127.5、R−B 0.57–0.72。

### 10.2 `/dev/video1` 是软件编码器（15 帧确认）

`camera_preview 600 jpeg=40 jpegb64` 取 15 帧全部一致：luma 110.3–120.9、Cr 129.8–130.3、饱和 1.25–1.62%、结构逐帧相同（`E0,DB,DB,C0,C4×4,SOS@609`）、`rst=0`、无 DRI。640x480 的 `codec 570ms` 即软件编码在 XIP 上的代价。

### 10.3 硬件采集通路（`CONFIG_BK7258_CAMERA_JPEG_SW=n`）仍不可用

15 帧中 11 帧坏。判据：好帧 entropy=884（`comlen=277`），坏帧 entropy=868（`comlen=261`），差 16 字节；坏帧声明的 entropy 起点处残留硬件 SOS 字节（`11 00 3f 00`、`00 0c 03 01 …`）被当画面解码。

根因：硬件确实输出自己的 SOS（与 `camera.md` §14.6 相反），但其前的 `0xFF` 填充长度逐帧变化，而 `write_header()` 以「第一个非 FF 字节」判头结束。

已修两处并通过桌面单测（新增 `test_write_header_entropy_starts_with_ff`、`test_write_header_consumes_shifted_sos`）：marker 白名单（修前 entropy 685→17136）、前向扫描吃掉硬件 SOS。**修复后的 15 帧复测未完成，暂勿依赖该配置。**

### 10.4 第三条取回路径：`agent_camera tcp=<ip>:<port>`

裸 JPEG、一帧一连接、关闭即分帧；主机用 `tools/tcpframes.py OUTDIR [PORT] [COUNT]` 或 `nc -l -p 5000 > f.jpg`。控制台地板 4.3 s/帧（11.5KB/s，base64 36KB→49KB），5fps 640x480 需 125KB/s，控制台达不到。

**吞吐未实测**：AIPC(192.168.31.x) 到公司网主机 `ENETUNREACH(101)`；MIPublic(10.192.105.x) 有路由但 `ETIMEDOUT(110)`，同时主机 `0.0.0.0:5000` 确认 LISTEN 且自连成功。需接收端与板子同网段且不拦 TCP 出口。

### 10.5 配网判据：新增第五个陷阱

- **新增**：MIPublic 这类网络拦 TCP 出口但放行 DNS/ICMP。`ping` 通、`net_status: yes`，而 `net_test` 报 `net_connect ... ret=0x52`（`MBEDTLS_ERR_NET_CONNECT_FAILED`）。判连通只能用 `net_test` 的 `Handshake OK` + `HTTP Status: 200`。
- **修正**：ICMP 并非所有网络都屏蔽。AIPC 实测 `ping -c 2 www.baidu.com` 解析 `153.3.238.127`、9.0/23.0 ms、0% 丢包。「ping 不通」不证明网坏，「ping 通」不证明 TCP 可用。

关联参照：`wapi psk wlan0 <密码> 3`、`wapi essid wlan0 AIPC 1`、`renew wlan0`（第一次必失败报 `netlib_obtain_ipv4addr() failed`，第二次成功），`ifconfig wlan0` 显示 `RUNNING` 且 `192.168.31.30`。

### 10.6 退出 `vela>` REPL 用 Ctrl-C

实测 `exit` 不能离开 REPL（之后 `ap_console open` 回 `Unknown command: ap_console`，像串口坏了）。`Ctrl-C`（0x03）可靠回到 `nsh>`；`serial_cmd.sh` 里写 `'\003'`。`quit` 未单独验证。

### 10.7 复位到出屏 1.8 s；两条复位路径差 8 秒

`configs/ai_agent` 实测（从写下 `reboot` 起算）：首字节 0.20 s、面板复位与 ramdisk 挂载 1.40 s、fb 注册与**首像素 1.80 s**、问候 2.60 s、状态屏与 NSH 2.80 s。littlefs autoformat 不是瓶颈。

「10 s」在 AP 快复位路径复现不出。两条路径既有实测值：AP 路径控制台 0.52 s、NSH 2.29 s；CP 路径（`bk_wdt_force_reboot()` + `CONFIG_NMI_WDT_EN=1`，等 CP 8000 ms 中断看门狗）第一行日志 8.373 s，三次一致。8.37 + 2.3 ≈ 10.7 s。

屏幕初始化排在 `bk7258_bringup.c` 很后面（BT → /proc → motor → mailbox 等待 → heartbeat → PWC → flash notify → mmcsd → WiFi → TRNG → 马达 → kvdb 任务 → 面板/`fb_register`），任何新增 bring-up 工作都直接推迟首像素。前移面板初始化时**必须留在 `bk7258_ipc_heartbeat_start()` 之后**：CP 心跳 deadline 约 2003 ms，1.7 s 的问候放到心跳前会踩 8 秒看门狗。

### 10.8 `-A` 只烧 AP：CP 重新打包过就不能用

踩中后的特征（每约 8.5 s 一轮重启）：

```text
IPC:E(...):ipc_router_tx_cmpl_isr tx2 error @440! 150 != 0.
ap_bridg:W(...):link down event=a2 queued=0 ready=6
hrt:E(8484):IPC[1]heartbeat timeout 479,8484
(8484)Assert at: mb_ipc_task:297
```

与 5.6「打开 flash 后端会停在 NSH 之前」同类：AP 在 bring-up 期间停止服务 mailbox → CP 心跳失效 → 8 秒看门狗复位。

该循环中控制台桥会在 NSH 提示符处掉线，固定节奏发命令会全部落空。可靠抓桥：

```bash
timeout 14 cat /dev/ttyUSB0 > /tmp/br.log &
for i in $(seq 1 40); do printf 'ap_console open\r\n' > /dev/ttyUSB0; sleep 0.3; done
```

### 10.9 构建：两处会浪费一轮的坑

- `-Werror` 会进 cmake 缓存：用 `-e -Werror` 配过的目录，之后不带该参数仍按它编。换目录或删掉重配。
- `build.sh` 的 `-b <dir>` 要写在 `--cmake` 之后；实测 `--cmake -b cmake_out_x` 产物在 `cmake_out_x/nuttx.bin`，**没有 board 子目录**，与第二节表格层级不同，拷贝路径别照抄。
- `sim_tests` 实际结尾是每个二进制一行 `PASS: all <file> tests passed`，不是 `all checks passed`。当日 7 个全 PASS。

### 10.10 `/dev/video0` 的 JPEG 通路已收口为单一实现（2026-08-17）

`CONFIG_BK7258_CAMERA_JPEG_SW` **已删除**。它此前是「软件编码器 / DVP 内联硬件组装」的构建期开关（默认 `y`），关掉就掉进 10.3 那 11/15 坏帧。硬件那条路的正确性依赖一个逐帧变化的填充长度，把它留成可以选错的开关没有价值，因此：

- `/dev/video0` 的 JPEG 现在**无条件**由软件编码器（libjpeg-turbo）提供，与 `/dev/video1` 同一实现（见 10.2）。
- 两个 board 配置的 `defconfig` 已移除该行；`bk7258_jpeg_enc.c` 的三处 `#ifdef` 已去掉。
- 启动日志固定为 `bk7258_jpeg: /dev/video0 JPEG served by the software encoder`。

验证：`sim_tests` 7/7 PASS；`configs/nsh` 带 `-Werror` 通过（558912 B）、`configs/ai_agent` 通过（828736 B）；上板 `agent_camera 640x480 n=8` 得 8/8 帧（`set_buf_live=8 armed=1`、`agent_camera: OK`），`camera_preview bars` 双屏正常。

**遗留清理（不影响功能）**：`bk7258_camera_imgdata.c` 内联硬件组装已成运行期不可达死代码但仍在编译 —— `bk7258_camera_jpeg_dma_arm()`、`bk7258_camera_jpeg_ring_write_pos()`、`bk7258_camera_jpeg_chunk_done()`、`bk7258_camera_jpeg_eof()`，连带 `jpeg_ring*` 字段与 `bk7258_jpeg_enc_write_header()`（约 500 行）。删除是机械工作，但牵动结构体字段、DMA 通道注册、统计打印三处，建议单独一次改动。

代价：软件编码器 640x480 约 290 ms/帧，上限约 3.7 fps，**达不到任务书 5 fps**。定速器按 5 fps 采样（实测 `sampler_skipped=92`，门在正常丢帧）。要拿回传感器速率应修好 10.3 的组装缺陷后重新引入，而不是恢复这个开关。

### 10.11 10.5 那条判据的正反对照（同一块板、同一固件、只换网络）

| 网络 | `ifconfig` | DNS / ICMP | `net_status` | `net_test` |
|---|---|---|---|---|
| MIPublic（开放网） | `RUNNING`，10.192.105.225 | 通 | `yes` | ❌ `net_connect www.baidu.com:443 ret=0x52` |
| AIPC（WPA2） | `RUNNING`，192.168.31.30 | 通 | `yes` | ✅ `Handshake OK: TLSv1.2 / TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256` → `SUCCESS! HTTP Status: 200` |

`ifconfig RUNNING`、DNS、ICMP、`net_status: yes` 四项全绿仍可能连不上任何 TCP 服务。只有 `net_test` 的 `HTTP Status: 200` 能证明链路可用。5.5 记录的套件是 `TLS-DHE-RSA-WITH-AES-256-CBC-SHA`，本次为 `TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256` —— 套件随协商变化，不是判据。

### 10.12 ai_agent 修复：`-Werror` 已可用，首次 `ask` 假超时已修（2026-08-17）

新增三个存档补丁，`apply.sh` 自动纳入（按 `[0-9]*.patch` 遍历）：

| 补丁 | 修的问题 |
|---|---|
| `0003-fix-format-truncation-and-uint32-format-specifier` | `agent_loop.c:665` 把最长 511 字节消息塞进 512 缓冲（`-Wformat-truncation`）；截断落在消息中间即**切断 UTF-8 序列**，把非法字节发到对话通道。改为按 `sizeof(remind_msg) + 64` 分配。另修 `skill_loader.c:448` 用 `%x` 打 `uint32_t`，改 `PRIx32` |
| `0004-llm-watchdog-use-monotonic-clock` | **首次 `ask` 假超时的根因**：看门狗用 `gettimeofday()`，而 `vela_tls.c` 首次握手把时钟从 1970 推到 2026，跨越跳变的调用被算成约 2.7e9 ms 判超时。新增 `agent_elapsed_mark()` 取 `CLOCK_MONOTONIC`（失败才回退），`calc_elapsed_ms()` 及其六个调用点不动 |
| `0005-allow-overriding-llm-timeout` | `AGENT_LLM_TIMEOUT_SEC` 原为无条件 `#define`，`agent_secrets.h` 无法调大。**该改动此前存在于工作树但未归档**，导致全新 checkout + `apply.sh` 复现不出已烧录的那棵树 |

**两条旧结论作废**：

1. 第二节「`configs/ai_agent` 不能加 `-Werror`」不成立 —— 实测 `-e -Werror` 通过，产物 828736 B。
2. 5.5.2「先跑 net_test 校准时钟再 ask」不再需要（补丁 0004）。缓存那条仍有效：同一问题会命中缓存（含失败文案），重测要换问题。

补丁集完整性往返检查（判断归档有没有漏的唯一办法）：

```bash
sh .../bk7258-ap/ai_agent/apply.sh --revert
git -C packages/ai_agent diff --stat -- src include     # 期望：空
sh .../bk7258-ap/ai_agent/apply.sh                     # 五个全部 applied
```

实测 revert 后 `git diff` 为空、re-apply 后五个全部应用。**补丁 0005 的存在正是因为此前这步不干净**；以后改了 `packages/ai_agent` 都要跑一次这个往返。

上板回归：`ai_agent` 正常启动（`Tools JSON loaded: 10363 bytes`、`All network services started!`）、`net_test` → `Handshake OK: TLSv1.2 / TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256` → `SUCCESS! HTTP Status: 200`、`heap_info` → `arena=6447864 free=6053816 used=394048`；`/dev/video0` 仍为软件编码器。

**未验证**：补丁 0004 的直接效果需可用 LLM key 才能端到端确认，本次无 key，仅验证编译与运行期无回归。拿到 key 后请**直接 `ask`、不要先跑 net_test**；若仍报超时说明 0004 没覆盖全部计时路径。

### 10.13 命令逐条复测（2026-08-18，收口为软件编码器之后）

**5.2 的 `agent_camera n=30` 现在会失败**；不带几何时 auto 选 480x480，所以「不带参数 + n>1」也失败。**多帧采集只在 640x480 可用。**

`agent_camera`：

| 命令 | 结果 | 实测 |
|---|---|---|
| `agent_camera caps` | ✅ | `driver enumerates 6 JPEG size(s)`：480x480/640x480/864x480 各两份 |
| `agent_camera` | ✅ | auto 选 480x480，`bytesused=46016`，structure 五项全 yes |
| `agent_camera 640x480` | ✅ | `bytesused=31901`，五项全 yes |
| `agent_camera out=/mnt/ram/cap.jpg` | ✅ | `wrote 45716 bytes`，`ls -l /mnt/ram` 一致 |
| `agent_camera low` | ✅ | `320x180 refused, using enumerated 480x480` → OK（补丁 0001 生效） |
| `agent_camera low strict` | ✅ | `FAILED (-22)`，符合预期 |
| `agent_camera 640x480 n=8` | ✅ | 8/8，`set_buf_live=8 armed=1` |
| `agent_camera 640x480 n=10` | ✅ | 10/10，`measured=1.26 fps`，`sampler_skipped=124` |
| `agent_camera n=30` | ❌ | `frames=1 sampler_skipped=66` → `FAILED (-110)` |
| `agent_camera n=10` | ❌ | 同上（auto→480x480） |
| `agent_camera 480x480 n=10` | ❌ | 同上 |
| `agent_camera 480x480 n=30` | ❌ | 同上 |

失败签名固定 `sw_jpeg frames=1 dropped_oldest=2 sampler_skipped=66`，即定速门把后续每帧都挡掉；640x480 成功时 `sampler_skipped` 随帧数增长而 `frames` 正常累加。传感器侧 480x480 报 `OK at 480x480 0fps`（640x480 报 30fps）。同 `camera.md` §14.10 的「480x480 `SEN_RESL` 缺陷」。

**变通**：要多帧就显式写 `640x480`；单张抓图 480x480 正常，只有 `n>1` 受影响。曾有一次 `480x480 n=10` 通过（`sampler_skipped=0`），同条件复现不出，按不可依赖处理。

**帧率数字作废重记**：5.2 原记 `n=30 → 16.55 fps` 是旧采集通路的值；现在 `/dev/video0` 固定软件编码器（约 290 ms/帧），实测 `640x480 n=10 → 1.26 fps`。

`camera_preview` 全部通过：`fill f800`/`grid`/`pattern`/`bars` 均 `drew '<name>' on 160x160, 2 panel(s)`；`face` 列出 `neutral static`/`smile static`…；`60 fb=0` → `28.92 fps (convert 7ms/f, push 24ms/f, 0 errors)`（对应 5.4 表的 28.7）；`live+face 40` → `fb0 live, fb1 expression 'neutral'`、`28.59 fps`；`stats`、`bench` 正常。

`ai_agent` 与 5.5 一致：`heap_info` → `arena=6447864 free=6015552 used=432312`；`config_show` 逐项 `(not set)`；`session_list` → `No sessions found`；`memory_read` → `=== MEMORY.md ===`；`show_chat` → `Unknown command`；退出用 `Ctrl-C`（见 10.6）。启动自检可见 `[boot +Nms]` 分阶段耗时，全程约 450 ms，`/mnt/sdnand/ai_agent` 与 `sessions` 由 agent 自建。
