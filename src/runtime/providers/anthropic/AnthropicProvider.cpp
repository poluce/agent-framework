#include "AnthropicProvider.h"

#include "providers/transport/HttpSseChannel.h"
#include "providers/core/ProviderRetryPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QUrl>

namespace {

QJsonObject buildContentBlock(const ProviderMessagePart &part)
{
    QJsonObject block;
    if (part.kind == ProviderPartKind::Image) {
        block.insert(QStringLiteral("type"), QStringLiteral("image"));
        QJsonObject source;
        if (part.image.hasUri()) {
            source.insert(QStringLiteral("type"), QStringLiteral("url"));
            source.insert(QStringLiteral("url"), part.image.uri);
        } else if (part.image.hasInlineData()) {
            source.insert(QStringLiteral("type"), QStringLiteral("base64"));
            source.insert(QStringLiteral("media_type"),
                          part.image.mimeType.isEmpty() ? QStringLiteral("image/png") : part.image.mimeType);
            source.insert(QStringLiteral("data"), QString::fromLatin1(part.image.data.toBase64()));
        }
        block.insert(QStringLiteral("source"), source);
    } else if (part.kind == ProviderPartKind::Document) {
        block.insert(QStringLiteral("type"), QStringLiteral("document"));
        QJsonObject source;
        if (part.document.hasUri()) {
            source.insert(QStringLiteral("type"), QStringLiteral("url"));
            source.insert(QStringLiteral("url"), part.document.uri);
        } else if (part.document.hasInlineData()) {
            source.insert(QStringLiteral("type"), QStringLiteral("base64"));
            source.insert(QStringLiteral("media_type"), part.document.mimeType);
            source.insert(QStringLiteral("data"),
                          QString::fromLatin1(part.document.data.toBase64()));
        }
        block.insert(QStringLiteral("source"), source);
        if (!part.document.title.isEmpty())
            block.insert(QStringLiteral("title"), part.document.title);
        if (!part.document.context.isEmpty())
            block.insert(QStringLiteral("context"), part.document.context);
    } else {
        block.insert(QStringLiteral("type"), QStringLiteral("text"));
        block.insert(QStringLiteral("text"), part.text);
    }
    if (part.cachePolicy == ProviderCachePolicy::Ephemeral)
        block.insert(QStringLiteral("cache_control"),
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("ephemeral")}});
    return block;
}

QJsonObject buildToolCallFromItem(const ProviderItem &item)
{
    QJsonObject block;
    block.insert(QStringLiteral("type"), QStringLiteral("tool_use"));
    block.insert(QStringLiteral("id"), item.callId);
    block.insert(QStringLiteral("name"), item.name);
    block.insert(QStringLiteral("input"), item.arguments);
    if (item.callerKind != ProviderCallerKind::Unset)
        block.insert(QStringLiteral("caller"),
                     QJsonObject{{QStringLiteral("type"), toString(item.callerKind)},
                                 {QStringLiteral("id"), item.callerId}});
    return block;
}

QJsonObject buildToolResultBlock(const ProviderItem &item)
{
    QJsonObject block;
    block.insert(QStringLiteral("type"), QStringLiteral("tool_result"));
    block.insert(QStringLiteral("tool_use_id"), item.callId);
    if (item.isError) {
        block.insert(QStringLiteral("is_error"), true);
    }
    // content 可以是字符串或数组
    block.insert(QStringLiteral("content"), item.output);
    return block;
}

QString anthropicServerToolName(const QString &wireName)
{
    if (wireName.contains(QStringLiteral("web_search")))
        return ProviderServerToolName::WebSearch;
    if (wireName.contains(QStringLiteral("web_fetch")))
        return ProviderServerToolName::WebFetch;
    if (wireName.contains(QStringLiteral("code_execution")))
        return ProviderServerToolName::CodeInterpreter;
    if (wireName.contains(QStringLiteral("tool_search")))
        return ProviderServerToolName::ToolSearch;
    if (wireName.contains(QStringLiteral("advisor")))
        return ProviderServerToolName::Advisor;
    return wireName;
}

QString anthropicServerResultType(const QString &name)
{
    if (name == ProviderServerToolName::WebSearch)
        return QStringLiteral("web_search_tool_result");
    if (name == ProviderServerToolName::WebFetch)
        return QStringLiteral("web_fetch_tool_result");
    if (name == ProviderServerToolName::CodeInterpreter)
        return QStringLiteral("code_execution_tool_result");
    if (name == ProviderServerToolName::ToolSearch)
        return QStringLiteral("tool_search_tool_result");
    if (name == ProviderServerToolName::Advisor)
        return QStringLiteral("advisor_tool_result");
    return name + QStringLiteral("_tool_result");
}

} // namespace

// ---- AnthropicProvider ----

AnthropicProvider::AnthropicProvider(QObject *parent)
    : AbstractProvider(QStringLiteral("anthropic"), parent)
{
    m_channel->setAuthMode(HttpSseChannel::AuthMode::ApiKey);
    m_channel->setVersionHeader("anthropic-version", "2023-06-01");
}

