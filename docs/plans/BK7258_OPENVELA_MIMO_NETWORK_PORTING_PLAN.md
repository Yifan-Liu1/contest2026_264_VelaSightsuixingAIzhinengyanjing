# 工作目录说明

`contest` 是 BK7258 OpenVela 移植开发仓库。所有正式移植代码必须维护在：

```text
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
├── chips/bk7258/
└── boards/bk7258/bk7258-ap/
```

对公共仓（`packages/ai_agent`、`packages/demos/mimo` 等）的修改，本机通过
`board/beken/boards/bk7258/bk7258-ap/ai_agent/apply.sh` 的 patch 机制落地，
最终需按 `README.md` 第五节以 PR 提交到 `dev-ai-contest-2026` 分支。

构建、打包、烧录与提交规则继续遵守：

- `docs/固件构建步骤.md`
- `docs/github开发指南.md`
- `docs/WiFi使用说明.md`
- `docs/8.16基础适配门禁验收记录.md`

# BK7258 OpenVela MiMo 多模态云端交互网络实施与验证方案

> 文档状态：2026-08-14 实施基线。链路层（关联/DHCP/DNS/TCP/TLS/HTTPS 200）
> 已实机验证；TLS 目前只加密不验证、无可信时间、模型名已失效。本文给出补齐
> "与 MiMo 多模态模型（图像+文本+语音 ASR/TTS）交互"所需全部网络设施的
> 可直接执行步骤。文中所有文件行号、配置符号、证书链和命令均为 2026-08-14
> 在本地工作区逐项核验的结果；标注"待平台确认"的项是实施时的阻塞项。

## 1. 目标和固定结论

### 1.1 目标

让 BK7258 AP 具备与 `api.xiaomimimo.com` 的 MiMo V2.5 系列模型交互的完整
网络条件：

- 图像多模态：相机 JPEG 帧 → OpenAI vision 格式 → `POST /v1/chat/completions`。
- 文本多模态：`ai_agent` 对话、思维链、工具调用。
- 语音多模态（本次范围已确认包含）：MiMo-V2.5-ASR 转写、MiMo-V2.5-TTS 合成。
- 安全基线：全链路 TLS 证书校验（`VERIFY_REQUIRED`）+ 可信时间（SNTP）。

### 1.2 固定结论

- **信任锚只编 DigiCert Global Root G2（根）**，可选附 RapidSSL TLS RSA CA G1
  （中间）。不 pin 叶证书：`*.xiaomimimo.com` 叶证书 2027-01-16 到期且半年轮换。
- **SNTP 用树内 `netutils/ntpclient`**：当前 `NETINIT_NETLOCAL=y` 不会自动启动
  NTPC（Kconfig 明文），用 `SYSTEM_NTPC` 提供的 `ntpcstart` 在 DHCP 成功后启动，
  并在 `network_wifi_reconnect()` 里自动化。
- **校验顺序不可颠倒**：先 SNTP 后 CA，最后 `VERIFY_REQUIRED`。只改最后一项会
  把网络整体关掉（无 CA bundle + 时钟 1970 年，所有证书判"未生效"）。
- **模型名从 `mimo-v2-flash` 迁到 V2.5 系列**：MiMo-V2 已于 2026.6.30 下线。
- 保持 TLS 1.2（当前 mbedTLS 未编译 TLS 1.3，`api.xiaomimimo.com` TLS 1.2 实测可通）。
- 本板配置只编译 `ai_agent` 的 `vela_tls.c`/`http_proxy.c`/`volc_asr.c`/
  `volc_tts_ws.c` 四条 TLS 路径（见 3.4），这四处的证书校验是本方案的第一批；
  `feishu_ws.c`/`node_client.c`/`bailian`/`mimo_provider.c` 随上游 PR 修复。

### 1.3 本轮明确不做

- 不启用 TLS 1.3、DTLS、IPv6、HTTP/2。
- 不接 MiMo 之外的 ASR/TTS 供应商（火山引擎 wss 代码保留，仅补证书校验）。
- 不做 API Key 的 OTA 远端下发（只做本地 flash 持久化，见阶段 M5）。
- 不改 CP 侧 Wi-Fi/RWNX；DHCP 首次失败重试沿用已有结论。

## 2. 平台调研结论（2026-08-14 实测）

### 2.1 Chat 端点与鉴权

