#include "CompactEngine.h"

#include "CompactToolPair.h"
#include "agent/ProviderRunLedger.h"
#include "config/SystemPromptBuilder.h"
#include "logging/LogManager.h"
#include "tools/BuiltinToolRuntime.h"
#include "tools/ToolTypes.h"

#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include "providers/core/AbstractProvider.h"
#include "providers/ProviderTypes/ProviderTypes.h"
#include "providers/service/ProviderCredential.h"

namespace {
ModelCapabilities buildCompactionFallbackCapabilities(const QString &modelName)
{
    ModelCapabilities capabilities;
    capabilities.modelId = modelName.trimmed();
    capabilities.enable(ProviderCapability::TextInput)
        .enable(ProviderCapability::TextOutput)
        .enable(ProviderCapability::Reasoning)
        .enable(ProviderCapability::MaxOutputTokens);
    return capabilities;
}

/// 条内裁剪：尽量保留结构，只截断过长正文
QString trimTextToBudget(const QString &text, const qint64 maxTokens)
{
    if (maxTokens <= 0 || text.isEmpty()) {
        return {};
    }
    if (estimateContextTokensForText(text) <= maxTokens) {
        return text;
    }
    int lo = 0;
    int hi = text.size();
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (estimateContextTokensForText(text.left(mid)) <= maxTokens) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    if (lo <= 0) {
        return {};
    }
    return text.left(lo) + QStringLiteral("…");
}

constexpr qint64 kAssistantCapTokens = 800;
constexpr qint64 kToolResultCapTokens = 200;
constexpr qint64 kToolResultErrorCapTokens = 320;
constexpr int kMinSummaryChars = 80;

QString toolNameOf(const ConversationMessage &entry)
{
    return entry.toolName.isEmpty() ? QStringLiteral("tool") : entry.toolName;
}

QString callIdOf(const ConversationMessage &entry)
{
    if (!entry.toolUseId.isEmpty()) {
        return entry.toolUseId;
    }
    if (!entry.id.isEmpty()) {
        return entry.id;
    }
    return {};
}

/// 调用对象：ledger.summaryText → Runtime 摘要 → toolName
QString resolveCallTarget(const ConversationMessage &entry)
{
    const QString fromSummary = entry.summaryText.trimmed();
    if (!fromSummary.isEmpty()) {
        return fromSummary;
    }

    ToolCall call = entry.toolCall;
    if (call.toolName.isEmpty()) {
        call.toolName = entry.toolName;
    }
    if (call.input.isEmpty() && !entry.toolInput.isEmpty()) {
        call.input = entry.toolInput;
    }
    if (!call.toolName.isEmpty() || !call.input.isEmpty()) {
        const QString fromRuntime = BuiltinToolRuntime::summarizeToolCall(call).trimmed();
        if (!fromRuntime.isEmpty()) {
            return fromRuntime;
        }
    }
    return toolNameOf(entry);
}

QString resultStatusLabel(const ConversationMessage &entry)
{
    if (entry.status == ConversationMessage::Status::Canceled) {
        return QStringLiteral("canceled");
    }
    if (entry.isError || entry.status == ConversationMessage::Status::Failed
        || entry.status == ConversationMessage::Status::Rejected) {
        return QStringLiteral("error");
    }
    return QStringLiteral("ok");
}

/// 短结果：失败优先错误首行；成功 previewText → 条内裁剪 text
QString resolveShortOutcome(const ConversationMessage &entry)
{
    const bool isErr = entry.isError
        || entry.status == ConversationMessage::Status::Failed
        || entry.status == ConversationMessage::Status::Rejected;
    const qint64 cap = isErr ? kToolResultErrorCapTokens : kToolResultCapTokens;

    if (isErr) {
        const QString preview = entry.previewText.trimmed();
        if (!preview.isEmpty()) {
            return trimTextToBudget(preview, cap);
        }
        const QString body = entry.text.trimmed();
        if (!body.isEmpty()) {
            // 错误优先首行
            const QString firstLine = body.section(QLatin1Char('\n'), 0, 0).trimmed();
            return trimTextToBudget(firstLine.isEmpty() ? body : firstLine, cap);
        }
        return QStringLiteral("（错误，无详情）");
    }

    const QString preview = entry.previewText.trimmed();
    if (!preview.isEmpty()) {
        return trimTextToBudget(preview, cap);
    }
    const QString body = entry.text.trimmed();
    if (!body.isEmpty()) {
        return trimTextToBudget(body, cap);
    }
    return QStringLiteral("（无输出）");
}

enum class MaterialTier {
    User,
    Assistant,
    Tool,
    Skill,
};

struct MaterialBlock
{
    MaterialTier tier = MaterialTier::User;
    int order = 0;
    QString text;
};

QString taskShell(const QString &material)
{
    return QStringLiteral(
               "请基于下列对话材料，为即将接手的模型写一份交接摘要。\n"
               "只输出摘要正文，禁止调用工具、禁止输出 tool_calls/DSML/函数调用 JSON、禁止复述整段材料。\n\n"
               "===== 材料开始 =====\n")
        + material
        + QStringLiteral("\n===== 材料结束 =====");
}

} // namespace

