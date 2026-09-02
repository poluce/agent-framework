#include "Agent.h"
#include "agent/AbstractOrchestration.h"
#include "agent/AgentSession.h"
#include "agent/compact/ModelViewAssembler.h"
#include "config/SystemPromptBuilder.h"
#include "tools/ToolCoordinator.h"
#include "providers/service/ProviderService.h"
#include "logging/LogManager.h"
#include "providers/core/AbstractProvider.h"
#include "providers/service/ProviderCredential.h"

#include <QDateTime>
#include <QDir>
#include <QUuid>

#include <algorithm>
#include <utility>

Agent::Agent(const QString &agentId,
             const QString &displayName,
             const SessionRuntime &runtime,
             QObject *parent)
    : QObject(parent)
    , m_agentId(agentId)
    , m_displayName(displayName)
    , m_runtime(runtime)
    , m_providerFactory(nullptr)
    , m_loop(std::make_unique<AbstractLoop>(this))
    , m_compactEngine(std::make_unique<CompactEngine>(this))
    , m_taskManager(std::make_unique<AgentTaskManager>(this))
{
    Q_ASSERT_X(!m_runtime.workingDirectory.trimmed().isEmpty(),
               "Agent::Agent", "workingDirectory 不能为空，调用方必须提供有效路径");
    Q_ASSERT_X(!m_runtime.systemPrompt.trimmed().isEmpty(),
               "Agent::Agent", "systemPrompt 不能为空，调用方必须提供有效提示词");

    // 转发配置到 loop
    m_loop->setAgentInfo(m_agentId, m_displayName);

    // 连接 loop 信号 — 替代回调注入，生命周期内始终有效
    connect(m_loop.get(), &AbstractLoop::stateChanged, this,
            &Agent::handleLoopStateChanged);
    connect(m_loop.get(), &AbstractLoop::dataChanged, this,
            &Agent::handleLoopDataChanged);
    connect(m_taskManager.get(), &AgentTaskManager::stateChanged, this,
            &Agent::stateChanged);

    // 注册为 AbstractLoop 内环事件消费者，透传 Event+Context+SubmissionId。
    // Loop 的 EventAgentStateChanged.status 保持默认 Idle；此处填入 Agent 派生 status。
    m_loop->addEventHandler([this](const core_ir::Event &event,
                                   const core_ir::EventContext &context,
                                   const core_ir::SubmissionId &submissionId) {
        auto emitToHandlers = [this](const core_ir::Event &out,
                                     const core_ir::EventContext &ctx,
                                     const core_ir::SubmissionId &sid) {
            for (auto &handler : m_protocolHandlers)
                handler(out, ctx, sid);
        };

        if (const auto *state = std::get_if<core_ir::EventAgentStateChanged>(&event); state) {
            core_ir::EventAgentStateChanged patched = *state;
            // Loop 不持 Agent 派生态（Queued 等）；一律用 Agent::status() 覆盖默认 Idle。
            patched.status = status();
            // 段摘要进度只在 Agent 侧可知（Loop 不持摘要游标）
            patched.segmentSummaryAddedTokens = segmentSummaryAddedTokens();
            emitToHandlers(core_ir::Event{std::move(patched)}, context, submissionId);
            return;
        }
        emitToHandlers(event, context, submissionId);
    });

    // 连接压缩请求
    connect(m_loop.get(), &AbstractLoop::compactionRequested, this,
            &Agent::onCompactionRequested);
    connect(m_loop.get(), &AbstractLoop::turnSucceeded, this,
            &Agent::onTurnSucceededForSummary);
    connect(m_compactEngine.get(), &CompactEngine::compactionFinished, this,
            &Agent::onCompactionFinished);
    connect(m_compactEngine.get(), &CompactEngine::compactionFailed, this,
            &Agent::onCompactionFailed);

    // CompactEngine → 内环 Event fan-out
    m_compactEngine->addProtocolHandler([this](const core_ir::Event &event,
                                               const core_ir::EventContext &context,
                                               const core_ir::SubmissionId &submissionId) {
        for (auto &handler : m_protocolHandlers)
            handler(event, context, submissionId);
    });

    LOGD(LogCat::Agent) << "创建 Agent"
        << logf("agentId", m_agentId)
        << logf("display", m_displayName);
}

Agent::~Agent()
{
    clearSummaryState();
    clearInbox(QStringLiteral("agent_destroyed"));
    for (auto &handler : m_protocolHandlers) {
        handler(core_ir::Event{core_ir::EventShutdownComplete{}}, {}, {});
    }
}

// ── 标识 ──

QString Agent::agentId() const { return m_agentId; }
QString Agent::displayName() const { return m_displayName; }
QString Agent::parentAgentId() const { return m_parentAgentId; }
void Agent::setParentAgentId(const QString &parentAgentId)
{
    if (m_parentAgentId == parentAgentId) {
        return;
    }
    m_parentAgentId = parentAgentId;
    m_loop->setAgentInfo(m_agentId, m_displayName, m_parentAgentId);
    emit stateChanged();
}

// ── Provider 配置 ──

ToolScope Agent::toolScope() const { return m_runtime.toolScope; }
ApprovalMode Agent::approvalMode() const { return m_runtime.approvalMode; }

