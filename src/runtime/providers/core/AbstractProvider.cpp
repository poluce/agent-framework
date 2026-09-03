#include "AbstractProvider.h"

#include "providers/transport/HttpSseChannel.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace {
ProviderItem &ensureFallbackItem(QList<ProviderItem> &items, ProviderItemKind kind)
{
    if (items.isEmpty() || items.last().kind != kind) {
        items.append(kind == ProviderItemKind::Reasoning
                         ? ProviderItem::makeReasoning({})
                         : ProviderItem::makeAssistantText({}));
    }
    return items.last();
}

void appendFallbackText(ProviderTurnState &state, const QString &text, bool reasoning)
{
    ProviderItem &item = ensureFallbackItem(
        state.fallbackOutputItems,
        reasoning ? ProviderItemKind::Reasoning : ProviderItemKind::AssistantMessage);
    if (reasoning) {
        item.reasoningText.append(text);
    } else {
        if (item.parts.isEmpty())
            item.parts.append(ProviderMessagePart::makeText({}));
        item.parts[0].text.append(text);
    }
}

void upsertFallbackTool(ProviderTurnState &state, const ProviderDeltaPayload &call)
{
    const auto found = state.fallbackFunctionCallIndices.constFind(call.toolCallId);
    if (found == state.fallbackFunctionCallIndices.constEnd()) {
        state.fallbackFunctionCallIndices.insert(call.toolCallId,
                                                  state.fallbackOutputItems.size());
        state.fallbackOutputItems.append(
            call.isServerTool
                ? ProviderItem::makeServerToolCall(call.toolCallId, call.toolName,
                                                   call.arguments, call.rawArguments)
                : ProviderItem::makeFunctionCall(call.toolCallId, call.toolName,
                                                 call.arguments, call.rawArguments));
        return;
    }
    ProviderItem &item = state.fallbackOutputItems[found.value()];
    if (!call.toolName.isEmpty())
        item.name = call.toolName;
    // Completed 才带完整参数；Started/空 delta 不得把已有 raw 抹成空
    if (call.rawArguments.trimmed().isEmpty() && call.arguments.isEmpty())
        return;
    if (!call.arguments.isEmpty())
        item.arguments = call.arguments;
    if (!call.rawArguments.trimmed().isEmpty())
        item.rawArguments = call.rawArguments;
    else if (item.rawArguments.trimmed().isEmpty() && !item.arguments.isEmpty())
        item.rawArguments = compactJson(item.arguments);
}

bool hasUnresolvedBlob(const ProviderMessagePart &part)
{
    switch (part.kind) {
    case ProviderPartKind::Image:
        return part.image.blobRef.hasBlobId()
               && !part.image.hasUri() && !part.image.hasInlineData();
    case ProviderPartKind::Audio:
        return part.audio.blobRef.hasBlobId()
               && !part.audio.hasUri() && !part.audio.hasInlineData();
    case ProviderPartKind::Document:
        return part.document.blobRef.hasBlobId()
               && !part.document.hasUri() && !part.document.hasInlineData();
    case ProviderPartKind::Video:
        return part.video.blobRef.hasBlobId()
               && !part.video.hasUri() && !part.video.hasInlineData();
    case ProviderPartKind::Text:
        return false;
    }
    return false;
}

bool hasUnresolvedBlob(const ProviderRequest &request)
{
    for (const ProviderItem &item : request.items) {
        for (const ProviderMessagePart &part : item.parts)
            if (hasUnresolvedBlob(part))
                return true;
        for (const ProviderMessagePart &part : item.outputParts)
            if (hasUnresolvedBlob(part))
                return true;
    }
    return false;
}
} // namespace

AbstractProvider::AbstractProvider(const QString &providerType, QObject *parent)
    : QObject(parent)
    , m_channel(std::make_unique<HttpSseChannel>(providerType))
{
    attachTransport(m_channel.get());
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &AbstractProvider::retryPendingTurn);
}

AbstractProvider::~AbstractProvider() = default;

void AbstractProvider::setRetryPolicy(const ProviderRetryPolicy &policy)
{
    m_retryPolicy = policy;
}

