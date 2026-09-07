#include "CompactPipeline.h"

#include "CompactEngine.h"
#include "ModelViewAssembler.h"
#include "ModelViewStore.h"
#include "SummaryJobQueue.h"
#include "SummaryStore.h"
#include "agent/AbstractLoop.h"
#include "logging/LogManager.h"

#include <QDateTime>
#include <QUuid>

CompactPipeline::CompactPipeline(const QString &agentId, QObject *parent)
    : QObject(parent)
    , m_agentId(agentId)
    , m_engine(std::make_unique<CompactEngine>())
    , m_summaryStore(std::make_unique<SummaryStore>())
    , m_modelViewStore(std::make_unique<ModelViewStore>())
{
    connect(m_engine.get(), &CompactEngine::compactionFinished, this,
            &CompactPipeline::onCompactionFinished);
    connect(m_engine.get(), &CompactEngine::compactionFailed, this,
            &CompactPipeline::onCompactionFailed);
    m_engine->addProtocolHandler([this](const core_ir::Event &event,
                                        const core_ir::EventContext &context,
                                        const core_ir::SubmissionId &submissionId) {
        Q_UNUSED(context);
        Q_UNUSED(submissionId);
        emit protocolEvent(event);
    });
}

CompactPipeline::~CompactPipeline() = default;

void CompactPipeline::setLoop(AbstractLoop *loop)
{
    m_loop = loop;
}

void CompactPipeline::setRuntime(const SessionRuntime &runtime)
{
    m_runtime = runtime;
}

void CompactPipeline::setCredentialStore(ProviderCredential *store)
{
    m_credentialStore = store;
}

void CompactPipeline::setProviderFactory(ProviderFactory factory)
{
    m_providerFactory = std::move(factory);
}

void CompactPipeline::setPromptBuilder(SystemPromptBuilder *builder)
{
    m_promptBuilder = builder;
    if (m_engine) {
        m_engine->setPromptBuilder(builder);
    }
}

void CompactPipeline::ensureInstalled(const bool usesSegmentSummary)
{
    if (m_queue || !usesSegmentSummary) {
        return;
    }
    m_queue = std::make_unique<SummaryJobQueue>();
    m_queue->setCompactEngine(m_engine.get());
    connect(m_queue.get(), &SummaryJobQueue::jobFinished, this,
            &CompactPipeline::onSummaryJobFinished);
    connect(m_queue.get(), &SummaryJobQueue::queueDrained, this,
            &CompactPipeline::resumeBoundaryAfterSummaryDrain);
}

void CompactPipeline::onCompactionRequested(const qint64 currentTokens, const qint64 threshold)
{
    LOGI(LogCat::Agent) << "收到压缩请求"
        << logf("agentId", m_agentId)
        << logf("currentTokens", currentTokens)
        << logf("threshold", threshold);

    m_manualCompaction = false;
    m_boundaryThreshold = threshold;

    if (!m_queue) {
        startCompactionEngine();
        return;
    }

    onBoundaryCompactionRequested(threshold);
}

void CompactPipeline::onBoundaryCompactionRequested(const qint64 threshold)
{
    m_boundaryThreshold = threshold;

    if (!summaryFeaturesEnabled()) {
        clearSummaryQueueForBulk();
        startCompactionEngine();
        return;
    }

    if (m_queue->hasPendingOrRunning()) {
        LOGI(LogCat::Agent) << "边界等待段摘要队列"
            << logf("agentId", m_agentId)
            << logf("jobs", m_queue->jobCount());
        m_waitingSummaryAtBoundary = true;
        if (m_loop) {
            m_loop->beginBoundarySummaryWait();
        }
        configureAndKickSummaryQueue();
        return;
    }

    if (tryContinueWithAssembledView(threshold, true)) {
        return;
    }

    clearSummaryQueueForBulk();
    startCompactionEngine();
}

