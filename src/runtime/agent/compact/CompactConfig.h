#pragma once

#include <QtCore/qtypes.h>

/// 压缩执行参数（给 CompactEngine）。
/// 触发门控在 AbstractLoop：min(contextWindow, compactTriggerTokens?) − reserve；
/// 不在此结构重复 trigger / enabled。
struct CompactConfig
{
    /// 压缩后上下文的目标 token 数
    qint64 targetTokenCount = 40000;

    /// 压缩时保留用户消息的 token 预算
    qint64 userMessageTokenBudget = 20000;

    /// 压缩 LLM 调用的最大输出 token 数
    int maxOutputTokens = 80000;

    /// 压缩 LLM 调用失败时的最大重试次数
    int maxRetries = 2;
};
