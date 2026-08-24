#include "GeminiProvider.h"

#include "providers/core/HttpSseChannel.h"
#include "providers/core/ProviderRetryPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QUrl>
#include <QUuid>

namespace {

QJsonObject interactionPart(const ProviderMessagePart &part)
{
    QJsonObject wire;
    switch (part.kind) {
    case ProviderPartKind::Text:
        wire = {{QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), part.text}};
        break;
    case ProviderPartKind::Image:
        wire.insert(QStringLiteral("type"), QStringLiteral("image"));
        if (part.image.hasUri())
            wire.insert(QStringLiteral("uri"), part.image.uri);
        else
            wire.insert(QStringLiteral("data"),
                        QString::fromLatin1(part.image.data.toBase64()));
        wire.insert(QStringLiteral("mime_type"), part.image.mimeType);
        break;
    case ProviderPartKind::Audio:
        wire.insert(QStringLiteral("type"), QStringLiteral("audio"));
        if (part.audio.hasUri())
            wire.insert(QStringLiteral("uri"), part.audio.uri);
        else
            wire.insert(QStringLiteral("data"),
                        QString::fromLatin1(part.audio.data.toBase64()));
        wire.insert(QStringLiteral("mime_type"), part.audio.mimeType);
        break;
    case ProviderPartKind::Document:
        wire.insert(QStringLiteral("type"), QStringLiteral("document"));
        if (part.document.hasUri())
            wire.insert(QStringLiteral("uri"), part.document.uri);
        else
            wire.insert(QStringLiteral("data"),
                        QString::fromLatin1(part.document.data.toBase64()));
        wire.insert(QStringLiteral("mime_type"), part.document.mimeType);
        wire.insert(QStringLiteral("title"), part.document.title);
        break;
    case ProviderPartKind::Video:
        wire.insert(QStringLiteral("type"), QStringLiteral("video"));
        if (part.video.hasUri())
            wire.insert(QStringLiteral("uri"), part.video.uri);
        else
            wire.insert(QStringLiteral("data"),
                        QString::fromLatin1(part.video.data.toBase64()));
        wire.insert(QStringLiteral("mime_type"), part.video.mimeType);
        wire.insert(QStringLiteral("start_ms"), part.video.startMs);
        wire.insert(QStringLiteral("end_ms"), part.video.endMs);
        wire.insert(QStringLiteral("fps"), part.video.fps);
        break;
    }
    if (!part.mediaResolution.isEmpty())
        wire.insert(QStringLiteral("resolution"), part.mediaResolution);
    return wire;
}

QJsonObject generateContentPart(const ProviderMessagePart &part)
{
    QJsonObject wire;
    if (part.kind == ProviderPartKind::Text) {
        wire.insert(QStringLiteral("text"), part.text);
    } else {
        QString mimeType;
        QString uri;
        QByteArray data;
        switch (part.kind) {
        case ProviderPartKind::Text:
            break;
        case ProviderPartKind::Image:
            mimeType = part.image.mimeType;
            uri = part.image.uri;
            data = part.image.data;
            break;
        case ProviderPartKind::Audio:
            mimeType = part.audio.mimeType;
            uri = part.audio.uri;
            data = part.audio.data;
            break;
        case ProviderPartKind::Document:
            mimeType = part.document.mimeType;
            uri = part.document.uri;
            data = part.document.data;
            break;
        case ProviderPartKind::Video:
            mimeType = part.video.mimeType;
            uri = part.video.uri;
            data = part.video.data;
            break;
        }
        if (!uri.isEmpty()) {
            wire.insert(QStringLiteral("fileData"),
                        QJsonObject{{QStringLiteral("mimeType"), mimeType},
                                    {QStringLiteral("fileUri"), uri}});
        } else {
            wire.insert(QStringLiteral("inlineData"),
                        QJsonObject{{QStringLiteral("mimeType"), mimeType},
                                    {QStringLiteral("data"),
                                     QString::fromLatin1(data.toBase64())}});
        }
        if (part.kind == ProviderPartKind::Video
            && (part.video.startMs > 0 || part.video.endMs > 0 || part.video.fps > 0)) {
            QJsonObject metadata;
            if (part.video.startMs > 0)
                metadata.insert(QStringLiteral("startOffset"),
                                QString::number(part.video.startMs) + QStringLiteral("ms"));
            if (part.video.endMs > 0)
                metadata.insert(QStringLiteral("endOffset"),
                                QString::number(part.video.endMs) + QStringLiteral("ms"));
            if (part.video.fps > 0)
                metadata.insert(QStringLiteral("fps"), part.video.fps);
            wire.insert(QStringLiteral("videoMetadata"), metadata);
        }
    }
    if (!part.mediaResolution.isEmpty())
        wire.insert(QStringLiteral("mediaResolution"), part.mediaResolution);
    return wire;
}

ProviderMessagePart messagePartFromGenerateContent(const QJsonObject &wire)
{
    const QJsonObject inlineData = wire.value(QStringLiteral("inlineData")).toObject();
    const QJsonObject fileData = wire.value(QStringLiteral("fileData")).toObject();
    const QString mimeType = !inlineData.isEmpty()
        ? inlineData.value(QStringLiteral("mimeType")).toString()
        : fileData.value(QStringLiteral("mimeType")).toString();
    const QByteArray data = QByteArray::fromBase64(
        inlineData.value(QStringLiteral("data")).toString().toLatin1());
    const QString uri = fileData.value(QStringLiteral("fileUri")).toString();
    ProviderMessagePart part;
    if (mimeType.startsWith(QStringLiteral("image/"))) {
        ProviderImageAsset asset;
        asset.mimeType = mimeType;
        asset.data = data;
        asset.uri = uri;
        part = ProviderMessagePart::makeImage(asset);
    } else if (mimeType.startsWith(QStringLiteral("audio/"))) {
        ProviderAudioAsset asset;
        asset.mimeType = mimeType;
        asset.data = data;
        asset.uri = uri;
        part = ProviderMessagePart::makeAudio(asset);
    } else if (mimeType.startsWith(QStringLiteral("video/"))) {
        ProviderVideoAsset asset;
        asset.mimeType = mimeType;
        asset.data = data;
        asset.uri = uri;
        const QJsonObject metadata = wire.value(QStringLiteral("videoMetadata")).toObject();
        asset.fps = metadata.value(QStringLiteral("fps")).toDouble();
        part = ProviderMessagePart::makeVideo(asset);
    } else {
        ProviderDocumentAsset asset;
        asset.mimeType = mimeType;
        asset.data = data;
        asset.uri = uri;
        part = ProviderMessagePart::makeDocument(asset);
    }
    part.mediaResolution = wire.value(QStringLiteral("mediaResolution")).toString();
    return part;
}

QString geminiServerCallType(const QString &name)
{
    if (name == ProviderServerToolName::WebSearch) return QStringLiteral("google_search_call");
    if (name == ProviderServerToolName::FileSearch) return QStringLiteral("file_search_call");
    if (name == ProviderServerToolName::CodeInterpreter) return QStringLiteral("code_execution_call");
    if (name == ProviderServerToolName::UrlContext) return QStringLiteral("url_context_call");
    if (name == ProviderServerToolName::GoogleMaps) return QStringLiteral("google_maps_call");
    if (name == ProviderServerToolName::Mcp) return QStringLiteral("mcp_server_tool_call");
    if (name == ProviderServerToolName::Computer) return QStringLiteral("computer_use_call");
    return name + QStringLiteral("_call");
}

QString geminiServerToolName(const QString &type)
{
    if (type.startsWith(QStringLiteral("google_search"))) return ProviderServerToolName::WebSearch;
    if (type.startsWith(QStringLiteral("file_search"))) return ProviderServerToolName::FileSearch;
    if (type.startsWith(QStringLiteral("code_execution"))) return ProviderServerToolName::CodeInterpreter;
    if (type.startsWith(QStringLiteral("url_context"))) return ProviderServerToolName::UrlContext;
    if (type.startsWith(QStringLiteral("google_maps"))) return ProviderServerToolName::GoogleMaps;
    if (type.startsWith(QStringLiteral("mcp_server"))) return ProviderServerToolName::Mcp;
    if (type.startsWith(QStringLiteral("computer_use"))) return ProviderServerToolName::Computer;
    return {};
}

