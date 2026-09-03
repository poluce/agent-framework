#include "ChatCompletionsProvider.h"

#include "providers/transport/HttpSseChannel.h"
#include "providers/core/ProviderRetryPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace {

QJsonObject roleContentItem(const QString &role, const QString &content)
{
    QJsonObject item;
    item.insert(QStringLiteral("role"), role);
    item.insert(QStringLiteral("content"), content);
    return item;
}


// 构建用户消息：纯文本时返回简单 content 字符串，含图片时返回 content 数组
QJsonObject buildUserMessage(const QString &text, const QList<ProviderImageAsset> &images)
{
    QJsonObject msg;
    msg.insert(QStringLiteral("role"), QStringLiteral("user"));

    if (images.isEmpty()) {
        msg.insert(QStringLiteral("content"), text);
        return msg;
    }

    QJsonArray contentParts;
    if (!text.isEmpty()) {
        QJsonObject textPart;
        textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
        textPart.insert(QStringLiteral("text"), text);
        contentParts.append(textPart);
    }
    for (const ProviderImageAsset &img : images) {
        if (!img.hasUri() && !img.hasInlineData()) continue;
        QJsonObject imgPart;
        imgPart.insert(QStringLiteral("type"), QStringLiteral("image_url"));
        QJsonObject imgUrlObj;
        if (img.hasUri()) {
            imgUrlObj.insert(QStringLiteral("url"), img.uri);
        } else {
            const QString mime = img.mimeType.isEmpty()
                ? QStringLiteral("image/png") : img.mimeType;
            const QString dataUrl = QStringLiteral("data:%1;base64,").arg(mime)
                                    + QString::fromLatin1(img.data.toBase64());
            imgUrlObj.insert(QStringLiteral("url"), dataUrl);
        }
        imgPart.insert(QStringLiteral("image_url"), imgUrlObj);
        contentParts.append(imgPart);
    }
    msg.insert(QStringLiteral("content"), contentParts);
    return msg;
}

QJsonValue toolChoiceJsonValue(const ProviderToolChoice &toolChoice)
{
    switch (toolChoice.mode) {
    case ProviderToolChoiceMode::ProviderDefault:
        return QJsonValue(QJsonValue::Undefined);
    case ProviderToolChoiceMode::None:
        return QStringLiteral("none");
    case ProviderToolChoiceMode::Auto:
        return QStringLiteral("auto");
    case ProviderToolChoiceMode::Required:
        return QStringLiteral("required");
    case ProviderToolChoiceMode::Named: {
        QJsonObject choice;
        choice.insert(QStringLiteral("type"), QStringLiteral("function"));
        choice.insert(QStringLiteral("name"), toolChoice.toolName);
        return choice;
    }
    }
    return QJsonValue(QJsonValue::Undefined);
}

} // namespace

// ---- ChatCompletionsProvider ----

ChatCompletionsProvider::ChatCompletionsProvider(QObject *parent)
    : ChatCompletionsProvider(QStringLiteral("chat-completions"), parent)
{
}

ChatCompletionsProvider::ChatCompletionsProvider(const QString &providerType, QObject *parent)
    : AbstractProvider(providerType, parent)
{
    connect(m_channel.get(), &HttpSseChannel::finished,
            this, &ChatCompletionsProvider::handleTransportFinished);
}

void ChatCompletionsProvider::handleTransportFinished()
{
    // Error / Cancelled 已标 terminal：禁止再合成成功完成态。
    if (turnState().terminal) {
        return;
    }
    // HTTP 200 空包 / 未识别正文：连接已结束，必须立刻失败，不能把 Loop 丢给看门狗。
    if (!turnState().messageStarted) {
        emitErrorOccurred(ProviderError{
            QStringLiteral("chat_completions_empty_response"),
            QStringLiteral("Chat Completions 响应结束但未产生任何内容。")});
        return;
    }
    ProviderMessageEnd messageEnd;
    messageEnd.messageId = m_currentMessageId;
    messageEnd.stopReason = m_stopReason;
    messageEnd.logprobs = m_logprobs;
    emitEvents({ProviderEvent::messageCompleted(messageEnd)});
}

ChatCompletionsProvider::~ChatCompletionsProvider() = default;

