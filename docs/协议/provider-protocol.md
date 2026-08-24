# agent-qt 多模型中间协议（v2）

> **状态：v2 当前** —— 延续 v1 主形状，删除历史源码兼容入口与隐式旧账本反推。
> 需要新能力时，优先「加法小补丁」或 `metadata` 暂存；禁止拆 `items` 双轨、恢复 Opaque、Event 全量 variant。

| 项 | 路径 |
|----|------|
| 定义目录 | `src/runtime/providers/ProviderTypes/` |
| 对外入口 | `#include "providers/ProviderTypes/ProviderTypes.h"` |
| 工具域 | `src/runtime/tools/ToolTypes.h`（与协议分离） |
| 协议版本 | `kProviderProtocolVersion = 2`，`kProviderProtocolRevision = 0` |
| 注释规范 | [codestyle.md](../规范/codestyle.md) |
| 结构图 | [provider-types-structure.svg](../图示/provider-types-structure.svg) |
| 工具类型图 | [tool-types-architecture.svg](../图示/tool-types-architecture.svg) |
| **更新协议时** | 仓库 skill：[`.agents/skills/update-provider-protocol/SKILL.md`](../../.agents/skills/update-provider-protocol/SKILL.md) |

### 源文件职责

| 文件 | 职责 |
|------|------|
| `ProviderCommon.h/.cpp` | 版本、资产、Usage/Error、能力（账本 IR） |
| `ProviderItem.h/.cpp` | MessagePart + ProviderItem |
| `ProviderRequest.h/.cpp` | Request + 选项 |
| `ProviderEvent.h/.cpp` | 流式事件（账本 IR） |
| `ProviderAdapterTypes.h` | Transport / TurnState（**仅 adapter**） |
| `ProviderTypes.h` | 聚合 include（账本/UI；**不含** AdapterTypes） |

依赖方向（无环）：

```
ProviderCommon
      ▲
ProviderItem
   ▲        ▲
Request   Event
   ▲        ▲
   └── ProviderTypes.h（账本 IR）──┘

ProviderAdapterTypes.h ──► Common + Item   （adapter 另 include）
```

---

## 1. 设计目标

跨厂商用一套 LLM-agnostic 类型描述：

| 方向 | 类型 | 形态 |
|------|------|------|
| **入** | `ProviderRequest` | 文档式完整请求 |
| **出（流）** | `ProviderEvent` | 增量事件信封 |
| **出（终态）** | `ProviderItem[]` | 与入站同型，写账本权威源 |

原则：

1. 入出共用终态模型 `ProviderItem`（不拆 ContentBlock / HistoryItem）
2. `items[]` 单一有序列表；是否已提交由账本管理，不分裂协议类型
3. Reasoning 一等 Item
4. **不设 Opaque**（见下）
5. Event 保持扁平信封 + 工厂（不做全量 variant）
6. `systemPrompt` 顶层字段
7. Text / Image / Audio / Document / Video 均为一等 `MessagePart`（Text 可附 `citations`）
8. 厂商服务端工具用 `ServerToolCall` / `ServerToolResult` 一等 kind（`ProviderServerToolName` 短名，禁止 Opaque）
9. 结构化输出 `ProviderResponseFormat`；工具结果可用 `outputParts` 多模态回传
10. 多协议族 `protocolFamily` + `sampling` / part 级 `cachePolicy`（无 Opaque）

### 扩展位白名单（软 Opaque 护栏）

协议禁止 Opaque kind，但保留少量 **扩展位**（map/object）。扩展位不是逃生舱：

| 扩展位 | 校验 | 账本策略 |
|--------|------|----------|
| `Request.metadata` | `isAllowedRequestMetadataKey` | 不进历史；仅请求旋钮 |
| `Item.details` | `validateToolDetailsObject` | 仅可移植摘要键 |
| `MessageEnd.logprobs` | `validateLogprobsObject` | 须含 content 或 x- 键 |
| `ResponseMetadata.vendorUsageRaw` | `validateVendorUsageRawObject` | 账本/UI：`forbidNonEmpty=true` |
| 权威用量 | `ProviderUsage` / `portableUsage` / `finalUsage` | **唯一**用量真相 |

需要长期核心语义时：查文档 → 加一等字段/kind，而不是扩大白名单堆业务。

### 不设 Opaque（关键决策）

协议**不提供**「厂商原始 JSON 黑盒」终态条目（无 `ProviderItemKind::Opaque` / `rawItem`）。

| 收到未知厂商块时 | 应做 | 禁止 |
|------------------|------|------|
| 客户端工具 | `FunctionCall` / `FunctionCallOutput` | raw 整包进账本 |
| 厂商服务端工具（web_search / file_search / code_interpreter / computer / web_fetch …） | `ServerToolCall` / `ServerToolResult`（`name` 用稳定短名；`details` 只放可移植摘要） | `makeOpaque(raw)`；把厂商整段 JSON 塞进 `details` |
| 文档 / PDF | `MessagePart::Document` | 伪造成 Image |
| 思考块（含 signature / redacted） | `Reasoning` + `reasoningSignature` / `reasoningRedacted` | 挂靠下一条的侧信道 |
| 文本引用 | `ProviderMessagePart.citations` | 丢弃且无替代摘要（若产品需要展示出处） |
| 仅本家续跑需要 | `continuationId` / adapter 内部瞬态 | 把 raw 写进 `items[]` |
| 请求扩展参数 | `ProviderRequest.metadata`（不进历史） | 用 metadata 冒充对话记忆 |
| 仍无法归类的实验字段 | 压成可读文本 / UI 提示 / 查文档后**正式加 kind** | 用 Opaque 无限期拖延建模 |

代码锚点：`ProviderItemKind`（`ProviderItem.h`）枚举注释。

### 一等 kind 与厂商类型对照（语义映射，非 wire 同构）

目标协议族见 `ProviderProtocolFamily`（Request 可显式指定，或 Auto 交给实例配置）。