| 项 | 结论 |
| --- | --- |
| 端点 | `https://api.xiaomimimo.com/v1/chat/completions`（OpenAI 兼容） |
| 鉴权 | `Authorization: Bearer <API_KEY>`，密钥申请于 `https://platform.xiaomimimo.com/` |
| 存活探测 | `POST /v1/chat/completions` → 401（路由存在，需鉴权）；`/v1/models` → 405（存在） |
| 音频端点 | `/v1/audio/transcriptions`、`/v1/audio/speech`、`/v1/asr`、`/v1/tts` 全部 404 —— ASR/TTS 是**独立模型系列、非 OpenAI 标准路径**，端点 URL 需平台控制台确认（见 9.1） |

### 2.2 证书链（openssl 实测，`Verification: OK`）

```text
0 s:CN = *.xiaomimimo.com                        notBefore 2026-07-02 / notAfter 2027-01-16
  i:CN = RapidSSL TLS RSA CA G1                  notBefore 2017-11-02 / notAfter 2027-11-02
1 s:CN = RapidSSL TLS RSA CA G1
  i:CN = DigiCert Global Root G2                 notBefore 2013-08-01 / notAfter 2038-01-15
```

- 叶证书**半年轮换**；中间 CA 2027-11 到期后 DigiCert 会签发新中间（根不变）。
  所以固件信任锚必须是根（2038 到期），中间证书仅作可选附带。
- 主机侧验证命令（实施中反复可用）：

  ```sh
  echo | openssl s_client -connect api.xiaomimimo.com:443 \
    -servername api.xiaomimimo.com 2>/dev/null | openssl x509 \
    -noout -subject -issuer -dates -ext subjectAltName
  ```

### 2.3 模型与能力

- MiMo-V2 系列 2026.6.30 下线（平台公告横幅）。工程内两处默认值已失效：
  - `packages/ai_agent/include/agent_config.h:159` `AGENT_LLM_MIMO_MODEL "mimo-v2-flash"`
  - `packages/demos/mimo/Kconfig:46` `default "mimo-v2-flash"`
- MiMo-V2.5：原生全模态（图/视频/音频/文本）、1M 上下文；输入 ¥1/MTok、输出 ¥2/MTok。
- MiMo-V2.5-ASR：¥0.5/小时；MiMo-V2.5-TTS：限免。二者是独立模型系列。
- GitHub `XiaomiMiMo/MiMo-V2.5-ASR` 是本地部署模型，不是云 API 接入方式。

## 3. 现状核验（本地文件逐项核对，2026-08-14）

### 3.1 已具备

| 项 | 证据 |
| --- | --- |
| 链路层全通 | `docs/8.16基础适配门禁验收记录.md` §4：关联/DHCP/DNS/TCP/TLSv1.2/`HTTP 200` 实机通过 |
| 硬件 TRNG 熵 | `defconfig:50,58-65`；`board/beken/chips/bk7258/bk7258_trng.c`；实测 `/dev/random`+`/dev/urandom` |
| mbedTLS 校验能力 | `apps/crypto/mbedtls/mbedtls/include/mbedtls/mbedtls_config.h`：`MBEDTLS_X509_CRT_PARSE_C`(3397)、`MBEDTLS_SSL_ALPN`(1727)、`MBEDTLS_SSL_SERVER_NAME_INDICATION`(1832)、`MBEDTLS_SSL_TLS_C`(3304) 全部开启；仅 `MBEDTLS_SSL_PROTO_TLS1_3`(1607) 注释 |
| OpenAI vision 代码路径 | `packages/ai_agent/src/llm/llm_vision.c:41`（`llm_chat_vision`，base64 data URI）与 `:212`（`llm_chat_vision_raw`，内存优化）；调用点 `src/tools/tool_camera.c:324` |
| vision 参数上限 | `agent_config.h:260-263`：JPEG ≤256KB、base64 ≤350KB、max_tokens 4096 |
| 响应与超时 | `agent_config.h:125` socket 超时 120s；`:142-144` 流缓冲 8KB、响应上限 512KB；chunked 解码 `vela_tls.c:72` |
| NTP 客户端代码在树内 | `apps/netutils/ntpclient/`（`CONFIG_NETUTILS_NTPCLIENT`），`apps/system/ntpc/`（`CONFIG_SYSTEM_NTPC` → `ntpcstart`/`ntpcstop`/`ntpcstatus`） |
| 语音管线代码已编译 | `packages/ai_agent/Makefile:117-124`：`volc_asr.c`、`volc_tts_ws.c` 无条件编译（本板配置含语音） |

### 3.2 缺口（阻塞项）