QList<ProviderMessagePart> messagePartsFromInteraction(const QJsonArray &content)
{
    QList<ProviderMessagePart> parts;
    for (const QJsonValue &value : content) {
        const QJsonObject wire = value.toObject();
        const QString type = wire.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("text")) {
            ProviderMessagePart part =
                ProviderMessagePart::makeText(wire.value(QStringLiteral("text")).toString());
            for (const QJsonValue &annotationValue :
                 wire.value(QStringLiteral("annotations")).toArray()) {
                const QJsonObject annotation = annotationValue.toObject();
                if (annotation.value(QStringLiteral("type")).toString()
                    == QStringLiteral("url_citation")) {
                    part.citations.append(ProviderCitation{
                        annotation.value(QStringLiteral("url")).toString(),
                        annotation.value(QStringLiteral("title")).toString(),
                        annotation.value(QStringLiteral("snippet")).toString(),
                        annotation.value(QStringLiteral("start_index")).toInt(-1),
                        annotation.value(QStringLiteral("end_index")).toInt(-1)});
                }
            }
            parts.append(part);
        } else if (type == QStringLiteral("image")) {
            ProviderImageAsset image;
            image.uri = wire.value(QStringLiteral("uri")).toString();
            image.data = QByteArray::fromBase64(
                wire.value(QStringLiteral("data")).toString().toLatin1());
            image.mimeType = wire.value(QStringLiteral("mime_type")).toString();
            parts.append(ProviderMessagePart::makeImage(image));
        } else if (type == QStringLiteral("audio")) {
            ProviderAudioAsset audio;
            audio.uri = wire.value(QStringLiteral("uri")).toString();
            audio.data = QByteArray::fromBase64(
                wire.value(QStringLiteral("data")).toString().toLatin1());
            audio.mimeType = wire.value(QStringLiteral("mime_type")).toString();
            audio.transcript = wire.value(QStringLiteral("transcript")).toString();
            parts.append(ProviderMessagePart::makeAudio(audio));
        } else if (type == QStringLiteral("document")) {
            ProviderDocumentAsset document;
            document.uri = wire.value(QStringLiteral("uri")).toString();
            document.data = QByteArray::fromBase64(
                wire.value(QStringLiteral("data")).toString().toLatin1());
            document.mimeType = wire.value(QStringLiteral("mime_type")).toString();
            document.title = wire.value(QStringLiteral("title")).toString();
            parts.append(ProviderMessagePart::makeDocument(document));
        } else if (type == QStringLiteral("video")) {
            ProviderVideoAsset video;
            video.uri = wire.value(QStringLiteral("uri")).toString();
            video.data = QByteArray::fromBase64(
                wire.value(QStringLiteral("data")).toString().toLatin1());
            video.mimeType = wire.value(QStringLiteral("mime_type")).toString();
            video.startMs = wire.value(QStringLiteral("start_ms")).toInt();
            video.endMs = wire.value(QStringLiteral("end_ms")).toInt();
            video.fps = wire.value(QStringLiteral("fps")).toDouble();
            parts.append(ProviderMessagePart::makeVideo(video));
        }
    }
    return parts;
}

QList<ProviderItem> itemsFromInteractionSteps(const QJsonArray &steps)
{
    QList<ProviderItem> items;
    for (const QJsonValue &value : steps) {
        const QJsonObject step = value.toObject();
        const QString type = step.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("thought")) {
            QStringList summaries;
            for (const QJsonValue &summary : step.value(QStringLiteral("summary")).toArray())
                if (!summary.toObject().value(QStringLiteral("text")).toString().isEmpty())
                    summaries.append(summary.toObject().value(QStringLiteral("text")).toString());
            ProviderItem item = ProviderItem::makeReasoning(
                summaries.join(QLatin1Char('\n')),
                step.value(QStringLiteral("signature")).toString(), false, true);
            item.itemId = step.value(QStringLiteral("id")).toString(item.itemId);
            items.append(item);
        } else if (type == QStringLiteral("model_output")
                   || type == QStringLiteral("user_input")) {
            const QList<ProviderMessagePart> parts =
                messagePartsFromInteraction(step.value(QStringLiteral("content")).toArray());
            ProviderItem item = type == QStringLiteral("model_output")
                ? ProviderItem::makeAssistantMessage(parts)
                : ProviderItem::makeUserMessage(parts);
            item.itemId = step.value(QStringLiteral("id")).toString(item.itemId);
            items.append(item);
        } else if (type == QStringLiteral("function_call")) {
            ProviderItem item = ProviderItem::makeFunctionCall(
                step.value(QStringLiteral("id")).toString(),
                step.value(QStringLiteral("name")).toString(),
                step.value(QStringLiteral("arguments")).toObject(),
                compactJson(step.value(QStringLiteral("arguments"))));
            item.reasoningSignature = step.value(QStringLiteral("signature")).toString();
            item.reasoningMustReplay = !item.reasoningSignature.isEmpty();
            items.append(item);
        } else if (type == QStringLiteral("function_result")) {
            ProviderItem item = ProviderItem::makeFunctionCallOutput(
                step.value(QStringLiteral("call_id")).toString(),
                step.value(QStringLiteral("name")).toString(),
                compactJson(step.value(QStringLiteral("result"))),
                step.value(QStringLiteral("is_error")).toBool(false));
            item.reasoningSignature = step.value(QStringLiteral("signature")).toString();
            item.reasoningMustReplay = !item.reasoningSignature.isEmpty();
            items.append(item);
        } else {
            const QString name = geminiServerToolName(type);
            if (name.isEmpty()) {
                qWarning().noquote()
                    << QStringLiteral("Gemini adapter 丢弃未知 interaction step type：%1")
                           .arg(type);
                continue;
            }
            const bool isResult = type.endsWith(QStringLiteral("_result"));
            ProviderItem item = isResult
                ? ProviderItem::makeServerToolResult(
                      step.value(QStringLiteral("call_id")).toString(), name,
                      compactJson(step.value(QStringLiteral("result"))), {},
                      step.value(QStringLiteral("is_error")).toBool(false))
                : ProviderItem::makeServerToolCall(
                      step.value(QStringLiteral("id")).toString(), name,
                      step.value(QStringLiteral("arguments")).toObject(),
                      compactJson(step.value(QStringLiteral("arguments"))));
            item.reasoningSignature = step.value(QStringLiteral("signature")).toString();
            item.reasoningMustReplay = !item.reasoningSignature.isEmpty();
            items.append(item);
        }
    }
    return items;
}

} // namespace

// ---- GeminiProvider ----

GeminiProvider::GeminiProvider(const ProviderProtocolFamily protocolFamily, QObject *parent)
    : AbstractProvider(protocolFamily == ProviderProtocolFamily::GeminiInteractions
                           ? QStringLiteral("google-interactions")
                           : QStringLiteral("google"),
                       parent),
      m_protocolFamily(protocolFamily)
{
    m_channel->setAuthMode(HttpSseChannel::AuthMode::GoogApiKey);
}

GeminiProvider::~GeminiProvider() = default;

// ---- URL ----

