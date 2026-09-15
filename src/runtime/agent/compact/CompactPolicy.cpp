#include "CompactPolicy.h"

#include "agent/ProviderRunLedger.h"
#include "types/ConversationMessage.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStringList>

namespace CompactPolicy {

QString pruneToolResultText(const QString &text,
                            const int thresholdChars,
                            const int headChars,
                            const int tailChars)
{
    if (thresholdChars <= 0 || text.size() <= thresholdChars) {
        return text;
    }
    const QString marker = QString::fromUtf8(kPruneMarker);
    if (headChars < 0 || tailChars < 0) {
        return text;
    }
    const int kept = headChars + marker.size() + tailChars;
    if (kept >= text.size()) {
        return text;
    }
    return text.left(headChars) + marker + text.right(tailChars);
}

int pruneOversizedToolResults(ProviderRunLedger &ledger)
{
    struct Job {
        QString id;
        QString pruned;
    };
    QList<Job> jobs;
    for (const ConversationMessage &entry : ledger.entries()) {
        if (entry.kind != ConversationMessage::Kind::ToolResult) {
            continue;
        }
        const QString pruned = pruneToolResultText(entry.text);
        if (pruned.size() < entry.text.size()) {
            jobs.append({entry.id, pruned});
        }
    }
    int n = 0;
    for (const Job &job : jobs) {
        if (ledger.updateToolResultOutput(job.id, job.pruned)) {
            ++n;
        }
    }
    return n;
}

QString frameCheckpoint(const QString &summary)
{
    const QString trimmed = summary.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    if (trimmed.contains(QLatin1String(kSummaryOpenTag))
        || trimmed.startsWith(QLatin1Char('['))) {
        return summary;
    }
    return QString::fromUtf8(kCheckpointPreamble)
        + QStringLiteral("\n\n")
        + QString::fromUtf8(kSummaryOpenTag) + QLatin1Char('\n')
        + trimmed + QLatin1Char('\n')
        + QString::fromUtf8(kSummaryCloseTag);
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
    if (!entry.toolCall.rawInputJson.isEmpty()) {
        tokens += estimateContextTokensForText(entry.toolCall.rawInputJson);
    } else if (!entry.toolInput.isEmpty()) {
        tokens += estimateContextTokensForText(
            QString::fromUtf8(QJsonDocument(entry.toolInput).toJson(QJsonDocument::Compact)));
    } else if (!entry.toolCall.input.isEmpty()) {
        tokens += estimateContextTokensForText(
            QString::fromUtf8(QJsonDocument(entry.toolCall.input).toJson(QJsonDocument::Compact)));
    }
    return tokens;
}

bool isContextWindowExceeded(const ProviderError &error)
{
    if (error.code == QLatin1String(kErrorContextWindowExceeded)
        || error.code == QLatin1String("context_window_exceeded")
        || error.code == QLatin1String("context_length_exceeded")) {
        return true;
    }
    const QString blob = (error.code + QLatin1Char(' ') + error.message).toLower();
    if (blob.isEmpty()) {
        return false;
    }
    static const QStringList needles = {
        QStringLiteral("context_length"),
        QStringLiteral("context length"),
        QStringLiteral("context_window"),
        QStringLiteral("context window"),
        QStringLiteral("maximum context"),
        QStringLiteral("max context"),
        QStringLiteral("prompt is too long"),
        QStringLiteral("prompt too long"),
        QStringLiteral("too many tokens"),
        QStringLiteral("token limit"),
        QStringLiteral("context limit"),
        QStringLiteral("range of input length"),
        QStringLiteral("model's maximum context"),
        QStringLiteral("exceeds the context"),
        QStringLiteral("上下文"),
    };
    for (const QString &needle : needles) {
        if (blob.contains(needle)) {
            return true;
        }
    }
    return false;
}

} // namespace CompactPolicy