| 缺口 | 证据 |
| --- | --- |
| TLS 只加密不校验 | 本板编译内的 4 处：`vela_tls.c:352`、`http_proxy.c:279`、`volc_asr.c:178`、`volc_tts_ws.c:157` 全部 `MBEDTLS_SSL_VERIFY_OPTIONAL`；另 `mimo_provider.c:351` 为 `VERIFY_NONE`（demo，未启用） |
| 无 CA bundle | 全仓无 `.crt/.pem/.der` 根证书文件（find 核验，仅命中无关 camera 头）；`vela_tls.c` 无 `mbedtls_ssl_conf_ca_chain()` 调用 |
| 无可信时间 | defconfig 无 `NETUTILS_NTPCLIENT`/`LIBC_NETDB`/`SYSTEM_NTPC`；`vela_tls.c:255-259` 时钟 <2024 时硬编码 `forcing to 2026` |
| NTP 不会自动启动 | `apps/netutils/netinit/Kconfig` `NETINIT_NETLOCAL` 帮助原文："It will not start the NTPC daemon. This may be done later from the apps/system/ntpc 'ntpcstart' command."；自动启动路径 `apps/netutils/netinit/netinit.c:669-674` 只在非 NETLOCAL 下生效 |
| 模型名失效 | `agent_config.h:159`、`cmd_llm.c`（`g_router_presets` 的 `mimo` 行，`mimo-v2-flash`）、`demos/mimo/Kconfig:46` |
| webclient 无 multipart | `apps/netutils/webclient/webclient.c` 全文无 `multipart/form-data`（grep 核验）——ASR 音频上传需自建 |
| 响应按 C 字符串处理 | `vela_tls.c` `tls_read_response` NUL 截断 + `strlen` 计长——TTS 二进制音频下载需改长度驱动 |
| 无持久存储 | `/mnt` 为 PSRAM ramdisk（`board/.../src/bk7258_ramdisk.c`，bringup 挂载 `bk7258_bringup.c:164`），API Key/CA 掉电即失；flash 持久化依赖 `docs/plans/BK7258_OPENVELA_SDIO_PORTING_PLAN.md` |

### 3.3 资源预算（`board/.../ai_agent/README.md` 实测）

| 资源 | 现状 | 本方案影响 |
| --- | --- | --- |
| FLASH | 剩余 413 KB | CA 证书 2.9KB（根+中间）可忽略；multipart/SSE 代码 ~几 KB |
| SRAM heap | arena 158KB 静态，agent 运行于 PSRAM heap（arena 6.4MB） | vision 峰值 = 请求 41KB b64 + 响应（上限 512KB，cap 收紧到 ~64KB 更稳） |
| 单帧 payload | 480x480 JPEG ~30KB → base64 ~41KB | 见阶段 M3 压测 |

### 3.4 本板 TLS 站点清单（Makefile 编译子集）

| 文件 | 行 | 现状 | 本板编译 | 处理 |
| --- | --- | --- | --- | --- |
| `src/infra/vela_tls.c` | 352 | `VERIFY_OPTIONAL` | 是 | M1 改 `VERIFY_REQUIRED` + CA |
| `src/infra/http_proxy.c` | 279 | `VERIFY_OPTIONAL` | 是 | M1 |
| `src/voice/volc_asr.c` | 178 | `VERIFY_OPTIONAL` | 是 | M1 |
| `src/voice/volc_tts_ws.c` | 157 | `VERIFY_OPTIONAL` | 是 | M1 |
| `src/channels/feishu_ws.c` | 150 | `VERIFY_OPTIONAL` | 否（FEISHU 关） | 随 PR 上游修复 |
| `src/node/node_client.c` | 377 | `VERIFY_OPTIONAL` | 否（NODE 关） | 随 PR 上游修复 |
| `demos/mimo/mimo_provider.c` | 351 | `VERIFY_NONE` | 否（DEMOS_MIMO 未启用） | M2 启用时一并修 |
| `demos/bailian/net/websocket.c` | 782 | `VERIFY_OPTIONAL`（有 GlobalSign R3） | 否 | 随 PR 上游修复 |
| `demos/bailian/net/http_client.c` | 355 | `VERIFY_NONE` | 否 | 随 PR 上游修复 |

## 4. 实施步骤

构建/烧录总流程（各阶段共用，详见 `docs/固件构建步骤.md`，config 换成 `ai_agent`）：