void AbstractProvider::setAuth(const ProviderAuth &auth)
{
    ProviderAuth normalized = auth;
    normalized.baseUrl = auth.baseUrl.trimmed();
    normalized.apiKey = auth.apiKey.trimmed();
    normalized.modelName = auth.modelName.trimmed();

    const bool modelCatalogKeyChanged = normalized.baseUrl != m_auth.baseUrl
                                        || normalized.apiKey != m_auth.apiKey;
    m_auth = normalized;
    if (modelCatalogKeyChanged) {
        invalidateModelCatalog();
    }
}

ProviderAuth AbstractProvider::auth() const
{
    return m_auth;
}

QString AbstractProvider::baseUrl() const
{
    return m_auth.baseUrl;
}

QString AbstractProvider::apiKey() const
{
    return m_auth.apiKey;
}

QString AbstractProvider::currentModel() const
{
    return m_auth.modelName;
}

void AbstractProvider::sendRequest(const ProviderRequest &request)
{
    beginProviderTurn();
    ensureModelsReadyOrRefresh(request);
}

void AbstractProvider::sendRequestWithoutModelRefresh(const ProviderRequest &request)
{
    beginProviderTurn();
    dispatchPendingRequest(request);
}

void AbstractProvider::ensureModelsReadyOrRefresh(const ProviderRequest &request)
{
    if (m_modelsLoaded) {
        dispatchPendingRequest(request);
        return;
    }
    m_pendingRequestAfterModelRefresh = request;
    requestModelRefresh();
}

void AbstractProvider::requestModelRefresh()
{
    if (m_modelRefreshInFlight) {
        return;
    }

    m_modelRefreshInFlight = true;

    QString startError;
    if (!startModelRefresh(&startError)) {
        failModelRefresh(startError.isEmpty()
                             ? QStringLiteral("模型刷新启动失败。")
                             : startError);
    }
}

void AbstractProvider::dispatchPendingRequest(const ProviderRequest &request)
{
    if (hasUnresolvedBlob(request)) {
        emitErrorOccurred(ProviderError{
            QStringLiteral("provider_blob_unavailable"),
            QStringLiteral("Provider 请求引用的本地媒体 Blob 不可用。")});
        return;
    }
    QString protocolError;
    // 账本只保存 ProviderBlobRef；buildRequest 在请求边界临时回填 blob 字节，
    // 因此线路校验允许本轮内联数据，但持久化 JSON 仍满足 validateForLedger。
    if (!request.validate(&protocolError, nullptr, -1)) {
        emitErrorOccurred(ProviderError{QStringLiteral("invalid_provider_request"),
                                        protocolError});
        return;
    }
    const ProviderError validationError = validateProviderRequest(request);
    if (validationError.isValid()) {
        emitErrorOccurred(validationError);
        return;
    }

    m_turnState.activeRequestId = request.requestId;

    const ProviderTransportRequest transportRequest = buildProviderTransportRequest(request);
    ProviderError transportError;
    if (!startProviderTransportRequest(transportRequest, &transportError)) {
        emitErrorOccurred(transportError.isValid()
                              ? transportError
                              : ProviderError{QStringLiteral("transport_start_failed"),
                                              QStringLiteral("Provider transport 启动失败。")});
        return;
    }

    // 启动成功才缓存：start 失败本身不可重试
    m_retryRequest = request;
}

void AbstractProvider::cancel()
{
    // 先停退避：等待期取消不再重发（channel 同步 cancel 再发 Cancelled）
    m_retryTimer.stop();
    m_pendingRequestAfterModelRefresh.reset();
    if (m_modelRefreshInFlight) {
        m_modelRefreshInFlight = false;
        cancelModelRefresh();
        emitCancelled();
        return;
    }
    cancelProviderRequest();
}

QList<ModelCapabilities> AbstractProvider::availableModels() const
{
    return m_availableModels;
}

void AbstractProvider::setLogContext(const AgentLogContext &logContext)
{
    m_logContext = logContext;
    m_channel->setLogContext(logContext);
}

AgentLogContext AbstractProvider::logContext() const
{
    return m_logContext;
}

ProviderTransportRequest AbstractProvider::buildStandardTransport(
    const QJsonObject &body, const bool expectsEventStream) const
{
    ProviderTransportRequest transport;
    transport.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    transport.contentType = QByteArrayLiteral("application/json");
    transport.accept = expectsEventStream
        ? QByteArrayLiteral("text/event-stream")
        : QByteArrayLiteral("application/json");
    transport.expectsEventStream = expectsEventStream;
    return transport;
}