AnthropicProvider::~AnthropicProvider() = default;

// ---- 认证与 URL ----

QUrl AnthropicProvider::buildModelsUrl(const QString &baseUrl) const
{
    if (baseUrl.endsWith(QStringLiteral("/models")) || baseUrl.contains(QStringLiteral("/models?"))) {
        return QUrl(baseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/v1"))) {
        return QUrl(baseUrl + QStringLiteral("/models"));
    }
    if (baseUrl.endsWith(QLatin1Char('/'))) {
        return QUrl(baseUrl + QStringLiteral("v1/models"));
    }
    return QUrl(baseUrl + QStringLiteral("/v1/models"));
}

QList<ModelCapabilities> AnthropicProvider::parseModelsPayload(const QByteArray &body,
                                                                QString *errorMessage) const
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Anthropic 模型列表不是有效 JSON。");
        }
        return {};
    }

    QList<ModelCapabilities> models;
    const QJsonArray data = document.object().value(QStringLiteral("data")).toArray();
    for (const QJsonValue &value : data) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        ModelCapabilities caps;
        caps.modelId = obj.value(QStringLiteral("id")).toString().trimmed();
        if (caps.modelId.isEmpty()) {
            continue;
        }
        caps.enable(ProviderCapability::TextInput)
            .enable(ProviderCapability::ImageInput)
            .enable(ProviderCapability::DocumentInput)
            .enable(ProviderCapability::TextOutput)
            .enable(ProviderCapability::ToolCalling)
            .enable(ProviderCapability::ToolChoice)
            .enable(ProviderCapability::MaxOutputTokens)
            .set(ProviderCapability::Reasoning,
                 caps.modelId.contains(QStringLiteral("claude"), Qt::CaseInsensitive))
            .enable(ProviderCapability::PromptCache)
            .enable(ProviderCapability::Citations)
            .enable(ProviderCapability::ServerTools);
        caps.supportedServerTools = {
            ProviderServerToolName::WebSearch,
            ProviderServerToolName::WebFetch,
            ProviderServerToolName::CodeInterpreter,
            ProviderServerToolName::ToolSearch,
            ProviderServerToolName::Advisor};
        models.append(caps);
    }

    if (models.isEmpty() && errorMessage) {
        *errorMessage = QStringLiteral("Anthropic 模型列表为空。");
    }
    return models;
}

bool AnthropicProvider::startProviderTransportRequest(const ProviderTransportRequest &request,
                                                       ProviderError *error)
{
    QString base = m_auth.baseUrl.trimmed();
    QUrl url;
    if (base.endsWith(QStringLiteral("/messages")) || base.contains(QStringLiteral("/messages?"))) {
        url = QUrl(base);
    } else if (base.endsWith(QStringLiteral("/v1"))) {
        url = QUrl(base + QStringLiteral("/messages"));
    } else if (base.endsWith(QLatin1Char('/'))) {
        url = QUrl(base + QStringLiteral("v1/messages"));
    } else {
        url = QUrl(base + QStringLiteral("/v1/messages"));
    }

    const HttpSseChannel::StartResult result = m_channel->start(url, m_auth.apiKey, request);
    if (!result.accepted) {
        if (error) {
            *error = result.error;
        }
        return false;
    }
    return true;
}

// ---- 请求验证 ----

ProviderError AnthropicProvider::validateProviderRequest(const ProviderRequest &request) const
{
    if (request.protocolFamily != ProviderProtocolFamily::Auto
        && request.protocolFamily != ProviderProtocolFamily::AnthropicMessages) {
        return ProviderError{QStringLiteral("protocol_family_mismatch"),
                             QStringLiteral("Anthropic 适配器不能处理指定的协议族。")};
    }
    if (m_auth.modelName.trimmed().isEmpty()) {
        return ProviderError{QStringLiteral("model_required"),
                             QStringLiteral("Anthropic 需要指定模型名称。")};
    }
    if (request.maxOutputTokens <= 0) {
        return ProviderError{QStringLiteral("max_tokens_required"),
                             QStringLiteral("Anthropic 要求 max_tokens > 0。")};
    }
    if (request.desiredOutput.imageEnabled || request.desiredOutput.audioEnabled
        || request.audio.isExplicit()) {
        return ProviderError{QStringLiteral("output_modality_not_supported"),
                             QStringLiteral("Anthropic Messages 当前仅支持文本输出。")};
    }
    if (request.responseFormat.isExplicit() || request.requestLogprobs
        || request.backgroundExecution != ProviderTriState::Unset
        || request.storeServerState != ProviderTriState::Unset) {
        return ProviderError{QStringLiteral("request_option_not_supported"),
                             QStringLiteral("Anthropic Messages 不支持结构化输出、logprobs、background 或 store。")};
    }
    if (request.sampling.seed >= 0 || request.sampling.penaltiesRequested) {
        return ProviderError{QStringLiteral("sampling_option_not_supported"),
                             QStringLiteral("Anthropic Messages 不支持 seed 或 penalties。")};
    }
    if (!request.responseInclude.isEmpty()
        || !request.providerConversationId.isEmpty()
        || !request.providerCachedContentId.isEmpty()
        || !request.mediaResolution.isEmpty()
        || !request.metadata.isEmpty()) {
        return ProviderError{
            QStringLiteral("request_option_not_supported"),
            QStringLiteral("Anthropic Messages 不支持 Responses/Gemini 专用请求选项。")};
    }
    if (request.hasAudioInput() || request.hasVideoInput()) {
        return ProviderError{QStringLiteral("media_input_not_supported"),
                             QStringLiteral("Anthropic Messages 不支持音频或视频输入。")};
    }
    for (const ProviderItem &item : request.items) {
        if (item.kind == ProviderItemKind::Program
            || item.kind == ProviderItemKind::ProgramOutput
            || item.kind == ProviderItemKind::ApprovalRequest
            || item.kind == ProviderItemKind::ApprovalResponse
            || item.kind == ProviderItemKind::Compaction) {
            return ProviderError{
                QStringLiteral("unsupported_provider_item"),
                QStringLiteral("Anthropic Messages 不支持该 ProviderItem kind：%1")
                    .arg(static_cast<int>(item.kind))};
        }
    }
    return {};
}

