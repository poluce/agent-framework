#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "AbstractOrchestration.h"
#include "Agent.h"
#include "tools/AbstractSession.h"
#include "types/CoreEvent.h"
#include "types/CoreEventChannel.h"
#include "config/SessionRuntime.h"
#include <functional>
#include <optional>
#include <memory>

class Agent;
struct SessionRuntime;
class ProviderCredential;
struct ToolCall;
struct ToolResult;
class AbstractToolSource;
class ToolCoordinator;
class WriteCoordinator;
class AbstractProvider;
class FileSkillLoader;
class SystemPromptBuilder;
struct ProviderAuth;
struct ProviderRequest;
struct ProviderEvent;

/// 创建 AgentSession 时一次性的不可变配置
struct AgentSessionConfig
{
    const SessionRuntime *globalDefaults = nullptr;
    ProviderCredential *credentialStore = nullptr;
    /// 进程级外部工具源（MCP 等）；非拥有。空 = 无外部工具。
    AbstractToolSource *externalToolSource = nullptr;
    /// 组合根注入的 Provider 工厂；空则本会话无法开轮 / 压缩 / 自动改名。
    AbstractLoop::ProviderFactory providerFactory;
    /// 技能目录；非拥有。空 = skill_list 无技能。
    FileSkillLoader *skillLoader = nullptr;
    /// 系统提示词拼装器；非拥有。空则 Loop 用无槽位的内置回落。
    SystemPromptBuilder *promptBuilder = nullptr;
    /// 模式策略工厂；空 = 不拦截工具，提示词走 promptBuilder / 内置回落。
    AbstractLoop::ModePolicyFactory modePolicyFactory;
    /// 编排实现；非拥有。空 = 无 spawn/team、createUnit 不可用（主会话由组合根注入）。
    AbstractOrchestration *orchestration = nullptr;
};

/// 执行单元表：登记 / 查找 / 喂任务。编排是可选配方，不是会话身份。
/// 接收 AgentSessionConfig，内部闭环单元 wiring（配置、工具、handler）。
class AgentSession : public QObject, public AbstractSession
{
    Q_OBJECT

public:
    explicit AgentSession(const AgentSessionConfig &config, QObject *parent = nullptr);
    ~AgentSession() override;

    ToolCoordinator *coordinator() const;
    [[nodiscard]] FileSkillLoader *skillLoader() const override { return m_config.skillLoader; }
    [[nodiscard]] SystemPromptBuilder *promptBuilder() const { return m_config.promptBuilder; }
    [[nodiscard]] AbstractOrchestration *orchestration() const { return m_config.orchestration; }

    // ── AbstractSession（工具层窄视图）──
    AbstractUnit *findUnit(const QString &agentId) const override { return findById(agentId); }
    bool toolVisible(AbstractUnit *unit,
                     const QString &sourceId,
                     const QString &toolName) const override
    {
        return m_config.orchestration
            ? m_config.orchestration->toolVisible(static_cast<const Agent *>(unit), sourceId, toolName)
            : true;
    }
    QString userCustomPrompt() const override;
    void setUserCustomPrompt(const QString &text) override;
    static AgentSession *fromAgent(Agent *agent)
    {
        return agent ? qobject_cast<AgentSession *>(agent->parent()) : nullptr;
    }
    void setToolResultStoreDir(const QString &dir);
    void setModelResponseTimeoutSecs(int timeoutSecs);
    void setMaxRetries(int retries);

    /// 会话唯一活运行时配置（非 Agent 执行副本）。
    const SessionRuntime &runtime() const override;
    /// 整表替换并规范化；变更字段逐条 EventConfigChanged，再同步主单元（若有）。
    void setRuntime(const SessionRuntime &runtime);
    /// 单字段更新（规范化）；返回是否实际变更。
    bool setRuntimeField(const QString &key, const QVariant &value) override;
    /**
     * 批量更新 runtime 字段（Host SetSessionConfig.patch）。
     * 全有或全无：任一键写失败则不改 m_runtime；成功时变更字段逐条 EventConfigChanged，
     * 且只同步一次主单元 / 发一次 runtimeChanged。
     * 幂等无变化仍返回 true。
     */
    bool setRuntimeFields(const QJsonObject &patch);
    void applyRuntimeToPrimary();
    QString workingDirectory() const;
    void setSessionWorkingDirectory(const QString &workingDirectory) override;

    // ── 会话标识 ──
    QString sessionId() const;
    void setSessionId(const QString &id);
    QString title() const;
    void setTitle(const QString &title);
    /** fork 源会话 id（空 = 普通/根会话）。供 TUI 分支面板构建 fork 树。 */
    QString forkedFromSessionId() const;
    void setForkedFromSessionId(const QString &sourceSessionId);

    // ── 执行单元 ──
    /// 登记一个执行单元。不要求已有主单元，不设 parent，不改审批/工作区。
    Agent *insertUnit(const QString &agentId, const QString &displayName);