void ChatCompletionsProvider::resetProviderTurnState()
{
    m_currentMessageId.clear();
    m_logprobs = {};
    m_stopReason = StopReason::EndTurn;
    m_pendingToolCallIds.clear();
    m_pendingToolCallNames.clear();
    m_pendingToolCallArgs.clear();
    m_activeReasoningPartIndex = -1;
}

QString ChatCompletionsProvider::currentMessageId() const
{
    return m_currentMessageId;
}

QUrl ChatCompletionsProvider::buildModelsUrl(const QString &baseUrl) const
{
    QUrl url;
    QString base = baseUrl.trimmed();
    if (base.endsWith(QStringLiteral("/models")) || base.contains(QStringLiteral("/models?"))) {
        url = QUrl(base);
    } else if (base.endsWith(QStringLiteral("/v1"))) {
        url = QUrl(base + QStringLiteral("/models"));
    } else if (base.endsWith(QLatin1Char('/'))) {
        url = QUrl(base + QStringLiteral("v1/models"));
    } else {
        url = QUrl(base + QStringLiteral("/v1/models"));
    }
    return url;
}

QList<ModelCapabilities> ChatCompletionsProvider::parseModelsPayload(const QByteArray &body,
                                                               QString *errorMessage) const
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("模型列表不是有效 JSON。");
        }
        return {};
    }

    QList<ModelCapabilities> models;
    const QJsonArray data = document.object().value(QStringLiteral("data")).toArray();
    for (const QJsonValue &value : data) {
        if (!value.isObject()) continue;
        ModelCapabilities capabilities;
        capabilities.modelId = value.toObject().value(QStringLiteral("id")).toString().trimmed();
        if (!capabilities.modelId.isEmpty()) {
            capabilities.enable(ProviderCapability::TextInput)
                .enable(ProviderCapability::ImageInput)
                .enable(ProviderCapability::TextOutput)
                .enable(ProviderCapability::ImageOutput)
                .enable(ProviderCapability::ToolCalling)
                .enable(ProviderCapability::Reasoning)
                .enable(ProviderCapability::ToolChoice)
                .enable(ProviderCapability::MaxOutputTokens);
            models.append(capabilities);
        }
    }

    if (models.isEmpty() && errorMessage) {
        *errorMessage = QStringLiteral("模型列表为空。");
    }
    return models;
}

ProviderError ChatCompletionsProvider::validateProviderRequest(const ProviderRequest &request) const
{
    if (request.protocolFamily != ProviderProtocolFamily::Auto
        && request.protocolFamily != ProviderProtocolFamily::OpenAiChatCompletions
        && request.protocolFamily != ProviderProtocolFamily::DeepSeekChatCompletions) {
        return {QStringLiteral("protocol_family_mismatch"),
                QStringLiteral("Chat Completions 适配器不能处理指定的协议族。")};
    }
    if (request.items.isEmpty()) {
        return {QStringLiteral("empty_input"), QStringLiteral("请求输入为空。")};
    }
    if (request.desiredOutput.imageEnabled || request.desiredOutput.audioEnabled
        || request.audio.isExplicit()) {
        return {QStringLiteral("output_modality_not_supported"),
                QStringLiteral("Chat Completions 适配器当前仅支持文本输出。")};
    }
    if (!request.responseInclude.isEmpty()
        || request.backgroundExecution != ProviderTriState::Unset
        || request.storeServerState != ProviderTriState::Unset
        || !request.providerConversationId.isEmpty()
        || !request.providerCachedContentId.isEmpty()
        || !request.mediaResolution.isEmpty()
        || !request.metadata.isEmpty()) {
        return {QStringLiteral("request_option_not_supported"),
                QStringLiteral("Chat Completions 方言不支持 Responses/Gemini 专用请求选项。")};
    }
    if (request.hasAudioInput() || request.hasDocumentInput() || request.hasVideoInput()) {
        return {QStringLiteral("media_input_not_supported"),
                QStringLiteral("Chat Completions 适配器仅支持文本和图片输入。")};
    }
    for (const ProviderItem &item : request.items) {
        if (item.isServerToolRelated() || item.isProgramRelated()
            || item.isApprovalRelated()
            || item.kind == ProviderItemKind::Compaction) {
            return {QStringLiteral("unsupported_provider_item"),
                    QStringLiteral("Chat Completions 方言不支持该 ProviderItem kind：%1")
                        .arg(static_cast<int>(item.kind))};
        }
        if (item.callerKind != ProviderCallerKind::Unset) {
            return {QStringLiteral("caller_not_supported"),
                    QStringLiteral("Chat Completions 方言不支持工具 caller。")};
        }
    }
    for (const ProviderToolSpecification &tool : request.tools) {
        if (!tool.outputSchema.isEmpty() || tool.deferLoading
            || !tool.allowedCallers.isEmpty()) {
            return {QStringLiteral("tool_extension_not_supported"),
                    QStringLiteral("Chat Completions 方言不支持 output_schema、defer_loading 或 allowed_callers。")};
        }
    }
    return {};
}