| 中间协议 | OpenAI Responses | OpenAI 兼容 Chat Completions | DeepSeek Chat 方言 | Anthropic Messages | Gemini（generateContent / Interactions） |
|----------|------------------|------------------------------|--------------------|--------------------|------------------------------------------|
| `UserMessage` / `AssistantMessage` | `message` | `messages[].role` | 同左 | content 角色 | `contents[].role` user/model |
| `FunctionCall` / `FunctionCallOutput` | `function_call` / `function_call_output` | `tool_calls` / `role:tool` | 同左 | `tool_use` / `tool_result` | `functionCall` / `functionResponse` |
| `Reasoning` | reasoning 相关 | 少见 | **`reasoning_content`** 历史字段 + 流式 delta | `thinking` / `redacted_thinking` + signature | `thought` / `thoughtSignature` |
| `ServerToolCall` / `Result` | web_search_call / file_search / code_interpreter / image_generation / mcp / tool_search / local_shell / shell / apply_patch 等 | 视网关 | 视网关 | `server_tool_use` + `*_tool_result`；code_execution / tool_search / advisor | Interactions steps：google_search_* / built-in tools |
| `Program` / `ProgramOutput` | `program` / `program_output`（Programmatic Tool Calling；fingerprint 必回放） | — | — | 无对等（code_execution 内嵌调用走 FunctionCall+caller） | — |
| `ApprovalRequest` / `Response` | `mcp_approval_request` / `mcp_approval_response` | — | — | MCP connector 审批（若启用） | — |
| `Compaction` | `compaction` item | — | — | — | — |
| `Part` 多模态 | input_text/image… | content array / image_url | 同兼容 | text/image/document… | text/inlineData/fileData（含 video） |
| `systemPrompt` | `instructions` | `role:system` 首条 | 同左 | 顶层 `system` | `systemInstruction` |
| `continuationId` | `previous_response_id` | 通常无 | 通常无 | 无（靠历史回放） | Interactions 会话 / 缓存名 |
| `reasoning` 选项 | reasoning 参数 | — | **`thinking` + `reasoning_effort`** | `thinking` / budget | thinkingConfig |
| `sampling` | top_p 等子集 | temperature/top_p/stop/seed/penalties | temperature 等 | temperature（thinking 时约束） | generationConfig |
| `responseFormat` | text/json schema | `response_format` | 视支持 | 有限 | responseMimeType + schema |
| `cachePolicy` on part | 网关 prompt cache | 视网关 | 视网关 | **`cache_control: ephemeral`** | part 断点（有限） |
| `providerCachedContentId` | — | — | — | — | **`cachedContent` 资源名** |
| `sampling.topK` | 少见 | 视网关 | — | — | **generationConfig.topK** |
| `mediaResolution` | — | — | — | — | part / generationConfig |
| `Usage.thoughtTokens` | — | — | — | — | **total_thought_tokens** |
| `requestLogprobs` | 视端点 | `logprobs` / `top_logprobs` | 视支持 | — | — |
| `MessageEnd.logprobs` | 可选投影 | choices[].logprobs | 可选 | — | — |

`ServerToolCall.name` 稳定短名见 `ProviderServerToolName`：`web_search`、`file_search`、`code_interpreter`、`computer`、`web_fetch`、`image_generation`、`mcp`、`mcp_list_tools`、`tool_search`、`local_shell`、`shell`、`apply_patch`、`advisor`、`url_context`、`google_maps` 等。

#### OpenAI Responses 一等条目覆盖（官方 `ResponseItem` / `ResponseOutputItem`）

| 官方 type | 中间协议 | 备注 |
|-----------|----------|------|
| `message` | User/AssistantMessage | input/output message |
| `function_call` / `function_call_output` | FunctionCall / FunctionCallOutput | 含 `caller` → `callerType`/`callerId` |
| `custom_tool_call` / output | FunctionCall / FunctionCallOutput | 客户端 custom tool |
| `reasoning` | Reasoning | `encrypted_content` → `reasoningSignature`；summary → content/includeSummary |
| `web_search_call` / `file_search_call` / `code_interpreter_call` / `computer_call`(+output) / `image_generation_call` / `local_shell_call`(+output) / `function_shell_*` / `apply_patch_*` / `tool_search_*` / `mcp_call` / `mcp_list_tools` | ServerToolCall/Result 或 FunctionCall*（本机执行时） | 短名归一化；本机 shell/patch 优先 FunctionCall |
| `program` / `program_output` | Program / ProgramOutput | fingerprint 必回放 |
| `mcp_approval_request` / `mcp_approval_response` | ApprovalRequest / ApprovalResponse | 审批往返 |
| `compaction` | Compaction | 上下文压缩 |
| `additional_tools` | 不进 items | 请求侧 tools 增量由运行时合并，不进账本线路 |

#### Anthropic Messages 注意（官方 server tools / tool reference）

- `server_tool_use` + `web_search_tool_result` / `web_fetch_tool_result` 等 → ServerTool*
- 客户端 schema 工具（bash / text_editor / memory / computer）：→ FunctionCall*（本机执行）
- `stop_reason: pause_turn` → `StopReason::PauseTurn`；续跑须原样回传 assistant content + 相同 tools
- `defer_loading` / `allowed_callers` / `strict` → `ProviderToolSpecification` 对应字段
- programmatic：code_execution 内嵌调用的 `caller` → FunctionCall.callerType/Id

#### Gemini 消息格式覆盖（Interactions + generateContent）

官方来源：`ai.google.dev` Interactions / generateContent / thought-signatures / tools / code-execution。

##### A. Interactions API（新项目推荐）