```bash
# 1) 应用 board patch（含本方案新增 patch）
cd /home/mi/vela_competition/contest
sh contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/boards/bk7258/bk7258-ap/ai_agent/apply.sh

# 2) 构建 AP（改过 defconfig 先 distclean）
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/ai_agent --cmake -j8
# 产物: cmake_out/bk7258-ap_ai_agent/nuttx.bin

# 3) 打包 + 烧录（照 docs/固件构建步骤.md §3-§5 与 autoflash.sh）
cp cmake_out/bk7258-ap_ai_agent/nuttx.bin /home/mi/vela_competition/bk_avdk_smp/build/openvela-ap.bin
cd /home/mi/vela_competition/bk_avdk_smp
podman run --rm --userns=keep-id -v "$PWD:/armino" -w /armino \
  localhost/bekencorp/armino-idk:1.5 \
  make -C projects/app_ab bk7258 SDK_DIR=/armino \
  EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin
cd /home/mi/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
./autoflash.sh -b 1500000
```

### 阶段 M0：SNTP 可信时间（实施并实机验收）

**M0.1 defconfig 追加**（`board/beken/boards/bk7258/bk7258-ap/configs/ai_agent/defconfig`）：

```text
CONFIG_LIBC_NETDB=y
CONFIG_NETUTILS_NTPCLIENT=y
CONFIG_NETUTILS_NTPCLIENT_SERVER="ntp.aliyun.com;ntp1.aliyun.com"
CONFIG_SYSTEM_NTPC=y
```

依赖核验（已确认满足）：`NETUTILS_NTPCLIENT` 依赖 `NET_UDP && NET_SOCKOPTS`
（defconfig:114,117 已开）；`NETUTILS_NTPCLIENT_SERVER` 依赖 `LIBC_NETDB`
（当前未开，故一并追加）；`SYSTEM_NTPC` 会 `select NETUTILS_NTPCLIENT`。
配置后核对最终 `.config`：

```bash
grep -E "NTPCLIENT|NTPC|LIBC_NETDB" cmake_out/bk7258-ap_ai_agent/.config
```

**M0.2 手动验证序列**（NSH，开放热点环境）：

```sh
ifup wlan0
wapi essid wlan0 <2.4G_SSID> 1
renew wlan0                    # 首次可能失败，重试一次
ntpcstart
ntpcstatus                     # 期望: 已向 ntp.aliyun.com 查询并完成校时
date                           # 期望: 真实日期（非 1970）
```

**M0.3 自动化**：`network_wifi_reconnect()`（`packages/ai_agent/src/infra/network_manager.c:872-891`
非 RPMSG 变体）在 `renew <dev>` 成功后追加 `ntpcstart`（`system()` 调用即可）。
以 `board/.../ai_agent/` 下新 patch `0002-*.patch` 落地，随 apply.sh 应用。

**M0.4 替换时钟伪造**：`vela_tls.c:255-259` 的 `forcing to 2026` 改为失败保护——
在 M1 完成前保留原逻辑，M1 合入后改为：

```c
    if (now < 1704067200) { /* Jan 1 2024 */
        syslog(LOG_ERR, "[%s] Clock not synced (UNIX=%ld), refusing TLS\n",
               TAG, (long)now);
        return VELA_TLS_ERR_HANDSHAKE;
    }
```

伪造时间基准会让 `VERIFY_REQUIRED` 的有效期检查失去意义，必须变成硬失败。

**M0 验收**：`date` 输出真实日期；重启后 `wifi_reconnect` 流程自动把时钟校准；
日志中不再出现 `Clock too old, forcing to 2026`。

### 阶段 M1：CA 证书 + VERIFY_REQUIRED（实施并实机验收）

**M1.1 新增证书头文件** `packages/ai_agent/src/infra/vela_tls_ca.h`（board patch 落地）：