CompactEngine::CompactEngine(QObject *parent)
    : QObject(parent)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &CompactEngine::startRequest);
}

CompactEngine::~CompactEngine() = default;

void CompactEngine::setPromptBuilder(SystemPromptBuilder *builder)
{
    m_promptBuilder = builder;
}

bool CompactEngine::prepareTempProvider(
    const QString &credentialInstanceId,
    ProviderCredential *credentialStore,
    const std::function<std::unique_ptr<AbstractProvider>(const QString &)> &providerFactory,
    const QString &modelName,
    AbstractProvider *activeProvider,
    QString *skipReason)
{
    const QVariantMap inst = credentialStore ? credentialStore->getInstance(credentialInstanceId)
                                             : QVariantMap{};
    const QString providerType = inst.value(QStringLiteral("providerType")).toString();
    if (providerType.isEmpty()) {
        if (skipReason)
            *skipReason = QStringLiteral("未配置可用的模型凭据。");
        return false;
    }
    if (!providerFactory) {
        if (skipReason)
            *skipReason = QStringLiteral("模型通道不可用。");
        return false;
    }
    m_tempProvider = providerFactory(providerType);
    if (!m_tempProvider) {
        if (skipReason)
            *skipReason = QStringLiteral("无法创建模型通道。");
        return false;
    }
    const ProviderAuth auth{
        inst.value(QStringLiteral("baseUrl")).toString(),
        inst.value(QStringLiteral("apiKey")).toString(),
        modelName
    };
    m_tempProvider->setAuth(auth);
    if (activeProvider) {
        const QList<ModelCapabilities> activeModels = activeProvider->availableModels();
        if (!activeModels.isEmpty()) {
            m_tempProvider->seedAvailableModels(activeModels);
        }
    }
    if (m_tempProvider->availableModels().isEmpty()) {
        m_tempProvider->seedAvailableModels(
            {buildCompactionFallbackCapabilities(modelName)});
    }
    return true;
}