QUrl GeminiProvider::buildModelsUrl(const QString &baseUrl) const
{
    if (baseUrl.endsWith(QStringLiteral("/models")) || baseUrl.contains(QStringLiteral("/models?"))) {
        return QUrl(baseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/v1beta"))) {
        return QUrl(baseUrl + QStringLiteral("/models"));
    }
    if (baseUrl.endsWith(QLatin1Char('/'))) {
        return QUrl(baseUrl + QStringLiteral("v1beta/models"));
    }
    return QUrl(baseUrl + QStringLiteral("/v1beta/models"));
}

QList<ModelCapabilities> GeminiProvider::parseModelsPayload(const QByteArray &body,
                                                              QString *errorMessage) const
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Gemini 模型列表不是有效 JSON。");
        }
        return {};
    }

    QList<ModelCapabilities> models;
    const QJsonArray data = document.object().value(QStringLiteral("models")).toArray();
    for (const QJsonValue &value : data) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        ModelCapabilities caps;
        caps.modelId = obj.value(QStringLiteral("name")).toString().trimmed();
        // Gemini API 返回的 name 格式如 "models/gemini-2.5-flash"，取最后一段
        if (caps.modelId.contains(QLatin1Char('/'))) {
            caps.modelId = caps.modelId.section(QLatin1Char('/'), -1);
        }
        if (caps.modelId.isEmpty()) {
            continue;
        }

        const QJsonArray methods = obj.value(QStringLiteral("supportedGenerationMethods")).toArray();
        const bool supportsInteractions = std::any_of(methods.constBegin(), methods.constEnd(),
            [](const QJsonValue &m) { return m.toString() == QStringLiteral("interactions"); });
        const bool supportsGenerateContent = std::any_of(
            methods.constBegin(), methods.constEnd(), [](const QJsonValue &m) {
                return m.toString() == QStringLiteral("generateContent");
            });

        caps.enable(ProviderCapability::TextInput)
            .enable(ProviderCapability::TextOutput)
            .set(ProviderCapability::ImageOutput,
                 caps.modelId.contains(QStringLiteral("image"), Qt::CaseInsensitive))
            .set(ProviderCapability::AudioOutput,
                 caps.modelId.contains(QStringLiteral("audio"), Qt::CaseInsensitive)
                     || caps.modelId.contains(QStringLiteral("tts"), Qt::CaseInsensitive)
                     || caps.modelId.contains(QStringLiteral("live"), Qt::CaseInsensitive))
            .enable(ProviderCapability::ImageInput)
            .enable(ProviderCapability::AudioInput)
            .enable(ProviderCapability::DocumentInput)
            .enable(ProviderCapability::VideoInput)
            .set(ProviderCapability::ToolCalling,
                 m_protocolFamily == ProviderProtocolFamily::GeminiInteractions
                     ? supportsInteractions : supportsGenerateContent)
            .set(ProviderCapability::ToolChoice,
                 m_protocolFamily == ProviderProtocolFamily::GeminiGenerateContent)
            .enable(ProviderCapability::MaxOutputTokens)
            .set(ProviderCapability::Reasoning,
                 caps.modelId.contains(QStringLiteral("thinking"), Qt::CaseInsensitive)
                     || caps.modelId.contains(QStringLiteral("2.5"), Qt::CaseInsensitive))
            .set(ProviderCapability::Continuation,
                 m_protocolFamily == ProviderProtocolFamily::GeminiInteractions)
            .enable(ProviderCapability::ServerTools)
            .enable(ProviderCapability::Citations)
            .enable(ProviderCapability::ResponseFormat)
            .enable(ProviderCapability::SamplingTopP)
            .enable(ProviderCapability::SamplingSeed)
            .enable(ProviderCapability::SamplingStop)
            .enable(ProviderCapability::PresencePenalty)
            .enable(ProviderCapability::FrequencyPenalty)
            .enable(ProviderCapability::BackgroundExecution)
            .enable(ProviderCapability::TopK)
            .enable(ProviderCapability::MediaResolution)
            .enable(ProviderCapability::UrlContext)
            .enable(ProviderCapability::GoogleMapsGrounding);
        caps.supportedServerTools = {
            ProviderServerToolName::WebSearch,
            ProviderServerToolName::FileSearch,
            ProviderServerToolName::CodeInterpreter,
            ProviderServerToolName::UrlContext,
            ProviderServerToolName::GoogleMaps,
            ProviderServerToolName::Mcp,
            ProviderServerToolName::Computer};
        models.append(caps);
    }

    if (models.isEmpty() && errorMessage) {
        *errorMessage = QStringLiteral("Gemini 模型列表为空。");
    }
    return models;
}

bool GeminiProvider::startProviderTransportRequest(const ProviderTransportRequest &request,
                                                     ProviderError *error)
{
    const QUrl url = buildRequestUrl(request);
    const HttpSseChannel::StartResult result = m_channel->start(url, m_auth.apiKey, request);
    if (!result.accepted) {
        if (error) {
            *error = result.error;
        }
        return false;
    }
    return true;
}

QUrl GeminiProvider::buildRequestUrl(const ProviderTransportRequest &request) const
{
    QString base = m_auth.baseUrl.trimmed();
    QUrl url;
    if (m_protocolFamily == ProviderProtocolFamily::GeminiInteractions) {
        if (base.endsWith(QStringLiteral("/interactions"))
            || base.contains(QStringLiteral("/interactions?"))) {
            url = QUrl(base);
        } else if (base.endsWith(QStringLiteral("/v1beta"))) {
            url = QUrl(base + QStringLiteral("/interactions"));
        } else if (base.endsWith(QLatin1Char('/'))) {
            url = QUrl(base + QStringLiteral("v1beta/interactions"));
        } else {
            url = QUrl(base + QStringLiteral("/v1beta/interactions"));
        }
    } else {
        const QString method = request.expectsEventStream
            ? QStringLiteral(":streamGenerateContent?alt=sse")
            : QStringLiteral(":generateContent");
        if (base.contains(QStringLiteral(":generateContent"))
            || base.contains(QStringLiteral(":streamGenerateContent"))) {
            url = QUrl(base);
        } else {
            while (base.endsWith(QLatin1Char('/')))
                base.chop(1);
            if (base.endsWith(QStringLiteral("/models"))) {
                url = QUrl(base + QLatin1Char('/') + m_auth.modelName + method);
            } else if (base.endsWith(QStringLiteral("/v1beta"))) {
                url = QUrl(base + QStringLiteral("/models/")
                           + m_auth.modelName + method);
            } else {
                base += QStringLiteral("/v1beta");
                url = QUrl(base + QStringLiteral("/models/")
                           + m_auth.modelName + method);
            }
        }
    }
    return url;
}

// ---- 验证 ----

ProviderError GeminiProvider::validateProviderRequest(const ProviderRequest &request) const
{
    if (request.protocolFamily != ProviderProtocolFamily::Auto
        && request.protocolFamily != m_protocolFamily) {
        return ProviderError{QStringLiteral("protocol_family_mismatch"),
                             QStringLiteral("Gemini 适配器实例不能处理指定的协议族。")};
    }
    if (m_auth.modelName.trimmed().isEmpty()) {
        return ProviderError{QStringLiteral("model_required"),
                             QStringLiteral("Gemini 需要指定模型名称。")};
    }
    for (const ProviderItem &item : request.items) {
        if (item.kind == ProviderItemKind::Program
            || item.kind == ProviderItemKind::ProgramOutput
            || item.kind == ProviderItemKind::ApprovalRequest
            || item.kind == ProviderItemKind::ApprovalResponse
            || item.kind == ProviderItemKind::Compaction) {
            return ProviderError{
                QStringLiteral("unsupported_provider_item"),
                QStringLiteral("Gemini 不支持该 ProviderItem kind：%1")
                    .arg(static_cast<int>(item.kind))};
        }
        if (m_protocolFamily == ProviderProtocolFamily::GeminiGenerateContent
            && item.isServerToolRelated()
            && item.name != ProviderServerToolName::CodeInterpreter) {
            return ProviderError{
                QStringLiteral("server_tool_history_not_supported"),
                QStringLiteral("Gemini generateContent 只能在线路中回放 code_interpreter 内置工具。")};
        }
    }
    if (m_protocolFamily == ProviderProtocolFamily::GeminiInteractions
        && !request.providerCachedContentId.isEmpty()) {
        return ProviderError{
            QStringLiteral("cached_content_not_supported"),
            QStringLiteral("Gemini Interactions 端点不接受 generateContent cachedContent。")};
    }
    for (const ProviderToolSpecification &tool : request.tools) {
        if (!tool.outputSchema.isEmpty() || tool.strictSchema || tool.deferLoading
            || !tool.allowedCallers.isEmpty()) {
            return ProviderError{
                QStringLiteral("tool_extension_not_supported"),
                QStringLiteral("Gemini function 工具不支持协议中的高级 schema/caller 扩展。")};
        }
    }
    if (request.toolChoice.allowParallel != ProviderTriState::Unset) {
        return ProviderError{
            QStringLiteral("parallel_tool_choice_not_supported"),
                             QStringLiteral("Gemini 不提供可移植的并行工具开关。")};
    }
    if (request.requestLogprobs) {
        return ProviderError{QStringLiteral("logprobs_not_supported"),
                             QStringLiteral("Gemini Interactions 不支持 logprobs。")};
    }
    if (m_protocolFamily == ProviderProtocolFamily::GeminiInteractions
        && request.reasoning.budgetTokens > 0) {
        return ProviderError{
            QStringLiteral("reasoning_budget_not_supported"),
            QStringLiteral("Gemini Interactions 使用 thinking_level，不接受 thinking budget。")};
    }
    if (!request.responseInclude.isEmpty()
        || !request.providerConversationId.isEmpty()
        || !request.metadata.isEmpty()
        || (m_protocolFamily == ProviderProtocolFamily::GeminiGenerateContent
            && (request.backgroundExecution != ProviderTriState::Unset
                || request.storeServerState != ProviderTriState::Unset
                || !request.continuationId.isEmpty()))) {
        return ProviderError{
            QStringLiteral("request_option_not_supported"),
            QStringLiteral("Gemini 方言不支持所请求的会话/include/metadata 状态选项。")};
    }
    return {};
}

