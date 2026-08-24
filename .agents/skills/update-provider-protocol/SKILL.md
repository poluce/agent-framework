---
name: update-provider-protocol
description: >
  更新 AgentFramework 多模型中间协议（ProviderTypes）时必须遵守的流程与禁令。
  在修改 ProviderItem/ProviderRequest/ProviderEvent/ProviderCommon、
  新增消息 kind/part、对接厂商私有字段、讨论 Opaque、response_format、
  ServerTool、多模态部件，或用户提到「扩展协议 / 加 kind / 协议演进」时使用。
  强制：无 Opaque、先查厂商文档再加一等 kind、同步文档与能力位。
---

# 更新多模型中间协议（ProviderTypes）

## 何时使用

- 修改 `src/runtime/providers/ProviderTypes/**`
- 新增/调整 `ProviderItemKind`、`ProviderPartKind`、请求选项、流式事件
- 为 OpenAI / Anthropic / Gemini 等适配器补协议能力
- 用户要求「支持某某 API 字段 / server tool / 新模态」

**不要用本 skill：** 只改 adapter 映射实现、不改协议类型时——但若映射需要新 kind，先完成本 skill 再写 adapter。

## 必读（改代码前）

1. `docs/协议/provider-protocol.md` — 协议定版说明与禁令  
2. `docs/规范/codestyle.md` — Doxygen 注释规范  
3. `src/runtime/providers/ProviderTypes/ProviderTypes.h` — 设计原则摘要  
4. `src/runtime/providers/ProviderTypes/ProviderItem.h` — `ProviderItemKind` 与「不设 Opaque」注释  

结构图（可选）：`docs/图示/provider-types-structure.svg`

## 核心原则（不可破）

| # | 原则 | 含义 |
|---|------|------|
| 1 | 入出同型 | 终态只有 `ProviderItem`；`Request.items` 与 `MessageCompleted.outputItems` 同型 |
| 2 | 单一 `items[]` | **禁止**再拆 `historyItems` / `inputs` 双轨 |
| 3 | **不设 Opaque** | **禁止** `ProviderItemKind::Opaque` / `rawItem` / 厂商 JSON 整包进账本 |
| 4 | Event 扁平信封 | **禁止**把 `ProviderEvent` 全量 `std::variant` 化 |
| 5 | system 顶层 | `systemPrompt` 不进 items |
| 6 | 语义映射非 wire 同构 | 协议是 LLM-agnostic 公共语义层，不是某家 SDK 镜像 |
| 6b | 值类型快照 | Item/Request/Event 按值传递；跨层只拷贝；immutability=新快照入账 |
| 7 | 工具域分离 | 客户端工具数据在 `ToolTypes.h`；协议只投影为 FunctionCall* |

### 不设 Opaque — 硬规则

收到未知厂商块时：

| 应做 | 禁止 |
|------|------|
| 映射到已有一等 Item/Part | `makeOpaque(raw)` |
| 压成可读文本 / UI 提示 | 把 raw 写入 `items[]` |
| `continuationId` / adapter **内部**瞬态 | 用 `details` 塞厂商整包 JSON |
| `Request.metadata`（仅请求旋钮，**不进历史**） | 用 metadata 冒充对话记忆 |
| **查文档后正式加 kind/字段** | 用 Opaque 无限期拖延建模 |

`ServerToolResult.details` **只允许可移植摘要**（如 query、results[{title,url,snippet}]），不是逃生舱。

### 扩展位白名单（必须）

| 扩展位 | 函数 |
|--------|------|
| Request.metadata | `isAllowedRequestMetadataKey` |
| Item.details | `validateToolDetailsObject` / `isAllowedToolDetailsKey` |
| MessageEnd.logprobs | `validateLogprobsObject` |
| vendorUsageRaw | `validateVendorUsageRawObject`（账本 forbidNonEmpty） |

禁止：用扩展位塞厂商整包 raw、对话正文、或绕过一等字段。

## 标准流程：查文档 → 再加 kind

```text
1. 确认需求属于协议层（终态语义 / 请求选项 / 流式事件）
2. 查至少 1～2 家官方文档（OpenAI Responses / Chat Completions / DeepSeek / Anthropic Messages / Gemini…）
3. 判断：已有 Item/Part/选项能否语义覆盖？
   - 能 → 只改 adapter 映射，不扩协议
   - 不能 → 设计最小一等加法（kind / part / 选项字段）
4. 实现：类型 + 工厂 +（如需）能力位 + 注释
5. 同步：docs/协议/provider-protocol.md（对照表、覆盖范围、演进列表）
6. 自检清单（见下）全部勾选
```

### 查文档时记什么

- 官方 **type / block 名称** 与关键字段  
- 是 **客户端工具** 还是 **服务端工具**  
- 是否需要 **历史回放**（签名、continuation）  
- 可移植子集是什么（跨厂商都能理解的字段）

### 优先映射到已有类型

