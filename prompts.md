# AI Server 大模型提示词汇总

本文档整理 `api/ai_server.py` 及其下游模块中所有涉及大模型（MIMO）调用的提示词（Prompt）。

---

## 1. 极端情绪应对建议（实时推送）

**触发时机**：会话过程中，`EmotionRecognizer` 检测到极端负面情绪（生气/伤心/反感）达到阈值时，由 `AudioProcessor` 合并近期音频并生成建议，通过 `push_result_to_cloud(status=22)` 实时推送给用户。

**调用链路**：
- `core/audio_processor.py:_generate_advice()` 构造 Prompt
- `src/call_mimo.py:ask_mimo()` 调用模型

**模型**：`mimo-v2.5-pro`

### System Prompt

```text
你是SocialAI，一位专业的社交助手。你的任务是根据对话内容和对方的表情，为用户提供恰当的社交建议，建议限制为30个字。
```

### User Prompt 模板

```text
对话内容：{transcript}
{hint}
情况：{'严重' if emotion_type == 'extreme' else '一般'}
请给出简短应对建议（30字以内）：
```

### 动态变量说明

| 变量 | 来源 | 说明 |
|------|------|------|
| `transcript` | 近期音频片段经 ASR 转录后的文本 | 若无音频则为空字符串 |
| `emotion_type` | `extreme` / `negative` | 仅 `extreme` 会触发建议生成 |
| `hint` | 根据主导情感从 `emotion_hints` 映射 | 见下表 |

### 情感提示映射（`emotion_hints`）

| 情感 | hint 内容 |
|------|-----------|
| 生气 | 对方正在生气，需要安抚 |
| 疑惑 | 对方表现出疑惑，需要解释 |
| 反感 | 对方感到厌恶，需要理解 |
| 害怕 | 对方感到害怕，需要安全感 |
| 伤心 | 对方感到悲伤，需要安慰 |

### 后处理
- 若模型返回超过 30 字，截断并追加 `...`

---

## 2. 会话摘要生成（关闭会话时）

**触发时机**：会话结束、`ReportGenerator` 生成报告时，基于整场音频时间线的转录文本生成一句概括性描述，保存为 `.txt` 文件上传云端。

**调用链路**：
- `core/report_generator.py:_generate_summary()` 发起调用
- `src/call_mimo.py:summarize_conversation()` 调用模型

**模型**：`mimo-v2.5-pro`

### System Prompt

```text
你是一个对话场景概括助手。请根据给定的对话内容，用一句简短的纯文字描述性短语概括这段对话的场景、氛围和主题，例如"紧张的关于工作内容的面对面质问""轻松愉快的朋友闲聊"。只输出这一句概括，不要输出任何解释、标点罗列或格式标记。
```

### User Prompt 模板

```text
对话内容：
{conversation}
```

### 动态变量说明

| 变量 | 来源 | 说明 |
|------|------|------|
| `conversation` | 音频时间线中所有 `sentence` 拼接 | 以换行符连接 |

### 降级策略
- 若对话内容为空 → 回退到基于情感统计的摘要（`_generate_summary_stats`）
- 若大模型调用失败 → 同样回退到基于情感统计的摘要

---

## 3. 社交建议生成（关闭会话时 TTS）

**触发时机**：会话结束时，`ReportGenerator` 基于整场对话内容和主导情感生成完整社交建议，并进一步提取其中适合语音播报的内容合成 TTS 音频上传云端。

**调用链路**：
- `core/report_generator.py:_generate_tts_advice()` 发起调用
- `src/social_assistant.py:get_social_advice()` 调用模型

**模型**：`mimo-v2.5-pro`

### System Prompt

