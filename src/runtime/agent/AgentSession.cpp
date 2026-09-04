#include "AgentSession.h"
#include "AbstractOrchestration.h"
#include "Agent.h"
#include "config/SessionRuntime.h"

#include "logging/LogManager.h"
#include "types/ConversationMessage.h"
#include "tools/AbstractToolSource.h"
#include "tools/ToolCoordinator.h"
#include "tools/WriteCoordinator.h"
#include "providers/core/AbstractProvider.h"
#include "providers/service/ProviderCredential.h"
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

#include <QDir>
#include <QDateTime>

namespace {

bool jsonAgentIsPrimary(const QJsonObject &agentObj, const bool fallback)
{
    if (agentObj.contains(QStringLiteral("isPrimary"))) {
        return agentObj.value(QStringLiteral("isPrimary")).toBool();
    }
    return fallback;
}

} // namespace

AgentSession::AgentSession(const AgentSessionConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_coordinator(std::make_unique<ToolCoordinator>(this, config.externalToolSource))
    , m_writeCoordinator(std::make_unique<WriteCoordinator>(this))
{
    m_sessionBlobKey = m_sessionId.trimmed().isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : m_sessionId;
    // 会话成员齐了再 attach，编排才能用 insertUnit / coordinator
    if (m_config.orchestration) {
        m_orchestrationGuard = m_config.orchestration;
        m_config.orchestration->attach(this);
        if (AbstractToolSource *source = m_config.orchestration->toolSource()) {
            m_coordinator->addSource(source);
        }
    }
}

AgentSession::~AgentSession()
{
    if (m_orchestrationGuard) {
        m_orchestrationGuard->detach();
    } else if (m_config.orchestration) {
        // 编排先于会话销毁：组合根声明顺序错误（须先声明编排、后声明会话）。
        LOGW(LogCat::Agent) << "编排对象先于会话销毁，跳过 detach（组合根声明顺序错误）";
    }
    // 会话销毁：自动清理本会话 blob 目录
    QDir(sessionBlobRoot()).removeRecursively();
}

const SessionRuntime &AgentSession::runtime() const
{
    return m_runtime;
}

void AgentSession::emitConfigFieldChanged(const QString &key, const QVariant &value)
{
    pushEvent(core_ir::EventConfigChanged{key, value});
}

void AgentSession::setRuntime(const SessionRuntime &runtime)
{
    SessionRuntime next = runtime;
    next.normalizeInPlace();
    (void)commitRuntime(next);
}

bool AgentSession::setRuntimeField(const QString &key, const QVariant &value)
{
    // 单键：值未变返回 false（ConfigTool / 调用方要知是否真改）
    const QString field = key.trimmed();
    if (field.isEmpty())
        return false;

    SessionRuntime next = m_runtime;
    if (!next.setFieldNormalized(field, value))
        return false;
    return commitRuntime(next);
}

bool AgentSession::setRuntimeFields(const QJsonObject &patch)
{
    if (patch.isEmpty())
        return false;

    // 先在副本上预跑全部键；任一失败整单回滚（不碰 m_runtime）
    SessionRuntime next = m_runtime;
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
        const QString field = it.key().trimmed();
        if (field.isEmpty())
            return false;
        if (!next.setFieldNormalized(field, it.value().toVariant()))
            return false;
    }

    // 幂等：值未变仍算成功（与 applyWhitelistedConfig 一致）
    (void)commitRuntime(next);
    return true;
}

bool AgentSession::commitRuntime(const SessionRuntime &next)
{
    const QJsonObject before = m_runtime.toJson();
    const QJsonObject after = next.toJson();
    if (before == after)
        return false;

    m_runtime = next;
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        if (before.value(it.key()) != it.value())
            emitConfigFieldChanged(it.key(), it.value().toVariant());
    }
    applyRuntimeToPrimary();
    emit runtimeChanged();
    return true;
}