| 官方概念 | 中间协议 | 备注 |
|----------|----------|------|
| `input` 文本 / content | `UserMessage` parts | 亦可 `user_input` step |
| `system_instruction` | `systemPrompt` | **每轮需重传**（history 由 previous 继承） |
| `tools`（function / google_search / …） | `tools` + 内置工具配置 | **每轮需重传** |
| `previous_interaction_id` | `continuationId` | 有状态续聊 |
| `store` | `storeServerState` | 默认持久化；`store=false` 无状态 |
| `background` | `backgroundExecution` | 长任务 / Deep Think / agents |
| `generation_config.thinking_level` | `reasoning.effort` | low/medium/high/max/minimal… |
| `thinking_summaries` | `reasoning.includeSummary` | auto 等 |
| step `thought` | `Reasoning` | **必填** `signature` → `reasoningSignature`；`summary` → content |
| step `function_call` | `FunctionCall` | `call_id`/`name`/`arguments`；Gemini 3+ 可有 signature |
| step `function_result` | `FunctionCallOutput` | `result` 可多 part → `output`/`outputParts` |
| step `model_output` | `AssistantMessage` | text + annotations → citations |
| step `google_search_call` / `google_search_result` | `ServerToolCall`/`Result` name=`web_search` | signature 无状态时须回放 |
| code execution 内置 | `ServerTool*` name=`code_interpreter` | details 可放 code/output 摘要 |
| `url_context` | `ServerTool*` name=`url_context` | URL 上下文工具 |
| `google_maps` | `ServerTool*` name=`google_maps` | Maps grounding |
| `file_search` | `ServerTool*` name=`file_search` | 语义文件检索 |
| status `requires_action` | `StopReason::ToolUse` | 等待客户端 function_result |
| status `incomplete` | `StopReason::Incomplete` | 完成但不完整 |
| stream `step.start` / `step.delta` | Event 扁平投影 | 不引入 Opaque step |
| `usage.total_thought_tokens` | `ProviderUsage.thoughtTokens` | |

**无状态（store=false）回放契约：**

1. 回传上一轮全部 model steps（含 thought + function_call + server tool call/result）
2. 附上本轮 `function_result`
3. thought / 内置工具步骤的 **signature 不得丢**（`reasoningMustReplay=true`）
4. **禁止** Opaque 整包塞 steps

##### B. generateContent API（legacy，仍支持）

| 官方概念 | 中间协议 | 备注 |
|----------|----------|------|
| `contents[].role=user/model` | User/AssistantMessage | |
| `parts[].text` | `Part::Text` | `thought=true` 的 text → `Reasoning` |
| `parts[].inlineData` | Image/Audio/Video/Document | 按 mime 分支 |
| `parts[].fileData` | 对应资产 `uri` | Files API / GCS URI |
| `parts[].videoMetadata` | `ProviderVideoAsset` startMs/endMs/fps | |
| `parts[].mediaResolution` | `Part.mediaResolution` | low/medium/high |
| `parts[].functionCall` | `FunctionCall` | |
| `parts[].functionResponse` | `FunctionCallOutput` | |
| `parts[].executableCode` | `ServerToolCall` `code_interpreter` | details.code |
| `parts[].codeExecutionResult` | `ServerToolResult` `code_interpreter` | details.output |
| `parts[].thoughtSignature` | `reasoningSignature` | 可挂在 FC / 末 part；**tool 轮必回放** |
| `systemInstruction` | `systemPrompt` | |
| `tools.functionDeclarations` | `tools` | |
| `tools.codeExecution` / googleSearch / urlContext / … | 内置 → ServerTool 能力 | |
| `toolConfig` | `toolChoice` | mode 映射 |
| `generationConfig.temperature/topP/topK/seed/stop/penalties` | temperature + `sampling` | topK 已一等 |
| `generationConfig.maxOutputTokens` | `maxOutputTokens` | |
| `generationConfig.responseMimeType` + `responseSchema` | `responseFormat` | |
| `generationConfig.thinkingConfig` | `reasoning` | includeThoughts / thinkingBudget / thinkingLevel |
| `generationConfig.mediaResolution` | `Request.mediaResolution` | 请求级默认 |
| `cachedContent` | `providerCachedContentId` | 已创建缓存资源名 |
| `groundingMetadata` 引用 | `Part.citations` / ServerToolResult.details | 可移植摘要 |

##### C. Gemini 内置工具短名

| 短名 | 官方 |
|------|------|
| `web_search` | `google_search` / `google_search_call` |
| `file_search` | File Search |
| `code_interpreter` | `code_execution` / executableCode |
| `url_context` | `url_context` |
| `google_maps` | Maps grounding |
| `computer` | Computer Use（若启用；客户端执行则 FunctionCall） |

##### D. 与 adapter 分工

- **协议**：上表语义字段均已一等或可映射
- **adapter**：role 折叠（model↔assistant）、step 时间线投影、signature 粘附规则、内置 tool 声明 JSON
- **禁止**：把整个 `steps[]` / `candidates[]` raw 进 items

#### DeepSeek 方言注意（官方 [Thinking Mode](https://api-docs.deepseek.com/guides/thinking_mode/)）

- 协议族：`ProviderProtocolFamily::DeepSeekChatCompletions`
- 端点：OpenAI 兼容 `POST /chat/completions`（`https://api.deepseek.com`）
- 请求：`thinking: {type: enabled|disabled}` + `reasoning_effort: high|max`
  - ← `ProviderReasoningOptions::{enabled, effort}`
  - 官方兼容：`low`/`medium`→`high`，`xhigh`→`max`（adapter 负责映射）
- 输出：`message.reasoning_content` 与 `content` 同级 ← `Reasoning` Item / `ReasoningDelta`
- **多轮无 tool call**：中间 assistant 的 `reasoning_content` 可不回传（回传也会被忽略）
- **多轮有 tool call**：中间 assistant 的 `reasoning_content` **必须**回传并参与后续所有轮次
  - 协议：`Reasoning` Item 设 `reasoningMustReplay=true`，账本压缩不得丢弃
- thinking 模式下 `temperature`/`top_p`/penalties **无效**（设置不报错但不生效）← adapter 应跳过 `sampling` 相关字段
- **不要**用 Opaque 保存 DeepSeek 私有字段


#### OpenAI Responses / Chat 注意（官方 conversation state）

- **本仓默认**：无状态全量 `input` 回放（不发 `previous_response_id`）
- Responses 有状态（opt-in）：`previous_response_id` ← 显式 `continuationId`；`store` ← `storeServerState`
- 无状态续聊：须保留上一轮 **完整** `output` 项（含加密 reasoning）再塞回 `input` ← `Reasoning` + `reasoningSignature` / redacted，**禁止** Opaque 乱塞
- Chat Completions：无状态；靠 `items` 全量回放
- Conversations API 对象 id ← `providerConversationId`（可选）
- 助手 `phase` 字段（推理模型）← `ProviderItem.assistantPhase`，回放时原样带回
- `include[]` 扩展输出 ← `responseInclude`（如 web_search sources、code_interpreter outputs）
- `background` ← `backgroundExecution`

