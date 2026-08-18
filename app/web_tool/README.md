# web_tool — 开发板 Web 控制台

浏览器里开一个页面，对 BK7258 AP 核上的 OpenVela 做几件事：配 LLM key 与 Wi-Fi、
读写 kvdb、在控制台里敲命令、看实时日志与摄像头预览、把帧和日志落到开发机磁盘。

替代的是原来那个循环：敲串口命令 → 把抄本存下来 → 再跑 `tools/b64frames.py` 还原帧。

**两跳都是 TLS**：

```
浏览器  --HTTPS/WSS-->  console.py（开发机）  <--TLS 1.2--  web_tool（板上）
```

板子是**拨出**的一方，因为它在 AP 的 NAT 后面——实测出得去、进不来
（`docs/local/2026-08-18-web_tool验收记录二-TLS.md` 第一节）。两端各按自己能做到的方式
互相认证：板子钉住控制台证书的 SHA-256，控制台检查板子首帧里的共享 token。

设计与决策依据：`docs/local/superpowers/specs/2026-08-17-web-tool-design.md`
（TLS 原为非目标，后按需求加入，理由与代价见
`docs/local/2026-08-17-TLS性能核查.md` 第七、八节）。

---

## 快速开始

先起控制台，它会把要在板上执行的命令原样打出来：

```sh
cd app/web_tool/host
./console.py
# console: https://0.0.0.0:8443/
# console: waiting for the board on 0.0.0.0:8899 (TLS 1.2, certificate pinned by the board)
#
# On the board, once:
#   kvdb set web.host <this machine's IP>
#   kvdb set web.port 8899
#   kvdb set web.fp 277fb342…
#   kvdb set web.token 2293d2de…
#   web_tool &
```

板上（**首次必须走一次串口**）：

```sh
kvdb set wifi.ssid <2.4G_SSID>
kvdb set wifi.psk  <密码>              # 开放热点跳过
kvdb wifi ; renew wlan0 ; renew wlan0  # 第一次 renew 可能失败，重试一次
# 然后粘上面那四条 web.* ，最后：
web_tool &
```

浏览器打开 `https://<开发机 IP>:8443/`。证书是自签的，第一次要手动信任——
没有 CA 就不可能消除这个告警，而把告警关掉只会让人以为它被验证过。

`web.fp` 没设置时板子**拒绝连接**，并把它看到的指纹打出来让你比对。
这是故意的：一条不验证对端的 TLS 连接，看起来受保护而其实谁都能接管。

不接硬件也能把整条链路跑起来：

```sh
./mock_board.py --connect 127.0.0.1:8899 --token <token> --pin-fp <fp> &
./console.py
```

---

## 目录

```
app/web_tool/
├── web_tool_main.c        板上 TCP 服务，NSH 命令名 web_tool
├── wt_protocol.c/.h       帧编解码，不依赖 NuttX，主机侧有单元测试
├── wt_command.c/.h        结构化命令分发 + 摄像头线程 + shell 透传
├── wt_queue.c/.h          有界发送队列（分类丢弃）+ syslog 环形缓冲
├── wt_io.c/.h             传输抽象：明文 / TLS 客户端（指纹钉扎）
├── wt_selftest.c/.h       板上自检客户端，走 loopback
├── Kconfig Make.defs CMakeLists.txt Makefile
└── host/                  主机侧，不进固件（CMakeLists 用 SRCS 显式列源文件）
    ├── console.py         后端入口（aiohttp）
    ├── board_link.py      TCP 链路 + 帧编解码 + 重连
    ├── serial_console.py  串口链路（termios），只做引导/早期日志/救援
    ├── bootstrap.py       引导状态机，四条路径
    ├── capture.py         落盘导出，坏帧进 rejected/
    ├── mock_board.py       假板子：TCP/TLS 拨出 + pty 上的类 NSH 控制台
    ├── tlsconf.py          自签 EC P-256 证书、指纹、两个 SSL 上下文
    ├── web/               前端单页，无构建步骤
    └── tests/             主机侧全部测试
```

## 测试