ProviderTransportRequest ChatCompletionsProvider::buildProviderTransportRequest(const ProviderRequest &request) const
{
    QJsonObject body = buildRequestBody(request);
    return buildStandardTransport(body, request.stream);
}

QJsonObject ChatCompletionsProvider::buildRequestBody(const ProviderRequest &request) const
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), m_auth.modelName);
    body.insert(QStringLiteral("stream"), request.stream);
    if (request.maxOutputTokens > 0)
        body.insert(QStringLiteral("max_tokens"), request.maxOutputTokens);
    if (request.temperature >= 0.0)
        body.insert(QStringLiteral("temperature"), request.temperature);
    // reasoning_effort — OpenAI 兼容端点的思考强度（minimal/low/medium/high 官方合法，
    // 部分模型支持 xhigh/max；不支持的值由服务端报错，客户端不猜测折叠）。
    if (request.reasoning.enabled
        && request.reasoning.effort != ProviderReasoningEffort::Unset) {
        body.insert(QStringLiteral("reasoning_effort"), toString(request.reasoning.effort));
    }
    if (request.sampling.topP >= 0.0)
        body.insert(QStringLiteral("top_p"), request.sampling.topP);
    if (request.sampling.seed >= 0)
        body.insert(QStringLiteral("seed"), static_cast<double>(request.sampling.seed));
    if (!request.sampling.stop.isEmpty())
        body.insert(QStringLiteral("stop"), QJsonArray::fromStringList(request.sampling.stop));
    if (request.sampling.penaltiesRequested) {
        body.insert(QStringLiteral("presence_penalty"), request.sampling.presencePenalty);
        body.insert(QStringLiteral("frequency_penalty"), request.sampling.frequencyPenalty);
    }
    if (request.requestLogprobs) {
        body.insert(QStringLiteral("logprobs"), true);
        if (request.topLogprobs > 0)
            body.insert(QStringLiteral("top_logprobs"), request.topLogprobs);
    }
    if (request.toolChoice.allowParallel != ProviderTriState::Unset)
        body.insert(QStringLiteral("parallel_tool_calls"),
                    request.toolChoice.allowParallel == ProviderTriState::Yes);
    if (request.responseFormat.kind == ProviderResponseFormatKind::JsonObject) {
        body.insert(QStringLiteral("response_format"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}});
    } else if (request.responseFormat.kind == ProviderResponseFormatKind::JsonSchema) {
        body.insert(QStringLiteral("response_format"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                                {QStringLiteral("json_schema"),
                                 QJsonObject{{QStringLiteral("name"), request.responseFormat.schemaName},
                                             {QStringLiteral("schema"), request.responseFormat.jsonSchema}}}});
    }
    if (request.stream) {
        body.insert(QStringLiteral("stream_options"),
                    QJsonObject{{QStringLiteral("include_usage"), true}});
    }

    QJsonArray messages = buildMessages(request);
    if (!messages.isEmpty()) {
        body.insert(QStringLiteral("messages"), messages);
    }

    QJsonArray tools = buildTools(request);
    if (!tools.isEmpty()) {
        body.insert(QStringLiteral("tools"), tools);
    }

    const QJsonValue toolChoice = toolChoiceJsonValue(request.toolChoice);
    if (!toolChoice.isUndefined()) {
        body.insert(QStringLiteral("tool_choice"), toolChoice);
    }

    return body;
}