// ---- 请求构造 ----

QJsonArray GeminiProvider::buildToolDefinitions(const ProviderRequest &request) const
{
    QJsonArray tools;
    for (const ProviderToolSpecification &tool : request.tools) {
        QJsonObject def;
        def.insert(QStringLiteral("type"), QStringLiteral("function"));
        def.insert(QStringLiteral("name"), tool.name);
        def.insert(QStringLiteral("description"), tool.description);
        def.insert(QStringLiteral("parameters"), tool.inputSchema);
        tools.append(def);
    }
    return tools;
}

ProviderTransportRequest GeminiProvider::buildProviderTransportRequest(const ProviderRequest &request) const
{
    return m_protocolFamily == ProviderProtocolFamily::GeminiInteractions
        ? buildInteractionsRequest(request)
        : buildGenerateContentRequest(request);
}

ProviderTransportRequest GeminiProvider::buildInteractionsRequest(
    const ProviderRequest &request) const
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), m_auth.modelName);
    body.insert(QStringLiteral("stream"), request.stream);
    if (request.storeServerState != ProviderTriState::Unset)
        body.insert(QStringLiteral("store"), request.storeServerState == ProviderTriState::Yes);
    if (request.backgroundExecution != ProviderTriState::Unset)
        body.insert(QStringLiteral("background"),
                    request.backgroundExecution == ProviderTriState::Yes);

    QJsonObject generationConfig;
    if (request.temperature >= 0.0)
        generationConfig.insert(QStringLiteral("temperature"), request.temperature);
    if (request.maxOutputTokens > 0)
        generationConfig.insert(QStringLiteral("max_output_tokens"), request.maxOutputTokens);
    if (request.sampling.topP >= 0.0)
        generationConfig.insert(QStringLiteral("top_p"), request.sampling.topP);
    if (request.sampling.topK >= 0)
        generationConfig.insert(QStringLiteral("top_k"), request.sampling.topK);
    if (request.sampling.seed >= 0)
        generationConfig.insert(QStringLiteral("seed"),
                                static_cast<double>(request.sampling.seed));
    if (request.sampling.penaltiesRequested) {
        generationConfig.insert(QStringLiteral("presence_penalty"),
                                request.sampling.presencePenalty);
        generationConfig.insert(QStringLiteral("frequency_penalty"),
                                request.sampling.frequencyPenalty);
    }
    if (!request.sampling.stop.isEmpty())
        generationConfig.insert(QStringLiteral("stop_sequences"),
                                QJsonArray::fromStringList(request.sampling.stop));
    if (!request.mediaResolution.isEmpty())
        generationConfig.insert(QStringLiteral("media_resolution"),
                                request.mediaResolution);
    if (request.reasoning.enabled) {
        const QString effort = toString(request.reasoning.effort);
        if (!effort.isEmpty())
            generationConfig.insert(QStringLiteral("thinking_level"), effort);
        if (request.reasoning.includeSummary)
            body.insert(QStringLiteral("thinking_summaries"), QStringLiteral("auto"));
    }
    QJsonArray responseFormats;
    const bool explicitResponseFormat =
        request.responseFormat.kind != ProviderResponseFormatKind::None
        || request.desiredOutput.imageEnabled || request.desiredOutput.audioEnabled;
    if (explicitResponseFormat && request.desiredOutput.textEnabled) {
        QJsonObject textFormat{{QStringLiteral("type"), QStringLiteral("text")}};
        if (request.responseFormat.kind != ProviderResponseFormatKind::None) {
            textFormat.insert(QStringLiteral("mime_type"),
                              QStringLiteral("application/json"));
            if (request.responseFormat.kind == ProviderResponseFormatKind::JsonSchema)
                textFormat.insert(QStringLiteral("schema"),
                                  request.responseFormat.jsonSchema);
            body.insert(QStringLiteral("response_mime_type"),
                        QStringLiteral("application/json"));
        }
        responseFormats.append(textFormat);
    }
    if (request.desiredOutput.imageEnabled)
        responseFormats.append(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("image")}});
    if (request.desiredOutput.audioEnabled) {
        QJsonObject audioFormat{{QStringLiteral("type"), QStringLiteral("audio")}};
        if (!request.audio.outputFormat.isEmpty())
            audioFormat.insert(QStringLiteral("mime_type"),
                               request.audio.outputFormat);
        responseFormats.append(audioFormat);
    }
    if (!responseFormats.isEmpty()) {
        body.insert(QStringLiteral("response_format"),
                    responseFormats.size() == 1
                        ? responseFormats.first() : QJsonValue(responseFormats));
    }
    if (!generationConfig.isEmpty())
        body.insert(QStringLiteral("generation_config"), generationConfig);

    // 默认无状态全量回放：仅当请求显式带 continuationId 才走 previous_interaction_id。
    // 禁止用成员 m_interactionId 偷偷有状态——兼容网关与本地完整 items 回放。
    const QString continuation = request.continuationId;
    if (!continuation.isEmpty())
        body.insert(QStringLiteral("previous_interaction_id"), continuation);

    QJsonArray input;
    const QList<ProviderItem> encodedItems =
        !continuation.isEmpty() && !request.items.isEmpty()
            ? QList<ProviderItem>{request.items.last()} : request.items;
    for (const ProviderItem &item : encodedItems) {
        QJsonObject step;
        switch (item.kind) {
        case ProviderItemKind::UserMessage:
        case ProviderItemKind::AssistantMessage: {
            step.insert(QStringLiteral("type"),
                        item.kind == ProviderItemKind::UserMessage
                            ? QStringLiteral("user_input") : QStringLiteral("model_output"));
            QJsonArray parts;
            for (const ProviderMessagePart &part : item.parts)
                parts.append(interactionPart(part));
            step.insert(QStringLiteral("content"), parts);
            break;
        }
        case ProviderItemKind::FunctionCall:
            step.insert(QStringLiteral("type"), QStringLiteral("function_call"));
            step.insert(QStringLiteral("id"), item.callId);
            step.insert(QStringLiteral("name"), item.name);
            step.insert(QStringLiteral("arguments"), item.arguments);
            if (!item.reasoningSignature.isEmpty())
                step.insert(QStringLiteral("signature"), item.reasoningSignature);
            break;
        case ProviderItemKind::FunctionCallOutput:
            step.insert(QStringLiteral("type"), QStringLiteral("function_result"));
            step.insert(QStringLiteral("call_id"), item.callId);
            step.insert(QStringLiteral("name"), item.name);
            step.insert(QStringLiteral("result"), QJsonArray{
                QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                            {QStringLiteral("text"), item.output}}});
            if (!item.reasoningSignature.isEmpty())
                step.insert(QStringLiteral("signature"), item.reasoningSignature);
            break;
        case ProviderItemKind::Reasoning:
            step.insert(QStringLiteral("type"), QStringLiteral("thought"));
            if (!item.reasoningText.isEmpty())
                step.insert(QStringLiteral("summary"), QJsonArray{
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                {QStringLiteral("text"), item.reasoningText}}});
            if (!item.reasoningSignature.isEmpty())
                step.insert(QStringLiteral("signature"), item.reasoningSignature);
            break;
        case ProviderItemKind::ServerToolCall:
            step.insert(QStringLiteral("type"), geminiServerCallType(item.name));
            step.insert(QStringLiteral("id"), item.callId);
            step.insert(QStringLiteral("arguments"), item.arguments);
            if (!item.reasoningSignature.isEmpty())
                step.insert(QStringLiteral("signature"), item.reasoningSignature);
            break;
        case ProviderItemKind::ServerToolResult: {
            QString resultType = geminiServerCallType(item.name);
            if (resultType.endsWith(QStringLiteral("_call")))
                resultType.chop(5);
            step.insert(QStringLiteral("type"), resultType + QStringLiteral("_result"));
            step.insert(QStringLiteral("call_id"), item.callId);
            QJsonParseError error;
            const QJsonDocument parsed =
                QJsonDocument::fromJson(item.output.toUtf8(), &error);
            if (error.error == QJsonParseError::NoError && parsed.isObject())
                step.insert(QStringLiteral("result"), parsed.object());
            else if (error.error == QJsonParseError::NoError && parsed.isArray())
                step.insert(QStringLiteral("result"), parsed.array());
            else
                step.insert(QStringLiteral("result"), item.output);
            step.insert(QStringLiteral("is_error"), item.isError);
            if (!item.reasoningSignature.isEmpty())
                step.insert(QStringLiteral("signature"), item.reasoningSignature);
            break;
        }
        case ProviderItemKind::Program:
        case ProviderItemKind::ProgramOutput:
        case ProviderItemKind::ApprovalRequest:
        case ProviderItemKind::ApprovalResponse:
        case ProviderItemKind::Compaction:
            continue;
        }
        input.append(step);
    }
    body.insert(QStringLiteral("input"), input);

    // system instruction（独立字段）
    const QString system = request.systemPrompt.trimmed();
    if (!system.isEmpty()) {
        body.insert(QStringLiteral("system_instruction"), system);
    }

    // tools
    if (!request.tools.isEmpty()) {
        body.insert(QStringLiteral("tools"), buildToolDefinitions(request));
    }
    if (request.toolChoice.mode != ProviderToolChoiceMode::ProviderDefault) {
        QJsonObject choice;
        switch (request.toolChoice.mode) {
        case ProviderToolChoiceMode::ProviderDefault:
            break;
        case ProviderToolChoiceMode::None:
            choice.insert(QStringLiteral("mode"), QStringLiteral("none"));
            break;
        case ProviderToolChoiceMode::Auto:
            choice.insert(QStringLiteral("mode"), QStringLiteral("auto"));
            break;
        case ProviderToolChoiceMode::Required:
            choice.insert(QStringLiteral("mode"), QStringLiteral("any"));
            break;
        case ProviderToolChoiceMode::Named:
            choice.insert(QStringLiteral("mode"), QStringLiteral("validated"));
            choice.insert(QStringLiteral("tools"),
                          QJsonArray{request.toolChoice.toolName});
            break;
        }
        if (!choice.isEmpty())
            body.insert(QStringLiteral("tool_choice"), choice);
    }

    return buildStandardTransport(body, request.stream);
}

