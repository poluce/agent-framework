#include "AbstractLoop.h"
#include "tools/ToolCoordinator.h"
#include "agent/AbstractOrchestration.h"
#include "agent/Agent.h"
#include "agent/AgentSession.h" // writeCoordinator() 需要完整类型
#include "tools/builtin/AskQuestionTool.h"
#include "tools/builtin/helpers/WorkspaceHelper.h" // normalizedPath（跨 Agent 广播 key 归一）
#include "providers/service/ProviderService.h"
#include "providers/service/ModelContextMetaStore.h"
#include "config/SystemPromptBuilder.h"
#include "logging/LogManager.h"
#include "AgentModePolicy.h"
#include "providers/core/AbstractProvider.h"
#include "providers/core/ProviderRetryPolicy.h"
#include "providers/service/ProviderCredential.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QTimer>
#include <QUuid>

namespace {
QString ptrTag(const void *ptr)
{
    return QStringLiteral("0x%1").arg(quintptr(ptr), 0, 16);
}

ConversationMessage::Status statusForToolResult(const ToolResult &result)
{
    switch (result.category) {
    case ToolResultCategory::Canceled:
        return ConversationMessage::Status::Canceled;
    case ToolResultCategory::Rejected:
        return ConversationMessage::Status::Rejected;
    case ToolResultCategory::Error:
        return ConversationMessage::Status::Failed;
    case ToolResultCategory::Success:
        return result.success ? ConversationMessage::Status::Completed
                              : ConversationMessage::Status::Failed;
    }
    return ConversationMessage::Status::Completed;
}

ConversationMessage::ResultCategory resultCategoryForToolResult(const ToolResult &result)
{
    switch (result.category) {
    case ToolResultCategory::Success:
        return ConversationMessage::ResultCategory::Success;
    case ToolResultCategory::Error:
        return ConversationMessage::ResultCategory::Error;
    case ToolResultCategory::Rejected:
        return ConversationMessage::ResultCategory::Rejected;
    case ToolResultCategory::Canceled:
        return ConversationMessage::ResultCategory::Canceled;
    }
    return ConversationMessage::ResultCategory::None;
}

QString decisionTextFor(ToolPermissionDecision decision)
{
    switch (decision) {
    case ToolPermissionDecision::Allow:
        return QStringLiteral("allow");
    case ToolPermissionDecision::Deny:
        return QStringLiteral("deny");
    case ToolPermissionDecision::NeedsApproval:
        return QStringLiteral("needs_approval");
    }
    return QStringLiteral("unknown");
}

QString decisionLabelFor(ToolPermissionDecision decision)
{
    switch (decision) {
    case ToolPermissionDecision::Allow:
        return QStringLiteral("允许");
    case ToolPermissionDecision::Deny:
        return QStringLiteral("拒绝");
    case ToolPermissionDecision::NeedsApproval:
        return QStringLiteral("需审批");
    }
    return QStringLiteral("未知");
}

} // namespace

using Phase = AbstractLoop::Phase;

AbstractLoop::AbstractLoop(QObject *parent)
    : QObject(parent)
    , m_sessionUuid(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_builtinRuntime(this)
{
    m_modelResponseWatchdogTimer.setSingleShot(true);
    connect(&m_modelResponseWatchdogTimer, &QTimer::timeout, this, &AbstractLoop::onWatchdogTimeout);

    connect(&m_builtinRuntime, &BuiltinToolRuntime::toolProgress, this,
            [this](const QString &toolUseId, const QString &progressKind, const QString &message) {
                updateToolEntry(toolUseId, ConversationMessage::Status::Running,
                                message.isEmpty() ? progressKind : message);
                notifyDataChanged();

                // stdout/stderr = 命令输出增量；其余 kind 是状态进度（展示文案在 Host 投影）
                const bool isOutputDelta = progressKind == QLatin1String("stdout")
                    || progressKind == QLatin1String("stderr");
                if (isOutputDelta) {
                    emitProtocolEvent(core_ir::EventCommandOutputDelta{
                        toolUseId, message, progressKind == QLatin1String("stderr")
                    });
                } else {
                    emitProtocolEvent(core_ir::EventToolProgress{
                        toolUseId, progressKind, message
                    });
                }
            });

    rebuildProviderToolSpecsCache();
}

AbstractLoop::~AbstractLoop() = default;

// ── 状态查询 ──

QString AbstractLoop::lastError() const
{
    return m_lastError;
}

// ── 阶段管理 ──

void AbstractLoop::setPhase(Phase phase)
{
    if (m_phase == phase && !m_phaseTrace.isEmpty()) {
        return;
    }

    m_phase = phase;
    m_phaseTrace.append(phase);

    // C12：终态事件必须在清 m_activeTurnId **之前**发出。
    // 旧实现 onPhaseChanged 先清空 id，随后 if (!m_activeTurnId.isEmpty()) 恒假，
    // EventTurnComplete 成为不可达死代码。先发事件再清 id。
    const QString turnIdForComplete = m_activeTurnId;
    if (phase == Phase::Completed || phase == Phase::Failed || phase == Phase::Canceled) {
        if (!turnIdForComplete.isEmpty()) {
            emitProtocolEvent(core_ir::EventTurnComplete{
                turnIdForComplete, 0, 0  // durationMs/timeToFirstToken 后续可精细计算
            });
        }
    }

    onPhaseChanged(phase);
    notifyStateChanged();
}

void AbstractLoop::clearStickyTerminalPhase()
{
    // 只在空闲时清；Busy 中的 phase 是活状态。静默回 Idle，不走 setPhase
    // （避免 EventTurnComplete / 又一条 AgentState 终态边沿）。
    if (m_mode != LoopMode::Idle)
        return;
    if (m_phase != Phase::Completed && m_phase != Phase::Failed
        && m_phase != Phase::Canceled) {
        return;
    }
    m_phase = Phase::Idle;
    m_phaseTrace.append(Phase::Idle);
}

void AbstractLoop::notifyStateChanged()
{
    // 对外状态以 ProtocolEvent 为准；Qt 信号仅供 Core 进程内协调（排队/重命名等）。
    const bool hasQuestion = hasPendingQuestion();
    if (hasQuestion) {
        LOGI(LogCat::Agent, logContext()) << "提问广播"
            << logf("count", m_pendingQuestions.size());
    }
    // status 由 Agent 在转发时填入（派生自 Loop phase + manager m_status）；Loop 本身不持有 AgentStatus。
    // canSubmit 在 Host 侧由 runState 投影覆盖；此处 busy 语义仅作 Core 内环提示。
    emitProtocolEvent(core_ir::EventAgentStateChanged{
        m_agentId,
        isBusy(),
        m_phase,
        !hasPendingApproval() && !hasQuestion,
        hasPendingApproval(),
        hasPendingApproval() ? m_pendingApproval.summary : QString(),
        m_lastError,
        m_currentContextTokenEstimate,
        {},
        hasQuestion,
        hasQuestion ? static_cast<int>(m_pendingQuestions.size()) : 0,
        hasQuestion ? pendingQuestionSnapshot() : QList<core_ir::PendingQuestion>{},
        pendingNextTurnCount(),
        pendingNextTurnPreviews()
    });
    emit stateChanged();
}

void AbstractLoop::notifyDataChanged()
{
    // 细粒度账本变更已有对应 ProtocolEvent；此信号仅驱动进程内粗粒度刷新。
    emit dataChanged();
}

void AbstractLoop::emitProtocolEvent(core_ir::Event event,
                                      const core_ir::SubmissionId &submissionId)
{
    if (m_protocolHandlers.empty()) {
        return;
    }
    const core_ir::EventContext context{};
    for (auto &pair : m_protocolHandlers) {
        pair.second(event, context, submissionId);
    }
}

core_ir::HandlerId AbstractLoop::addEventHandler(core_ir::EventHandler handler)
{
    const core_ir::HandlerId id = m_nextHandlerId;
    m_nextHandlerId = reinterpret_cast<core_ir::HandlerId>(reinterpret_cast<std::uintptr_t>(m_nextHandlerId) + 1);
    m_protocolHandlers[id] = std::move(handler);
    return id;
}

void AbstractLoop::removeEventHandler(core_ir::HandlerId id)
{
    m_protocolHandlers.erase(id);
}

// ── 配置 ──



void AbstractLoop::setToolResultStoreDirectory(const QString &directoryPath)
{
    m_builtinRuntime.setResultStoreDirectory(directoryPath);
}

void AbstractLoop::setCoordinator(ToolCoordinator *coordinator)
{
    if (m_coordinator == coordinator) return;

    if (m_coordinator) {
        disconnect(m_coordinator, &ToolCoordinator::toolsUpdated, this, &AbstractLoop::rebuildProviderToolSpecsCache);
    }
    m_coordinator = coordinator;
    if (coordinator) {
        auto *session = static_cast<AgentSession *>(coordinator->session());
        m_builtinRuntime.setSession(session);
        // 会话级写协调器：per-file 互斥跨 Agent 共享（nullptr 安全）
        m_builtinRuntime.setWriteCoordinator(session ? session->writeCoordinator() : nullptr);
        connect(coordinator, &ToolCoordinator::toolsUpdated, this, &AbstractLoop::rebuildProviderToolSpecsCache);
    }
    rebuildProviderToolSpecsCache();
    refreshAgentType();
    refreshRuntimeLogContexts();
}

void AbstractLoop::refreshAgentType()
{
    if (m_coordinator) {
        if (auto *session = static_cast<AgentSession *>(m_coordinator->session())) {
            if (Agent *unit = session->findById(m_agentId)) {
                m_agentType = session->isPrimary(unit)
                    ? QStringLiteral("main")
                    : QStringLiteral("sub");
                return;
            }
        }
    }
    m_agentType = m_parentAgentId.isEmpty() ? QStringLiteral("main") : QStringLiteral("sub");
}

void AbstractLoop::setToolCallsDenied(const bool denied)
{
    m_toolCallsDenied = denied;
}

void AbstractLoop::notifyFileWrittenByOther(const QString &absPath)
{
    m_builtinRuntime.invalidateReadFileState(absPath);
}

void AbstractLoop::setProviderFactory(AbstractLoop::ProviderFactory factory)
{
    m_providerFactory = std::move(factory);
    m_provider.reset();
    m_activeProviderType.clear();
}

void AbstractLoop::setPromptBuilder(SystemPromptBuilder *builder)
{
    m_promptBuilder = builder;
}

void AbstractLoop::setModePolicyFactory(ModePolicyFactory factory)
{
    m_modePolicyFactory = std::move(factory);
    m_modePolicy.reset();
}

void AbstractLoop::setCredentialStore(ProviderCredential *credentialStore)
{
    m_credentialStore = credentialStore;
}

// setAdditionalToolSpecs 已移除，规格由 ToolCoordinator::specsForAgent() 统一管理



void AbstractLoop::setAgentInfo(const QString &agentId,
                                const QString &displayName,
                                const QString &parentAgentId)
{
    m_agentId = agentId;
    m_displayName = displayName;
    m_parentAgentId = parentAgentId;
    refreshAgentType();
    refreshRuntimeLogContexts();
    LOGD(LogCat::Agent, logContext()) << "绑定 Agent"
        << logf("tag", QStringLiteral("agent-bind"))
        << logf("loop", ptrTag(this))
        << logf("agent", m_agentId)
        << logf("parent", m_parentAgentId)
        << logf("display", m_displayName)
        << logf("type", m_agentType);
}

void AbstractLoop::setSessionUuid(const QString &uuid)
{
    m_sessionUuid = uuid;
    refreshRuntimeLogContexts();
}

AgentLogContext AbstractLoop::logContext() const
{
    AgentLogContext ctx;
    ctx.sessionUuid = m_sessionUuid;
    ctx.sessionShortId = sessionShortId();
    ctx.agentId = m_agentId;
    ctx.agentType = m_agentType;
    return ctx;
}

QString AbstractLoop::sessionShortId() const
{
    return m_sessionUuid.left(8);
}

void AbstractLoop::refreshRuntimeLogContexts()
{
    const AgentLogContext ctx = logContext();
    m_builtinRuntime.setLogContext(ctx);
    if (m_provider) {
        m_provider->setLogContext(ctx);
    }
}

SessionRuntime AbstractLoop::nextQueuedConfig() const
{
    return m_pendingConfig.value_or(m_activeConfig.value());
}

void AbstractLoop::activateConfig(const SessionRuntime &config)
{
    m_activeConfig = config;
    m_pendingConfig = std::nullopt;
    m_builtinRuntime.setDefaultShell(config.defaultShell);
}

AgentPromptContext AbstractLoop::buildPromptContext(const SessionRuntime &config) const
{
    AgentPromptContext ctx;
    ctx.agentId = m_agentId;
    ctx.displayName = m_displayName;
    ctx.parentAgentId = m_parentAgentId;
    ctx.workspacePath = config.workingDirectory;
    ctx.defaultShell = config.defaultShell;
    if (m_coordinator) {
        if (auto *session = static_cast<AgentSession *>(m_coordinator->session())) {
            if (AbstractOrchestration *orch = session->orchestration()) {
                ctx.rolePromptFile = orch->rolePromptFile(session->findById(m_agentId));
            }
        }
    }
    return ctx;
}

void AbstractLoop::applyRuntimeConfig(const SessionRuntime &config)
{
    const bool shellChanged = !m_systemPromptDefaultShell.isEmpty()
        && m_systemPromptDefaultShell != config.defaultShell;
    m_builtinRuntime.setDefaultShell(config.defaultShell);
    if (m_activeConfig) {
        m_activeConfig = config;
    }

    refreshModePolicyIfNeeded(config);
    if (!shellChanged) {
        setSystemPrompt(assembleSystemPrompt(config));
        m_systemPromptDefaultShell = config.defaultShell;
    }
    // shellChanged 时：不修改已有系统提示词；切换通知由下一次 Provider 启动时作为隐藏消息注入。
}

QString AbstractLoop::assembleSystemPrompt(const SessionRuntime &config)
{
    const AgentPromptContext ctx = buildPromptContext(config);
    if (m_modePolicy) {
        return m_modePolicy->buildPrompt(ctx);
    }
    if (m_promptBuilder) {
        return m_promptBuilder->buildPrompt(ctx);
    }
    SystemPromptBuilder fallback;
    return fallback.buildPrompt(ctx);
}

void AbstractLoop::refreshModePolicyIfNeeded(const SessionRuntime &config)
{
    const AgentMode nextMode = config.agentMode;
    const QString nextWorkspace = config.workingDirectory;

    if (!m_modePolicyFactory) {
        m_modePolicy.reset();
        m_currentPolicyMode = nextMode;
        m_currentPolicyWorkspace = nextWorkspace;
        return;
    }

    if (!m_modePolicy) {
        m_modePolicy = m_modePolicyFactory(
            nextMode, buildPromptContext(config), m_promptBuilder);
        m_currentPolicyMode = nextMode;
        m_currentPolicyWorkspace = nextWorkspace;
        return;
    }

    const bool modeChanged = nextMode != m_currentPolicyMode;
    const bool workspaceChanged = nextWorkspace != m_currentPolicyWorkspace;

    if (modeChanged) {
        m_modePolicy.reset();
        m_modePolicy = m_modePolicyFactory(
            nextMode, buildPromptContext(config), m_promptBuilder);
        m_currentPolicyMode = nextMode;
        m_currentPolicyWorkspace = nextWorkspace;
        return;
    }

    if (workspaceChanged) {
        if (auto hiddenMessage = m_modePolicy->onWorkspaceChanged(m_currentPolicyWorkspace, nextWorkspace)) {
            appendHiddenSystemMessage(*hiddenMessage);
        }
        m_currentPolicyWorkspace = nextWorkspace;
    }
}


// ── 生命周期 ──

void AbstractLoop::switchMode(const LoopMode target)
{
    if (m_mode == target) {
        return;
    }
    m_mode = target;
}

void AbstractLoop::resetLoopState()
{
    LOGD(LogCat::Agent, logContext()) << "resetLoopState 调用"
        << logf("phase", core_ir::agentPhaseKey(m_phase))
        << logf("pendingTools", m_pendingToolCalls.size())
        << logf("pendingNext", m_nextTurnQueue.size())
        << logf("pendingSteer", m_steerQueue.size())
        << logf("providerActive", m_providerRequestActive);

    // 停止超时检测
    stopModelResponseWatchdog();

    // 清除流式响应状态
    m_pendingUserEntryId.clear();
    m_streamingAssistantEntryId.clear();
    m_streamingReasoningEntryId.clear();
    m_currentResponseEntryIds.clear();
    m_pendingUsage = {};
    m_hasPendingUsage = false;
    m_currentResponseId.clear();
    m_providerContinuationId.clear();

    // 清除挂起的工具和审批
    if (!m_pendingToolCalls.isEmpty()) {
        QStringList abandonedIds;
        for (const auto &tc : m_pendingToolCalls) {
            abandonedIds.append(tc.id);
            // 兜底：把被遗弃的排队中工具标记为失败，避免 UI 永远显示"排队中"
            ConversationMessage *entry = m_ledger.findToolCallByUseId(tc.id);
            if (entry && entry->status == ConversationMessage::Status::Queued) {
                entry->status = ConversationMessage::Status::Failed;
                entry->text.append(QStringLiteral("\n[工具被取消：队列在重置时被清空]"));
            }
        }
        LOGW(LogCat::Tool, logContext()) << "resetLoopState 清空了排队中的工具"
            << logf("count", m_pendingToolCalls.size())
            << logf("ids", abandonedIds.join(QStringLiteral(", ")));
    }
    m_pendingToolCalls.clear();
    m_activeToolCallsById.clear();
    m_pendingApproval = {};
    m_pendingApprovalCall = {};
    m_pendingApprovalEntryId.clear();

    // 清除当前 Turn 和错误
    m_activeTurnId.clear();
    m_lastError.clear();

    // 清除阶段历史和配置
    m_phaseTrace.clear();
    m_activeConfig = std::nullopt;
    m_pendingConfig = std::nullopt;

    // 重置运行计数器
    m_cancelRequested = false;
    m_internalStepCount = 0;

    // 标记为空闲
    m_providerRequestActive = false;
    m_mode = LoopMode::Idle;
}

void AbstractLoop::clear()
{
    LOGD(LogCat::Agent, logContext()) << "清空循环状态";
    cancel();
    m_steerQueue.clear();
    m_nextTurnQueue.clear();
    m_phase = Phase::Idle;
    m_turnSequence = 0;
    m_ledger.clear();
    m_currentContextTokenEstimate = 0;
    m_waitingBoundarySummary = false;
    m_lastFailedTurnId.clear();
    m_builtinRuntime.clearReadFileStates();
    m_modePolicy.reset();
    m_currentPolicyMode = AgentMode::Normal;
    m_currentPolicyWorkspace.clear();
    m_activeToolCallsById.clear();
}

void AbstractLoop::enqueuePending(PendingMessage item, UserDelivery delivery)
{
    // 必须在通知之前：Session 会因 dataChanged 补发 AgentState，
    // 粘滞「本轮完成」会在 Host 已 openTurn、Loop 尚未 Busy 时被重播。
    clearStickyTerminalPhase();
    if (delivery == UserDelivery::Steer) {
        m_steerQueue.append(item);
    } else {
        m_nextTurnQueue.append(item);
    }
    notifyDataChanged();
    // 队列变 → 投影 pending*（Host AgentState 消费）
    notifyStateChanged();
}

bool AbstractLoop::enqueueMessage(const QString &message, ConversationMessage::Kind kind,
                                  const QString &displaySummary,
                                  UserDelivery delivery)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    enqueuePending({trimmed, kind, {}, {}, {}, displaySummary}, delivery);
    return true;
}

