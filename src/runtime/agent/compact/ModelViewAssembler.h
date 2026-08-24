#pragma once

#include "SummaryStore.h"
#include "agent/ProviderRunLedger.h"

#include <QList>
#include <QString>

struct ModelViewAssembleResult {
    bool ok = false;
    /// 应 markCompacted 的 entry id（被摘要覆盖、且不在近尾 K 轮保护内）
    QList<QString> entryIdsToCompact;
    /// 按序注入模型的摘要正文（每条对应 SummaryRecord）
    QList<QString> summaryTexts;
    /// 组装后估 token（摘要 + 未覆盖尾部）
    qint64 estimatedTokens = 0;
    QString failReason;
};

/**
 * 组装「摘要链 + 近 K 轮完整结构」模型短上下文。
 * 不写账本；调用方负责 apply。
 */
namespace ModelViewAssembler {

[[nodiscard]] ModelViewAssembleResult assemble(const ProviderRunLedger &ledger,
                                               const SummaryStore &store,
                                               int recentTurns,
                                               qint64 requestOverheadTokens = 0);

/// 估算 ledger 中自 afterEntryId 之后（不含）可摘要内容的 token；after 空=从起点
[[nodiscard]] qint64 estimateTokensSince(const ProviderRunLedger &ledger,
                                         const QString &afterEntryId);

/// 收集自 afterEntryId 之后可摘要条目（提交过、未 compact、非 exempt）
[[nodiscard]] QList<ConversationMessage> collectSummarizableSince(const ProviderRunLedger &ledger,
                                                                  const QString &afterEntryId);

[[nodiscard]] QList<QString> entryIdsOf(const QList<ConversationMessage> &entries);

/// 从账本尾部取最近 K 条用户原文（按时间正序；含已 compact，便于醒目回灌）
[[nodiscard]] QList<QString> collectRecentUserTexts(const ProviderRunLedger &ledger,
                                                   int recentTurns);

} // namespace ModelViewAssembler
