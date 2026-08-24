#pragma once

#include "AbstractLoop.h"
#include "AgentMode.h"
#include "AgentTaskManager.h"
#include "agent/compact/CompactEngine.h"
#include "agent/compact/ModelViewStore.h"
#include "agent/compact/SummaryJobQueue.h"
#include "agent/compact/SummaryStore.h"
#include "SessionRuntime.h"
#include <QDateTime>
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
    QString replyTo;
    QDateTime timestamp;
    bool acked = false;
};

class AbstractOrchestration;
class ToolCoordinator;

class Agent : public QObject
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
    QString agentId() const;
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

    // ── 操作 ──
    void submitUserMessageWithSkill(const QString &message,
                                    const QStringList &filePaths,
                                    const QString &skillName,
                                    const QString &skillBody);
    /**
     * 忙时/空闲统一入口：按 delivery 入队；Idle 且 NextTurn 时 start 开轮。
     * Busy 时仅入队不 start。
     */
    void submitUserDelivery(const QString &message,
                            const QStringList &attachedFilePaths,
                            AbstractLoop::UserDelivery delivery);
    /// 确认 next_turn 待发送（须 Idle）；无条目返回 false
    [[nodiscard]] bool confirmPendingNextTurns();
    void discardPendingNextTurns();
    [[nodiscard]] int pendingNextTurnCount() const;
    [[nodiscard]] QStringList pendingNextTurnPreviews(int maxItems = 10) const;
    [[nodiscard]] bool prefersSteerDelivery() const;
    void submitAgentTask(const QString &message);
    void cancelCurrentTurn();
    /// 失败收口后同会话再打一轮；无待重放用户消息则 false。
    [[nodiscard]] bool retryFailedMessage();
    [[nodiscard]] bool canRetryFailedMessage() const;
    void approvePendingAction();
    void rejectPendingAction();
    void submitQuestionAnswer(int questionIndex, const QString &answer);
    void appendSessionEvent(const QString &text);

    /**
     * 手动压缩（Host CompactSession）：绕过 token 门控，共用 CompactEngine。
     * 压完停 Idle，不自动续轮。G5：先 abort 段摘要并 clear 队列再大压。
     * 仍拒：主 Loop Busy、已在手动压、大压引擎真在跑。
     */
    [[nodiscard]] bool requestManualCompaction(qint64 targetTokens = -1);

    /// ClearConversation / 析构：清摘要队列与库
    void clearSummaryState();

    // ── 段摘要可观测（测试/探针；子代理无队列）──
    [[nodiscard]] bool hasSegmentSummaryQueue() const { return m_summaryQueue != nullptr; }
    [[nodiscard]] int segmentSummaryJobCount() const;
    [[nodiscard]] bool isWaitingSegmentSummaryAtBoundary() const
    {
        return m_waitingSummaryAtBoundary;
    }
    [[nodiscard]] bool segmentSummaryStoreEmpty() const { return m_summaryStore.isEmpty(); }
    [[nodiscard]] int segmentSummaryRecordCount() const { return m_summaryStore.recordCount(); }
    [[nodiscard]] const SummaryStore &summaryStore() const { return m_summaryStore; }
    [[nodiscard]] const ModelViewStore &modelViewStore() const { return m_modelViewStore; }
    [[nodiscard]] QJsonObject exportSummaryState() const;
    void importSummaryState(const QJsonObject &obj);
    /// 与主轮成功收口相同的入队/resume 检查（测试可直调）
    void probeSegmentSummaryAfterTurnSuccess() { onTurnSucceededForSummary(); }
    [[nodiscard]] bool hasFailedSegmentSummaryJobs() const;

    // ── 运行时 ──
    class AbstractLoop *loop() const;

    // ── 收件箱（单元邮箱；报文格式与何时注入账本由编排决定）──
    void enqueueInboxMessage(const AgentInboxMessage &msg);
    bool hasPendingInboxMessages() const;
    /// 取出未确认消息并标为已确认。编排负责编码并注入账本。
    QList<AgentInboxMessage> takePendingInboxMessages();

    // ── 消息直接访问（替代旧 ConversationListModel::messages()）──
    QList<ConversationMessage> ledgerMessages() const;

    // ── 内环事件 fan-out（Core 私有；非跨层契约）──
    core_ir::HandlerId addEventHandler(core_ir::EventHandler handler);
    void removeEventHandler(core_ir::HandlerId id);

    /// 管理器 / dataChanged 等非 Loop 路径的协议状态出口（含完整 status）。
    void emitAgentStateProtocolEvent();