void Agent::applySessionSettings(const SessionRuntime &settings)
{
    m_runtime = settings;
    const QVariantMap inst = m_credentialStore ? m_credentialStore->getInstance(m_runtime.credentialInstanceId)
                                               : QVariantMap{};
    if (!inst.isEmpty()) {
        m_runtime.providerType = ProviderService::normalizeProviderType(
            inst.value(QStringLiteral("providerType")).toString());
    }
    LOGD(LogCat::Config) << "Agent 应用配置"
        << logf("agentId", m_agentId)
        << logf("model", m_runtime.modelName)
        << logf("instance", m_runtime.credentialInstanceId)
        << logf("workDir", m_runtime.workingDirectory);
    if (m_loop) {
        m_loop->applyRuntimeConfig(m_runtime);
    }
}

QString Agent::sessionUuid() const
{
    return m_loop->sessionUuid();
}

void Agent::setSessionUuid(const QString &uuid)
{
    m_loop->setSessionUuid(uuid);
}

AgentTaskManager *Agent::taskManager() const
{
    return m_taskManager.get();
}

void Agent::setSystemPrompt(const QString &systemPrompt)
{
    m_runtime.systemPrompt = systemPrompt.trimmed();
}

void Agent::setModelResponseTimeoutSecs(const int timeoutSecs)
{
    m_runtime.modelResponseTimeoutSecs = qMax(1, timeoutSecs);
}

void Agent::setCoordinator(ToolCoordinator *coordinator)
{
    m_coordinator = coordinator;
    if (m_loop) {
        m_loop->setCoordinator(coordinator);
    }
    ensureSegmentSummaryPipeline();
}

AbstractOrchestration *Agent::orchestration() const
{
    return m_coordinator && m_coordinator->session()
        ? m_coordinator->session()->orchestration()
        : nullptr;
}

void Agent::ensureSegmentSummaryPipeline()
{
    if (m_summaryQueue) {
        return;
    }
    AbstractOrchestration *orch = orchestration();
    if (!orch || !orch->usesSegmentSummary(this)) {
        return;
    }
    m_summaryQueue = std::make_unique<SummaryJobQueue>(this);
    m_summaryQueue->setCompactEngine(m_compactEngine.get());
    connect(m_summaryQueue.get(), &SummaryJobQueue::jobFinished, this,
            &Agent::onSummaryJobFinished);
    connect(m_summaryQueue.get(), &SummaryJobQueue::queueDrained, this,
            &Agent::resumeBoundaryAfterSummaryDrain);
}

bool Agent::remainsIdleAfterTurn() const
{
    AbstractOrchestration *orch = orchestration();
    if (!orch) {
        return true;
    }
    return orch->remainsIdleAfterTurn(this);
}

void Agent::setProviderFactory(ProviderFactory factory)
{
    m_providerFactory = std::move(factory);
    m_loop->setProviderFactory(m_providerFactory);
}

void Agent::setPromptBuilder(SystemPromptBuilder *builder)
{
    m_promptBuilder = builder;
    m_loop->setPromptBuilder(builder);
    if (m_compactEngine) {
        m_compactEngine->setPromptBuilder(builder);
    }
}

void Agent::setModePolicyFactory(AbstractLoop::ModePolicyFactory factory)
{
    m_loop->setModePolicyFactory(std::move(factory));
}

void Agent::setCredentialStore(ProviderCredential *credentialStore)
{
    m_credentialStore = credentialStore;
    m_loop->setCredentialStore(credentialStore);
}

void Agent::setToolResultStoreDirectory(const QString &directoryPath)
{
    m_loop->setToolResultStoreDirectory(directoryPath);
}

int Agent::maxInternalSteps() const
{
    return m_runtime.maxInternalSteps;
}

void Agent::setMaxInternalSteps(const int steps)
{
    m_runtime.maxInternalSteps = qBound(1, steps, 100);
}

void Agent::setMaxRetries(const int retries)
{
    m_runtime.maxRetries = qBound(0, retries, 10);
}

QString Agent::defaultShell() const
{
    return m_runtime.defaultShell;
}

void Agent::setDefaultShell(const QString &shell)
{
    const QString normalized = shell.trimmed().toLower();
    m_runtime.defaultShell = (normalized == QStringLiteral("powershell")) ? QStringLiteral("powershell") : QStringLiteral("bash");
}

// ── 状态查询 ──

core_ir::AgentPhase Agent::currentPhase() const
{
    return m_loop ? m_loop->phase() : core_ir::AgentPhase::Idle;
}

QString Agent::lastError() const { return m_loop->lastError(); }
QString Agent::systemPrompt() const { return m_runtime.systemPrompt; }
bool Agent::busy() const { return m_loop->isStreaming(); }
bool Agent::hasPendingApproval() const { return m_loop->hasPendingApproval(); }

bool Agent::hasPendingQuestion() const { return m_loop->hasPendingQuestion(); }
int Agent::pendingQuestionCount() const { return m_loop->pendingQuestionCount(); }
QString Agent::pendingQuestionIdAt(int index) const { return m_loop->pendingQuestionIdAt(index); }
QString Agent::pendingQuestionTextAt(int index) const { return m_loop->pendingQuestionTextAt(index); }
QStringList Agent::pendingQuestionOptionsAt(int index) const { return m_loop->pendingQuestionOptionsAt(index); }
bool Agent::pendingQuestionIsMultiSelectAt(int index) const { return m_loop->pendingQuestionIsMultiSelectAt(index); }
QString Agent::pendingApprovalSummary() const { return m_loop->pendingApprovalRequest().summary; }