bool AbstractLoop::enqueueUserMessageWithFiles(const QString &message, const QStringList &filePaths,
                                               UserDelivery delivery)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty() && filePaths.isEmpty()) return false;
    enqueuePending({trimmed, ConversationMessage::Kind::UserText, filePaths, {}, {}, {}}, delivery);
    return true;
}

bool AbstractLoop::enqueueUserMessageWithSkill(const QString &message,
                                               const QStringList &filePaths,
                                               const QString &skillName,
                                               const QString &skillBody,
                                               UserDelivery delivery)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty() && filePaths.isEmpty()) return false;
    enqueuePending({trimmed, ConversationMessage::Kind::UserText, filePaths,
                    skillName, skillBody.trimmed(), {}}, delivery);
    return true;
}

int AbstractLoop::pendingNextTurnCount() const
{
    return m_nextTurnQueue.size();
}

QStringList AbstractLoop::pendingNextTurnPreviews(const int maxItems) const
{
    QStringList out;
    const int n = qMin(maxItems, m_nextTurnQueue.size());
    for (int i = 0; i < n; ++i)
        out.append(m_nextTurnQueue.at(i).text.trimmed());
    return out;
}

bool AbstractLoop::prefersSteerDelivery() const
{
    return isBusy()
        && (!m_pendingToolCalls.isEmpty()
            || hasUnresolvedToolCalls()
            || m_phase == Phase::ExecutingTool
            || m_phase == Phase::WaitingTool
            || m_phase == Phase::AppendingToolResults
            || m_phase == Phase::WaitingApproval);
}

void AbstractLoop::discardPendingNextTurns()
{
    if (m_nextTurnQueue.isEmpty()) {
        return;
    }
    m_nextTurnQueue.clear();
    notifyDataChanged();
    notifyStateChanged();
}

bool AbstractLoop::confirmPendingNextTurns(const SessionRuntime &config)
{
    if (m_nextTurnQueue.isEmpty()) {
        return false;
    }
    if (m_mode == LoopMode::Busy) {
        LOGW(LogCat::Agent, logContext()) << "confirmPendingNextTurns 被拒：仍 Busy";
        return false;
    }
    start(config);
    return isBusy();
}


bool AbstractLoop::canStartTurn()
{
    if (m_activeConfig->credentialInstanceId.isEmpty()
        || !m_credentialStore
        || m_credentialStore->getInstance(m_activeConfig->credentialInstanceId).isEmpty()
        || !m_providerFactory) {
        LOGW(LogCat::Agent, logContext()) << "start() 失败：Provider 设置不完整"
            << logf("hasFactory", static_cast<bool>(m_providerFactory));
        m_lastError = QStringLiteral("Provider 设置不完整。");
        setPhase(Phase::Failed);
        return false;
    }

    return true;
}

bool AbstractLoop::canContinueTurn()
{
    if (m_cancelRequested) {
        setPhase(Phase::Canceled);
        return false;
    }

    int maxSteps = m_activeConfig ? m_activeConfig->maxInternalSteps : 100;
    if (m_internalStepCount >= maxSteps) {
        LOGW(LogCat::Agent, logContext()) << "主循环超过最大内部步数，已中止"
            << logf("maxSteps", maxSteps);
        failTurn(QStringLiteral("主循环超过最大内部步数，已中止。"));
        return false;
    }
    ++m_internalStepCount;

    if (!ensureProvider()) {
        LOGE(LogCat::Agent, logContext()) << "无法创建 Provider";
        failTurn(QStringLiteral("无法创建 Provider。"));
        return false;
    }

    return true;
}