```c
/* DigiCert Global Root G2 (root, trust anchor, valid until 2038-01-15)
 * and RapidSSL TLS RSA CA G1 (intermediate, optional offline chain).
 * Chain for api.xiaomimimo.com, verified 2026-08-14. */
#ifndef VELA_TLS_CA_H
#define VELA_TLS_CA_H

static const char vela_ca_digicert_g2_pem[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
"MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
"MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
"b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
"9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
"2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
"1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
"q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
"tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
"vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
"BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
"5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
"1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
"NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
"Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
"8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
"pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
"MrY=\n"
"-----END CERTIFICATE-----\n";

static const char vela_ca_rapidssl_g1_pem[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIEszCCA5ugAwIBAgIQCyWUIs7ZgSoVoE6ZUooO+jANBgkqhkiG9w0BAQsFADBh\n"
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
"MjAeFw0xNzExMDIxMjI0MzNaFw0yNzExMDIxMjI0MzNaMGAxCzAJBgNVBAYTAlVT\n"
"MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
"b20xHzAdBgNVBAMTFlJhcGlkU1NMIFRMUyBSU0EgQ0EgRzEwggEiMA0GCSqGSIb3\n"
"DQEBAQUAA4IBDwAwggEKAoIBAQC/uVklRBI1FuJdUEkFCuDL/I3aJQiaZ6aibRHj\n"
"ap/ap9zy1aYNrphe7YcaNwMoPsZvXDR+hNJOo9gbgOYVTPq8gXc84I75YKOHiVA4\n"
"NrJJQZ6p2sJQyqx60HkEIjzIN+1LQLfXTlpuznToOa1hyTD0yyitFyOYwURM+/CI\n"
"8FNFMpBhw22hpeAQkOOLmsqT5QZJYeik7qlvn8gfD+XdDnk3kkuuu0eG+vuyrSGr\n"
"5uX5LRhFWlv1zFQDch/EKmd163m6z/ycx/qLa9zyvILc7cQpb+k7TLra9WE17YPS\n"
"n9ANjG+ECo9PDW3N9lwhKQCNvw1gGoguyCQu7HE7BnW8eSSFAgMBAAGjggFmMIIB\n"
"YjAdBgNVHQ4EFgQUDNtsgkkPSmcKuBTuesRIUojrVjgwHwYDVR0jBBgwFoAUTiJU\n"
"IBiV5uNu5g/6+rkS7QYXjzkwDgYDVR0PAQH/BAQDAgGGMB0GA1UdJQQWMBQGCCsG\n"
"AQUFBwMBBggrBgEFBQcDAjASBgNVHRMBAf8ECDAGAQH/AgEAMDQGCCsGAQUFBwEB\n"
"BCgwJjAkBggrBgEFBQcwAYYYaHR0cDovL29jc3AuZGlnaWNlcnQuY29tMEIGA1Ud\n"
"HwQ7MDkwN6A1oDOGMWh0dHA6Ly9jcmwzLmRpZ2ljZXJ0LmNvbS9EaWdpQ2VydEds\n"
"b2JhbFJvb3RHMi5jcmwwYwYDVR0gBFwwWjA3BglghkgBhv1sAQEwKjAoBggrBgEF\n"
"BQcCARYcaHR0cHM6Ly93d3cuZGlnaWNlcnQuY29tL0NQUzALBglghkgBhv1sAQIw\n"
"CAYGZ4EMAQIBMAgGBmeBDAECAjANBgkqhkiG9w0BAQsFAAOCAQEAGUSlOb4K3Wtm\n"
"SlbmE50UYBHXM0SKXPqHMzk6XQUpCheF/4qU8aOhajsyRQFDV1ih/uPIg7YHRtFi\n"
"CTq4G+zb43X1T77nJgSOI9pq/TqCwtukZ7u9VLL3JAq3Wdy2moKLvvC8tVmRzkAe\n"
"0xQCkRKIjbBG80MSyDX/R4uYgj6ZiNT/Zg6GI6RofgqgpDdssLc0XIRQEotxIZcK\n"
"zP3pGJ9FCbMHmMLLyuBd+uCWvVcF2ogYAawufChS/PT61D9rqzPRS5I2uqa3tmIT\n"
"44JhJgWhBnFMb7AGQkvNq9KNS9dd3GWc17H/dXa1enoxzWjE0hBdFjxPhUb0W3wi\n"
"8o34/m8Fxw==\n"
"-----END CERTIFICATE-----\n";

#endif /* VELA_TLS_CA_H */
```

**M1.2 `vela_tls.c` 挂载 CA 并切换校验模式**。`tls_ctx_t` 增加
`mbedtls_x509_crt ca;`（`tls_ctx_free` 里 `mbedtls_x509_crt_free`），
在 `mbedtls_ssl_conf_authmode` 之前：

```c
    ret = mbedtls_x509_crt_parse(&ctx->ca,
        (const unsigned char *)vela_ca_digicert_g2_pem,
        sizeof(vela_ca_digicert_g2_pem));
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] CA parse failed: -0x%04x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    /* rapidssl_g1 is the optional intermediate; failure here is tolerable
     * because the server sends its own chain during the handshake. */
    (void)mbedtls_x509_crt_parse(&ctx->ca,
        (const unsigned char *)vela_ca_rapidssl_g1_pem,
        sizeof(vela_ca_rapidssl_g1_pem));

    mbedtls_ssl_conf_ca_chain(&ctx->cfg, &ctx->ca, NULL);
    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_REQUIRED);
```