```sh
make -C app/web_tool/host/tests          # 全部：349 项
make -C app/web_tool/host/tests protocol # 分帧边界，69 项
make -C app/web_tool/host/tests queue    # 丢弃策略与日志环，57 项
make -C app/web_tool/host/tests e2e      # 后端对 mock_board，97 项
make -C app/web_tool/host/tests tls      # TLS/WSS 与板端 TLS，40 项
make -C app/web_tool/host/tests keystore # 浏览器端密钥加密，45 项（用 node）
```

C 那两组带 ASan + UBSan，编译选项含 `-Werror -Wconversion -Wsign-conversion`。
`e2e` 不需要开发板也不需要串口——真板是共享设备且串口是独占的，
需要硬件才能跑的测试等于不会被跑的测试。

板上自检（在板子上跑客户端打自己的服务，23 项）：

```sh
kvdb set web.allow 127.0.0.1,<开发机 IP>
web_tool &
web_tool selftest
```

它存在的理由是环境：本机只有有线口，板子只能连开放客网，两者被策略隔离
（双向实测不通，见 `docs/local/2026-08-17-web_tool验收记录.md` 第四节）。
没有它，板上这半边代码就会在没跑过真硬件的情况下交付。
它覆盖板上全部路径——accept、白名单、双向分帧、每条命令、syslog channel
与日志环、摄像头线程与队列、shell 闸门；页面、WebSocket 桥和落盘不在板上，
由 `test_e2e.py` 覆盖。

真板全链路验收（需要板子与开发机能互通，且 `web.allow` 里有本机）：

```sh
./tests/acceptance.py --board 10.192.105.127            # 约 1 分钟
./tests/acceptance.py --board 10.192.105.127 --long     # 加 10 分钟连续预览
```

它打印每一项**测到的数字**而不是结论，方便把验收记录重新生成而不是重新抄。

---

## 四件容易踩的事

### 为什么两个 secret 都不能省，且默认拒绝

`web.fp` 未设置 → 板子拒绝连接。`web.token` 不匹配 → 控制台断开。两者缺一不可：

- **`web.fp`（板子认控制台）**：TLS 若不验证对端，中间人可以直接接管，
  与明文的实际防护等价。指纹钉扎不需要时钟也不需要 CA，是这块没有 RTC 的板子
  唯一能做的真身份判断。
- **`web.token`（控制台认板子）**：TLS 只证明了控制台一方。任何能连到控制台端口
  的东西都能完成握手并假装是板子——而**操作员会把 API key 打进页面**。

`web.allow`（IP 白名单）只对 listen 模式有效，而 listen 模式在 NAT 后面用不了，
现在只服务于 loopback 自检。

### 串口是独占的，后端只在引导期借用

`serial_cmd.sh` 用 `fuser` 检查 `/dev/ttyUSB0`，被占就拒绝运行；`autoflash.sh` 烧固件
同样要独占。所以后端**只在需要时打开串口，引导一结束立刻关闭**，页面上有显式的
「占用串口 / 释放串口」开关和状态灯。占用检查复用同一个 `fuser`，
提示文案与 `serial_cmd.sh` 一致，让两边的失败长得一样。

### API key 存在浏览器里，能买到什么买不到什么

`web/keystore.mjs`：PBKDF2-HMAC-SHA-256（600 000 轮）把**数字 PIN** 拉伸成
AES-256-GCM 密钥，密文进 localStorage，**连续错 5 次自动清除**。
计数器在尝试之前就写回存储，所以刷新页面清不掉它。

它防的是「有人拿到浏览器后乱猜」。**它不防「整份记录被拷走后离线爆破」**——
计数器就在密文旁边，攻击者不会去增加它。挡在中间的只有 PIN 的熵乘上 PBKDF2 的代价，
所以最少 6 位数字。这把 key 值钱就别勾「记住」。页面上原话写着这段。

### 校验失败的帧不丢

坏帧写进会话目录的 `rejected/`。2026-08-17 修掉的那个硬件 JPEG 码流缺陷，
正是靠留下坏帧、在主机侧逐字节平移重组比对才定位的；那批帧**每一帧的标记结构都完整合法**，
只有像素内容能出卖它。所以「能解码」从来不是判断一帧完好的依据，也就不能拿来当丢弃的理由。

---

## 构建接入（改动必须成对）

