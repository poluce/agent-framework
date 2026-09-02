#pragma once

#include <optional>
#include "AgentMode.h"
#include "SessionRuntime.h"
#include "config/SystemPromptBuilder.h"
#include "models/ConversationMessage.h"
#include "agent/ProviderRunLedger.h"
#include "tools/BuiltinToolRuntime.h"
#include "providers/ProviderTypes/ProviderTypes.h"

#include "ir/CoreEventChannel.h"
#include "ir/CoreEvent.h"
#include "AgentModePolicy.h"

#include <QList>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>

class AbstractProvider;
class ToolCoordinator;
struct ProviderError;
struct ProviderMessageEnd;
struct ProviderMessageStart;

class AbstractLoop : public QObject
{
    Q_OBJECT

public:
    // ── 阶段枚举（身份在 core_ir::AgentPhase；展示文案不在 Loop）──
    using Phase = core_ir::AgentPhase;

    enum class LoopMode { Idle, Busy };

    using ProviderFactory = std::function<std::unique_ptr<AbstractProvider>(const QString &providerType)>;
    using ModePolicyFactory = std::function<std::unique_ptr<AgentModePolicy>(
        AgentMode mode,
        const AgentPromptContext &ctx,
        SystemPromptBuilder *promptBuilder)>;

    /**
     * 用户消息投递意图（与 HostUserTurnDelivery 对齐）。
     * Steer：工具间隙注入当前轮；NextTurn：成功收口后自动开新轮。
     */
    enum class UserDelivery {
        Steer,
        NextTurn,
    };

    struct PendingMessage {
        QString text;
        ConversationMessage::Kind kind = ConversationMessage::Kind::UserText;
        QStringList attachedFilePaths;
        // 技能负载：非空时在用户条目之后追加 SkillInvoke（同一 Turn 发给模型，不上墙）
        QString skillName;
        QString skillBody;
        // 显示摘要：非空时写入条目 summaryText（UI 优先展示），text 仍按原文发给模型
        QString displaySummary;
    };

    explicit AbstractLoop(QObject *parent = nullptr);
    ~AbstractLoop() override;

    // ── 生命周期 ──
    void start(const SessionRuntime &config);
    /// 失败收口后：复用账本里上一轮用户消息再开一轮（不重记用户话、不依赖 next 队列）。
    /// @return 已受理并进入 Busy
    [[nodiscard]] bool retryLastFailedTurn(const SessionRuntime &config);
    /// Idle 且上一轮 Failed，账本里找得到可重放的用户消息。
    [[nodiscard]] bool canRetryLastFailedTurn() const;
    void cancel();
    void clear();

    // ── 输入 ──
    /// 入队一条消息；trimmed 为空时返回 false。
    bool enqueueMessage(const QString &message, ConversationMessage::Kind kind,
                        const QString &displaySummary = {},
                        UserDelivery delivery = UserDelivery::NextTurn);
    bool enqueueUserMessage(const QString &message,
                            UserDelivery delivery = UserDelivery::NextTurn)
    {
        return enqueueMessage(message, ConversationMessage::Kind::UserText, {}, delivery);
    }
    bool enqueueUserMessageWithFiles(const QString &message, const QStringList &filePaths,
                                     UserDelivery delivery = UserDelivery::NextTurn);
    bool enqueueUserMessageWithSkill(const QString &message,
                                     const QStringList &filePaths,
                                     const QString &skillName,
                                     const QString &skillBody,
                                     UserDelivery delivery = UserDelivery::NextTurn);
    bool enqueueAgentTask(const QString &message, const QString &displaySummary = {})
    {
        return enqueueMessage(message, ConversationMessage::Kind::AgentTask, displaySummary,
                              UserDelivery::NextTurn);
    }
    void appendExternalMessage(const ConversationMessage &message);

    /// 确认发送 next_turn 待发送队列（Idle 时开新轮）；无条目返回 false
    [[nodiscard]] bool confirmPendingNextTurns(const SessionRuntime &config);
    /// 丢弃 next_turn 待发送队列
    void discardPendingNextTurns();

    [[nodiscard]] int pendingNextTurnCount() const;
    [[nodiscard]] QStringList pendingNextTurnPreviews(int maxItems = 10) const;
    /// Running 且有未完成工具调用时适合默认 steer
    [[nodiscard]] bool prefersSteerDelivery() const;

    // ── 审批 ──
    bool approvePendingToolCall(bool approved);
    bool hasPendingApproval() const;