signals:
    void stateChanged();
    void dataChanged();

private:
    using ToolCompletion = BuiltinToolRuntime::Completion;

    void submitMessageInternal(const QString &message, ConversationMessage::Kind kind, const QString &logLabel);
    void handleLoopStateChanged();
    void handleLoopDataChanged();
    void onCompactionRequested(qint64 currentTokens, qint64 threshold);
    void onCompactionFinished(bool success);
    void onCompactionFailed(const QString &reason);
    void startCompactionEngine(qint64 targetTokensOverride = -1);
    void onBoundaryCompactionRequested(qint64 threshold);
    void onTurnSucceededForSummary();
    void onSummaryJobFinished(const QString &jobId, bool success, const QString &summaryText,
                              const QList<QString> &spanEntryIds);
    void maybeEnqueueSegmentSummary();
    void applyAssembledModelView();
    void syncModelViewPrefixFromStore();
    void emitContextCompactedNotice(core_ir::CompactReason reason);
    void resumeBoundaryAfterSummaryDrain();
    void clearSummaryQueueForBulk();
    void configureAndKickSummaryQueue();
    void ensureSegmentSummaryPipeline();
    [[nodiscard]] AbstractOrchestration *orchestration() const;
    [[nodiscard]] bool remainsIdleAfterTurn() const;
    [[nodiscard]] bool summaryFeaturesEnabled() const;
    [[nodiscard]] QString segmentSummaryCursor() const;
    /// 组装模型视图；若占用 ≤ threshold 则 continueAfterCompaction 并返回 true
    [[nodiscard]] bool tryContinueWithAssembledView(qint64 threshold, bool logWhenOver);
    static SummaryRecord makeSummaryRecord(const QString &summaryId,
                                           QList<QString> spanEntryIds,
                                           const QString &text,
                                           const QString &source);
    static QString deriveLatestSummary(const QList<ConversationMessage> &messages);

    QString m_agentId;
    QString m_displayName;
    QString m_parentAgentId;
    /// CompactSession 触发：结束后不 continueAfterCompaction
    bool m_manualCompaction = false;
    /// 边界上等待摘要队列排空
    bool m_waitingSummaryAtBoundary = false;
    qint64 m_boundaryThreshold = 0;

    // Provider 配置（apiKey/baseUrl 由实例管理，Loop 按需解析）
    class ProviderCredential *m_credentialStore = nullptr;

    SessionRuntime m_runtime;

    ToolCoordinator *m_coordinator = nullptr;
    ProviderFactory m_providerFactory;
    class SystemPromptBuilder *m_promptBuilder = nullptr;

    // 运行时
    std::unique_ptr<AbstractLoop> m_loop;
    std::unique_ptr<CompactEngine> m_compactEngine;
    /// 编排 usesSegmentSummary 时安装；无配方则空
    std::unique_ptr<SummaryJobQueue> m_summaryQueue;
    SummaryStore m_summaryStore;
    ModelViewStore m_modelViewStore;
    /// 已成功写库的末 entry id
    QString m_lastSummarizedEntryId;
    /// 已入队尚未写库的末 entry id（防重叠入队）
    QString m_lastEnqueuedEntryId;
    // 派生与回退状态
    AgentStatus m_status = AgentStatus::Idle;
    std::unique_ptr<AgentTaskManager> m_taskManager;

    // 收件箱
    QList<AgentInboxMessage> m_inbox;

    // 内环 Event handlers（Event+Context+SubmissionId）
    std::vector<core_ir::EventHandler> m_protocolHandlers;
};
