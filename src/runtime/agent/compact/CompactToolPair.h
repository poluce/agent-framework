#pragma once

#include "types/ConversationMessage.h"

#include <QList>
#include <QSet>
#include <QString>

/**
 * 压缩选型的工具对原子性：同一 toolUseId 的 ToolCall 与 ToolResult 必须同 mark / 同保留。
 * 半对 mark 会在 buildRequest 后留下孤儿 role=tool → DeepSeek 等 Chat Completions 400。
 */
namespace CompactToolPair {

/// Call / Result 配对键：优先 toolUseId，否则 ToolCall 用 toolCall.id / entry.id
[[nodiscard]] QString pairKey(const ConversationMessage &entry);

/**
 * 闭合 selected：对每个 pairKey 分组
 * - 组内任一条在 protectedIds 中 → 整组移出 selected（近尾保护优先）
 * - 否则组内任一条已选 → 整组并入 selected
 * 仅处理 Kind::ToolCall / Kind::ToolResult；其它 kind 不动。
 */
void closeSelection(const QList<ConversationMessage> &entries,
                    QSet<QString> &selectedIds,
                    const QSet<QString> *protectedIds = nullptr);

/**
 * 近尾按 retainTokens 原样保留，更旧的已提交/未 compact/非 exempt 前缀可压。
 * retainTokens==0：只留最末一条候选。切口再 closeSelection（近尾工具对整组保护）。
 */
[[nodiscard]] QList<QString> selectPrefixToCompact(const QList<ConversationMessage> &entries,
                                                   qint64 retainTokens);

} // namespace CompactToolPair