ProviderTransportRequest GeminiProvider::buildGenerateContentRequest(
    const ProviderRequest &request) const
{
    QJsonObject body;
    QJsonArray contents;
    for (const ProviderItem &item : request.items) {
        QJsonObject content;
        QJsonArray parts;
        switch (item.kind) {
        case ProviderItemKind::UserMessage:
        case ProviderItemKind::AssistantMessage:
            content.insert(QStringLiteral("role"),
                           item.kind == ProviderItemKind::UserMessage
                               ? QStringLiteral("user") : QStringLiteral("model"));
            for (const ProviderMessagePart &part : item.parts)
                parts.append(generateContentPart(part));
            break;
        case ProviderItemKind::Reasoning: {
            content.insert(QStringLiteral("role"), QStringLiteral("model"));
            QJsonObject part{{QStringLiteral("text"), item.reasoningText},
                             {QStringLiteral("thought"), true}};
            if (!item.reasoningSignature.isEmpty())
                part.insert(QStringLiteral("thoughtSignature"),
                            item.reasoningSignature);
            parts.append(part);
            break;
        }
        case ProviderItemKind::FunctionCall: {
            content.insert(QStringLiteral("role"), QStringLiteral("model"));
            QJsonObject call{{QStringLiteral("name"), item.name},
                             {QStringLiteral("args"), item.arguments}};
            if (!item.callId.isEmpty())
                call.insert(QStringLiteral("id"), item.callId);
            QJsonObject part{{QStringLiteral("functionCall"), call}};
            if (!item.reasoningSignature.isEmpty())
                part.insert(QStringLiteral("thoughtSignature"),
                            item.reasoningSignature);
            parts.append(part);
            break;
        }
        case ProviderItemKind::FunctionCallOutput: {
            content.insert(QStringLiteral("role"), QStringLiteral("user"));
            QJsonObject response{{QStringLiteral("name"), item.name},
                                 {QStringLiteral("response"),
                                  QJsonObject{{QStringLiteral("output"),
                                               item.output},
                                              {QStringLiteral("isError"),
                                               item.isError}}}};
            if (!item.callId.isEmpty())
                response.insert(QStringLiteral("id"), item.callId);
            parts.append(QJsonObject{{QStringLiteral("functionResponse"),
                                      response}});
            for (const ProviderMessagePart &outputPart : item.outputParts)
                parts.append(generateContentPart(outputPart));
            break;
        }
        case ProviderItemKind::ServerToolCall:
            if (item.name == ProviderServerToolName::CodeInterpreter) {
                content.insert(QStringLiteral("role"), QStringLiteral("model"));
                parts.append(QJsonObject{
                    {QStringLiteral("executableCode"),
                     QJsonObject{{QStringLiteral("language"),
                                  item.details.value(QStringLiteral("language"))
                                      .toString(QStringLiteral("PYTHON"))},
                                 {QStringLiteral("code"),
                                  item.details.value(QStringLiteral("code"))
                                      .toString(item.rawArguments)}}}});
            }
            break;
        case ProviderItemKind::ServerToolResult:
            if (item.name == ProviderServerToolName::CodeInterpreter) {
                content.insert(QStringLiteral("role"), QStringLiteral("model"));
                parts.append(QJsonObject{
                    {QStringLiteral("codeExecutionResult"),
                     QJsonObject{{QStringLiteral("outcome"),
                                  item.isError ? QStringLiteral("OUTCOME_FAILED")
                                               : QStringLiteral("OUTCOME_OK")},
                                 {QStringLiteral("output"), item.output}}}});
            }
            break;
        case ProviderItemKind::Program:
        case ProviderItemKind::ProgramOutput:
        case ProviderItemKind::ApprovalRequest:
        case ProviderItemKind::ApprovalResponse:
        case ProviderItemKind::Compaction:
            break;
        }
        if (!parts.isEmpty()) {
            content.insert(QStringLiteral("parts"), parts);
            contents.append(content);
        }
    }
    body.insert(QStringLiteral("contents"), contents);

    if (!request.systemPrompt.trimmed().isEmpty()) {
        body.insert(QStringLiteral("systemInstruction"),
                    QJsonObject{{QStringLiteral("parts"),
                                 QJsonArray{QJsonObject{
                                     {QStringLiteral("text"),
                                      request.systemPrompt.trimmed()}}}}});
    }

    QJsonObject generationConfig;
    if (request.temperature >= 0)
        generationConfig.insert(QStringLiteral("temperature"), request.temperature);
    if (request.maxOutputTokens >= 0)
        generationConfig.insert(QStringLiteral("maxOutputTokens"),
                                request.maxOutputTokens);
    if (request.sampling.topP >= 0)
        generationConfig.insert(QStringLiteral("topP"), request.sampling.topP);
    if (request.sampling.topK >= 0)
        generationConfig.insert(QStringLiteral("topK"), request.sampling.topK);
    if (request.sampling.seed >= 0)
        generationConfig.insert(QStringLiteral("seed"),
                                static_cast<double>(request.sampling.seed));
    if (!request.sampling.stop.isEmpty())
        generationConfig.insert(QStringLiteral("stopSequences"),
                                QJsonArray::fromStringList(request.sampling.stop));
    if (request.sampling.penaltiesRequested) {
        generationConfig.insert(QStringLiteral("presencePenalty"),
                                request.sampling.presencePenalty);
        generationConfig.insert(QStringLiteral("frequencyPenalty"),
                                request.sampling.frequencyPenalty);
    }
    if (!request.mediaResolution.isEmpty())
        generationConfig.insert(QStringLiteral("mediaResolution"),
                                request.mediaResolution);
    if (request.responseFormat.kind != ProviderResponseFormatKind::None) {
        generationConfig.insert(QStringLiteral("responseMimeType"),
                                QStringLiteral("application/json"));
        if (request.responseFormat.kind == ProviderResponseFormatKind::JsonSchema)
            generationConfig.insert(QStringLiteral("responseSchema"),
                                    request.responseFormat.jsonSchema);
    }
    if (request.desiredOutput.imageEnabled || request.desiredOutput.audioEnabled) {
        QJsonArray modalities;
        if (request.desiredOutput.textEnabled)
            modalities.append(QStringLiteral("TEXT"));
        if (request.desiredOutput.imageEnabled)
            modalities.append(QStringLiteral("IMAGE"));
        if (request.desiredOutput.audioEnabled)
            modalities.append(QStringLiteral("AUDIO"));
        generationConfig.insert(QStringLiteral("responseModalities"), modalities);
    }
    // 门控仅 enabled（与 Interactions / Responses / DeepSeek 一致）：禁用态不得
    // 把残留 effort 写上线缆。requested 是「显式指定」标记（AbstractLoop 恒置 true），
    // 若用它参与门控，禁用 + 残留 effort 时仍会写 thinkingLevel → 用户禁用无效。
    if (request.reasoning.enabled) {
        QJsonObject thinkingConfig{
            {QStringLiteral("includeThoughts"), request.reasoning.includeSummary}};
        if (request.reasoning.budgetTokens > 0)
            thinkingConfig.insert(QStringLiteral("thinkingBudget"),
                                  request.reasoning.budgetTokens);
        const QString level = toString(request.reasoning.effort);
        if (!level.isEmpty())
            thinkingConfig.insert(QStringLiteral("thinkingLevel"),
                                  level.toUpper());
        generationConfig.insert(QStringLiteral("thinkingConfig"),
                                thinkingConfig);
    }
    if (!generationConfig.isEmpty())
        body.insert(QStringLiteral("generationConfig"), generationConfig);

    if (!request.providerCachedContentId.isEmpty())
        body.insert(QStringLiteral("cachedContent"),
                    request.providerCachedContentId);

    if (!request.tools.isEmpty()) {
        QJsonArray declarations;
        for (const ProviderToolSpecification &tool : request.tools) {
            declarations.append(QJsonObject{
                {QStringLiteral("name"), tool.name},
                {QStringLiteral("description"), tool.description},
                {QStringLiteral("parameters"), tool.inputSchema}});
        }
        body.insert(QStringLiteral("tools"),
                    QJsonArray{QJsonObject{
                        {QStringLiteral("functionDeclarations"),
                         declarations}}});
    }
    if (request.toolChoice.mode != ProviderToolChoiceMode::ProviderDefault) {
        QString mode;
        switch (request.toolChoice.mode) {
        case ProviderToolChoiceMode::ProviderDefault:
            break;
        case ProviderToolChoiceMode::None:
            mode = QStringLiteral("NONE");
            break;
        case ProviderToolChoiceMode::Auto:
            mode = QStringLiteral("AUTO");
            break;
        case ProviderToolChoiceMode::Required:
        case ProviderToolChoiceMode::Named:
            mode = QStringLiteral("ANY");
            break;
        }
        QJsonObject functionCallingConfig{{QStringLiteral("mode"), mode}};
        if (request.toolChoice.mode == ProviderToolChoiceMode::Named)
            functionCallingConfig.insert(
                QStringLiteral("allowedFunctionNames"),
                QJsonArray{request.toolChoice.toolName});
        body.insert(QStringLiteral("toolConfig"),
                    QJsonObject{{QStringLiteral("functionCallingConfig"),
                                 functionCallingConfig}});
    }
    return buildStandardTransport(body, request.stream);
}