QString Agent::statusToString(const AgentStatus s)
{
    return core_ir::agentStatusKey(s);
}

Agent::AgentStatus Agent::status() const
{
    // 派生优先级（Running 只来自 Loop 忙闲，禁止回落粘滞 m_status==Running）：
    // 1) 管理排队 2) Loop Busy 3) 轮次硬终态 phase 4) 管理取消 5) Idle
    if (m_status == AgentStatus::Queued) {
        return AgentStatus::Queued;
    }
    if (m_loop->isBusy()) {
        return AgentStatus::Running;
    }
    switch (currentPhase()) {
    case core_ir::AgentPhase::Failed:
        return AgentStatus::Failed;
    case core_ir::AgentPhase::Canceled:
        return AgentStatus::Canceled;
    case core_ir::AgentPhase::Completed:
        return remainsIdleAfterTurn() ? AgentStatus::Idle : AgentStatus::Completed;
    default:
        break;
    }
    if (m_status == AgentStatus::Canceled) {
        return AgentStatus::Canceled;
    }
    return AgentStatus::Idle;
}

QString Agent::latestSummary() const { return deriveLatestSummary(m_loop->messages()); }
qint64 Agent::currentContextTokenEstimate() const { return m_loop->currentContextTokenEstimate(); }

qint64 Agent::segmentSummaryAddedTokens() const
{
    if (!summaryFeaturesEnabled() || !m_loop) {
        return 0;
    }
    return ModelViewAssembler::estimateTokensSince(m_loop->ledger(), segmentSummaryCursor());
}

QString Agent::workingDirectory() const { return m_runtime.workingDirectory; }
void Agent::setManagerStatus(const AgentStatus status)
{
    // 会话排队/停止等管理器写入；不经 AbstractLoop，在此直接发协议事件（含 status）。
    if (m_status == status)
        return;
    m_status = status;
    emitAgentStateProtocolEvent();
    emit stateChanged();
}

void Agent::emitAgentStateProtocolEvent()
{
    // canSubmit 由 Host runState 投影覆盖；Core 只报审批/提问阻塞提示
    const core_ir::EventAgentStateChanged payload{
        m_agentId,
        busy(),
        currentPhase(),
        !hasPendingApproval() && !hasPendingQuestion(),
        hasPendingApproval(),
        pendingApprovalSummary(),
        lastError(),
        currentContextTokenEstimate(),
        status(),
        hasPendingQuestion(),
        pendingQuestionCount(),
        m_loop ? m_loop->pendingQuestionSnapshot() : QList<core_ir::PendingQuestion>{},
        pendingNextTurnCount(),
        pendingNextTurnPreviews(),
        segmentSummaryAddedTokens()
    };
    for (auto &handler : m_protocolHandlers) {
        handler(core_ir::Event{payload}, {}, {});
    }
}

void Agent::emitInboxEnqueued(const AgentInboxMessage &msg)
{
    const core_ir::EventInboxMessageEnqueued payload{
        msg.id, msg.fromAgentId, m_agentId, msg.priority
    };
    for (auto &handler : m_protocolHandlers) {
        handler(core_ir::Event{payload}, {}, {});
    }
}

void Agent::emitInboxDelivered(const AgentInboxMessage &msg)
{
    const core_ir::EventInboxMessageDelivered payload{
        msg.id, msg.fromAgentId, m_agentId
    };
    for (auto &handler : m_protocolHandlers) {
        handler(core_ir::Event{payload}, {}, {});
    }
}

void Agent::emitInboxDropped(const AgentInboxMessage &msg, const QString &reason)
{
    const core_ir::EventInboxMessageDropped payload{
        msg.id, msg.fromAgentId, m_agentId, reason
    };
    for (auto &handler : m_protocolHandlers) {
        handler(core_ir::Event{payload}, {}, {});
    }
}

// ── 操作 ──

void Agent::submitUserDelivery(const QString &message,
                               const QStringList &attachedFilePaths,
                               AbstractLoop::UserDelivery delivery)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty() && attachedFilePaths.isEmpty()) {
        return;
    }

    LOGD(LogCat::Agent) << "提交用户消息"
        << logf("agentId", m_agentId)
        << logf("delivery", delivery == AbstractLoop::UserDelivery::Steer
                                ? QStringLiteral("steer")
                                : QStringLiteral("next_turn"))
        << logf("busy", m_loop->isBusy())
        << logf("preview", trimmed.left(80));

    if (attachedFilePaths.isEmpty()) {
        m_loop->enqueueUserMessage(trimmed, delivery);
    } else {
        m_loop->enqueueUserMessageWithFiles(trimmed, attachedFilePaths, delivery);
    }

    // Idle + NextTurn：立即开轮；Busy 或 Steer 仅排队
    if (!m_loop->isBusy() && delivery == AbstractLoop::UserDelivery::NextTurn) {
        m_status = AgentStatus::Running;
        m_loop->start(m_runtime);
    }
}