同法修改 `http_proxy.c:279`、`volc_asr.c:178`、`volc_tts_ws.c:157`（各自 TLS
结构挂 CA + `VERIFY_REQUIRED`）。

**M1.3 合入 M0.4 的时钟硬失败**（本阶段必须生效，否则 `VERIFY_REQUIRED`
会因时钟错误导致全部握手失败——这恰好是期望的安全行为，但生产流程必须由
SNTP 先行校时）。

**M1 验收**：

```sh
ai_agent
net_test
# 期望: [vela_tls] Handshake OK: TLSv1.2 / ... 且无 cert verify 警告，
#       SUCCESS! HTTP Status: 200

# 反向验证（校时前/烧坏时间）：手设 1970 时间后 net_test 必须失败：
date -s "2020-01-01 00:00:00" 2>/dev/null || true
net_test    # 期望: FAILED!（Clock not synced 或证书尚未生效），不得返回 200
ntpcstart && ntpcstatus && net_test   # 校时后恢复 200
```

### 阶段 M2：模型迁移 V2.5（实施并实机验收）

**M2.1 改默认值**（模型名以平台控制台"可用模型"为准，占位写法）：

- `packages/ai_agent/include/agent_config.h:159`：
  `#define AGENT_LLM_MIMO_MODEL "mimo-v2.5"`（待平台确认最终名）
- `packages/ai_agent/src/channels/cmd_llm.c` `g_router_presets` 的
  `"mimo"` 行同改。
- `packages/demos/mimo/Kconfig:46` `default "mimo-v2.5"`（PR 上游，本机可直改）。

**M2.2 启用 mimo CLI 作调试工具**（可选但建议）：defconfig 追加
`CONFIG_DEMOS_MIMO=y`，`MIMO_API_KEY` 留空（运行时传入），同时把
`mimo_provider.c:351` 改 `VERIFY_REQUIRED` 并按 M1.2 挂 CA。
`demos/mimo` 依赖 `NETUTILS_WEBCLIENT`（Kconfig `select`，自动）。

**M2.3 运行时配置**（NSH）：

```sh
ai_agent
set_llm mimo <API_KEY>
# vision 专用模型可另行配置:
#   vision_host / vision_model / vision_api_key（agent_config.h:252-254）
net_test   # 期望 200 + 证书校验通过
```

**M2.4 平台文档确认 V2.5 的 vision 请求体**（阻塞项，见 9.1）：确认
`/v1/chat/completions` 的 `content` 数组是否接受 OpenAI 的
`{"type":"image_url","image_url":{"url":"data:image/jpeg;base64,..."}}`
（`llm_vision.c` 现状格式），以及全模态是否需要独立模型名。用平台 Studio 或
官方 SDK 在主机上先跑通同一请求体，再上板。

**M2 验收**：`ai_agent` 内一条纯文本对话返回有效回复；模型名不再报"已下线"类错误。

### 阶段 M3：图像多模态实机验证（实施并实机验收）

链路（代码已存在，本阶段只验证+压测）：`tool_camera.c:324` →
`llm_chat_vision_raw`（base64 流式编码，峰值内存 = 41KB b64 + 请求头尾）→
`llm_http_direct`（`llm_proxy.c:323`）→ `vela_https_post_json` → 握手+POST。

**M3.1 真机一次往返**：NSH 进入 `ai_agent`，触发 camera 工具（如
`social_cue` 或 agent_camera 流程），日志核对：

```text
[llm_vision] Vision raw API call (model: ..., body=~42KB, image=30763 raw -> 41xxx b64)
[vela_tls] Handshake OK: TLSv1.2 / ...
HTTP 200
```

**M3.2 内存与响应上限压测**：请求 41KB、响应上限 512KB（`agent_config.h:143`）
叠加 6.4MB PSRAM heap 无压力；但连续多轮对话历史 + 思维链 `reasoning_content`
回传会放大请求体，实测记录 `heap_info` 与最长响应，必要时把
`AGENT_LLM_MAX_RESP_SIZE` 收紧到 128KB。

**M3 验收**：真机拍摄 → vision 问答有实质回复；`frames=1 timeouts=0`；
TLS 证书校验通过（无 OPTIONAL 跳过日志）；10 次连续请求无 OOM/无 TLS 重连风暴。

### 阶段 M4：SSE 流式输出（P1 体验项，实施并实机验收）

请求体加 `"stream":true`；响应为 chunked + `data: {...}\n\n` 帧。
`vela_tls.c:72` 已有 chunked 解码；新增：