#### Gemini Interactions 注意（官方 Interactions / thinking）

- 新项目推荐 Interactions API；`generateContent` 仍支持（legacy）
- **本仓默认**：无状态全量 steps 回放；`previous_interaction_id` **仅**来自显式 `continuationId`（不用 adapter 内部 interaction 缓存自动提升）
- 有状态（opt-in）：`previous_interaction_id` ← `continuationId`；`store` ← `storeServerState`
- 思考：`generation_config.thinking_level` ← `reasoning.effort`；`thinking_summaries` ← `reasoning.includeSummary`
- Interactions 中 `thought` step 含必填 `signature` + 可选 `summary` ← `Reasoning` Item
- `tools` / `system_instruction` / `generation_config` **每轮需重传**

#### 协议族与 adapter 分工

| `ProviderProtocolFamily` | 预期 adapter |
|--------------------------|--------------|
| `OpenAiResponses` | ResponsesProvider |
| `OpenAiChatCompletions` | ChatCompletionsProvider |
| `DeepSeekChatCompletions` | DeepSeekProvider（可继承 ChatCompletions） |
| `AnthropicMessages` | AnthropicProvider |
| `GeminiGenerateContent` | GeminiProvider（generateContent） |
| `GeminiInteractions` | GeminiProvider（interactions + continuationId） |
| `Auto` | 由凭证/实例上的 provider 类型决定 |

---

## 2. 三类型关系

| 类型 | 角色 | 比喻 |
|------|------|------|
| **`ProviderItem`** | 一条终态线路内容（零件） | 积木块 |
| **`ProviderRequest`** | 一次 API 调用的完整入参 | 寄出的包裹（内含 items + 旋钮） |
| **`ProviderEvent`** | 一次调用过程中的增量事件 | 拆包裹直播；最后一幕再交出新积木 |

```
账本 / 工具结果
      │  投影为 ProviderItem[]
      ▼
ProviderRequest { items, tools, systemPrompt, … }
      │  adapter.buildTransport()
      ▼
厂商 API（HTTP / SSE）
      │  adapter.parse → ProviderEvent 流
      ▼
MessageCompleted.outputItems: ProviderItem[]   ← 与入站同型
      │
      ▼
写回账本 → 下一轮再进入 Request.items
```

要点：

- **Item 是语义核心**（对话真相）
- **Request / Event 是一次 RPC 的入出边界**
- Event 中间的 delta 主要服务 UI；**写账本以 `outputItems` 为准**

与工具域的边界：

```
ToolTypes     = 工具域名词（Spec / Call / Result / 审批）
ProviderTypes = 模型对话协议（Request / Item / Event）
```

`ToolCall` / `ToolResult` 经工厂投影为 `ProviderItem::FunctionCall` / `FunctionCallOutput`。

---

## 3. 入站：`ProviderRequest`

### 3.1 覆盖范围

| 场景 | 入侧 |
|------|------|
| 多轮文本对话 | ✅ |
| 系统提示 | ✅ |
| 工具定义 + 客户端结果回灌 | ✅ |
| 图片 / 音频 / 文档 / 视频输入 | ✅ |
| 多模态引用语义（BlobRef / UriScheme / 账本禁内联） | ✅ |
| Reasoning（effort / signature / redacted） | ✅ |
| 期望输出模态（文/图/音） | ✅ |
| 有状态续跑 `continuationId` | ✅ |
| 工具选择策略 | ✅ |
| 厂商服务端工具（ServerTool*） | ✅ 一等 kind |
| 文本 citations | ✅ |
| 基础采样（temperature / max tokens / stream） | ✅ |
| 扩展采样（top_p / seed / stop / penalties） | ✅ |
| 协议族 protocolFamily（含 DeepSeek 方言） | ✅ |
| part 级 cachePolicy（Anthropic cache_control） | ✅ |
| logprobs 请求与 MessageEnd 投影 | ✅ |
| Responses include[] / background | ✅ |
| Reasoning.mustReplay（DeepSeek tool 轮） | ✅ |
| Program / ProgramOutput（OpenAI PTC） | ✅ |
| ApprovalRequest / ApprovalResponse（MCP 审批） | ✅ |
| Compaction 条目 | ✅ |
| FunctionCall.caller* / tools.deferLoading / allowedCallers | ✅ |
| StopReason::PauseTurn（Anthropic） | ✅ |
| Gemini Interactions steps 消息格式对照 | ✅ |
| Gemini generateContent parts（含 executableCode / thoughtSignature） | ✅ |
| Gemini `url_context` / `google_maps` 短名 | ✅ |
| `sampling.topK` / `thoughtTokens` / `providerCachedContentId` / `mediaResolution` | ✅ |
| StopReason::Incomplete | ✅ |
| 结构化输出 responseFormat / 视频 part | ✅（见上文） |
| 实时双向语音会话（WebRTC） | ❌ 属传输/会话层 |
| 网关长尾私有参数 | ⚠️ metadata 暂存，禁止进 items |

### 3.2 字段形状

```
ProviderRequest
├── 身份
│   ├── requestId
│   └── conversationId
├── 上下文
│   ├── systemPrompt                 // 顶层，不进 items
│   ├── items: ProviderItem[]        // 历史 + 本轮，有序
│   └── continuationId               // 可选；有状态续跑
├── 工具
│   ├── tools: ProviderToolSpecification[]
│   └── toolChoice
├── 采样 / 控制
│   ├── maxOutputTokens   // <0 = 未指定，adapter 不序列化
│   ├── temperature       // <0 = 未指定；>=0 含 0.0 为显式
│   ├── stream
│   ├── desiredOutput                // text / image / audio
│   └── sampling                     // topP / seed / stop / penalties
├── 模态 / 格式 / 方言
│   ├── reasoning                    // enabled + effort
│   ├── audio                        // voice / format / transcript
│   ├── responseFormat               // none / json_object / json_schema
│   ├── protocolFamily               // Responses / Chat / DeepSeek / Anthropic / Gemini…
│   ├── requestLogprobs / topLogprobs
│   └── continuationId
└── metadata                         // 未一等化参数的暂存阀
```