1. `contest2026_264_VelaSightsuixingAIzhinengyanjing.xml`：
   `<linkfile src="app/web_tool" dest="packages/demos/contest2026_264_web_tool"/>`
2. `configs/ai_agent/defconfig`：`CONFIG_LVX_USE_DEMO_CONTEST2026_264_WEB_TOOL=y`
3. 同一份 defconfig 还需要 `CONFIG_SYSLOG_MAX_CHANNELS=2`

缺软链时第 2 步会被**静默丢弃**，app 不进固件而构建照样成功。加完后核对：

```sh
grep -E "WEB_TOOL|SYSLOG_MAX_CHANNELS" cmake_out/bk7258-ap_ai_agent/.config
```

第 3 步不是可选项：日志流是第二个 syslog channel，而只有一个槽位时
`syslog_channel_register()` 会**替换**掉第 0 个 channel，也就是串口控制台——
引导和救援的唯一依据。`web_tool_main.c` 为此放了一个 `#error`，让它在构建期失败而不是在板上静默消失。

开机自启连网在 `board/beken/boards/bk7258/bk7258-ap/src/bk7258_net_autostart.c`，
**不在这个目录里**：那是板级行为，不该取决于某个调试 app 有没有被编进固件。

---

## 协议

一条 TCP 连接上同时跑命令响应、日志流、摄像头帧，所以要自己分帧：

```
偏移  长度  字段
0     1     type      0x01 REQ / 0x02 RSP / 0x03 EVT_LOG
                      0x04 EVT_FRAME / 0x05 PING / 0x06 PONG
                      0x07 HELLO（板子拨入时的首帧，带 web.token）
1     1     flags     保留，置 0
2     2     req_id    小端；主动推送填 0
4     4     len       小端，payload 字节数，上限 64 KB
8     len   payload   文本类是 JSON；EVT_FRAME 是 seq(4) + fnv1a(4) + JPEG
```

`len` 超限或 `type` 未知一律断连，不尝试恢复：恢复意味着猜下一个帧头在哪，
猜错就会产出用 JPEG 中段拼出来的「帧」。同一套判断在
`wt_protocol.c` 和 `host/board_link.py` 两边都实现了，`test_e2e.py` 里有一组
断言专门保证两边说的是同一种话。

完整命令表见 spec 第六节。


## 哪些消息不能丢

`EVT_LOG` 按设计是可丢的（丢了会补一条 `dropped=N`）。有两条消息**必须**走
「不可丢」那一类（`RSP`），因为它们的全部价值就在于不会丢：

| 消息 | 丢了会怎样 |
|---|---|
| `{"dropped":N}` | 页面会以为自己看到了全部日志——调试工具最坏的失败模式 |
| `{"exit":…}` | 页面永远显示命令还在跑 |

第二条是实测踩出来的：`ls /dev` 一次吐 16 行进 16 深的队列，尾部被丢，
被丢的正好包括 exit。`test_e2e.py` 里有针对性回归（mock `--shell-burst` 灌 40 行）。


---

## 完整操作指南

### 1. 前置条件与依赖

- 已烧录 `configs/ai_agent` 固件，且配置包含
  `CONFIG_BK7258_KVDB_FLASH=y`、`CONFIG_BK7258_CAMERA_HW_JPEG=y`、
  `CONFIG_SYSLOG_MAX_CHANNELS=2` 和 web_tool。
- 开发机使用 Linux、Python 3.10+、OpenSSL、`fuser`（通常来自 `psmisc`）以及可用的
  `/dev/ttyUSB0`；主机测试另需 C 编译器、make 和 Node.js。
- Python 依赖只有 `aiohttp`：`python3 -m pip install aiohttp`。建议装进项目虚拟环境，
  不要用 root 修改系统 Python。
- 板子只支持 **2.4 GHz Wi-Fi**。开发机必须能被板子访问 TCP 8899；浏览器必须能访问
  开发机 TCP 8443。不要把真实 Wi-Fi 密码、LLM key、共享 token 或 TLS 私钥提交到 git。

### 2. 启动主机后端

```sh
cd app/web_tool/host
python3 console.py
```

默认端点：