void AbstractProvider::beginProviderTurn()
{
    m_nextEventSequence = 1;
    m_turnErrorEmitted = false;
    m_turnState = ProviderTurnState{};
    resetProviderTurnState();
    m_retryAttempts = 0;
    m_retryTimer.stop();
    m_retryRequest = ProviderRequest{};
}

void AbstractProvider::emitEvents(const QList<ProviderEvent> &events)
{
    for (const ProviderEvent &event : events) {
        emitProviderEvent(event);
    }
}

void AbstractProvider::processProviderPayload(const ProviderTransportPayload &payload)
{
    emitEvents(parseProviderTransportPayload(payload));
}

ProviderTurnState &AbstractProvider::turnState()
{
    return m_turnState;
}

const ProviderTurnState &AbstractProvider::turnState() const
{
    return m_turnState;
}

void AbstractProvider::seedAvailableModels(const QList<ModelCapabilities> &models)
{
    m_availableModels = models;
    m_modelsLoaded = true;
}

void AbstractProvider::completeModelRefresh(const QList<ModelCapabilities> &models)
{
    m_availableModels = models;
    m_modelsLoaded = true;
    m_modelRefreshInFlight = false;
    emit modelRefreshFinished();

    if (m_pendingRequestAfterModelRefresh.has_value()) {
        dispatchPendingRequest(m_pendingRequestAfterModelRefresh.value());
        m_pendingRequestAfterModelRefresh.reset();
    }
}

void AbstractProvider::failModelRefresh(const QString &errorMessage)
{
    m_modelRefreshInFlight = false;
    emit modelRefreshFailed(errorMessage);

    if (!m_pendingRequestAfterModelRefresh.has_value()) {
        return;
    }

    m_pendingRequestAfterModelRefresh.reset();
    ProviderError error;
    error.code = QStringLiteral("model_refresh_failed");
    error.message = errorMessage.isEmpty()
                        ? QStringLiteral("模型刷新失败。")
                        : errorMessage;
    emitErrorOccurred(error);
}

void AbstractProvider::invalidateModelCatalog()
{
    m_availableModels.clear();
    m_modelsLoaded = false;

    if (!m_modelRefreshInFlight) {
        return;
    }

    const bool hadPendingRequest = m_pendingRequestAfterModelRefresh.has_value();
    m_pendingRequestAfterModelRefresh.reset();
    m_modelRefreshInFlight = false;
    cancelModelRefresh();

    const QString reason = QStringLiteral("Provider 配置已变化，模型刷新已取消。");
    if (!hadPendingRequest) {
        emit modelRefreshFailed(reason);
        return;
    }

    ProviderError error;
    error.code = QStringLiteral("model_refresh_cancelled");
    error.message = reason;
    emitErrorOccurred(error);
}

void AbstractProvider::emitTextDelta(ProviderTextDelta delta)
{
    emitProviderEvent(ProviderEvent::fromTextDelta(delta));
}

void AbstractProvider::emitReasoningDelta(ProviderReasoningDelta delta)
{
    emitProviderEvent(ProviderEvent::fromReasoningDelta(delta));
}

void AbstractProvider::emitErrorOccurred(ProviderError error)
{
    if (maybeScheduleRetry(error))
        return;
    emitProviderEvent(ProviderEvent::fromError(error));
}

void AbstractProvider::handleTransportFailed(const ProviderError &error)
{
    // 取消优先于重试（退避等待期 cancel 也由此闭环）
    if (error.code == QStringLiteral("transport_canceled")) {
        emitCancelled();
        return;
    }
    emitErrorOccurred(error);
}

