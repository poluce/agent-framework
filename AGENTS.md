# AGENTS.md

本文件是 **AgentFramework 内核** 的目标与边界。包版本 **0.5.1**。只依赖 Qt 6 Core + Network + Concurrent。

不是 GUI/TUI 手册，也不是 Host 协议。宿主怎么把本内核嵌进桌面应用，见上层产品仓的 `docs/agent-framework.md`。

---

## 1. 目标

给开发者一套 **执行单元稳定、编排可自写** 的框架。

- **内核只提供最小执行单元**：一轮对话、工具、账本、压缩、邮箱、可选父子边。
- **编排是配方**：谁建哪些单元、谁能看到哪些工具、单元之间怎么通信、宿主建/关单元时怎么解释，全部由配方决定。
- **使用者自己搭模式**：需要树、对等团队、顺序交接、路由、单人旁路……都写成配方，注入会话。框架不预置一张「标准模式菜单」。

### 明确不是什么

| 不是 | 原因 |
|------|------|
| 五种 Anthropic 协调模式的产品目录 | 那些文章只用来检验「单元够不够自由」。框架不负责实现、也不推销那五种。 |
| 已脱离 Qt 的 SDK | 运行时仍是 Qt Core/Network + 事件循环。这是有意选择，不是债。 |
| 表现层 / Host 协议 | GUI、TUI、`HostCommand`/`HostEvent` 属于宿主。内核不 include 它们。 |
| AppPaths / MCP / 落盘 `config.json` | 产品根路径、MCP 适配、配置文件格式由宿主注入或实现。 |

---

## 2. 分层

```
配方（宿主提供的 AbstractOrchestration 子类）
        │ 注入 AgentSessionConfig.orchestration
        ▼
AgentSession  单元表：insertUnit / findById / start
        │
        ▼
Agent         执行单元：Loop + 账本 + 工具 + 邮箱 + 可选 parentAgentId
        │
        ▼
AbstractLoop / Provider / BuiltinTools / CompactEngine
```

| 层 | 库 | 职责 |
|----|----|------|
| 公开面 | `agent_framework`（INTERFACE） | 伞头 `framework/AgentFramework.h`；只导出 `runtime/` 短路径 |
| 内核 | `agent_runtime` | 单元、会话表、Loop、工具、Provider、技能、注册表、内环 IR |
| 共享 | `agent_shared` | 日志、ProcessSafety、PathGuard、`SessionRuntime` 字段表 |

配方作者链 `agent_framework`，include `framework/AgentFramework.h`。不要去够宿主的 `CoreApplicationService` / `HostBus` / 具体配方头。

### 安装包

```powershell
cmake --install <build> --prefix <prefix> --component AgentFramework
```

```cmake
find_package(AgentFramework 0.5 REQUIRED)
target_link_libraries(my_orch PRIVATE AgentFramework::agent_framework)
```

头装在 `<prefix>/include/agent-framework/`（`framework/AgentFramework.h`）。**只装公开闭包**：伞头 + 注入面（`AbstractProvider` / 凭据 / 技能加载 / Provider 注册表）。不装传输层（`HttpSseChannel`）、厂商适配器、内置工具实现。需要 Qt 6 Core + Network + Concurrent。

`find_package(AgentFramework 0.5)`；0.x 按 SameMinorVersion（0.5 不匹配 0.6）。in-tree `agent_framework` 与安装包同一份头闭包。

仓外最小宿主：`examples/minimal`（自写单单元配方 + 假 Provider，跑完一轮；不访问网络）。

---

## 3. 执行单元

一个 `Agent` 能独立跑完一轮：调模型、执行工具、写账本。会话用 `AgentSession::insertUnit` 登记它，**不要求先有主单元，不自动设 parent**。

单元上给编排用的原语：

| 原语 | 接口 | 说明 |
|------|------|------|
| 身份 | `agentId` / `displayName` | 会话内唯一 id |
| 可选树边 | `parentAgentId` / `setParentAgentId` | 口不规定必须成树；空=无父 |
| 哑巴邮箱 | `enqueueInboxMessage` / `hasPendingInboxMessages` / `takePendingInboxMessages` / `ackInboxMessages` / `requeueInboxMessages` / `clearInbox` | 只收信。take 后需 ack/requeue；清理时 `clearInbox` 发 Dropped；报文格式由配方编码后再注入账本 |
| 开轮 | `submitUserDelivery` / `submitAgentTask` / `loop()->enqueueAgentTask` | 配方把任务喂给单元 |
| 状态 | `status()` / `busy()` / `stateChanged` | 配方在 `onUnitStateChanged` 里看 |

内核 **不** 解释 Leader、子代理、团队。那些词只允许出现在某个配方的提示词和工具 JSON 里。

会话生命周期：

