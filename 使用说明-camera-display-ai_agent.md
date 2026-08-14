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

**为什么要分工**，实测对比：

| 配置 | 显示帧率 | 相机帧率 |
|---|---|---|
| 单屏预览（`fb=0`） | 28.7 fps | 29.2 fps |
| 双屏都推相机帧（默认） | **16.8 fps** | 29.4 fps（丢 45/105 帧） |
| `live+face`（一屏相机 + 一屏表情） | **29.2 fps** | 29.6 fps |

一次推屏 51200 字节要 24–26 ms，两屏串行就是 50 ms/帧。表情屏只在表情变化时重画，
所以不进每帧路径，预览就能跑满相机速率。

## 二、从预览流产出 JPEG（上传用）

`/dev/video0` 只有一个拥有者：预览开着时别的进程打不开相机。所以持有相机的进程必须同时
产出上传用的 JPEG —— 这是 `camera_preview` 的 `jpeg` 选项。

| 命令 | 状态 | 说明与实测输出 |
|---|---|---|
| `camera_preview 90 fb=0 jpeg=30` | ✅ | 每 30 帧编一张：`jpeg #1 35064 bytes (copy 27ms + codec 289ms), SOI=ffd8 EOI=ffd9` |
| `camera_preview jpeg` | ⬜ | 默认每 60 帧一张（≈2 s，与方案文档的 Tool 周期一致） |
| `camera_preview 90 fb=0 jpeg=30 jpegout=/mnt/pv.jpg` | ✅ | 首张落盘：`wrote 35064 bytes to /mnt/pv.jpg`，`ls -l /mnt` 核对一致 |
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
| `agent_camera out=/mnt/cap.jpg` | ✅ | `wrote 59851 bytes to /mnt/cap.jpg` |
| `agent_camera b64` | ✅ | 见第五节，取回后主机零警告解码 |
| `agent_camera low` | ✅ | 复现 ai_agent 默认请求：`S_FMT JPEG 320x180 failed: 22` → `320x180 refused, using enumerated 480x480` → `OK` |
| `agent_camera low strict` | ✅ | 不协商，`FAILED (-22)`——这正是未打补丁的 ai_agent 的行为 |
| `nxcamera` | ✅ | 交互式：`input /dev/video0` → `output /mnt/n.jpg 1` → `stream 480 480 30 JPEG` → `q`，产出 18567 字节 |
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

`ai_agent` 启动后进入 `vela>`，`quit` 退出。数据目录 `/mnt/ai_agent`，开机自动挂载。

### 6.1 启动与网络

| 命令 | 状态 | 说明与实测输出 |
|---|---|---|
| `ai_agent` | ✅ | 约 2.7 s 就绪；`[cfgstore] Config store ready at /mnt/ai_agent/config/config.json`、10 个内置 skill 全部写入、无 `Cannot write skill` |
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
> ⚠️ **保存位置是 `/mnt/ai_agent/config/config.json`，而 `/mnt` 是 PSRAM ramdisk，掉电即失。**
> 所以方式 A 每次上电都要重敲。键名：`api_key` / `model` / `llm_host` / `llm_path` /
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
| `social_cue install` | ✅ | 把 skill 文档写进 `/mnt/ai_agent/skills/`（4767 字节） |


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
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh -e -Werror --cmake -j8
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
