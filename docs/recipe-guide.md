# 配方指南（Recipe Guide）

面向配方作者：用 AgentFramework 内核写自定义编排（`AbstractOrchestration` 子类）。

内核只提供**最小执行单元**（一轮对话、工具、账本、压缩、邮箱、可选父子边）；树、团队、顺序交接、路由等模式全部由配方搭建。本指南讲怎么搭。

---

## 1. 概念

| 概念 | 说明 |
|------|------|
| 执行单元（`Agent`） | 能独立跑完一轮：调模型、执行工具、写账本 |
| 会话（`AgentSession`） | 单元表：`insertUnit` / `findById` / `start` |
| 编排（`AbstractOrchestration`） | 你的配方：谁建哪些单元、谁能用哪些工具/技能、单元间怎么通信 |

内核**不**解释 Leader、子代理、团队——那些词只出现在你的提示词和工具 JSON 里。

---

## 2. 最小配方（10 分钟上手）

```cpp
class MyOrchestration final : public AbstractOrchestration
{
public:
    AbstractToolSource *toolSource() override { return nullptr; } // 无额外工具
    void attach(AgentSession *session) override { m_session = session; }
    void detach() override { m_session = nullptr; }

    void onSessionStarted() override
    {
        m_session->insertUnit(QStringLiteral("agent-0"), QStringLiteral("主单元"));
    }

private:
    AgentSession *m_session = nullptr;
};
```

组合根注入：

```cpp
MyOrchestration orch;                 // 先声明编排
AgentSessionConfig cfg;
cfg.orchestration = &orch;
AgentSession session(cfg);            // 后声明会话（析构顺序相反）
session.start();
```

在宿主的 `OrchestrationRegistry::add` 登记配方 id，不要改 `Agent` / `AgentSession`。

---

## 3. 编排口契约

### 3.1 必须实现

| 方法 | 含义 |
|------|------|
| `toolSource()` | 本配方的工具源。无额外工具返回 `nullptr` |
| `attach(session)` | 接到会话（不拥有）。此后才能 `insertUnit` |
| `detach()` | 会话拆除时清空内部指针 |

### 3.2 按需覆盖（有默认）

| 方法 | 默认 | 典型用途 |
|------|------|----------|
| `onSessionStarted()` | 不插单元 | 插入首批单元 |
| `onUnitInserted(unit)` | 记录第一个单元为主单元 | 覆盖时如需默认行为请调用基类 |
| `onUnitStateChanged(unit)` | 空 | 拉排队、空闲时投递邮箱 |
| `onUnitsClearing()` | 清主单元记录 | 清团队、清排队 |
| `primaryUnit()` / `isPrimary(unit)` | 第一个登记的单元 | 快照、改标题、宿主选中回落 |
| `toolVisible(unit, sourceId, toolName)` | 全可见 | 按单元裁剪工具（含 MCP 工具） |
| `skillVisible(unit, skillName)` | 全可见 | 按单元裁剪技能（skillName = 技能目录名） |
| `rolePromptFile(unit)` | 空=不拼角色块 | 只返回 **basename**（如 `role_leader.md`），禁止路径分隔符；解析根 = `:/system_prompts/`（qrc）+ `<可执行目录>/system_prompts/` |
| `ownsSessionTitle(unit)` | false | 该单元空闲时是否跑 AutoRename |
| `usesSegmentSummary(unit)` | false | 是否安装段摘要队列 |
| `remainsIdleAfterTurn(unit)` | true | Completed 后是否回到 Idle（子单元常为 false） |
| `createUnit(request)` | 拒绝 | 宿主建单元走这里 |
| `closeUnit(agentId)` | 拒绝 | 宿主关单元走这里。返回已从表移除的指针，调用方 `deleteLater` |

`UnitCreateRequest` 字段全可选：`displayName` / `parentAgentId` / `workingDirectory` / `modelName` / `approvalMode`。配方自己解释空 parent：建对等单元、建到主单元下、或直接拒绝。

---

## 4. 会话生命周期