void AgentSession::applyRuntimeToPrimary()
{
    Agent *primary = primaryUnit();
    if (!primary)
        return;

    primary->applySessionSettings(m_runtime);
}

ToolCoordinator *AgentSession::coordinator() const
{
    return m_coordinator.get();
}


void AgentSession::setToolResultStoreDir(const QString &dir)
{
    m_toolResultStoreDir = dir;
    for (Agent *agent : std::as_const(m_agents)) {
        agent->setToolResultStoreDirectory(dir);
    }
}

void AgentSession::setModelResponseTimeoutSecs(const int timeoutSecs)
{
    for (Agent *agent : std::as_const(m_agents)) {
        agent->setModelResponseTimeoutSecs(timeoutSecs);
    }
}

void AgentSession::setMaxRetries(const int retries)
{
    for (Agent *agent : std::as_const(m_agents)) {
        agent->setMaxRetries(retries);
    }
}

QString AgentSession::workingDirectory() const
{
    if (!m_runtime.workingDirectory.trimmed().isEmpty())
        return m_runtime.workingDirectory;
    return QDir::currentPath();
}

void AgentSession::setSessionWorkingDirectory(const QString &workingDirectory)
{
    setRuntimeField(QStringLiteral("workingDirectory"), workingDirectory);
}

QString AgentSession::userCustomPrompt() const
{
    return m_config.promptBuilder ? m_config.promptBuilder->userCustomPrompt() : QString();
}

void AgentSession::setUserCustomPrompt(const QString &text)
{
    if (m_config.promptBuilder) {
        m_config.promptBuilder->setUserCustomPrompt(text);
    }
}

// ── 会话标识 ──

QString AgentSession::sessionId() const { return m_sessionId; }
void AgentSession::setSessionId(const QString &id)
{
    if (m_sessionId == id) {
        return;
    }
    m_sessionId = id;
    if (!m_sessionId.trimmed().isEmpty()) {
        m_sessionBlobKey = m_sessionId;
    }
    applyBlobRootToAgents();
}

QString AgentSession::sessionBlobRoot() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/provider-blobs/") + m_sessionBlobKey;
}

void AgentSession::applyBlobRootToAgents()
{
    const QString root = sessionBlobRoot();
    for (Agent *agent : std::as_const(m_agents)) {
        agent->loop()->ledger().setBlobRoot(root);
    }
}
QString AgentSession::forkedFromSessionId() const { return m_forkedFromSessionId; }
void AgentSession::setForkedFromSessionId(const QString &sourceSessionId)
{
    m_forkedFromSessionId = sourceSessionId.trimmed();
}
QString AgentSession::title() const { return m_title; }
void AgentSession::setTitle(const QString &title)
{
    const QString trimmed = title.trimmed();
    if (m_title == trimmed) {
        return;
    }
    m_title = trimmed;
    emit titleChanged(trimmed);
    pushEvent(core_ir::EventConfigChanged{QStringLiteral("sessionTitle"), trimmed});
}

// ── 工厂 ──