void AbstractLoop::start(const SessionRuntime &config)
{
    if (m_mode == LoopMode::Busy) {
        m_pendingConfig = config;
        LOGW(LogCat::Agent, logContext()) << "start() 被忽略：Loop 处于繁忙模式，消息已排队等待下次 Turn";
        return;
    }

    // 防御：不经 enqueue 的 start（如 retry）也清粘滞终态，避免首条状态仍带「本轮完成」
    clearStickyTerminalPhase();
    switchMode(LoopMode::Busy);
    activateConfig(config);

    if (m_nextTurnQueue.isEmpty()) {
        LOGW(LogCat::Agent, logContext()) << "start() 失败：没有待处理的消息";
        failTurn(QStringLiteral("没有待处理的消息。"));
        return;
    }

    LOGI(LogCat::Agent, logContext()) << "开始 Turn"
        << logf("sessionUuid", m_sessionUuid);

    m_activeTurnId = nextTurnId();
    const PendingMessage pending = m_nextTurnQueue.takeFirst();

    // 先上墙：首次创建新条目，重试则复用已有条目
    QString entryId;
    if (!m_pendingUserEntryId.isEmpty()) {
        entryId = m_pendingUserEntryId;
        if (ConversationMessage *existing = m_ledger.findById(entryId)) {
            existing->status = ConversationMessage::Status::Queued;
            existing->isError = false;
            existing->resultCategory = ConversationMessage::ResultCategory::None;
            existing->turnId = m_activeTurnId;
            emitProtocolEvent(
                core_ir::EventMessageStatusChanged{entryId,
                                                    ConversationMessage::Status::Queued});
        }
    } else {
        ConversationMessage entry;
        entry.kind = pending.kind;
        entry.status = ConversationMessage::Status::Queued;
        entry.text = pending.text;
        entry.summaryText = pending.displaySummary;
        entry.attachedFilePaths = pending.attachedFilePaths;
        entry.turnId = m_activeTurnId;
        entry.submittedToModel = false;
        entryId = appendLedgerEntry(entry);
        m_pendingUserEntryId = entryId;

        // 技能负载：紧随用户条目之后插入，同一 Turn 作为待发送输入注入。
        // 不上墙（与 SystemPrompt 同类）：text 是给模型的全文，不是聊天消息。
        if (!pending.skillName.isEmpty() && !pending.skillBody.isEmpty()) {
            ConversationMessage skillEntry;
            skillEntry.kind = ConversationMessage::Kind::SkillInvoke;
            skillEntry.status = ConversationMessage::Status::Completed;
            skillEntry.toolName = pending.skillName;
            skillEntry.text = QStringLiteral("<skill-invoke name=\"%1\">\n%2\n</skill-invoke>")
                                  .arg(pending.skillName.toHtmlEscaped(), pending.skillBody);
            skillEntry.summaryText = QStringLiteral("已加载技能：%1").arg(pending.skillName);
            skillEntry.turnId = m_activeTurnId;
            skillEntry.submittedToModel = false;
            appendLedgerEntry(skillEntry);
        }
    }

    if (!canStartTurn()) {
        // 标记为失败态（保留内容，支持重试）
        if (ConversationMessage *appended = m_ledger.findById(entryId)) {
            appended->status = ConversationMessage::Status::Failed;
            appended->isError = true;
            appended->resultCategory = ConversationMessage::ResultCategory::Error;
            emitProtocolEvent(
                core_ir::EventMessageStatusChanged{entryId,
                                                    ConversationMessage::Status::Failed});
        }

        // 第一发送失败时追加错误提示
        if (m_pendingUserEntryId == entryId) {
            ConversationMessage errorEntry;
            errorEntry.kind = ConversationMessage::Kind::Error;
            errorEntry.status = ConversationMessage::Status::Failed;
            errorEntry.text = m_lastError;
            errorEntry.isError = true;
            errorEntry.resultCategory = ConversationMessage::ResultCategory::Error;
            errorEntry.turnId = m_activeTurnId;
            appendLedgerEntry(errorEntry);
        }

        // 放回 next 队列支持重试
        m_nextTurnQueue.prepend(pending);
        // C14：canStartTurn 失败前 setPhase(Failed) 时 mode 仍是 Busy，
        // 最后一条 AgentState 会带着 busy=true。必须先 Idle 再推状态，
        // 否则 Host 轮次不收口 → 后续 Send 永久 InvalidState。
        switchMode(LoopMode::Idle);
        notifyStateChanged();
        notifyDataChanged();
        return;
    }

    // 校验通过，清除追踪 ID
    m_pendingUserEntryId.clear();

    if (ConversationMessage *appended = m_ledger.findById(entryId)) {
        appended->status = ConversationMessage::Status::Completed;
        emitProtocolEvent(
            core_ir::EventMessageStatusChanged{entryId,
                                                ConversationMessage::Status::Completed});
    }

    startProviderTurn();
}

bool AbstractLoop::canRetryLastFailedTurn() const
{
    return m_mode == LoopMode::Idle
        && m_phase == Phase::Failed
        && !lastFailedUserEntryId().isEmpty();
}

QString AbstractLoop::lastFailedUserEntryId() const
{
    const QList<ConversationMessage> &entries = m_ledger.entries();
    for (qsizetype i = entries.size() - 1; i >= 0; --i) {
        const ConversationMessage &entry = entries.at(i);
        if (entry.kind == ConversationMessage::Kind::Error)
            continue;
        if (entry.kind == ConversationMessage::Kind::UserText
            || entry.kind == ConversationMessage::Kind::AgentTask) {
            return entry.id;
        }
        // 已提交的工具对仍可能留在失败轮末尾；继续往前找用户话。
    }
    return {};
}

void AbstractLoop::removeTrailingErrorEntries(const QString &turnId)
{
    const QList<ConversationMessage> entries = m_ledger.entries();
    for (qsizetype i = entries.size() - 1; i >= 0; --i) {
        const ConversationMessage &entry = entries.at(i);
        if (entry.kind != ConversationMessage::Kind::Error)
            break;
        if (!turnId.isEmpty() && entry.turnId != turnId)
            break;
        m_ledger.removeEntry(entry.id);
    }
}

bool AbstractLoop::retryLastFailedTurn(const SessionRuntime &config)
{
    if (m_mode == LoopMode::Busy) {
        LOGW(LogCat::Agent, logContext()) << "retryLastFailedTurn 被忽略：Loop 正在运行";
        return false;
    }

    const QString userEntryId = lastFailedUserEntryId();
    if (userEntryId.isEmpty()) {
        LOGW(LogCat::Agent, logContext()) << "retryLastFailedTurn 被忽略：没有可重放的用户消息";
        return false;
    }

    const ConversationMessage *userEntry = m_ledger.findById(userEntryId);
    if (!userEntry) {
        return false;
    }

    // removeEntry 会挪动账本，必须先拷字段。
    PendingMessage pending;
    pending.text = userEntry->text;
    pending.kind = userEntry->kind;
    pending.attachedFilePaths = userEntry->attachedFilePaths;
    pending.displaySummary = userEntry->summaryText;
    const QString failedTurnId = m_lastFailedTurnId.isEmpty() ? userEntry->turnId : m_lastFailedTurnId;
    removeTrailingErrorEntries(failedTurnId);

    // 复用账本用户消息：不重记一条，也不依赖已被 failTurn 清空的 next 队列。
    m_pendingUserEntryId = userEntryId;
    if (ConversationMessage *existing = m_ledger.findById(userEntryId)) {
        existing->status = ConversationMessage::Status::Queued;
        existing->isError = false;
        existing->resultCategory = ConversationMessage::ResultCategory::None;
        emitProtocolEvent(
            core_ir::EventMessageStatusChanged{userEntryId,
                                                ConversationMessage::Status::Queued});
    }

    m_nextTurnQueue.prepend(pending);
    m_lastFailedTurnId.clear();

    LOGI(LogCat::Agent, logContext()) << "重试上一失败轮"
        << logf("userEntry", userEntryId)
        << logf("failedTurn", failedTurnId);
    start(config);
    return isBusy();
}

void AbstractLoop::cancel()
{
    if (m_mode != LoopMode::Busy) {
        return;
    }
    // 先记下 turnId：cancelActiveExecution 可能同步触发 onToolCompleted→resetLoopState，
    // 若在其后才读 m_activeTurnId 会得到空串，rollback 变成空操作。
    const QString canceledTurnId = m_activeTurnId;
    LOGI(LogCat::Agent, logContext()) << "取消 Turn"
        << logf("sessionUuid", m_sessionUuid)
        << logf("turnId", canceledTurnId)
        << logf("unresolvedTools", m_ledger.hasUnresolvedToolCalls());
    m_cancelRequested = true;
    // A1：引导作废；B1：下一轮保留待确认
    m_steerQueue.clear();
    m_pendingConfig = std::nullopt;
    m_pendingApproval = {};
    m_pendingApprovalCall = {};
    m_pendingApprovalEntryId.clear();

    // 尽量打断底层请求/工具，但本地状态必须立刻收口：
    // 不能只依赖异步 Cancelled 事件，否则 reply 已空/丢事件时 UI 会永久 Busy。
    if (m_providerRequestActive && m_provider) {
        m_provider->cancel();
    }
    if (m_builtinRuntime.isRunning()) {
        m_builtinRuntime.cancelActiveExecution();
    }

    // 与 handleCancelled / handleError 同源：先 finalize 流式，再 rollback 未提交条目。
    // cancel() 本地收口后 mode=Idle，迟到的 Provider Cancelled 会被守卫直接丢弃，
    // 因此账本清理必须发生在这里，否则未闭合的 FunctionCall 会残留进下一轮
    // Chat Completions 全量回放（DeepSeek 等 400：tool_calls 缺 tool 回执）。
    // onToolCompleted 若已本地 Canceled 收口，mode 可能已 Idle；discard 仍用捕获的 turnId。
    m_providerRequestActive = false;
    finalizeAndDiscardTurn(canceledTurnId);
    if (m_ledger.hasUnresolvedToolCalls()) {
        LOGW(LogCat::Tool, logContext()) << "cancel 后仍有未闭合 tool_call"
            << logf("turnId", canceledTurnId);
    }
    notifyDataChanged();

    // 同步回调可能已 reset+Canceled；仍再走一遍保证队列/活动表清空（幂等）
    if (m_mode == LoopMode::Busy || !m_pendingToolCalls.isEmpty()
        || !m_activeToolCallsById.isEmpty() || m_cancelRequested) {
        resetLoopState();
        setPhase(Phase::Canceled);
    }
}

// ── 审批 ──

bool AbstractLoop::hasPendingApproval() const
{
    return m_pendingApproval.isValid();
}

bool AbstractLoop::hasPendingQuestion() const
{
    return !m_pendingQuestions.isEmpty();
}

int AbstractLoop::pendingQuestionCount() const
{
    return m_pendingQuestions.size();
}

QString AbstractLoop::pendingQuestionIdAt(const int index) const
{
    if (index < 0 || index >= m_pendingQuestions.size()) return {};
    return m_pendingQuestions.at(index).questionId;
}

QString AbstractLoop::pendingQuestionTextAt(const int index) const
{
    if (index < 0 || index >= m_pendingQuestions.size()) return {};
    return m_pendingQuestions.at(index).question;
}

QStringList AbstractLoop::pendingQuestionOptionsAt(const int index) const
{
    if (index < 0 || index >= m_pendingQuestions.size()) return {};
    return m_pendingQuestions.at(index).options;
}

bool AbstractLoop::pendingQuestionIsMultiSelectAt(const int index) const
{
    if (index < 0 || index >= m_pendingQuestions.size()) return false;
    return m_pendingQuestions.at(index).isMultiSelect;
}

QList<core_ir::PendingQuestion> AbstractLoop::pendingQuestionSnapshot() const
{
    QList<core_ir::PendingQuestion> list;
    list.reserve(m_pendingQuestions.size());
    for (const auto &q : m_pendingQuestions) {
        core_ir::PendingQuestion item;
        item.questionId = q.questionId;
        item.question = q.question;
        item.options = q.options;
        item.isMultiSelect = q.isMultiSelect;
        item.answered = q.answered;
        list.append(item);
    }
    return list;
}

bool AbstractLoop::submitQuestionAnswer(const int questionIndex, const QString &answer)
{
    if (questionIndex < 0 || questionIndex >= m_pendingQuestions.size()) {
        return false;
    }

    auto &q = m_pendingQuestions[questionIndex];
    q.answered = true;
    q.answer = answer;

    int answeredCount = 0;
    for (const auto &r : m_pendingQuestions) { if (r.answered) ++answeredCount; }
    LOGI(LogCat::Agent, logContext()) << "提问应答"
        << logf("index", questionIndex)
        << logf("answer", answer)
        << logf("progress", QStringLiteral("%1/%2").arg(answeredCount).arg(m_pendingQuestions.size()));

    // 检查是否全部已回答
    if (answeredCount < m_pendingQuestions.size()) {
        notifyStateChanged();
        return true;
    }

    // 全部已回答 — 统一 completeToolResult（必发 EventToolCallEnd）；仅最后一题 advanceLoop
    LOGI(LogCat::Agent, logContext()) << "全部提问已应答，开始逐个提交 ToolResult"
        << logf("count", m_pendingQuestions.size());
    const QList<PendingQuestionRequest> completed = m_pendingQuestions;
    const QList<ToolCall> completedCalls = m_pendingQuestionCalls;
    m_pendingQuestions.clear();
    m_pendingQuestionCalls.clear();

    for (int i = 0; i < completed.size(); ++i) {
        const ToolCall &tc = completedCalls.at(i);
        const QString responseText = completed.at(i).answer.isEmpty()
            ? QStringLiteral("(用户跳过了此问题)") : completed.at(i).answer;

        LOGI(LogCat::Agent, logContext()) << "提交 ToolResult"
            << logf("index", i)
            << logf("total", completed.size())
            << logf("toolId", tc.id)
            << logf("text", responseText);

        ToolResult result;
        result.toolName = tc.toolName;
        result.toolUseId = tc.id;
        result.success = true;
        result.isError = false;
        result.category = ToolResultCategory::Success;
        result.text = QStringLiteral("Q%1: %2").arg(i + 1).arg(responseText);
        result.summaryText = QStringLiteral("Q%1: %2").arg(i + 1).arg(responseText.left(30));
        result.progressText = QStringLiteral("answered");

        if (!completeToolResult(result, /*advanceLoop=*/(i == completed.size() - 1))) {
            LOGW(LogCat::Tool, logContext()) << "提问 ToolResult 未在活动表，跳过"
                << logf("toolId", tc.id)
                << logf("index", i);
        }
    }
    return true;
}

PendingApprovalRequest AbstractLoop::pendingApprovalRequest() const
{
    return m_pendingApproval;
}

