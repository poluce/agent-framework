#pragma once

/**
 * @file ProviderAdapterTypes.h
 * @brief Adapter / 传输边界类型（**不是**账本 IR）
 *
 * 账本、UI、Agent 环应只依赖：
 *   #include "providers/ProviderTypes/ProviderTypes.h"
 *
 * Adapter 与 HTTP/SSE 通道额外 include 本头：
 *   #include "providers/ProviderTypes/ProviderAdapterTypes.h"
 *
 * 禁止把 Transport / TurnState 写入 ProviderItem 或会话历史。
 */

#include "ProviderCommon.h"
#include "ProviderItem.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

// ── 传输层 ──

/**
 * @brief Adapter 编好的 HTTP 请求体与头提示
 *
 * 由 HttpSseChannel 等真正发出。**不要**写入 ProviderItem / 账本。
 */
struct ProviderTransportRequest
{
    QByteArray body;                                              ///< 请求体（通常为 JSON）
    QByteArray contentType = QByteArrayLiteral("application/json"); ///< Content-Type
    QByteArray accept = QByteArrayLiteral("application/json");      ///< Accept
    bool expectsEventStream = false;                              ///< 是否期望 SSE 流式响应
};

/// 传输层收到的单帧载荷（SSE data 解析后的 JSON 对象，或非流式完整 body）
struct ProviderTransportPayload
{
    QJsonObject document; ///< 解析后的 JSON 文档
};

// ── 回合瞬态 ──

/**
 * @brief Adapter 解析单次流式响应时的瞬态状态
 *
 * 不进入协议持久化/账本。Error/Cancelled 后可把可恢复片段放 fallbackOutputItems。
 */
struct ProviderTurnState
{
    QString activeRequestId;              ///< 当前请求 id
    bool messageStarted = false;          ///< 是否已发 MessageStarted
    bool reasoningStreamed = false;       ///< 是否已流式输出过 reasoning（完成时勿整段回放）
    bool terminal = false;                ///< 已 Error/Cancelled：禁止再发 Completed/delta
    int activeTextPartIndex = -1;         ///< 当前文本 part 下标
    QHash<QString, int> toolPartIndices;  ///< toolCallId → part 下标
    int nextSyntheticPartIndex = 0;       ///< 合成 part 下标分配器
    QList<ProviderItem> fallbackOutputItems; ///< 无官方 output 时的回退完成态
    QHash<QString, int> fallbackFunctionCallIndices; ///< 回退列表中 FunctionCall 下标
};

Q_DECLARE_METATYPE(ProviderTransportRequest)
Q_DECLARE_METATYPE(ProviderTransportPayload)
Q_DECLARE_METATYPE(ProviderTurnState)