// ---- 请求构造 ----

QJsonArray AnthropicProvider::buildMessages(const ProviderRequest &request) const
{
    QJsonArray messages;

    for (const ProviderItem &item : request.items) {
        QString role;
        QJsonArray blocks;

        switch (item.kind) {
        case ProviderItemKind::UserMessage:
        case ProviderItemKind::AssistantMessage: {
            role = item.kind == ProviderItemKind::UserMessage ? QStringLiteral("user") : QStringLiteral("assistant");
            for (const ProviderMessagePart &part : item.parts) {
                blocks.append(buildContentBlock(part));
            }
            break;
        }
        case ProviderItemKind::FunctionCall: {
            role = QStringLiteral("assistant");
            blocks.append(buildToolCallFromItem(item));
            break;
        }
        case ProviderItemKind::FunctionCallOutput: {
            role = QStringLiteral("user");
            blocks.append(buildToolResultBlock(item));
            break;
        }
        case ProviderItemKind::Reasoning: {
            role = QStringLiteral("assistant");
            QJsonObject block;
            block.insert(QStringLiteral("type"),
                         item.reasoningRedacted ? QStringLiteral("redacted_thinking")
                                                : QStringLiteral("thinking"));
            if (!item.reasoningText.isEmpty())
                block.insert(QStringLiteral("thinking"), item.reasoningText);
            if (!item.reasoningSignature.isEmpty())
                block.insert(QStringLiteral("signature"), item.reasoningSignature);
            blocks.append(block);
            break;
        }
        case ProviderItemKind::ServerToolCall: {
            role = QStringLiteral("assistant");
            QJsonObject block{
                {QStringLiteral("type"), QStringLiteral("server_tool_use")},
                {QStringLiteral("id"), item.callId},
                {QStringLiteral("name"), item.name},
                {QStringLiteral("input"), item.arguments}};
            if (!item.callerId.isEmpty()) {
                block.insert(QStringLiteral("caller"),
                             QJsonObject{{QStringLiteral("type"), toString(item.callerKind)},
                                         {QStringLiteral("id"), item.callerId}});
            }
            blocks.append(block);
            break;
        }
        case ProviderItemKind::ServerToolResult: {
            role = QStringLiteral("user");
            QJsonObject block{
                {QStringLiteral("type"), anthropicServerResultType(item.name)},
                {QStringLiteral("tool_use_id"), item.callId},
                {QStringLiteral("content"), item.output}};
            if (item.isError)
                block.insert(QStringLiteral("is_error"), true);
            blocks.append(block);
            break;
        }
        case ProviderItemKind::Program:
        case ProviderItemKind::ProgramOutput:
        case ProviderItemKind::ApprovalRequest:
        case ProviderItemKind::ApprovalResponse:
        case ProviderItemKind::Compaction:
            break;
        }

        if (blocks.isEmpty() || role.isEmpty()) {
            continue;
        }

        if (!messages.isEmpty()) {
            QJsonObject lastMsg = messages.last().toObject();
            if (lastMsg.value(QStringLiteral("role")).toString() == role) {
                QJsonArray content = lastMsg.value(QStringLiteral("content")).toArray();
                for (const QJsonValue &b : blocks) {
                    content.append(b);
                }
                lastMsg.insert(QStringLiteral("content"), content);
                messages[messages.size() - 1] = lastMsg;
                continue;
            }
        }

        messages.append(QJsonObject{
            {QStringLiteral("role"), role},
            {QStringLiteral("content"), blocks}
        });
    }

    return messages;
}