1. `tls_read_response` 增加逐段回调（增量吐给上层），不再等完整响应。
2. SSE 行解析器：按 `\n\n` 分帧、剥 `data: `、过滤 `[DONE]`、空行/注释行。
3. 与 `llm_http_direct` 对接：流式路径绕过 `AGENT_LLM_MAX_RESP_SIZE` 512KB
   完整缓冲，改为边收边出（每 token 一段）。
4. 超时策略不变（SO_RCVTIMEO 120s，`agent_config.h:125`），流式下首帧应在
   1~3s 内到达，作为看门狗附加判据。

**M4 验收**：记录首 token 延迟（对比非流式全响应延迟 5~20s）；`[DONE]` 帧正常
收尾；断网中断流时无死锁（socket 超时兜底）。

### 阶段 M5：凭证与 CA 持久化（依赖 SDIO flash 计划）

现状：`/mnt` 为 PSRAM ramdisk，掉电即失（`docs/8.16基础适配门禁验收记录.md` §8 已记录）。
实施：

1. 依 `docs/plans/BK7258_OPENVELA_SDIO_PORTING_PLAN.md` 打通 flash 分区挂载后，把
   `AGENT_DATA_DIR` 指向 flash 分区（替换 `bk7258_ramdisk.c` 设备名）。
2. API Key（`set_llm` 写入的 `llm_host/llm_api_key` 等 claw config）随之持久化。
3. CA 根证书同时保留编译期内置 + 运行时从 flash 加载（mbedTLS 支持多根并列，
   为 OTA 轮换留路径）。

**M5 验收**：断电重启后 `set_llm` 免重配、`net_test` 直接可过。

### 阶段 M6：语音 ASR/TTS 管道（实施并实机验收）

前置阻塞：从平台控制台/官方 SDK 取得 ASR/TTS 的**确切端点 URL、鉴权头、
音频格式**（探测 404 证明不是 OpenAI 标准路径）。

**M6.1 multipart/form-data 上传（ASR）**：webclient 无 multipart（3.2 核验），
在 board patch 中新增 `multipart_body_builder`：边界生成、字段头、音频
（16kHz/16bit/mono 的 PCM 或压缩格式）二进制段。上传大小核算：2s PCM = 64KB，
建议先传短样本（≤5s）。

**M6.2 二进制安全接收（TTS）**：`vela_tls.c` `tls_read_response` 目前 NUL 截断
+ `strlen` 计长，改为长度驱动（`out_body_len` 已存在），并让 sink 直接对接
`audio_playback`（`packages/ai_agent/src/voice/audio_playback.c` 已无条件编译）。

**M6.3 超时独立化**：TTS 合成耗时与 LLM 不同，SO_RCVTIMEO 不能共用 120s，
按端点实测值单独设置。

**M6 验收**：ASR 一次真机转写正确文本；TTS 一次真机播放正确音频；两链路
TLS 证书校验均通过。

## 5. 测试和门禁（总表）

| 门禁项 | 命令 | 期望 |
| --- | --- | --- |
| 时钟可信 | `ntpcstart && ntpcstatus && date` | 真实日期，非 1970；重启后经 `wifi_reconnect` 自动校时 |
| 证书校验生效 | `ai_agent` → `net_test` | `Handshake OK` 且**无** `VERIFY_OPTIONAL` 跳过痕迹；`HTTP 200` |
| 时钟错误必须失败 | 设 1970 时间后 `net_test` | FAILED（不得 200）；校时后恢复 |
| 模型可用 | `set_llm mimo <key>` + 文本对话 | 有效回复，无"已下线"错误 |
| 图像多模态 | vision 问答 | 实质回复 + `frames=1 timeouts=0` + 证书校验通过 |
| 大 payload | 连续 10 次 vision 请求 | 无 OOM、无 TLS 重连风暴；`heap_info` 记录 |
| 流式（M4） | 流式问答 | 首 token ≤3s，`[DONE]` 收尾 |
| 持久化（M5） | 断电重启 | `set_llm` 免重配 |
| 语音 ASR（M6） | 真机转写 | 文本正确、TLS 校验通过 |
| 语音 TTS（M6） | 真机播放 | 音频可听、TLS 校验通过 |
| 回归 | `social_cue` 全流程 + `audio_test rec` | 原有三格不退化 |

## 6. 故障定位顺序

### 证书校验失败（M1 后 net_test 失败）

