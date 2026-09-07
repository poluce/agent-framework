#include "Agent.h"
#include "agent/compact/CompactPipeline.h"
#include "tools/AbstractSession.h"
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
    , m_compact(std::make_unique<CompactPipeline>(m_agentId))
    , m_taskManager(std::make_unique<AgentTaskManager>(this))
{
    Q_ASSERT_X(!m_runtime.workingDirectory.trimmed().isEmpty(),
               "Agent::Agent", "workingDirectory 不能为空，调用方必须提供有效路径");

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
            m_protocolHandlers.dispatch(out, ctx, sid);
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

    m_compact->setLoop(m_loop.get());
    m_compact->setRuntime(m_runtime);
    connect(m_loop.get(), &AbstractLoop::compactionRequested, m_compact.get(),
            &CompactPipeline::onCompactionRequested);
    connect(m_loop.get(), &AbstractLoop::turnSucceeded, m_compact.get(),
            &CompactPipeline::onTurnSucceeded);
    connect(m_compact.get(), &CompactPipeline::protocolEvent, this,
            [this](const core_ir::Event &event) {
                m_protocolHandlers.dispatch(event);
            });
    connect(m_compact.get(), &CompactPipeline::unitStateChanged, this, &Agent::stateChanged);
    connect(m_compact.get(), &CompactPipeline::unitDataChanged, this, &Agent::dataChanged);
    connect(m_compact.get(), &CompactPipeline::agentStateRefreshNeeded, this,
            &Agent::emitAgentStateProtocolEvent);

    LOGD(LogCat::Agent) << "创建 Agent"
        << logf("agentId", m_agentId)
        << logf("display", m_displayName);
}