QJsonObject ChatCompletionsProvider::buildAssistantMessageForToolCall(const ProviderItem &item, const ProviderRequest &request) const
{
    QJsonObject msg;
    msg.insert(QStringLiteral("role"), QStringLiteral("assistant"));
    msg.insert(QStringLiteral("content"), QJsonValue::Null);
    msg.insert(QStringLiteral("tool_calls"), QJsonArray{});
    // deepseek 思考模式要求：携带 tools 的请求必须回传 reasoning_content，
    // 否则思考上下文断裂（生成质量崩坏/半句截断）。有值才插入。
    if (!item.reasoningText.trimmed().isEmpty()) {
        msg.insert(QStringLiteral("reasoning_content"), item.reasoningText);
    }
    return msg;
}

// ──────────────────────────────────────────────────────────────────────────────
// buildMessages：assistant 消息组装（OpenAI 兼容协议族，含 DeepSeek 思考模式）
//
// 【「半句就停」根因（2026-08-02 已修复，手测通过）】
//
// DeepSeek 思考模式下，一次模型响应在一条 assistant 消息里同时携带三个字段：
//   reasoning_content（思维链） + content（正文） + tool_calls（工具调用）
// 官方样例（api-docs.deepseek.com/zh-cn/guides/thinking_mode 工具调用一节）：
//   messages.append(response.choices[0].message)  // 三字段同存、同一条
// 且官方明确要求：携带 tools 的请求在后续所有轮次必须完整回传 reasoning_content，
// 否则思考上下文断裂。
//
// 但流式增量按 reasoning_content → content → tool_calls 顺序到达，账本
// （ProviderRunLedger）据此把同一次响应拆成三个独立条目：
//   Reasoning → AssistantMessage（正文） → FunctionCall
// 旧实现逐条各自发 wire：pendingReasoning 被 AssistantMessage 分支消费并清空，
// FunctionCall 分支建 tool_calls 消息时已拿不到思考 → history 里变成两条：
//   [assistant rc=xxx content="半句…："] （思考+正文，无工具调用）
//   [assistant rc="" content=null tool_calls=[...]] （工具调用，思考丢失）
//
// 后果：模型每轮在上下文里看到「自己说半句冒号结尾、且不带工具调用」的完整
// 历史消息，会模仿该输出模式——新一轮只输出半句正文就 finish_reason=stop
// （「半句就停」：output 仅几十 token、wasTruncated=false、未超 max_tokens）。
// 排查过程曾排除：SSE 断流、工具解析丢失、8192 max_tokens、上下文超窗。
// 铁证：request_bodies 里同一次响应被拆两条（如 [15] rc=228 content=29 tc=0
// 与 [16] rc=0 tc=2）；修复后同一条 assistant 同时带三字段。
//
// 修复：正文先暂存 pendingText，不提前发 wire；
//   · 有 FunctionCall → 合并进同一条 tool_calls assistant（content + reasoning_content
//     + tool_calls 同存，符合官方契约）；
//   · 无工具调用 → flushPendingText 在 UserMessage 边界或末尾独立落 wire（携带思考）。
// 对不开思考模式的普通 chat-completions 行为等价（pendingReasoning 恒空、
// 正文 flush 顺序不变，仅多一个空字段判断）。
// ──────────────────────────────────────────────────────────────────────────────
QJsonArray ChatCompletionsProvider::buildMessages(const ProviderRequest &request) const
{
    QJsonArray messages;

    auto flushPendingToolCalls = [&messages](QJsonObject &pendingMsg, bool &hasPending) {
        if (!hasPending) return;
        messages.append(pendingMsg);
        pendingMsg = QJsonObject{};
        hasPending = false;
    };

    QJsonObject pendingToolCallMsg;
    bool hasPendingToolCalls = false;
    QString pendingReasoning;
    // 正文暂存：见函数头注释（「半句就停」根因）——等 FunctionCall 一起合并进同一条 assistant
    QString pendingText;

    // 无工具调用的正文：作为独立 assistant 消息落 wire（携带其前序思考；
    // deepseek 对无工具轮次会忽略 reasoning_content，带上无害且保持字段一致）
    auto flushPendingText = [&]() {
        const QString text = pendingText.trimmed();
        if (text.isEmpty())
            return;
        QJsonObject msg;
        msg.insert(QStringLiteral("role"), QStringLiteral("assistant"));
        msg.insert(QStringLiteral("content"), text);
        const QString reasoning = pendingReasoning.trimmed();
        if (!reasoning.isEmpty())
            msg.insert(QStringLiteral("reasoning_content"), reasoning);
        messages.append(msg);
        pendingText.clear();
        pendingReasoning.clear();
    };

    for (const ProviderItem &item : request.items) {
        switch (item.kind) {
        case ProviderItemKind::UserMessage: {
            flushPendingToolCalls(pendingToolCallMsg, hasPendingToolCalls);
            flushPendingText();
            QList<ProviderImageAsset> images;
            for (const ProviderMessagePart &part : item.parts) {
                if (part.kind == ProviderPartKind::Image)
                    images.append(part.image);
            }
            const QString text = joinedText(item.parts);
            if (!text.isEmpty() || !images.isEmpty())
                messages.append(buildUserMessage(text, images));
            break;
        }
        case ProviderItemKind::AssistantMessage: {
            const QString text = joinedText(item.parts).trimmed();
            // 空助手气泡不进 wire（Responses 完成态常夹空 AssistantMessage），思考留给后续条目
            if (text.isEmpty())
                break;
            // 正文暂存：等 FunctionCall 一起合并进同一条 assistant（deepseek 思考模式要求）
            if (hasPendingToolCalls) {
                // 挂起 tool_calls 时正文并入 tool_calls 消息（思考不能清：工具消息仍需要它）
                const QString existing =
                    pendingToolCallMsg.value(QStringLiteral("content")).toString().trimmed();
                pendingToolCallMsg.insert(
                    QStringLiteral("content"),
                    existing.isEmpty() ? text
                                       : existing + QLatin1Char('\n') + text);
            } else {
                if (!pendingText.trimmed().isEmpty())
                    pendingText += QLatin1Char('\n');
                pendingText += text;
            }
            break;
        }
        case ProviderItemKind::FunctionCall: {
            if (!hasPendingToolCalls) {
                ProviderItem callItem = item;
                callItem.reasoningText = pendingReasoning;
                pendingToolCallMsg = buildAssistantMessageForToolCall(callItem, request);
                hasPendingToolCalls = true;
                // 正文合并进 tool_calls 消息：思考模式下次响应若带 content，与 tool_calls 同存
                const QString text = pendingText.trimmed();
                if (!text.isEmpty())
                    pendingToolCallMsg.insert(QStringLiteral("content"), text);
                pendingText.clear();
            }
            QJsonObject tc;
            tc.insert(QStringLiteral("id"), item.callId);
            tc.insert(QStringLiteral("type"), QStringLiteral("function"));
            QJsonObject func;
            func.insert(QStringLiteral("name"), item.name);
            // 优先 raw；空则从已解析 arguments 回填，避免历史条目 arguments="" 污染下一轮
            QString rawArgs = item.rawArguments;
            if (rawArgs.trimmed().isEmpty() && !item.arguments.isEmpty())
                rawArgs = compactJson(item.arguments);
            func.insert(QStringLiteral("arguments"), rawArgs);
            tc.insert(QStringLiteral("function"), func);
            QJsonArray arr = pendingToolCallMsg.value(QStringLiteral("tool_calls")).toArray();
            arr.append(tc);
            pendingToolCallMsg.insert(QStringLiteral("tool_calls"), arr);
            break;
        }
        case ProviderItemKind::Reasoning:
            pendingReasoning = item.reasoningText;
            break;
        case ProviderItemKind::FunctionCallOutput: {
            // 先落 tool_calls 助手消息，再立刻写 tool，保证 DeepSeek 严格配对顺序
            flushPendingToolCalls(pendingToolCallMsg, hasPendingToolCalls);
            QJsonObject msg;
            msg.insert(QStringLiteral("role"), QStringLiteral("tool"));
            msg.insert(QStringLiteral("content"), item.output);
            msg.insert(QStringLiteral("tool_call_id"), item.callId);
            messages.append(msg);
            break;
        }
        case ProviderItemKind::ServerToolCall:
        case ProviderItemKind::ServerToolResult:
        case ProviderItemKind::Program:
        case ProviderItemKind::ProgramOutput:
        case ProviderItemKind::ApprovalRequest:
        case ProviderItemKind::ApprovalResponse:
        case ProviderItemKind::Compaction:
            break;
        }
    }

    flushPendingToolCalls(pendingToolCallMsg, hasPendingToolCalls);
    // 末尾无工具调用的正文独立落 wire（如最后的纯文本回答轮次）
    flushPendingText();

    // system prompt 作为首条 system 消息
    const QString systemText = request.systemPrompt.trimmed();
    if (!systemText.isEmpty())
        messages.prepend(roleContentItem(QStringLiteral("system"), systemText));

    return messages;
}