Agent *AgentSession::insertUnit(const QString &agentId,
                                const QString &displayName)
{
    Q_ASSERT_X(m_config.globalDefaults, "AgentSession::insertUnit", "globalDefaults 不能为空");

    // createSession 已 setRuntime；globalDefaults.workingDirectory 在 TUI FromLaunchCwd 下可能仍为空。
    SessionRuntime runtime = m_runtime;
    if (runtime.workingDirectory.trimmed().isEmpty()) {
        runtime.workingDirectory = m_config.globalDefaults->workingDirectory;
    }
    if (runtime.workingDirectory.trimmed().isEmpty()) {
        runtime.workingDirectory = QDir::currentPath();
    }
    if (runtime.systemPrompt.trimmed().isEmpty()) {
        runtime.systemPrompt = m_config.globalDefaults->systemPrompt;
    }
    auto *agent = new Agent(agentId, displayName, runtime, this);
    agent->setCredentialStore(m_config.credentialStore);
    agent->setToolResultStoreDirectory(m_toolResultStoreDir);
    agent->setPromptBuilder(m_config.promptBuilder);
    if (m_config.modePolicyFactory) {
        agent->setModePolicyFactory(m_config.modePolicyFactory);
    }
    // CompactEngine 无单例回落：漏注入工厂则压缩必跳过
    if (m_config.providerFactory) {
        agent->setProviderFactory(m_config.providerFactory);
    }

    // 先入表再通知编排、再 setCoordinator：主单元身份与段摘要管线都依赖 findById。
    m_agents.insert(agentId, agent);
    if (!m_order.contains(agentId)) {
        m_order.append(agentId);
    }
    if (m_config.orchestration) {
        m_config.orchestration->onUnitInserted(agent);
    }
    agent->setCoordinator(m_coordinator.get());
    agent->loop()->ledger().setBlobRoot(sessionBlobRoot());
    connectAgentSignals(agent);
    emit agentAdded(agentId);
    pushEvent(core_ir::EventAgentStateChanged{agentId, false, {}, true, false, {}, {}, 0,
                                              core_ir::AgentStatus::Idle});
    return agent;
}

// ── 查找 ──

Agent *AgentSession::findById(const QString &agentId) const
{
    return m_agents.value(agentId, nullptr);
}

Agent *AgentSession::primaryUnit() const
{
    if (m_config.orchestration) {
        if (Agent *primary = m_config.orchestration->primaryUnit()) {
            return primary;
        }
    }
    if (m_order.isEmpty()) {
        return nullptr;
    }
    return findById(m_order.first());
}

bool AgentSession::isPrimary(const Agent *unit) const
{
    if (!unit) {
        return false;
    }
    if (m_config.orchestration) {
        return m_config.orchestration->isPrimary(unit);
    }
    return !m_order.isEmpty() && unit->agentId() == m_order.first();
}

bool AgentSession::contains(const QString &agentId) const
{
    return m_agents.contains(agentId);
}

QList<Agent *> AgentSession::allAgents() const
{
    QList<Agent *> result;
    result.reserve(m_agents.size());
    for (const QString &id : m_order) {
        if (Agent *agent = m_agents.value(id, nullptr)) {
            result.append(agent);
        }
    }
    return result;
}

int AgentSession::count() const
{
    return m_agents.size();
}

// ── 账本导出/导入 ──

QJsonObject AgentSession::exportLedger() const
{
    // 与 session.json 的 agents[] 同构：每 agent 带 id/display/parent/primary/runtime/ledger
    QJsonObject root;
    QJsonArray agentsArr;
    for (Agent *agent : allAgents()) {
        if (!agent || !agent->loop())
            continue;
        QJsonObject agentObj;
        agentObj.insert(QStringLiteral("agentId"), agent->agentId());
        agentObj.insert(QStringLiteral("displayName"), agent->displayName());
        agentObj.insert(QStringLiteral("parentAgentId"), agent->parentAgentId());
        agentObj.insert(QStringLiteral("isPrimary"), isPrimary(agent));
        agentObj.insert(QStringLiteral("runtime"), agent->runtime().toJson());
        agentObj.insert(QStringLiteral("ledger"), agent->loop()->ledger().toJson());
        agentsArr.append(agentObj);
    }
    root.insert(QStringLiteral("agents"), agentsArr);
    return root;
}

