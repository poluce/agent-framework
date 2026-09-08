#pragma once

#include "config/SessionRuntime.h"
#include "types/CoreEvent.h"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class AbstractLoop;
class AbstractProvider;
class CompactEngine;
class ModelViewStore;
class ProviderCredential;
class SummaryJobQueue;
class SummaryStore;
class SystemPromptBuilder;

/**
 * 压缩管线：等段摘要队列 / 拼模型视图 / 开大压。
 * Agent 只转接 Loop 的 compactionRequested 与回合成功。
 */
class CompactPipeline : public QObject
{
    Q_OBJECT

public:
    using ProviderFactory = std::function<std::unique_ptr<AbstractProvider>(const QString &)>;

    explicit CompactPipeline(const QString &agentId, QObject *parent = nullptr);
    ~CompactPipeline() override;

    void setLoop(AbstractLoop *loop);
    void setRuntime(const SessionRuntime &runtime);
    void setCredentialStore(ProviderCredential *store);
    void setProviderFactory(ProviderFactory factory);
    void setPromptBuilder(SystemPromptBuilder *builder);

    /// 编排 usesSegmentSummary 时安装段摘要队列。
    void ensureInstalled(bool usesSegmentSummary);

    void onCompactionRequested(qint64 currentTokens, qint64 threshold);
    void onOverflowCompactionRequested();
    void onTurnSucceeded();
    [[nodiscard]] bool requestManualCompaction(qint64 targetTokens = -1);
    void clear();
    /// 边界等摘要：中止在飞、结束等待，返回 true（调用方不再 cancel Loop）。
    [[nodiscard]] bool cancelBoundaryWait();
    /// 途中取消：abort 段摘要在飞，停大压。
    void cancelInFlight();

    [[nodiscard]] bool hasQueue() const;
    [[nodiscard]] int jobCount() const;
    [[nodiscard]] bool waitingAtBoundary() const;
    [[nodiscard]] bool storeEmpty() const;
    [[nodiscard]] int recordCount() const;
    [[nodiscard]] qint64 addedTokens() const;
    [[nodiscard]] bool hasFailedJobs() const;
    [[nodiscard]] QJsonObject exportState() const;
    void importState(const QJsonObject &obj);

signals:
    void protocolEvent(const core_ir::Event &event);
    void unitStateChanged();
    void unitDataChanged();
    void agentStateRefreshNeeded();

private:
    void onBoundaryCompactionRequested(qint64 threshold);
    void onCompactionFinished(bool success);
    void onCompactionFailed(const QString &reason);
    void startCompactionEngine(qint64 targetTokensOverride = -1);
    /// 修剪超长工具结果；若 threshold>0 且修剪后低于阈值则 continue，返回 true。
    [[nodiscard]] bool pruneToolResultsThenContinue(qint64 threshold);
    void maybeEnqueueSegmentSummary();
    void onSummaryJobFinished(const QString &jobId, bool success, const QString &summaryText,
                              const QList<QString> &spanEntryIds);
    void applyAssembledModelView();
    void syncModelViewPrefixFromStore();
    void emitContextCompactedNotice(core_ir::CompactReason reason);
    void resumeBoundaryAfterSummaryDrain();
    void clearSummaryQueueForBulk();
    void configureAndKickSummaryQueue();
    [[nodiscard]] bool summaryFeaturesEnabled() const;
    [[nodiscard]] QString segmentSummaryCursor() const;
    [[nodiscard]] bool tryContinueWithAssembledView(qint64 threshold, bool logWhenOver);

    QString m_agentId;
    AbstractLoop *m_loop = nullptr;
    SessionRuntime m_runtime;
    ProviderCredential *m_credentialStore = nullptr;
    ProviderFactory m_providerFactory;
    SystemPromptBuilder *m_promptBuilder = nullptr;

    std::unique_ptr<CompactEngine> m_engine;
    std::unique_ptr<SummaryJobQueue> m_queue;
    std::unique_ptr<SummaryStore> m_summaryStore;
    std::unique_ptr<ModelViewStore> m_modelViewStore;
    QString m_lastSummarizedEntryId;
    QString m_lastEnqueuedEntryId;
    bool m_manualCompaction = false;
    bool m_overflowCompaction = false;
    bool m_waitingSummaryAtBoundary = false;
    qint64 m_boundaryThreshold = 0;
};