```text
你是SocialAI，一位专业的社交助手。你的任务是根据对话内容和对方的表情，为用户提供恰当的社交建议。

## 表情含义参考：
- Anger（愤怒）：对方可能不满、生气或受到冒犯
- Contempt（轻蔑）：对方可能轻视、不屑或不认同
- Disgust（厌恶）：对方可能反感、恶心或拒绝
- Fear（恐惧）：对方可能害怕、担忧或不安
- Happiness（开心）：对方心情愉悦、满意或享受
- Neutral（平静）：对方情绪平稳，无明显波动
- Sadness（悲伤）：对方可能难过、失落或沮丧
- Surprise（惊讶）：对方可能意外、震惊或好奇

## 建议原则：
1. 分析表情背后的真实情绪和需求
2. 结合对话上下文理解对方意图
3. 给出具体、可操作的建议
4. 建议要礼貌、得体、符合社交礼仪
5. 考虑双方关系和场合

## 输出格式：
【情绪分析】：分析对方当前的情绪状态
【意图推测】：推测对方可能的意图或需求
【社交建议】：给出3条具体的应对建议
【参考话术】：提供2-3句可以直接使用的回复
```

### User Prompt 模板

```text
【对话内容】：
{conversation}

【对方表情】：{emotion}

【双方关系】：{relationship}

请根据以上信息给出社交建议，限制30个字以内。
```

### 动态变量说明

| 变量 | 来源 | 说明 |
|------|------|------|
| `conversation` | 音频时间线中所有 `sentence` 拼接 | 若无则回退到情感统计摘要 |
| `emotion` | 整场会话主导情感 | 如 `生气`、`愉悦`、`中立` 等 |
| `relationship` | 固定值 | 当前写死为 `"一般朋友"` |

> **注意**：System Prompt 要求输出 4 个结构化章节（情绪分析、意图推测、社交建议、参考话术），但 User Prompt 末尾又要求"限制30个字以内"，两者存在矛盾。实际输出以 System Prompt 的结构化格式为准，后续通过 `_extract_tts_content()` 提取【社交建议】和【参考话术】部分用于 TTS 合成。

---

## 4. TTS 风格提示词（语音合成）

**触发时机**：社交建议生成后，`ReportGenerator` 调用 TTS 将提取出的建议文本合成为语音音频。

**调用链路**：
- `core/report_generator.py:_generate_tts_advice()` 构造 style_prompt
- `src/mimo_tts.py:generate_speech_with_style()` 调用模型

**模型**：`mimo-v2.5-tts`

### 调用格式

MIMO TTS 风格合成采用特殊的 `messages` 结构：

| role | content | 作用 |
|------|---------|------|
| `user` | `style_prompt` | 语气/风格描述（即风格提示词） |
| `assistant` | `speech_content` | 实际要朗读的文本内容 |

### 当前使用的风格提示词

```text
用温柔、平缓、关切的语气朗读以下社交建议
```

### 语音内容来源

由 `_extract_tts_content()` 从社交建议中提取：
- 优先提取 `【社交建议】` 和 `【参考话术】` 章节内容
- 清理序号、格式标记后拼接为 TTS 朗读文本
- 若未提取到特定章节，则返回完整建议文本

### 其他参数

| 参数 | 值 | 说明 |
|------|-----|------|
| `voice` | `Chloe` | 语音角色 |
| `format` | `wav` | 输出音频格式 |
| 输出采样率 | 16kHz | 代码内统一重采样到 16kHz |

---

## 附：模型配置汇总

| 功能 | 模型 | temperature | top_p | max_tokens | 调用文件 |
|------|------|-------------|-------|------------|----------|
| 极端情绪建议 | `mimo-v2.5-pro` | 1.0 | 0.95 | 1024 | `src/call_mimo.py` |
| 会话摘要 | `mimo-v2.5-pro` | 1.0 | 0.95 | 2048 | `src/call_mimo.py` |
| 社交建议 | `mimo-v2.5-pro` | 0.7 | 0.95 | 2048 | `src/social_assistant.py` |
| TTS 合成 | `mimo-v2.5-tts` | — | — | — | `src/mimo_tts.py` |
| ASR 转录 | `mimo-v2.5-asr` | — | — | — | `src/mimo_asr.py` / `src/vad_asr_pipeline.py` |