void AgentSession::importLedger(const QJsonObject &json,
                                const QString &workingDirectoryOverride)
{
    // 记录导入时刻：账本中更早的用户消息视为历史（fork 复制），不得触发 AutoRename
    m_ledgerImportedAtMs = QDateTime::currentMSecsSinceEpoch();

    const QJsonArray agentsArr = json.value(QStringLiteral("agents")).toArray();
    struct Row {
        QJsonObject obj;
        QString id;
        QString displayName;
        bool primary = false;
    };
    QList<Row> rows;
    rows.reserve(agentsArr.size());
    int primaryIndex = -1;
    for (const QJsonValue &v : agentsArr) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject agentObj = v.toObject();
        const QString agentId = agentObj.value(QStringLiteral("agentId")).toString().trimmed();
        if (agentId.isEmpty()) {
            continue;
        }
        Row row;
        row.obj = agentObj;
        row.id = agentId;
        row.displayName = agentObj.value(QStringLiteral("displayName")).toString();
        row.primary = jsonAgentIsPrimary(agentObj, false);
        if (row.primary && primaryIndex < 0) {
            primaryIndex = rows.size();
        }
        rows.append(std::move(row));
    }
    if (rows.isEmpty()) {
        return;
    }
    if (primaryIndex < 0) {
        primaryIndex = 0;
        rows[primaryIndex].primary = true;
    }

    QList<int> order;
    order.append(primaryIndex);
    for (int i = 0; i < rows.size(); ++i) {
        if (i != primaryIndex) {
            order.append(i);
        }
    }

    QString sharedUuid;
    if (Agent *existing = primaryUnit()) {
        sharedUuid = existing->sessionUuid();
    }

    for (const int idx : order) {
        const Row &row = rows.at(idx);
        Agent *agent = findById(row.id);
        if (!agent) {
            agent = insertUnit(row.id, row.displayName.isEmpty() ? row.id : row.displayName);
            if (!agent) {
                continue;
            }
            agent->setParentAgentId(row.obj.value(QStringLiteral("parentAgentId")).toString());
            if (sharedUuid.isEmpty()) {
                sharedUuid = agent->sessionUuid();
            } else {
                agent->setSessionUuid(sharedUuid);
            }
        } else if (sharedUuid.isEmpty()) {
            sharedUuid = agent->sessionUuid();
        }
        if (!agent->loop()) {
            continue;
        }

        const QJsonObject agentRuntime = row.obj.value(QStringLiteral("runtime")).toObject();
        if (!agentRuntime.isEmpty()) {
            SessionRuntime rt = SessionRuntime::fromJson(agentRuntime);
            rt.normalizeInPlace();
            if (!workingDirectoryOverride.isEmpty()) {
                rt.workingDirectory = workingDirectoryOverride;
            }
            agent->applySessionSettings(rt);
        }
        agent->loop()->ledger().fromJson(row.obj.value(QStringLiteral("ledger")));
        agent->loop()->refreshContextTokenEstimate();
    }
}

// ── 生命周期 ──

Agent *AgentSession::removeAgent(const QString &agentId)
{
    Agent *agent = m_agents.take(agentId);
    if (agent) {
        m_order.removeAll(agentId);
        emit agentRemoved(agentId);
    }
    return agent;
}

void AgentSession::clear()
{
    if (m_config.orchestration) {
        m_config.orchestration->onUnitsClearing();
    }

    // 显式清邮箱：未读消息以 Dropped 事件可观测地丢弃（编排可在 onUnitsClearing 先抢救）。
    for (Agent *agent : std::as_const(m_agents)) {
        agent->clearInbox(QStringLiteral("session_cleared"));
    }

    qDeleteAll(m_agents);
    m_agents.clear();
    m_order.clear();
    m_nextIndex = 1;
    m_selectedAgentId.clear();
    // 会话清空：自动清理本会话 blob 目录
    QDir(sessionBlobRoot()).removeRecursively();
}

void AgentSession::start()
{
    clear();
    if (m_config.orchestration) {
        m_config.orchestration->onSessionStarted();
    }
}

// ── ID 分配 ──

QString AgentSession::allocateAgentId()
{
    while (true) {
        const QString candidate = QStringLiteral("agent-%1").arg(m_nextIndex++);
        if (!m_agents.contains(candidate)) {
            return candidate;
        }
    }
}

QString AgentSession::allocateDisplayName(const QString &requestedName) const
{
    const QString trimmed = requestedName.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }
    return QStringLiteral("agent-%1").arg(m_nextIndex);
}



