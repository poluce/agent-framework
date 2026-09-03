#pragma once

#include "config/CompactConfig.h"
#include "SummaryStore.h"
#include "types/ConversationMessage.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class CompactEngine;
class AbstractProvider;
class ProviderCredential;

/// 段摘要任务状态
enum class SummaryJobState {
    Pending,
    Running,
    Failed,
    Done,
};

struct SummaryJob {
    QString jobId;
    QList<QString> spanEntryIds;
    QList<ConversationMessage> payloadSnapshot;
    SummaryJobState state = SummaryJobState::Pending;
    QString failReason;
};

/**
 * 主代理串行段摘要队列。
 * - 入队时冻结快照（D18）
 * - 同时最多一个旁路 LLM 调用
 * - abortRunning ≠ clear（G4a / G7）
 */
class SummaryJobQueue : public QObject
{
    Q_OBJECT

public:
    using ProviderFactory = std::function<std::unique_ptr<AbstractProvider>(const QString &)>;

    explicit SummaryJobQueue(QObject *parent = nullptr);
    ~SummaryJobQueue() override;

    void setCompactEngine(CompactEngine *engine);
    void setProviderContext(const QString &credentialInstanceId,
                            ProviderCredential *credentialStore,
                            ProviderFactory factory,
                            const QString &modelName,
                            AbstractProvider *activeProvider);
    void setCompactConfig(const CompactConfig &config);

    /// 入队一段；返回 jobId（空=拒绝）
    QString enqueue(const QList<QString> &spanEntryIds,
                    const QList<ConversationMessage> &snapshot);

    void kick();
    /// 仅中止在飞 job（进度丢弃）；pending/failed 保留
    void abortRunning();
    /// 清空全部任务并中止在飞
    void clear();

    [[nodiscard]] bool isIdle() const;
    [[nodiscard]] bool hasPendingOrRunning() const;
    [[nodiscard]] bool hasFailed() const;
    [[nodiscard]] bool hasRunning() const;
    [[nodiscard]] int jobCount() const;
    [[nodiscard]] const QList<SummaryJob> &jobs() const { return m_jobs; }

signals:
    /// 单个 job 完成（成功时 summaryText 为正文；失败为原因）
    void jobFinished(const QString &jobId, bool success, const QString &summaryText,
                     const QList<QString> &spanEntryIds);
    /// 队列从「有待做」变为空闲（全部 done 出队或仅剩 failed 且无 running）
    void queueDrained();

private slots:
    void onSummaryOnlyFinished(bool success, const QString &text);

private:
    void startNext(bool resumeFailed = false);
    void finishCurrent(bool success, const QString &text);
    int indexOfRunning() const;
    /// resumeFailed=false 时只取 Pending；true 时 Pending 优先，否则 Failed
    int indexOfNextRunnable(bool resumeFailed) const;

    CompactEngine *m_engine = nullptr;
    QList<SummaryJob> m_jobs;
    QString m_credentialInstanceId;
    ProviderCredential *m_credentialStore = nullptr;
    ProviderFactory m_providerFactory;
    QString m_modelName;
    AbstractProvider *m_activeProvider = nullptr;
    CompactConfig m_config;
    bool m_waitingEngine = false;
    /// abortRunning 期间忽略引擎完成回调，避免 cancel 把 job 误标 Failed
    bool m_aborting = false;
};
