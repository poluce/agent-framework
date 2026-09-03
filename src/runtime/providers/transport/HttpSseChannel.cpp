#include "HttpSseChannel.h"

#include "providers/core/ProviderRetryPolicy.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {
// 传输层空闲超时：连续无字节超过此时长则失败，避免半开连接干等 Loop 300s 看门狗。
// 与 modelResponseTimeout（整轮）不同，这是「无数据进展」时钟。
constexpr int kSseTransferIdleTimeoutMs = 90 * 1000;
} // namespace

HttpSseChannel::HttpSseChannel(const QString &providerType, QObject *parent)
    : QObject(parent)
    , m_providerType(providerType)
{
}

HttpSseChannel::~HttpSseChannel()
{
    cleanupReply();
}

void HttpSseChannel::setLogContext(const AgentLogContext &logContext)
{
    m_logContext = logContext;
}

void HttpSseChannel::setAuthMode(AuthMode mode)
{
    m_authMode = mode;
}

void HttpSseChannel::setVersionHeader(const QByteArray &name, const QByteArray &value)
{
    m_versionHeaderName = name;
    m_versionHeaderValue = value;
}

void HttpSseChannel::applyAuthHeaders(QNetworkRequest &request, const QString &apiKey) const
{
    switch (m_authMode) {
    case AuthMode::Bearer:
        request.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
        break;
    case AuthMode::ApiKey:
        request.setRawHeader("x-api-key", apiKey.toUtf8());
        break;
    case AuthMode::GoogApiKey:
        request.setRawHeader("x-goog-api-key", apiKey.toUtf8());
        break;
    }
    if (!m_versionHeaderName.isEmpty()) {
        request.setRawHeader(m_versionHeaderName, m_versionHeaderValue);
    }
}

void HttpSseChannel::disposeActiveReply(bool abort)
{
    if (!m_activeReply) {
        return;
    }
    QNetworkReply *reply = m_activeReply;
    m_activeReply = nullptr;
    // 断开本对象槽，避免 abort/finished 落到已替换的 active 指针上
    QObject::disconnect(reply, nullptr, this, nullptr);
    if (abort) {
        reply->abort();
    }
    reply->deleteLater();
}

void HttpSseChannel::abandonActiveReply()
{
    // 替换旧连接：abort 丢弃，不向业务层发 failed
    disposeActiveReply(true);
}

HttpSseChannel::StartResult HttpSseChannel::start(const QUrl &url,
                                                  const QString &apiKey,
                                                  const ProviderTransportRequest &request)
{
    StartResult result;
    if (url.isEmpty() || apiKey.trimmed().isEmpty()) {
        LOGE(LogCat::Network, m_logContext) << "通信启动失败：url 或 apiKey 未设置"
            << logf("provider", m_providerType);
        ProviderError error;
        error.code = QStringLiteral("transport_config_incomplete");
        error.message = QStringLiteral("%1 url 或 apiKey 未设置。").arg(m_providerType);
        result.error = error;
        return result;
    }

    // 新请求前安全丢弃旧 reply（重试/连发不得留下孤儿连接）
    abandonActiveReply();

    m_rawBuffer.clear();
    m_sseMode = false;
    m_parser = std::make_unique<ProviderSseParser>();

    LogManager::instance().saveRequestBody(m_providerType, request.body, m_logContext);

    QNetworkRequest networkRequest(url);
    networkRequest.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromUtf8(request.contentType));
    applyAuthHeaders(networkRequest, apiKey);
    networkRequest.setRawHeader("Accept", request.accept);
    // 无字节进展超时（半开 SSE / 代理挂死）
    networkRequest.setTransferTimeout(kSseTransferIdleTimeoutMs);

    QNetworkReply *reply = m_networkAccessManager.post(networkRequest, request.body);
    m_activeReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, &HttpSseChannel::onReadyRead);
    connect(reply, &QNetworkReply::finished, this, &HttpSseChannel::onReplyFinished);
    connect(reply, &QNetworkReply::errorOccurred, this, &HttpSseChannel::onErrorOccurred);

    LOGD(LogCat::Network, m_logContext) << "SSE 请求已发出"
        << logf("provider", m_providerType)
        << logf("transferIdleTimeoutMs", kSseTransferIdleTimeoutMs);

    result.accepted = true;
    return result;
}

void HttpSseChannel::cancel()
{
    if (m_activeReply) {
        QNetworkReply *reply = m_activeReply;
        // abort 异步；errorOccurred 里用 sender() 对齐，避免误清后续新 reply
        reply->abort();
        return;
    }

    // 无活跃 reply 时 abort 不会触发 errorOccurred，必须同步上报取消，
    // 否则上层若只依赖异步 Cancelled 事件会永久卡住。
    ProviderError error;
    error.code = QStringLiteral("transport_canceled");
    error.message = QStringLiteral("Transport 请求已取消。");
    emit failed(error);
}

void HttpSseChannel::onReadyRead()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_activeReply) {
        return;
    }

    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty()) {
        return;
    }

    if (!m_sseMode) {
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (contentType.contains(QStringLiteral("text/event-stream"), Qt::CaseInsensitive)) {
            m_sseMode = true;
            m_rawBuffer.clear();
        }
    }

    if (m_sseMode) {
        m_parser->feed(chunk);
        processSseEvents();
        return;
    }

    m_rawBuffer.append(chunk);
    trySwitchToSseMode();
}