// ── 遍历 ──

void AgentSession::forEach(const std::function<void(Agent *)> &fn) const
{
    for (Agent *agent : std::as_const(m_agents)) {
        fn(agent);
    }
}

void AgentSession::connectAgentSignals(Agent *agent)
{
    // 协议 EventAgentStateChanged：
    // - Loop 路径：AbstractLoop 发事件 → Agent 转发时填 status
    // - 管理器路径：Agent::setManagerStatus → emitAgentStateProtocolEvent
    // 此处只做会话内副作用，禁止再按枚举变化二次 push（会与 Loop 事件重复）。
    connect(agent, &Agent::stateChanged, agent, [this, agent]() {
        if (m_config.orchestration) {
            m_config.orchestration->onUnitStateChanged(agent);
        }

        if (!agent->busy()) {
            triggerAutoRenameIfNeeded(agent);
        }
    });

    // dataChanged 无对应细粒度「上下文估计」事件：补一发状态投影。
    // 与 setManagerStatus 共用 Agent 状态构造，避免字段列表分叉。
    connect(agent, &Agent::dataChanged, agent, [agent]() {
        agent->emitAgentStateProtocolEvent();
    });
}

void AgentSession::setSelectedAgentId(const QString &id)
{
    if (m_selectedAgentId != id) {
        m_selectedAgentId = id;
        emit selectedAgentChanged(id);
    }
}

QString AgentSession::selectedAgentId() const
{
    return m_selectedAgentId;
}

Agent *AgentSession::selectedAgent() const
{
    return findById(m_selectedAgentId);
}

void AgentSession::triggerAutoRenameIfNeeded(Agent *agent)
{
    QString firstUserText;
    if (!isEligibleForRename(agent, firstUserText)) {
        return;
    }

    m_autoRenamePending = true;

    ProviderAuth auth;
    QString providerType;
    if (!getProviderAuth(auth, providerType)) {
        LOGW(LogCat::Session) << "【自动重命名】中止：无法获取有效的 Provider 凭据配置"
            << logf("sessionId", m_sessionId);
        fallbackRename(agent);
        return;
    }

    if (!m_config.providerFactory) {
        LOGW(LogCat::Session) << "【自动重命名】中止：Provider 工厂未注入"
            << logf("sessionId", m_sessionId);
        fallbackRename(agent);
        return;
    }

    AbstractProvider *provider = nullptr;
    try {
        auto summaryProvider = m_config.providerFactory(providerType);
        if (!summaryProvider) {
            LOGW(LogCat::Session) << "【自动重命名】错误：无法创建 Provider，可能不支持此类型"
                << logf("providerType", providerType);
            fallbackRename(agent);
            return;
        }
        provider = summaryProvider.release();
    } catch (const std::exception &e) {
        LOGW(LogCat::Session) << "【自动重命名】异常：创建 Provider 异常"
            << logf("error", e.what());
        fallbackRename(agent);
        return;
    } catch (...) {
        LOGW(LogCat::Session) << "【自动重命名】异常：创建 Provider 未知异常";
        fallbackRename(agent);
        return;
    }

    provider->setParent(this);
    provider->setAuth(auth);

    ProviderRequest request = buildRenameRequest(firstUserText);

    auto connection = std::make_shared<QMetaObject::Connection>();
    auto summaryText = std::make_shared<QString>();

    *connection = connect(provider, &AbstractProvider::eventEmitted, this,
        [this, provider, connection, summaryText](const ProviderEvent &event) {
            handleRenameEvent(event, provider, *connection, summaryText);
        });

    LOGD(LogCat::Session) << "【自动重命名】启动异步标题提炼"
        << logf("providerType", providerType)
        << logf("firstUserText", firstUserText);

    try {
        provider->sendRequestWithoutModelRefresh(request);
    } catch (const std::exception &e) {
        LOGW(LogCat::Session) << "【自动重命名】异常：发送请求异常"
            << logf("error", e.what());
        fallbackRename(agent);
        m_autoRenamePending = false;
        disconnect(*connection);
        provider->deleteLater();
    } catch (...) {
        LOGW(LogCat::Session) << "【自动重命名】异常：发送请求未知异常";
        fallbackRename(agent);
        m_autoRenamePending = false;
        disconnect(*connection);
        provider->deleteLater();
    }
}