### 3.3 `items` 内容

| `ProviderItemKind` | 含义 | 典型来源 |
|--------------------|------|----------|
| `UserMessage` | 用户文本 / 图 / 音 / 文档 | 用户输入、附件 |
| `AssistantMessage` | 历史助手回复 | 上一轮 `outputItems` |
| `FunctionCall` | 客户端工具调用 | 上一轮模型输出 |
| `FunctionCallOutput` | 客户端工具结果 | ToolRuntime |
| `Reasoning` | 思考块（可含 signature） | 上一轮 / 策略 |
| `ServerToolCall` | 厂商服务端工具调用 | 上一轮模型/服务端 |
| `ServerToolResult` | 服务端工具结果（摘要 + details） | 厂商执行结果 |
| `Program` | 程序化工具编排（code + fingerprint） | OpenAI Responses program |
| `ProgramOutput` | 程序编排结果 | OpenAI program_output |
| `ApprovalRequest` | MCP 等审批请求 | 厂商 mcp_approval_request |
| `ApprovalResponse` | 审批答复 | 应用/用户决策后回灌 |
| `Compaction` | 上下文压缩摘要 | OpenAI compaction |

常用工厂：

```cpp
ProviderItem::makeUserText(text)
ProviderItem::makeUserImage(image, caption)
ProviderItem::makeUserAudio(audio, caption)
ProviderItem::makeUserDocument(document, caption)
ProviderItem::makeAssistantText(text)
ProviderItem::makeFunctionCall(toolCall)
ProviderItem::makeFunctionCallOutput(toolResult)
ProviderItem::makeReasoning(content, signature, redacted)
ProviderItem::makeServerToolCall(callId, u"web_search", args, rawArgs)
ProviderItem::makeServerToolResult(callId, u"web_search", summary, details)
ProviderItem::makeProgram(callId, code, fingerprint)
ProviderItem::makeProgramOutput(callId, result)
ProviderItem::makeApprovalRequest(id, name, argsJson, serverLabel)
ProviderItem::makeApprovalResponse(id, requestId, true)
ProviderItem::makeCompaction(summary)
```

### 3.4 消息部件与多模态资产

```
ProviderMessagePart
├── Text     → text + optional citations[]
├── Image    → ProviderImageAsset
├── Audio    → ProviderAudioAsset
├── Document → ProviderDocumentAsset
└── Video    → ProviderVideoAsset { uri | data | blobRef, mimeType, startMs/endMs/fps }
```

`FunctionCallOutput` / `ServerToolResult` 还可带 `outputParts[]`（工具结果里的图/文部件）。

#### 资产承载优先级（引用语义）

每类资产（Image / Audio / Document / Video）统一为：

```
uri  |  data（小载荷）  |  blobRef（应用内引用）
```

| 字段 | 含义 |
|------|------|
| `uri` | 远程/本地/厂商 file 地址；方案见 `ProviderUriScheme` |
| `data` | 内联字节；**仅小载荷**；历史/账本应转 blob |
| `blobRef` | `ProviderBlobRef`：`blobId` / `contentHash` / `byteSize` / `expiresAtMs` / `scheme` |
| `mimeType` | IANA MIME |

`ProviderUriScheme`：`Unset` / `Https` / `Http` / `File` / `Data` / `ProviderFile` / `Blob`。
可用 `inferUriScheme(uri)` 从字符串推断；厂商 file id 无 scheme 时由调用方显式设 `ProviderFile`。

工厂：

```cpp
ProviderImageAsset::fromUrl(url, mime)
ProviderImageAsset::fromBytes(bytes, mime)   // 单次请求小图
ProviderImageAsset::fromBlob(blobRef, mime)  // 账本/历史推荐
// Audio / Document / Video 同理
```

#### 请求路径 vs 账本路径（内联策略）

| 路径 | API | 内联 `data` |
|------|-----|-------------|
| **发请求** | `request.validate(&err, &caps)` | 默认允许 ≤ `kProviderMaxInlineAssetBytes`（256KiB）；超限且无 `blobRef` → 失败 |
| **写账本** | `request.validateForLedger(&err, &caps)` | **禁止任何内联**（`maxInline=0`）；必须 `uri` 或 `blobRef` |
| **完成态入账** | `messageEnd.validate(&err, true, maxInline)` | 账本场景传 `0` |

**分工：**

- **协议**：表达「字节在别处」的引用（`blobId` 等）
- **运行时 blob store**：真正存/取字节（**不在** ProviderTypes 内）
- **账本**：只持引用，避免多轮 `QByteArray` 爆炸

错误示范：

```cpp
// 禁止：大图内联后直接 append 进历史且不校验
item = ProviderItem::makeUserImage(
    ProviderImageAsset::fromBytes(hugePng, u"image/png"));
ledger.append(item); // 未 validateForLedger → 内存与序列化灾难
```

正确示范：

```cpp
ProviderBlobRef ref;
ref.blobId = blobStore.put(png);
ref.byteSize = png.size();
ref.scheme = ProviderUriScheme::Blob;
ref.contentHash = sha256Hex(png);

auto image = ProviderImageAsset::fromBlob(ref, u"image/png");
auto item = ProviderItem::makeUserImage(image, u"先看这张图");

QString err;
if (!item.validate(&err, true, /*maxInline=*/0)) {
    // 拒绝入账
}
ledger.append(item);
```

### 3.5 入侧契约