bool Agent::confirmPendingNextTurns()
{
    if (!m_loop || m_loop->pendingNextTurnCount() <= 0) {
        return false;
    }
    m_status = AgentStatus::Running;
    return m_loop->confirmPendingNextTurns(m_runtime);
}

void Agent::discardPendingNextTurns()
{
    if (m_loop) {
        m_loop->discardPendingNextTurns();
    }
}

int Agent::pendingNextTurnCount() const
{
    return m_loop ? m_loop->pendingNextTurnCount() : 0;
}

QStringList Agent::pendingNextTurnPreviews(const int maxItems) const
{
    return m_loop ? m_loop->pendingNextTurnPreviews(maxItems) : QStringList{};
}

bool Agent::prefersSteerDelivery() const
{
    return m_loop && m_loop->prefersSteerDelivery();
}

void Agent::submitAgentTask(const QString &message)
{
    submitMessageInternal(message, ConversationMessage::Kind::AgentTask, QStringLiteral("接收代理任务:"));
}

void Agent::submitMessageInternal(const QString &message, ConversationMessage::Kind kind, const QString &logLabel)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    LOGD(LogCat::Agent) << logLabel
        << logf("agentId", m_agentId)
        << logf("preview", trimmed.left(80));
    m_status = AgentStatus::Running;
    m_loop->enqueueMessage(trimmed, kind, {}, AbstractLoop::UserDelivery::NextTurn);
    m_loop->start(m_runtime);
}

bool Agent::canRetryFailedMessage() const
{
    return m_loop && m_loop->canRetryLastFailedTurn();
}

bool Agent::retryFailedMessage()
{
    if (m_loop->isBusy()) {
        LOGD(LogCat::Agent) << "重试被忽略：Loop 正在运行"
            << logf("agentId", m_agentId);
        return false;
    }
    if (!m_loop->canRetryLastFailedTurn()) {
        LOGD(LogCat::Agent) << "重试被忽略：没有可重放的失败轮"
            << logf("agentId", m_agentId);
        return false;
    }
    LOGI(LogCat::Agent) << "重试上一失败轮"
        << logf("agentId", m_agentId);
    m_status = AgentStatus::Running;
    return m_loop->retryLastFailedTurn(m_runtime);
}

void Agent::cancelCurrentTurn()
{
    LOGI(LogCat::Agent) << "取消当前 Turn"
        << logf("agentId", m_agentId)
        << logf("boundaryWait", m_waitingSummaryAtBoundary);

    // G4a：边界等摘要 — abort 在飞、保留队列、结束等待
    if (m_waitingSummaryAtBoundary) {
        m_waitingSummaryAtBoundary = false;
        if (m_summaryQueue) {
            m_summaryQueue->abortRunning();
        }
        m_loop->endBoundarySummaryWait(true);
        handleLoopStateChanged();
        return;
    }

    // G4b / 途中：先 abort 段摘要（保留队列），再停大压。
    // 禁止对 summaryOnly 直接 cancel（否则 job 会被误标 Failed）。
    if (m_summaryQueue && m_summaryQueue->hasRunning()) {
        m_summaryQueue->abortRunning();
    }
    m_loop->cancel();
    if (m_compactEngine->isRunning()) {
        m_compactEngine->cancel();
    }
    m_manualCompaction = false;
    handleLoopStateChanged();
}

void Agent::approvePendingAction()
{
    m_loop->approvePendingToolCall(true);
    handleLoopStateChanged();
}

void Agent::rejectPendingAction()
{
    m_loop->approvePendingToolCall(false);
    handleLoopStateChanged();
}

void Agent::submitQuestionAnswer(const int questionIndex, const QString &answer)
{
    m_loop->submitQuestionAnswer(questionIndex, answer);
    handleLoopStateChanged();
}

void Agent::appendSessionEvent(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return;
    }

    ConversationMessage message;
    message.kind = ConversationMessage::Kind::SessionEvent;
    message.status = ConversationMessage::Status::Completed;
    message.text = text.trimmed();
    m_loop->appendExternalMessage(message);
    emit stateChanged();
    emit dataChanged();

    // ProtocolEvent
    for (auto &handler : m_protocolHandlers) {
        handler(core_ir::Event{core_ir::EventSessionEvent{m_agentId, text.trimmed()}}, {}, {});
    }
}

void Agent::submitUserMessageWithSkill(const QString &message,
                                        const QStringList &filePaths,
                                        const QString &skillName,
                                        const QString &skillBody)
{
    LOGD(LogCat::Agent) << "提交用户消息（技能）"
        << logf("agentId", m_agentId)
        << logf("skill", skillName)
        << logf("preview", message.left(80));
    // 技能固定 next_turn；Idle 开轮，Busy 仅入队（门禁 SubmitSkill 仍 Idle-only）
    m_loop->enqueueUserMessageWithSkill(message.trimmed(), filePaths, skillName, skillBody,
                                        AbstractLoop::UserDelivery::NextTurn);
    if (!m_loop->isBusy()) {
        m_status = AgentStatus::Running;
        m_loop->start(m_runtime);
    }
}

