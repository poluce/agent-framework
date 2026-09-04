# TODO / 技术债

> 记录已知的技术债与待办重构。条目按优先级排序，完成一项勾掉一项。

## 耦合类

### [x] 1. `providers ↔ types` 依赖环（模块级环）

**状态**：已解决（2026-09-04，`refactor/decouple-modules`）

**问题**：`providers` 与 `types` 互相依赖，形成模块级环：

```
providers/service/ProviderCredential.h
  └── types/CoreEvent.h
        └── types/ConversationMessage.h
              └── providers/ProviderTypes/ProviderCommon.h
```

**根因（修正）**：环有两条边，实际拆的是最小的一条：

- 边 A（`types → providers`）：`ConversationMessage::imageOutput` 使用 `ProviderImageAsset`——稳定且有意义的依赖（会话记录携带协议资产），**保留**
- 边 B（`providers → types`）：`ProviderCredential.h` include `CoreEvent.h` 仅为 `core_ir::ApiKeyUpdateMode` 一个枚举——意外依赖，**拆除**

**修法**：把 `ApiKeyUpdateMode` 从 `types/CoreEvent.h` 挪进 `providers/service/ProviderCredential.h`（随使用方走），删除 `ProviderCredential.h` 对 `types/CoreEvent.h` 的 include。环消除，资产类型留在协议层不动。

**影响面**：

- `ApiKeyUpdateMode` 公开路径变化（`core_ir::ApiKeyUpdateMode` → `ApiKeyUpdateMode`，breaking）
- 涉及 `CoreEvent.h` / `ProviderCredential.h` / `ProviderCredential.cpp`
- 测试与文档已同步

**备注**：2026-09-03 目录调整审计时发现，属**重构前已存在**的耦合，非本次引入。原建议方向（资产下沉 `types/`）因拆协议层代价大而放弃，改为拆最小边。

---

### [x] 2. `tools → agent` 依赖（工具层反向依赖执行单元）

**状态**：已解决（2026-09-04，`refactor/decouple-modules`）

**问题**：`tools` 模块反向依赖 `agent` 模块（实为模块级环：`agent` 也正向依赖 `tools`）：

```
tools/ToolCoordinator.cpp        → agent/AbstractOrchestration.h、agent/Agent.h、agent/AgentSession.h
tools/SessionToolRuntime.cpp     → agent/AgentSession.h、agent/Agent.h
tools/session/ConfigTool.h       → agent/Agent.h、agent/AgentSession.h
tools/session/AgentTodoWriteTool.h → agent/Agent.h
tools/builtin/RunCommandTool.cpp → agent/Agent.h、agent/AgentSession.h
```

**根因**：工具运行时需要访问会话/执行单元（拿配置、写 todo、广播路径等），直接依赖了具体类；`ConfigTool` 甚至用 `qobject_cast<AgentSession*>(caller->parent())` 隐式约定取会话。

**修法**：消费方定义窄接口，agent 侧实现：

- 新增 `tools/AbstractUnit.h`（agentId / todos / setTodos / appendSessionEvent）与 `tools/AbstractSession.h`（findUnit / toolVisible / skillLoader / runtime / setRuntimeField / setSessionWorkingDirectory / userCustomPrompt / setUserCustomPrompt），由 `Agent` / `AgentSession` 实现
- 新增 `tools/SessionToolContext.h`（session + caller 包装），`AbstractSessionTool::execute/tryExecuteAsync` 入参由 `Agent*` 改为 `SessionToolContext*`（**breaking**，产品仓工具需跟随）
- `ConfigTool` 的 `parent()` hack 消除；`ToolCoordinator` / `SessionToolRuntime` / `RunCommandTool` / `BuiltinToolRuntime` 全部改持 `AbstractSession*`
- `tools/` 不再 include `agent/` 任何头

**影响面**：

- `AbstractSessionTool` 公开签名变化（`Agent*` → `SessionToolContext*`，breaking）
- 新增 3 个公开头（`tools/AbstractSession.h` / `AbstractUnit.h` / `SessionToolContext.h`）
- 涉及 `tools/` 7 个文件 + `agent/` 4 个文件 + CMake 公开头闭包
- 测试与文档已同步

**备注**：2026-09-03 目录调整审计时发现，属**重构前已存在**的耦合，非本次引入。`AgentMode` 依赖已随目录调整解开（`agent/AgentMode.h` → `config/AgentMode.h`），本次解决剩余会话/单元依赖。

---

## 其他

（暂无）
