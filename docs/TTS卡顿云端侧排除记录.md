# TTS 卡顿排查：云端侧排除记录

> 测量日期：2026-08-31　测量方式：主机侧对照实验（方案 1）
> 结论：**云端服务无问题，故障在设备侧。** 同时查实两处客户端缺陷。

## 1. 起因

板上闲时语音助手播报时只发出少量破碎声音。设备日志（2026-08-31 20:53 一轮）：

```
[volc_tts_ws] TTS request: reqid=01cf094c-... text=267 bytes
[volc_tts_ws] idle timeout: 6 chunk(s) 94208 byte(s) (2944 ms audio),
              last seq 13 flags 1, 0 ping(s) 1 ack(s) 0 frontend 0 skipped
vs_voice: tts synthesis returned 0 after 28765 ms
vs_voice: playback starved 4 time(s)
vs_audio: network buffers low-water 25 of 40
vs_audio: playback watermark 1000 -> 2000 ms after 4 underrun(s)
```

267 字节 UTF-8 中文约 89 字，按正常语速应产出 18~22 秒音频，实收 2944 ms，约为应有量的 1/6。

## 2. 对照实验设计

在**同一网络出口**用主机复刻设备的请求，逐帧记录到达时刻。探针严格对齐 `packages/ai_agent/src/voice/volc_tts_ws.c`：

| 项 | 值 |
|---|---|
| 端点 | `wss://openspeech.bytedance.com/api/v1/tts/ws_binary` |
| 认证头 | `Authorization: Bearer;<token>`（分号、无空格） |
| 火山帧头 | 8 字节 `0x11 0x10 0x10 0x00` + 4 字节大端长度 |
| 请求体 | `app{appid,token,cluster}` / `user{uid:"agent"}` / `audio{voice_type, encoding:"pcm", rate:16000, speed_ratio:1.0}` / `request{reqid, text, text_type:"plain", operation:"submit"}` |
| cluster | `volcano_tts` |
| voice | `zh_female_meilinvyou_emo_v2_mars_bigtts` |
| 文本 | 88 字 / 264 字节（与失败那轮的 267 字节对齐） |

两处**故意与设备不同**，这是实验价值所在：

1. 空闲预算放宽，并在设备的 10 秒处标记 `DEVICE WOULD HAVE DECLARED EOF HERE`，单独统计该点之后还到了多少音频——设备自己测不出这个数。
2. 打印所有 `msg_type`。`volc_tts_ws.c` 只检查 `0xB0`/`0xC0`/`0xF0`，其它类型直接落到循环尾，不计入 `skipped`/`acks`/`frontend`，在板上不可见。

主机侧环境：有线出口，到 `openspeech.bytedance.com` TLS 可达（约 565 ms，TLSv1.3，证书 CN `*.bytedance.com`）。凭据仅经环境变量传入，未落盘。

## 3. 测量结果

两次独立运行，结果一致：

| | 板子 | 主机（第 1 次） | 主机（第 2 次） |
|---|---|---|---|
| chunk 数 | 6 | 41 | 38 |
| 音频总量 | 2944 ms | **18358 ms** | **17851 ms** |
| 到货窗口 | — | 0.53 → 2.68 s | 0.53 → 3.10 s |
| 到货速率 | （见 §5 更正） | **约 8.5x 实时** | **约 5.7x 实时** |
| 首帧延迟 | 未知 | 0.53 s | 0.53 s |
| 帧间隔 | — | 均 69 ms / 最大 343 ms | 同量级 |
| 结束方式 | `idle timeout`，无终止符 | 收到终止帧 | 收到终止帧 |

**服务端毫无问题**：88 字文本产出约 18 秒音频，2~3 秒内全部送达，速率达实时的 5.7~8.5 倍。

### 3.1 板子收到的是完整流的前缀

对齐两侧帧序列，板子的 6 个 chunk 正好是主机最前面 6 帧：

```
448 + 512 + 512 + 448 + 512 + 512 = 2944 ms   ← 与板子 6 chunk / 94208 字节吻合
```

主机收完这 6 帧只用了 **0.79 秒**。也就是说板子前 2.9 秒音频接收完全正常，之后**一个字节都没再来**。

### 3.2 板子那 10 秒是彻底静默，不是变慢

- 主机实测服务端每 **5.000 秒**发一次 PING（`5.000 / 10.000 / 15.000 …`），板子 28 秒会话里 `0 ping(s)`
- 板子报的是 `idle timeout` 而非 `-EPROTO`，说明 `tls_read_all()` 是在**干净的帧边界**上连续 20 次拿到 `-ETIMEDOUT`——客户端在正常询问，底层无数据