void CompactEngine::start(
    ProviderRunLedger *ledger,
    const QString &credentialInstanceId,
    ProviderCredential *credentialStore,
    const std::function<std::unique_ptr<AbstractProvider>(const QString &)> &providerFactory,
    const QString &modelName,
    AbstractProvider *activeProvider
)
{
    if (m_running) {
        LOGW(LogCat::Agent) << "压缩引擎已在运行，忽略重复 start";
        return;
    }

    if (!ledger) {
        LOGW(LogCat::Agent) << "压缩跳过：ledger 为空";
        finishSkipped(QStringLiteral("压缩跳过：内部账本不可用。"));
        return;
    }

    m_compactedIds = selectEntriesToCompact(*ledger);
    if (m_compactedIds.isEmpty()) {
        LOGD(LogCat::Agent) << "压缩跳过：没有可压缩的条目";
        finishSkipped(QStringLiteral("压缩跳过：没有可压缩的已提交条目。"));
        return;
    }

    QString skipReason;
    if (!prepareTempProvider(credentialInstanceId, credentialStore, providerFactory,
                             modelName, activeProvider, &skipReason)) {
        LOGW(LogCat::Agent) << "压缩跳过" << logf("reason", skipReason);
        finishSkipped(QStringLiteral("压缩跳过：") + skipReason);
        return;
    }

    m_selectedEntries.clear();
    m_selectedEntries.reserve(m_compactedIds.size());
    for (const ConversationMessage &entry : ledger->entries()) {
        if (m_compactedIds.contains(entry.id)) {
            m_selectedEntries.append(entry);
        }
    }

    m_ledger = ledger;
    m_summaryText.clear();
    m_lastBulkSummaryText.clear();
    m_lastBulkCompactedIds.clear();
    m_retryCount = 0;
    m_summaryOnly = false;
    m_running = true;

    // 开始压缩：仅日志；ContextCompacted 只在真正改写账本后发（成功摘要 / 降级截断）
    LOGI(LogCat::Agent) << "压缩引擎启动"
        << logf("entries", m_compactedIds.size());
    startRequest();
}

void CompactEngine::startSummaryOnly(
    const QList<ConversationMessage> &snapshot,
    const QString &credentialInstanceId,
    ProviderCredential *credentialStore,
    const std::function<std::unique_ptr<AbstractProvider>(const QString &)> &providerFactory,
    const QString &modelName,
    AbstractProvider *activeProvider
)
{
    if (m_running) {
        LOGW(LogCat::Agent) << "压缩引擎已在运行，忽略 summaryOnly";
        emit summaryOnlyFinished(false, QStringLiteral("压缩引擎忙"));
        return;
    }
    if (snapshot.isEmpty()) {
        emit summaryOnlyFinished(false, QStringLiteral("段摘要快照为空"));
        return;
    }
    QString skipReason;
    if (!prepareTempProvider(credentialInstanceId, credentialStore, providerFactory,
                             modelName, activeProvider, &skipReason)) {
        LOGW(LogCat::Agent) << "段摘要跳过" << logf("reason", skipReason);
        emit summaryOnlyFinished(false, skipReason);
        return;
    }

    m_ledger = nullptr;
    m_compactedIds.clear();
    m_selectedEntries = snapshot;
    m_summaryText.clear();
    m_retryCount = 0;
    m_summaryOnly = true;
    m_running = true;

    LOGI(LogCat::Agent) << "段摘要引擎启动"
        << logf("entries", m_selectedEntries.size());
    startRequest();
}

void CompactEngine::cancel()
{
    if (!m_running) {
        return;
    }

    const bool summaryOnly = m_summaryOnly;
    m_retryTimer.stop();
    QObject::disconnect(m_providerConnection);
    if (m_tempProvider) {
        m_tempProvider->cancel();
    }
    resetState();
    if (summaryOnly) {
        emit summaryOnlyFinished(false, QStringLiteral("段摘要已取消。"));
        return;
    }
    emit compactionFailed(QStringLiteral("压缩已取消。"));
    pushEvent(core_ir::EventWarning{{}, QStringLiteral("压缩已取消。")});
    emit compactionFinished(false);
}

bool CompactEngine::isRunning() const
{
    return m_running;
}

QList<QString> CompactEngine::selectEntriesToCompact(const ProviderRunLedger &ledger) const
{
    // 源头：token 前缀 + 工具对原子闭合（Call/Result 同 mark，禁止半对）
    return CompactToolPair::selectPrefixToCompact(ledger.entries(), config.targetTokenCount);
}