// ---- SSE 事件解析 ----

QList<ProviderEvent> GeminiProvider::parseProviderTransportPayload(const ProviderTransportPayload &payload)
{
    const QJsonObject &doc = payload.document;

    // 检查错误
    const QJsonObject errorObj = doc.value(QStringLiteral("error")).toObject();
    if (!errorObj.isEmpty()) {
        ProviderError error;
        error.code = QStringLiteral("gemini_error");
        error.message = errorObj.value(QStringLiteral("message")).toString();
        if (error.message.isEmpty()) {
            error.message = QStringLiteral("Gemini 返回了错误响应。");
        }
        error.providerRaw = doc;
        const ProviderRetry::Classification cls =
            ProviderRetry::classifyApiErrorObject(errorObj);
        error.retryable = cls.retryable;
        emitErrorOccurred(error);
        return {};
    }

    if (m_protocolFamily == ProviderProtocolFamily::GeminiGenerateContent)
        return parseGenerateContentPayload(doc);

    const QString type = doc.value(QStringLiteral("type")).toString();
    if (type.isEmpty() && doc.contains(QStringLiteral("status"))
        && doc.contains(QStringLiteral("steps"))) {
        return handleInteractionCompleted(
            QJsonObject{{QStringLiteral("interaction"), doc}});
    }

    static const QHash<QString, QList<ProviderEvent> (GeminiProvider::*)(const QJsonObject &)> handlers = {
        {QStringLiteral("interaction.created"), &GeminiProvider::handleInteractionCreated},
        {QStringLiteral("interaction.in_progress"), &GeminiProvider::handleInteractionInProgress},
        {QStringLiteral("step.start"), &GeminiProvider::handleStepStart},
        {QStringLiteral("step.delta"), &GeminiProvider::handleStepDelta},
        {QStringLiteral("step.stop"), &GeminiProvider::handleStepStop},
        {QStringLiteral("interaction.requires_action"), &GeminiProvider::handleInteractionRequiresAction},
        {QStringLiteral("interaction.completed"), &GeminiProvider::handleInteractionCompleted},
    };

    const auto it = handlers.constFind(type);
    if (it != handlers.constEnd()) {
        return (this->*(it.value()))(doc);
    }

    return {};
}