bool AgentSession::isEligibleForRename(Agent *agent, QString &firstUserText) const
{
    if (!agent) {
        return false;
    }
    if (!m_config.orchestration || !m_config.orchestration->ownsSessionTitle(agent)) {
        return false;
    }

    if (m_title != QStringLiteral("新会话")) {
        return false;
    }

    if (m_autoRenamePending) {
        return false;
    }

    const QList<ConversationMessage> msgs = agent->ledgerMessages();
    QString userText;
    qint64 lastUserCreatedAtMs = 0;
    bool hasAssistantReply = false;

    // 取「最后一条」用户消息作标题输入：fork 会话复制账本后继续新对话，
    // 标题随最近对话主题演化（与源会话天然区分）；普通新会话首轮第一条=最后一条，行为不变。
    for (const auto &msg : msgs) {
        if (msg.role() == ConversationMessage::Role::User && !msg.text.trimmed().isEmpty()) {
            userText = msg.text.trimmed();
            lastUserCreatedAtMs = msg.createdAtMs;
        }
        if (msg.role() == ConversationMessage::Role::Assistant && !msg.text.trimmed().isEmpty()) {
            hasAssistantReply = true;
        }
    }

    if (userText.isEmpty() || !hasAssistantReply) {
        return false;
    }

    // 导入账本的会话（fork/恢复）：最后一条用户消息必须是导入后新增的，
    // 否则视为历史对话（fork 瞬间误触发会与源会话重名）
    if (m_ledgerImportedAtMs > 0 && lastUserCreatedAtMs < m_ledgerImportedAtMs) {
        return false;
    }

    firstUserText = userText;
    return true;
}

bool AgentSession::getProviderAuth(ProviderAuth &auth, QString &providerType) const
{
    if (!m_config.credentialStore) {
        return false;
    }

    const SessionRuntime &rt = m_runtime;
    QString credentialInstanceId = rt.credentialInstanceId;
    QString modelName = rt.modelName;

    QVariantMap inst = m_config.credentialStore->getInstance(credentialInstanceId);
    providerType = inst.value(QStringLiteral("providerType")).toString();
    QString baseUrl = inst.value(QStringLiteral("baseUrl")).toString();
    QString apiKey = inst.value(QStringLiteral("apiKey")).toString();

    if (providerType.isEmpty() || apiKey.isEmpty()) {
        LOGW(LogCat::Session) << "【自动重命名】凭据读取失败：providerType 或 apiKey 为空"
            << logf("providerType", providerType)
            << logf("hasApiKey", !apiKey.isEmpty());
        return false;
    }

    auth = ProviderAuth{ baseUrl, apiKey, modelName };
    return true;
}

ProviderRequest AgentSession::buildRenameRequest(const QString &firstUserText) const
{
    ProviderRequest request;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.systemPrompt = QStringLiteral("你是一个会话标题提炼专家。请阅读用户的第一句话，提炼出一个简短、精准、不超过6个字的中文会话名称作为标题，直接输出该标题，不要包含任何前缀、标点或解释。");
    request.items.append(ProviderItem::makeUserText(firstUserText));
    request.maxOutputTokens = 1024;
    request.temperature = 0.3;
    request.stream = true;
    request.reasoning.enabled = false;
    request.desiredOutput = ProviderOutputSpec::textOnly();
    return request;
}