bool AbstractLoop::approvePendingToolCall(const bool approved)
{
    if (!hasPendingApproval()) {
        return false;
    }

    LOGI(LogCat::Agent, logContext()) << "审批决策"
        << logf("approved", approved);

    if (ConversationMessage *approvalEntry = m_ledger.findById(m_pendingApprovalEntryId)) {
        approvalEntry->status = approved
                                    ? ConversationMessage::Status::Approved
                                    : ConversationMessage::Status::Rejected;
    }

    const ToolCall toolCall =m_pendingApprovalCall;
    m_pendingApproval = {};
    m_pendingApprovalCall = {};
    m_pendingApprovalEntryId.clear();

    if (!approved) {
        updateToolEntry(toolCall.id,
                        ConversationMessage::Status::Rejected,
                        QStringLiteral("Permission rejected"));
        appendToolResultEntry(BuiltinToolRuntime::makeRejectedResult(toolCall,
                                                               QStringLiteral("用户拒绝执行工具 %1。").arg(toolCall.toolName),
                                                               logContext()));
        setPhase(Phase::AppendingToolResults);
        continueAfterToolResult();
        return true;
    }

    executeToolCall(toolCall);
    return true;
}

// ── 状态查询 ──

bool AbstractLoop::isStreaming() const
{
    return m_mode == LoopMode::Busy;
}


QList<ConversationMessage> AbstractLoop::messages() const
{
    return m_ledger.projectMessages();
}

const QList<AbstractLoop::PendingMessage> &AbstractLoop::pendingMessages() const
{
    return m_nextTurnQueue;
}

qint64 AbstractLoop::currentContextTokenEstimate() const
{
    return m_currentContextTokenEstimate;
}

void AbstractLoop::appendExternalMessage(const ConversationMessage &message)
{
    ConversationMessage entry = message;

    switch (message.kind) {
    case ConversationMessage::Kind::SessionEvent:
        entry.kind = ConversationMessage::Kind::SessionEvent;
        entry.submittedToModel = false;
        break;
    case ConversationMessage::Kind::UserText:
        entry.kind = ConversationMessage::Kind::UserText;
        entry.submittedToModel = true;
        break;
    case ConversationMessage::Kind::AssistantText:
        entry.kind = ConversationMessage::Kind::AssistantText;
        entry.submittedToModel = true;
        break;
    case ConversationMessage::Kind::ToolCall:
        entry.kind = ConversationMessage::Kind::ToolCall;
        entry.submittedToModel = true;
        break;
    case ConversationMessage::Kind::ToolResult:
        entry.kind = ConversationMessage::Kind::ToolResult;
        entry.submittedToModel = true;
        break;
    case ConversationMessage::Kind::ApprovalRequest:
        entry.kind = ConversationMessage::Kind::ApprovalRequest;
        entry.submittedToModel = false;
        break;
    case ConversationMessage::Kind::Error:
        entry.kind = ConversationMessage::Kind::Error;
        entry.submittedToModel = false;
        break;
    case ConversationMessage::Kind::SkillInvoke:
        // 技能指令随下一轮请求作为待发送输入注入
        entry.submittedToModel = false;
        break;
    case ConversationMessage::Kind::SystemPrompt:
    case ConversationMessage::Kind::Summary:
        return;
    }

    appendLedgerEntry(entry);
}

// ── Turn 生命周期（内部） ──

void AbstractLoop::startProviderTurn()
{
    // 上下文压缩检测 —— 必须在任何状态修改之前
    // 语义：threshold = min(contextWindow, compactTriggerTokens?) - compactReserveTokens
    //   - contextWindow：模型窗口（含用户覆盖）
    //   - compactTriggerTokens：统一上限；<=0 表示不设统一上限
    //   - compactReserveTokens：预留；<0 按 0
    qint64 estimated = -1;
    if (m_activeConfig && m_activeConfig->compactEnabled) {
        // 与 UI 占用同一估算：账本可回放项 + 系统提示/工具 schema 开销
        updateContextTokenEstimate();
        estimated = m_currentContextTokenEstimate;
        const qint64 window = m_activeConfig->contextWindow > 0
            ? m_activeConfig->contextWindow
            : ModelContextMetaStore::kDefaultContextWindow;
        const qint64 unified = m_activeConfig->compactTriggerTokens;
        const qint64 cap = (unified > 0) ? qMin(window, unified) : window;
        const qint64 reserve = qMax<qint64>(0, m_activeConfig->compactReserveTokens);
        const qint64 threshold = cap > reserve ? (cap - reserve) : 0;
        if (estimated > threshold) {
            LOGI(LogCat::Agent, logContext()) << "触发压缩"
                << logf("tokens", estimated)
                << logf("threshold", threshold)
                << logf("contextWindow", window)
                << logf("unifiedTrigger", unified)
                << logf("reserve", reserve)
                << logf("providerInputFloor", m_ledger.lastProviderInputTokens());
            setPhase(Phase::Compacting);
            emit compactionRequested(estimated, threshold);
            return;
        }
    }

    startProviderTurnImpl(estimated);
}

void AbstractLoop::startProviderTurnImpl(qint64 contextTokenEstimate)
{
    LOGD(LogCat::Agent, logContext()) << "启动 Turn 实现"
        << logf("tag", QStringLiteral("turn"))
        << logf("loop", ptrTag(this))
        << logf("agent", m_agentId)

        << logf("step", m_internalStepCount + 1)
        << logf("toolScope", m_activeConfig ? toolScopeToString(m_activeConfig->toolScope) : QString())
        << logf("approvalMode", m_activeConfig ? approvalModeToString(m_activeConfig->approvalMode) : QString())
        << logf("workdir", m_activeConfig ? m_activeConfig->workingDirectory : QString())
        << logf("canceled", m_cancelRequested)
        << logf("pendingNext", m_nextTurnQueue.size())
        << logf("pendingSteer", m_steerQueue.size())
        << logf("pendingToolCalls", m_pendingToolCalls.size());
    if (!canContinueTurn()) {
        return;
    }

    emitProtocolEvent(core_ir::EventTurnStarted{m_activeTurnId, QDateTime::currentMSecsSinceEpoch()});

    LOGD(LogCat::Provider, logContext()) << "启动 Provider Turn"
        << logf("step", m_internalStepCount);

    setPhase(Phase::Preparing);

    ProviderRequestBuild build = m_ledger.buildRequest(toolSpecs(),
                                                       ProviderOutputSpec::textOnly(),
                                                       m_sessionUuid);
    if (!build.hydrateError.isEmpty()) {
        failTurn(QStringLiteral("附件资源缺失：%1").arg(build.hydrateError));
        return;
    }
    // 模型短上下文：摘要链 + 最近用户醒目块等前缀（Agent 已打标签；不物化进账本）
    if (!m_modelViewPrefixTexts.isEmpty()) {
        QList<ProviderItem> prefixed;
        prefixed.reserve(m_modelViewPrefixTexts.size() + build.request.items.size());
        for (const QString &text : m_modelViewPrefixTexts) {
            if (text.trimmed().isEmpty()) {
                continue;
            }
            // 已带方括号标签则原样注入；否则补默认摘要标签
            const QString body = text.trimmed().startsWith(QLatin1Char('['))
                ? text
                : (QStringLiteral("[上下文摘要]\n") + text);
            prefixed.append(ProviderItem::makeUserText(body));
        }
        prefixed += build.request.items;
        build.request.items = std::move(prefixed);
    }
    build.request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    build.request.systemPrompt = m_systemPrompt;
    // 输出上限由模型元数据层 resolve（user 原样 / cache 用 models.dev / default 按 provider 查表），
    // 此处只读值。-1 语义：协议中的 -1 仅表示交给 Provider 默认值。
    const qint64 kMinOutput = 1;
    build.request.maxOutputTokens = static_cast<int>(
        qBound<qint64>(kMinOutput, m_activeConfig->maxOutputTokens, INT64_MAX));
    build.request.temperature = 0.0;
    build.request.reasoning.requested = true;
    build.request.reasoning.enabled = m_activeConfig->reasoningEnabled;
    build.request.reasoning.effort = parseReasoningEffort(m_activeConfig->reasoningEffort);
    build.request.protocolFamily =
        protocolFamilyForProviderType(m_activeConfig->providerType);

    // 同一轮内不重复估算：压缩检测已算过则直接复用；其他入口（压缩后/工具后）重新计算。
    if (contextTokenEstimate >= 0)
        m_currentContextTokenEstimate = contextTokenEstimate;
    else
        updateContextTokenEstimate();
    m_ledger.markSubmitted(build.submittedEntryIds);
    m_providerRequestActive = true;
    m_streamingAssistantEntryId.clear();
    m_streamingReasoningEntryId.clear();
    m_currentResponseEntryIds.clear();

    setPhase(Phase::CallingProvider);

    // 瞬时错误重试预算下沉 Provider（CompactEngine/AutoRename 不调 setRetryPolicy → 零重试）
    m_provider->setRetryPolicy(ProviderRetryPolicy{m_activeConfig->maxRetries});
    m_provider->sendRequest(build.request);
    armModelResponseWatchdog();
}

void AbstractLoop::continueAfterCompaction()
{
    m_waitingBoundarySummary = false;
    startProviderTurnImpl();
}

void AbstractLoop::beginManualCompaction()
{
    // 与自动路径共用 Phase::Compacting；手动无 activeTurn，不发 TurnStarted
    m_waitingBoundarySummary = false;
    clearStickyTerminalPhase();
    switchMode(LoopMode::Busy);
    setPhase(Phase::Compacting);
}

void AbstractLoop::endManualCompaction()
{
    // 账本已变：刷新占用估算再推状态
    m_waitingBoundarySummary = false;
    updateContextTokenEstimate();
    // 正常路径仍在 Compacting；同步跳过/异常路径可能已离开 phase，但仍可能卡在 Busy
    if (m_phase == Phase::Compacting || (m_mode == LoopMode::Busy && !m_providerRequestActive)) {
        switchMode(LoopMode::Idle);
        if (m_phase == Phase::Compacting) {
            setPhase(Phase::Idle); // 顺带 notifyStateChanged
        } else {
            notifyStateChanged();
        }
    }
}

void AbstractLoop::beginBoundarySummaryWait()
{
    m_waitingBoundarySummary = true;
    clearStickyTerminalPhase();
    switchMode(LoopMode::Busy);
    setPhase(Phase::Compacting);
}

void AbstractLoop::endBoundarySummaryWait(const bool cancelled)
{
    if (!m_waitingBoundarySummary && m_phase != Phase::Compacting) {
        return;
    }
    m_waitingBoundarySummary = false;
    if (!cancelled) {
        return;
    }
    // G4a：结束等待回 Idle；队列由 Agent 保留
    if (m_mode == LoopMode::Busy && !m_providerRequestActive) {
        switchMode(LoopMode::Idle);
        if (m_phase == Phase::Compacting) {
            setPhase(Phase::Idle);
        } else {
            notifyStateChanged();
        }
    }
}

void AbstractLoop::continueAfterToolResult()
{
    if (m_cancelRequested) {
        const QString canceledTurnId = m_activeTurnId;
        finalizeAndDiscardTurn(canceledTurnId);
        resetLoopState();
        setPhase(Phase::Canceled);
        return;
    }

    if (!m_pendingToolCalls.isEmpty()) {
        processReadyToolCalls();
        return;
    }

    // 工具间隙：只注入 steer（引导），next_turn 留待本轮成功收口后开新轮
    if (!m_steerQueue.isEmpty()) {
        if (m_pendingConfig) {
            activateConfig(*m_pendingConfig);
        }

        const QList<PendingMessage> steerCopy = m_steerQueue;
        m_steerQueue.clear();

        for (const PendingMessage &msg : steerCopy) {
            ConversationMessage entry;
            entry.kind = msg.kind;
            entry.status = ConversationMessage::Status::Completed;
            entry.text = msg.text;
            entry.summaryText = msg.displaySummary;
            entry.attachedFilePaths = msg.attachedFilePaths;
            entry.turnId = m_activeTurnId;
            entry.submittedToModel = false;
            appendLedgerEntry(entry);
        }

        notifyDataChanged();
        notifyStateChanged();
    }

    startProviderTurn();
}

