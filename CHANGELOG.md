# Changelog

本仓库所有值得使用者关注的变更都记录在此。格式采用使用者视角分类（🔴 Breaking / 🟢 新增 / 🟡 修改 / 🔵 修复），版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [0.5.3] - 2026-09-05

### 🟢 新增功能

- **工具分组可见性（per-role 服务器裁剪）**：`ToolSpec::group` 给工具标逻辑分组（如 MCP 服务器名），源覆盖 `AbstractToolSource::visibleGroups(agentId)` 声明「该 agent 可见哪些分组」（空 = 不限）
  - 目录期（`ToolCoordinator::specsForAgent`）与调用期（`dispatch`，拦在 `source->invoke` 之前）**同时强制**——列得出 = 调得到；分组不在集合内的工具即使手工拼名调用也会被直接拒绝，源自包含转发路径（如 MCP `callToolAsync`）无法绕过
  - 与 `toolVisible` 是 **AND**：`toolVisible` 目录期按单工具名裁（列不列），分组按「集合」裁（调不调）
  - 内置/普通源不填 `group` 则完全不受影响（默认空 = 不限，零破坏）
  - 适合宿主做 MCP per-role 会话：角色 A 只见 `anysearch` 服务器、角色 B 只见 `fetch`（配合 mcp-qt `McpServerView` 注入提示词/资源）；内核不建模 MCP，只提供通用「分组可见性」抽象（issue #30）

## [0.5.2] - 2026-09-04

### 🔴 Breaking Changes（升级前必看）

- `config` 工具**写白名单收窄**：`approvalMode` / `toolScope` / `providerType` / `workingDirectory` 及只读投影（`contextWindow` / `maxOutputTokens` / `maxOutputTokensSource`）不再可由 agent 修改（读取不受限）。宿主/配方若依赖 agent 自改这些键，需迁移到宿主侧或配方工具

### 🟢 新增功能

- **脚本工具桥 `ScriptToolSource`**：磁盘脚本（py/js/ts）→ 内核工具；agent 可运行时用 `create_tool` / `delete_tool` 自加/删除工具（同名覆盖=更新，`keep_file`=暂停）
- **异步推送通道**：`AbstractUnit::enqueueInboxMessage(UnitInboxMessage)`——push 型工具事件经邮箱投递，配方照常 take/ack/投递（监控工具）
- **工具源生命周期钩子**：`AbstractToolSource::sessionClosing`（会话销毁）/ `sessionCleared`（会话清空）——脚本进程/临时资源确定性收尾，宿主无需手动清理
- `config("systemPromptAppend", ...)`：追加到用户提示词并持久化（agent 固化经验的安全通道；只追加不重写）
- `ToolCoordinator::removeSource` / `sourceOwner`：工具源注销与登记归属查询

### 🟡 功能修改

- `config("systemPrompt", ...)` 现在**持久化**到用户提示词文件（宿主配置 `PromptPaths.userPromptFile` 后生效）；工具描述枚举全部可写键与示例
- `config` 工具描述与读写语义更新：`SessionRuntime.systemPrompt` 字段标注「仅宿主查询用」，不参与系统提示词拼装（拼装走 `SystemPromptBuilder` 体系）
- `AbstractToolSource` 新增 `sessionClosing` / `sessionCleared` 虚方法（默认空，非破坏性）
- `ToolCoordinator::addSource` 增加 `ownerAgentId` 参数（内核内调用点已同步）
- `SystemPromptBuilder` 信号改名 `environmentDetected()`（原 `environmentReady` 与查询方法同名导致订阅方无法 connect；此前无人可订阅，实际零破坏）

### 🔵 修复

- #26 #27：`AbstractLoop.h` 聚合头改为直接 include `ProviderEvent.h`（缩小传递 include 面）
- #28：删除流式文本增量 per-delta DEBUG 日志（正常使用噪音过大）
- #29：`SystemPromptBuilder` 信号与查询方法同名，订阅方无法 connect

## [0.5.1] - 2026-09-04

### 🔴 Breaking Changes（升级前必看）

- `AgentSession::createAgent` 删除（与 `insertUnit` 完全重复；保留 `insertUnit`）

### 🟢 新增功能

- `AbstractOrchestration::skillVisible(unit, skillName)`：按单元裁剪 `<available_skills>` 技能块（per-agent 技能；与 `toolVisible` 平行）
- `SessionRuntime::fieldKeys()`：字段集指纹（X-macro 同源展开，与 `toJson()` 键一致；宿主白名单可自动比对）
- `SystemPromptBuilder::environmentReady()`：环境块就绪查询

### 🟡 功能修改

- `submitUserDelivery` / `submitAgentTask` 返回 `bool`（是否成功入队；配方可据此 ack/requeue；void→bool 对现有调用方源码兼容）
- `onUnitInserted` 默认记录第一个单元为主单元；`primaryUnit()` / `isPrimary()` 默认行为随之生效
- `rolePromptFile` 接口文档补解析根（`:/system_prompts/` + `<可执行目录>/system_prompts/`）
- `AgentSession` 析构对「编排先于会话销毁」告警（组合根声明顺序错误不再悬垂崩溃）
- `buildPrompt` 在环境块就绪前被调用时告警（不再静默降级）
- 安装包测试移除产品段（产品兼容性由产品仓负责，见 poluce/agent#21）
- 测试覆盖率方案：`AGENT_FRAMEWORK_COVERAGE` 开关 + `scripts/coverage.ps1` + skills 模块测试（0% → 87-100%）

### 🔵 修复

- #13：`agent_framework_install_layout` 全量并行偶发超时（根因：产品段拖慢测试）

## [0.5.0] - 2026-09-04

### 🔴 Breaking Changes（升级前必看）