Agent::~Agent()
{
    clearSummaryState();
    clearInbox(QStringLiteral("agent_destroyed"));
    m_protocolHandlers.dispatch(core_ir::Event{core_ir::EventShutdownComplete{}});
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
    if (m_compact) {
        m_compact->setRuntime(m_runtime);
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

/// 仅宿主查询用：不参与系统提示词拼装（拼装走 SystemPromptBuilder 体系）。
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
    AbstractSession *session = coordinator ? coordinator->session() : nullptr;
    if (m_compact) {
        m_compact->ensureInstalled(session && session->usesSegmentSummary(m_agentId));
    }
}

bool Agent::remainsIdleAfterTurn() const
{
    AbstractSession *session = m_coordinator ? m_coordinator->session() : nullptr;
    if (!session) {
        return true;
    }
    return session->remainsIdleAfterTurn(m_agentId);
}

void Agent::setProviderFactory(ProviderFactory factory)
{
    m_providerFactory = std::move(factory);
    m_loop->setProviderFactory(m_providerFactory);
    if (m_compact) {
        m_compact->setProviderFactory(m_providerFactory);
    }
}

void Agent::setPromptBuilder(SystemPromptBuilder *builder)
{
    m_promptBuilder = builder;
    m_loop->setPromptBuilder(builder);
    if (m_compact) {
        m_compact->setPromptBuilder(builder);
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
    if (m_compact) {
        m_compact->setCredentialStore(credentialStore);
    }
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
    if (normalized == QStringLiteral("pwsh") || normalized == QStringLiteral("powershell")) {
        m_runtime.defaultShell = normalized;
    } else {
        m_runtime.defaultShell = QStringLiteral("bash");
    }
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
    return m_compact ? m_compact->addedTokens() : 0;
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
    m_protocolHandlers.dispatch(core_ir::Event{payload});
}

void Agent::emitInboxEnqueued(const AgentInboxMessage &msg)
{
    const core_ir::EventInboxMessageEnqueued payload{
        msg.id, msg.fromAgentId, m_agentId, msg.priority
    };
    m_protocolHandlers.dispatch(core_ir::Event{payload});
}

void Agent::emitInboxDelivered(const AgentInboxMessage &msg)
{
    const core_ir::EventInboxMessageDelivered payload{
        msg.id, msg.fromAgentId, m_agentId
    };
    m_protocolHandlers.dispatch(core_ir::Event{payload});
}

void Agent::emitInboxDropped(const AgentInboxMessage &msg, const QString &reason)
{
    const core_ir::EventInboxMessageDropped payload{
        msg.id, msg.fromAgentId, m_agentId, reason
    };
    m_protocolHandlers.dispatch(core_ir::Event{payload});
}

// ── 操作 ──

bool Agent::submitUserDelivery(const QString &message,
                               const QStringList &attachedFilePaths,
                               AbstractLoop::UserDelivery delivery)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty() && attachedFilePaths.isEmpty()) {
        return false;
    }

    LOGD(LogCat::Agent) << "提交用户消息"
        << logf("agentId", m_agentId)
        << logf("delivery", delivery == AbstractLoop::UserDelivery::Steer
                                ? QStringLiteral("steer")
                                : QStringLiteral("next_turn"))
        << logf("busy", m_loop->isBusy())
        << logf("preview", trimmed.left(80));

    bool accepted = false;
    if (attachedFilePaths.isEmpty()) {
        accepted = m_loop->enqueueUserMessage(trimmed, delivery);
    } else {
        accepted = m_loop->enqueueUserMessageWithFiles(trimmed, attachedFilePaths, delivery);
    }

    // Idle + NextTurn：立即开轮；Busy 或 Steer 仅排队
    if (!m_loop->isBusy() && delivery == AbstractLoop::UserDelivery::NextTurn) {
        m_status = AgentStatus::Running;
        m_loop->start(m_runtime);
    }
    return accepted;
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

bool Agent::submitAgentTask(const QString &message)
{
    return submitMessageInternal(message, ConversationMessage::Kind::AgentTask, QStringLiteral("接收代理任务:"));
}

bool Agent::submitMessageInternal(const QString &message, ConversationMessage::Kind kind, const QString &logLabel)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    LOGD(LogCat::Agent) << logLabel
        << logf("agentId", m_agentId)
        << logf("preview", trimmed.left(80));
    m_status = AgentStatus::Running;
    const bool accepted = m_loop->enqueueMessage(trimmed, kind, {}, AbstractLoop::UserDelivery::NextTurn);
    m_loop->start(m_runtime);
    return accepted;
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
        << logf("boundaryWait", m_compact && m_compact->waitingAtBoundary());

    if (m_compact && m_compact->cancelBoundaryWait()) {
        handleLoopStateChanged();
        return;
    }

    m_loop->cancel();
    if (m_compact) {
        m_compact->cancelInFlight();
    }
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
    m_protocolHandlers.dispatch(core_ir::Event{core_ir::EventSessionEvent{m_agentId, text.trimmed()}});
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

bool Agent::enqueueInboxMessage(const UnitInboxMessage &msg)
{
    AgentInboxMessage full;
    full.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    full.fromAgentId = msg.fromAgentId;
    full.fromDisplayName = msg.fromAgentId;
    full.content = msg.content;
    full.type = msg.type;
    full.payload = msg.payload;
    full.timestamp = QDateTime::currentDateTimeUtc();
    return enqueueInboxMessage(full);
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
    return m_protocolHandlers.add(std::move(handler));
}

void Agent::removeEventHandler(core_ir::HandlerId id)
{
    m_protocolHandlers.remove(id);
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

bool Agent::requestManualCompaction(const qint64 targetTokens)
{
    return m_compact && m_compact->requestManualCompaction(targetTokens);
}

void Agent::clearSummaryState()
{
    if (m_compact) {
        m_compact->clear();
    }
}

bool Agent::hasSegmentSummaryQueue() const
{
    return m_compact && m_compact->hasQueue();
}

int Agent::segmentSummaryJobCount() const
{
    return m_compact ? m_compact->jobCount() : 0;
}

bool Agent::isWaitingSegmentSummaryAtBoundary() const
{
    return m_compact && m_compact->waitingAtBoundary();
}

bool Agent::segmentSummaryStoreEmpty() const
{
    return !m_compact || m_compact->storeEmpty();
}

int Agent::segmentSummaryRecordCount() const
{
    return m_compact ? m_compact->recordCount() : 0;
}

QJsonObject Agent::exportSummaryState() const
{
    return m_compact ? m_compact->exportState() : QJsonObject();
}

void Agent::importSummaryState(const QJsonObject &obj)
{
    if (m_compact) {
        m_compact->importState(obj);
    }
}

void Agent::probeSegmentSummaryAfterTurnSuccess()
{
    if (m_compact) {
        m_compact->onTurnSucceeded();
    }
}

bool Agent::hasFailedSegmentSummaryJobs() const
{
    return m_compact && m_compact->hasFailedJobs();
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