bool AbstractProvider::maybeScheduleRetry(ProviderError &error)
{
    // 无缓存请求禁止吞错（否则 timer 空转、无最终 Error）
    if (m_retryRequest.requestId.isEmpty()) {
        return false;
    }
    // 策略关 / 不可重试 / 消息已开始（流中失败重放语义不完整）
    if (m_retryPolicy.maxRetries <= 0 || !error.retryable || m_turnState.messageStarted) {
        return false;
    }
    if (m_retryAttempts >= m_retryPolicy.maxRetries) {
        error.attempts = m_retryAttempts;
        return false;
    }

    int delayMs = ProviderRetry::backoffDelayMs(m_retryAttempts);
    if (m_retryPolicy.respectRetryAfter && error.retryAfterMs >= 0
        && error.retryAfterMs <= m_retryPolicy.maxRetryAfterMs) {
        delayMs = error.retryAfterMs;
    }

    ++m_retryAttempts;
    LOGW(LogCat::Provider, logContext()) << "Provider 瞬时错误，退避后重试"
        << logf("code", error.code)
        << logf("retry", m_retryAttempts)
        << logf("maxRetries", m_retryPolicy.maxRetries)
        << logf("delayMs", delayMs);
    m_retryTimer.start(delayMs);
    return true;
}

void AbstractProvider::retryPendingTurn()
{
    // 禁止静默 return：否则 Loop 永远等不到终态
    if (m_turnState.messageStarted || m_retryRequest.requestId.isEmpty()) {
        ProviderError error;
        error.code = QStringLiteral("retry_aborted");
        error.message = m_turnState.messageStarted
            ? QStringLiteral("重试中止：消息已开始，流中失败不可重放。")
            : QStringLiteral("重试中止：可重发请求已失效。");
        error.attempts = m_retryAttempts;
        emitErrorOccurred(error);
        return;
    }

    const ProviderTransportRequest transport = buildProviderTransportRequest(m_retryRequest);
    ProviderError startError;
    if (!startProviderTransportRequest(transport, &startError)) {
        // start 失败 = 最终失败（不再二次重试判定）
        emitErrorOccurred(startError.isValid()
                              ? startError
                              : ProviderError{QStringLiteral("transport_start_failed"),
                                              QStringLiteral("重试传输启动失败。")});
    }
}

void AbstractProvider::emitCancelled()
{
    emitProviderEvent(ProviderEvent::cancelled());
}

