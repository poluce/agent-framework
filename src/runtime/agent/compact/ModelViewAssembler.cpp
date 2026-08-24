#include "ModelViewAssembler.h"

#include "CompactToolPair.h"
#include "agent/ProviderRunLedger.h"

#include <QSet>

namespace {

bool isSummarizable(const ConversationMessage &entry)
{
    if (!entry.submittedToModel || entry.wasCompacted) {
        return false;
    }
    if (entry.isCompactExempt()) {
        return false;
    }
    switch (entry.kind) {
    case ConversationMessage::Kind::UserText:
    case ConversationMessage::Kind::AssistantText:
    case ConversationMessage::Kind::ToolCall:
    case ConversationMessage::Kind::ToolResult:
    case ConversationMessage::Kind::SkillInvoke:
    case ConversationMessage::Kind::AssistantReasoning:
        return true;
    default:
        return false;
    }
}

qint64 estimateEntryTokens(const ConversationMessage &entry)
{
    qint64 tokens = estimateContextTokensForText(entry.text);
    if (!entry.toolName.isEmpty()) {
        tokens += estimateContextTokensForText(entry.toolName);
    }
    if (!entry.reasoningContent.isEmpty()) {
        tokens += estimateContextTokensForText(entry.reasoningContent);
    }
    return tokens;
}

/// 从尾部回溯，找到「最近 K 个用户轮」起点在 entries 中的下标；K<=0 视为 0
int recentTailStartIndex(const QList<ConversationMessage> &entries, const int recentTurns)
{
    if (recentTurns <= 0 || entries.isEmpty()) {
        return entries.size();
    }
    int userTurns = 0;
    for (int i = entries.size() - 1; i >= 0; --i) {
        if (entries.at(i).kind == ConversationMessage::Kind::UserText) {
            ++userTurns;
            if (userTurns >= recentTurns) {
                return i;
            }
        }
    }
    return 0;
}

template <typename Fn>
void forEachSummarizableSince(const ProviderRunLedger &ledger,
                              const QString &afterEntryId,
                              Fn &&fn)
{
    bool past = afterEntryId.isEmpty();
    for (const ConversationMessage &entry : ledger.entries()) {
        if (!past) {
            if (entry.id == afterEntryId) {
                past = true;
            }
            continue;
        }
        if (isSummarizable(entry)) {
            fn(entry);
        }
    }
}

} // namespace

namespace ModelViewAssembler {

qint64 estimateTokensSince(const ProviderRunLedger &ledger, const QString &afterEntryId)
{
    qint64 total = 0;
    forEachSummarizableSince(ledger, afterEntryId, [&](const ConversationMessage &entry) {
        total += estimateEntryTokens(entry);
    });
    return total;
}

QList<ConversationMessage> collectSummarizableSince(const ProviderRunLedger &ledger,
                                                    const QString &afterEntryId)
{
    QList<ConversationMessage> out;
    forEachSummarizableSince(ledger, afterEntryId, [&](const ConversationMessage &entry) {
        out.append(entry);
    });
    return out;
}

QList<QString> entryIdsOf(const QList<ConversationMessage> &entries)
{
    QList<QString> ids;
    ids.reserve(entries.size());
    for (const ConversationMessage &e : entries) {
        ids.append(e.id);
    }
    return ids;
}

QList<QString> collectRecentUserTexts(const ProviderRunLedger &ledger, const int recentTurns)
{
    QList<QString> out;
    if (recentTurns <= 0) {
        return out;
    }
    const QList<ConversationMessage> &entries = ledger.entries();
    for (int i = entries.size() - 1; i >= 0; --i) {
        const ConversationMessage &entry = entries.at(i);
        if (entry.kind != ConversationMessage::Kind::UserText) {
            continue;
        }
        const QString text = entry.text.trimmed();
        if (text.isEmpty()) {
            continue;
        }
        out.prepend(text);
        if (out.size() >= recentTurns) {
            break;
        }
    }
    return out;
}

ModelViewAssembleResult assemble(const ProviderRunLedger &ledger,
                                 const SummaryStore &store,
                                 const int recentTurns,
                                 const qint64 requestOverheadTokens)
{
    ModelViewAssembleResult result;
    if (store.isEmpty()) {
        result.failReason = QStringLiteral("摘要库为空，无法组装");
        return result;
    }

    const QList<ConversationMessage> &entries = ledger.entries();
    const QList<QString> coveredIds = store.allCoveredEntryIds();
    const QSet<QString> covered(coveredIds.cbegin(), coveredIds.cend());
    const int tailStart = recentTailStartIndex(entries, recentTurns);

    // 近尾保护：tailStart 及之后不 markCompacted
    QSet<QString> protectedIds;
    for (int i = tailStart; i < entries.size(); ++i) {
        protectedIds.insert(entries.at(i).id);
    }

    QSet<QString> selected;
    for (const ConversationMessage &entry : entries) {
        if (!covered.contains(entry.id) || protectedIds.contains(entry.id) || entry.wasCompacted) {
            continue;
        }
        selected.insert(entry.id);
    }
    // Call/Result 同 mark；近尾保护整组保留在回放侧
    CompactToolPair::closeSelection(entries, selected, &protectedIds);

    result.entryIdsToCompact.reserve(selected.size());
    for (const ConversationMessage &entry : entries) {
        if (selected.contains(entry.id)) {
            result.entryIdsToCompact.append(entry.id);
        }
    }

    for (const SummaryRecord &rec : store.records()) {
        if (!rec.text.trimmed().isEmpty()) {
            result.summaryTexts.append(rec.text);
        }
    }

    // 估 token：摘要 + 未 compact 的已提交尾部 + overhead
    // （摘要正文走 totalTokenEstimate；selected 段将被 compact，不计）
    qint64 tokens = qMax<qint64>(0, requestOverheadTokens) + store.totalTokenEstimate();
    for (const ConversationMessage &entry : entries) {
        if (entry.wasCompacted || selected.contains(entry.id) || !entry.submittedToModel) {
            continue;
        }
        tokens += estimateEntryTokens(entry);
    }
    result.estimatedTokens = tokens;
    result.ok = !result.summaryTexts.isEmpty();
    if (!result.ok) {
        result.failReason = QStringLiteral("摘要正文为空");
    }
    return result;
}

} // namespace ModelViewAssembler