bool Agent::enqueueInboxMessage(const AgentInboxMessage &msg)
{
    if (m_runtime.maxInboxMessages > 0) {
        int active = 0;
        for (const auto &m : m_inbox) {
            if (!m.acked) {
                ++active;
            }
        }
        if (active >= m_runtime.maxInboxMessages) {
            LOGW(LogCat::Agent) << "收件箱拒绝：容量超限"
                << logf("agentId", m_agentId)
                << logf("id", msg.id)
                << logf("from", msg.fromAgentId)
                << logf("limit", m_runtime.maxInboxMessages);
            emitInboxDropped(msg, QStringLiteral("capacity"));
            return false;
        }
    }
    if (m_runtime.maxInboxMessageSize > 0
        && msg.content.size() > m_runtime.maxInboxMessageSize) {
        LOGW(LogCat::Agent) << "收件箱拒绝：单条消息超限"
            << logf("agentId", m_agentId)
            << logf("id", msg.id)
            << logf("from", msg.fromAgentId)
            << logf("size", msg.content.size())
            << logf("limit", m_runtime.maxInboxMessageSize);
        emitInboxDropped(msg, QStringLiteral("size"));
        return false;
    }
    m_inbox.append(msg);
    LOGD(LogCat::Agent) << "收件箱消息入队"
        << logf("from", msg.fromAgentId)
        << logf("id", msg.id)
        << logf("preview", msg.content.left(80));
    emitInboxEnqueued(msg);
    emit stateChanged();
    return true;
}

bool Agent::hasPendingInboxMessages() const
{
    for (const auto &msg : m_inbox) {
        if (!msg.acked && !msg.inFlight) {
            return true;
        }
    }
    return false;
}

QList<AgentInboxMessage> Agent::takePendingInboxMessages()
{
    QList<AgentInboxMessage> pending;
    for (AgentInboxMessage &msg : m_inbox) {
        if (msg.acked || msg.inFlight) {
            continue;
        }
        msg.inFlight = true;
        pending.append(msg);
    }
    std::stable_sort(pending.begin(), pending.end(),
        [](const AgentInboxMessage &a, const AgentInboxMessage &b) {
            if (a.priority != b.priority) {
                return a.priority > b.priority;
            }
            return a.timestamp < b.timestamp;
        });
    return pending;
}

void Agent::ackInboxMessages(const QStringList &ids)
{
    for (const QString &id : ids) {
        for (auto it = m_inbox.begin(); it != m_inbox.end(); ++it) {
            if (it->id != id) {
                continue;
            }
            AgentInboxMessage msg = *it;
            msg.acked = true;
            m_inbox.erase(it);
            emitInboxDelivered(msg);
            break;
        }
    }
}

void Agent::requeueInboxMessages(const QStringList &ids)
{
    for (const QString &id : ids) {
        for (AgentInboxMessage &msg : m_inbox) {
            if (msg.id == id && msg.inFlight) {
                msg.inFlight = false;
                break;
            }
        }
    }
}

void Agent::clearInbox(const QString &reason)
{
    if (m_inbox.isEmpty()) {
        return;
    }
    for (const AgentInboxMessage &msg : std::as_const(m_inbox)) {
        if (!msg.acked) {
            emitInboxDropped(msg, reason);
        }
    }
    m_inbox.clear();
}

AbstractLoop *Agent::loop() const
{
    return m_loop.get();
}

// ── 消息直接访问 ──

QList<ConversationMessage> Agent::ledgerMessages() const
{
    return m_loop ? m_loop->messages() : QList<ConversationMessage>{};
}

// ── 内环事件 fan-out ──

core_ir::HandlerId Agent::addEventHandler(core_ir::EventHandler handler)
{
    m_protocolHandlers.push_back(std::move(handler));
    return reinterpret_cast<core_ir::HandlerId>(m_protocolHandlers.size());
}

void Agent::removeEventHandler(core_ir::HandlerId id)
{
    Q_UNUSED(id);
    // 本对象只挂会话转发这一个 handler；remove 即清空。
    m_protocolHandlers.clear();
}

// ── 内部 ──

void Agent::handleLoopStateChanged()
{
    // Loop 回 Idle 时收敛粘滞 Running（提交路径写入的旁注）；Queued/Canceled 管理态保留。
    if (!m_loop->isBusy()
        && m_status != AgentStatus::Queued
        && m_status != AgentStatus::Canceled
        && m_status != AgentStatus::Idle) {
        m_status = AgentStatus::Idle;
    }
    // Loop 已发 EventAgentStateChanged（Agent 转发时补 status）；此处只驱动会话内 Qt 协调。
    // 勿再叠 dataChanged，否则 Session 会二次 push 同一状态。
    emit stateChanged();
}

void Agent::handleLoopDataChanged()
{
    emit dataChanged();
}

void Agent::onCompactionRequested(const qint64 currentTokens, const qint64 threshold)
{
    LOGI(LogCat::Agent) << "收到压缩请求"
        << logf("agentId", m_agentId)
        << logf("currentTokens", currentTokens)
        << logf("threshold", threshold);

    m_manualCompaction = false;
    m_boundaryThreshold = threshold;

    if (!m_summaryQueue) {
        startCompactionEngine();
        return;
    }

    onBoundaryCompactionRequested(threshold);
}

