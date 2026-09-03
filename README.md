# AgentFramework

Qt 6 上的 **Agent 执行单元内核**：一轮对话、工具、账本、压缩、邮箱、编排口。包版本 0.3.0。

目标与边界见 [AGENTS.md](AGENTS.md)。

不是 GUI/TUI，也不是 Host 协议。配方、MCP、AppPaths、组合根由宿主提供。

## 0.3 变更（breaking）

- `ProviderRunLedger` 对外 API 形状统一：
  - `setProviderItemForEntry` / `rollbackUncommittedTurn` / `markSubmitted` / `markEntriesCompacted` / `fromJson` 改为返回 `bool`
  - `toJson()` 返回版本化信封 `QJsonObject`（`schemaVersion` / `providerProtocolVersion` / `providerProtocolRevision` / `entries`）
  - `fromJson()` 接受新信封或旧裸数组，并做版本校验 + ProviderItem 校验
  - `providerItems()` 返回 hydrate 后的完整数据；新增 `providerItemsUnhydrated()`
  - `estimatedContextTokens()` 改为 non-const
- 账本内部引入索引，`findById` / `findProviderRecord` / `findToolCallByUseId` 变 O(1)
- 回滚统一为回合事务模型：孤儿 ToolResult 和 Error 会被清理
- 加载历史数据不再写 blob 文件；blob hydrate 失败可观测（`buildRequest().hydrateError`）
- 新增 `referencedBlobIds()` 供宿主做全局 blob GC

## 0.2 变更（breaking）

- 邮箱投递改为两阶段确认：`takePendingInboxMessages()` 不再自动 ack，投递成功需调用 `ackInboxMessages()`，失败调用 `requeueInboxMessages()`；清理时 `clearInbox()` 发出 Dropped 事件。
- `enqueueInboxMessage()` 返回 `bool`（容量/大小超限时拒绝并发出 `EventInboxMessageDropped`）。
- `AbstractLoop::enqueueMessage()` / `enqueueAgentTask()` / `enqueueUserMessage*()` 返回 `bool`（空消息返回 `false`）。
- `AgentInboxMessage` 增加 `schemaVersion` / `type` / `payload` / `priority` / `inFlight`。
- `SessionRuntime` 新增 `maxInboxMessages` / `maxInboxMessageSize`（0 = 不限）。
- 新增 IR 事件：`EventInboxMessageEnqueued` / `EventInboxMessageDelivered` / `EventInboxMessageDropped`。

```powershell
# 本仓根（独立）
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure

# 安装
cmake --install build --prefix <prefix> --component AgentFramework
```

公开头：`src/runtime/framework/AgentFramework.h`。仓外最小宿主：`examples/minimal`。

Provider 协议：`docs/协议/provider-protocol.md`；演进 skill：`.agents/skills/update-provider-protocol/SKILL.md`。