void AbstractProvider::emitProviderEvent(ProviderEvent event)
{
    if (m_turnState.terminal)
        return;

    // ── 事件预处理：自动注入 messageId、创建隐含 message/part 事件 ──
    switch (event.kind) {
    case ProviderEventKind::ReasoningDelta: {
        const QString msgId = resolveMessageId(event.deltaPayload.base.messageId);
        ensureMessageStarted(msgId);
        ensureReasoningPartStarted(msgId);
        event.deltaPayload.base.messageId = msgId;
        event.deltaPayload.base.partIndex = m_activeReasoningPartIndex;
        appendFallbackText(m_turnState, event.deltaPayload.text, true);
        break;
    }
    case ProviderEventKind::TextDelta: {
        const QString msgId = resolveMessageId(event.deltaPayload.base.messageId);
        completeReasoningPartIfOpen(msgId);
        ensureMessageStarted(msgId);
        ensureTextPartStarted(msgId);
        event.deltaPayload.base.messageId = msgId;
        event.deltaPayload.base.partIndex = m_turnState.activeTextPartIndex;
        appendFallbackText(m_turnState, event.deltaPayload.text, false);
        break;
    }
    case ProviderEventKind::ToolCallStarted: {
        const QString msgId = resolveMessageId(event.deltaPayload.base.messageId);
        completeReasoningPartIfOpen(msgId);
        completeTextPartIfOpen(msgId);
        ensureMessageStarted(msgId);
        ensureToolPartStarted(event.deltaPayload.toolCallId, msgId, event.deltaPayload.base.partIndex);
        event.deltaPayload.base.messageId = msgId;
        event.deltaPayload.base.partIndex = m_turnState.toolPartIndices.value(event.deltaPayload.toolCallId);
        upsertFallbackTool(m_turnState, event.deltaPayload);
        break;
    }
    case ProviderEventKind::ToolCallCompleted: {
        const QString msgId = resolveMessageId(event.deltaPayload.base.messageId);
        completeReasoningPartIfOpen(msgId);
        completeTextPartIfOpen(msgId);
        ensureMessageStarted(msgId);
        if (!m_turnState.toolPartIndices.contains(event.deltaPayload.toolCallId)) {
            ensureToolPartStarted(event.deltaPayload.toolCallId, msgId, event.deltaPayload.base.partIndex);
        }
        event.deltaPayload.base.messageId = msgId;
        event.deltaPayload.base.partIndex = m_turnState.toolPartIndices.value(event.deltaPayload.toolCallId);
        // Started 时参数常未到齐；Completed 才有完整 rawArguments。
        // Chat Completions 的 MessageCompleted 常靠 fallbackOutputItems 填 outputItems，
        // 若不在此回填，下一轮全量回放会带空 arguments（DeepSeek 仍配对但工具语义丢光）。
        upsertFallbackTool(m_turnState, event.deltaPayload);
        break;
    }
    case ProviderEventKind::ImageOutput: {
        const QString msgId = resolveMessageId(event.deltaPayload.base.messageId);
        ensureMessageStarted(msgId);
        const int partIndex = resolvePartIndex(event.deltaPayload.base.partIndex);
        event.deltaPayload.base.messageId = msgId;
        event.deltaPayload.base.partIndex = partIndex;
        event.providerResponseId = msgId;

        ProviderContentPartStart partStart;
        partStart.base.messageId = msgId;
        partStart.base.partIndex = partIndex;
        partStart.partKind = ProviderStreamPartKind::Image;
        emitProviderEvent(ProviderEvent::contentPartStarted(partStart));
        break;
    }
    case ProviderEventKind::AudioDelta: {
        const QString msgId = resolveMessageId(event.deltaPayload.base.messageId);
        ensureMessageStarted(msgId);
        event.deltaPayload.base.messageId = msgId;
        ProviderItem &item = ensureFallbackItem(
            m_turnState.fallbackOutputItems, ProviderItemKind::AssistantMessage);
        if (item.parts.isEmpty()
            || item.parts.last().kind != ProviderPartKind::Audio) {
            item.parts.append(
                ProviderMessagePart::makeAudio(event.deltaPayload.audio));
        } else {
            ProviderAudioAsset &audio = item.parts.last().audio;
            audio.data.append(event.deltaPayload.audio.data);
            if (!event.deltaPayload.audio.uri.isEmpty())
                audio.uri = event.deltaPayload.audio.uri;
            if (!event.deltaPayload.audio.mimeType.isEmpty())
                audio.mimeType = event.deltaPayload.audio.mimeType;
            audio.transcript.append(event.deltaPayload.audio.transcript);
        }
        break;
    }
    case ProviderEventKind::TranscriptDelta: {
        const QString msgId = resolveMessageId(event.deltaPayload.base.messageId);
        ensureMessageStarted(msgId);
        event.deltaPayload.base.messageId = msgId;
        appendFallbackText(m_turnState, event.deltaPayload.text, false);
        break;
    }
    case ProviderEventKind::MessageCompleted: {
        const QString msgId = resolveMessageId(event.messageEnd.messageId);
        ensureMessageStarted(msgId);
        completeReasoningPartIfOpen(msgId);
        completeTextPartIfOpen(msgId);

        // 完成所有仍处于打开状态的 tool part
        const QStringList openToolIds = m_turnState.toolPartIndices.keys();
        for (const QString &toolId : openToolIds) {
            completeToolPartIfOpen(toolId, msgId);
        }
        event.messageEnd.messageId = msgId;
        if (event.messageEnd.outputItems.isEmpty())
            event.messageEnd.outputItems = m_turnState.fallbackOutputItems;
        const bool hasClientToolCall = std::any_of(
            event.messageEnd.outputItems.cbegin(),
            event.messageEnd.outputItems.cend(),
            [](const ProviderItem &item) {
                return item.kind == ProviderItemKind::FunctionCall;
            });
        if (hasClientToolCall) {
            for (ProviderItem &item : event.messageEnd.outputItems) {
                if (item.kind == ProviderItemKind::Reasoning)
                    item.reasoningMustReplay = true;
            }
        }
        m_turnState.terminal = true;
        break;
    }
    case ProviderEventKind::MessageStarted: {
        m_turnState.messageStarted = true;
        break;
    }
    default:
        break;
    }

    // 统编序号
    stampEventSequence(event);

    // ── 事件后处理：完成 tool/image part，去重 error ──
    switch (event.kind) {
    case ProviderEventKind::ToolCallCompleted:
        completeToolPartIfOpen(event.deltaPayload.toolCallId, event.deltaPayload.base.messageId);
        break;
    case ProviderEventKind::ImageOutput: {
        ProviderContentPartEnd partEnd;
        partEnd.base.messageId = event.deltaPayload.base.messageId;
        partEnd.base.partIndex = event.deltaPayload.base.partIndex;
        partEnd.partKind = ProviderStreamPartKind::Image;
        emitProviderEvent(ProviderEvent::contentPartCompleted(partEnd));
        break;
    }
    case ProviderEventKind::Error: {
        if (m_turnErrorEmitted) {
            return;
        }
        m_turnErrorEmitted = true;
        m_turnState.terminal = true;
        break;
    }
    case ProviderEventKind::Cancelled: {
        m_turnState.terminal = true;
        break;
    }
    default:
        break;
    }

    emit eventEmitted(event);
}


