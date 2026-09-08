#pragma once

#include "config/ModelTokenDefaults.h"
#include "providers/ProviderTypes/ProviderTypes.h"

#include <QtGlobal>
#include <QString>

class ProviderRunLedger;

/**
 * 压缩策略常量与纯函数：窗口比例、工具结果修剪、检查点框、超窗判定。
 * 不进 SessionRuntime 字段表。
 */
namespace CompactPolicy {

inline constexpr double kThresholdRatio = 0.8;
inline constexpr double kRetainRatio = 0.16;
inline constexpr int kPruneThresholdChars = 8192;
inline constexpr int kPruneHeadChars = 4096;
inline constexpr int kPruneTailChars = 1024;
inline constexpr int kMaxOverflowRetries = 1;

inline constexpr auto kSummaryOpenTag = "<compacted-summary>";
inline constexpr auto kSummaryCloseTag = "</compacted-summary>";
inline constexpr auto kCheckpointPreamble =
    "This is an automatically generated checkpoint condensing an earlier span of the conversation to free up context. Treat the captured context as established background and build on it without restating it. Continue the task directly from the messages that follow, without acknowledging this checkpoint.";
inline constexpr auto kPruneMarker = "\n[...middle pruned...]\n";

[[nodiscard]] inline qint64 pressureThreshold(qint64 window,
                                              const qint64 triggerCap,
                                              const qint64 reserve)
{
    if (window <= 0) {
        window = ModelTokenDefaults::kContextWindow;
    }
    qint64 threshold = window * 4 / 5;
    if (triggerCap > 0) {
        threshold = qMin(threshold, triggerCap);
    }
    if (reserve > 0) {
        threshold = threshold > reserve ? (threshold - reserve) : 0;
    }
    return threshold;
}

[[nodiscard]] inline qint64 retainTokens(qint64 window)
{
    if (window <= 0) {
        window = ModelTokenDefaults::kContextWindow;
    }
    return qMax<qint64>(1, window * 16 / 100);
}

[[nodiscard]] QString pruneToolResultText(const QString &text,
                                          int thresholdChars = kPruneThresholdChars,
                                          int headChars = kPruneHeadChars,
                                          int tailChars = kPruneTailChars);

/// 修剪账本里超长 ToolResult（同时改 UI 投影与线路 output）。返回修剪条数。
int pruneOversizedToolResults(ProviderRunLedger &ledger);

[[nodiscard]] QString frameCheckpoint(const QString &summary);

[[nodiscard]] bool isContextWindowExceeded(const ProviderError &error);

[[nodiscard]] inline bool summaryShrinks(const qint64 sourceTokens, const qint64 summaryTokens)
{
    if (sourceTokens <= 0) {
        return true;
    }
    return summaryTokens > 0 && summaryTokens < sourceTokens;
}

} // namespace CompactPolicy