QJsonObject AnthropicProvider::buildToolDefinition(const ProviderToolSpecification &tool) const
{
    QJsonObject def;
    def.insert(QStringLiteral("name"), tool.name);
    def.insert(QStringLiteral("description"), tool.description);
    def.insert(QStringLiteral("input_schema"), tool.inputSchema);
    if (!tool.outputSchema.isEmpty())
        def.insert(QStringLiteral("output_schema"), tool.outputSchema);
    if (tool.strictSchema)
        def.insert(QStringLiteral("strict"), true);
    if (tool.deferLoading)
        def.insert(QStringLiteral("defer_loading"), true);
    if (!tool.allowedCallers.isEmpty()) {
        QJsonArray callers;
        for (const ProviderCallerKind caller : tool.allowedCallers)
            callers.append(toString(caller));
        def.insert(QStringLiteral("allowed_callers"), callers);
    }
    return def;
}

QJsonValue AnthropicProvider::buildToolChoice(const ProviderToolChoice &toolChoice) const
{
    auto withParallelPolicy = [&toolChoice](QJsonObject choice) {
        if (toolChoice.allowParallel != ProviderTriState::Unset)
            choice.insert(QStringLiteral("disable_parallel_tool_use"),
                          toolChoice.allowParallel == ProviderTriState::No);
        return choice;
    };
    switch (toolChoice.mode) {
    case ProviderToolChoiceMode::None:
        return QJsonValue(QJsonValue::Undefined); // Anthropic 无 "none"，不传即可
    case ProviderToolChoiceMode::ProviderDefault:
        if (toolChoice.allowParallel == ProviderTriState::Unset)
            return QJsonValue(QJsonValue::Undefined);
        return withParallelPolicy(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("auto")}});
    case ProviderToolChoiceMode::Auto: {
        QJsonObject choice;
        choice.insert(QStringLiteral("type"), QStringLiteral("auto"));
        return withParallelPolicy(choice);
    }
    case ProviderToolChoiceMode::Required: {
        QJsonObject choice;
        choice.insert(QStringLiteral("type"), QStringLiteral("any"));
        return withParallelPolicy(choice);
    }
    case ProviderToolChoiceMode::Named: {
        QJsonObject choice;
        choice.insert(QStringLiteral("type"), QStringLiteral("tool"));
        choice.insert(QStringLiteral("name"), toolChoice.toolName);
        return withParallelPolicy(choice);
    }
    }
    return QJsonValue(QJsonValue::Undefined);
}

ProviderTransportRequest AnthropicProvider::buildProviderTransportRequest(const ProviderRequest &request) const
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), m_auth.modelName);
    body.insert(QStringLiteral("max_tokens"), request.maxOutputTokens);
    body.insert(QStringLiteral("stream"), request.stream);
    if (!m_containerId.isEmpty())
        body.insert(QStringLiteral("container"), m_containerId);

    // system prompt（顶层字段）
    const QString system = request.systemPrompt.trimmed();
    if (!system.isEmpty()) {
        body.insert(QStringLiteral("system"), system);
    }

    // temperature
    if (request.temperature >= 0.0) {
        body.insert(QStringLiteral("temperature"), request.temperature);
    }
    if (request.sampling.topP >= 0.0)
        body.insert(QStringLiteral("top_p"), request.sampling.topP);
    if (request.sampling.topK >= 0)
        body.insert(QStringLiteral("top_k"), request.sampling.topK);
    if (!request.sampling.stop.isEmpty())
        body.insert(QStringLiteral("stop_sequences"),
                    QJsonArray::fromStringList(request.sampling.stop));

    // thinking
    if (request.reasoning.enabled) {
        QJsonObject thinking;
        thinking.insert(QStringLiteral("type"), QStringLiteral("enabled"));
        // budget_tokens 需要 < max_tokens，取一半作为默认
        const int budget = request.reasoning.budgetTokens > 0
            ? request.reasoning.budgetTokens
            : qMax(1024, request.maxOutputTokens / 2);
        thinking.insert(QStringLiteral("budget_tokens"), budget);
        body.insert(QStringLiteral("thinking"), thinking);
        // thinking 启用时 temperature 必须为 1
        body.insert(QStringLiteral("temperature"), 1);
    }

    // messages
    const QJsonArray messages = buildMessages(request);
    body.insert(QStringLiteral("messages"), messages);

    // tools
    if (!request.tools.isEmpty()
        && request.toolChoice.mode != ProviderToolChoiceMode::None) {
        QJsonArray tools;
        for (const ProviderToolSpecification &tool : request.tools) {
            tools.append(buildToolDefinition(tool));
        }
        body.insert(QStringLiteral("tools"), tools);
    }

    // tool_choice
    const QJsonValue toolChoice = buildToolChoice(request.toolChoice);
    if (!toolChoice.isUndefined()) {
        body.insert(QStringLiteral("tool_choice"), toolChoice);
    }

    return buildStandardTransport(body, request.stream);
}

// ---- SSE 事件解析 ----