qint64 AbstractProvider::nextEventSequence()
{
    return m_nextEventSequence++;
}

void AbstractProvider::stampEventSequence(ProviderEvent &event)
{
    if (event.sequence == 0) {
        event.sequence = nextEventSequence();
    }

    const bool isDelta = (event.kind == ProviderEventKind::ContentPartStarted ||
                          event.kind == ProviderEventKind::ContentPartCompleted ||
                          event.kind == ProviderEventKind::TextDelta ||
                          event.kind == ProviderEventKind::ReasoningDelta ||
                          event.kind == ProviderEventKind::ToolCallStarted ||
                          event.kind == ProviderEventKind::ToolCallCompleted ||
                          event.kind == ProviderEventKind::ImageOutput);

    if (isDelta) {
        event.deltaPayload.base.sequence = event.sequence;
        if (event.kind == ProviderEventKind::ImageOutput) {
            event.providerResponseId = event.deltaPayload.base.messageId;
        }
    } else {
        switch (event.kind) {
        case ProviderEventKind::MessageStarted:
            event.messageStart.sequence = event.sequence;
            break;
        case ProviderEventKind::MessageCompleted:
            event.messageEnd.sequence = event.sequence;
            break;
        case ProviderEventKind::UsageUpdated:
            event.usage.sequence = event.sequence;
            break;
        case ProviderEventKind::ResponseMetadata:
            event.responseMetadata.sequence = event.sequence;
            event.providerResponseId = event.responseMetadata.providerResponseId;
            break;
        case ProviderEventKind::Error:
            event.error.sequence = event.sequence;
            break;
        default:
            break;
        }
    }
}

QString AbstractProvider::resolveMessageId(const QString &candidate) const
{
    return candidate.isEmpty() ? m_turnState.activeRequestId : candidate;
}

bool AbstractProvider::startModelRefresh(QString *errorMessage)
{
    if (m_auth.baseUrl.trimmed().isEmpty() || m_auth.apiKey.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("baseUrl 或 apiKey 未设置。");
        }
        return false;
    }

    if (m_modelFetchReply) {
        m_modelFetchReply->abort();
        m_modelFetchReply->deleteLater();
        m_modelFetchReply.clear();
    }

    QUrl url = buildModelsUrl(m_auth.baseUrl.trimmed());

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    m_channel->applyAuthHeaders(request, m_auth.apiKey);
    request.setRawHeader("Accept", QByteArrayLiteral("application/json"));

    QNetworkReply *reply = m_modelFetchAccessManager.get(request);
    m_modelFetchReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onModelFetchFinished(reply);
    });
    return true;
}

void AbstractProvider::cancelModelRefresh()
{
    if (!m_modelFetchReply) {
        return;
    }

    QNetworkReply *reply = m_modelFetchReply;
    m_modelFetchReply.clear();
    reply->abort();
    reply->deleteLater();
}

void AbstractProvider::onModelFetchFinished(QNetworkReply *reply)
{
    if (!reply) {
        return;
    }

    if (reply != m_modelFetchReply) {
        reply->deleteLater();
        return;
    }

    m_modelFetchReply.clear();

    const QByteArray body = reply->readAll();
    const auto networkError = reply->error();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError) {
        const QString detail = extractApiErrorMessage(body);
        failModelRefresh(detail.isEmpty()
                             ? QStringLiteral("获取模型列表失败。")
                             : detail);
        return;
    }

    QString error;
    const QList<ModelCapabilities> models = parseModelsPayload(body, &error);
    if (models.isEmpty()) {
        failModelRefresh(error.isEmpty()
                             ? QStringLiteral("模型列表为空。")
                             : error);
        return;
    }

    completeModelRefresh(models);
}