    // ── 查找 ──
    Agent *findById(const QString &agentId) const;
    Agent *selectedAgent() const;
    /// 编排主单元；无编排则表中第一个。
    Agent *primaryUnit() const;
    /// 该单元是否主单元（无编排：表中第一个）。
    [[nodiscard]] bool isPrimary(const Agent *unit) const;
    bool contains(const QString &agentId) const;
    QList<Agent *> allAgents() const;
    int count() const;

    // ── 账本导出/导入（高内聚：账本进出会话归会话自己）──
    /**
     * 导出全部 agent 账本为 JSON（与 session.json 的 agents[] 同构）：
     * {agents:[{agentId,displayName,parentAgentId,isPrimary,runtime,ledger}]}
     * 供持久化 / Fork / btw 复用；不暴露 Agent/loop/ledger 细节。
     */
    QJsonObject exportLedger() const;
    /**
     * 把导出的账本 JSON 灌进本会话：缺失单元会 insertUnit。
     * 主单元优先插入（键 isPrimary）；无标记则数组第一项。
     * runtime 恢复 + ledger fromJson。与 /resume 恢复语义一致。
     * @param workingDirectoryOverride 非空时覆盖各 agent 的 workingDirectory
     *        （TUI FromLaunchCwd 锁定启动 cwd，不从 history 恢复）
     */
    void importLedger(const QJsonObject &json,
                      const QString &workingDirectoryOverride = {});

    // ── 生命周期 ──
    Agent *removeAgent(const QString &agentId);
    void clear();
    /// 清空单元表后通知编排 `onSessionStarted()`。无编排则保持空表。
    void start();

    // ── 写协调（per-file 互斥，跨 Agent 共享） ──
    WriteCoordinator *writeCoordinator() const { return m_writeCoordinator.get(); }

    // ── ID 分配 ──
    QString allocateAgentId();
    QString allocateDisplayName(const QString &requestedName) const;

    // ── 遍历 ──
    void forEach(const std::function<void(Agent *)> &fn) const;

    // ── 会话内选中 Agent（命令回落 / Host 快照；不是 UI 列表模型）──
    void setSelectedAgentId(const QString &id);
    QString selectedAgentId() const;

    // ── 内环事件 fan-out（Core 私有；非跨层契约）──
    core_ir::HandlerId addEventHandler(core_ir::EventHandler handler);
    void removeEventHandler(core_ir::HandlerId id);
    /// 供编排把配方内事件并入会话 fan-out（如团队看门狗）。
    void fanOutEvent(const core_ir::Event &event,
                     const core_ir::EventContext &context = {},
                     const core_ir::SubmissionId &submissionId = {}) const;

signals:
    // 下列信号仅供 Core 进程内（CoreApplicationService）协调；客户端只吃 HostEvent。
    void agentAdded(const QString &agentId);
    void agentRemoved(const QString &agentId);
    void titleChanged(const QString &newTitle);
    void selectedAgentChanged(const QString &agentId);
    /// 会话活 runtime 任一字段变更后发出（写回全局默认 / 快照由 Core 处理）。
    void runtimeChanged();

private:
    void connectAgentSignals(Agent *agent);
    void emitConfigFieldChanged(const QString &key, const QVariant &value);

    void triggerAutoRenameIfNeeded(Agent *agent);
    void fallbackRename(Agent *agent);
    bool isEligibleForRename(Agent *agent, QString &firstUserText) const;
    bool getProviderAuth(ProviderAuth &auth, QString &providerType) const;
    ProviderRequest buildRenameRequest(const QString &firstUserText) const;
    void handleRenameEvent(const ProviderEvent &event, AbstractProvider *provider,
                           const QMetaObject::Connection &connection,
                           const std::shared_ptr<QString> &summaryText);
    [[nodiscard]] QString sessionBlobRoot() const;
    void applyBlobRootToAgents();

    AgentSessionConfig m_config;
    /// 编排存活守卫：编排先于会话销毁（组合根声明顺序错误）时自动置空，
    /// 析构据此告警而非悬垂调用 detach。
    QPointer<AbstractOrchestration> m_orchestrationGuard;
    SessionRuntime m_runtime;
    QString m_sessionId;
    QString m_sessionBlobKey;
    QString m_title;
    QString m_forkedFromSessionId;
    QString m_toolResultStoreDir;

    std::unique_ptr<ToolCoordinator> m_coordinator;
    std::unique_ptr<WriteCoordinator> m_writeCoordinator;

    void pushEvent(const core_ir::Event &event) const;
    /// 把已规范化的 next 提交为 m_runtime（对比 toJson → 事件 → 主单元）。
    /// @return 是否实际写入（before==after 时 false）
    bool commitRuntime(const SessionRuntime &next);

    // 内环 Event handlers
    std::vector<core_ir::EventHandler> m_protocolHandlers;

    QHash<QString, Agent *> m_agents;
    QStringList m_order;
    int m_nextIndex = 1;

    QString m_selectedAgentId;
    bool m_autoRenamePending = false;
    /** importLedger 时刻（fork/恢复导入账本时置位）：AutoRename 只认导入后新增的
     *  用户消息，防 fork 瞬间用复制的历史第一条误定标题（与源会话重名）。 */
    qint64 m_ledgerImportedAtMs = 0;
};