QJsonArray ChatCompletionsProvider::buildTools(const ProviderRequest &request) const
{
    QJsonArray tools;
    for (const ProviderToolSpecification &spec : request.tools) {
        QJsonObject tool;
        tool.insert(QStringLiteral("type"), QStringLiteral("function"));
        QJsonObject function;
        function.insert(QStringLiteral("name"), spec.name);
        function.insert(QStringLiteral("description"), spec.description);
        function.insert(QStringLiteral("parameters"), spec.inputSchema);
        if (spec.strictSchema)
            function.insert(QStringLiteral("strict"), true);
        tool.insert(QStringLiteral("function"), function);
        tools.append(tool);
    }
    return tools;
}

QList<ProviderEvent> ChatCompletionsProvider::parseProviderTransportPayload(const ProviderTransportPayload &payload)
{
    return handleChunk(payload.document);
}

bool ChatCompletionsProvider::startProviderTransportRequest(const ProviderTransportRequest &request,
                                                      ProviderError *error)
{
    QString base = m_auth.baseUrl.trimmed();
    QUrl url;
    if (base.endsWith(QStringLiteral("/chat/completions")) || base.contains(QStringLiteral("/chat/completions?"))) {
        url = QUrl(base);
    } else if (base.endsWith(QStringLiteral("/v1"))) {
        url = QUrl(base + QStringLiteral("/chat/completions"));
    } else if (base.endsWith(QLatin1Char('/'))) {
        url = QUrl(base + QStringLiteral("v1/chat/completions"));
    } else {
        url = QUrl(base + QStringLiteral("/v1/chat/completions"));
    }

    const auto result = m_channel->start(url, m_auth.apiKey, request);
    if (!result.accepted) {
        if (error) *error = result.error;
        return false;
    }
    return true;
}