void AgentSession::handleRenameEvent(const ProviderEvent &event, AbstractProvider *provider,
                                      const QMetaObject::Connection &connection,
                                      const std::shared_ptr<QString> &summaryText)
{
    // 不打印每个 TextDelta/ReasoningDelta，避免自动重命名把日志刷爆。
    if (event.kind == ProviderEventKind::TextDelta) {
        *summaryText += event.deltaPayload.text;
        return;
    }

    const auto finishRename = [this, provider, connection]() {
        m_autoRenamePending = false;
        disconnect(connection);
        provider->deleteLater();
    };

    if (event.kind == ProviderEventKind::MessageCompleted) {
        QString finalTitle = summaryText->trimmed();
        finalTitle.replace('\n', ' ');
        finalTitle.replace('\r', ' ');

        if (finalTitle.startsWith(QLatin1Char('"')) && finalTitle.endsWith(QLatin1Char('"'))) {
            finalTitle = finalTitle.mid(1, finalTitle.length() - 2);
        }
        if (finalTitle.startsWith(QStringLiteral("“")) && finalTitle.endsWith(QStringLiteral("”"))) {
            finalTitle = finalTitle.mid(1, finalTitle.length() - 2);
        }
        finalTitle = finalTitle.trimmed();

        if (finalTitle.length() > 8) {
            finalTitle = finalTitle.mid(0, 8);
        }

        if (!finalTitle.isEmpty()) {
            LOGI(LogCat::Session) << "【自动重命名】成功"
                << logf("title", finalTitle);
            setTitle(finalTitle);
        } else {
            LOGW(LogCat::Session) << "【自动重命名】警告：大模型返回提炼标题为空，走保底机制";
            fallbackRename(primaryUnit());
        }
        finishRename();
        return;
    }

    if (event.kind == ProviderEventKind::Error || event.kind == ProviderEventKind::Cancelled) {
        LOGW(LogCat::Session) << "【自动重命名】API 错误/取消，走保底机制"
            << logf("kind", event.kind == ProviderEventKind::Error ? "Error" : "Cancelled")
            << logf("code", event.error.code)
            << logf("error", event.error.message);
        fallbackRename(primaryUnit());
        finishRename();
    }
}

void AgentSession::fallbackRename(Agent *agent)
{
    if (!agent) {
        return;
    }
    const QList<ConversationMessage> msgs = agent->ledgerMessages();
    if (msgs.isEmpty()) {
        return;
    }
    QString firstUserText;
    // 保底机制与 isEligibleForRename 同源：取「最后一条」用户消息（fork 会话随新对话演化标题）
    for (const auto &msg : msgs) {
        if (msg.role() == ConversationMessage::Role::User && !msg.text.trimmed().isEmpty()) {
            firstUserText = msg.text.trimmed();
        }
    }
    if (!firstUserText.isEmpty()) {
        QString newTitle = firstUserText.mid(0, 8).trimmed();
        if (firstUserText.length() > 8) {
            newTitle += QStringLiteral("...");
        }
        setTitle(newTitle);
    }
}

void AgentSession::pushEvent(const core_ir::Event &event) const
{
    fanOutEvent(event);
}

void AgentSession::fanOutEvent(const core_ir::Event &event,
                               const core_ir::EventContext &context,
                               const core_ir::SubmissionId &submissionId) const
{
    for (auto &h : m_protocolHandlers)
        h(event, context, submissionId);
}

// ── 内环事件 fan-out ──

core_ir::HandlerId AgentSession::addEventHandler(core_ir::EventHandler handler)
{
    const auto id = reinterpret_cast<core_ir::HandlerId>(m_protocolHandlers.size() + 1);
    m_protocolHandlers.push_back(std::move(handler));
    return id;
}

void AgentSession::removeEventHandler(core_ir::HandlerId id)
{
    const size_t idx = reinterpret_cast<std::uintptr_t>(id) - 1;
    if (idx < m_protocolHandlers.size()) {
        m_protocolHandlers.erase(m_protocolHandlers.begin() + static_cast<ptrdiff_t>(idx));
    }
}