void Agent::onBoundaryCompactionRequested(const qint64 threshold)
{
    m_boundaryThreshold = threshold;

    if (!summaryFeaturesEnabled()) {
        clearSummaryQueueForBulk();
        startCompactionEngine();
        return;
    }

    // 队列有未完成任务 → 等排空（D16）
    if (m_summaryQueue->hasPendingOrRunning()) {
        LOGI(LogCat::Agent) << "边界等待段摘要队列"
            << logf("agentId", m_agentId)
            << logf("jobs", m_summaryQueue->jobCount());
        m_waitingSummaryAtBoundary = true;
        m_loop->beginBoundarySummaryWait();
        configureAndKickSummaryQueue();
        return;
    }

    if (tryContinueWithAssembledView(threshold, true)) {
        return;
    }

    clearSummaryQueueForBulk();
    startCompactionEngine();
}

bool Agent::requestManualCompaction(const qint64 targetTokens)
{
    if (!m_loop || !m_compactEngine) {
        return false;
    }
    if (m_manualCompaction) {
        LOGW(LogCat::Agent) << "手动压缩拒绝：已在手动压缩"
            << logf("agentId", m_agentId);
        return false;
    }

    // G5：先 abort 段摘要并 clear 队列（途中 summaryOnly 占引擎会误拒大压）
    m_waitingSummaryAtBoundary = false;
    if (m_loop->isWaitingBoundarySummary()) {
        m_loop->endBoundarySummaryWait(true);
    }
    clearSummaryQueueForBulk();

    // Host 门禁已限 Idle；清完旁路后再挡真忙 / 真大压重入
    if (m_loop->isBusy() || m_compactEngine->isRunning()) {
        LOGW(LogCat::Agent) << "手动压缩拒绝：忙"
            << logf("agentId", m_agentId)
            << logf("loopBusy", m_loop->isBusy())
            << logf("engineRunning", m_compactEngine->isRunning());
        return false;
    }

    LOGI(LogCat::Agent) << "手动压缩开始"
        << logf("agentId", m_agentId)
        << logf("targetTokens", targetTokens);

    m_manualCompaction = true;
    m_loop->beginManualCompaction();
    startCompactionEngine(targetTokens);
    // 同步跳过会立刻 finished；异步则引擎在跑。两种都算受理。
    return true;
}

void Agent::clearSummaryState()
{
    m_waitingSummaryAtBoundary = false;
    if (m_summaryQueue) {
        m_summaryQueue->clear();
    }
    m_summaryStore.clear();
    m_modelViewStore.clear();
    m_lastSummarizedEntryId.clear();
    m_lastEnqueuedEntryId.clear();
    if (m_loop) {
        m_loop->clearModelViewPrefix();
    }
}

int Agent::segmentSummaryJobCount() const
{
    return m_summaryQueue ? m_summaryQueue->jobCount() : 0;
}

QJsonObject Agent::exportSummaryState() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("summaryStore"), m_summaryStore.toJson());
    obj.insert(QStringLiteral("modelView"), m_modelViewStore.toJson());
    obj.insert(QStringLiteral("lastSummarizedEntryId"), m_lastSummarizedEntryId);
    obj.insert(QStringLiteral("lastEnqueuedEntryId"), m_lastEnqueuedEntryId);
    return obj;
}

void Agent::importSummaryState(const QJsonObject &obj)
{
    if (!m_summaryQueue) {
        clearSummaryState();
        return;
    }
    m_summaryStore.fromJson(obj.value(QStringLiteral("summaryStore")).toObject());
    m_modelViewStore.fromJson(obj.value(QStringLiteral("modelView")).toObject());
    m_lastSummarizedEntryId = obj.value(QStringLiteral("lastSummarizedEntryId")).toString();
    m_lastEnqueuedEntryId = obj.value(QStringLiteral("lastEnqueuedEntryId")).toString();
    // 从摘要库重建前缀，并挂最近用户醒目块（不信任盘上旧 modelView 快照）
    syncModelViewPrefixFromStore();
}

bool Agent::hasFailedSegmentSummaryJobs() const
{
    return m_summaryQueue && m_summaryQueue->hasFailed();
}

void Agent::clearSummaryQueueForBulk()
{
    if (m_summaryQueue) {
        m_summaryQueue->clear();
    }
    m_lastEnqueuedEntryId = m_lastSummarizedEntryId;
}

void Agent::configureAndKickSummaryQueue()
{
    if (!m_summaryQueue || !m_loop) {
        return;
    }
    m_summaryQueue->setProviderContext(
        m_runtime.credentialInstanceId,
        m_credentialStore,
        m_providerFactory,
        m_runtime.modelName,
        m_loop->provider());
    m_summaryQueue->setCompactConfig(m_runtime.toCompactConfig());
    m_summaryQueue->kick();
}

bool Agent::summaryFeaturesEnabled() const
{
    return m_summaryQueue
        && m_runtime.summaryEnabled && m_runtime.compactEnabled;
}

QString Agent::segmentSummaryCursor() const
{
    return !m_lastEnqueuedEntryId.isEmpty()
        ? m_lastEnqueuedEntryId
        : m_lastSummarizedEntryId;
}