| 厂商概念（例） | 优先映射 |
|----------------|----------|
| tool_use / function_call | `FunctionCall` |
| tool_result / function_call_output | `FunctionCallOutput`（+ 可选 `outputParts`） |
| thinking / reasoning / thought | `Reasoning`（+ signature/redacted） |
| web_search_call / server_tool_use / toolCall | `ServerToolCall` + `ProviderServerToolName::*` |
| 对应 result | `ServerToolResult` |
| image / pdf / audio / video | `Part::Image/Document/Audio/Video` + `ProviderBlobRef`（账本禁大块 data） |
| response_format / responseMimeType | `ProviderResponseFormat` |
| previous_response_id | `continuationId`（**opt-in**；默认空=全量 items 回放，ledger 不自动提升） |
| DeepSeek thinking / reasoning_content | `reasoning` 选项 + `Reasoning` Item / ReasoningDelta |
| Anthropic cache_control | part.`cachePolicy` = Ephemeral |
| top_p / seed / stop | `ProviderSamplingOptions` |
| 协议方言选择 | `ProviderProtocolFamily` |
| store / previous_*_id | `storeServerState` / `continuationId`（默认无状态；adapter 禁止内部成员偷偷补 previous_*） |
| DeepSeek 有 tool 时的 reasoning_content 必回传 | `Reasoning` Item 保留并进入后续 items |
| Gemini thought signature | `Reasoning.reasoningSignature` |
| OpenAI assistant phase | `ProviderItem.assistantPhase` |
| OpenAI include[] | `responseInclude` |
| background 长任务 | `backgroundExecution` |
| DeepSeek tool 轮 reasoning 必回传 | `Reasoning.reasoningMustReplay=true` |
| 禁用并行工具 | `toolChoice.allowParallel = No` |
| Anthropic thinking budget | `reasoning.budgetTokens` |
| 严格工具 schema | `strictSchema` on tool spec |
| OpenAI program / program_output | `Program` / `ProgramOutput`（fingerprint 必回放） |
| OpenAI mcp_approval_* | `ApprovalRequest` / `ApprovalResponse` |
| OpenAI compaction | `Compaction` |
| function_call.caller / tool_use.caller | Item.`callerType` + `callerId` |
| defer_loading / allowed_callers | tools.`deferLoading` / `allowedCallers`（协议侧 `ProviderCallerKind[]`） |
| Anthropic pause_turn | `StopReason::PauseTurn` |
| Anthropic code_execution / tool_search / advisor | ServerTool 短名 code_interpreter / tool_search / advisor |
| Gemini Interactions thought/function_call/model_output | Reasoning / FunctionCall* / AssistantMessage |
| Gemini google_search_call/result | ServerTool* web_search + signature 回放 |
| Gemini executableCode / codeExecutionResult | ServerTool* code_interpreter |
| Gemini url_context / google_maps | ServerTool 短名 url_context / google_maps |
| Gemini thoughtSignature on FC | FunctionCall.reasoningSignature + mustReplay |
| Gemini cachedContent | Request.providerCachedContentId |
| Gemini topK / mediaResolution / thought tokens | sampling.topK / mediaResolution / Usage.thoughtTokens |
| Gemini requires_action / incomplete | StopReason::ToolUse / Incomplete |
| 非法字段组合 | `ProviderItem::validate` / `ProviderRequest::validate` |
| 未指定 temperature/max tokens | `< 0` 哨兵，adapter 不序列化 |
| 多模态历史 | `fromBlob` / `validateForLedger`（禁内联）；blob store 在运行时 |
| 模型能力 | `QSet<ProviderCapability>` + `supportedServerTools`，勿再堆 bool |
| caller/effort/status | 用枚举 `ProviderCallerKind` / `ReasoningEffort` / `ItemStatus` |
| 多模态历史 | `ProviderBlobRef`（blobId），禁止大块 data 进账本 |
| 条目身份 | `itemId`（≠ callId） |
| 本机权限审批 | `ToolPermissionApprovalRequest`（≠ MCP Approval*） |
| ToolSpec → 线路 | `toProviderToolSpecification` |

服务端工具短名登记处：`ProviderServerToolName`（`ProviderItem.h`）。  
新短名：先查文档 → 加入该命名空间 → 更新 `provider-protocol.md` 对照表。

## 允许的改动 vs 禁止的改动

### 允许（加法小补丁）

- 新 `ProviderItemKind` 枚举值 + 工厂 + `toDebugString` 分支  
- 新 `ProviderPartKind` + 资产 struct + `MessagePart` 工厂  
- `ProviderRequest` 新选项（如 `responseFormat`）  
- `ModelCapabilities` 新布尔位  
- `ProviderEventKind` / delta 字段（保持扁平信封 + 工厂）  
- `ProviderServerToolName` 新常量  
- 文档与结构图同步  

### 禁止