QList<ProviderEvent> AnthropicProvider::parseProviderTransportPayload(const ProviderTransportPayload &payload)
{
    const QJsonObject &doc = payload.document;
    const QString type = doc.value(QStringLiteral("type")).toString();

    // SSE 流中的错误事件（HTTP 200 但 type: "error"）
    if (type == QStringLiteral("error")) {
        ProviderError error;
        error.code = QStringLiteral("anthropic_error");
        const QJsonValue errorValue = doc.value(QStringLiteral("error"));
        const QJsonObject errorObj = errorValue.toObject();
        error.message = errorObj.value(QStringLiteral("message")).toString();
        if (error.message.isEmpty()) {
            error.message = doc.value(QStringLiteral("message")).toString();
        }
        if (error.message.isEmpty()) {
            error.message = QStringLiteral("Anthropic 返回了错误响应。");
        }
        error.providerRaw = doc;
        const ProviderRetry::Classification cls =
            ProviderRetry::classifyApiErrorValue(errorValue);
        error.retryable = cls.retryable;
        emitErrorOccurred(error);
        return {};
    }

    // 非流式 Messages API 直接返回 type=message 的完整对象。
    if (type == QStringLiteral("message")) {
        QList<ProviderEvent> events;
        m_currentMessageId = doc.value(QStringLiteral("id")).toString();
        const QJsonValue container = doc.value(QStringLiteral("container"));
        m_containerId = container.isObject()
            ? container.toObject().value(QStringLiteral("id")).toString()
            : container.toString();
        m_lastUsage = usageFromJson(doc.value(QStringLiteral("usage")).toObject());

        ProviderMessageStart start;
        start.messageId = m_currentMessageId;
        start.initialUsage = m_lastUsage;
        events.append(ProviderEvent::messageStarted(start));

        QList<ProviderItem> outputItems;
        const QJsonArray content = doc.value(QStringLiteral("content")).toArray();
        for (int index = 0; index < content.size(); ++index) {
            const QJsonObject block = content.at(index).toObject();
            const QString blockType = block.value(QStringLiteral("type")).toString();
            if (blockType == QStringLiteral("text")) {
                ProviderMessagePart part = ProviderMessagePart::makeText(
                    block.value(QStringLiteral("text")).toString());
                for (const QJsonValue &citationValue :
                     block.value(QStringLiteral("citations")).toArray()) {
                    const QJsonObject citation = citationValue.toObject();
                    part.citations.append(ProviderCitation{
                        citation.value(QStringLiteral("url")).toString(),
                        citation.value(QStringLiteral("title")).toString(
                            citation.value(QStringLiteral("document_title")).toString()),
                        citation.value(QStringLiteral("cited_text")).toString(),
                        citation.value(QStringLiteral("start_char_index")).toInt(-1),
                        citation.value(QStringLiteral("end_char_index")).toInt(-1)});
                }
                outputItems.append(
                    ProviderItem::makeAssistantMessage({part}));
                if (!part.text.isEmpty()) {
                    ProviderTextDelta delta;
                    delta.base.messageId = m_currentMessageId;
                    delta.base.partIndex = index;
                    delta.text = part.text;
                    events.append(ProviderEvent::fromTextDelta(delta));
                }
            } else if (blockType == QStringLiteral("thinking")
                       || blockType == QStringLiteral("redacted_thinking")) {
                ProviderItem item = ProviderItem::makeReasoning(
                    block.value(QStringLiteral("thinking")).toString(),
                    blockType == QStringLiteral("redacted_thinking")
                        ? block.value(QStringLiteral("data")).toString()
                        : block.value(QStringLiteral("signature")).toString(),
                    blockType == QStringLiteral("redacted_thinking"), true);
                outputItems.append(item);
                if (!item.reasoningText.isEmpty()) {
                    ProviderReasoningDelta delta;
                    delta.base.messageId = m_currentMessageId;
                    delta.base.partIndex = index;
                    delta.text = item.reasoningText;
                    events.append(ProviderEvent::fromReasoningDelta(delta));
                }
            } else if (blockType == QStringLiteral("tool_use")
                       || blockType == QStringLiteral("server_tool_use")) {
                const QString callId = block.value(QStringLiteral("id")).toString();
                const QString name = blockType == QStringLiteral("server_tool_use")
                    ? anthropicServerToolName(
                          block.value(QStringLiteral("name")).toString())
                    : block.value(QStringLiteral("name")).toString();
                const QJsonObject arguments =
                    block.value(QStringLiteral("input")).toObject();
                ProviderItem item = blockType == QStringLiteral("server_tool_use")
                    ? ProviderItem::makeServerToolCall(
                          callId, name, arguments, compactJson(arguments))
                    : ProviderItem::makeFunctionCall(
                          callId, name, arguments, compactJson(arguments));
                const QJsonObject caller =
                    block.value(QStringLiteral("caller")).toObject();
                item.callerKind = parseCallerKind(
                    caller.value(QStringLiteral("type")).toString());
                item.callerId = caller.value(QStringLiteral("id")).toString();
                outputItems.append(item);

                ProviderToolCallStart callStart;
                callStart.base.messageId = m_currentMessageId;
                callStart.base.partIndex = index;
                callStart.toolCallId = callId;
                callStart.toolName = name;
                callStart.isServerTool =
                    blockType == QStringLiteral("server_tool_use");
                events.append(ProviderEvent::toolCallStarted(callStart));
                ProviderToolCallEnd callEnd;
                callEnd.base = callStart.base;
                callEnd.toolCallId = callId;
                callEnd.toolName = name;
                callEnd.isServerTool = callStart.isServerTool;
                callEnd.arguments = arguments;
                callEnd.rawArguments = item.rawArguments;
                events.append(ProviderEvent::toolCallCompleted(callEnd));
            } else if (blockType.endsWith(QStringLiteral("_tool_result"))) {
                outputItems.append(ProviderItem::makeServerToolResult(
                    block.value(QStringLiteral("tool_use_id")).toString(),
                    anthropicServerToolName(blockType),
                    compactJson(block.value(QStringLiteral("content"))), {},
                    block.value(QStringLiteral("is_error")).toBool(false)));
            } else if (!blockType.isEmpty()) {
                qWarning().noquote()
                    << QStringLiteral("Anthropic adapter 丢弃未知非流式 content block type：%1")
                           .arg(blockType);
            }
        }

        if (m_lastUsage.inputTokens || m_lastUsage.outputTokens
            || m_lastUsage.cacheReadTokens || m_lastUsage.cacheWriteTokens
            || m_lastUsage.thoughtTokens) {
            events.append(ProviderEvent::usageUpdated(m_lastUsage));
        }
        ProviderResponseMetadata metadata;
        metadata.providerResponseId = m_currentMessageId;
        metadata.containerId = m_containerId;
        events.append(ProviderEvent::responseMetadataUpdated(metadata));
        ProviderMessageEnd end;
        end.messageId = m_currentMessageId;
        end.stopReason = stopReasonFromString(
            doc.value(QStringLiteral("stop_reason")).toString());
        end.finalUsage = m_lastUsage;
        end.outputItems = outputItems;
        events.append(ProviderEvent::messageCompleted(end));
        return events;
    }

    static const QHash<QString, QList<ProviderEvent> (AnthropicProvider::*)(const QJsonObject &)> handlers = {
        {QStringLiteral("message_start"), &AnthropicProvider::handleMessageStart},
        {QStringLiteral("content_block_start"), &AnthropicProvider::handleContentBlockStart},
        {QStringLiteral("content_block_delta"), &AnthropicProvider::handleContentBlockDelta},
        {QStringLiteral("content_block_stop"), &AnthropicProvider::handleContentBlockStop},
        {QStringLiteral("message_delta"), &AnthropicProvider::handleMessageDelta},
        {QStringLiteral("message_stop"), &AnthropicProvider::handleMessageStop},
    };

    const auto it = handlers.constFind(type);
    if (it != handlers.constEnd()) {
        return (this->*(it.value()))(doc);
    }

    return {};
}