void AbstractProvider::cancelProviderRequest()
{
    m_channel->cancel();
}

int AbstractProvider::resolvePartIndex(int requestedIndex)
{
    if (requestedIndex >= 0) {
        m_turnState.nextSyntheticPartIndex = std::max(m_turnState.nextSyntheticPartIndex, requestedIndex + 1);
        return requestedIndex;
    }
    return m_turnState.nextSyntheticPartIndex++;
}

void AbstractProvider::ensureMessageStarted(const QString &messageId)
{
    if (m_turnState.messageStarted) {
        return;
    }

    ProviderMessageStart start;
    start.messageId = messageId;
    m_turnState.messageStarted = true;
    emitProviderEvent(ProviderEvent::messageStarted(start));
}

void AbstractProvider::ensureTextPartStarted(const QString &messageId)
{
    if (m_turnState.activeTextPartIndex >= 0) {
        return;
    }

    m_turnState.activeTextPartIndex = resolvePartIndex();

    ProviderContentPartStart part;
    part.base.messageId = messageId;
    part.base.partIndex = m_turnState.activeTextPartIndex;
    part.partKind = ProviderStreamPartKind::Text;
    emitProviderEvent(ProviderEvent::contentPartStarted(part));
}

void AbstractProvider::ensureReasoningPartStarted(const QString &messageId, int requestedIndex)
{
    if (m_activeReasoningPartIndex >= 0) {
        return;
    }

    m_activeReasoningPartIndex = resolvePartIndex(requestedIndex);

    ProviderContentPartStart part;
    part.base.messageId = messageId;
    part.base.partIndex = m_activeReasoningPartIndex;
    part.partKind = ProviderStreamPartKind::Reasoning;
    emitProviderEvent(ProviderEvent::contentPartStarted(part));
}

void AbstractProvider::ensureToolPartStarted(const QString &toolCallId, const QString &messageId, int requestedIndex)
{
    if (m_turnState.toolPartIndices.contains(toolCallId)) {
        return;
    }

    const int partIndex = resolvePartIndex(requestedIndex);
    m_turnState.toolPartIndices.insert(toolCallId, partIndex);

    ProviderContentPartStart part;
    part.base.messageId = messageId;
    part.base.partIndex = partIndex;
    part.partKind = ProviderStreamPartKind::ToolCall;
    emitProviderEvent(ProviderEvent::contentPartStarted(part));
}

void AbstractProvider::completeTextPartIfOpen(const QString &messageId)
{
    if (m_turnState.activeTextPartIndex < 0) {
        return;
    }

    ProviderContentPartEnd part;
    part.base.messageId = messageId;
    part.base.partIndex = m_turnState.activeTextPartIndex;
    part.partKind = ProviderStreamPartKind::Text;
    m_turnState.activeTextPartIndex = -1;
    emitProviderEvent(ProviderEvent::contentPartCompleted(part));
}

void AbstractProvider::completeReasoningPartIfOpen(const QString &messageId)
{
    if (m_activeReasoningPartIndex < 0) {
        return;
    }

    ProviderContentPartEnd part;
    part.base.messageId = messageId;
    part.base.partIndex = m_activeReasoningPartIndex;
    part.partKind = ProviderStreamPartKind::Reasoning;
    m_activeReasoningPartIndex = -1;
    emitProviderEvent(ProviderEvent::contentPartCompleted(part));
}

void AbstractProvider::completeToolPartIfOpen(const QString &toolCallId, const QString &messageId)
{
    auto &indices = m_turnState.toolPartIndices;
    const auto it = indices.find(toolCallId);
    if (it == indices.end()) {
        return;
    }

    ProviderContentPartEnd part;
    part.base.messageId = messageId;
    part.base.partIndex = it.value();
    part.partKind = ProviderStreamPartKind::ToolCall;
    indices.erase(it);
    emitProviderEvent(ProviderEvent::contentPartCompleted(part));
}


