# Changelog

本仓库所有值得使用者关注的变更都记录在此。格式采用使用者视角分类（🔴 Breaking / 🟢 新增 / 🟡 修改 / 🔵 修复），版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [Unreleased]

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