void AnthropicProvider::resetProviderTurnState()
{
    m_currentMessageId.clear();
    m_blockTypes.clear();
    m_toolUseIds.clear();
    m_toolUseNames.clear();
    m_toolArgsBuffer.clear();
    m_toolCallerKinds.clear();
    m_toolCallerIds.clear();
    m_stopReason.clear();
    m_lastUsage = {};
    m_activeReasoningPartIndex = -1;
}

// ---- 事件处理器 ----

QList<ProviderEvent> AnthropicProvider::handleMessageStart(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const QJsonObject message = payload.value(QStringLiteral("message")).toObject();
    m_currentMessageId = message.value(QStringLiteral("id")).toString();
    const QJsonValue container = message.value(QStringLiteral("container"));
    if (container.isString())
        m_containerId = container.toString();
    else if (container.isObject())
        m_containerId = container.toObject().value(QStringLiteral("id")).toString();

    const QJsonObject usage = message.value(QStringLiteral("usage")).toObject();
    m_lastUsage = usageFromJson(usage);

    ProviderMessageStart start;
    start.messageId = m_currentMessageId;
    start.initialUsage = m_lastUsage;
    events.append(ProviderEvent::messageStarted(start));

    if (m_lastUsage.inputTokens || m_lastUsage.outputTokens
        || m_lastUsage.cacheReadTokens || m_lastUsage.cacheWriteTokens) {
        events.append(ProviderEvent::usageUpdated(m_lastUsage));
    }

    return events;
}