void CompactEngine::startRequest()
{
    if (!m_running || !m_tempProvider) {
        return;
    }

    QObject::disconnect(m_providerConnection);

    m_summaryText.clear();

    m_providerConnection = connect(m_tempProvider.get(), &AbstractProvider::eventEmitted, this, [this](const ProviderEvent &event) {
        if (event.kind == ProviderEventKind::TextDelta) {
            m_summaryText += event.deltaPayload.text;
        } else if (event.kind == ProviderEventKind::MessageCompleted) {
            finishWithSummary();
        } else if (event.kind == ProviderEventKind::Error) {
            scheduleRetry(event.error.message);
        } else if (event.kind == ProviderEventKind::Cancelled) {
            scheduleRetry(QStringLiteral("压缩请求被取消。"));
        }
    });

    ProviderRequest request;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // 段摘要与大压提示词分离：有注入拼装器则走槽位追加，否则只用内置模板
    if (m_summaryOnly) {
        request.systemPrompt = m_promptBuilder
            ? m_promptBuilder->segmentSystemPrompt()
            : SystemPromptBuilder::builtinSegmentSystemPrompt();
    } else {
        request.systemPrompt = m_promptBuilder
            ? m_promptBuilder->compactSystemPrompt()
            : SystemPromptBuilder::builtinCompactSystemPrompt();
    }
    // 段摘要与大压：规则文档材料 + 任务句，单条 UserText（非 API role 回放）
    request.items = CompactEngine::buildDocumentCompactInput(
        m_selectedEntries, config.userMessageTokenBudget);
    request.tools = {};
    request.maxOutputTokens = config.maxOutputTokens;
    request.desiredOutput = ProviderOutputSpec::textOnly();
    m_tempProvider->sendRequestWithoutModelRefresh(request);
}

void CompactEngine::scheduleRetry(const QString &reason)
{
    if (!m_running) {
        return;
    }

    QObject::disconnect(m_providerConnection);

    if (m_retryCount >= config.maxRetries) {
        finishWithFailure(reason);
        return;
    }

    ++m_retryCount;
    const int delayMs = m_retryCount * 2000;
    LOGW(LogCat::Agent) << "压缩 LLM 调用失败，稍后重试"
        << logf("delayMs", delayMs)
        << logf("retry", m_retryCount)
        << logf("maxRetries", config.maxRetries)
        << logf("reason", reason);
    m_retryTimer.start(delayMs);
}

void CompactEngine::finishWithSummary()
{
    if (!m_running) {
        return;
    }

    QObject::disconnect(m_providerConnection);

    const SummaryValidation validation = CompactEngine::validateSummaryText(m_summaryText);
    if (!validation.ok) {
        LOGW(LogCat::Agent) << "摘要正文校验失败"
            << logf("reason", validation.reason)
            << logf("summaryChars", m_summaryText.size())
            << logf("preview", m_summaryText.left(120));
        scheduleRetry(QStringLiteral("摘要校验失败: ") + validation.reason);
        return;
    }

    if (m_summaryOnly) {
        LOGI(LogCat::Agent) << "段摘要完成"
            << logf("entries", m_selectedEntries.size())
            << logf("summaryChars", m_summaryText.size());
        const QString text = m_summaryText;
        resetState();
        emit summaryOnlyFinished(true, text);
        return;
    }

    if (!m_ledger) {
        resetState();
        emit compactionFinished(false);
        return;
    }

    applyCompaction(*m_ledger, m_compactedIds, m_summaryText);
    LOGI(LogCat::Agent) << "压缩完成"
        << logf("entries", m_compactedIds.size())
        << logf("summaryChars", m_summaryText.size());

    // 供 Agent §5.3 写 bulk 进摘要库（reset 会清运行态，先落档）
    m_lastBulkSummaryText = m_summaryText;
    m_lastBulkCompactedIds = m_compactedIds;

    resetState();
    // ContextCompacted 由 Agent 带摘要库可观测字段发出，避免双发
    emit compactionFinished(true);
}