| 方向 | 地址 | 说明 |
|---|---|---|
| 浏览器 → 主机 | `https://<主机IP>:8443/` | HTTPS 页面；同端口 `/ws` 自动升级为 WSS |
| 板子 → 主机 | `<主机IP>:8899` | TLS 1.2 二进制帧协议，板子主动拨入 |

首次启动会在 `host/tls/` 生成 EC P-256 自签证书、私钥和 128-bit 随机
`board-token`。它们被 `.gitignore` 排除；重启后端会复用它们，保证证书指纹和 token 不变。
浏览器第一次打开自签 HTTPS 时必须人工确认。仅调试页面时可用 `--no-tls`，不可作为正常部署。
常用参数：

```sh
python3 console.py --serial-port /dev/ttyUSB1
python3 console.py --pair-host 192.168.31.10
python3 console.py --host 127.0.0.1 --port 8443
python3 console.py --no-bootstrap
```

`--pair-host` 用于主机有多个网卡、自动选出的回拨地址不对时。`--board <IP>` 是同网段下
由主机主动连接板子的兼容/调试路径；NAT 环境应保留默认的板子拨入模式。

### 3. 首次一键配对与跨重启

1. 关闭串口终端、`serial_cmd.sh` 和烧录工具，确保 `/dev/ttyUSB0` 空闲。
2. 打开页面，在“连接板子”填写 2.4G SSID 和密码（开放网络留空），点击“一键配对”。
3. 后端临时独占串口，依次写入 `wifi.*`、`web.host`、`web.port`、`web.fp`、
   `web.token`，联网后启动 `web_tool`，随后立即释放串口。
4. 状态灯显示板子通过 TLS 接入后即可操作。

当前固件的 flash kvdb 已在真板上验证：配置跨多次 reboot 保留，开机会自动加入 Wi-Fi 并
重新拨入主机，不会再触发 CP heartbeat reset。以后普通重启不需要重复配对。更换主机 IP、
删除 `host/tls/`、重新生成证书/token、修改 Wi-Fi，才需要重新配对。手工配对命令可从页面
“或者自己在串口粘这些”复制；不要手工猜测证书指纹或 token。

### 4. 页面功能

- **系统/控制台**：`free`、`ps`、`ifconfig`、`agent_camera` 等命令走受闸门保护的 shell
  透传；同一时刻只允许一条。↑/↓ 浏览历史，“停止当前命令”停止转发但不保证杀死板上进程。
- **日志**：可订阅、暂停、过滤；队列溢出会明确显示 `dropped=N`，不可把未显示丢弃计数
  当成“零丢帧”。
- **Wi-Fi**：“连接并保存”会先应答再切换网络，TLS 短暂断开是正常现象；板子随后重新拨入。
- **LLM**：`llm.host`、`llm.model`、`llm.key` 写入板上 kvdb。可选择用至少 6 位数字 PIN
  将 key 以 PBKDF2-HMAC-SHA-256（600000 轮）+ AES-256-GCM 加密保存在浏览器；高价值 key
  建议不要勾选本机保存。
- **串口**：仅配对/救援时占用。烧录前先点“释放串口”，并用 `fuser -v /dev/ttyUSB0`
  确认空闲。

### 5. 摄像头模式与正确性策略

| 页面尺寸 | 编码路径 | 2026-08-18 真板结论 |
|---|---|---|
| 480x480 | 硬件 JPEG | 30/30 有效，应用交付 16.72 fps，`bit_fail=0` |
| 640x480 | 硬件 JPEG | 30 次采集约 11.95 fps；28 有效交付、2 个无效熵流 fail-closed 拒绝 |
| 864x480 | 软件 JPEG | 10 帧通过，约 1.15 fps；硬件计数和 validator 均为 0 |

硬件输出不是仅做 SOI/EOI 检查：驱动按完整 4:2:2 MCU 数解析标准 Huffman 熵流，必要时修复
已确认的一位起始偏移，不能严格完成解析的帧以错误 buffer 完成，不会伪装成有效 JPEG。
864x480 的硬件流虽然语法有效，但解码像素不稳定，因此按宽度自动回退软件编码。不要把
“熵偏移字节数每帧相同”作为正确性条件；硬件 fill/header 长度会变化。

### 6. “抓一帧”与连续录制