QList<ProviderEvent> AnthropicProvider::handleContentBlockStart(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const int index = payload.value(QStringLiteral("index")).toInt(-1);
    const QJsonObject contentBlock = payload.value(QStringLiteral("content_block")).toObject();
    const QString blockType = contentBlock.value(QStringLiteral("type")).toString();

    m_blockTypes.insert(index, blockType);

    if (blockType == QStringLiteral("text")) {
        ensureTextPartStarted(m_currentMessageId);
    } else if (blockType == QStringLiteral("thinking")
               || blockType == QStringLiteral("redacted_thinking")) {
        ensureReasoningPartStarted(m_currentMessageId, index);
        if (blockType == QStringLiteral("redacted_thinking")) {
            turnState().fallbackOutputItems.append(ProviderItem::makeReasoning(
                {}, contentBlock.value(QStringLiteral("data")).toString(), true, true));
        }
    } else if (blockType == QStringLiteral("tool_use")
               || blockType == QStringLiteral("server_tool_use")) {
        const QString toolId = contentBlock.value(QStringLiteral("id")).toString();
        const QString toolName = blockType == QStringLiteral("server_tool_use")
            ? anthropicServerToolName(contentBlock.value(QStringLiteral("name")).toString())
            : contentBlock.value(QStringLiteral("name")).toString();
        m_toolUseIds.insert(index, toolId);
        m_toolUseNames.insert(index, toolName);
        m_toolArgsBuffer.insert(toolId, QString());
        const QJsonObject caller = contentBlock.value(QStringLiteral("caller")).toObject();
        m_toolCallerKinds.insert(
            toolId, parseCallerKind(caller.value(QStringLiteral("type")).toString()));
        m_toolCallerIds.insert(toolId, caller.value(QStringLiteral("id")).toString());

        ensureToolPartStarted(toolId, m_currentMessageId, index);

        ProviderToolCallStart call;
        call.base.messageId = m_currentMessageId;
        call.base.partIndex = index;
        call.toolCallId = toolId;
        call.toolName = toolName;
        call.isServerTool = blockType == QStringLiteral("server_tool_use");
        events.append(ProviderEvent::toolCallStarted(call));
    } else if (blockType.endsWith(QStringLiteral("_tool_result"))) {
        const QString callId = contentBlock.value(QStringLiteral("tool_use_id")).toString();
        const QString name = anthropicServerToolName(blockType);
        ProviderItem result = ProviderItem::makeServerToolResult(
            callId, name, compactJson(contentBlock.value(QStringLiteral("content"))),
            {}, contentBlock.value(QStringLiteral("is_error")).toBool(false));
        result.itemId = contentBlock.value(QStringLiteral("id")).toString(result.itemId);
        turnState().fallbackOutputItems.append(result);
    } else if (!blockType.isEmpty()) {
        qWarning().noquote()
            << QStringLiteral("Anthropic adapter 丢弃未知 content block type：%1")
                   .arg(blockType);
    }

    return events;
}

QList<ProviderEvent> AnthropicProvider::handleContentBlockDelta(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const int index = payload.value(QStringLiteral("index")).toInt(-1);
    const QJsonObject delta = payload.value(QStringLiteral("delta")).toObject();
    const QString deltaType = delta.value(QStringLiteral("type")).toString();

    if (deltaType == QStringLiteral("text_delta")) {
        ProviderTextDelta textDelta;
        textDelta.base.messageId = m_currentMessageId;
        textDelta.base.partIndex = index;
        textDelta.text = delta.value(QStringLiteral("text")).toString();
        emitTextDelta(textDelta);
    } else if (deltaType == QStringLiteral("thinking_delta")) {
        ProviderReasoningDelta reasoningDelta;
        reasoningDelta.base.messageId = m_currentMessageId;
        reasoningDelta.base.partIndex = index;
        reasoningDelta.text = delta.value(QStringLiteral("thinking")).toString();
        emitReasoningDelta(reasoningDelta);
    } else if (deltaType == QStringLiteral("signature_delta")) {
        for (int i = turnState().fallbackOutputItems.size() - 1; i >= 0; --i) {
            ProviderItem &item = turnState().fallbackOutputItems[i];
            if (item.kind == ProviderItemKind::Reasoning) {
                item.reasoningSignature.append(delta.value(QStringLiteral("signature")).toString());
                item.reasoningMustReplay = true;
                break;
            }
        }
    } else if (deltaType == QStringLiteral("citations_delta")) {
        const QJsonObject citationObject =
            delta.value(QStringLiteral("citation")).toObject();
        ProviderCitation citation{
            citationObject.value(QStringLiteral("url")).toString(),
            citationObject.value(QStringLiteral("title")).toString(
                citationObject.value(QStringLiteral("document_title")).toString()),
            citationObject.value(QStringLiteral("cited_text")).toString(),
            citationObject.value(QStringLiteral("start_char_index")).toInt(-1),
            citationObject.value(QStringLiteral("end_char_index")).toInt(-1)};
        for (qsizetype i = turnState().fallbackOutputItems.size() - 1; i >= 0; --i) {
            ProviderItem &item = turnState().fallbackOutputItems[i];
            if (item.kind == ProviderItemKind::AssistantMessage) {
                if (item.parts.isEmpty())
                    item.parts.append(ProviderMessagePart::makeText({}));
                item.parts.first().citations.append(citation);
                break;
            }
        }
    } else if (deltaType == QStringLiteral("input_json_delta")) {
        const QString partialJson = delta.value(QStringLiteral("partial_json")).toString();
        const QString toolId = m_toolUseIds.value(index);
        if (!toolId.isEmpty()) {
            m_toolArgsBuffer[toolId].append(partialJson);
        }
    }

    return events;
}