void CompactEngine::finishWithFailure(const QString &reason)
{
    if (!m_running) {
        return;
    }

    QObject::disconnect(m_providerConnection);

    if (m_summaryOnly) {
        LOGW(LogCat::Agent) << "段摘要失败"
            << logf("reason", reason);
        resetState();
        emit summaryOnlyFinished(false, reason);
        return;
    }

    if (!m_ledger) {
        resetState();
        emit compactionFinished(false);
        return;
    }

    LOGW(LogCat::Agent) << "压缩失败，降级为截断"
        << logf("reason", reason);
    const bool truncated = truncateOldestRoundTrip(*m_ledger);
    emit compactionFailed(reason);
    pushEvent(core_ir::EventWarning{{}, reason});
    resetState();
    // 截断成功：Agent 侧发 ContextCompacted(reason=truncate)
    emit compactionFinished(truncated);
}

void CompactEngine::finishSkipped(const QString &userMessage)
{
    // start() 同步跳过路径不会置 m_summaryOnly；段摘要失败走 summaryOnlyFinished 直发
    pushEvent(core_ir::EventWarning{{}, userMessage});
    emit compactionFinished(false);
}

void CompactEngine::resetState()
{
    m_retryTimer.stop();
    m_ledger = nullptr;
    m_tempProvider.reset();
    m_compactedIds.clear();
    m_selectedEntries.clear();
    m_summaryText.clear();
    m_retryCount = 0;
    m_running = false;
    m_summaryOnly = false;
}

void CompactEngine::applyCompaction(ProviderRunLedger &ledger,
                                    const QList<QString> &compactedIds,
                                    const QString & /*summaryText*/)
{
    // 摘要正文由 Agent 写入 SummaryStore；账本侧只 mark 覆盖段（不物化 Summary 条目）
    ledger.markEntriesCompacted(compactedIds);
}

bool CompactEngine::truncateOldestRoundTrip(ProviderRunLedger &ledger)
{
    QList<QString> ids;
    for (const ConversationMessage &entry : ledger.entries()) {
        if (!entry.submittedToModel || entry.wasCompacted || entry.isCompactExempt()) {
            continue;
        }
        ids.append(entry.id);
        // 凑满「最旧一轮」：再遇到 UserText 即止（首条 UserText 本身仍纳入）
        if (entry.kind == ConversationMessage::Kind::UserText && ids.size() > 1) {
            break;
        }
    }

    if (ids.isEmpty()) {
        return false;
    }

    // 与 bulk 选型同源：截断切口不得拆开 tool 对
    QSet<QString> selected(ids.cbegin(), ids.cend());
    CompactToolPair::closeSelection(ledger.entries(), selected, nullptr);
    ids.clear();
    ids.reserve(selected.size());
    for (const ConversationMessage &entry : ledger.entries()) {
        if (selected.contains(entry.id)) {
            ids.append(entry.id);
        }
    }

    ledger.markEntriesCompacted(ids);
    LOGI(LogCat::Agent) << "降级截断完成"
        << logf("removed", ids.size());
    return true;
}