bool CompactPipeline::requestManualCompaction(const qint64 targetTokens)
{
    if (!m_loop || !m_engine) {
        return false;
    }
    if (m_manualCompaction) {
        LOGW(LogCat::Agent) << "手动压缩拒绝：已在手动压缩"
            << logf("agentId", m_agentId);
        return false;
    }

    m_waitingSummaryAtBoundary = false;
    if (m_loop->isWaitingBoundarySummary()) {
        m_loop->endBoundarySummaryWait(true);
    }
    clearSummaryQueueForBulk();

    if (m_loop->isBusy() || m_engine->isRunning()) {
        LOGW(LogCat::Agent) << "手动压缩拒绝：忙"
            << logf("agentId", m_agentId)
            << logf("loopBusy", m_loop->isBusy())
            << logf("engineRunning", m_engine->isRunning());
        return false;
    }

    LOGI(LogCat::Agent) << "手动压缩开始"
        << logf("agentId", m_agentId)
        << logf("targetTokens", targetTokens);

    m_manualCompaction = true;
    m_loop->beginManualCompaction();
    startCompactionEngine(targetTokens);
    return true;
}

bool CompactPipeline::cancelBoundaryWait()
{
    if (!m_waitingSummaryAtBoundary) {
        return false;
    }
    m_waitingSummaryAtBoundary = false;
    if (m_queue) {
        m_queue->abortRunning();
    }
    if (m_loop) {
        m_loop->endBoundarySummaryWait(true);
    }
    return true;
}

void CompactPipeline::cancelInFlight()
{
    if (m_queue && m_queue->hasRunning()) {
        m_queue->abortRunning();
    }
    if (m_engine && m_engine->isRunning()) {
        m_engine->cancel();
    }
    m_manualCompaction = false;
}

void CompactPipeline::clear()
{
    m_waitingSummaryAtBoundary = false;
    if (m_queue) {
        m_queue->clear();
    }
    m_summaryStore->clear();
    m_modelViewStore->clear();
    m_lastSummarizedEntryId.clear();
    m_lastEnqueuedEntryId.clear();
    if (m_loop) {
        m_loop->clearModelViewPrefix();
    }
}

void CompactPipeline::onTurnSucceeded()
{
    if (!m_queue) {
        return;
    }
    maybeEnqueueSegmentSummary();
    if (m_queue->hasPendingOrRunning()) {
        configureAndKickSummaryQueue();
    }
}

bool CompactPipeline::hasQueue() const
{
    return m_queue != nullptr;
}

int CompactPipeline::jobCount() const
{
    return m_queue ? m_queue->jobCount() : 0;
}

bool CompactPipeline::waitingAtBoundary() const
{
    return m_waitingSummaryAtBoundary;
}

bool CompactPipeline::storeEmpty() const
{
    return m_summaryStore->isEmpty();
}

int CompactPipeline::recordCount() const
{
    return m_summaryStore->recordCount();
}

qint64 CompactPipeline::addedTokens() const
{
    if (!summaryFeaturesEnabled() || !m_loop) {
        return 0;
    }
    return ModelViewAssembler::estimateTokensSince(m_loop->ledger(), segmentSummaryCursor());
}

bool CompactPipeline::hasFailedJobs() const
{
    return m_queue && m_queue->hasFailed();
}

QJsonObject CompactPipeline::exportState() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("summaryStore"), m_summaryStore->toJson());
    obj.insert(QStringLiteral("modelView"), m_modelViewStore->toJson());
    obj.insert(QStringLiteral("lastSummarizedEntryId"), m_lastSummarizedEntryId);
    obj.insert(QStringLiteral("lastEnqueuedEntryId"), m_lastEnqueuedEntryId);
    return obj;
}

void CompactPipeline::importState(const QJsonObject &obj)
{
    if (!m_queue) {
        clear();
        return;
    }
    m_summaryStore->fromJson(obj.value(QStringLiteral("summaryStore")).toObject());
    m_modelViewStore->fromJson(obj.value(QStringLiteral("modelView")).toObject());
    m_lastSummarizedEntryId = obj.value(QStringLiteral("lastSummarizedEntryId")).toString();
    m_lastEnqueuedEntryId = obj.value(QStringLiteral("lastEnqueuedEntryId")).toString();
    syncModelViewPrefixFromStore();
}