1. `date`：时钟是否已由 NTP 校准（M0 是否生效）。
2. 日志中 `mbedtls_strerror` 的 -0xXXXX：`VERIFY_FAILED` 还是
   `CERT_EXPIRED`/`CERT_FUTURE`（时钟嫌疑）。
3. 主机侧 `openssl s_client` 确认服务器链未变（2.2 命令）。
4. 确认固件内 CA 数组与 4.1 逐字节一致（`sha256sum` 生成脚本固化）。
5. 确认 `mbedtls_ssl_set_hostname` 在 `setup` 后仍被调用（SNI 影响证书选择）。

### NTP 不收敛

1. `ntpcstatus` 状态与重试计数。
2. DNS 是否可用：`ping www.baidu.com` 打印解析 IP（ICMP 丢包忽略）。
3. UDP 123 出站是否被网络屏蔽——换 `ntp1.aliyun.com` 或运营商 NTP 再试。
4. `LIBC_NETDB` 是否生效（否则 hostname 解析为空，只能走
   `NTPCLIENT_SERVERIP`）。

### 大 payload 超时/OOM

1. 区分 TLS 层超时（SO_RCVTIMEO）与 agent 看门狗（`AGENT_LLM_TIMEOUT_SEC`）。
2. `heap_info` 对比请求前后 arena。
3. 检查响应是否触到 `AGENT_LLM_MAX_RESP_SIZE` 截断。
4. 检查 CP→AP 数据面是否丢大包（先小包验证正常）。

### ASR/TTS 404/401

1. 端点路径是否来自平台控制台（2.1 探测结论：非 OpenAI 标准路径）。
2. 鉴权头名是否与平台要求一致（可能不是 Bearer）。
3. 音频格式（采样率/编码/容器）是否匹配端点要求。

## 7. 构建接入备忘

- defconfig 每阶段追加符号后必须核对最终 `.config`，不能只改 defconfig
  （README 既有告诫）。
- 新增源文件加入 `packages/ai_agent/src/infra/` 时同步
  `packages/ai_agent/Makefile` 与 `CMakeLists.txt` 的 CSRCS。
- board patch 命名沿用 `board/.../ai_agent/` 下 `000N-*.patch` 与 apply.sh
  幂等机制；新文件（如 `vela_tls_ca.h`）以 patch 中 `new file mode` 落地。
- `-Werror` 在 `ai_agent` 配置不可用（既有上游告警），保持现状。

## 8. 风险和待确认事项

### 8.1 待平台确认（阻塞项）

1. **V2.5 确切模型名**与 vision 请求体格式（`image_url` data URI 是否接受）。
2. **ASR/TTS 端点 URL、鉴权、音频格式**（探测已证非 OpenAI 标准路径）。
3. V2.5 全模态是否同一个 `/v1/chat/completions` 端点。

### 8.2 固有风险

- 叶证书半年轮换 + 中间 CA 2027-11 到期：信任锚必须根（2038），且 M5 的
  flash CA 更新路径要在量产前就位。
- `NETINIT_NETLOCAL` 下 NTP 永不自动启动：依赖 M0.3 的自动化钩子，漏掉则
  `VERIFY_REQUIRED` 全挂（安全失败，非静默）。
- PSRAM 掉电即失：API Key 与运行时 CA 在 M5 前不持久。
- mbedTLS 无 TLS 1.3：若服务器侧未来强制 1.3，需开启
  `MBEDTLS_SSL_PROTO_TLS1_3`（当前注释于 `mbedtls_config.h:1607`）并补 PSA 依赖。

## 9. 完成定义

满足以下全部条件后，本方案可记为完成：

- 联网后系统时钟来自 SNTP，`vela_tls.c` 无 `forcing to 2026` 伪造。
- DigiCert Global Root G2（+可选中间）编入固件，本板 4 条 TLS 路径
  （vela_tls/http_proxy/volc_asr/volc_tts_ws）全部 `VERIFY_REQUIRED`。
- 时钟错误/证书错误场景实测为连接失败而非降级通过。
- MiMo V2.5 模型名生效，文本与图像多模态各完成一次真机往返（HTTP 200 +
  证书校验通过 + 有实质内容）。
- 语音 ASR/TTS 各完成一次真机往返（M6 范围项）。
- 流式输出首 token 延迟记录在案（M4 范围项）。
- API Key/CA 持久化到 flash 并断电验证（M5 范围项）。
- `docs/8.16基础适配门禁验收记录.md` 原三格回归通过。
- 所有本机公共仓改动以 board patch 归档，并已按 README 第五节发起上游 PR。