QList<ProviderEvent> GeminiProvider::parseGenerateContentPayload(
    const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const QJsonObject usageObject =
        payload.value(QStringLiteral("usageMetadata")).toObject();
    if (!usageObject.isEmpty()) {
        m_lastUsage.inputTokens =
            usageObject.value(QStringLiteral("promptTokenCount")).toInt();
        m_lastUsage.outputTokens =
            usageObject.value(QStringLiteral("candidatesTokenCount")).toInt();
        m_lastUsage.cacheReadTokens =
            usageObject.value(QStringLiteral("cachedContentTokenCount")).toInt();
        m_lastUsage.thoughtTokens =
            usageObject.value(QStringLiteral("thoughtsTokenCount")).toInt();
        events.append(ProviderEvent::usageUpdated(m_lastUsage));
    }

    const QJsonArray candidates = payload.value(QStringLiteral("candidates")).toArray();
    if (candidates.isEmpty())
        return events;
    const QJsonObject candidate = candidates.first().toObject();
    if (m_generateMessageId.isEmpty()) {
        m_generateMessageId =
            QStringLiteral("gemini-") + QUuid::createUuid().toString(
                QUuid::WithoutBraces);
    }

    auto ensureItem = [this](const ProviderItemKind kind) -> ProviderItem & {
        if (m_generateOutputItems.isEmpty()
            || m_generateOutputItems.last().kind != kind) {
            m_generateOutputItems.append(
                kind == ProviderItemKind::Reasoning
                    ? ProviderItem::makeReasoning({})
                    : ProviderItem::makeAssistantMessage({}));
        }
        return m_generateOutputItems.last();
    };

    const QJsonArray parts =
        candidate.value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("parts")).toArray();
    for (const QJsonValue &partValue : parts) {
        const QJsonObject part = partValue.toObject();
        const QString signature =
            part.value(QStringLiteral("thoughtSignature")).toString();
        if (part.contains(QStringLiteral("text"))) {
            const QString text = part.value(QStringLiteral("text")).toString();
            if (part.value(QStringLiteral("thought")).toBool()) {
                ProviderItem &item = ensureItem(ProviderItemKind::Reasoning);
                item.reasoningText.append(text);
                if (!signature.isEmpty()) {
                    item.reasoningSignature = signature;
                    item.reasoningMustReplay = true;
                }
                ProviderReasoningDelta delta;
                delta.base.messageId = m_generateMessageId;
                delta.text = text;
                emitReasoningDelta(delta);
            } else {
                ProviderItem &item = ensureItem(ProviderItemKind::AssistantMessage);
                if (item.parts.isEmpty()
                    || item.parts.last().kind != ProviderPartKind::Text) {
                    item.parts.append(ProviderMessagePart::makeText({}));
                }
                item.parts.last().text.append(text);
                if (!signature.isEmpty()) {
                    item.reasoningSignature = signature;
                    item.reasoningMustReplay = true;
                }
                ProviderTextDelta delta;
                delta.base.messageId = m_generateMessageId;
                delta.text = text;
                emitTextDelta(delta);
            }
            continue;
        }
        if (part.contains(QStringLiteral("inlineData"))
            || part.contains(QStringLiteral("fileData"))) {
            ProviderMessagePart messagePart = messagePartFromGenerateContent(part);
            ProviderItem &item = ensureItem(ProviderItemKind::AssistantMessage);
            item.parts.append(messagePart);
            if (messagePart.kind == ProviderPartKind::Image) {
                ProviderImageOutput image;
                image.base.messageId = m_generateMessageId;
                image.image = messagePart.image;
                events.append(ProviderEvent::fromImageOutput(image));
            } else if (messagePart.kind == ProviderPartKind::Audio) {
                ProviderAudioDelta audio;
                audio.base.messageId = m_generateMessageId;
                audio.audio = messagePart.audio;
                events.append(ProviderEvent::fromAudioDelta(audio));
            }
            continue;
        }
        const QJsonObject functionCall =
            part.value(QStringLiteral("functionCall")).toObject();
        if (!functionCall.isEmpty()) {
            const QString callId =
                functionCall.value(QStringLiteral("id")).toString(
                    QUuid::createUuid().toString(QUuid::WithoutBraces));
            const QString name = functionCall.value(QStringLiteral("name")).toString();
            const QJsonObject args =
                functionCall.value(QStringLiteral("args")).toObject();
            ProviderItem item = ProviderItem::makeFunctionCall(
                callId, name, args,
                QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)));
            item.reasoningSignature = signature;
            item.reasoningMustReplay = !signature.isEmpty();
            m_generateOutputItems.append(item);
            ProviderToolCallStart start;
            start.base.messageId = m_generateMessageId;
            start.toolCallId = callId;
            start.toolName = name;
            events.append(ProviderEvent::toolCallStarted(start));
            ProviderToolCallEnd end;
            end.base.messageId = m_generateMessageId;
            end.toolCallId = callId;
            end.toolName = name;
            end.arguments = args;
            end.rawArguments = item.rawArguments;
            events.append(ProviderEvent::toolCallCompleted(end));
            continue;
        }
        const QJsonObject executableCode =
            part.value(QStringLiteral("executableCode")).toObject();
        if (!executableCode.isEmpty()) {
            const QString callId =
                QStringLiteral("code-") + QUuid::createUuid().toString(
                    QUuid::WithoutBraces);
            ProviderItem item = ProviderItem::makeServerToolCall(
                callId, ProviderServerToolName::CodeInterpreter, {},
                executableCode.value(QStringLiteral("code")).toString());
            item.details = QJsonObject{
                {QStringLiteral("code"),
                 executableCode.value(QStringLiteral("code")).toString()},
                {QStringLiteral("language"),
                 executableCode.value(QStringLiteral("language")).toString()}};
            m_generateOutputItems.append(item);
            continue;
        }
        const QJsonObject executionResult =
            part.value(QStringLiteral("codeExecutionResult")).toObject();
        if (!executionResult.isEmpty()) {
            QString callId;
            for (qsizetype i = m_generateOutputItems.size() - 1; i >= 0; --i) {
                if (m_generateOutputItems[i].kind
                    == ProviderItemKind::ServerToolCall) {
                    callId = m_generateOutputItems[i].callId;
                    break;
                }
            }
            const QString outcome =
                executionResult.value(QStringLiteral("outcome")).toString();
            m_generateOutputItems.append(ProviderItem::makeServerToolResult(
                callId, ProviderServerToolName::CodeInterpreter,
                executionResult.value(QStringLiteral("output")).toString(), {},
                outcome.contains(QStringLiteral("FAILED"), Qt::CaseInsensitive)
                    || outcome.contains(QStringLiteral("ERROR"),
                                        Qt::CaseInsensitive)));
            continue;
        }
        qWarning().noquote()
            << QStringLiteral("Gemini generateContent adapter 丢弃未知 part：%1")
                   .arg(QString::fromUtf8(
                       QJsonDocument(part).toJson(QJsonDocument::Compact)));
    }

    const QJsonObject grounding =
        candidate.value(QStringLiteral("groundingMetadata")).toObject();
    if (!grounding.isEmpty()) {
        QList<ProviderCitation> citations;
        for (const QJsonValue &chunkValue :
             grounding.value(QStringLiteral("groundingChunks")).toArray()) {
            const QJsonObject web =
                chunkValue.toObject().value(QStringLiteral("web")).toObject();
            if (!web.isEmpty())
                citations.append(ProviderCitation{
                    web.value(QStringLiteral("uri")).toString(),
                    web.value(QStringLiteral("title")).toString(), {}, -1, -1});
        }
        if (!citations.isEmpty()) {
            ProviderItem &item = ensureItem(ProviderItemKind::AssistantMessage);
            if (item.parts.isEmpty())
                item.parts.append(ProviderMessagePart::makeText({}));
            item.parts.last().citations.append(citations);
        }
    }

    const QString finishReason =
        candidate.value(QStringLiteral("finishReason")).toString();
    if (!finishReason.isEmpty()) {
        ProviderMessageEnd end;
        end.messageId = m_generateMessageId;
        if (finishReason == QStringLiteral("STOP"))
            end.stopReason = StopReason::EndTurn;
        else if (finishReason == QStringLiteral("MAX_TOKENS"))
            end.stopReason = StopReason::MaxTokens;
        else if (finishReason == QStringLiteral("MALFORMED_FUNCTION_CALL"))
            end.stopReason = StopReason::ToolUse;
        else
            end.stopReason = StopReason::Incomplete;
        end.finalUsage = m_lastUsage;
        end.outputItems = m_generateOutputItems;
        events.append(ProviderEvent::messageCompleted(end));
    }
    return events;
}

void GeminiProvider::resetProviderTurnState()
{
    // m_interactionId 不清除——需要跨 turn 保持以支持 tool result 链式调用
    // 由 handleInteractionCreated 在新交互开始时更新
    m_stepTypes.clear();
    m_functionCallIds.clear();
    m_functionCallNames.clear();
    m_argsBuffer.clear();
    m_requiresAction = false;
    m_lastUsage = {};
    m_generateOutputItems.clear();
    m_generateMessageId.clear();
    m_activeReasoningPartIndex = -1;
}

// ---- 事件处理器 ----

QList<ProviderEvent> GeminiProvider::handleInteractionCreated(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const QJsonObject interaction = payload.value(QStringLiteral("interaction")).toObject();
    m_interactionId = interaction.value(QStringLiteral("id")).toString();

    ProviderMessageStart start;
    start.messageId = m_interactionId;
    events.append(ProviderEvent::messageStarted(start));

    return events;
}

QList<ProviderEvent> GeminiProvider::handleInteractionInProgress(const QJsonObject &payload)
{
    Q_UNUSED(payload);
    return {};
}

QList<ProviderEvent> GeminiProvider::handleStepStart(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const int index = payload.value(QStringLiteral("index")).toInt(-1);
    const QJsonObject step = payload.value(QStringLiteral("step")).toObject();
    const QString stepType = step.value(QStringLiteral("type")).toString();

    m_stepTypes.insert(index, stepType);

    if (stepType == QStringLiteral("thought")) {
        ensureReasoningPartStarted(m_interactionId, index);
        const QString signature = step.value(QStringLiteral("signature")).toString();
        if (!signature.isEmpty())
            turnState().fallbackOutputItems.append(
                ProviderItem::makeReasoning({}, signature, false, true));
    } else if (stepType == QStringLiteral("function_call")
               || !geminiServerToolName(stepType).isEmpty()
                      && stepType.endsWith(QStringLiteral("_call"))) {
        const QString callId = step.value(QStringLiteral("id")).toString();
        const QString serverName = geminiServerToolName(stepType);
        const QString name = serverName.isEmpty()
            ? step.value(QStringLiteral("name")).toString() : serverName;
        m_functionCallIds.insert(index, callId);
        m_functionCallNames.insert(index, name);
        const QJsonObject initialArguments =
            step.value(QStringLiteral("arguments")).toObject();
        m_argsBuffer.insert(callId, initialArguments.isEmpty()
            ? QString() : compactJson(initialArguments));

        ensureToolPartStarted(callId, m_interactionId, index);

        ProviderToolCallStart call;
        call.base.messageId = m_interactionId;
        call.base.partIndex = index;
        call.toolCallId = callId;
        call.toolName = name;
        call.isServerTool = !serverName.isEmpty();
        events.append(ProviderEvent::toolCallStarted(call));
    } else if (stepType == QStringLiteral("model_output")) {
        ensureTextPartStarted(m_interactionId);
    } else if (stepType.endsWith(QStringLiteral("_result"))
               && !geminiServerToolName(stepType).isEmpty()) {
        ProviderItem result = ProviderItem::makeServerToolResult(
            step.value(QStringLiteral("call_id")).toString(),
            geminiServerToolName(stepType),
            compactJson(step.value(QStringLiteral("result"))), {},
            step.value(QStringLiteral("is_error")).toBool(false));
        result.reasoningSignature = step.value(QStringLiteral("signature")).toString();
        result.reasoningMustReplay = !result.reasoningSignature.isEmpty();
        turnState().fallbackOutputItems.append(result);
    }

    return events;
}