    bool submitQuestionAnswer(int questionIndex, const QString &answer);
    bool hasPendingQuestion() const;
    int pendingQuestionCount() const;
    QString pendingQuestionIdAt(int index) const;
    QString pendingQuestionTextAt(int index) const;
    QStringList pendingQuestionOptionsAt(int index) const;
    bool pendingQuestionIsMultiSelectAt(int index) const;
    [[nodiscard]] QList<core_ir::PendingQuestion> pendingQuestionSnapshot() const;
    PendingApprovalRequest pendingApprovalRequest() const;

    // ── 状态查询 ──
    bool isStreaming() const;
    bool isBusy() const { return m_mode == LoopMode::Busy; }
    bool isCancelled() const { return m_cancelRequested; }
    [[nodiscard]] Phase phase() const { return m_phase; }
    QString lastError() const;

    QList<ConversationMessage> messages() const;
    /// 兼容：返回 next_turn 队列视图（含历史调用方）
    const QList<PendingMessage> &pendingMessages() const;
    qint64 currentContextTokenEstimate() const;

    // ── 配置 ──
    void setCoordinator(ToolCoordinator *coordinator);
    void setToolResultStoreDirectory(const QString &dir);
    void setProviderFactory(ProviderFactory factory);
    void setPromptBuilder(class SystemPromptBuilder *builder);
    void setModePolicyFactory(ModePolicyFactory factory);
    void setCredentialStore(class ProviderCredential *credentialStore);
    void setAgentInfo(const QString &agentId,
                      const QString &displayName = {},
                      const QString &parentAgentId = {});
    void applyRuntimeConfig(const SessionRuntime &config);

    /**
     * @brief 拒绝本会话一切工具调用（btw 纯问答旁路用）
     *
     * 与提示词强禁令配套的执行期兜底（参考 sideQuestion 的 canUseTool: deny）：
     * true 时模型若仍输出工具调用，一律回填「拒绝」结果并继续（模型据此转文字回答），
     * 不执行、不进审批。false = 正常按 toolScope/approvalMode 判权。
     */
    void setToolCallsDenied(bool denied);

    /** 同会话其他 Agent 写入了工作区文件 → 本 Agent 读缓存失效（写前需重读）。 */
    void notifyFileWrittenByOther(const QString &absPath);

    QString sessionUuid() const { return m_sessionUuid; }
    void setSessionUuid(const QString &uuid);

    /// Agent 获取账本引用
    ProviderRunLedger &ledger() { return m_ledger; }
    const ProviderRunLedger &ledger() const { return m_ledger; }

    // 检查是否有未完成的工具调用（有 ToolCall 但无对应 ToolResult）
    // 此时不应启动新的 Turn，否则 DeepSeek API 会返回 400
    bool hasUnresolvedToolCalls() const { return m_ledger.hasUnresolvedToolCalls(); }

    /// Agent 获取 Provider 引用
    AbstractProvider *provider() const { return m_provider.get(); }

    /// 自动压缩完成后恢复循环（继续当前轮次）
    void continueAfterCompaction();

    /// 手动 CompactSession：进入压缩 phase（Busy），不打开 turn
    void beginManualCompaction();
    /// 手动压缩收口：回到 Idle，不 continueAfterCompaction
    void endManualCompaction();

    /// 边界等段摘要：保持 Compacting/Busy，不跑大压；Cancel 时由 Agent 结束等待
    void beginBoundarySummaryWait();
    void endBoundarySummaryWait(bool cancelled);
    [[nodiscard]] bool isWaitingBoundarySummary() const { return m_waitingBoundarySummary; }

    /// 账本被外部改写（段摘要组装 markCompacted）后刷新占用缓存
    void refreshContextTokenEstimate() { updateContextTokenEstimate(); }

    /// 模型短上下文前缀（摘要库正文链）；仅影响 buildRequest / token 估算
    void setModelViewPrefixTexts(QList<QString> texts);
    void clearModelViewPrefix();
    [[nodiscard]] QList<QString> modelViewPrefixTexts() const { return m_modelViewPrefixTexts; }

    /// 获取组装后的完整系统提示词
    QString assembledSystemPrompt() const { return m_systemPrompt; }

    // ── 内环事件 fan-out（Core 私有；非跨层契约）──
    core_ir::HandlerId addEventHandler(core_ir::EventHandler handler);
    void removeEventHandler(core_ir::HandlerId id);

signals:
    void stateChanged();
    void dataChanged();
    void compactionRequested(qint64 currentTokens, qint64 threshold);
    /// 主轮成功收口（Completed）；编排 usesSegmentSummary 时用于段摘要入队检查
    void turnSucceeded();

private:
    AgentLogContext logContext() const;
    QString sessionShortId() const;
    void refreshRuntimeLogContexts();
    SessionRuntime nextQueuedConfig() const;
    void activateConfig(const SessionRuntime &config);