QList<ProviderEvent> ChatCompletionsProvider::handleChunk(const QJsonObject &chunk)
{
    QList<ProviderEvent> events;

    // HTTP 200 也可能是 OpenAI 系 error JSON（流式 data: 或整包 JSON）。
    // 必须经 emitErrorOccurred 收口；否则 finished 会空转，Loop 只能等看门狗。
    const QJsonValue errorValue = chunk.value(QStringLiteral("error"));
    if (errorValue.isObject() || errorValue.isString()) {
        ProviderError error;
        error.code = QStringLiteral("chat_completions_error");
        error.providerRaw = chunk;
        if (errorValue.isObject()) {
            const QJsonObject errorObject = errorValue.toObject();
            error.message = errorObject.value(QStringLiteral("message")).toString().trimmed();
            if (error.message.isEmpty()) {
                error.message = errorObject.value(QStringLiteral("msg")).toString().trimmed();
            }
        } else {
            error.message = errorValue.toString().trimmed();
        }
        if (error.message.isEmpty()) {
            error.message = chunk.value(QStringLiteral("message")).toString().trimmed();
        }
        if (error.message.isEmpty()) {
            error.message = QStringLiteral("Chat Completions 返回了错误响应。");
        }
        const ProviderRetry::Classification cls =
            ProviderRetry::classifyApiErrorValue(errorValue);
        error.retryable = cls.retryable;
        emitErrorOccurred(error);
        return {};
    }

    const QString chunkId = chunk.value(QStringLiteral("id")).toString();
    if (!chunkId.isEmpty()) {
        m_currentMessageId = chunkId;
    }

    if (chunk.contains(QStringLiteral("usage"))) {
        const ProviderUsage usage = usageFromChunk(chunk);
        events.append(ProviderEvent::usageUpdated(usage));
    }

    const QJsonArray choices = chunk.value(QStringLiteral("choices")).toArray();
    for (const QJsonValue &choiceVal : choices) {
        const QJsonObject choice = choiceVal.toObject();
        events.append(handleChunkChoice(choice));
    }

    return events;
}