QList<ProviderEvent> AnthropicProvider::handleContentBlockStop(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const int index = payload.value(QStringLiteral("index")).toInt(-1);
    const QString blockType = m_blockTypes.value(index);

    if (blockType == QStringLiteral("text")) {
        completeTextPartIfOpen(m_currentMessageId);
    } else if (blockType == QStringLiteral("thinking")
               || blockType == QStringLiteral("redacted_thinking")) {
        completeReasoningPartIfOpen(m_currentMessageId);
    } else if (blockType == QStringLiteral("tool_use")
               || blockType == QStringLiteral("server_tool_use")) {
        const QString toolId = m_toolUseIds.value(index);
        const QString toolName = m_toolUseNames.value(index);
        const QString rawArgs = m_toolArgsBuffer.value(toolId);

        completeToolPartIfOpen(toolId, m_currentMessageId);

        ProviderToolCallEnd callEnd;
        callEnd.base.messageId = m_currentMessageId;
        callEnd.base.partIndex = index;
        callEnd.toolCallId = toolId;
        callEnd.toolName = toolName;
        callEnd.isServerTool = blockType == QStringLiteral("server_tool_use");
        callEnd.rawArguments = rawArgs;

        const QJsonDocument argsDoc = QJsonDocument::fromJson(rawArgs.toUtf8());
        if (argsDoc.isObject()) {
            callEnd.arguments = argsDoc.object();
        } else if (!rawArgs.isEmpty()) {
            callEnd.parseFailed = true;
        }

        events.append(ProviderEvent::toolCallCompleted(callEnd));
    }

    return events;
}

QList<ProviderEvent> AnthropicProvider::handleMessageDelta(const QJsonObject &payload)
{
    QList<ProviderEvent> events;

    const QJsonObject delta = payload.value(QStringLiteral("delta")).toObject();
    const QString stopReason = delta.value(QStringLiteral("stop_reason")).toString();
    m_stopReason = stopReason;

    const QJsonObject usage = payload.value(QStringLiteral("usage")).toObject();
    if (!usage.isEmpty()) {
        const ProviderUsage deltaUsage = usageFromJson(usage);
        if (deltaUsage.inputTokens)
            m_lastUsage.inputTokens = deltaUsage.inputTokens;
        if (deltaUsage.outputTokens)
            m_lastUsage.outputTokens = deltaUsage.outputTokens;
        if (deltaUsage.cacheReadTokens)
            m_lastUsage.cacheReadTokens = deltaUsage.cacheReadTokens;
        if (deltaUsage.cacheWriteTokens)
            m_lastUsage.cacheWriteTokens = deltaUsage.cacheWriteTokens;
        if (deltaUsage.thoughtTokens)
            m_lastUsage.thoughtTokens = deltaUsage.thoughtTokens;
        events.append(ProviderEvent::usageUpdated(m_lastUsage));
    }

    return events;
}

QList<ProviderEvent> AnthropicProvider::handleMessageStop(const QJsonObject &payload)
{
    Q_UNUSED(payload);

    QList<ProviderEvent> events;

    ProviderResponseMetadata metadata;
    metadata.providerResponseId = m_currentMessageId;
    metadata.containerId = m_containerId;
    events.append(ProviderEvent::responseMetadataUpdated(metadata));

    ProviderMessageEnd messageEnd;
    messageEnd.messageId = m_currentMessageId;
    messageEnd.stopReason = stopReasonFromString(m_stopReason);
    messageEnd.finalUsage = m_lastUsage;
    for (ProviderItem &item : turnState().fallbackOutputItems) {
        if ((item.kind == ProviderItemKind::FunctionCall
             || item.kind == ProviderItemKind::ServerToolCall)
            && m_toolCallerKinds.contains(item.callId)) {
            item.callerKind = m_toolCallerKinds.value(item.callId);
            item.callerId = m_toolCallerIds.value(item.callId);
        }
    }
    messageEnd.outputItems = turnState().fallbackOutputItems;
    events.append(ProviderEvent::messageCompleted(messageEnd));

    return events;
}

// ---- 辅助 ----

ProviderUsage AnthropicProvider::usageFromJson(const QJsonObject &usageObject) const
{
    ProviderUsage usage;
    usage.inputTokens = usageObject.value(QStringLiteral("input_tokens")).toInt();
    usage.outputTokens = usageObject.value(QStringLiteral("output_tokens")).toInt();
    usage.cacheReadTokens = usageObject.value(QStringLiteral("cache_read_input_tokens")).toInt();
    usage.cacheWriteTokens = usageObject.value(QStringLiteral("cache_creation_input_tokens")).toInt();
    usage.thoughtTokens = usageObject.value(QStringLiteral("thinking_tokens")).toInt();
    return usage;
}

StopReason AnthropicProvider::stopReasonFromString(const QString &reason) const
{
    if (reason == QStringLiteral("end_turn")) return StopReason::EndTurn;
    if (reason == QStringLiteral("max_tokens")) return StopReason::MaxTokens;
    if (reason == QStringLiteral("tool_use")) return StopReason::ToolUse;
    if (reason == QStringLiteral("pause_turn")) return StopReason::PauseTurn;
    if (reason == QStringLiteral("stop_sequence")) return StopReason::EndTurn;
    return StopReason::EndTurn;
}

QString AnthropicProvider::currentMessageId() const
{
    return m_currentMessageId;
}