1. 构造 `AgentSession(config)` → 若有编排则 `attach`，并把 `toolSource()` 加进工具总线。
2. `start()` → `clear()` → `orchestration->onSessionStarted()`。无编排则空表。
3. 配方在 `onSessionStarted` / `createUnit` 里 `insertUnit`。
4. 析构：先 `session->detach`（会话拆掉），再销毁编排对象。组合根必须保证这个顺序（先声明编排、后声明会话）。

---

## 4. 编排口

头文件：`src/runtime/agent/AbstractOrchestration.h`。

配方继承它，由组合根放进 `AgentSessionConfig.orchestration`（非拥有指针；所有权在组合根）。

### 4.1 必须实现

| 方法 | 含义 |
|------|------|
| `toolSource()` | 本配方的工具源。无额外工具则返回 `nullptr`。 |
| `attach(session)` | 接到会话（不拥有）。此后才能 `insertUnit`。 |
| `detach()` | 会话拆除时清空内部指针。 |

### 4.2 按需覆盖（有默认）

| 方法 | 默认 | 典型用途 |
|------|------|----------|
| `onSessionStarted()` | 不插单元 | 插入首批单元 |
| `onUnitInserted(unit)` | 记录第一个单元为主单元 | 覆盖时如需默认行为请调用基类（须在 `setCoordinator` 之前，段摘要管线依赖 `findById`） |
| `onUnitStateChanged(unit)` | 空 | 拉排队、空闲时投递邮箱 |
| `onUnitsClearing()` | 空 | 清团队、清排队、清主单元 id |
| `primaryUnit()` / `isPrimary(unit)` | 第一个登记的单元 | 快照、改标题、宿主选中回落 |
| `toolVisible(unit, sourceId, toolName)` | 全可见 | 对非主单元隐藏 spawn/config/mcp 等 |
| `skillVisible(unit, skillName)` | 全可见 | 按单元裁剪 `<available_skills>` 块（skillName = 技能目录名）；内核按单元组装 |
| `rolePromptFile(unit)` | 空=不拼角色块 | 只返回 **basename**（如 `role_leader.md`），禁止路径分隔符；解析根 = `:/system_prompts/`（qrc）+ `<可执行目录>/system_prompts/` |
| 模式文案 | 策略填 `AgentPromptContext.modePromptFile` | 只返回 basename。内核 `SystemPromptBuilder` 不认 `AgentMode` |
| `ownsSessionTitle(unit)` | false | 该单元空闲时是否跑 AutoRename |
| `usesSegmentSummary(unit)` | false | 是否安装段摘要队列 |
| `remainsIdleAfterTurn(unit)` | true | Completed 后是否回到 Idle（子单元常为 false） |
| `createUnit(request)` | 拒绝 | 宿主建单元走这里。`parentAgentId` 只是可选元数据 |
| `closeUnit(agentId)` | 拒绝 | 宿主关单元走这里。返回已从表移除的指针，调用方 `deleteLater` |

`UnitCreateRequest` 字段全可选：`displayName`、`parentAgentId`、`workingDirectory`、`modelName`、`approvalMode`。配方自己解释空 parent：建对等单元、建到主单元下、或直接拒绝。

### 4.3 工具

- 内核会话工具只有 `config`、`agent_todo_write`。
- spawn / 组内发信 / team_* 属于配方工具源，不要再放回 `SessionToolRuntime`。
- 特权名（谁能调用）记在配方里，用 `toolVisible` 裁；内核不认 `leaderOnly`。
- 外部工具源（如 MCP）由宿主注入 `externalToolSource`；可见性同样走 `toolVisible`。

---

## 5. 怎样写一个自定义配方

最小步骤：

1. 继承 `AbstractOrchestration`。
2. 实现 `toolSource` / `attach` / `detach`。
3. 在 `onSessionStarted` 里按你的模式 `insertUnit`。需要主单元就在 `onUnitInserted` 记下第一个（或你指定的）id。
4. 若单元之间要通信：往目标 `enqueueInboxMessage`，在 `onUnitStateChanged` 里对空闲单元 `takePendingInboxMessages`，编成你自己的报文，再 `loop()->enqueueAgentTask`；投递成功调用 `ackInboxMessages`，失败调用 `requeueInboxMessages`（`submitAgentTask` 返回是否成功入队，据此 ack/requeue）。
5. 若允许宿主加/删单元：覆盖 `createUnit` / `closeUnit`。
6. 若有专用工具：实现 `AbstractToolSource`，在 `toolSource()` 返回；用 `toolVisible` 按单元裁剪。
7. 在宿主的 `OrchestrationRegistry::add` 登记，不要改 `Agent` / `AgentSession`。

对照：本仓 `examples/minimal` 是仓外单单元 + 假 Provider。更复杂的树/对等样例在宿主产品仓的配方里，不进本内核。

不要做的：