bool Agent::tryContinueWithAssembledView(const qint64 threshold, const bool logWhenOver)
{
    if (m_summaryStore.isEmpty() || !m_loop) {
        return false;
    }
    applyAssembledModelView();
    const qint64 estimated = m_loop->currentContextTokenEstimate();
    if (estimated <= threshold) {
        LOGI(LogCat::Agent) << "边界组装成功，继续主模型"
            << logf("tokens", estimated)
            << logf("threshold", threshold);
        m_loop->continueAfterCompaction();
        return true;
    }
    if (logWhenOver) {
        LOGW(LogCat::Agent) << "组装后仍超阈值，改大压"
            << logf("tokens", estimated)
            << logf("threshold", threshold);
    }
    return false;
}

SummaryRecord Agent::makeSummaryRecord(const QString &summaryId,
                                       QList<QString> spanEntryIds,
                                       const QString &text,
                                       const QString &source)
{
    SummaryRecord rec;
    rec.summaryId = summaryId;
    rec.spanEntryIds = std::move(spanEntryIds);
    rec.text = text;
    rec.tokenEstimate = estimateContextTokensForText(text);
    rec.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    rec.source = source;
    return rec;
}

void Agent::startCompactionEngine(const qint64 targetTokensOverride)
{
    m_compactEngine->config = m_runtime.toCompactConfig();
    if (targetTokensOverride > 0) {
        m_compactEngine->config.targetTokenCount = targetTokensOverride;
    }
    m_compactEngine->start(
        &m_loop->ledger(),
        m_runtime.credentialInstanceId,
        m_credentialStore,
        m_providerFactory,
        m_runtime.modelName,
        m_loop->provider()
    );
}

void Agent::onCompactionFinished(const bool success)
{
    if (!success) {
        LOGW(LogCat::Agent) << "压缩未完全成功（降级处理）"
            << logf("agentId", m_agentId)
            << logf("manual", m_manualCompaction);
    }

    // 大压/截断成功：引擎已 markCompacted。D11 仅 leader 写摘要库。
    if (success) {
        const QString bulkText = m_compactEngine->lastBulkSummaryText();
        const QList<QString> bulkIds = m_compactEngine->lastBulkCompactedIds();
        if (m_summaryQueue && !bulkText.trimmed().isEmpty()) {
            m_summaryStore.replaceAll(makeSummaryRecord(
                QUuid::createUuid().toString(QUuid::WithoutBraces),
                bulkIds,
                bulkText,
                QStringLiteral("bulk")));
            if (!m_summaryStore.lastCoveredEntryId().isEmpty()) {
                m_lastSummarizedEntryId = m_summaryStore.lastCoveredEntryId();
                m_lastEnqueuedEntryId = m_lastSummarizedEntryId;
            }
            syncModelViewPrefixFromStore();
            emitContextCompactedNotice(core_ir::CompactReason::Bulk);
        } else {
            // 截断降级或子代理：不 wipe 摘要库
            emitContextCompactedNotice(bulkText.trimmed().isEmpty()
                                           ? core_ir::CompactReason::Truncate
                                           : core_ir::CompactReason::Bulk);
        }
    }

    const bool manual = m_manualCompaction;
    m_manualCompaction = false;
    m_waitingSummaryAtBoundary = false;

    if (manual) {
        m_loop->endManualCompaction();
    }

    emit stateChanged();
    emit dataChanged();

    if (!manual) {
        m_loop->continueAfterCompaction();
    }
}

void Agent::onCompactionFailed(const QString &reason)
{
    LOGW(LogCat::Agent) << "压缩失败"
        << logf("agentId", m_agentId)
        << logf("reason", reason)
        << logf("manual", m_manualCompaction);
}

void Agent::onTurnSucceededForSummary()
{
    if (!m_summaryQueue) {
        return;
    }
    maybeEnqueueSegmentSummary();
    // 主模型成功后 resume 失败/pending 任务（D6）
    if (m_summaryQueue->hasPendingOrRunning()) {
        configureAndKickSummaryQueue();
    }
}

void Agent::maybeEnqueueSegmentSummary()
{
    if (!summaryFeaturesEnabled() || !m_loop) {
        return;
    }
    const qint64 threshold = m_runtime.summarySegmentTokens > 0
        ? m_runtime.summarySegmentTokens
        : 180000;
    const QString afterId = segmentSummaryCursor();
    const qint64 added = ModelViewAssembler::estimateTokensSince(m_loop->ledger(), afterId);
    if (added < threshold) {
        LOGD(LogCat::Agent) << "段摘要未达阈值"
            << logf("added", added)
            << logf("threshold", threshold);
        return;
    }
    const QList<ConversationMessage> snapshot =
        ModelViewAssembler::collectSummarizableSince(m_loop->ledger(), afterId);
    if (snapshot.isEmpty()) {
        return;
    }
    const QList<QString> spanIds = ModelViewAssembler::entryIdsOf(snapshot);
    const QString jobId = m_summaryQueue->enqueue(spanIds, snapshot);
    if (jobId.isEmpty()) {
        return;
    }
    m_lastEnqueuedEntryId = spanIds.last();
    configureAndKickSummaryQueue();
    // 入队后累计起点前移：立刻刷新状态栏进度
    emitAgentStateProtocolEvent();
}