1. **`items` 按时间顺序**，adapter 一遍遍历编码；禁止再拆 history/inputs。
2. **`systemPrompt` 不进 items**，由 adapter 映射为 `system` / `instructions` / `system_instruction`。
3. **默认无状态全量回放**：`continuationId` 默认空，adapter 编码**全部** `items`。账本可保留厂商 response/interaction id 作元数据，但 `ProviderRunLedger::buildRequest` **不**自动提升到请求。仅调用方**显式**写入 `continuationId` 时才可走有状态续跑（Responses `previous_response_id` / Gemini `previous_interaction_id`，并裁增量 items）；禁止 adapter 用内部成员偷偷补 id。
4. **每个 `FunctionCall` 须能对应后续 `FunctionCallOutput`**（否则部分 API 400）——由账本/校验层保证。
5. **`ServerToolCall` / `ServerToolResult` 成对**（若厂商要求回放）；`details` 只放可移植摘要，禁止厂商 raw 整包。
6. **`metadata` 不得承载核心语义**；仅暂存未一等化参数。
7. **能力校验**：图/音/文档/视频输入为真时，对照 `ModelCapabilities` 拒绝或降级，禁止静默丢 part。
8. **写账本前必须** `validateForLedger()`（或等价：`validate(..., maxInlineAssetBytes=0)`），禁止大块内联 `data` 进历史。
9. **多模态优先引用**：账本/多轮历史用 `blobRef` 或稳定 `uri`；`fromBytes` 仅单次请求小载荷。


### 3.7 多协议族请求字段

| 字段 | 用途 |
|------|------|
| `protocolFamily` | 指定编码方言；`Auto` 则跟实例 provider 类型 |
| `sampling.topP` / `seed` / `stop` / penalties | OpenAI 兼容与 Responses 子集；<0 或空表示不发送 |
| `reasoning` | 通用思考开关；DeepSeek → thinking + reasoning_effort；Anthropic → thinking |
| part.`cachePolicy` | Anthropic `cache_control: ephemeral` 等 |
| `requestLogprobs` | Chat Completions logprobs；完成态写入 `MessageEnd.logprobs` |
| `continuationId` | Responses `previous_response_id`；Gemini `previous_interaction_id` |
| `storeServerState` | Responses/Interactions `store` |
| `providerConversationId` | OpenAI Conversations 对象 id |
| `reasoning.includeSummary` | Gemini thinking_summaries / includeThoughts |
| `assistantPhase` | OpenAI 推理助手 phase 回放 |
| `responseInclude` | Responses `include[]`（sources/outputs 等） |
| `backgroundExecution` | Responses/Interactions `background` |
| `reasoningMustReplay` | DeepSeek tool 轮 / 加密 reasoning 强制回传 |
| `toolChoice.allowParallel` | Anthropic disable_parallel_tool_use；OpenAI parallel_tool_calls |
| `reasoning.budgetTokens` | Anthropic thinking.budget_tokens |
| `ToolSpec/ProviderToolSpecification.strictSchema` | Anthropic strict tool use |
| tools.`deferLoading` / `allowedCallers` / `outputSchema` | Anthropic/OpenAI tool search + programmatic callers；`allowedCallers` 使用 `ProviderCallerKind[]` |
| Item.`callerType`/`callerId` | OpenAI program 嵌套 call；Anthropic code_execution caller |
| Item Program/Approval/Compaction | Responses PTC / MCP 审批 / compaction |
| `providerCachedContentId` | Gemini cachedContent |
| `mediaResolution`（request/part） | Gemini mediaResolution |
| `sampling.topK` | Gemini topK |
| `Usage.thoughtTokens` | Gemini total_thought_tokens |

### 3.5b 校验与未指定语义

| 字段 | 未指定 | 显式 |
|------|--------|------|
| `sampling.topP` | `< 0` | `>= 0` |
| `sampling.topK` | `< 0` | `>= 0` |
| `sampling.seed` | `< 0` | `>= 0` |
| `temperature` | `< 0` | `>= 0`（**含 0.0**） |
| `maxOutputTokens` | `< 0` | `>= 0` |
| `storeServerState` / `allowParallel` / `backgroundExecution` | `TriState::Unset` | Yes/No |

- `ProviderItem::validate()`：按 kind 必填/禁止字段（含 Compaction 禁止占用 callId）；默认 reasoning 单轨；默认内联上限 256KiB
- `ProviderRequest::validate(caps)`：items + metadata 白名单 + 能力门控（**发请求**）
- `ProviderRequest::validateForLedger(caps)`：同上且 **禁止内联 data**（**写账本**）
- `itemId`：稳定身份（工厂默认 UUID）；`callId`：仅工具/编排/MCP 审批关联
- 多模态：超限内联且无 `blobRef` → validate 失败；账本路径 `maxInline=0`

### 3.6 有意不放进 Request 的内容

| 不放 | 归属 |
|------|------|
| API Key / Base URL | `ProviderAuth` |
| 超时 / 重试 / 取消 | 运行时 / 传输层 |
| 审批 / Skill / 会话摘要 | 账本层，投影后再进 items |
| 完整 HTTP 头 | 传输层 |
| 真实 blob 字节存储 / 淘汰 | 运行时 blob store（协议只持 `blobId` 引用） |

---

## 4. 出站：`ProviderEvent`

### 4.1 事件序列

```
MessageStarted
  → (Text|Reasoning|ToolCall|Image|Audio|Transcript delta / part | Usage | ResponseMetadata)*
  → MessageCompleted { outputItems: ProviderItem[] }
  ↘ Error | Cancelled   // 终端：之后不得 Completed/delta
```

补充契约：

- 同一次 `outputItems` 可含**多个** `FunctionCall`（并行工具）
- tool 参数 `rawArguments` 流式增量语义为 **append**
- 图片/音频最终应进入 `outputItems` 的 `AssistantMessage` parts，便于下一轮历史回放
- 写账本以 **`MessageCompleted.outputItems`** 为权威源；delta 不单独作为终态真相
- **`providerResponseId`** = 厂商 continuation id；**`messageId`** = 本地消息 id；禁止混用
- **`Event.sequence`** 为排序权威源；payload 内 sequence 仅为镜像
- `ToolCall*` 用 `deltaPayload.isServerTool` 区分客户端/服务端工具
- Error/Cancelled 后部分 delta 作废；可恢复片段仅可进 adapter `TurnState.fallbackOutputItems`

### 4.2 多模态矩阵

输入/输出模态与 part 对应关系如下。**终态 parts 入账时须满足 3.4 引用策略**（账本禁大块内联 `data`）。