    // ── Provider 连接 ──
    bool ensureProvider();
    void applyProviderSettings();
    AgentPromptContext buildPromptContext(const SessionRuntime &config) const;

    // ── 模式管理 ──
    void switchMode(LoopMode target);
    void resetLoopState();
    void refreshModePolicyIfNeeded(const SessionRuntime &config);
    [[nodiscard]] QString assembleSystemPrompt(const SessionRuntime &config);

    // ── Turn 生命周期 ──
    bool canStartTurn();
    bool canContinueTurn();
    void startProviderTurn();
    void startProviderTurnImpl(qint64 contextTokenEstimate = -1);
    void continueAfterToolResult();
    void processReadyToolCalls();
    void executeToolCall(const ToolCall &toolCall);
    void failTurn(const QString &errorMessage);
    /// 从账本末尾找回「最近失败轮」的用户消息（跳过 Error；半截助手已 rollback）。
    [[nodiscard]] QString lastFailedUserEntryId() const;
    /// 去掉最近失败轮留下的 Error 条目（不进模型，重试前清掉以免 UI 叠两条）。
    void removeTrailingErrorEntries(const QString &turnId);

    // ── 账本操作 ──
    QString appendLedgerEntry(ConversationMessage entry);
    void appendHiddenSystemMessage(const QString &text);
    QString appendApprovalEntry(const ToolCall &toolCall);
    ConversationMessage *ensureStreamingEntry(ConversationMessage::Kind kind, QString &entryId);
    ConversationMessage *ensureStreamingAssistantEntry();
    ConversationMessage *ensureStreamingReasoningEntry();
    void finalizeStreamingReasoning();
    void finalizeStreamingAssistant();
    void recordCurrentResponseEntryId(const QString &entryId);
    void applyPendingUsage();
    void discardIncompleteEntriesForTurn(const QString &turnId);
    /// 取消/失败收口：先 finalize 流式，再 rollback 指定 turn 的未提交条目。
    void finalizeAndDiscardTurn(const QString &turnId);
    void discardCurrentAssistantTextForPolicyRetry();
    void updateToolEntry(const QString &toolUseId,
                         ConversationMessage::Status status,
                         const QString &progressText = {},
                         const QString &summaryText = {});
    QString appendToolResultEntry(const ToolResult &result);

    // ── 工具 ──
    QList<ProviderToolSpecification> toolSpecs() const;

    // ── 阶段管理 ──
    void setPhase(Phase phase);
    /// 按投递意图入队（Steer→引导队列；NextTurn→下一轮队列）；触发队列变更通知
    void enqueuePending(PendingMessage item, UserDelivery delivery);
    /**
     * @brief 清掉 Idle 下粘滞的终态 phase（Completed/Failed/Canceled）
     * @note 上一轮 `setPhase(Completed)` 后 mode=Idle 但 phase 仍是 Completed。
     *       新消息 `enqueue*` 会 `notifyDataChanged` → Session 补发 AgentState；
     *       若不先清 phase，Host 会看到 sticky idle/completed（已用 observedBusy 挡，
     *       此处从 Core 源头不再重播脏终态）。不经 setPhase，避免再推一条终态事件。
     */
    void clearStickyTerminalPhase();
    void notifyStateChanged();
    void notifyDataChanged();
    void emitProtocolEvent(core_ir::Event event,
                           const core_ir::SubmissionId &submissionId = {});
    // ── 看门狗 ──
    void armModelResponseWatchdog();
    void stopModelResponseWatchdog();

    // ── 内部状态钩子 ──
    void onPhaseChanged(Phase newPhase);
    void onWatchdogTimeout();

    // ── Provider 事件处理 ──
    void onProviderEvent(const ProviderEvent &event);
    void handleMessageStarted(const ProviderEvent &event);
    void handleTextDelta(const ProviderEvent &event);
    void handleReasoningDelta(const ProviderEvent &event);
    void handleToolCallStarted(const ProviderEvent &event);
    void handleToolCallCompleted(const ProviderEvent &event);
    void handleUsageUpdated(const ProviderEvent &event);
    void handleResponseMetadata(const ProviderEvent &event);
    void handleMessageCompleted(const ProviderEvent &event);
    void handleImageOutput(const ProviderEvent &event);
    void handleError(const ProviderEvent &event);
    void handleCancelled(const ProviderEvent &event);