void AbstractLoop::processReadyToolCalls()
{
    if (m_pendingToolCalls.isEmpty()) {
        setPhase(Phase::Completed);
        return;
    }

    const ToolCall toolCall = m_pendingToolCalls.takeFirst();
    if (m_modePolicy) {
        if (auto blocked = m_modePolicy->beforeToolCall(toolCall)) {
            // 策略拒绝也走 onToolCompleted：先登记活动表，避免被迟到结果守卫丢弃导致 Turn 卡住。
            m_activeToolCallsById.insert(toolCall.id, toolCall);
            updateToolEntry(toolCall.id,
                            statusForToolResult(*blocked),
                            blocked->summaryText);
            onToolCompleted(*blocked);
            return;
        }
    }

    const ToolPermissionDecision decision = m_coordinator
        ? m_coordinator->evaluatePermission(toolCall.toolName,
                                            m_activeConfig->toolScope,
                                            m_activeConfig->approvalMode)
        : ToolPermissionDecision::Allow;
    // btw 纯问答旁路：执行期兜底拒绝一切工具调用（参考 sideQuestion canUseTool: deny）——
    // 模型若仍输出工具调用，一律回填「拒绝」结果并继续，不执行、不进审批。
    const ToolPermissionDecision finalDecision = m_toolCallsDenied
        ? ToolPermissionDecision::Deny
        : decision;
    const QString decisionText = decisionTextFor(finalDecision);
    LOGD(LogCat::Tool, logContext()) << "权限检查"
        << logf("tag", QStringLiteral("perm"))
        << logf("loop", ptrTag(this))
        << logf("agent", m_agentId)

        << logf("toolId", toolCall.id)
        << logf("tool", toolCall.toolName)
        << logf("decision", decisionText)
        << logf("toolScope", toolScopeToString(m_activeConfig->toolScope))
        << logf("approvalMode", approvalModeToString(m_activeConfig->approvalMode))
        << logf("remainingQueue", m_pendingToolCalls.size());
    LOGD(LogCat::Tool, logContext()) << "处理工具"
        << logf("tool", toolCall.toolName)
        << logf("decision", decisionLabelFor(finalDecision));
    if (finalDecision == ToolPermissionDecision::Deny) {
        const QString denyReason = m_toolCallsDenied
            ? QStringLiteral("btw 旁路不允许使用工具，请直接文字回答。")
            : QStringLiteral("当前权限模式拒绝执行工具 %1。").arg(toolCall.toolName);
        updateToolEntry(toolCall.id,
                        ConversationMessage::Status::Rejected,
                        QStringLiteral("Permission denied"));
        appendToolResultEntry(BuiltinToolRuntime::makeRejectedResult(toolCall,
                                                               denyReason,
                                                               logContext()));
        setPhase(Phase::AppendingToolResults);
        continueAfterToolResult();
        return;
    }

    if (finalDecision == ToolPermissionDecision::NeedsApproval) {
        updateToolEntry(toolCall.id, ConversationMessage::Status::ClassifierChecking);

        m_pendingApproval.toolUseId = toolCall.id;
        m_pendingApproval.toolName = toolCall.toolName;
        m_pendingApproval.summary = BuiltinToolRuntime::summarizeToolCall(toolCall);
        m_pendingApproval.rawInputJson = toolCall.rawInputJson;
        if (m_coordinator) {
            const ToolSpec spec = m_coordinator->specForName(toolCall.toolName);
            m_pendingApproval.permissionKind = spec.name.trimmed().isEmpty()
                ? ToolPermissionKind::Command
                : spec.permissionKind;
        } else {
            m_pendingApproval.permissionKind = ToolPermissionKind::Command;
        }
        m_pendingApprovalCall = toolCall;
        m_pendingApprovalEntryId = appendApprovalEntry(toolCall);

        updateToolEntry(toolCall.id, ConversationMessage::Status::WaitingApproval);

        emitProtocolEvent(core_ir::EventApprovalRequested{
            toolCall.id, toolCall.toolName,
            BuiltinToolRuntime::summarizeToolCall(toolCall),
            toolCall.rawInputJson
        });
        setPhase(Phase::WaitingApproval);
        return;
    }

    executeToolCall(toolCall);
}

void AbstractLoop::executeToolCall(const ToolCall &providerToolCall)
{
    const ToolCall toolCall = providerToolCall;
    LOGI(LogCat::Tool, logContext()) << "执行工具"
        << logf("tool", toolCall.toolName)
        << logf("toolId", toolCall.id);

    if (toolCall.toolName == QStringLiteral("ask_question")) {
        // 解析归 AskQuestionTool；Loop 只负责收集 pending 并挂起等待回答
        auto collectOne = [this](const ToolCall &tc) {
            // 先登记再解析：校验失败走 onToolCompleted 时活动表必须已有该 id，
            // 否则会被「迟到异步结果」守卫直接丢弃导致 Turn 卡住。
            m_activeToolCallsById.insert(tc.id, tc);

            const auto parsed = AskQuestionTool::parseCall(tc);
            if (const auto *error = std::get_if<ToolResult>(&parsed)) {
                LOGW(LogCat::Tool, logContext()) << "ask_question 校验失败"
                    << logf("msg", error->text);
                onToolCompleted(*error);
                return;
            }

            const auto &request = std::get<AskQuestionTool::ParsedRequest>(parsed);
            PendingQuestionRequest pending;
            pending.questionId = request.questionId;
            pending.question = request.question;
            pending.options = request.options;
            pending.isMultiSelect = request.isMultiSelect;
            m_pendingQuestions.append(pending);
            m_pendingQuestionCalls.append(tc);
            if (m_modePolicy) {
                m_modePolicy->afterToolAccepted(tc);
            }
            updateToolEntry(tc.id, ConversationMessage::Status::WaitingAnswer);
        };
        collectOne(toolCall);

        while (!m_pendingToolCalls.isEmpty()
               && m_pendingToolCalls.first().toolName == QStringLiteral("ask_question")) {
            collectOne(m_pendingToolCalls.takeFirst());
        }

        LOGI(LogCat::Tool, logContext()) << "ask_question 收集问题"
            << logf("count", m_pendingQuestions.size())
            << logf("remainingQueue", m_pendingToolCalls.size());

        // 全部校验失败时不要进入 WaitingAnswer，交给工具结果回流继续推进
        if (m_pendingQuestions.isEmpty()) {
            return;
        }

        setPhase(Phase::WaitingAnswer);
        return;
    }

    updateToolEntry(toolCall.id, ConversationMessage::Status::Running);
    setPhase(Phase::ExecutingTool);
    m_activeToolCallsById.insert(toolCall.id, toolCall);
    if (m_modePolicy) {
        m_modePolicy->afterToolAccepted(toolCall);
    }

    if (m_coordinator) {
        // QPointer 保护：异步工具（如 mcp_server add 最长 90s）回调时 this 可能已销毁。
        // 迟到结果由 onToolCompleted 的活动表守卫统一丢弃。
        const QPointer<AbstractLoop> self(this);
        m_coordinator->dispatch(m_agentId, toolCall, m_activeConfig->workingDirectory,
                                m_builtinRuntime,
                                [self](ToolResult result) {
                                    if (self) {
                                        self->onToolCompleted(result);
                                    }
                                });
    }
}

void AbstractLoop::onToolCompleted(const ToolResult &result)
{
    (void)completeToolResult(result, /*advanceLoop=*/true);
}

bool AbstractLoop::completeToolResult(const ToolResult &result, const bool advanceLoop)
{
    // 二次校验：cancel/reset 后活动表不含该 id，直接忽略（防止竞态双入）。
    if (!m_activeToolCallsById.contains(result.toolUseId)) {
        LOGD(LogCat::Tool, logContext()) << "completeToolResult 忽略非活动工具结果"
            << logf("toolId", result.toolUseId)
            << logf("tool", result.toolName)
            << logf("advance", advanceLoop);
        return false;
    }

    const ToolCall originalToolCall = m_activeToolCallsById.take(result.toolUseId);

    updateToolEntry(result.toolUseId,
                    statusForToolResult(result),
                    result.progressText);

    // 账本写入前：summary 必须是调用对象。工厂/会话工具应已填；空则从原始 call 补。
    ToolResult ledgerResult = result;
    if (ledgerResult.summaryText.trimmed().isEmpty()) {
        ledgerResult.summaryText = BuiltinToolRuntime::summarizeToolCall(originalToolCall);
    }

    const QString resultEntryId = appendToolResultEntry(ledgerResult);

    // 每条结果必发 End；客户端 live 卡只认这条
    emitProtocolEvent(core_ir::EventToolCallEnd{result.toolUseId, ledgerResult,
                                                 resultEntryId, m_activeTurnId});

    // 成功写入类工具 → 协议 FileTouch，供右栏「本会话 touch」投影（禁止 GUI 猜路径）
    if (result.success) {
        QStringList touchPaths;
        if (result.toolName == QLatin1String("write_file")
            || result.toolName == QLatin1String("edit")
            || result.toolName == QLatin1String("notebook_edit")) {
            const QString rawPath = originalToolCall.input.value(QStringLiteral("filePath")).toString().trimmed();
            if (!rawPath.isEmpty())
                touchPaths.append(rawPath);
        } else if (result.toolName == QLatin1String("multi_edit")) {
            // 逐文件 touch，同文件去重
            QSet<QString> seen;
            const QJsonArray edits = originalToolCall.input.value(QStringLiteral("edits")).toArray();
            for (const QJsonValue &v : edits) {
                const QString rawPath = v.toObject().value(QStringLiteral("filePath")).toString().trimmed();
                if (rawPath.isEmpty() || seen.contains(rawPath))
                    continue;
                seen.insert(rawPath);
                touchPaths.append(rawPath);
            }
        }
        for (const QString &rawPath : touchPaths) {
            QString absPath = rawPath;
            if (m_activeConfig.has_value() && !QDir::isAbsolutePath(rawPath)) {
                absPath = QDir(m_activeConfig->workingDirectory).absoluteFilePath(rawPath);
            }
            // 广播 key 统一 normalizedPath（小写 + 正斜杠）：
            // 否则跨 Agent 失效广播的 key 与缓存 key 不匹配，删除静默失败
            emitProtocolEvent(core_ir::EventFileTouched{
                WorkspaceHelper::normalizedPath(absPath),
                result.toolName,
                QDateTime::currentMSecsSinceEpoch()});
        }
    }

    if (result.category == ToolResultCategory::Canceled || m_cancelRequested) {
        // 本条已 append 闭合；同 turn 其它未完成 tool_call 仍要 rollback，
        // 否则只闭合当前一条，兄弟 tool_calls 仍会污染下一轮 Chat Completions。
        const QString canceledTurnId = m_activeTurnId;
        finalizeAndDiscardTurn(canceledTurnId);
        resetLoopState();
        setPhase(Phase::Canceled);
        return true;
    }

    // Idle / 无活动配置：说明 Turn 已收口，不能再 continue。
    if (m_mode != LoopMode::Busy || !m_activeConfig.has_value()) {
        LOGD(LogCat::Tool, logContext()) << "completeToolResult 时 Loop 已非 Busy，丢弃后续推进"
            << logf("toolId", result.toolUseId);
        return true;
    }

    if (m_modePolicy && !originalToolCall.id.isEmpty()) {
        m_modePolicy->afterToolCall(originalToolCall, result);
    }

    if (!advanceLoop)
        return true;

    setPhase(Phase::AppendingToolResults);
    continueAfterToolResult();
    return true;
}

void AbstractLoop::failTurn(const QString &errorMessage)
{
    LOGE(LogCat::Agent, logContext()) << "Turn 失败"
        << logf("msg", errorMessage);
    // reset 会清 m_activeTurnId：先记下失败轮，Error 条目与后续 RetryTurn 都靠它。
    const QString failedTurnId = m_activeTurnId;
    const QString failedResponseId = m_currentResponseId;
    m_lastFailedTurnId = failedTurnId;
    // A1：引导作废；B1：next_turn 保留且不自动开轮
    m_steerQueue.clear();
    resetLoopState();
    m_lastError = errorMessage;
    setPhase(Phase::Failed);

    if (!errorMessage.trimmed().isEmpty()) {
        ConversationMessage entry;
        entry.kind = ConversationMessage::Kind::Error;
        entry.status = ConversationMessage::Status::Failed;
        entry.text = errorMessage;
        entry.isError = true;
        entry.resultCategory = ConversationMessage::ResultCategory::Error;
        entry.turnId = failedTurnId;
        entry.responseId = failedResponseId;
        entry.submittedToModel = false;
        appendLedgerEntry(entry);
    }
}

// ── 账本操作 ──

QString AbstractLoop::appendLedgerEntry(ConversationMessage entry)
{
    const QString entryId = m_ledger.appendUiIngress(std::move(entry));

    // ProtocolEvent: 消息追加通知
    // 工具/审批/任务类消息由专用 Event 处理（ToolCallBegin/End, ApprovalRequested）
    const ConversationMessage *appended = m_ledger.findById(entryId);
    if (appended
        && appended->kind != ConversationMessage::Kind::ToolCall
        && appended->kind != ConversationMessage::Kind::ToolResult
        && appended->kind != ConversationMessage::Kind::ApprovalRequest
        && appended->kind != ConversationMessage::Kind::AgentTask
        && appended->kind != ConversationMessage::Kind::SystemPrompt
        && appended->kind != ConversationMessage::Kind::Summary
        && appended->kind != ConversationMessage::Kind::SkillInvoke) {
        emitProtocolEvent(core_ir::EventMessageAppended{*appended});
    }

    return entryId;
}

