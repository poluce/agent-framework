#include "ProviderRetryPolicy.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QString>

namespace {

bool isTransientHttpStatus(const int httpStatus)
{
    switch (httpStatus) {
    case ProviderRetry::kStatusRequestTimeout:
    case ProviderRetry::kStatusConflict:
    case ProviderRetry::kStatusTooManyRequests:
    case ProviderRetry::kStatusOverloaded:
        return true;
    default:
        return httpStatus >= ProviderRetry::kStatusServerErrorMin;
    }
}

} // namespace

namespace ProviderRetry {

Classification classifyHttpStatus(const int httpStatus,
                                  const QByteArray &retryAfterHeader,
                                  const int maxRetryAfterMs)
{
    Classification result;

    if (httpStatus <= 0) {
        result.retryable = true;
        return result;
    }

    switch (httpStatus) {
    case kStatusBadRequest:
    case kStatusUnauthorized:
    case kStatusForbidden:
    case kStatusNotFound:
    case kStatusUnprocessable:
        return result;
    default:
        break;
    }

    result.retryable = isTransientHttpStatus(httpStatus);
    if (result.retryable) {
        result.retryAfterMs = parseRetryAfter(retryAfterHeader, maxRetryAfterMs);
    }
    return result;
}

Classification classifyApiErrorObject(const QJsonObject &errorObject)
{
    Classification result;
    if (errorObject.isEmpty()) {
        return result;
    }

    const QString type = errorObject.value(QStringLiteral("type")).toString().trimmed().toLower();
    const QString status = errorObject.value(QStringLiteral("status")).toString().trimmed().toUpper();
    const QString message = errorObject.value(QStringLiteral("message")).toString().trimmed().toLower();
    const QJsonValue codeValue = errorObject.value(QStringLiteral("code"));
    QString code;
    int numericCode = 0;
    if (codeValue.isString()) {
        code = codeValue.toString().trimmed().toLower();
        bool ok = false;
        const int parsed = code.toInt(&ok);
        if (ok) {
            numericCode = parsed;
        }
    } else if (codeValue.isDouble()) {
        numericCode = codeValue.toInt();
    }

    const QString errorBlob = type + QLatin1Char(' ') + code + QLatin1Char(' ') + message;
    if (errorBlob.contains(QStringLiteral("context_length"))
        || errorBlob.contains(QStringLiteral("context length"))
        || errorBlob.contains(QStringLiteral("context_window"))
        || errorBlob.contains(QStringLiteral("context window"))
        || errorBlob.contains(QStringLiteral("maximum context"))
        || errorBlob.contains(QStringLiteral("max context"))
        || errorBlob.contains(QStringLiteral("prompt is too long"))
        || errorBlob.contains(QStringLiteral("prompt too long"))
        || errorBlob.contains(QStringLiteral("too many tokens"))
        || errorBlob.contains(QStringLiteral("token limit"))) {
        result.isContextWindowExceeded = true;
        result.retryable = false;
        return result;
    }

    const auto looksTransient = [](const QString &token) {
        return token.contains(QStringLiteral("overloaded"))
            || token.contains(QStringLiteral("rate_limit"))
            || token.contains(QStringLiteral("server_error"))
            || token == QStringLiteral("unavailable")
            || token == QStringLiteral("internal")
            || token == QStringLiteral("timeout")
            || token == QStringLiteral("aborted");
    };

    if (looksTransient(type) || looksTransient(code)
        || status == QStringLiteral("RESOURCE_EXHAUSTED")
        || status == QStringLiteral("UNAVAILABLE")
        || status == QStringLiteral("DEADLINE_EXCEEDED")
        || status == QStringLiteral("ABORTED")
        || status == QStringLiteral("INTERNAL")) {
        result.retryable = true;
    }

    if (numericCode > 0) {
        const Classification http = classifyHttpStatus(numericCode, {}, kMaxRetryAfterMs);
        if (http.retryable) {
            result.retryable = true;
        } else if (numericCode == kStatusBadRequest
                   || numericCode == kStatusUnauthorized
                   || numericCode == kStatusForbidden
                   || numericCode == kStatusNotFound
                   || numericCode == kStatusUnprocessable) {
            result.retryable = false;
        }
    }

    return result;
}

Classification classifyApiErrorValue(const QJsonValue &errorValue)
{
    if (errorValue.isObject()) {
        return classifyApiErrorObject(errorValue.toObject());
    }
    if (!errorValue.isString()) {
        return {};
    }

    const QString text = errorValue.toString().trimmed().toLower();
    Classification result;
    if (text.contains(QStringLiteral("context_length"))
        || text.contains(QStringLiteral("context length"))
        || text.contains(QStringLiteral("context_window"))
        || text.contains(QStringLiteral("context window"))
        || text.contains(QStringLiteral("maximum context"))
        || text.contains(QStringLiteral("max context"))
        || text.contains(QStringLiteral("prompt is too long"))
        || text.contains(QStringLiteral("prompt too long"))
        || text.contains(QStringLiteral("too many tokens"))
        || text.contains(QStringLiteral("token limit"))) {
        result.isContextWindowExceeded = true;
        result.retryable = false;
        return result;
    }
    result.retryable = text.contains(QStringLiteral("overloaded"))
        || text.contains(QStringLiteral("rate limit"))
        || text.contains(QStringLiteral("too many requests"))
        || text.contains(QStringLiteral("service unavailable"))
        || text.contains(QStringLiteral("temporarily unavailable"))
        || text.contains(QStringLiteral("server error"));
    return result;
}

int parseRetryAfter(const QByteArray &header, const int maxMs)
{
    const QByteArray trimmed = header.trimmed();
    if (trimmed.isEmpty()) {
        return -1;
    }

    // retry-after-ms（OpenAI 非标准，毫秒）
    const QByteArray lower = trimmed.toLower();
    if (lower.endsWith("ms")) {
        bool ok = false;
        const double value = trimmed.left(trimmed.size() - 2).trimmed().toDouble(&ok);
        if (!ok || value < 0) {
            return -1;
        }
        const int ms = static_cast<int>(value);
        return ms <= maxMs ? ms : -1;
    }

    // 纯秒数
    bool ok = false;
    const double seconds = trimmed.toDouble(&ok);
    if (ok && seconds >= 0) {
        const int ms = static_cast<int>(seconds * 1000.0);
        return ms <= maxMs ? ms : -1;
    }

    // HTTP-date（RFC 7231）
    const QDateTime date = QDateTime::fromString(QString::fromLatin1(trimmed), Qt::RFC2822Date);
    if (date.isValid()) {
        const qint64 remainingMs = date.toMSecsSinceEpoch() - QDateTime::currentMSecsSinceEpoch();
        if (remainingMs >= 0 && remainingMs <= maxMs) {
            return static_cast<int>(remainingMs);
        }
    }

    return -1;
}

int backoffDelayMs(const int attempt)
{
    const int base = qMin(kBackoffBaseMs * (1 << qMin(attempt, 20)), kBackoffMaxMs);
    // jitter 0.75–1.0，防多 Agent 同步退避
    const double factor = 0.75 + 0.25 * QRandomGenerator::global()->generateDouble();
    return static_cast<int>(base * factor);
}

} // namespace ProviderRetry
