#include "SummaryJobQueue.h"

#include "CompactEngine.h"
#include "logging/LogManager.h"

#include <QSet>
#include <QUuid>

SummaryJobQueue::SummaryJobQueue(QObject *parent)
    : QObject(parent)
{
}

SummaryJobQueue::~SummaryJobQueue()
{
    abortRunning();
    m_jobs.clear();
}

void SummaryJobQueue::setCompactEngine(CompactEngine *engine)
{
    if (m_engine == engine) {
        return;
    }
    if (m_engine) {
        disconnect(m_engine, nullptr, this, nullptr);
    }
    m_engine = engine;
    if (m_engine) {
        connect(m_engine, &CompactEngine::summaryOnlyFinished,
                this, &SummaryJobQueue::onSummaryOnlyFinished);
    }
}

void SummaryJobQueue::setProviderContext(const QString &credentialInstanceId,
                                         ProviderCredential *credentialStore,
                                         ProviderFactory factory,
                                         const QString &modelName,
                                         AbstractProvider *activeProvider)
{
    m_credentialInstanceId = credentialInstanceId;
    m_credentialStore = credentialStore;
    m_providerFactory = std::move(factory);
    m_modelName = modelName;
    m_activeProvider = activeProvider;
}

void SummaryJobQueue::setCompactConfig(const CompactConfig &config)
{
    m_config = config;
}

QString SummaryJobQueue::enqueue(const QList<QString> &spanEntryIds,
                                 const QList<ConversationMessage> &snapshot)
{
    if (spanEntryIds.isEmpty() || snapshot.isEmpty()) {
        return {};
    }

    // 相邻未覆盖段合并：末尾仍为 Pending（含 abort 回 Pending）则并入同一 job
    if (!m_jobs.isEmpty() && m_jobs.last().state == SummaryJobState::Pending) {
        SummaryJob &tail = m_jobs.last();
        for (const QString &id : spanEntryIds) {
            if (!tail.spanEntryIds.contains(id)) {
                tail.spanEntryIds.append(id);
            }
        }
        QSet<QString> snapSeen;
        for (const ConversationMessage &old : tail.payloadSnapshot) {
            snapSeen.insert(old.id);
        }
        for (const ConversationMessage &m : snapshot) {
            if (snapSeen.contains(m.id)) {
                continue;
            }
            snapSeen.insert(m.id);
            tail.payloadSnapshot.append(m);
        }
        LOGI(LogCat::Agent) << "段摘要合并入队"
            << logf("jobId", tail.jobId)
            << logf("span", tail.spanEntryIds.size())
            << logf("queue", m_jobs.size());
        return tail.jobId;
    }

    SummaryJob job;
    job.jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.spanEntryIds = spanEntryIds;
    job.payloadSnapshot = snapshot;
    job.state = SummaryJobState::Pending;
    m_jobs.append(std::move(job));
    LOGI(LogCat::Agent) << "段摘要入队"
        << logf("jobId", m_jobs.last().jobId)
        << logf("span", spanEntryIds.size())
        << logf("queue", m_jobs.size());
    return m_jobs.last().jobId;
}

void SummaryJobQueue::kick()
{
    if (m_waitingEngine || hasRunning()) {
        return;
    }
    startNext(/*resumeFailed=*/true);
}

void SummaryJobQueue::abortRunning()
{
    // 先置 aborting：CompactEngine::cancel 会同步 emit summaryOnlyFinished(false)
    // 若未拦截，finishCurrent 会把 Running 误标 Failed（G4a/G4b 要求仅 abort≠失败）
    m_aborting = true;
    const int idx = indexOfRunning();
    if (idx >= 0) {
        m_jobs[idx].state = SummaryJobState::Pending; // 进度丢弃，任务保留可再跑
        m_jobs[idx].failReason.clear();
        LOGI(LogCat::Agent) << "段摘要在飞任务已中止，队列保留"
            << logf("jobId", m_jobs.at(idx).jobId);
    }
    m_waitingEngine = false;
    if (m_engine && m_engine->isRunning()) {
        m_engine->cancel();
    }
    m_aborting = false;
}

void SummaryJobQueue::clear()
{
    abortRunning();
    m_jobs.clear();
    m_waitingEngine = false;
    LOGI(LogCat::Agent) << "段摘要队列已清空";
}

bool SummaryJobQueue::isIdle() const
{
    return !hasPendingOrRunning() && !m_waitingEngine;
}

bool SummaryJobQueue::hasPendingOrRunning() const
{
    for (const SummaryJob &job : m_jobs) {
        if (job.state == SummaryJobState::Pending
            || job.state == SummaryJobState::Running
            || job.state == SummaryJobState::Failed) {
            // Failed 仍算「未完成」：边界需等/清队；kick 会 resume
            return true;
        }
    }
    return false;
}

bool SummaryJobQueue::hasFailed() const
{
    for (const SummaryJob &job : m_jobs) {
        if (job.state == SummaryJobState::Failed) {
            return true;
        }
    }
    return false;
}

bool SummaryJobQueue::hasRunning() const
{
    return indexOfRunning() >= 0 || m_waitingEngine;
}

int SummaryJobQueue::jobCount() const
{
    return static_cast<int>(m_jobs.size());
}

void SummaryJobQueue::onSummaryOnlyFinished(const bool success, const QString &text)
{
    if (m_aborting || !m_waitingEngine) {
        return;
    }
    finishCurrent(success, text);
}

void SummaryJobQueue::startNext(const bool resumeFailed)
{
    if (!m_engine || m_waitingEngine) {
        return;
    }
    const int idx = indexOfNextRunnable(resumeFailed);
    if (idx < 0) {
        emit queueDrained();
        return;
    }

    SummaryJob &job = m_jobs[idx];
    job.state = SummaryJobState::Running;
    job.failReason.clear();
    m_waitingEngine = true;

    m_engine->config = m_config;
    m_engine->startSummaryOnly(
        job.payloadSnapshot,
        m_credentialInstanceId,
        m_credentialStore,
        m_providerFactory,
        m_modelName,
        m_activeProvider);
}

void SummaryJobQueue::finishCurrent(const bool success, const QString &text)
{
    m_waitingEngine = false;
    const int idx = indexOfRunning();
    if (idx < 0) {
        return;
    }
    SummaryJob &job = m_jobs[idx];
    const QString jobId = job.jobId;
    const QList<QString> spanIds = job.spanEntryIds;
    if (success && !text.trimmed().isEmpty()) {
        job.state = SummaryJobState::Done;
        emit jobFinished(jobId, true, text, spanIds);
        m_jobs.removeAt(idx);
    } else {
        job.state = SummaryJobState::Failed;
        job.failReason = text.trimmed().isEmpty()
            ? QStringLiteral("段摘要失败或空结果")
            : text;
        emit jobFinished(jobId, false, job.failReason, spanIds);
    }

    // 同轮只继续 pending，不立刻重跑刚失败的任务（避免死循环）
    startNext(/*resumeFailed=*/false);
}

int SummaryJobQueue::indexOfRunning() const
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs.at(i).state == SummaryJobState::Running) {
            return i;
        }
    }
    return -1;
}

int SummaryJobQueue::indexOfNextRunnable(const bool resumeFailed) const
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs.at(i).state == SummaryJobState::Pending) {
            return i;
        }
    }
    if (!resumeFailed) {
        return -1;
    }
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs.at(i).state == SummaryJobState::Failed) {
            return i;
        }
    }
    return -1;
}