1. 构造 `AgentSession(config)` → 若有编排则 `attach`，并把 `toolSource()` 加进工具总线。
2. `start()` → `clear()` → `orchestration->onSessionStarted()`。无编排则空表。
3. 配方在 `onSessionStarted` / `createUnit` 里 `insertUnit`。
4. 析构：先会话、后编排（组合根先声明编排、后声明会话）。顺序错误时内核会告警。

---

## 5. 为不同工作打造不同 agent（per-agent 环境）

三个通道表达每个单元的环境：

| 通道 | 机制 | 例子 |
|------|------|------|
| 创建时 | `UnitCreateRequest` 字段 | 工作目录、模型、审批模式 |
| 创建后 setter | `Agent` 公开方法（直接写执行副本，即时生效） | `setSystemPrompt` / `setDefaultShell` / `setMaxInternalSteps` / `setMaxRetries` |
| 编排口查询 | 按单元回答「这个单元能用什么」 | `toolVisible` / `skillVisible` / `rolePromptFile` |

### 表驱动模式（推荐）

```cpp
class JobOrchestration final : public AbstractOrchestration
{
public:
    // agentId → 画像
    QHash<QString, QString>     m_prompts;   // 系统提示词
    QHash<QString, QStringList> m_tools;     // 工具集（含 MCP 工具名）
    QHash<QString, QStringList> m_skills;    // 技能集（技能目录名）

    void onSessionStarted() override
    {
        for (auto it = m_prompts.begin(); it != m_prompts.end(); ++it) {
            Agent *unit = m_session->insertUnit(it.key(), it.key());
            unit->setSystemPrompt(it.value());
        }
    }

    bool toolVisible(const Agent *unit, const QString &, const QString &toolName) const override
    {
        return m_tools.value(unit->agentId()).contains(toolName);
    }

    bool skillVisible(const Agent *unit, const QString &skillName) const override
    {
        return m_skills.value(unit->agentId()).contains(skillName);
    }

private:
    AgentSession *m_session = nullptr;
};
```

组合根填表即得「审查员 / 文档作者 / 测试员」等不同 agent：提示词、工具、MCP 工具、技能各自独立。

---

## 6. 单元间通信（邮箱）

哑巴邮箱：只收信，不解释报文。两阶段确认：

```cpp
void onUnitStateChanged(Agent *unit) override
{
    if (unit->busy()) return;
    const auto messages = unit->takePendingInboxMessages();
    for (const auto &msg : messages) {
        // 编成你自己的报文，喂给目标单元
        const bool accepted = target->submitAgentTask(decode(msg));
        if (accepted) {
            unit->ackInboxMessages({msg.id});      // 投递成功 → ack
        } else {
            unit->requeueInboxMessages({msg.id});   // 失败 → requeue
        }
    }
}
```

- `submitAgentTask` 返回是否成功入队——据此 ack/requeue
- 报文格式（`type` / `payload` / `content`）由配方编码，内核不解释
- 清理时 `clearInbox` 会发 Dropped

---

## 7. 工具与 MCP

- 内核会话工具只有 `config`、`agent_todo_write`；spawn / 组内发信 / team_* 属于配方工具源
- 实现 `AbstractToolSource`，在 `toolSource()` 返回；用 `toolVisible` 按单元裁剪
- MCP 由宿主注入会话级 `externalToolSource`；谁能用哪个 MCP 工具同样走 `toolVisible`
- 特权名（谁能调用）记在配方里，内核不认 `leaderOnly`

---

## 8. 脚本工具（agent 自加工具）

`ScriptToolSource` 把磁盘上的脚本（py / js / ts）变成内核工具，并给 agent 一条**运行时自加工具**的路径。C++ 无法在运行时编译，动态工具的现实路径就是脚本（或 MCP 外部进程）。

### 8.1 装配

```cpp
auto *scriptSource = new ScriptToolSource(host);
scriptSource->setToolDirectory(host->toolDir());          // 持久：重启扫描加载
scriptSource->setEphemeralDirectory(host->blobDir());     // 临时：会话结束删除
scriptSource->setRuntimeCommand("py", "python3");         // 缺省 python3/node/ts-node
cfg.externalToolSource = scriptSource;                    // 会话级注入（同 MCP）
```