- 不要在 GUI/TUI 里 spawn 或持有 `Agent*`。
- 不要给内核加「第二种 Leader」。新模式 = 新配方。
- 不要把排队正文、报文 XML 塞回 `Agent`。
- 不要假定单元 id 一定是 `agent-0`。宿主快照用 `isPrimary` 找主单元。
- 不要让 `AbstractToolSource` 既 `unique_ptr` 拥有又设 QObject parent（会双删）。

---

## 6. 现状与冻结面

**已经能做的**

- 写新的 `AbstractOrchestration` 子类，不改单元内核。
- `OrchestrationRegistry::add` 登记后，组合根按 id 注入。
- 同一进程里不同会话挂不同配方。
- 仓外只链安装包：自写配方 + 注入 `AbstractProvider` 工厂，跑完一轮（`examples/minimal`；假 Provider，不打真实 HTTP）。

**有意保留的约束**

| 项 | 说明 |
|----|------|
| 运行时绑 Qt | `QObject` + Qt Network + 事件循环 |
| 日志宏 | `LOGI` 走 `LogManager::instance()`。未 `init` / 未注入目录则不写文件 |
| 执行配置字段 | **0.5 冻结**：`SessionRuntime`（模型/压缩/审批/段摘要/邮箱…）。增删字段须改 `src/shared/config/SessionRuntime.fields.h` 并更新 `tests/SessionRuntimeFieldsTests.cpp` |

Provider 协议正文：`docs/协议/provider-protocol.md`。改协议走 `.agents/skills/update-provider-protocol`。

---

## 7. 源码索引

| 路径 | 内容 |
|------|------|
| `src/runtime/framework/AgentFramework.h` | 框架伞头 |
| `src/runtime/agent/AbstractOrchestration.h` | 编排口 |
| `src/runtime/agent/Agent.h` | 执行单元（含哑巴邮箱） |
| `src/runtime/agent/AgentSession.h` | 单元表 + `AgentSessionConfig` |
| `src/runtime/agent/OrchestrationRegistry.*` | 按 id 创建配方 |
| `src/runtime/agent/AgentModePolicy.h` | 模式策略口（具体策略由宿主注入） |
| `src/runtime/tools/ScriptToolSource.*` | 脚本工具桥：磁盘脚本 → 工具；create_tool/delete_tool 元工具 |
| `src/shared/config/SessionRuntime.h` | 执行配置（含 `SessionRuntime.fields.h` 字段表） |
| `src/runtime/types/` | 公共类型层：ConversationMessage / CoreEvent / CoreEventChannel |
| `docs/TODO.md` | 技术债清单（已知耦合与待办重构） |
| `docs/recipe-guide.md` | **面向配方作者**的编排指南（用户文档；AGENTS.md 是内核边界文档） |
| `docs/测试/coverage.md` | 测试覆盖率方案与踩坑记录 |
| `scripts/coverage.ps1` | 覆盖率一键流程（Windows 原生 PowerShell） |
| `examples/minimal/` | 仓外最小宿主：假 Provider 跑一轮 |
| `tests/` | 公开面测试 + 本树内核私有头测试 |

---

## 8. 构建与测试（本仓根）

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

作为产品 submodule 检出时，用产品仓脚本：`pwsh scripts/build.ps1 -Target framework` / `pwsh scripts/test.ps1 -Module Framework`（在产品仓根执行）。

### 覆盖率

```powershell
# 一键流程（Windows 原生 PowerShell；详见 docs/测试/coverage.md）
pwsh scripts/coverage.ps1
```

- 开关：`-DAGENT_FRAMEWORK_COVERAGE=ON`（仅 GCC/Clang；用独立 build 目录，不污染普通构建）
- 报告：文本汇总 + `coverage.html`（逐文件）+ `coverage.xml`（CI 上传）

**踩过的坑（问题 → 处理办法）**：

| 问题 | 处理办法 |
|------|----------|
| WSL 里跑 gcovr + Windows MinGW gcov 路径映射失败（`/mnt/...` vs `F:/...`），`--html-details` 报大量 source not found | 在 **Windows 原生 PowerShell** 用 Windows Python 装 gcovr 再跑；`--filter` 必须用正斜杠；实在不行先用普通 `--html`（单页汇总） |
| 排除 `tests/` 对象目录导致产品源码覆盖率虚低（0%） | **不要排除** tests 对象目录；用 `--filter` 只保留要统计的源码路径（如 `src/`） |
| 覆盖率构建下安装布局测试链接缺 `__gcov_*` 符号失败 | 覆盖率跑测排除：`ctest --test-dir <build> -E agent_framework_install_layout` |
| `_deps` / `third_party` 无关源码拖慢 gcovr 扫描 | `--exclude-directories` 排除 `_deps`、`third_party/*` |
| 全量 `--coverage` 首次编译很慢 | 独立 build 目录 + 增量构建，不污染普通构建 |