void AbstractLoop::appendHiddenSystemMessage(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    ConversationMessage entry;
    entry.kind = ConversationMessage::Kind::SystemPrompt;
    entry.status = ConversationMessage::Status::Completed;
    entry.text = trimmed;
    entry.turnId = m_activeTurnId;
    entry.submittedToModel = false;
    appendLedgerEntry(entry);
}

QString AbstractLoop::appendApprovalEntry(const ToolCall &toolCall)
{
    ConversationMessage entry;
    entry.kind = ConversationMessage::Kind::ApprovalRequest;
    entry.status = ConversationMessage::Status::WaitingApproval;
    entry.text = BuiltinToolRuntime::summarizeToolCall(toolCall);
    entry.toolName = toolCall.toolName;
    entry.toolUseId = toolCall.id;
    entry.toolInput = toolCall.input;
    entry.summaryText = entry.text;
    entry.turnId = m_activeTurnId;
    entry.responseId = m_currentResponseId;
    entry.submittedToModel = false;
    return appendLedgerEntry(entry);
}

ConversationMessage *AbstractLoop::ensureStreamingEntry(ConversationMessage::Kind kind,
                                                        QString &entryId)
{
    if (!entryId.isEmpty()) {
        return m_ledger.findById(entryId);
    }

    ConversationMessage entry;
    entry.kind = kind;
    if (!m_currentResponseId.isEmpty()) {
        if (kind == ConversationMessage::Kind::AssistantReasoning) {
            entry.id = m_currentResponseId + QStringLiteral("-reasoning");
        } else {
            entry.id = m_currentResponseId;
        }
    }
    entry.status = ConversationMessage::Status::Streaming;
    entry.turnId = m_activeTurnId;
    entry.responseId = m_currentResponseId;
    entry.submittedToModel = true;
    entryId = appendLedgerEntry(entry);
    recordCurrentResponseEntryId(entryId);
    return m_ledger.findById(entryId);
}

ConversationMessage *AbstractLoop::ensureStreamingAssistantEntry()
{
    return ensureStreamingEntry(ConversationMessage::Kind::AssistantText,
                               m_streamingAssistantEntryId);
}

ConversationMessage *AbstractLoop::ensureStreamingReasoningEntry()
{
    return ensureStreamingEntry(ConversationMessage::Kind::AssistantReasoning,
                               m_streamingReasoningEntryId);
}

void AbstractLoop::finalizeStreamingReasoning()
{
    if (m_streamingReasoningEntryId.isEmpty()) {
        return;
    }

    // 推理消息必须显式广播 StatusChanged：过程卡 allDone 依赖它。
    // 仅写账本时，非焦点会话收不到 ConversationSnapshot，会永久停在「思考中」。
    if (ConversationMessage *entry = m_ledger.findById(m_streamingReasoningEntryId)) {
        entry->status = ConversationMessage::Status::Completed;
        emitProtocolEvent(core_ir::EventMessageStatusChanged{
            m_streamingReasoningEntryId, ConversationMessage::Status::Completed});
    }
    m_streamingReasoningEntryId.clear();
}

void AbstractLoop::finalizeStreamingAssistant()
{
    if (m_streamingAssistantEntryId.isEmpty() && m_streamingReasoningEntryId.isEmpty()
        && !m_hasPendingUsage) {
        return;
    }

    // 先收口推理：纯思考 turn 没有助手正文，也必须推送 Completed
    finalizeStreamingReasoning();
    if (ConversationMessage *entry = m_ledger.findById(m_streamingAssistantEntryId)) {
        entry->status = ConversationMessage::Status::Completed;
        entry->responseId = m_currentResponseId;
        emitProtocolEvent(core_ir::EventMessageStatusChanged{
            m_streamingAssistantEntryId, ConversationMessage::Status::Completed});
    }
    m_streamingAssistantEntryId.clear();

    // 合并：仅在所有字段更新完毕后重建一次
    applyPendingUsage();
}

void AbstractLoop::recordCurrentResponseEntryId(const QString &entryId)
{
    if (entryId.isEmpty()) {
        return;
    }

    if (!m_currentResponseEntryIds.contains(entryId)) {
        m_currentResponseEntryIds.append(entryId);
    }
}

void AbstractLoop::applyPendingUsage()
{
    if ((!m_hasPendingUsage && m_currentResponseId.isEmpty()) || m_currentResponseEntryIds.isEmpty()) {
        return;
    }

    for (const QString &entryId : std::as_const(m_currentResponseEntryIds)) {
        if (ConversationMessage *entry = m_ledger.findById(entryId)) {
            if (!m_currentResponseId.isEmpty()) {
                entry->responseId = m_currentResponseId;
            }
            entry->inputTokens = m_pendingUsage.inputTokens;
            entry->outputTokens = m_pendingUsage.outputTokens;
            entry->cacheReadTokens = m_pendingUsage.cacheReadTokens;
            entry->cacheCreationTokens = m_pendingUsage.cacheWriteTokens;
            entry->thoughtTokens = m_pendingUsage.thoughtTokens;
        }
    }

    updateContextTokenEstimate();
    m_currentResponseEntryIds.clear();
    m_pendingUsage = {};
    m_hasPendingUsage = false;
}

void AbstractLoop::discardIncompleteEntriesForTurn(const QString &turnId)
{
    if (turnId.isEmpty()) {
        return;
    }

    m_ledger.rollbackUncommittedTurn(turnId);
    m_streamingAssistantEntryId.clear();
    m_streamingReasoningEntryId.clear();

    for (qsizetype i = m_currentResponseEntryIds.size() - 1; i >= 0; --i) {
        if (!m_ledger.findById(m_currentResponseEntryIds.at(i))) {
            m_currentResponseEntryIds.removeAt(i);
        }
    }
}

void AbstractLoop::finalizeAndDiscardTurn(const QString &turnId)
{
    finalizeStreamingAssistant();
    discardIncompleteEntriesForTurn(turnId);
}

void AbstractLoop::discardCurrentAssistantTextForPolicyRetry()
{
    if (m_activeTurnId.isEmpty()) {
        return;
    }

    const QList<ConversationMessage> entries = m_ledger.entries();
    QStringList removedEntryIds;
    for (qsizetype i = entries.size() - 1; i >= 0; --i) {
        const ConversationMessage &entry = entries.at(i);
        if (entry.turnId != m_activeTurnId) {
            continue;
        }
        if (!m_currentResponseId.isEmpty() && entry.responseId != m_currentResponseId) {
            continue;
        }
        if (entry.kind != ConversationMessage::Kind::AssistantText
            && entry.kind != ConversationMessage::Kind::AssistantReasoning) {
            continue;
        }

        m_currentResponseEntryIds.removeAll(entry.id);
        removedEntryIds.append(entry.id);
    }
    for (const QString &entryId : removedEntryIds)
        m_ledger.removeEntry(entryId);

    notifyDataChanged();
}

bool AbstractLoop::runModePolicyCheckpoint()
{
    if (!m_modePolicy) {
        return false;
    }

    const std::optional<QString> hiddenMessage = m_modePolicy->afterMessageCompleted();
    if (!hiddenMessage || hiddenMessage->trimmed().isEmpty()) {
        return false;
    }

    discardCurrentAssistantTextForPolicyRetry();
    appendHiddenSystemMessage(*hiddenMessage);
    startProviderTurn();
    return true;
}

void AbstractLoop::updateToolEntry(const QString &toolUseId,
                                        const ConversationMessage::Status status,
                                        const QString &progressText,
                                        const QString &summaryText)
{
    if (ConversationMessage *entry = m_ledger.findToolCallByUseId(toolUseId)) {
        entry->status = status;
        if (!progressText.isNull()) {
            entry->progressText = progressText;
        }
        if (!summaryText.isNull() && !summaryText.isEmpty()) {
            entry->summaryText = summaryText;
        }
    }
}

QString AbstractLoop::appendToolResultEntry(const ToolResult &result)
{
    ConversationMessage entry;
    entry.kind = ConversationMessage::Kind::ToolResult;
    entry.status = statusForToolResult(result);
    entry.text = result.text;
    entry.toolName = result.toolName;
    entry.toolUseId = result.toolUseId;
    entry.toolPayloadType = result.payloadType;
    entry.toolPayload = result.payload;
    entry.isError = result.isError;
    entry.summaryText = result.summaryText;
    entry.progressText = result.progressText;
    entry.previewText = result.previewText;
    entry.persistedPath = result.persistedPath;
    entry.wasPersisted = result.wasPersisted;
    entry.wasTruncated = result.wasTruncated;
    entry.resultCategory = resultCategoryForToolResult(result);
    entry.turnId = m_activeTurnId;
    entry.responseId = m_currentResponseId;
    entry.submittedToModel = false;
    return appendLedgerEntry(entry);
}

// ── 工具 ──

QList<ProviderToolSpecification> AbstractLoop::toolSpecs() const
{
    return m_cachedProviderToolSpecs;
}

void AbstractLoop::rebuildProviderToolSpecsCache()
{
    m_cachedProviderToolSpecs.clear();
    if (!m_coordinator) {
        return;
    }
    const QList<ToolSpec> specs = m_coordinator->specsForAgent(m_agentId);
    m_cachedProviderToolSpecs.reserve(specs.size());
    for (const ToolSpec &spec : specs) {
        ProviderToolSpecification tool;
        tool.name = spec.name;
        tool.description = spec.description;
        tool.inputSchema = spec.inputSchema;
        m_cachedProviderToolSpecs.append(tool);
    }
    // 工具集变化 → 请求固定开销（tool schema）随之失效
    m_requestOverheadDirty = true;
}

// ── 阶段管理 ──


void AbstractLoop::setSystemPrompt(const QString &prompt)
{
    if (m_systemPrompt == prompt)
        return;
    m_systemPrompt = prompt;
    m_requestOverheadDirty = true;
}



void AbstractLoop::updateContextTokenEstimate()
{
    // 请求级固定开销：系统提示 + 当前工具 schema（不进账本 records）。
    // 三者在各自变更点失效缓存，不重复做文本/JSON 的 token 估算。
    if (m_requestOverheadDirty) {
        qint64 overhead = estimateContextTokensForText(m_systemPrompt)
            + estimateContextTokensForToolSpecs(toolSpecs());
        // 摘要库投影的模型视图前缀（与 startProviderTurnImpl 注入同源）
        for (const QString &text : m_modelViewPrefixTexts) {
            overhead += estimateContextTokensForText(text);
        }
        m_cachedRequestOverheadTokens = overhead;
        m_requestOverheadDirty = false;
    }
    m_currentContextTokenEstimate =
        m_ledger.estimatedContextTokens(m_cachedRequestOverheadTokens);
}

void AbstractLoop::setModelViewPrefixTexts(QList<QString> texts)
{
    m_modelViewPrefixTexts = std::move(texts);
    m_requestOverheadDirty = true;
    updateContextTokenEstimate();
}

void AbstractLoop::clearModelViewPrefix()
{
    if (m_modelViewPrefixTexts.isEmpty()) {
        return;
    }
    m_modelViewPrefixTexts.clear();
    m_requestOverheadDirty = true;
    updateContextTokenEstimate();
}

QString AbstractLoop::nextTurnId()
{
    return QStringLiteral("turn-%1").arg(++m_turnSequence);
}


// ── Provider 连接 ──

bool AbstractLoop::ensureProvider()
{
    const QString requestedProviderType = ProviderService::normalizeProviderType(m_activeConfig->providerType);
    if (m_provider && m_activeProviderType != requestedProviderType) {
        LOGD(LogCat::Provider, logContext()) << "Provider 类型变化，重建实例"
            << logf("from", m_activeProviderType)
            << logf("to", requestedProviderType);
        m_provider.reset();
        m_activeProviderType.clear();
    }

    if (m_provider) {
        applyProviderSettings();
        return true;
    }

    if (!m_providerFactory) {
        return false;
    }
    std::unique_ptr<AbstractProvider> provider = m_providerFactory(requestedProviderType);
    if (!provider) {
        return false;
    }

    m_provider = std::move(provider);
    m_activeProviderType = requestedProviderType;
    applyProviderSettings();

    // 链接 provider 信号到本类槽函数
    connect(m_provider.get(), &AbstractProvider::eventEmitted, this, &AbstractLoop::onProviderEvent);
    return true;
}