void CompactPipeline::clearSummaryQueueForBulk()
{
    if (m_queue) {
        m_queue->clear();
    }
    m_lastEnqueuedEntryId = m_lastSummarizedEntryId;
}

void CompactPipeline::configureAndKickSummaryQueue()
{
    if (!m_queue || !m_loop) {
        return;
    }
    m_queue->setProviderContext(
        m_runtime.credentialInstanceId,
        m_credentialStore,
        m_providerFactory,
        m_runtime.modelName,
        m_loop->provider());
    m_queue->setCompactConfig(m_runtime.toCompactConfig());
    m_queue->kick();
}

bool CompactPipeline::summaryFeaturesEnabled() const
{
    return m_queue
        && m_runtime.summaryEnabled && m_runtime.compactEnabled;
}

QString CompactPipeline::segmentSummaryCursor() const
{
    return !m_lastEnqueuedEntryId.isEmpty()
        ? m_lastEnqueuedEntryId
        : m_lastSummarizedEntryId;
}

bool CompactPipeline::tryContinueWithAssembledView(const qint64 threshold, const bool logWhenOver)
{
    if (m_summaryStore->isEmpty() || !m_loop) {
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

void CompactPipeline::startCompactionEngine(const qint64 targetTokensOverride)
{
    if (!m_loop || !m_engine) {
        return;
    }
    m_engine->config = m_runtime.toCompactConfig();
    if (targetTokensOverride > 0) {
        m_engine->config.targetTokenCount = targetTokensOverride;
    }
    m_engine->start(
        &m_loop->ledger(),
        m_runtime.credentialInstanceId,
        m_credentialStore,
        m_providerFactory,
        m_runtime.modelName,
        m_loop->provider());
}

void CompactPipeline::onCompactionFinished(const bool success)
{
    if (!success) {
        LOGW(LogCat::Agent) << "压缩未完全成功（降级处理）"
            << logf("agentId", m_agentId)
            << logf("manual", m_manualCompaction);
    }

    if (success) {
        const QString bulkText = m_engine->lastBulkSummaryText();
        const QList<QString> bulkIds = m_engine->lastBulkCompactedIds();
        if (m_queue && !bulkText.trimmed().isEmpty()) {
            SummaryRecord rec;
            rec.summaryId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            rec.spanEntryIds = bulkIds;
            rec.text = bulkText;
            rec.tokenEstimate = estimateContextTokensForText(bulkText);
            rec.createdAtMs = QDateTime::currentMSecsSinceEpoch();
            rec.source = QStringLiteral("bulk");
            m_summaryStore->replaceAll(std::move(rec));
            if (!m_summaryStore->lastCoveredEntryId().isEmpty()) {
                m_lastSummarizedEntryId = m_summaryStore->lastCoveredEntryId();
                m_lastEnqueuedEntryId = m_lastSummarizedEntryId;
            }
            syncModelViewPrefixFromStore();
            emitContextCompactedNotice(core_ir::CompactReason::Bulk);
        } else {
            emitContextCompactedNotice(bulkText.trimmed().isEmpty()
                                           ? core_ir::CompactReason::Truncate
                                           : core_ir::CompactReason::Bulk);
        }
    }

    const bool manual = m_manualCompaction;
    m_manualCompaction = false;
    m_waitingSummaryAtBoundary = false;

    if (manual && m_loop) {
        m_loop->endManualCompaction();
    }

    emit unitStateChanged();
    emit unitDataChanged();

    if (!manual && m_loop) {
        m_loop->continueAfterCompaction();
    }
}

void CompactPipeline::onCompactionFailed(const QString &reason)
{
    LOGW(LogCat::Agent) << "压缩失败"
        << logf("agentId", m_agentId)
        << logf("reason", reason)
        << logf("manual", m_manualCompaction);
}

void CompactPipeline::maybeEnqueueSegmentSummary()
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
    const QString jobId = m_queue->enqueue(spanIds, snapshot);
    if (jobId.isEmpty()) {
        return;
    }
    m_lastEnqueuedEntryId = spanIds.last();
    configureAndKickSummaryQueue();
    emit agentStateRefreshNeeded();
}

void CompactPipeline::onSummaryJobFinished(const QString &jobId, const bool success,
                                           const QString &summaryText,
                                           const QList<QString> &spanEntryIds)
{
    LOGI(LogCat::Agent) << "段摘要任务结束"
        << logf("jobId", jobId)
        << logf("success", success)
        << logf("chars", summaryText.size());

    if (!success) {
        return;
    }

    SummaryRecord rec;
    rec.summaryId = jobId;
    rec.spanEntryIds = spanEntryIds;
    rec.text = summaryText;
    rec.tokenEstimate = estimateContextTokensForText(summaryText);
    rec.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    rec.source = QStringLiteral("segment");
    m_summaryStore->append(std::move(rec));
    if (!spanEntryIds.isEmpty()) {
        m_lastSummarizedEntryId = spanEntryIds.last();
    }
    syncModelViewPrefixFromStore();
    emitContextCompactedNotice(core_ir::CompactReason::Segment);
    emit agentStateRefreshNeeded();
}

void CompactPipeline::resumeBoundaryAfterSummaryDrain()
{
    if (!m_waitingSummaryAtBoundary) {
        return;
    }
    m_waitingSummaryAtBoundary = false;
    if (m_loop) {
        m_loop->endBoundarySummaryWait(false);
    }

    if (m_queue && m_queue->hasFailed()) {
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

void CompactPipeline::applyAssembledModelView()
{
    if (!m_loop) {
        return;
    }
    const int recentTurns = m_runtime.summaryRecentTurns > 0 ? m_runtime.summaryRecentTurns : 5;
    const ModelViewAssembleResult assembled = ModelViewAssembler::assemble(
        m_loop->ledger(),
        *m_summaryStore,
        recentTurns);

    if (!assembled.ok) {
        LOGW(LogCat::Agent) << "组装模型视图失败"
            << logf("reason", assembled.failReason);
        return;
    }

    if (!assembled.entryIdsToCompact.isEmpty()) {
        m_loop->ledger().markEntriesCompacted(assembled.entryIdsToCompact);
    }

    syncModelViewPrefixFromStore();
    m_loop->refreshContextTokenEstimate();
    emitContextCompactedNotice(core_ir::CompactReason::Assemble);
    emit unitDataChanged();
}

void CompactPipeline::syncModelViewPrefixFromStore()
{
    if (!m_queue) {
        m_modelViewStore->clear();
        if (m_loop) {
            m_loop->clearModelViewPrefix();
        }
        return;
    }
    m_modelViewStore->syncFromSummaryStore(*m_summaryStore);

    QList<QString> prefixes = m_modelViewStore->prefixTexts();
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
            m_modelViewStore->setPrefixTexts(prefixes);
        }
    }

    if (m_loop) {
        m_loop->setModelViewPrefixTexts(m_modelViewStore->prefixTexts());
    }
}

void CompactPipeline::emitContextCompactedNotice(const core_ir::CompactReason reason)
{
    core_ir::EventContextCompacted ev;
    ev.reason = reason;
    ev.summaryRecordCount = m_summaryStore->recordCount();
    ev.modelViewPrefixCount = m_modelViewStore->count();
    ev.summaryTokenEstimate = m_summaryStore->totalTokenEstimate();
    emit protocolEvent(core_ir::Event{ev});
    LOGI(LogCat::Agent) << "ContextCompacted 可观测"
        << logf("agentId", m_agentId)
        << logf("reason", core_ir::compactReasonKey(reason))
        << logf("records", ev.summaryRecordCount)
        << logf("prefixes", ev.modelViewPrefixCount)
        << logf("summaryTokens", ev.summaryTokenEstimate);
}