- `Opaque` / 无约束 `rawItem` 进 `ProviderItem`  
- 拆回 `inputs` + `historyItems`  
- Event 全量 variant 化  
- 把 Auth、超时、重试、审批、HTTP 头塞进协议  
- 为单家厂商在 Item 上挂私有必填字段（应放 adapter 内部或可移植子集）  
- 只改 `.h` 不改工厂/文档/能力位导致半套协议  

## 文件落点

| 改动 | 文件 |
|------|------|
| 资产、通用枚举、Usage/Error、能力 | `ProviderCommon.h/.cpp` |
| Item / Part / Citation / ServerTool 名 / 工厂 / validate | `ProviderItem.h/.cpp` |
| Request 与选项（reasoning/audio/responseFormat…） | `ProviderRequest.h/.cpp` |
| 流式事件（账本 IR） | `ProviderEvent.h/.cpp` |
| Transport / TurnState（仅 adapter） | `ProviderAdapterTypes.h` |
| 原则摘要 | `ProviderTypes.h`（账本聚合头，不含 AdapterTypes） |
| 客户端工具 Spec/Call/Result | `src/runtime/tools/ToolTypes.h`（**不要**塞进 ProviderItem 执行逻辑） |
| 定版说明 | `docs/协议/provider-protocol.md` |
| 注释风格 | `docs/规范/codestyle.md` |

依赖方向（**保持无环**）：

```text
ProviderCommon ← ProviderItem ← ProviderRequest
                      ↑
                 ProviderEvent
                      ↑
               ProviderTypes.h（仅聚合 include）
```

## 实现检查清单

改协议时逐项确认：

- [ ] 已读 `provider-protocol.md` 禁令与对照表  
- [ ] 已查相关厂商文档，并在 PR/说明里写清映射来源  
- [ ] 不能用现有 kind/part 覆盖才新增  
- [ ] 新类型有 `///` / `@brief` 注释（符合 codestyle）  
- [ ] 新 Item/Part 有工厂方法；禁止靠手填非法字段组合成为常规路径  
- [ ] `toDebugString` / `isToolRelated` 等辅助方法已更新  
- [ ] 如影响能力探测：更新 `ModelCapabilities`  
- [ ] 如影响请求：更新 `ProviderRequest` 字段与文档中的字段树  
- [ ] `docs/协议/provider-protocol.md`：覆盖表、对照表、演进「已落地加法」已改  
- [ ] **未**引入 Opaque；`details`/`metadata` 未变成 raw 垃圾桶  
- [ ] 聚合头 `ProviderTypes.h` 原则列表若语义变化则同步  

## 示例：正确扩展

**需求：** 支持厂商联网搜索进历史，便于 UI 展示且跨模型不炸。

1. 查文档：OpenAI `web_search_call`、Anthropic `server_tool_use` + `web_search_tool_result`  
2. 映射：`ServerToolCall` / `ServerToolResult`，`name = ProviderServerToolName::WebSearch`  
3. `details` 只放 `{ "query", "results":[{title,url,snippet}] }`  
4. 不把官方整段 JSON 存进 Item  
5. 更新对照表与能力位 `supportsServerTools`  

**错误示范：**

```cpp
// 禁止
items.append(makeOpaque(vendorJson));
item.details = vendorJson; // 整包 raw
```

## 示例：不应改协议

**需求：** 某网关要自定义 HTTP 头。

→ 放传输层 / `ProviderAuth` 或 channel 配置，**不动** ProviderTypes。

**需求：** 用户审批危险工具。

→ `ToolTypes::PendingApprovalRequest` + 运行时，**不**为审批加 Item kind（审批不是模型线路语义）。

## 与 adapter 的分工

| 层 | 职责 |
|----|------|
| 协议（本 skill） | 稳定、可移植的语义类型 |
| Adapter | 厂商 JSON ↔ 协议；归一化 server tool 短名；消化无法建模的长尾（文本/UI/continuation） |
| 账本 / Loop | 只认 `ProviderItem` / `ProviderEvent`，不解析厂商 raw |

Adapter 可以暂时「降级丢弃」未知块并打日志；**若产品需要长期保留语义，必须回到本 skill 加 kind**，而不是在账本里藏 raw。

## 版本策略

- 主形状（单一 items、入出同型、无 Opaque、扁平 Event）由 **v2 延续冻结**
- 一等加法不强制 bump `kProviderProtocolVersion`，除非破坏性变更  
- 破坏性变更必须：升版本号 + 写明迁移说明 + 更新所有消费者  
- 源码消费者只使用 `providers/ProviderTypes/ProviderTypes.h` 正式入口；
  禁止新增历史 include 转发头或 deprecated 字符串兼容 API

## 快速命令（给执行者）

```text
协议根目录: src/runtime/providers/ProviderTypes/
文档:       docs/协议/provider-protocol.md
工具域:     src/runtime/tools/ToolTypes.h
本 skill:   .agents/skills/update-provider-protocol/SKILL.md
```

改完后至少 `Grep` 确认：

- 无新建 `Opaque`  
- 新 kind 出现在 `ProviderItemKind` 与工厂中  
- `provider-protocol.md` 含新类型名称  