结论：不是下发慢，是**收到约 94 KB 之后连接硬卡死**。

## 4. 顺带查实的两个客户端缺陷

### 4.1 终止帧被当作垃圾丢弃（会导致每次回复固定多等 10 秒）

主机看到的真正结束帧：

```
t=3.115s   12 bytes   seq=-62   flags=3   （无 PCM 载荷）
```

12 字节 = `4 字节头 + 4 字节 seq + 4 字节 size`。而 `volc_tts_ws.c` 的判断顺序是：

```c
size_t audio_off = volc_hdr_len + 8;              /* = 12 */
if (flen <= audio_off) { skipped++; continue; }   /* 12 <= 12 命中，丢弃 */
...
if (seq < 0) { ended = "final sequence"; break; } /* 永远执行不到 */
```

`flen <= audio_off` 先命中，负 seq 终止判据永不执行。**即使一次回复完整收完，设备也检测不到结束，必定走满 20×500 ms = 10 秒空闲预算。** 本次板上 `0 skipped` 是因为压根没收到这一帧。

### 4.2 `sequence` 不是逐帧递增，此前的"丢帧"推断不成立

主机实测序号：`5, 6, 8, 9, 11, 12, 13, 15, 16, 17, 19, 21, …` 有跳号。因此板子"`last seq 13` 却只有 6 帧"属正常现象，**不存在丢帧**。

## 5. 对此前两处错误结论的更正

**更正一：不存在"云端下发速率 r≈0.16"。** 该数字是把被截断的 2944 ms 除以一段几乎全是死寂的墙钟时间（含 10 秒空闲预算）算出的假象。实测服务端速率为 5.7~8.5x 实时。此前引用的 `app/velasight/Kconfig` 注释里 "0.60x–1.93x"、"4 s 音频 / 5.5 s 墙钟"、"r=0.73" 等历史记录，均未能在本次测量中复现，不应作为设计依据。

**更正二：`network buffers low-water 25 of 40` 不能用于排除设备侧。** `iob_min` 仅在 TTS 写回调内采样（`vs_audio.c:1790-1800`），卡死开始后不再有 chunk 到达、回调不再执行，因此那个 25 **只描述健康的头 2.9 秒，对卡死期间一无所知**。

## 6. 修正后的因果链

1. 板子正常收下前约 2.9 秒音频（约 94 KB）
2. 连接卡死，音频与 PING 全断，10 秒无任何字节
3. 客户端空闲预算耗尽判 EOF，丢弃 5/6 回复 → **"少量"**
4. 已到的 2.9 秒因预缓冲闸门被墙钟提前放开而饿死 4 次 → **"破碎"**

第 4 步的闸门缺陷（`vs_audio.c:1356-1362`，字节阈值 `prebuffer` 与墙钟阈值 `prebuffer_ms` 共用同一个 W 且取 OR，导致 r<1 时有效头量被打成 r 倍）独立存在且仍然成立，但相对第 2 步属次要问题。

## 7. 待验证的主嫌（H2）

设备日志显示 mbedtls 层拿到的是干净超时，问题在 mbedtls 之下：TCP/IP 栈、Wi-Fi 驱动或链路。

量级上 IOB 池吻合：主机数据显示音频帧为 16384+12 字节的大帧、成组到达且组内间隔仅 13~15 ms。一个 16 KB TLS 记录约需 11 个 IOB，连续三帧在途约 48 KB ≈ 33 个 IOB，而 `CONFIG_IOB_NBUFFERS=40`、`CONFIG_IOB_THROTTLE=8` 使读缓存有效视角约 32 个——正好在临界点。一次突发打空池子 → 通告零窗口 → 若零窗口探测/恢复未走通即永久死锁，与"音频和 PING 全断且不恢复"的现象吻合。

**此为假设，尚未验证。**

## 8. 复现方式

探针脚本当前位于 `/tmp/tts_probe.py`（标准库实现，主机上无 websocket 包）。运行：

```bash
VOLC_APPID='<app_id>' VOLC_TOKEN='<token>' python3 /tmp/tts_probe.py
```

可选覆盖：`VOLC_CLUSTER` `VOLC_VOICE` `VOLC_RATE` `TTS_TEXT` `TTS_IDLE_S`。

注：本次测量所用凭据已在排查过程中以明文出现，建议轮换。