QString CompactEngine::buildDocumentMaterial(
    const QList<ConversationMessage> &entries,
    const qint64 tokenBudget)
{
    const qint64 budget = qMax<qint64>(0, tokenBudget);
    if (budget <= 0 || entries.isEmpty()) {
        return {};
    }

    // 预建 callId → 首个 ToolResult 下标（工具对同进同出）
    QHash<QString, int> firstResultIndexByCallId;
    for (int i = 0; i < entries.size(); ++i) {
        const ConversationMessage &e = entries.at(i);
        if (e.kind != ConversationMessage::Kind::ToolResult) {
            continue;
        }
        const QString callId = callIdOf(e);
        if (callId.isEmpty() || firstResultIndexByCallId.contains(callId)) {
            continue;
        }
        firstResultIndexByCallId.insert(callId, i);
    }

    QList<MaterialBlock> blocks;
    QSet<int> consumedResultIndices;

    for (int i = 0; i < entries.size(); ++i) {
        if (consumedResultIndices.contains(i)) {
            continue;
        }
        const ConversationMessage &entry = entries.at(i);
        MaterialBlock block;
        block.order = i;

        switch (entry.kind) {
        case ConversationMessage::Kind::UserText: {
            const QString body = entry.text.trimmed();
            if (body.isEmpty()) {
                continue;
            }
            block.tier = MaterialTier::User;
            block.text = QStringLiteral("## 用户\n") + body;
            break;
        }
        case ConversationMessage::Kind::AssistantText: {
            const QString body = entry.text.trimmed();
            if (body.isEmpty()) {
                continue;
            }
            const QString capped = trimTextToBudget(body, kAssistantCapTokens);
            if (capped.isEmpty()) {
                continue;
            }
            block.tier = MaterialTier::Assistant;
            block.text = QStringLiteral("## 助手\n") + capped;
            break;
        }
        case ConversationMessage::Kind::SkillInvoke: {
            const QString name = toolNameOf(entry);
            QString body = entry.text.trimmed();
            if (!body.isEmpty()) {
                body = trimTextToBudget(body, kAssistantCapTokens);
            }
            block.tier = MaterialTier::Skill;
            block.text = QStringLiteral("## 技能 ") + name;
            if (!body.isEmpty()) {
                block.text += QLatin1Char('\n') + body;
            }
            break;
        }
        case ConversationMessage::Kind::ToolCall: {
            const QString callId = callIdOf(entry);
            const int resultIdx = callId.isEmpty()
                ? -1
                : firstResultIndexByCallId.value(callId, -1);
            const QString name = toolNameOf(entry);
            const QString target = resolveCallTarget(entry);
            QString text = QStringLiteral("## 工具调用 ") + name
                + QStringLiteral("\n对象: ") + target;
            if (resultIdx > i && !consumedResultIndices.contains(resultIdx)) {
                const ConversationMessage &result = entries.at(resultIdx);
                text += QStringLiteral("\n## 工具结果 ") + toolNameOf(result)
                    + QStringLiteral(" [") + resultStatusLabel(result) + QStringLiteral("]\n")
                    + resolveShortOutcome(result);
                consumedResultIndices.insert(resultIdx);
            }
            block.tier = MaterialTier::Tool;
            block.text = text;
            break;
        }
        case ConversationMessage::Kind::ToolResult: {
            // 孤立结果（无前序 call 或 call 已消费）：单独写结果行
            const QString name = toolNameOf(entry);
            block.tier = MaterialTier::Tool;
            block.text = QStringLiteral("## 工具结果 ") + name
                + QStringLiteral(" [") + resultStatusLabel(entry) + QStringLiteral("]\n")
                + resolveShortOutcome(entry);
            break;
        }
        case ConversationMessage::Kind::AssistantReasoning:
        case ConversationMessage::Kind::SystemPrompt:
        case ConversationMessage::Kind::Summary:
        case ConversationMessage::Kind::SessionEvent:
        case ConversationMessage::Kind::Error:
        case ConversationMessage::Kind::ApprovalRequest:
        case ConversationMessage::Kind::AgentTask:
        default:
            // 推理噪声 / 系统 / 会话事件等一律不进材料
            continue;
        }

        if (block.text.trimmed().isEmpty()) {
            continue;
        }
        blocks.append(std::move(block));
    }

    if (blocks.isEmpty()) {
        return {};
    }

    // 预算：先保全部用户，再按时间序填其余；超预算砍最早非用户
    QSet<int> selectedOrders;
    qint64 used = 0;

    auto tryAdd = [&](MaterialBlock &b, const bool allowTrim) -> bool {
        qint64 cost = estimateContextTokensForText(b.text);
        if (used + cost <= budget) {
            selectedOrders.insert(b.order);
            used += cost;
            return true;
        }
        if (!allowTrim) {
            return false;
        }
        const qint64 remain = budget - used;
        if (remain <= 0) {
            return false;
        }
        // 仅对用户块做条内截断（保留意图碎片）
        const QString header = QStringLiteral("## 用户\n");
        QString body = b.text;
        if (body.startsWith(header)) {
            body = body.mid(header.size());
        }
        const QString trimmedBody = trimTextToBudget(body, remain - estimateContextTokensForText(header));
        if (trimmedBody.isEmpty()) {
            return false;
        }
        b.text = header + trimmedBody;
        cost = estimateContextTokensForText(b.text);
        if (used + cost > budget) {
            return false;
        }
        selectedOrders.insert(b.order);
        used += cost;
        return true;
    };

    // Pass 1：用户
    for (MaterialBlock &b : blocks) {
        if (b.tier == MaterialTier::User) {
            tryAdd(b, /*allowTrim=*/true);
        }
    }
    // Pass 2：非用户按原序
    for (MaterialBlock &b : blocks) {
        if (b.tier != MaterialTier::User) {
            tryAdd(b, /*allowTrim=*/false);
        }
    }

    QStringList parts;
    for (const MaterialBlock &b : blocks) {
        if (selectedOrders.contains(b.order)) {
            parts.append(b.text);
        }
    }
    return parts.join(QStringLiteral("\n\n"));
}

