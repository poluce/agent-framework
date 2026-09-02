# AgentFramework

Qt 6 上的 **Agent 执行单元内核**：一轮对话、工具、账本、压缩、邮箱、编排口。包版本 0.2.0。

目标与边界见 [AGENTS.md](AGENTS.md)。

不是 GUI/TUI，也不是 Host 协议。配方、MCP、AppPaths、组合根由宿主提供。

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