| 模态 | 输入 | 流式出 | 终态回写 |
|------|------|--------|----------|
| 文本 | `Part::Text` | `TextDelta` | `AssistantMessage` |
| 图片 | `Part::Image` | `ImageOutput` | `makeAssistantImage` |
| 音频 | `Part::Audio` | `AudioDelta` / `TranscriptDelta` | `makeAssistantAudio` |
| 文档 | `Part::Document` | （多随完成态） | Document part |
| 视频 | `Part::Video` | （多随完成态） | Video part |
| 推理 | `reasoning` 选项 | `ReasoningDelta` | `Reasoning` Item |
| 客户端工具 | `tools` + Call/Output | `ToolCall*` | `FunctionCall` / `Output` |
| 服务端工具 | 历史 ServerTool* | 可复用 ToolCall* 或完成态 | `ServerToolCall` / `Result` |

---

## 5. 能力协商

```cpp
ModelCapabilities {
    supportsTextInput / ImageInput / AudioInput / DocumentInput / VideoInput,
    supportsTextOutput / ImageOutput / AudioOutput,
    supportsToolCalling,       // 客户端 FunctionCall
    supportsServerTools,       // 厂商 ServerTool*
    supportsReasoning, supportsToolChoice, supportsMaxOutputTokens,
    supportsCitations, supportsResponseFormat,
    supportsStatelessHistory,  // false → 上层截断/摘要
    supportsContinuation       // true → 可填 continuationId
}
```

---

## 6. 用法示例

```cpp
ProviderRequest request;
request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
request.systemPrompt = systemText;
request.stream = true;
// maxOutputTokens / temperature 默认 -1 = 未指定（不序列化）
request.tools = toolSpecs;
request.toolChoice = ProviderToolChoice::autoChoice();
request.desiredOutput = ProviderOutputSpec::textOnly();
request.reasoning = ProviderReasoningOptions::enabledOption();

// 账本/历史推荐 blob 引用；单次小图仍可用 fromBytes 后 validate 发送
ProviderBlobRef imageBlob;
imageBlob.blobId = u"blob-..."_s;
imageBlob.byteSize = png.size();
imageBlob.scheme = ProviderUriScheme::Blob;

request.items = {
    ProviderItem::makeUserText(u"先看这张图"),
    ProviderItem::makeUserImage(ProviderImageAsset::fromBlob(imageBlob, u"image/png")),
    ProviderItem::makeAssistantText(u"图里是一只猫"),
};

QString err;
if (!request.validate(&err, &capabilities)) {
    // 拒绝发送
}
// 写账本：
if (!request.validateForLedger(&err, &capabilities)) {
    // 拒绝入账（仍含内联 data 时会失败）
}

// 工具结果回灌：
// request.items.append(ProviderItem::makeFunctionCallOutput(result));
```

语音输出期望：

```cpp
request.desiredOutput = ProviderOutputSpec::textAndAudio();
request.audio.voice = u"alloy";
request.audio.requestTranscript = true;
```

---

## 7. 版本与演进

### 问题清单收口状态（协议层）

| # | 主题 | 协议层结论 |
|---|------|------------|
| P0.1 | fat Item 不变量 | `validate()` 必调约定 + kind 矩阵 + 工厂默认 itemId；C++ 不提供 private 字段（Qt MetaType） |
| P0.2 | 未指定语义 | temperature/maxOutputTokens `<0` 哨兵 |
| P0.3 | 资产引用 | **协议已解**：BlobRef + UriScheme + fromBlob；请求 ≤256KB 内联；账本 `validateForLedger` 禁内联。真实 blob store 属运行时 |
| P0.4 | itemId | 工厂默认 UUID；与 callId 分离 |
| P1.5 | Core/Extended | 分层 + 能力门控；IR 是语义层非 SDK 镜像 |
| P1.6 | Reasoning 单轨 | 字段名 reasoningText（仅 Reasoning）；工厂无侧轨；validate 默认禁其它 kind |
| P1.7 | Event 身份 | 仅 providerResponseId（已删旧 responseId）；≠ messageId；isServerTool；sequence 外层权威 |
| P1.8 | 软 Opaque | metadata/details/logprobs/vendorUsageRaw 均白名单；账本禁 vendorUsageRaw；权威用量 portableUsage |
| P1.9 | Tool 胶水 | toProviderToolSpecification / functionCallOutputFromToolResult |
| P1.10 | Approval 命名 | ToolPermissionApprovalRequest vs Item Approval* |
| P2.11 | 能力 | **flags: QSet&lt;ProviderCapability&gt;** + supportedServerTools + baseline；supportsXxx() 为便捷只读 |
| P2.12 | 字符串 | **ProviderCallerKind / ReasoningEffort / ItemStatus 枚举** + parse/toString；server tool 短名仍 QString+isKnown |
| P2.13 | 传输泄漏 | ProviderAdapterTypes.h，不进 ProviderTypes.h |
| P2.14 | 值语义 | **按值快照 + 只拷贝** 即本模块 immutability；关键类型注释已钉死；非 private 封装 |

`kProviderProtocolRevision` 随硬化递增。Adapter/单测属实现层，不在协议类型收口范围。

v2 清理源码兼容层：所有消费者只允许包含正式入口
`providers/ProviderTypes/ProviderTypes.h`；删除历史
`providers/core/ProviderTypes.h` 转发头和 deprecated 字符串辅助 API。
账本 JSON 中没有 `providerItem` 的旧记录仅作为 UI 投影读取，不再反向猜测协议历史。

### 值语义与不可变约定

协议对象（`ProviderItem` / `ProviderRequest` / `ProviderEvent` / `ModelCapabilities`）是 **值类型快照**。

本模块对全局「immutability」规范的落地形态：

| 要求 | 本协议做法 |
|------|------------|
| 避免隐蔽副作用 | 跨层 **只拷贝 / 按值**，禁止多模块长期共享可写引用 |
| 变更可追踪 | 入账前 `validate` / `validateForLedger`，写入 **新快照** |
| 类型可信号传递 | 保持 public struct + MetaType（非 private 实体对象） |

约定：

- 跨线程 / 跨层 **只拷贝**，禁止跨任务共享可写引用并就地改
- 工厂 + `validate()` 是推荐路径；public 字段为 Qt MetaType/信号友好，**不是**鼓励跨层 mutate
- 发请求：`request.validate(...)`
- 写账本：`request.validateForLedger(...)`（禁内联多模态）
- 需要「修改」时：改本地副本或重建对象，再替换账本中的条目

