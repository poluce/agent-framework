#pragma once

#include "CompactConfig.h"
#include "agent/ProviderRunLedger.h"
#include "ir/CoreEvent.h"
#include "ir/CoreEventChannel.h"

#include <QObject>
#include <QList>
#include <QString>
#include <QTimer>

#include <memory>
#include <functional>
#include <vector>

class AbstractProvider;
class ProviderCredential;

/// 摘要正文校验结果（空文本 / DSML / 过短等）
struct SummaryValidation
{
    bool ok = false;
    QString reason;
};

/// 压缩引擎：选条 → 组请求材料 → 调 LLM → 校验摘要 → 写回账本
/// 对标 codex codex-rs/core/src/compact.rs
class CompactEngine : public QObject
{
    Q_OBJECT

public:
    explicit CompactEngine(QObject *parent = nullptr);
    ~CompactEngine() override;

    CompactConfig config;

    /// 组合根注入的提示词拼装器；空则只用内置 compact.md / summary.md。
    void setPromptBuilder(class SystemPromptBuilder *builder);

    /// 规则裁剪后的架构化文档材料（纯文本；不含任务外壳）
    [[nodiscard]] static QString buildDocumentMaterial(
        const QList<ConversationMessage> &entries,
        qint64 tokenBudget);
    /// 单条 UserText：任务句 + 文档材料（段摘要与大压共用）
    [[nodiscard]] static QList<ProviderItem> buildDocumentCompactInput(
        const QList<ConversationMessage> &entries,
        qint64 tokenBudget);
    /// 摘要正文硬校验：拒空/过短/DSML/tool 壳/伪续聊
    [[nodiscard]] static SummaryValidation validateSummaryText(const QString &text);

    /// 异步执行压缩（使用给定的临时 Provider，无信号冲突）
    void start(
        ProviderRunLedger *ledger,
        const QString &credentialInstanceId,
        ProviderCredential *credentialStore,
        const std::function<std::unique_ptr<AbstractProvider>(const QString &)> &providerFactory,
        const QString &modelName,
        AbstractProvider *activeProvider = nullptr
    );
    /**
     * 旁路段摘要：直接用入队快照调 LLM，**不**改账本。
     * 成功/失败只发 summaryOnlyFinished；不发 compactionFinished。
     */
    void startSummaryOnly(
        const QList<ConversationMessage> &snapshot,
        const QString &credentialInstanceId,
        ProviderCredential *credentialStore,
        const std::function<std::unique_ptr<AbstractProvider>(const QString &)> &providerFactory,
        const QString &modelName,
        AbstractProvider *activeProvider = nullptr
    );
    void cancel();
    [[nodiscard]] bool isRunning() const;

    /// 最近一次 bulk 成功摘要正文 / 覆盖 entry id（供 §5.3 写库）
    [[nodiscard]] QString lastBulkSummaryText() const { return m_lastBulkSummaryText; }
    [[nodiscard]] QList<QString> lastBulkCompactedIds() const { return m_lastBulkCompactedIds; }

    // ── 内环 Event fan-out ──
    using EventHandler = core_ir::EventHandler;
    void addProtocolHandler(EventHandler h) { m_protocolHandlers.push_back(std::move(h)); }
    void pushEvent(const core_ir::Event &event) {
        for (auto &h : m_protocolHandlers)
            h(event, {}, {});
    }

signals:
    void compactionFinished(bool success);
    void compactionFailed(const QString &reason);
    /// 段摘要专用完成（success + 正文；失败时 text 为原因）
    void summaryOnlyFinished(bool success, const QString &text);

private:
    /// 从账本中选出要压缩的条目 ID（已提交 + 未压缩，从最旧开始）
    [[nodiscard]] QList<QString> selectEntriesToCompact(const ProviderRunLedger &ledger) const;

    void startRequest();
    void scheduleRetry(const QString &reason);
    void finishWithSummary();
    void finishWithFailure(const QString &reason);
    /// start() 入口同步跳过：用户可见 Warning + finished(false)，不置 m_running
    void finishSkipped(const QString &userMessage);
    void resetState();
    [[nodiscard]] bool prepareTempProvider(
        const QString &credentialInstanceId,
        ProviderCredential *credentialStore,
        const std::function<std::unique_ptr<AbstractProvider>(const QString &)> &providerFactory,
        const QString &modelName,
        AbstractProvider *activeProvider,
        QString *skipReason
    );

    /// 在账本中标记旧条目为已压缩，并插入摘要条目
    void applyCompaction(
        ProviderRunLedger &ledger,
        const QList<QString> &compactedIds,
        const QString &summaryText
    );

    /// 降级：直接截断最旧条目
    bool truncateOldestRoundTrip(ProviderRunLedger &ledger);

    class SystemPromptBuilder *m_promptBuilder = nullptr;
    ProviderRunLedger *m_ledger = nullptr;
    std::unique_ptr<AbstractProvider> m_tempProvider;
    QList<QString> m_compactedIds;
    QList<ConversationMessage> m_selectedEntries;
    QString m_summaryText;
    QString m_lastBulkSummaryText;
    QList<QString> m_lastBulkCompactedIds;
    int m_retryCount = 0;
    bool m_running = false;
    /// true：段摘要模式（不写账本）
    bool m_summaryOnly = false;
    QMetaObject::Connection m_providerConnection;
    QTimer m_retryTimer;
    std::vector<EventHandler> m_protocolHandlers;
};
