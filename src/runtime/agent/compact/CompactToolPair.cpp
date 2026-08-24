#include "CompactToolPair.h"

#include "agent/ProviderRunLedger.h"

#include <QHash>
#include <QSet>

namespace CompactToolPair {
namespace {

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

bool isToolPairKind(const ConversationMessage::Kind kind)
{
    return kind == ConversationMessage::Kind::ToolCall
        || kind == ConversationMessage::Kind::ToolResult;
}

bool isCompactCandidate(const ConversationMessage &entry)
{
    if (!entry.submittedToModel || entry.wasCompacted) {
        return false;
    }
    if (entry.isCompactExempt()) {
        return false;
    }
    return true;
}

} // namespace

QString pairKey(const ConversationMessage &entry)
{
    const QString useId = entry.toolUseId.trimmed();
    if (!useId.isEmpty()) {
        return useId;
    }
    if (entry.kind != ConversationMessage::Kind::ToolCall) {
        return {};
    }
    const QString callId = entry.toolCall.id.trimmed();
    if (!callId.isEmpty()) {
        return callId;
    }
    return entry.id.trimmed();
}

void closeSelection(const QList<ConversationMessage> &entries,
                    QSet<QString> &selectedIds,
                    const QSet<QString> *protectedIds)
{
    // pairKey → 该组全部 entry id
    QHash<QString, QList<QString>> groupIds;
    QHash<QString, bool> groupProtected;
    QHash<QString, bool> groupSelected;

    for (const ConversationMessage &entry : entries) {
        if (!isToolPairKind(entry.kind)) {
            continue;
        }
        const QString key = pairKey(entry);
        if (key.isEmpty() || entry.id.isEmpty()) {
            continue;
        }
        groupIds[key].append(entry.id);
        if (protectedIds && protectedIds->contains(entry.id)) {
            groupProtected[key] = true;
        }
        if (selectedIds.contains(entry.id)) {
            groupSelected[key] = true;
        }
    }

    for (auto it = groupIds.constBegin(); it != groupIds.constEnd(); ++it) {
        const QString &key = it.key();
        const QList<QString> &ids = it.value();
        if (groupProtected.value(key, false)) {
            // 近尾保护：整组不得 mark
            for (const QString &id : ids) {
                selectedIds.remove(id);
            }
            continue;
        }
        if (!groupSelected.value(key, false)) {
            continue;
        }
        // 半选 → 整组纳入
        for (const QString &id : ids) {
            selectedIds.insert(id);
        }
    }
}

QList<QString> selectPrefixToCompact(const QList<ConversationMessage> &entries,
                                     const qint64 targetTokenCount)
{
    QList<QString> ids;
    qint64 accumulated = 0;
    const qint64 budget = qMax<qint64>(0, targetTokenCount);

    for (const ConversationMessage &entry : entries) {
        if (!isCompactCandidate(entry)) {
            continue;
        }

        const qint64 tokens = estimateEntryTokens(entry);
        if (!ids.isEmpty() && accumulated + tokens > budget) {
            break;
        }

        accumulated += tokens;
        ids.append(entry.id);
    }

    QSet<QString> selected(ids.cbegin(), ids.cend());
    closeSelection(entries, selected, nullptr);

    // 保持账本顺序，便于摘要输入与日志稳定
    QList<QString> ordered;
    ordered.reserve(selected.size());
    for (const ConversationMessage &entry : entries) {
        if (selected.contains(entry.id)) {
            ordered.append(entry.id);
        }
    }
    return ordered;
}

} // namespace CompactToolPair