void HttpSseChannel::onReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        return;
    }
    // 过期 reply：只回收，不动当前 active
    if (reply != m_activeReply) {
        reply->deleteLater();
        return;
    }

    const QByteArray tail = reply->readAll();
    if (!tail.isEmpty()) {
        if (m_sseMode) {
            m_parser->feed(tail);
        } else {
            m_rawBuffer.append(tail);
            trySwitchToSseMode();
        }
    }

    if (reply->error() != QNetworkReply::NoError) {
        // 错误路径由 onErrorOccurred 负责 failed + cleanup
        return;
    }

    if (m_sseMode) {
        m_parser->finish();
        processSseEvents();
    } else {
        finalizeAsJson(m_rawBuffer);
    }

    if (m_activeReply != reply) {
        return;
    }

    cleanupReply();
    emit finished();
}

void HttpSseChannel::onErrorOccurred()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        return;
    }
    if (reply != m_activeReply) {
        // 已被 start() abandon 或已被替换的旧连接
        reply->deleteLater();
        return;
    }

    const QNetworkReply::NetworkError networkError = reply->error();
    const QByteArray errorBody = reply->readAll();
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    const QString urlStr = reply->request().url().toString();
    LOGE(LogCat::Network, m_logContext) << "网络错误"
        << logf("provider", m_providerType)
        << logf("url", urlStr)
        << logf("httpStatus", httpStatus)
        << logf("networkError", static_cast<int>(networkError))
        << logf("error", reply->errorString());

    ProviderError error;
    if (networkError == QNetworkReply::OperationCanceledError) {
        error.code = QStringLiteral("transport_canceled");
        error.message = QStringLiteral("Transport 请求已取消。");
    } else if (networkError == QNetworkReply::TimeoutError) {
        error.code = QStringLiteral("transport_idle_timeout");
        error.message = QStringLiteral("%1 传输空闲超时（长时间无数据）。").arg(m_providerType);
    } else {
        // 厂商正文优先；否则回退 Qt errorString，并尽量附 HTTP 状态
        const QString detail =
            extractApiErrorMessage(errorBody.isEmpty() ? m_rawBuffer : errorBody);
        error.code = QStringLiteral("%1_network_error").arg(m_providerType);
        if (!detail.isEmpty()) {
            error.message = httpStatus > 0
                ? QStringLiteral("%1（HTTP %2）").arg(detail).arg(httpStatus)
                : detail;
        } else if (httpStatus > 0) {
            error.message = QStringLiteral("%1 请求失败（HTTP %2）：%3")
                                .arg(m_providerType)
                                .arg(httpStatus)
                                .arg(reply->errorString());
        } else {
            error.message = QStringLiteral("%1 请求失败：%2")
                                .arg(m_providerType, reply->errorString());
        }
    }

    // 瞬时过载分类（连接级 httpStatus==0 也进；取消/超时保持不可重试）
    error.retryable = false;
    error.retryAfterMs = -1;
    if (networkError != QNetworkReply::OperationCanceledError
        && networkError != QNetworkReply::TimeoutError) {
        const ProviderRetry::Classification cls = ProviderRetry::classifyHttpStatus(
            httpStatus, reply->rawHeader("Retry-After"), ProviderRetry::kMaxRetryAfterMs);
        error.retryable = cls.retryable;
        error.retryAfterMs = cls.retryAfterMs;
    }

    cleanupReply();
    emit failed(error);
}

void HttpSseChannel::handleSseMessage(const QString &eventName, const QByteArray &data)
{
    Q_UNUSED(eventName);

    const QByteArray trimmed = data.trimmed();
    if (trimmed.isEmpty() || trimmed == QByteArrayLiteral("[DONE]")) {
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        LOGE(LogCat::Network, m_logContext) << "SSE 解析失败"
            << logf("provider", m_providerType)
            << logf("error", parseError.errorString());
        ProviderError error;
        error.code = QStringLiteral("%1_invalid_json").arg(m_providerType);
        error.message = QStringLiteral("无法解析 %1 SSE 事件。").arg(m_providerType);
        cleanupReply();
        emit failed(error);
        return;
    }

    emit payloadReceived(ProviderTransportPayload{document.object()});
}

void HttpSseChannel::finalizeAsJson(const QByteArray &payload)
{
    if (payload.isEmpty()) {
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        LOGE(LogCat::Network, m_logContext) << "响应 JSON 解析失败"
            << logf("provider", m_providerType)
            << logf("error", parseError.errorString());
        ProviderError error;
        error.code = QStringLiteral("%1_invalid_json").arg(m_providerType);
        error.message = QStringLiteral("%1 响应不是有效 JSON。").arg(m_providerType);
        cleanupReply();
        emit failed(error);
        return;
    }
    emit payloadReceived(ProviderTransportPayload{document.object()});
}

void HttpSseChannel::cleanupReply()
{
    disposeActiveReply(false);
    m_parser.reset();
    m_rawBuffer.clear();
    m_sseMode = false;
}

void HttpSseChannel::trySwitchToSseMode()
{
    if (looksLikeSsePayload(m_rawBuffer)) {
        m_sseMode = true;
        QByteArray buffered;
        buffered.swap(m_rawBuffer);
        m_parser->feed(buffered);
        processSseEvents();
    }
}

void HttpSseChannel::processSseEvents()
{
    if (!m_parser) {
        return;
    }

    while (m_parser && m_parser->hasNext()) {
        const auto event = m_parser->takeNext();
        handleSseMessage(event.eventName, event.data);
    }
}

bool HttpSseChannel::looksLikeSsePayload(const QByteArray &buffer) const
{
    if (buffer.startsWith("event:") || buffer.startsWith("data:") || buffer.startsWith(":")) {
        return true;
    }
    return buffer.contains("\nevent:") || buffer.contains("\ndata:") || buffer.contains("\n:");
}