void AbstractLoop::applyProviderSettings()
{

    m_provider->setLogContext(logContext());

    const QVariantMap inst = m_credentialStore ? m_credentialStore->getInstance(m_activeConfig->credentialInstanceId)
                                               : QVariantMap{};
    const ProviderAuth auth{
        inst.value(QStringLiteral("baseUrl")).toString(),
        inst.value(QStringLiteral("apiKey")).toString(),
        m_activeConfig->modelName
    };
    m_provider->setAuth(auth);

    const QString currentShell = m_activeConfig->defaultShell;
    const bool shellChanged = !m_systemPromptDefaultShell.isEmpty()
        && m_systemPromptDefaultShell != currentShell;
    refreshModePolicyIfNeeded(*m_activeConfig);
    if (shellChanged) {
        // 对话中切换默认终端：不修改系统提示词，改为在本次 Turn 注入隐藏系统消息。
        appendHiddenSystemMessage(
            QStringLiteral("用户已将默认终端切换为 %1，后续命令请使用该终端的语法。")
                .arg(currentShell));
    } else {
        setSystemPrompt(assembleSystemPrompt(*m_activeConfig));
    }
    m_systemPromptDefaultShell = currentShell;
}


// ── Provider 事件处理 ──

void AbstractLoop::onProviderEvent(const ProviderEvent &event)
{
    switch (event.kind) {
    case ProviderEventKind::MessageStarted:      handleMessageStarted(event);      break; // LLM 开始生成回复
    case ProviderEventKind::TextDelta:           handleTextDelta(event);           break; // 流式文本增量
    case ProviderEventKind::ReasoningDelta:      handleReasoningDelta(event);      break; // 推理过程增量
    case ProviderEventKind::ToolCallStarted:     handleToolCallStarted(event);     break; // LLM 开始输出工具调用
    case ProviderEventKind::ToolCallCompleted:   handleToolCallCompleted(event);   break; // 工具调用参数完整接收
    case ProviderEventKind::UsageUpdated:        handleUsageUpdated(event);        break; // Token 用量更新
    case ProviderEventKind::ResponseMetadata:    handleResponseMetadata(event);    break;
    case ProviderEventKind::MessageCompleted:    handleMessageCompleted(event);    break; // LLM 回复完成（可能含工具调用）
    case ProviderEventKind::ImageOutput:         handleImageOutput(event);         break; // 模型生成图片输出
    case ProviderEventKind::TranscriptDelta:     handleTextDelta(event);           break; // 音频转写按文本展示
    case ProviderEventKind::AudioDelta:          armModelResponseWatchdog();        break; // 音频由终态 ProviderItem 落账
    case ProviderEventKind::Error:               handleError(event);               break; // Provider 层错误
    case ProviderEventKind::Cancelled:           handleCancelled(event);           break; // 请求被取消
    default:                                     break;
    }
}

void AbstractLoop::handleMessageStarted(const ProviderEvent &event)
{
    LOGD(LogCat::Provider, logContext()) << "消息开始"
        << logf("id", event.messageStart.messageId);
    armModelResponseWatchdog();
    setPhase(Phase::Streaming);
    m_currentResponseId = event.messageStart.messageId;

    // ProtocolEvent
    emitProtocolEvent(core_ir::EventItemStarted{
        event.messageStart.messageId,
        m_activeTurnId,
        core_ir::ItemKind::Message,
        QDateTime::currentMSecsSinceEpoch()
    });
}

void AbstractLoop::handleTextDelta(const ProviderEvent &event)
{
    armModelResponseWatchdog();
    if (ConversationMessage *entry = ensureStreamingAssistantEntry()) {
        // 流式文本增量日志：记录增量长度/累计长度/增量文本预览，
        // 便于复现「半句停止」时判断模型输出到哪、是否突然断流
        LOGD(LogCat::Provider, logContext()) << "流式文本增量"
            << logf("mid", event.deltaPayload.base.messageId)
            << logf("deltaLen", event.deltaPayload.text.size())
            << logf("totalLen", entry->text.size() + event.deltaPayload.text.size())
            << logf("delta", event.deltaPayload.text.left(120));
        entry->text.append(event.deltaPayload.text);
        entry->responseId = event.deltaPayload.base.messageId;
        emitProtocolEvent(core_ir::EventAgentMessageContentDelta{
            entry->id,
            event.deltaPayload.text
        });
    }
    setPhase(Phase::Streaming);
    notifyDataChanged();
}

void AbstractLoop::handleReasoningDelta(const ProviderEvent &event)
{
    armModelResponseWatchdog();
    if (ConversationMessage *entry = ensureStreamingReasoningEntry()) {
        entry->reasoningContent += event.deltaPayload.text;
        // 同步线路记录：流式只改 UI 时，ProviderRecord 会停在双空态，
        // 工具轮次 step≥2 会把空 reasoning 回放给厂商并被拒。
        if (!entry->reasoningContent.trimmed().isEmpty()
            || !entry->reasoningSignature.trimmed().isEmpty()
            || entry->reasoningRedacted) {
            m_ledger.setProviderItemForEntry(
                entry->id,
                ProviderItem::makeReasoning(entry->reasoningContent,
                                            entry->reasoningSignature,
                                            entry->reasoningRedacted,
                                            entry->reasoningMustReplay),
                m_providerContinuationId);
        }
        emitProtocolEvent(core_ir::EventReasoningContentDelta{
            entry->id,
            event.deltaPayload.text,
            0
        });
    }
}

void AbstractLoop::handleUsageUpdated(const ProviderEvent &event)
{
    m_pendingUsage = event.usage;
    m_hasPendingUsage = true;
    // 厂商 input_tokens 作后续本地估算下界，减轻系统性低估
    if (event.usage.inputTokens > 0) {
        m_ledger.noteProviderInputTokens(event.usage.inputTokens);
    }

    emitProtocolEvent(core_ir::EventTokenCount{
        event.usage.inputTokens,
        event.usage.outputTokens,
        event.usage.cacheReadTokens,
        event.usage.cacheWriteTokens
    });
}

void AbstractLoop::handleResponseMetadata(const ProviderEvent &event)
{
    if (!event.responseMetadata.providerResponseId.isEmpty())
        m_providerContinuationId = event.responseMetadata.providerResponseId;
}

void AbstractLoop::handleImageOutput(const ProviderEvent &event)
{
    armModelResponseWatchdog();
    finalizeStreamingAssistant();

    const ProviderImageAsset &image = event.deltaPayload.image;
    if (!image.hasUri() && !image.hasInlineData()) return;

    const QString msgId = event.deltaPayload.base.messageId;
    ConversationMessage entry;
    entry.kind = ConversationMessage::Kind::AssistantText;
    entry.status = ConversationMessage::Status::Completed;
    entry.imageOutput = image;
    entry.responseId = msgId;
    entry.turnId = m_activeTurnId;

    // 如果有 altText，放入 text 字段供 UI 展示
    if (!image.altText.isEmpty()) {
        entry.text = image.altText;
    }

    appendLedgerEntry(entry);

    emitProtocolEvent(core_ir::EventImageOutput{entry.id, image});
    emitProtocolEvent(core_ir::EventMessageAppended{entry});
    notifyDataChanged();
}

void AbstractLoop::handleToolCallStarted(const ProviderEvent &event)
{
    const auto &call = event.deltaPayload;
    LOGD(LogCat::Tool, logContext()) << "Provider 工具调用开始"
        << logf("tag", QStringLiteral("provider-tool"))
        << logf("loop", ptrTag(this))
        << logf("agent", m_agentId)

        << logf("toolId", call.toolCallId)
        << logf("tool", call.toolName)
        << logf("messageId", call.base.messageId)
        << logf("queueSize", m_pendingToolCalls.size());
    LOGD(LogCat::Tool, logContext()) << "工具调用开始"
        << logf("tool", call.toolName)
        << logf("toolId", call.toolCallId);
    armModelResponseWatchdog();
    finalizeStreamingAssistant();

    // 厂商服务端工具由 Provider 自己执行，绝不能进入本地 ToolRuntime 队列。
    // 完整、可回放的 ServerToolCall/Result 会随 MessageCompleted.outputItems
    // 写入 ProviderRunLedger；Started 这里只保留流式生命周期信号。
    if (call.isServerTool) {
        emitProtocolEvent(core_ir::EventItemStarted{
            call.toolCallId,
            m_activeTurnId,
            core_ir::ItemKind::FunctionCall,
            QDateTime::currentMSecsSinceEpoch()
        });
        return;
    }

    ConversationMessage *entry = m_ledger.findToolCallByUseId(call.toolCallId);
    const bool isNewEntry = (entry == nullptr);
    QString entryId;
    if (!entry) {
        ConversationMessage newEntry;
        newEntry.kind = ConversationMessage::Kind::ToolCall;
        newEntry.status = ConversationMessage::Status::Queued;
        newEntry.toolCall.id = call.toolCallId;
        newEntry.toolCall.toolName = call.toolName;
        newEntry.text = BuiltinToolRuntime::summarizeToolCall(newEntry.toolCall);
        newEntry.summaryText = newEntry.text;
        newEntry.toolName = call.toolName;
        newEntry.toolUseId = call.toolCallId;
        newEntry.groupKey = call.toolName;
        newEntry.turnId = m_activeTurnId;
        newEntry.responseId = call.base.messageId;
        newEntry.submittedToModel = true;
        entryId = appendLedgerEntry(newEntry);
        recordCurrentResponseEntryId(entryId);
    } else {
        entryId = entry->id;
        entry->toolName = call.toolName;
        entry->toolUseId = call.toolCallId;
        entry->groupKey = call.toolName;
        entry->responseId = call.base.messageId;
        if (entry->toolCall.id.isEmpty()) {
            entry->toolCall.id = call.toolCallId;
            entry->toolCall.toolName = call.toolName;
        }
    }

    emitProtocolEvent(core_ir::EventItemStarted{
        call.toolCallId,
        m_activeTurnId,
        core_ir::ItemKind::FunctionCall,
        QDateTime::currentMSecsSinceEpoch()
    });
    // 仅新建条目时广播 Begin：重复 Started（如 response.completed 回放）不得再 append GUI 行
    if (isNewEntry) {
        emitProtocolEvent(core_ir::EventToolCallBegin{
            call.toolCallId,
            call.toolName,
            QJsonObject(),
            BuiltinToolRuntime::summarizeToolCall(ToolCall{call.toolCallId, call.toolName, {}}),
            entryId,
            m_activeTurnId
        });
    }
}

void AbstractLoop::handleToolCallCompleted(const ProviderEvent &event)
{
    const auto &call = event.deltaPayload;
    const int queueBeforeAppend = m_pendingToolCalls.size();
    LOGD(LogCat::Tool, logContext()) << "Provider 工具调用完成"
        << logf("tag", QStringLiteral("provider-tool"))
        << logf("loop", ptrTag(this))
        << logf("agent", m_agentId)

        << logf("toolId", call.toolCallId)
        << logf("tool", call.toolName)
        << logf("messageId", call.base.messageId)
        << logf("queueBefore", queueBeforeAppend);
    LOGI(LogCat::Tool, logContext()) << "模型请求工具调用"
        << logf("tool", call.toolName)
        << logf("toolId", call.toolCallId)
        << logf("args", QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact)));

    armModelResponseWatchdog();
    finalizeStreamingAssistant();

    // 服务端工具不在客户端执行。调用与结果由 MessageCompleted.outputItems
    // 统一落账，避免流式 Completed 和最终快照各写一份。
    if (call.isServerTool) {
        emitProtocolEvent(core_ir::EventItemCompleted{
            call.toolCallId,
            m_activeTurnId,
            core_ir::ItemKind::FunctionCall,
            QDateTime::currentMSecsSinceEpoch(),
            std::nullopt,
            std::nullopt,
            call.toolName
        });
        notifyDataChanged();
        return;
    }

    ConversationMessage *entry = m_ledger.findToolCallByUseId(call.toolCallId);
    if (!entry) {
        ConversationMessage newEntry;
        newEntry.kind = ConversationMessage::Kind::ToolCall;
        newEntry.status = ConversationMessage::Status::Queued;
        newEntry.toolName = call.toolName;
        newEntry.toolUseId = call.toolCallId;
        newEntry.toolInput = call.arguments;
        newEntry.groupKey = call.toolName;
        newEntry.turnId = m_activeTurnId;
        newEntry.responseId = call.base.messageId;
        newEntry.submittedToModel = true;
        newEntry.toolCall.id = call.toolCallId;
        newEntry.toolCall.toolName = call.toolName;
        newEntry.toolCall.input = call.arguments;
        newEntry.toolCall.rawInputJson = call.rawArguments;
        newEntry.text = BuiltinToolRuntime::summarizeToolCall(newEntry.toolCall);
        newEntry.summaryText = newEntry.text;
        const QString entryId = appendLedgerEntry(newEntry);
        recordCurrentResponseEntryId(entryId);
        entry = m_ledger.findById(entryId);
    } else {
        entry->toolInput = call.arguments;
        entry->toolCall.id = call.toolCallId;
        entry->toolCall.toolName = call.toolName;
        entry->toolCall.input = call.arguments;
        entry->toolCall.rawInputJson = call.rawArguments;
        entry->text = BuiltinToolRuntime::summarizeToolCall(entry->toolCall);
        entry->summaryText = entry->text;
        entry->responseId = call.base.messageId;
    }

    LOGI(LogCat::Tool, logContext()) << "工具摘要"
        << logf("summary", entry->summaryText);

    if (!entry->toolCall.id.isEmpty()) {
        bool exists = false;
        for (const auto &tc : m_pendingToolCalls) {
            if (tc.id == entry->toolCall.id) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            m_pendingToolCalls.append(entry->toolCall);
            LOGD(LogCat::Tool, logContext()) << "工具入队"
                << logf("tag", QStringLiteral("tool-queue"))
                << logf("loop", ptrTag(this))
                << logf("agent", m_agentId)
        
                << logf("toolId", call.toolCallId)
                << logf("tool", call.toolName)
                << logf("queueAfter", m_pendingToolCalls.size());
            notifyDataChanged();
        }
    }

    // 参数齐了立刻落线路；勿只等 MessageCompleted（Chat Completions 常靠空 fallback 填 outputItems）。
    m_ledger.setProviderItemForEntry(
        entry->id, ProviderItem::makeFunctionCall(entry->toolCall), m_providerContinuationId);

    // ProtocolEvent: 工具调用参数完整接收
    emitProtocolEvent(core_ir::EventItemCompleted{
        call.toolCallId,
        m_activeTurnId,
        core_ir::ItemKind::FunctionCall,
        QDateTime::currentMSecsSinceEpoch(),
        ToolCall{call.toolCallId, call.toolName, call.arguments, call.rawArguments},
        std::nullopt,
        entry->summaryText
    });
}