### 能力集合（ProviderCapability）

`ModelCapabilities` **不再**以 30+ 个 bool 为主存储：

```cpp
QSet<ProviderCapability> flags;      // 主存储
QStringList supportedServerTools;  // 细粒度 server tool 短名
bool has(ProviderCapability) const;
bool supportsTextInput() const;    // 便捷：读 flags
```

默认能力档：`textToolsBaseline()` / `agentMultimodalBaseline()`。

### 枚举化线路字符串

| 原自由字符串 | 枚举 |
|--------------|------|
| callerType | `ProviderCallerKind` |
| reasoning.effort | `ProviderReasoningEffort` |
| item.status | `ProviderItemStatus` |
| server tool name | 仍为 `QString` + `isKnownServerToolName`（开放登记表） |

`toString` / `parse*` 供 adapter 与 wire 互转。

### Core vs Extended kind

| 层 | kind | 能力门控 |
|----|------|----------|
| **Core** | User/Assistant/Function*/Reasoning | 基线，所有 adapter |
| **Extended** | ServerTool* / Program* / Approval*（MCP）/ Compaction | 对应 `supports*`；未声明则 `Request::validate` 拒绝 |

`kProviderProtocolVersion=2` 延续 v1 主形状；`kProviderProtocolRevision`
记录不破坏主形状的一等加法修订。

### v2（当前）

主形状保持不变，但删除兼容入口：

- 删除历史 `providers/core/ProviderTypes.h` 转发头
- 删除 deprecated caller / reasoning 字符串辅助 API，统一使用枚举 parse/toString
- `ProviderToolSpecification.allowedCallers` 改为 `ProviderCallerKind[]`，wire 字符串只存在于 adapter 边界
- 账本 UI/工具域入站显式走 `appendUiIngress`；厂商终态只走 `appendProviderItem`
- 缺少 `providerItem` 的旧账本记录只作 UI 展示，不再反向生成协议历史

### v1（历史）

主形状冻结：单一 `items[]`、入出同型 Item、无 Opaque、Event 扁平信封。

**已落地的一等加法（仍属 v1 语义扩展，非双轨）：**

- `ServerToolCall` / `ServerToolResult` + `ProviderServerToolName`
- `Part::Document` / `Part::Video` + 对应资产
- `ProviderCitation`（挂在 Text part）
- `Reasoning` 的 `signature` / `redacted`
- `ProviderResponseFormat`（JSON / JSON Schema）
- 工具结果 `outputParts`（多模态 tool result）
- `ProviderProtocolFamily`（Responses / Chat / DeepSeek / Anthropic / Gemini…）
- `ProviderSamplingOptions`（top_p / seed / stop / penalties）
- part 级 `cachePolicy`（Ephemeral）
- `requestLogprobs` + `MessageEnd.logprobs`（可移植 JSON）
- `storeServerState` / `providerConversationId`（厂商侧会话持久化）
- `reasoning.includeSummary`、`effort=max`；`assistantPhase`；推理 signature 回放约定
- DeepSeek / Gemini Interactions / OpenAI Responses 官方字段对照专节
- `responseInclude` / `backgroundExecution` / `reasoningMustReplay`
- `toolChoice.allowParallel`；`reasoning.budgetTokens`；`strictSchema`
- `Program` / `ProgramOutput` / `ApprovalRequest` / `ApprovalResponse` / `Compaction` 一等 kind
- `ProviderServerToolName` 扩展：mcp_list_tools / tool_search / local_shell / shell / apply_patch / advisor
- `callerType` / `callerId`；工具 `deferLoading` / `allowedCallers` / `outputSchema`
- `StopReason::PauseTurn`；`ProviderResponseMetadata.containerId`
- 能力位：`supportsProgrammaticToolCalling` / `supportsToolSearch` / `supportsMcpApproval` / `supportsCompaction`
- Gemini 消息格式：Interactions steps 全表 + generateContent parts 全表；`url_context`/`google_maps`
- `sampling.topK`；`Usage.thoughtTokens`；`providerCachedContentId`；`mediaResolution`（request + part）
- `StopReason::Incomplete`；能力位 topK/cachedContent/mediaResolution/urlContext/googleMaps
- **硬化 P0–P2**：`validate()` / `validateForLedger()`；temperature/maxOutputTokens 哨兵；`itemId`；`ProviderBlobRef` + `UriScheme`；
  Event `providerResponseId` 与 messageId 分离；`isServerTool`；metadata/details 键白名单；
  `toProviderToolSpecification`；`ToolPermissionApprovalRequest`；`supportedServerTools`；
  parse/isKnown*；Core/Extended kind；`kProviderProtocolRevision`

### 可加、不破主形状

- 实时双向语音（WebRTC，属传输层）
- 完整 computer/code 逐步 UI 事件细分
- 新厂商能力：查文档 → 登记短名或新 kind（仍禁止 Opaque；遵循 `.agents/skills/update-provider-protocol`）

### 禁止

- 将 `items` 再拆成 history / inputs 双轨
- 恢复 `Opaque` / 把厂商 raw 整包塞进 `details`
- `ProviderEvent` 全量 `std::variant` 化
- 把 Auth / 超时 / 审批塞进本协议

### 落地优先级（协议之外）

1. `AbstractProvider` + 传输通道  
2. 最小 ChatCompletions adapter  
3. `ProviderRunLedger`（账本 ↔ items）  
4. Agent 工具环  

---

## 8. 相关文件

```
src/runtime/providers/ProviderTypes/
├── ProviderTypes.h          # 账本 IR 聚合头
├── ProviderAdapterTypes.h   # Transport/TurnState（adapter）
├── ProviderCommon.h/.cpp
├── ProviderItem.h/.cpp
├── ProviderRequest.h/.cpp
└── ProviderEvent.h/.cpp
src/runtime/tools/ToolTypes.h
docs/协议/provider-protocol.md                # 本文
docs/图示/provider-types-structure.svg        # 协议结构总览
docs/图示/tool-types-architecture.svg         # 工具类型
docs/规范/codestyle.md
```