QList<ProviderEvent> ChatCompletionsProvider::handleChunkChoice(const QJsonObject &choice)
{
    QList<ProviderEvent> events;

    QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
    const QJsonObject completeMessage =
        choice.value(QStringLiteral("message")).toObject();
    const bool nonStreaming = delta.isEmpty() && !completeMessage.isEmpty();
    if (nonStreaming)
        delta = completeMessage;
    const QString finishReason = choice.value(QStringLiteral("finish_reason")).toString();
    if (choice.value(QStringLiteral("logprobs")).isObject())
        m_logprobs = choice.value(QStringLiteral("logprobs")).toObject();

    events.append(handleDeltaContent(delta));
    events.append(handleDeltaToolCalls(delta));
    events.append(handleDeltaContentParts(delta));

    if (!finishReason.isEmpty() && finishReason != QStringLiteral("null")) {
        events.append(handleFinishReason(finishReason));
        if (nonStreaming) {
            QList<ProviderItem> outputItems;
            const QString reasoning =
                completeMessage.value(QStringLiteral("reasoning_content")).toString();
            const QJsonArray toolCalls =
                completeMessage.value(QStringLiteral("tool_calls")).toArray();
            if (!reasoning.isEmpty())
                outputItems.append(ProviderItem::makeReasoning(
                    reasoning, {}, false, !toolCalls.isEmpty()));
            const QString content =
                completeMessage.value(QStringLiteral("content")).toString();
            if (!content.isEmpty())
                outputItems.append(ProviderItem::makeAssistantText(content));
            for (const QJsonValue &toolCallValue : toolCalls) {
                const QJsonObject toolCall = toolCallValue.toObject();
                const QJsonObject function =
                    toolCall.value(QStringLiteral("function")).toObject();
                const QString rawArguments =
                    function.value(QStringLiteral("arguments")).toString();
                const QJsonDocument argumentsDocument =
                    QJsonDocument::fromJson(rawArguments.toUtf8());
                outputItems.append(ProviderItem::makeFunctionCall(
                    toolCall.value(QStringLiteral("id")).toString(),
                    function.value(QStringLiteral("name")).toString(),
                    argumentsDocument.isObject()
                        ? argumentsDocument.object() : QJsonObject{},
                    rawArguments));
            }
            ProviderMessageEnd end;
            end.messageId = currentMessageId();
            end.stopReason = m_stopReason;
            end.outputItems = outputItems;
            end.logprobs = m_logprobs;
            events.append(ProviderEvent::messageCompleted(end));
        }
    }

    return events;
}

QList<ProviderEvent> ChatCompletionsProvider::handleDeltaContent(const QJsonObject &delta)
{
    QList<ProviderEvent> events;
    const QString content = delta.value(QStringLiteral("content")).toString();

    if (!content.isEmpty()) {
        ProviderTextDelta textDelta;
        textDelta.base.messageId = currentMessageId();
        textDelta.text = content;
        events.append(ProviderEvent::fromTextDelta(textDelta));
    }

    return events;
}

QList<ProviderEvent> ChatCompletionsProvider::handleDeltaContentParts(const QJsonObject &delta)
{
    QList<ProviderEvent> events;

    const QJsonArray parts = delta.value(QStringLiteral("content_parts")).toArray();
    for (const QJsonValue &partVal : parts) {
        const QJsonObject part = partVal.toObject();
        const QString type = part.value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("image_url") && type != QStringLiteral("image")) continue;

        const QJsonObject imgUrl = part.value(QStringLiteral("image_url")).toObject();
        const QString url = imgUrl.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) continue;

        // 解析 data: URL → 内联 base64 数据
        static const QString kDataPrefix = QStringLiteral("data:");
        static const QString kBase64Suffix = QStringLiteral(";base64,");
        if (!url.startsWith(kDataPrefix) || !url.contains(kBase64Suffix)) continue;

        const int mimeEnd = url.indexOf(QLatin1Char(';'), kDataPrefix.length());
        const QString mimeType = (mimeEnd > kDataPrefix.length())
            ? url.mid(kDataPrefix.length(), mimeEnd - kDataPrefix.length())
            : QStringLiteral("image/png");
        const int b64Start = url.indexOf(kBase64Suffix) + kBase64Suffix.length();
        const QByteArray rawData = QByteArray::fromBase64(
            url.mid(b64Start).toLatin1());

        ProviderImageOutput img;
        img.base.messageId = currentMessageId();
        img.image = ProviderImageAsset::fromBytes(rawData, mimeType);
        events.append(ProviderEvent::fromImageOutput(img));
    }

    return events;
}