### 8.2 元工具：create_tool / delete_tool

agent 用 `create_tool` 自加工具（写文件 + 注册，下一轮即可调用）：

| 参数 | 说明 |
|------|------|
| `name` | 工具名（字母/数字/下划线，不能以数字开头） |
| `description` | 给模型的描述 |
| `code` | 脚本代码（协议见 8.3） |
| `language` | py / js / ts，缺省 py |
| `mode` | sync（同步返回）/ push（常驻 + 事件推送），缺省 sync |
| `input_schema` | 工具入参 JSON Schema |
| `ephemeral` | true = 临时工具（会话结束删除），缺省 false |

`delete_tool(name, keep_file)`：删除（注销 + 删文件）或暂停（`keep_file=true` 只注销）。同名 `create_tool` = 更新（覆盖旧文件）。

### 8.3 脚本协议（JSON 行，stdin/stdout）

脚本从 stdin 读请求，向 stdout 写结果（**stdout 只允许协议行**，调试输出走 stderr）：

```json
{"type":"invoke","id":"<reqId>","callId":"<callId>","tool":"<name>","args":{...}}
{"type":"result","id":"<reqId>","ok":true,"text":"...","structured":{...}}
{"type":"result","id":"<reqId>","ok":false,"error":"..."}
```

push 模式额外支持主动事件：

```json
{"type":"event","targetAgentId":"<可选>","text":"...","payload":{...}}
```

事件缺省投给该工具最近一次调用者；`targetAgentId` 可显式指定。事件经 `UnitInboxMessage` 进目标单元邮箱（`type="tool_event"`，`payload.tool` = 工具名），配方在 `onUnitStateChanged` 里照常 take/ack/投递。

### 8.4 监控工具（push 模式）

调用立即返回「已订阅」，事件异步到达——**不要用 `tryExecuteAsync`**（无超时、阻塞整轮）：

```cpp
void onUnitStateChanged(Agent *unit) override
{
    if (unit->busy()) return;
    const auto messages = unit->takePendingInboxMessages();
    for (const auto &msg : messages) {
        if (msg.type == "tool_event") {
            // 把事件编成任务喂回该单元
            const bool accepted = unit->submitAgentTask(decode(msg.payload));
            if (accepted) unit->ackInboxMessages({msg.id});
            else unit->requeueInboxMessages({msg.id});
        }
    }
}
```

### 8.5 生命周期与边界

- 每个脚本一个长驻进程：懒启动；崩溃自动重启；sync 型空闲超时回收（`setIdleTimeoutMs`），push 型常驻；单次调用超时杀进程（`setInvokeTimeoutMs`）
- **会话关闭（协调器析构）时**：所有脚本进程关闭、临时工具文件删除——`AbstractToolSource::sessionClosing` 钩子，宿主无需手动清理
- **会话清空（`AgentSession::clear()`）时**：脚本进程全部关闭、临时工具注销（文件删除）、订阅清空；持久工具保留，下次调用懒重启进程——`sessionCleared` 钩子
- 文件按 `<目录>/<agentId>/<name>.<ext>` 组织；首行 `@tool {...}` manifest 声明 spec
- 脚本工具默认 `Write` 权限（任意代码）；谁能调用仍由 `toolVisible` 裁，审批走 `evaluatePermission`
- 删除/暂停的归属边界在配方（`toolVisible`），内核不内置「只有创建者能删」

---

## 9. 技能

- 技能目录由宿主注入 `FileSkillLoader`（会话级）
- 内核按单元组装 `<available_skills>` 提示块：`skillVisible(unit, skillName)` 过滤
- 不覆盖 `skillVisible` = 全部用户可调技能可见

---

## 10. 不要做的

- 不要在 GUI/TUI 里 spawn 或持有 `Agent*`
- 不要给内核加「第二种 Leader」——新模式 = 新配方
- 不要把排队正文、报文 XML 塞回 `Agent`
- 不要假定单元 id 一定是 `agent-0`——宿主快照用 `isPrimary` 找主单元
- 不要让 `AbstractToolSource` 既 `unique_ptr` 拥有又设 QObject parent（会双删）