    // ── 工具完成 ──
    void onToolCompleted(const ToolResult &result);
    /// 统一收口：take 活动表 → 账本 → EventToolCallEnd；advanceLoop=false 时只结算本条
    bool completeToolResult(const ToolResult &result, bool advanceLoop);
    bool runModePolicyCheckpoint();
    void rebuildProviderToolSpecsCache();
    /** 设置系统提示并失效固定开销缓存；禁止直接写 m_systemPrompt。 */
    void setSystemPrompt(const QString &prompt);
    void updateContextTokenEstimate();
    QString nextTurnId();
    /// 日志 agentType：有会话时按 isPrimary（main/sub），否则回落 parent 是否为空。
    void refreshAgentType();
private:

    // ── 成员变量 ──
    Phase m_phase = Phase::Idle;
    LoopMode m_mode = LoopMode::Idle;
    QList<Phase> m_phaseTrace;
    QString m_lastError;

    std::optional<SessionRuntime> m_activeConfig;
    std::optional<SessionRuntime> m_pendingConfig;
    ProviderFactory m_providerFactory;
    ModePolicyFactory m_modePolicyFactory;
    class SystemPromptBuilder *m_promptBuilder = nullptr;
    QString m_activeProviderType;

    QTimer m_modelResponseWatchdogTimer;

    QString m_sessionUuid;
    QString m_agentId;
    QString m_agentType;
    QString m_displayName;
    QString m_parentAgentId;
    class ProviderCredential *m_credentialStore = nullptr;
    std::unique_ptr<AbstractProvider> m_provider;

    ProviderRunLedger m_ledger;
    qint64 m_currentContextTokenEstimate = 0;
    /** 请求固定开销（系统提示 + 工具 schema + 模型视图前缀）的 token 估算缓存。 */
    qint64 m_cachedRequestOverheadTokens = 0;
    bool m_requestOverheadDirty = true;
    /// 摘要库投影的模型视图前缀（不进账本）
    QList<QString> m_modelViewPrefixTexts;

    QList<PendingMessage> m_steerQueue;     ///< 引导：工具间隙注入当前轮
    QList<PendingMessage> m_nextTurnQueue;  ///< 下一轮：成功收口后自动开；Cancel/Failed 保留
    ToolCoordinator *m_coordinator = nullptr;
    QList<ProviderToolSpecification> m_cachedProviderToolSpecs;
    BuiltinToolRuntime m_builtinRuntime;

    /// 边界等段摘要（leader）；Cancel 结束等待但不 clear 队列
    bool m_waitingBoundarySummary = false;

    QList<ToolCall> m_pendingToolCalls;
    PendingApprovalRequest m_pendingApproval;
    ToolCall m_pendingApprovalCall;
    QString m_pendingApprovalEntryId;

    struct PendingQuestionRequest {
        QString questionId;
        QString question;
        QStringList options;
        bool isMultiSelect = false;
        bool answered = false;
        QString answer;

        [[nodiscard]] bool isValid() const {
            return !questionId.isEmpty() && !question.isEmpty();
        }
    };
    QList<PendingQuestionRequest> m_pendingQuestions;
    QList<ToolCall> m_pendingQuestionCalls;
    QString m_pendingUserEntryId; // 已入账待发送的用户消息 ID（支持失败后重试不重复创建）
    /// failTurn 记下的失败轮 id；reset 后仍可据此找回账本里的用户消息
    QString m_lastFailedTurnId;
    QString m_streamingAssistantEntryId;
    QString m_streamingReasoningEntryId;
    QStringList m_currentResponseEntryIds;
    ProviderUsage m_pendingUsage;
    bool m_hasPendingUsage = false;
    QString m_currentResponseId;
    QString m_providerContinuationId;

    int m_internalStepCount = 0;
    bool m_cancelRequested = false;
    bool m_providerRequestActive = false;
    QString m_systemPrompt;
    /// 拒绝一切工具调用（btw 纯问答；false=正常判权）
    bool m_toolCallsDenied = false;
    QString m_activeTurnId;
    int m_turnSequence = 0;

    std::unique_ptr<AgentModePolicy> m_modePolicy;
    AgentMode m_currentPolicyMode = AgentMode::Normal;
    QString m_currentPolicyWorkspace;
    QHash<QString, ToolCall> m_activeToolCallsById;

    // 内环 Event handlers（Event+Context+SubmissionId）
    std::map<core_ir::HandlerId, core_ir::EventHandler> m_protocolHandlers;
    core_ir::HandlerId m_nextHandlerId = reinterpret_cast<core_ir::HandlerId>(1);
};