- `core_ir::ApiKeyUpdateMode` 移出 `types/CoreEvent.h`，随使用方迁入 `providers/service/ProviderCredential.h`（`core_ir::ApiKeyUpdateMode` → `ApiKeyUpdateMode`）
- `AbstractSessionTool::execute/tryExecuteAsync` 入参由 `Agent*` 改为 `SessionToolContext*`（新增 `tools/AbstractSession.h` / `AbstractUnit.h` / `SessionToolContext.h` 三个公开头）

### 🟡 功能修改

- 模块解耦：`providers ↔ types` 依赖环消除——`ProviderCredential` 不再依赖 `types/CoreEvent.h`（TODO #1）
- 模块解耦：`tools → agent` 反向依赖消除——工具层只依赖自定窄接口（`AbstractSession` / `AbstractUnit` / `SessionToolContext`），`ConfigTool` 的 `caller->parent()` hack 移除（TODO #2）

## [0.4.0] - 2026-09-03

### 🔴 Breaking Changes（升级前必看）

- 目录结构调整，公开头路径变化：
  - `agent/SessionRuntime.h` → `config/SessionRuntime.h`
  - `agent/AgentMode.h` → `config/AgentMode.h`
  - `agent/compact/CompactConfig.h` → `config/CompactConfig.h`
  - `models/ConversationMessage.h` → `types/ConversationMessage.h`
  - `ir/CoreEvent.h` / `ir/CoreEventChannel.h` → `types/CoreEvent.h` / `types/CoreEventChannel.h`
- 移除历史兼容（#9 #10）：
  - `AgentSession::importLedger` 不再识别旧键 `isLeader`，只认 `isPrimary`
  - `ProviderRunLedger::fromJson` 只接受版本化信封（`schemaVersion=1`），裸数组旧格式直接拒绝

### 🟡 功能修改

- 目录结构调整：`SessionRuntime`/`AgentMode`/`CompactConfig` 下沉 `shared/config`，`models/`+`ir/` 合并为 `types/`，`providers/core` 拆出 `transport/`，工具目录统一小写（`builtin`/`session`）
- 模块解耦：`tools` 不再为 `AgentMode` 依赖 `agent`

### 🔵 修复

- 移除 `isLeader` 旧键兼容（#9）
- 移除裸数组旧格式兼容（#10）

## [0.3.0] - 2026-09-02

### 🔴 Breaking Changes（升级前必看）

- `ProviderRunLedger` 对外 API 形状统一：
  - `setProviderItemForEntry` / `rollbackUncommittedTurn` / `markSubmitted` / `markEntriesCompacted` / `fromJson` 改为返回 `bool`
  - `toJson()` 返回版本化信封 `QJsonObject`（`schemaVersion` / `providerProtocolVersion` / `providerProtocolRevision` / `entries`）
  - `fromJson()` 接受新信封或旧裸数组，并做版本校验 + ProviderItem 校验
  - `providerItems()` 返回 hydrate 后的完整数据；新增 `providerItemsUnhydrated()`
  - `estimatedContextTokens()` 改为 non-const
- session.json 中 ledger 字段从数组变为对象信封（旧格式仍可加载）

### 🟢 新增功能

- 账本内部引入索引，`findById` / `findProviderRecord` / `findToolCallByUseId` 变 O(1)
- blob 存储按会话隔离，会话关闭/清空时自动清理
- 新增 `referencedBlobIds()` / `gcProviderBlobs()` 作为崩溃残留的兜底
- `buildRequest()` 新增 `hydrateError`，blob 缺失可观测

### 🟡 功能修改

- 回滚统一为回合事务模型：孤儿 ToolResult 和 Error 会被清理
- 加载历史数据不再写 blob 文件
- `providerItems()` 现在返回 hydrate 后的完整数据

### 🔵 修复

- 账本线性查找导致长对话性能下降
- 序列化无版本校验，不兼容数据可能静默加载
- 加载时不校验 ProviderItem，损坏数据可能进入线路
- blob hydrate 失败静默，请求可能带空内容
- 回滚可能残留孤儿 ToolResult / Error
- 加载历史数据产生写盘副作用

## [0.2.0] - 2026-09-02

### 🔴 Breaking Changes（升级前必看）

- 邮箱投递语义变更：`takePendingInboxMessages()` 不再自动 ack
  - 调用方必须改为：投递成功 → `ackInboxMessages()`；失败 → `requeueInboxMessages()`
- `enqueueInboxMessage()` 返回类型从 `void` 改为 `bool`
- `AbstractLoop::enqueueMessage()` / `enqueueAgentTask()` / `enqueueUserMessage*()` 返回类型从 `void` 改为 `bool`
- `SessionRuntime` 新增字段 `maxInboxMessages` / `maxInboxMessageSize`（默认 0 = 不限，不改变旧行为）

### 🟢 新增功能

- 邮箱消息支持优先级（Low / Normal / High / Urgent），`takePendingInboxMessages()` 按优先级排序
- 邮箱消息模型版本化：`schemaVersion` / `type` / `payload`
- 邮箱容量/大小限制：`maxInboxMessages` / `maxInboxMessageSize`
- 新增 IR 事件：`EventInboxMessageEnqueued` / `EventInboxMessageDelivered` / `EventInboxMessageDropped`

### 🟡 功能修改

- 邮箱投递改为两阶段确认：take 只标记 in-flight，投递成功才 ack
- 会话清理时未读邮箱消息不再静默丢失，会发出 Dropped 事件
- `enqueueAgentTask()` 现在能感知投递结果（空消息返回 false）

### 🔵 修复

- #1 邮箱消息可能假送达：投递成功后才 ack
- #5 会话清理时未读邮箱消息直接丢失
- #8 enqueueAgentTask 无返回值，投递结果不可知
