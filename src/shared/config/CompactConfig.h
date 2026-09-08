#pragma once

#include <QtCore/qtypes.h>

/// 压缩执行参数（给 CompactEngine）。
/// 触发门控在 AbstractLoop：window×0.8，再与 compactTriggerTokens / reserve 取限。
/// 不在此结构重复 trigger / enabled。
struct CompactConfig
{
    /// 近尾原样保留的 token 预算（选型从尾向前累加；0 = 只留最末一条候选）
    qint64 retainTokenCount = 40000;

    /// 压缩后上下文的目标 token 数（兼容旧字段；选型走 retainTokenCount）
    qint64 targetTokenCount = 40000;

    /// 压缩时保留用户消息的 token 预算
    qint64 userMessageTokenBudget = 20000;

    /// 压缩 LLM 调用的最大输出 token 数
    int maxOutputTokens = 80000;

    /// 压缩 LLM 调用失败时的最大重试次数
    int maxRetries = 2;
};