“抓一帧”严格执行：先创建主机落盘会话 → 启动板端摄像头 → 收到第一条
`capture.saved` → 立即停止摄像头和落盘。成功时浏览器命令控制台打印：

```text
抓帧已保存：/absolute/path/app/web_tool/host/captures/YYYYMMDD-HHMMSS/frame_00.jpg
```

该路径由后端返回，是开发机上的**实际绝对路径**；按钮只保存一张有效帧。5 秒内没有可保存帧
会超时并清理摄像头/会话。连续录制用“开始录制/停止录制”或导出区“开始落盘/停止落盘”。目录：

```text
host/captures/YYYYMMDD-HHMMSS/
├── frame_00.jpg, frame_01.jpg, ...
├── session.log
└── rejected/                 # 传输校验或 JPEG 首尾检查失败的证据
```

文件不会自动上传，也没有自动过期策略。空间不足前应先停止录制，再人工归档或删除旧会话：

```sh
du -sh captures/* | sort -h
find captures -mindepth 1 -maxdepth 1 -type d -mtime +7 -print
# 审核上面的列表后再手工 rm -rf 指定目录；工具不会替你删除证据。
```

### 7. kvdb 与 secret

默认 `kvdb list` / `kvdb get` 会遮蔽以 `.key`、`.psk`、`.token` 结尾的值，包含
`llm.key`、`wifi.psk`、`web.token`。只有在确认终端和日志不会外泄时才使用：

```sh
kvdb list --raw
kvdb get web.token --raw
```

页面和正常测试不需要展示原文。TLS 私钥位于 `host/tls/console-key.pem`（0600），共享 token
位于 `host/tls/board-token`（0600）；备份或迁移时按 secret 处理。

### 8. 无硬件 mock 与测试分层

```sh
# 终端 A：先运行一次 console.py 取得启动时显示的 token 与 fingerprint
./mock_board.py --connect 127.0.0.1:8899 --token <token> --pin-fp <fingerprint>
# 终端 B
./console.py
```

也可用 `./console.py --mock` 测主机主动连接路径。回归层级：

1. `make -C app/web_tool/host/tests`：349 项主机回归，含 ASan/UBSan、保存绝对路径、TLS/WSS。
2. 板上 `web_tool selftest`：loopback 和全部板端命令路径。
3. `host/tests/audit_controls.py --url https://127.0.0.1:8443 --ssid <SSID> --psk <PSK>`：
   浏览器 27 控件审计；凭据只从命令行临时传入，不写文档。
4. `host/tests/acceptance.py --board <IP>`（可直连环境）：真板短验收；加 `--long` 做连续预览。
5. 最终发布必须再做 reboot 持久化、480/640 硬件像素正确性、864 软件回退和物理浏览器操作。

### 9. 故障排查

| 现象 | 检查 |
|---|---|
| 浏览器打不开 | 确认 8443 防火墙、URL 使用 `https://`，并接受自签证书 |
| 页面打开但板子未接入 | 检查主机 8899、防火墙、`web.host/port`、2.4G Wi-Fi；有多网卡时传 `--pair-host` |
| `fingerprint mismatch` | 主机证书变了；核对页面/后端打印的指纹后重新配对，绝不跳过校验 |
| `bad token` | 主机 `board-token` 与板上 `web.token` 不一致；重新一键配对 |
| 串口 busy | 关闭 minicom/screen，停止 `serial_cmd.sh`，点“释放串口”，运行 `fuser -v /dev/ttyUSB0` |
| Wi-Fi 提交后页面暂时离线 | 正常重关联；等待板子拨回，再点“查看状态” |
| 抓帧 5 秒超时 | 先看板子 TLS 是否连接、是否已有 camera/shell 命令占用，再查日志中的 validator/内存错误 |
| 640 偶发少帧 | fail-closed 可能拒绝结构无效帧；看驱动 `bit_fail`，不要放宽验证 |
| 864 很慢 | 这是正确性优先的软件回退（实测约 1.15 fps），不是 TLS 丢包 |
| reboot 后不自动回来 | `kvdb list` 看 `persistent:true`，核对 `wifi.*`/`web.*`，并检查是否有 heartbeat reset |

不要用 `--no-tls`、空指纹或固定假 token“修复”连接问题；这些做法只会把身份校验移除。