QList<ProviderEvent> GeminiProvider::handleStepDelta(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const int index = payload.value(QStringLiteral("index")).toInt(-1);
    const QJsonObject delta = payload.value(QStringLiteral("delta")).toObject();
    const QString deltaType = delta.value(QStringLiteral("type")).toString();
    const QString stepType = m_stepTypes.value(index);

    if (stepType == QStringLiteral("thought")
        && (deltaType == QStringLiteral("thought")
            || deltaType == QStringLiteral("text"))) {
        ProviderReasoningDelta reasoningDelta;
        reasoningDelta.base.messageId = m_interactionId;
        reasoningDelta.base.partIndex = index;
        reasoningDelta.text = delta.value(QStringLiteral("text")).toString();
        emitReasoningDelta(reasoningDelta);
    } else if (deltaType == QStringLiteral("arguments")
               || deltaType == QStringLiteral("arguments_delta")) {
        QString partialArgs = delta.value(QStringLiteral("partial_arguments")).toString();
        if (partialArgs.isEmpty())
            partialArgs = delta.value(QStringLiteral("arguments")).toString();
        const QString callId = m_functionCallIds.value(index);
        if (!callId.isEmpty()) {
            m_argsBuffer[callId].append(partialArgs);
        }
    } else if (deltaType == QStringLiteral("thought_signature")) {
        for (qsizetype i = turnState().fallbackOutputItems.size() - 1; i >= 0; --i) {
            ProviderItem &item = turnState().fallbackOutputItems[i];
            if (item.kind == ProviderItemKind::Reasoning) {
                item.reasoningSignature.append(delta.value(QStringLiteral("signature")).toString());
                item.reasoningMustReplay = true;
                break;
            }
        }
    } else if (deltaType == QStringLiteral("image")) {
        ProviderImageOutput image;
        image.base.messageId = m_interactionId;
        image.base.partIndex = index;
        image.image.uri = delta.value(QStringLiteral("uri")).toString();
        image.image.data = QByteArray::fromBase64(
            delta.value(QStringLiteral("data")).toString().toLatin1());
        image.image.mimeType = delta.value(QStringLiteral("mime_type")).toString();
        events.append(ProviderEvent::fromImageOutput(image));
    } else if (deltaType == QStringLiteral("audio")) {
        ProviderAudioDelta audio;
        audio.base.messageId = m_interactionId;
        audio.base.partIndex = index;
        audio.audio.uri = delta.value(QStringLiteral("uri")).toString();
        audio.audio.data = QByteArray::fromBase64(
            delta.value(QStringLiteral("data")).toString().toLatin1());
        audio.audio.mimeType = delta.value(QStringLiteral("mime_type")).toString();
        audio.audio.sampleRate = delta.value(QStringLiteral("sample_rate")).toInt();
        events.append(ProviderEvent::fromAudioDelta(audio));
    } else if (deltaType == QStringLiteral("transcript")) {
        ProviderTranscriptDelta transcript;
        transcript.base.messageId = m_interactionId;
        transcript.base.partIndex = index;
        transcript.text = delta.value(QStringLiteral("text")).toString();
        events.append(ProviderEvent::fromTranscriptDelta(transcript));
    } else if (deltaType == QStringLiteral("text")) {
        ProviderTextDelta textDelta;
        textDelta.base.messageId = m_interactionId;
        textDelta.base.partIndex = index;
        textDelta.text = delta.value(QStringLiteral("text")).toString();
        emitTextDelta(textDelta);
    }

    return events;
}

QList<ProviderEvent> GeminiProvider::handleStepStop(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const int index = payload.value(QStringLiteral("index")).toInt(-1);
    const QString stepType = m_stepTypes.value(index);

    if (stepType == QStringLiteral("thought")) {
        completeReasoningPartIfOpen(m_interactionId);
    } else if (stepType == QStringLiteral("function_call")
               || !geminiServerToolName(stepType).isEmpty()
                      && stepType.endsWith(QStringLiteral("_call"))) {
        const QString callId = m_functionCallIds.value(index);
        const QString name = m_functionCallNames.value(index);
        const QString rawArgs = m_argsBuffer.value(callId);

        completeToolPartIfOpen(callId, m_interactionId);

        ProviderToolCallEnd callEnd;
        callEnd.base.messageId = m_interactionId;
        callEnd.base.partIndex = index;
        callEnd.toolCallId = callId;
        callEnd.toolName = name;
        callEnd.isServerTool = stepType != QStringLiteral("function_call");
        callEnd.rawArguments = rawArgs;

        const QJsonDocument argsDoc = QJsonDocument::fromJson(rawArgs.toUtf8());
        if (argsDoc.isObject()) {
            callEnd.arguments = argsDoc.object();
        } else if (!rawArgs.isEmpty()) {
            callEnd.parseFailed = true;
        }

        events.append(ProviderEvent::toolCallCompleted(callEnd));
    } else if (stepType == QStringLiteral("model_output")) {
        completeTextPartIfOpen(m_interactionId);
    }

    return events;
}

QList<ProviderEvent> GeminiProvider::handleInteractionRequiresAction(const QJsonObject &payload)
{
    Q_UNUSED(payload);
    m_requiresAction = true;
    return {};
}

QList<ProviderEvent> GeminiProvider::handleInteractionCompleted(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const QJsonObject interaction = payload.value(QStringLiteral("interaction")).toObject();
    const QJsonObject usage = interaction.value(QStringLiteral("usage")).toObject();
    if (m_interactionId.isEmpty())
        m_interactionId = interaction.value(QStringLiteral("id")).toString();

    if (!usage.isEmpty()) {
        m_lastUsage = usageFromJson(usage);
        events.append(ProviderEvent::usageUpdated(m_lastUsage));
    }

    ProviderResponseMetadata metadata;
    metadata.providerResponseId = m_interactionId;
    events.append(ProviderEvent::responseMetadataUpdated(metadata));

    ProviderMessageEnd messageEnd;
    messageEnd.messageId = m_interactionId;
    const QString status = interaction.value(QStringLiteral("status")).toString();
    messageEnd.stopReason = (m_requiresAction || status == QStringLiteral("requires_action"))
        ? StopReason::ToolUse
        : status == QStringLiteral("incomplete") ? StopReason::Incomplete
                                                  : StopReason::EndTurn;
    messageEnd.finalUsage = m_lastUsage;
    messageEnd.outputItems =
        itemsFromInteractionSteps(interaction.value(QStringLiteral("steps")).toArray());
    events.append(ProviderEvent::messageCompleted(messageEnd));

    return events;
}

// ---- 辅助 ----

ProviderUsage GeminiProvider::usageFromJson(const QJsonObject &usageObject) const
{
    ProviderUsage usage;
    usage.inputTokens = usageObject.value(QStringLiteral("total_input_tokens")).toInt(
        usageObject.value(QStringLiteral("prompt_tokens")).toInt());
    usage.outputTokens = usageObject.value(QStringLiteral("total_output_tokens")).toInt(
        usageObject.value(QStringLiteral("completion_tokens")).toInt());
    usage.cacheReadTokens = usageObject.value(QStringLiteral("total_cached_tokens")).toInt();
    usage.thoughtTokens = usageObject.value(QStringLiteral("total_thought_tokens")).toInt();
    return usage;
}

QString GeminiProvider::currentMessageId() const
{
    return m_interactionId;
}