void AbstractLoop::handleMessageCompleted(const ProviderEvent &event)
{
    const auto &message = event.messageEnd;
    QString completedError;
    if (!message.validate(&completedError, true, -1)) {
        failTurn(QStringLiteral("Provider 完成态无效：%1").arg(completedError));
        return;
    }
    stopModelResponseWatchdog();
    m_providerRequestActive = false;
    m_currentResponseId = message.messageId;

    for (const ProviderItem &item : message.outputItems) {
        switch (item.kind) {
        case ProviderItemKind::Reasoning: {
            // 工具开始时 finalizeStreamingAssistant 已清掉 streaming id；
            // 若完成态再 ensure 会新建一条空推理（日志里 tools=3 reasoning=2）。
            // 优先复用本 turn 最近一条推理条目，空完成态则跳过。
            ConversationMessage *entry = nullptr;
            if (!m_streamingReasoningEntryId.isEmpty())
                entry = m_ledger.findById(m_streamingReasoningEntryId);
            if (!entry)
                entry = m_ledger.findLatestReasoningForTurn(m_activeTurnId);
            const bool itemReplayable = item.reasoningRedacted
                || !item.reasoningText.trimmed().isEmpty()
                || !item.reasoningSignature.trimmed().isEmpty();
            if (!entry) {
                if (!itemReplayable)
                    break;
                entry = ensureStreamingReasoningEntry();
            }
            if (!entry)
                break;
            if (entry->reasoningContent.isEmpty())
                entry->reasoningContent = item.reasoningText;
            if (!item.reasoningSignature.trimmed().isEmpty()
                || entry->reasoningSignature.trimmed().isEmpty()) {
                entry->reasoningSignature = item.reasoningSignature;
            }
            entry->reasoningRedacted = item.reasoningRedacted || entry->reasoningRedacted;
            entry->reasoningMustReplay =
                item.reasoningMustReplay || entry->reasoningMustReplay;
            entry->providerContinuationId = m_providerContinuationId;
            m_streamingReasoningEntryId = entry->id;
            m_ledger.setProviderItemForEntry(
                entry->id, item, m_providerContinuationId);
            break;
        }
        case ProviderItemKind::AssistantMessage: {
            ConversationMessage *entry = ensureStreamingAssistantEntry();
            if (!entry)
                break;
            if (entry->text.isEmpty())
                entry->text = joinedText(item.parts);
            for (const ProviderMessagePart &part : item.parts) {
                if (part.kind == ProviderPartKind::Image
                    && !entry->imageOutput.hasUri()
                    && !entry->imageOutput.hasInlineData()) {
                    entry->imageOutput = part.image;
                    break;
                }
            }
            entry->providerLogprobs = message.logprobs;
            entry->providerContinuationId = m_providerContinuationId;
            m_ledger.setProviderItemForEntry(
                entry->id, item, m_providerContinuationId);
            break;
        }
        case ProviderItemKind::FunctionCall: {
            ConversationMessage *entry = m_ledger.findToolCallByUseId(item.callId);
            if (!entry) {
                ProviderToolCallEnd call;
                call.base.messageId = message.messageId;
                call.toolCallId = item.callId;
                call.toolName = item.name;
                call.arguments = item.arguments;
                call.rawArguments = item.rawArguments;
                handleToolCallCompleted(ProviderEvent::toolCallCompleted(call));
                entry = m_ledger.findToolCallByUseId(item.callId);
            }
            if (!entry)
                break;
            entry->providerContinuationId = m_providerContinuationId;
            entry->toolCall.callerType = toString(item.callerKind);
            entry->toolCall.callerId = item.callerId;
            m_ledger.setProviderItemForEntry(
                entry->id, item, m_providerContinuationId);
            break;
        }
        default: {
            const QString entryId = m_ledger.appendProviderItem(
                item, m_activeTurnId, m_providerContinuationId);
            if (const ConversationMessage *entry = m_ledger.findById(entryId))
                emitProtocolEvent(core_ir::EventMessageAppended{*entry});
            break;
        }
        }
    }
    finalizeStreamingAssistant();

    if (m_pendingToolCalls.isEmpty()) {
        LOGD(LogCat::Provider, logContext()) << "消息完成（无工具调用）"
            << logf("id", message.messageId)
            << logf("queueWasEmpty", true);

        emitProtocolEvent(core_ir::EventItemCompleted{
            message.messageId,
            m_activeTurnId,
            core_ir::ItemKind::Message,
            QDateTime::currentMSecsSinceEpoch(),
            std::nullopt,
            std::nullopt
        });

        if (runModePolicyCheckpoint()) {
            return;
        }

        // 成功收口前：残留引导作废（A1）；next_turn 自动开新轮
        if (!m_steerQueue.isEmpty()) {
            m_steerQueue.clear();
            notifyDataChanged();
        }
        if (!m_nextTurnQueue.isEmpty() && m_activeConfig) {
            // 先 Idle 收口，让 Host 见 busy=false 关上一轮；再 start 开下一轮。
            // 否则 Host turnOpen 仍真，EventTurnStarted 被去重，UI 轮次/时长错乱。
            const SessionRuntime nextCfg = nextQueuedConfig();
            resetLoopState();
            setPhase(Phase::Completed);
            emit turnSucceeded();
            start(nextCfg);
            return;
        }

        // 静默失败提示：模型在工具执行后结束回合且本轮无文本输出 →
        // 追加一条 SessionEvent 提示（UI 显示弱色行，用户可区分「故意停」vs「跑偏」）
        {
            // 「本 turn」边界：自最后一个 Kind::User 条目以来是否有 ToolResult
            bool hasToolResultThisTurn = false;
            const QList<ConversationMessage> &ledger = m_ledger.entries();
            for (int i = ledger.size() - 1; i >= 0; --i) {
                const ConversationMessage &e = ledger.at(i);
                if (e.kind == ConversationMessage::Kind::UserText)
                    break;
                if (e.kind == ConversationMessage::Kind::ToolResult) {
                    hasToolResultThisTurn = true;
                    break;
                }
            }
            // 本轮模型响应无文本（扫描完成态 outputItems 的 AssistantMessage parts 是否有文本）
            bool hasAssistantText = false;
            for (const ProviderItem &item : message.outputItems) {
                if (item.kind != ProviderItemKind::AssistantMessage)
                    continue;
                for (const ProviderMessagePart &part : item.parts) {
                    if (!part.text.trimmed().isEmpty()) {
                        hasAssistantText = true;
                        break;
                    }
                }
                if (hasAssistantText)
                    break;
            }
            if (hasToolResultThisTurn && !hasAssistantText) {
                const int maxSteps = m_activeConfig ? m_activeConfig->maxInternalSteps : 100;
                const QString notice = (m_internalStepCount >= maxSteps)
                    ? QStringLiteral("已达步数上限，回合强制结束。")
                    : QStringLiteral("模型在工具执行后结束回合，未输出说明。");
                ConversationMessage noticeEntry;
                noticeEntry.kind = ConversationMessage::Kind::SessionEvent;
                noticeEntry.status = ConversationMessage::Status::Completed;
                noticeEntry.text = notice;
                noticeEntry.turnId = m_activeTurnId;
                appendLedgerEntry(noticeEntry);
            }
        }
        resetLoopState();
        setPhase(Phase::Completed);
        emit turnSucceeded();
        return;
    }

    emitProtocolEvent(core_ir::EventItemCompleted{
        message.messageId,
        m_activeTurnId,
        core_ir::ItemKind::Message,
        QDateTime::currentMSecsSinceEpoch(),
        std::nullopt,
        std::nullopt
    });

    LOGI(LogCat::Tool, logContext()) << "消息完成，模型发起了工具调用请求"
        << logf("count", m_pendingToolCalls.size())
        << logf("id", message.messageId)
        << logf("queueNotEmpty", true);
    processReadyToolCalls();
}

void AbstractLoop::handleError(const ProviderEvent &event)
{
    const auto &error = event.error;
    const QString failedTurnId = m_activeTurnId;
    const QString rawMessage =
        error.message.isEmpty() ? QStringLiteral("Provider 请求失败。") : error.message;
    // 常见厂商协议错误：补一句可读说明（不改 code，保留原文）
    // 含 "tool_calls" 的消息必然也含 "tool" 子串，只判前者即可。
    QString displayMessage =
        rawMessage.contains(QStringLiteral("tool_calls"), Qt::CaseInsensitive)
            ? QStringLiteral("模型历史中的工具调用与结果未按协议配对，请重试或切换模型。"
                             "\n原始错误：%1")
                  .arg(rawMessage)
            : rawMessage;
    if (error.attempts > 0) {
        displayMessage = QStringLiteral("%1（已自动重试 %2 次）")
                             .arg(displayMessage)
                             .arg(error.attempts);
    }
    LOGE(LogCat::Provider, logContext()) << "Provider 错误"
        << logf("msg", rawMessage)
        << logf("code", error.code)
        << logf("attempts", error.attempts)
        << logf("providerType", m_activeConfig ? m_activeConfig->providerType : QString())
        << logf("model", m_activeConfig ? m_activeConfig->modelName : QString());
    stopModelResponseWatchdog();
    m_providerRequestActive = false;
    finalizeAndDiscardTurn(failedTurnId);
    failTurn(displayMessage);

    emitProtocolEvent(core_ir::EventError{m_agentId, displayMessage});
}

void AbstractLoop::handleCancelled(const ProviderEvent &event)
{
    Q_UNUSED(event);
    // cancel() 可能已经本地收口；迟到的 Cancelled 事件直接忽略，避免二次重置。
    if (m_mode != LoopMode::Busy && !m_providerRequestActive) {
        return;
    }
    const QString canceledTurnId = m_activeTurnId;
    LOGI(LogCat::Provider, logContext()) << "Provider 请求已取消";
    stopModelResponseWatchdog();
    // 先清标志再收尾，避免 finalize/discard 过程中迟到的 Cancelled 再次进入本函数。
    m_providerRequestActive = false;
    finalizeAndDiscardTurn(canceledTurnId);
    resetLoopState();
    setPhase(Phase::Canceled);
    // Canceled 状态已通过 setPhase→notifyStateChanged→EventAgentStateChanged 推送
}

// ── 内部状态钩子 ──

void AbstractLoop::onPhaseChanged(AbstractLoop::Phase newPhase)
{
    if (newPhase == Phase::Completed || newPhase == Phase::Failed
        || newPhase == Phase::Canceled || newPhase == Phase::Idle) {
        m_activeTurnId.clear();
    }
}

void AbstractLoop::onWatchdogTimeout()
{
    if (m_mode != LoopMode::Busy) {
        return;
    }

    // 看门狗只收口无响应：瞬时错误重试已在 Provider 层，到此 = 预算耗尽或非重试性无响应
    LOGW(LogCat::Provider, logContext()) << "模型响应看门狗超时";
    const QString failedTurnId = m_activeTurnId;
    finalizeAndDiscardTurn(failedTurnId);
    failTurn(QStringLiteral("模型响应超时。"));
}

void AbstractLoop::armModelResponseWatchdog()
{
    m_modelResponseWatchdogTimer.start(m_activeConfig->modelResponseTimeoutSecs * 1000);
}
void AbstractLoop::stopModelResponseWatchdog()
{
    m_modelResponseWatchdogTimer.stop(); 
}