QList<ProviderItem> CompactEngine::buildDocumentCompactInput(
    const QList<ConversationMessage> &entries,
    const qint64 tokenBudget)
{
    // 任务外壳占少量预算；材料用剩余额度
    const QString emptyShell = taskShell(QString());
    const qint64 shellTokens = estimateContextTokensForText(emptyShell);
    const qint64 materialBudget = qMax<qint64>(0, tokenBudget - shellTokens);
    const QString material = buildDocumentMaterial(entries, materialBudget);
    if (material.isEmpty()) {
        // 材料为空时仍给任务句，避免 provider 收到空 user
        return {ProviderItem::makeUserText(taskShell(QStringLiteral("（无可摘要材料）")))};
    }
    return {ProviderItem::makeUserText(taskShell(material))};
}

SummaryValidation CompactEngine::validateSummaryText(const QString &text)
{
    SummaryValidation v;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        v.reason = QStringLiteral("empty");
        return v;
    }
    if (trimmed.size() < kMinSummaryChars) {
        v.reason = QStringLiteral("too_short");
        return v;
    }

    // DSML / 工具调用壳（含真实故障样例特征）
    static const QRegularExpression kJunkPatterns(
        QString::fromUtf8(
            R"((?:<\|tool|</?tool_call|tool_calls\b|FunctionCall\b|<minimax|)"
            R"(invoke\s+(?:tool|function)|)"
            R"(│\s*(?:read_file|write_file|run_command|bash|grep|glob)\b|)"
            R"(\bDSML\b|)"
            R"(<function_calls>|<parameter\s))"),
        QRegularExpression::CaseInsensitiveOption);

    if (kJunkPatterns.match(trimmed).hasMatch()) {
        v.reason = QStringLiteral("tool_or_dsml_shell");
        return v;
    }

    // 伪续聊：无 Markdown 小节，且以续聊口吻开场
    const bool hasSection = trimmed.contains(QStringLiteral("## "));
    if (!hasSection) {
        static const QRegularExpression kChatOpen(
            QString::fromUtf8(
                R"(^(?:好的[，,]?\s*我来|让我来|我来帮你|I will (?:call|use|help)|Sure[,!]?\s+I\s+will))"),
            QRegularExpression::CaseInsensitiveOption);
        if (kChatOpen.match(trimmed).hasMatch()) {
            v.reason = QStringLiteral("chatty_without_sections");
            return v;
        }
    }

    v.ok = true;
    return v;
}