QList<ProviderEvent> ChatCompletionsProvider::handleDeltaToolCalls(const QJsonObject &delta)
{
    QList<ProviderEvent> events;

    const QJsonArray toolCalls = delta.value(QStringLiteral("tool_calls")).toArray();
    for (const QJsonValue &tcVal : toolCalls) {
        const QJsonObject tc = tcVal.toObject();
        const int index = tc.value(QStringLiteral("index")).toInt(-1);
        if (index < 0) continue;

        const QString id = tc.value(QStringLiteral("id")).toString();
        const QJsonObject func = tc.value(QStringLiteral("function")).toObject();
        const QString funcName = func.value(QStringLiteral("name")).toString();
        const QString funcArgs = func.value(QStringLiteral("arguments")).toString();

        if (!id.isEmpty()) {
            m_pendingToolCallIds[index] = id;
        }
        if (!funcName.isEmpty()) {
            m_pendingToolCallNames[index] = funcName;
        }
        if (!funcArgs.isEmpty()) {
            m_pendingToolCallArgs[index].append(funcArgs);
        }

        const QString callId = m_pendingToolCallIds.value(index);
        const QString callName = m_pendingToolCallNames.value(index);
        if (!callId.isEmpty() && !callName.isEmpty()
            && !turnState().toolPartIndices.contains(callId)) {

            ProviderToolCallStart start;
            start.base.messageId = currentMessageId();
            start.toolCallId = callId;
            start.toolName = callName;
            events.append(ProviderEvent::toolCallStarted(start));
        }
    }

    return events;
}

QList<ProviderEvent> ChatCompletionsProvider::handleFinishReason(const QString &reason)
{
    QList<ProviderEvent> events;
    if (reason == QStringLiteral("tool_calls")
        || reason == QStringLiteral("function_call")) {
        m_stopReason = StopReason::ToolUse;
    } else if (reason == QStringLiteral("length")) {
        m_stopReason = StopReason::MaxTokens;
    } else if (reason == QStringLiteral("content_filter")) {
        m_stopReason = StopReason::Safety;
    } else {
        m_stopReason = StopReason::EndTurn;
    }

    if (reason == QStringLiteral("tool_calls")) {
        QList<int> sortedIdxs = m_pendingToolCallIds.keys();
        std::sort(sortedIdxs.begin(), sortedIdxs.end());
        for (int idx : sortedIdxs) {
            const QString &id = m_pendingToolCallIds.value(idx);
            const QString &name = m_pendingToolCallNames.value(idx);
            const QString &rawArgs = m_pendingToolCallArgs.value(idx);

            ProviderToolCallEnd end;
            end.base.messageId = currentMessageId();
            end.toolCallId = id;
            end.toolName = name;
            end.rawArguments = rawArgs;
            if (!rawArgs.isEmpty()) {
                end.arguments = QJsonDocument::fromJson(rawArgs.toUtf8()).object();
            } else {
                end.arguments = QJsonObject();
            }
            events.append(ProviderEvent::toolCallCompleted(end));
        }
        m_pendingToolCallIds.clear();
        m_pendingToolCallNames.clear();
        m_pendingToolCallArgs.clear();
    }

    return events;
}

ProviderUsage ChatCompletionsProvider::usageFromChunk(const QJsonObject &chunk) const
{
    const QJsonObject usage = chunk.value(QStringLiteral("usage")).toObject();
    ProviderUsage u;
    u.inputTokens = usage.value(QStringLiteral("prompt_tokens")).toInt();
    u.outputTokens = usage.value(QStringLiteral("completion_tokens")).toInt();
    u.cacheReadTokens = usage.value(QStringLiteral("prompt_tokens_details"))
                            .toObject()
                            .value(QStringLiteral("cached_tokens"))
                            .toInt();
    u.cacheWriteTokens = 0;
    u.thoughtTokens = usage.value(QStringLiteral("completion_tokens_details"))
                          .toObject()
                          .value(QStringLiteral("reasoning_tokens"))
                          .toInt();
    return u;
}

std::optional<ModelCapabilities> ChatCompletionsProvider::capabilitiesForModel(const QString &modelId) const
{
    for (const ModelCapabilities &capabilities : availableModels()) {
        if (capabilities.modelId == modelId) {
            return capabilities;
        }
    }
    return std::nullopt;
}
