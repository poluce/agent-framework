#pragma once

#include "AbstractLoop.h"
#include "config/AgentMode.h"
#include "AgentTaskManager.h"
#include "config/SessionRuntime.h"
#include "tools/AbstractUnit.h"
#include "types/CoreEvent.h"
#include <QDateTime>
#include <QJsonObject>
#include <QObject>

#include <functional>
#include <memory>
#include <optional>

struct AgentInboxMessage
{
    QString id;
    QString fromAgentId;
    QString fromDisplayName;
    QString content;
    /// 编排层语义：指向哪条消息由配方解释，内核不校验。
    QString replyTo;
    QDateTime timestamp;
    /// 消息模型版本；扩展字段时递增。
    int schemaVersion = 1;
    /// 结构化消息类型（task / status / file_ref / ...），由编排自定义。
    QString type;
    /// 结构化载荷；content 保留为人类可读兜底。
    QJsonObject payload;
    core_ir::InboxPriority priority = core_ir::InboxPriority::Normal;
    /// 内核内部状态：已确认送达（ack 后消息即从列表移除）。
    bool acked = false;
    /// 内核内部状态：take 后、ack/requeue 前为 in-flight。
    bool inFlight = false;
};

class CompactPipeline;
class ToolCoordinator;

class Agent : public QObject, public AbstractUnit
{
    Q_OBJECT

public:
    using AgentStatus = core_ir::AgentStatus;

public:
    using ProviderFactory = AbstractLoop::ProviderFactory;

    explicit Agent(const QString &agentId,
                   const QString &displayName,
                   const SessionRuntime &runtime,
                   QObject *parent = nullptr);
    ~Agent() override;

    // ── 标识 ──
    QString agentId() const override;
    QString displayName() const;
    QString parentAgentId() const;
    void setParentAgentId(const QString &parentAgentId);

    // ── Provider 配置 ──
    ToolScope toolScope() const;
    ApprovalMode approvalMode() const;
    void setSystemPrompt(const QString &systemPrompt);
    void setModelResponseTimeoutSecs(int timeoutSecs);
    void setCoordinator(ToolCoordinator *coordinator);
    void setProviderFactory(ProviderFactory factory);
    void setPromptBuilder(class SystemPromptBuilder *builder);
    void setModePolicyFactory(AbstractLoop::ModePolicyFactory factory);
    void setCredentialStore(class ProviderCredential *credentialStore);
    void setToolResultStoreDirectory(const QString &directoryPath);
    int maxInternalSteps() const;
    void setMaxInternalSteps(int steps);
    void setMaxRetries(int retries);
    QString defaultShell() const;
    void setDefaultShell(const QString &shell);

    const SessionRuntime &runtime() const { return m_runtime; }
    void applySessionSettings(const SessionRuntime &settings);

    // ── 状态查询 ──
    [[nodiscard]] core_ir::AgentPhase currentPhase() const;
    QString lastError() const;
    /// 仅宿主查询用；不参与系统提示词拼装（拼装走 SystemPromptBuilder 体系）。
    QString systemPrompt() const;
    bool busy() const;
    bool hasPendingApproval() const;
    QString pendingApprovalSummary() const;
    bool hasPendingQuestion() const;
    int pendingQuestionCount() const;
    QString pendingQuestionIdAt(int index) const;
    QString pendingQuestionTextAt(int index) const;
    QStringList pendingQuestionOptionsAt(int index) const;
    bool pendingQuestionIsMultiSelectAt(int index) const;
    AgentStatus status() const;
    static QString statusToString(AgentStatus s);
    void setManagerStatus(AgentStatus status);
    QString latestSummary() const;
    qint64 currentContextTokenEstimate() const;
    /// 段摘要进度：自上次写库/入队末尾起累计可摘要 token（编排未开段摘要 / 关摘要 → 0）
    [[nodiscard]] qint64 segmentSummaryAddedTokens() const;
    QString workingDirectory() const;
    QString sessionUuid() const;
    void setSessionUuid(const QString &uuid);
    AgentTaskManager *taskManager() const;

    // ── AbstractUnit（工具层窄视图）──
    QJsonArray todos() const override { return taskManager() ? taskManager()->todos() : QJsonArray(); }
    void setTodos(const QJsonArray &todos) override
    {
        if (taskManager()) {
            taskManager()->setTodos(todos);
        }
    }
    /// 窄报文入队：补全 id/时间戳/优先级后转完整报文（见 AgentInboxMessage）。
    bool enqueueInboxMessage(const UnitInboxMessage &msg) override;

