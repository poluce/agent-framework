#pragma once

/**
 * @file ProviderRetryPolicy.h
 * @brief Provider 层瞬时错误分类 / 退避纯函数（无 Network 依赖）
 *
 * 重试下沉到 Provider：HTTP 状态、Retry-After、厂商 error type 在传输/解析层产生，
 * 只有 Provider 能统一「请求 + 错误」上下文（主链路 / CompactEngine / AutoRename 共享）。
 *
 * 语义：首字节前可重试；流中断 = 最终失败；指数退避 + jitter；Retry-After 优先且封顶。
 */

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>

namespace ProviderRetry {

// ── HTTP 状态码（单一来源，避免裸数字漂移）──
inline constexpr int kStatusRequestTimeout  = 408;
inline constexpr int kStatusConflict        = 409;
inline constexpr int kStatusTooManyRequests = 429;
inline constexpr int kStatusBadRequest      = 400;
inline constexpr int kStatusUnauthorized    = 401;
inline constexpr int kStatusForbidden       = 403;
inline constexpr int kStatusNotFound        = 404;
inline constexpr int kStatusUnprocessable   = 422;
inline constexpr int kStatusOverloaded      = 529;  // Anthropic overloaded_error
inline constexpr int kStatusServerErrorMin  = 500;

// ── 退避 / Retry-After ──
inline constexpr int kBackoffBaseMs   = 500;     // 对齐 OpenAI 0.5s
inline constexpr int kBackoffMaxMs    = 8000;    // 对齐 OpenAI 8s
inline constexpr int kMaxRetryAfterMs = 60'000;  // 超过则忽略，防死等

} // namespace ProviderRetry

/// 本 turn 重试策略（sendRequest 前由调用方下发；默认关闭）
struct ProviderRetryPolicy
{
    int maxRetries = 0;             ///< 不含首次；0 = 关闭
    bool respectRetryAfter = true;
    int maxRetryAfterMs = ProviderRetry::kMaxRetryAfterMs;
};

namespace ProviderRetry {

struct Classification
{
    bool retryable = false;
    int retryAfterMs = -1;  ///< -1 = 未提供 / 不合理
};

/// 连接级(0)/408/409/429/529/≥500 → retryable；401/400/403/404/422 → 否。
/// TimeoutError 由调用方挡在门外（idle timeout 归看门狗，不空转重试）。
[[nodiscard]] Classification classifyHttpStatus(int httpStatus,
                                                const QByteArray &retryAfterHeader,
                                                int maxRetryAfterMs);

/// 厂商 error 对象（OpenAI `error.type/code`、Gemini `error.status`、
/// Anthropic `error.type`）。瞬时过载可重试；鉴权/参数/额度不足不可重试。
[[nodiscard]] Classification classifyApiErrorObject(const QJsonObject &errorObject);

/// `error` 字段可能是对象或字符串；字符串仅在明确过载措辞时标 retryable。
[[nodiscard]] Classification classifyApiErrorValue(const QJsonValue &errorValue);

/// 秒数字 / retry-after-ms / HTTP-date；失败或超 maxMs → -1
[[nodiscard]] int parseRetryAfter(const QByteArray &header, int maxMs);

/// min(500 * 2^attempt, 8000) * (0.75 ~ 1.0)；attempt 从 0 起
[[nodiscard]] int backoffDelayMs(int attempt);

} // namespace ProviderRetry