void Agent::onSummaryJobFinished(const QString &jobId, const bool success,
                                 const QString &summaryText,
                                 const QList<QString> &spanEntryIds)
{
    LOGI(LogCat::Agent) << "段摘要任务结束"
        << logf("jobId", jobId)
        << logf("success", success)
        << logf("chars", summaryText.size());

    // 未到边界：Failed 任务保留在队列，不启动大压
    if (!success) {
        return;
    }

    m_summaryStore.append(makeSummaryRecord(
        jobId, spanEntryIds, summaryText, QStringLiteral("segment")));
    if (!spanEntryIds.isEmpty()) {
        m_lastSummarizedEntryId = spanEntryIds.last();
    }
    syncModelViewPrefixFromStore();
    // 段摘要写库本身不 mark 账本；仍发可观测通知
    emitContextCompactedNotice(core_ir::CompactReason::Segment);
    emitAgentStateProtocolEvent();
}

void Agent::resumeBoundaryAfterSummaryDrain()
{
    if (!m_waitingSummaryAtBoundary) {
        return;
    }
    m_waitingSummaryAtBoundary = false;
    m_loop->endBoundarySummaryWait(false);

    if (m_summaryQueue && m_summaryQueue->hasFailed()) {
        LOGW(LogCat::Agent) << "边界摘要有失败，清队大压"
            << logf("agentId", m_agentId);
        clearSummaryQueueForBulk();
        startCompactionEngine();
        return;
    }

    if (tryContinueWithAssembledView(m_boundaryThreshold, false)) {
        return;
    }

    clearSummaryQueueForBulk();
    startCompactionEngine();
}

void Agent::applyAssembledModelView()
{
    if (!m_loop) {
        return;
    }
    const int recentTurns = m_runtime.summaryRecentTurns > 0 ? m_runtime.summaryRecentTurns : 5;
    const ModelViewAssembleResult assembled = ModelViewAssembler::assemble(
        m_loop->ledger(),
        m_summaryStore,
        recentTurns);

    if (!assembled.ok) {
        LOGW(LogCat::Agent) << "组装模型视图失败"
            << logf("reason", assembled.failReason);
        return;
    }

    // 完整记录保留：仅 mark 被摘要覆盖且不在近尾保护内的条目
    if (!assembled.entryIdsToCompact.isEmpty()) {
        m_loop->ledger().markEntriesCompacted(assembled.entryIdsToCompact);
    }

    // 模型短上下文 = ModelViewStore 前缀（请求时注入）；不写 Kind::Summary 进账本
    syncModelViewPrefixFromStore();
    m_loop->refreshContextTokenEstimate();
    emitContextCompactedNotice(core_ir::CompactReason::Assemble);
    emit dataChanged();
}

void Agent::syncModelViewPrefixFromStore()
{
    if (!m_summaryQueue) {
        m_modelViewStore.clear();
        if (m_loop) {
            m_loop->clearModelViewPrefix();
        }
        return;
    }
    m_modelViewStore.syncFromSummaryStore(m_summaryStore);

    // 有摘要前缀时：再挂「最近用户输入」醒目块（对齐任务目标，防大压失忆）
    QList<QString> prefixes = m_modelViewStore.prefixTexts();
    if (!prefixes.isEmpty() && m_loop) {
        const int recentTurns = m_runtime.summaryRecentTurns > 0 ? m_runtime.summaryRecentTurns : 5;
        const QList<QString> recentUsers =
            ModelViewAssembler::collectRecentUserTexts(m_loop->ledger(), recentTurns);
        if (!recentUsers.isEmpty()) {
            QString block = QStringLiteral(
                "[最近用户输入 · 请优先对齐这些目标与约束，勿因摘要省略而改问任务]");
            for (int i = 0; i < recentUsers.size(); ++i) {
                block += QStringLiteral("\n") + QString::number(i + 1) + QStringLiteral(". ")
                    + recentUsers.at(i);
            }
            prefixes.append(block);
            m_modelViewStore.setPrefixTexts(prefixes);
        }
    }

    if (m_loop) {
        m_loop->setModelViewPrefixTexts(m_modelViewStore.prefixTexts());
    }
}

void Agent::emitContextCompactedNotice(const core_ir::CompactReason reason)
{
    core_ir::EventContextCompacted ev;
    ev.reason = reason;
    ev.summaryRecordCount = m_summaryStore.recordCount();
    ev.modelViewPrefixCount = m_modelViewStore.count();
    ev.summaryTokenEstimate = m_summaryStore.totalTokenEstimate();
    for (auto &handler : m_protocolHandlers) {
        handler(core_ir::Event{ev}, {}, {});
    }
    LOGI(LogCat::Agent) << "ContextCompacted 可观测"
        << logf("agentId", m_agentId)
        << logf("reason", core_ir::compactReasonKey(reason))
        << logf("records", ev.summaryRecordCount)
        << logf("prefixes", ev.modelViewPrefixCount)
        << logf("summaryTokens", ev.summaryTokenEstimate);
}

QString Agent::deriveLatestSummary(const QList<ConversationMessage> &messages)
{
    for (int index = messages.size() - 1; index >= 0; --index) {
        const ConversationMessage &message = messages.at(index);
        if (message.text.trimmed().isEmpty()) {
            continue;
        }
        if (message.kind == ConversationMessage::Kind::AssistantText
            || message.kind == ConversationMessage::Kind::SessionEvent
            || message.kind == ConversationMessage::Kind::ToolResult) {
            return message.text.simplified();
        }
    }
    return QString();
}