    // ── 操作 ──
    void submitUserMessageWithSkill(const QString &message,
                                    const QStringList &filePaths,
                                    const QString &skillName,
                                    const QString &skillBody);
    /**
     * 忙时/空闲统一入口：按 delivery 入队；Idle 且 NextTurn 时 start 开轮。
     * Busy 时仅入队不 start。返回是否成功入队（空消息/空附件返回 false）。
     */
    bool submitUserDelivery(const QString &message,
                            const QStringList &attachedFilePaths,
                            AbstractLoop::UserDelivery delivery);
    /// 确认 next_turn 待发送（须 Idle）；无条目返回 false
    [[nodiscard]] bool confirmPendingNextTurns();
    void discardPendingNextTurns();
    [[nodiscard]] int pendingNextTurnCount() const;
    [[nodiscard]] QStringList pendingNextTurnPreviews(int maxItems = 10) const;
    [[nodiscard]] bool prefersSteerDelivery() const;
    /// 提交代理任务（NextTurn 开轮）；返回是否成功入队（空消息返回 false）。
    bool submitAgentTask(const QString &message);
    void cancelCurrentTurn();
    /// 失败收口后同会话再打一轮；无待重放用户消息则 false。
    [[nodiscard]] bool retryFailedMessage();
    [[nodiscard]] bool canRetryFailedMessage() const;
    void approvePendingAction();
    void rejectPendingAction();
    void submitQuestionAnswer(int questionIndex, const QString &answer);
    void appendSessionEvent(const QString &text) override;

    /**
     * 手动压缩：绕过 token 门控。压完停 Idle，不自动续轮。
     * 仍拒：主 Loop Busy、已在手动压、大压引擎真在跑。
     */
    [[nodiscard]] bool requestManualCompaction(qint64 targetTokens = -1);

    /// ClearConversation / 析构：清摘要队列与库
    void clearSummaryState();

    [[nodiscard]] bool hasSegmentSummaryQueue() const;
    [[nodiscard]] int segmentSummaryJobCount() const;
    [[nodiscard]] bool isWaitingSegmentSummaryAtBoundary() const;
    [[nodiscard]] bool segmentSummaryStoreEmpty() const;
    [[nodiscard]] int segmentSummaryRecordCount() const;
    [[nodiscard]] QJsonObject exportSummaryState() const;
    void importSummaryState(const QJsonObject &obj);
    void probeSegmentSummaryAfterTurnSuccess();
    [[nodiscard]] bool hasFailedSegmentSummaryJobs() const;

    // ── 运行时 ──
    class AbstractLoop *loop() const;

    // ── 收件箱（单元邮箱；报文格式与何时注入账本由编排决定）──
    /// 入队一条消息。容量/大小超限时返回 false 并发出 Dropped 事件。
    bool enqueueInboxMessage(const AgentInboxMessage &msg);
    /// 是否存在待投递（Pending）消息；in-flight 不计入。
    bool hasPendingInboxMessages() const;
    /**
     * 取出待投递消息并标记为 in-flight（不 ack）。
     * 按优先级降序、同优先级按时间升序返回。
     * 编排在成功注入后调用 ackInboxMessages()，失败时调用 requeueInboxMessages()。
     */
    QList<AgentInboxMessage> takePendingInboxMessages();
    /// 确认送达：标记 acked 并从列表移除，逐个发出 Delivered 事件。
    void ackInboxMessages(const QStringList &ids);
    /// 投递失败回滚：in-flight 回到 pending，可再次 take。
    void requeueInboxMessages(const QStringList &ids);
    /// 清空邮箱（会话清理/析构）：剩余 pending/in-flight 逐个发出 Dropped 事件。
    void clearInbox(const QString &reason);

    // ── 消息直接访问（替代旧 ConversationListModel::messages()）──
    QList<ConversationMessage> ledgerMessages() const;

    // ── 内环事件 fan-out（Core 私有；非跨层契约）──
    core_ir::HandlerId addEventHandler(core_ir::EventHandler handler);
    void removeEventHandler(core_ir::HandlerId id);

    /// 管理器 / dataChanged 等非 Loop 路径的协议状态出口（含完整 status）。
    void emitAgentStateProtocolEvent();
    void emitInboxEnqueued(const AgentInboxMessage &msg);
    void emitInboxDelivered(const AgentInboxMessage &msg);
    void emitInboxDropped(const AgentInboxMessage &msg, const QString &reason);

signals:
    void stateChanged();
    void dataChanged();

private:
    using ToolCompletion = BuiltinToolRuntime::Completion;

    bool submitMessageInternal(const QString &message, ConversationMessage::Kind kind, const QString &logLabel);
    void handleLoopStateChanged();
    void handleLoopDataChanged();
    [[nodiscard]] bool remainsIdleAfterTurn() const;
    static QString deriveLatestSummary(const QList<ConversationMessage> &messages);

    QString m_agentId;
    QString m_displayName;
    QString m_parentAgentId;

    class ProviderCredential *m_credentialStore = nullptr;

    SessionRuntime m_runtime;

    ToolCoordinator *m_coordinator = nullptr;
    ProviderFactory m_providerFactory;
    class SystemPromptBuilder *m_promptBuilder = nullptr;

    std::unique_ptr<AbstractLoop> m_loop;
    std::unique_ptr<CompactPipeline> m_compact;
    // 派生与回退状态
    AgentStatus m_status = AgentStatus::Idle;
    std::unique_ptr<AgentTaskManager> m_taskManager;

    // 收件箱
    QList<AgentInboxMessage> m_inbox;

    core_ir::EventHandlerRegistry m_protocolHandlers;
};
