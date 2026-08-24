#include "ResponsesProvider.h"

#include "providers/core/HttpSseChannel.h"
#include "providers/core/ProviderRetryPolicy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

namespace {

bool arrayContains(const QJsonArray &values, const QString &expected)
{
    for (const QJsonValue &value : values) {
        if (value.toString().compare(expected, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString responseServerCallType(const QString &name)
{
    if (name == ProviderServerToolName::McpListTools)
        return QStringLiteral("mcp_list_tools");
    if (name == ProviderServerToolName::Shell)
        return QStringLiteral("shell_call");
    return name + QStringLiteral("_call");
}

QString responseServerResultType(const QString &name)
{
    if (name == ProviderServerToolName::Computer)
        return QStringLiteral("computer_call_output");
    if (name == ProviderServerToolName::LocalShell)
        return QStringLiteral("local_shell_call_output");
    if (name == ProviderServerToolName::Shell)
        return QStringLiteral("shell_call_output");
    if (name == ProviderServerToolName::ApplyPatch)
        return QStringLiteral("apply_patch_call_output");
    // OpenAI 托管工具的结果包含在其 call item 中；回放仍使用原 call type。
    return responseServerCallType(name);
}

QString responseServerResultPayloadKey(const QString &name)
{
    if (name == ProviderServerToolName::WebSearch) return QStringLiteral("action");
    if (name == ProviderServerToolName::FileSearch) return QStringLiteral("results");
    if (name == ProviderServerToolName::CodeInterpreter) return QStringLiteral("outputs");
    if (name == ProviderServerToolName::ImageGeneration) return QStringLiteral("result");
    if (name == ProviderServerToolName::McpListTools) return QStringLiteral("tools");
    return QStringLiteral("output");
}

ModelCapabilities capabilitiesFromObject(const QJsonObject &object)
{
    ModelCapabilities capabilities;
    capabilities.modelId = object.value(QStringLiteral("id")).toString().trimmed();

    const QJsonArray inputModalities = object.value(QStringLiteral("input_modalities")).toArray();
    const QJsonArray outputModalities = object.value(QStringLiteral("output_modalities")).toArray();
    const QString modelId = capabilities.modelId;

    capabilities.set(ProviderCapability::TextInput,
                     inputModalities.isEmpty() || arrayContains(inputModalities, QStringLiteral("text")));
    capabilities.set(ProviderCapability::ImageInput,
                     inputModalities.isEmpty() || arrayContains(inputModalities, QStringLiteral("image")));
    capabilities.set(ProviderCapability::AudioInput,
                     arrayContains(inputModalities, QStringLiteral("audio")));
    capabilities.set(ProviderCapability::DocumentInput,
                     inputModalities.isEmpty() || arrayContains(inputModalities, QStringLiteral("file")));
    capabilities.set(ProviderCapability::TextOutput,
                     outputModalities.isEmpty() || arrayContains(outputModalities, QStringLiteral("text")));
    capabilities.set(ProviderCapability::ImageOutput,
                     outputModalities.isEmpty() || arrayContains(outputModalities, QStringLiteral("image")));
    capabilities.set(ProviderCapability::AudioOutput,
                     arrayContains(outputModalities, QStringLiteral("audio")));
    capabilities.set(ProviderCapability::ToolCalling,
                     !modelId.contains(QStringLiteral("image"), Qt::CaseInsensitive));
    if (object.contains(QStringLiteral("supports_function_calling"))) {
        capabilities.set(ProviderCapability::ToolCalling,
                         object.value(QStringLiteral("supports_function_calling")).toBool());
    }
    capabilities.set(ProviderCapability::Reasoning, capabilities.supportsTextOutput());
    capabilities.set(ProviderCapability::ToolChoice, capabilities.supportsToolCalling());
    capabilities.set(ProviderCapability::MaxOutputTokens,
                     capabilities.supportsTextOutput() || capabilities.supportsImageOutput());
    if (object.contains(QStringLiteral("supports_reasoning"))) {
        capabilities.set(ProviderCapability::Reasoning,
                         object.value(QStringLiteral("supports_reasoning")).toBool());
    }
    if (object.contains(QStringLiteral("supports_tool_choice"))) {
        capabilities.set(ProviderCapability::ToolChoice,
                         object.value(QStringLiteral("supports_tool_choice")).toBool());
    }
    if (object.contains(QStringLiteral("supports_max_output_tokens"))) {
        capabilities.set(ProviderCapability::MaxOutputTokens,
                         object.value(QStringLiteral("supports_max_output_tokens")).toBool());
    }
    capabilities.enable(ProviderCapability::ResponseFormat)
        .enable(ProviderCapability::SamplingTopP)
        .enable(ProviderCapability::BackgroundExecution)
        .enable(ProviderCapability::ResponseInclude);
    capabilities.set(ProviderCapability::ServerTools,
                     capabilities.supportsToolCalling());
    capabilities.set(ProviderCapability::ProgrammaticToolCalling,
                     capabilities.supportsToolCalling());
    capabilities.set(ProviderCapability::ToolSearch,
                     capabilities.supportsToolCalling());
    capabilities.set(ProviderCapability::McpApproval,
                     capabilities.supportsToolCalling());
    capabilities.enable(ProviderCapability::Compaction);
    capabilities.supportedServerTools = {
        ProviderServerToolName::WebSearch,
        ProviderServerToolName::FileSearch,
        ProviderServerToolName::CodeInterpreter,
        ProviderServerToolName::Computer,
        ProviderServerToolName::ImageGeneration,
        ProviderServerToolName::Mcp,
        ProviderServerToolName::McpListTools,
        ProviderServerToolName::ToolSearch,
        ProviderServerToolName::LocalShell,
        ProviderServerToolName::Shell,
        ProviderServerToolName::ApplyPatch};
    return capabilities;
}

bool requestHasTextInput(const ProviderRequest &request)
{
    for (const ProviderItem &item : request.items) {
        if (item.kind == ProviderItemKind::FunctionCallOutput)
            return true;
        for (const ProviderMessagePart &part : item.parts)
            if (part.kind == ProviderPartKind::Text)
                return true;
    }
    return false;
}

QJsonObject buildMessageItem(const QString &role, const QList<ProviderMessagePart> &parts)
{
    QJsonArray content;
    const bool isAssistant = role.compare(QStringLiteral("assistant"), Qt::CaseInsensitive) == 0;
    for (const ProviderMessagePart &part : parts) {
        QJsonObject item;
        if (part.kind == ProviderPartKind::Image) {
            item.insert(QStringLiteral("type"), QStringLiteral("input_image"));
            if (part.image.hasUri()) {
                item.insert(QStringLiteral("image_url"), part.image.uri);
            } else if (part.image.hasInlineData()) {
                const QString mime = part.image.mimeType.isEmpty()
                    ? QStringLiteral("image/png") : part.image.mimeType;
                item.insert(QStringLiteral("image_url"),
                    QStringLiteral("data:%1;base64,").arg(mime)
                    + QString::fromLatin1(part.image.data.toBase64()));
            }
        } else if (part.kind == ProviderPartKind::Audio) {
            item.insert(QStringLiteral("type"), QStringLiteral("input_audio"));
            QJsonObject audio;
            audio.insert(QStringLiteral("data"),
                         QString::fromLatin1(part.audio.data.toBase64()));
            audio.insert(QStringLiteral("format"),
                         part.audio.mimeType.section(QLatin1Char('/'), -1));
            if (part.audio.hasUri())
                audio.insert(QStringLiteral("url"), part.audio.uri);
            item.insert(QStringLiteral("input_audio"), audio);
        } else if (part.kind == ProviderPartKind::Document) {
            item.insert(QStringLiteral("type"), QStringLiteral("input_file"));
            if (part.document.hasUri())
                item.insert(QStringLiteral("file_url"), part.document.uri);
            else
                item.insert(QStringLiteral("file_data"),
                            QString::fromLatin1(part.document.data.toBase64()));
            if (!part.document.title.isEmpty())
                item.insert(QStringLiteral("filename"), part.document.title);
        } else if (part.kind == ProviderPartKind::Text) {
            item.insert(QStringLiteral("type"),
                        isAssistant ? QStringLiteral("output_text")
                                    : QStringLiteral("input_text"));
            item.insert(QStringLiteral("text"), part.text);
        } else {
            continue;
        }
        content.append(item);
    }

    QJsonObject item;
    item.insert(QStringLiteral("type"), QStringLiteral("message"));
    item.insert(QStringLiteral("role"), role);
    item.insert(QStringLiteral("content"), content);
    return item;
}

QJsonArray buildItemsInput(const QList<ProviderItem> &items)
{
    QJsonArray input;
    for (qsizetype itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
        const ProviderItem &providerItem = items.at(itemIndex);
        QJsonObject item;
        switch (providerItem.kind) {
        case ProviderItemKind::UserMessage:
            input.append(buildMessageItem(QStringLiteral("user"), providerItem.parts));
            break;
        case ProviderItemKind::AssistantMessage:
            input.append(buildMessageItem(QStringLiteral("assistant"), providerItem.parts));
            break;
        case ProviderItemKind::FunctionCall:
            item.insert(QStringLiteral("type"), QStringLiteral("function_call"));
            item.insert(QStringLiteral("call_id"), providerItem.callId);
            item.insert(QStringLiteral("name"), providerItem.name);
            item.insert(QStringLiteral("arguments"), providerItem.rawArguments);
            if (providerItem.callerKind != ProviderCallerKind::Unset)
                item.insert(QStringLiteral("caller"),
                            QJsonObject{{QStringLiteral("type"),
                                         toString(providerItem.callerKind)},
                                        {QStringLiteral("id"), providerItem.callerId}});
            input.append(item);
            break;
        case ProviderItemKind::FunctionCallOutput:
            item.insert(QStringLiteral("type"), QStringLiteral("function_call_output"));
            item.insert(QStringLiteral("call_id"), providerItem.callId);
            item.insert(QStringLiteral("output"), providerItem.output);
            if (providerItem.callerKind != ProviderCallerKind::Unset)
                item.insert(QStringLiteral("caller"),
                            QJsonObject{{QStringLiteral("type"),
                                         toString(providerItem.callerKind)},
                                        {QStringLiteral("id"), providerItem.callerId}});
            input.append(item);
            break;
        case ProviderItemKind::Reasoning: {
            // 编码侧闸：双空且非 redacted 的 reasoning 不得出站
            // （与 ProviderRunLedger::buildRequest / IR validate 一致）。
            if (!providerItem.reasoningRedacted
                && providerItem.reasoningText.trimmed().isEmpty()
                && providerItem.reasoningSignature.trimmed().isEmpty()) {
                break;
            }
            item.insert(QStringLiteral("type"), QStringLiteral("reasoning"));
            if (!providerItem.reasoningText.isEmpty())
                item.insert(QStringLiteral("summary"), QJsonArray{
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("summary_text")},
                                {QStringLiteral("text"), providerItem.reasoningText}}});
            if (!providerItem.reasoningSignature.isEmpty())
                item.insert(QStringLiteral("encrypted_content"), providerItem.reasoningSignature);
            input.append(item);
            break;
        }
        case ProviderItemKind::ServerToolCall:
            item.insert(QStringLiteral("type"), responseServerCallType(providerItem.name));
            item.insert(QStringLiteral("id"), providerItem.itemId);
            item.insert(QStringLiteral("call_id"), providerItem.callId);
            item.insert(QStringLiteral("arguments"), providerItem.arguments);
            if (!providerItem.reasoningSignature.isEmpty())
                item.insert(QStringLiteral("encrypted_content"),
                            providerItem.reasoningSignature);
            if (providerItem.callerKind != ProviderCallerKind::Unset)
                item.insert(QStringLiteral("caller"),
                            QJsonObject{{QStringLiteral("type"),
                                         toString(providerItem.callerKind)},
                                        {QStringLiteral("id"), providerItem.callerId}});
            if (itemIndex + 1 < items.size()) {
                const ProviderItem &next = items.at(itemIndex + 1);
                if (next.kind == ProviderItemKind::ServerToolResult
                    && next.callId == providerItem.callId
                    && responseServerResultType(next.name)
                           == responseServerCallType(providerItem.name)) {
                    const QString payloadKey =
                        responseServerResultPayloadKey(providerItem.name);
                    QJsonParseError error;
                    const QJsonDocument parsed =
                        QJsonDocument::fromJson(next.output.toUtf8(), &error);
                    if (error.error == QJsonParseError::NoError && parsed.isObject())
                        item.insert(payloadKey, parsed.object());
                    else if (error.error == QJsonParseError::NoError && parsed.isArray())
                        item.insert(payloadKey, parsed.array());
                    else
                        item.insert(payloadKey, next.output);
                    item.insert(QStringLiteral("status"),
                                next.isError ? QStringLiteral("failed")
                                             : QStringLiteral("completed"));
                    ++itemIndex;
                }
            }
            input.append(item);
            break;
        case ProviderItemKind::ServerToolResult:
            item.insert(QStringLiteral("type"), responseServerResultType(providerItem.name));
            item.insert(QStringLiteral("call_id"), providerItem.callId);
            item.insert(QStringLiteral("output"), providerItem.output);
            if (!providerItem.details.isEmpty())
                item.insert(QStringLiteral("details"), providerItem.details);
            input.append(item);
            break;
        case ProviderItemKind::Program:
            item.insert(QStringLiteral("type"), QStringLiteral("program"));
            item.insert(QStringLiteral("id"), providerItem.callId);
            item.insert(QStringLiteral("code"), providerItem.programCode);
            item.insert(QStringLiteral("fingerprint"), providerItem.programFingerprint);
            input.append(item);
            break;
        case ProviderItemKind::ProgramOutput:
            item.insert(QStringLiteral("type"), QStringLiteral("program_output"));
            item.insert(QStringLiteral("call_id"), providerItem.callId);
            item.insert(QStringLiteral("output"), providerItem.output);
            if (providerItem.status != ProviderItemStatus::Unset)
                item.insert(QStringLiteral("status"), toString(providerItem.status));
            input.append(item);
            break;
        case ProviderItemKind::ApprovalRequest:
            item.insert(QStringLiteral("type"), QStringLiteral("mcp_approval_request"));
            item.insert(QStringLiteral("id"), providerItem.callId);
            item.insert(QStringLiteral("name"), providerItem.name);
            item.insert(QStringLiteral("arguments"), providerItem.rawArguments);
            item.insert(QStringLiteral("server_label"), providerItem.serverLabel);
            input.append(item);
            break;
        case ProviderItemKind::ApprovalResponse:
            item.insert(QStringLiteral("type"), QStringLiteral("mcp_approval_response"));
            item.insert(QStringLiteral("id"), providerItem.callId);
            item.insert(QStringLiteral("approval_request_id"),
                        providerItem.approvalRequestId);
            item.insert(QStringLiteral("approve"), providerItem.approved);
            if (!providerItem.approvalReason.isEmpty())
                item.insert(QStringLiteral("reason"), providerItem.approvalReason);
            input.append(item);
            break;
        case ProviderItemKind::Compaction:
            item.insert(QStringLiteral("type"), QStringLiteral("compaction"));
            item.insert(QStringLiteral("id"), providerItem.itemId);
            item.insert(QStringLiteral("encrypted_content"),
                        providerItem.compactionSummary);
            input.append(item);
            break;
        }
    }
    return input;
}

ToolCall toolCallFromItem(const QJsonObject &item)
{
    ToolCall toolCall;
    toolCall.id = item.value(QStringLiteral("call_id")).toString();
    toolCall.toolName = item.value(QStringLiteral("name")).toString();
    toolCall.rawInputJson = item.value(QStringLiteral("arguments")).toString();
    if (toolCall.rawInputJson.isEmpty())
        toolCall.rawInputJson = item.value(QStringLiteral("input")).toString();

    const QJsonDocument inputDocument = QJsonDocument::fromJson(toolCall.rawInputJson.toUtf8());
    if (inputDocument.isObject()) {
        toolCall.input = inputDocument.object();
    }

    return toolCall;
}

QString serverToolNameForResponseType(const QString &type)
{
    if (type.startsWith(QStringLiteral("web_search")))
        return ProviderServerToolName::WebSearch;
    if (type.startsWith(QStringLiteral("file_search")))
        return ProviderServerToolName::FileSearch;
    if (type.startsWith(QStringLiteral("code_interpreter")))
        return ProviderServerToolName::CodeInterpreter;
    if (type.startsWith(QStringLiteral("computer")))
        return ProviderServerToolName::Computer;
    if (type.startsWith(QStringLiteral("image_generation")))
        return ProviderServerToolName::ImageGeneration;
    if (type.startsWith(QStringLiteral("mcp_list_tools")))
        return ProviderServerToolName::McpListTools;
    if (type.startsWith(QStringLiteral("mcp_")))
        return ProviderServerToolName::Mcp;
    if (type.startsWith(QStringLiteral("tool_search")))
        return ProviderServerToolName::ToolSearch;
    if (type.startsWith(QStringLiteral("local_shell")))
        return ProviderServerToolName::LocalShell;
    if (type.startsWith(QStringLiteral("function_shell"))
        || type.startsWith(QStringLiteral("shell")))
        return ProviderServerToolName::Shell;
    if (type.startsWith(QStringLiteral("apply_patch")))
        return ProviderServerToolName::ApplyPatch;
    qWarning().noquote()
        << QStringLiteral("Responses adapter 丢弃未知 output item type：%1").arg(type);
    return {};
}

QList<ProviderItem> providerItemsFromResponseItem(const QJsonObject &item)
{
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("message")) {
        QList<ProviderMessagePart> parts;
        const QString role = item.value(QStringLiteral("role")).toString();
        const QJsonArray content = item.value(QStringLiteral("content")).toArray();
        for (const QJsonValue &contentValue : content) {
            const QJsonObject contentItem = contentValue.toObject();
            const QString contentType = contentItem.value(QStringLiteral("type")).toString();
            if (contentType == QStringLiteral("output_text")
                || contentType == QStringLiteral("input_text")
                || contentType == QStringLiteral("text")) {
                ProviderMessagePart part = ProviderMessagePart::makeText(
                    contentItem.value(QStringLiteral("text")).toString());
                for (const QJsonValue &annotationValue :
                     contentItem.value(QStringLiteral("annotations")).toArray()) {
                    const QJsonObject annotation = annotationValue.toObject();
                    const QString annotationType =
                        annotation.value(QStringLiteral("type")).toString();
                    if (annotationType.contains(QStringLiteral("citation"))) {
                        part.citations.append(ProviderCitation{
                            annotation.value(QStringLiteral("url")).toString(),
                            annotation.value(QStringLiteral("title")).toString(
                                annotation.value(QStringLiteral("filename")).toString()),
                            annotation.value(QStringLiteral("text")).toString(),
                            annotation.value(QStringLiteral("start_index")).toInt(-1),
                            annotation.value(QStringLiteral("end_index")).toInt(-1)});
                    }
                }
                parts.append(part);
            } else if (contentType.contains(QStringLiteral("image"))) {
                ProviderImageAsset image;
                image.uri = contentItem.value(QStringLiteral("image_url")).toString();
                image.mimeType = contentItem.value(QStringLiteral("mime_type")).toString();
                image.altText = contentItem.value(QStringLiteral("alt_text")).toString();
                parts.append(ProviderMessagePart::makeImage(image));
            } else if (contentType.contains(QStringLiteral("audio"))) {
                ProviderAudioAsset audio;
                audio.uri = contentItem.value(QStringLiteral("audio_url")).toString();
                audio.data = QByteArray::fromBase64(
                    contentItem.value(QStringLiteral("data")).toString().toLatin1());
                audio.mimeType = contentItem.value(QStringLiteral("mime_type")).toString();
                audio.transcript = contentItem.value(QStringLiteral("transcript")).toString();
                parts.append(ProviderMessagePart::makeAudio(audio));
            } else if (contentType == QStringLiteral("input_file")
                       || contentType == QStringLiteral("file")) {
                ProviderDocumentAsset document;
                document.uri = contentItem.value(QStringLiteral("file_url")).toString(
                    contentItem.value(QStringLiteral("file_id")).toString());
                document.data = QByteArray::fromBase64(
                    contentItem.value(QStringLiteral("file_data")).toString().toLatin1());
                document.title = contentItem.value(QStringLiteral("filename")).toString();
                parts.append(ProviderMessagePart::makeDocument(document));
            }
        }

        ProviderItem result = role == QStringLiteral("user")
            ? ProviderItem::makeUserMessage(parts)
            : ProviderItem::makeAssistantMessage(parts);
        result.itemId = item.value(QStringLiteral("id")).toString(result.itemId);
        result.assistantPhase = item.value(QStringLiteral("phase")).toString();
        return {result};
    }

    if (type == QStringLiteral("function_call")
        || type == QStringLiteral("custom_tool_call")) {
        const ToolCall toolCall = toolCallFromItem(item);
        ProviderItem result = ProviderItem::makeFunctionCall(
            toolCall.id, toolCall.toolName, toolCall.input, toolCall.rawInputJson);
        result.itemId = item.value(QStringLiteral("id")).toString(result.itemId);
        const QJsonObject caller = item.value(QStringLiteral("caller")).toObject();
        result.callerKind = parseCallerKind(caller.value(QStringLiteral("type")).toString());
        result.callerId = caller.value(QStringLiteral("id")).toString();
        result.serverLabel = item.value(QStringLiteral("server_label")).toString();
        return {result};
    }

    if (type == QStringLiteral("function_call_output")
        || type == QStringLiteral("custom_tool_call_output")) {
        ProviderItem result = ProviderItem::makeFunctionCallOutput(
            item.value(QStringLiteral("call_id")).toString(),
            item.value(QStringLiteral("name")).toString(),
            compactJson(item.value(QStringLiteral("output"))),
            item.value(QStringLiteral("is_error")).toBool(false));
        const QJsonObject caller = item.value(QStringLiteral("caller")).toObject();
        result.callerKind = parseCallerKind(caller.value(QStringLiteral("type")).toString());
        result.callerId = caller.value(QStringLiteral("id")).toString();
        result.serverLabel = item.value(QStringLiteral("server_label")).toString();
        return {result};
    }

    if (type.startsWith(QStringLiteral("reasoning"))) {
        QStringList texts;
        for (const QJsonValue &value : item.value(QStringLiteral("summary")).toArray()) {
            const QString text = value.toObject().value(QStringLiteral("text")).toString();
            if (!text.isEmpty())
                texts.append(text);
        }
        const QString directText = item.value(QStringLiteral("text")).toString();
        if (!directText.isEmpty())
            texts.append(directText);
        const QString text = texts.join(QStringLiteral("\n"));
        ProviderItem result = ProviderItem::makeReasoning(
            text, item.value(QStringLiteral("encrypted_content")).toString(),
            item.value(QStringLiteral("encrypted_content")).toString().size() > 0, true);
        result.itemId = item.value(QStringLiteral("id")).toString(result.itemId);
        return {result};
    }

    if (type == QStringLiteral("program")) {
        ProviderItem result = ProviderItem::makeProgram(
            item.value(QStringLiteral("id")).toString(),
            item.value(QStringLiteral("code")).toString(),
            item.value(QStringLiteral("fingerprint")).toString());
        result.itemId = item.value(QStringLiteral("id")).toString(result.itemId);
        return {result};
    }
    if (type == QStringLiteral("program_output")) {
        return {ProviderItem::makeProgramOutput(
            item.value(QStringLiteral("call_id")).toString(),
            compactJson(item.value(QStringLiteral("output"))),
            item.value(QStringLiteral("status")).toString())};
    }
    if (type == QStringLiteral("mcp_approval_request")) {
        ProviderItem result = ProviderItem::makeApprovalRequest(
            item.value(QStringLiteral("id")).toString(),
            item.value(QStringLiteral("name")).toString(),
            item.value(QStringLiteral("arguments")).toString(),
            item.value(QStringLiteral("server_label")).toString());
        result.approvalReason = item.value(QStringLiteral("reason")).toString();
        return {result};
    }
    if (type == QStringLiteral("mcp_approval_response")) {
        return {ProviderItem::makeApprovalResponse(
            item.value(QStringLiteral("id")).toString(),
            item.value(QStringLiteral("approval_request_id")).toString(),
            item.value(QStringLiteral("approve")).toBool(),
            item.value(QStringLiteral("reason")).toString())};
    }
    if (type == QStringLiteral("compaction")) {
        return {ProviderItem::makeCompaction(
            item.value(QStringLiteral("encrypted_content")).toString(),
            item.value(QStringLiteral("id")).toString())};
    }

    const QString serverToolName = serverToolNameForResponseType(type);
    if (!serverToolName.isEmpty()) {
        const bool isResult = type.endsWith(QStringLiteral("_output"))
            || type.endsWith(QStringLiteral("_result"));
        const QString callId = isResult
            ? item.value(QStringLiteral("call_id")).toString()
            : item.value(QStringLiteral("call_id")).toString(
                  item.value(QStringLiteral("id")).toString());
        if (isResult) {
            return {ProviderItem::makeServerToolResult(
                callId, serverToolName,
                compactJson(item.contains(QStringLiteral("output"))
                                ? item.value(QStringLiteral("output"))
                                : item.value(QStringLiteral("result"))),
                {}, item.value(QStringLiteral("is_error")).toBool(false))};
        }
        QJsonObject arguments = item.value(QStringLiteral("arguments")).toObject();
        if (arguments.isEmpty() && item.contains(QStringLiteral("action")))
            arguments.insert(QStringLiteral("action"), item.value(QStringLiteral("action")));
        ProviderItem result = ProviderItem::makeServerToolCall(
            callId, serverToolName, arguments,
            compactJson(item.value(QStringLiteral("arguments"))));
        result.itemId = item.value(QStringLiteral("id")).toString(result.itemId);
        result.reasoningSignature =
            item.value(QStringLiteral("encrypted_content")).toString();
        result.reasoningMustReplay = !result.reasoningSignature.isEmpty();
        const QJsonObject caller = item.value(QStringLiteral("caller")).toObject();
        result.callerKind = parseCallerKind(caller.value(QStringLiteral("type")).toString());
        result.callerId = caller.value(QStringLiteral("id")).toString();
        result.serverLabel = item.value(QStringLiteral("server_label")).toString();
        QList<ProviderItem> mapped{result};
        const QString payloadKey = responseServerResultPayloadKey(serverToolName);
        if (item.contains(payloadKey)
            || item.value(QStringLiteral("status")).toString()
                   == QStringLiteral("completed")) {
            const QJsonValue payload = item.contains(payloadKey)
                ? item.value(payloadKey) : item.value(QStringLiteral("status"));
            mapped.append(ProviderItem::makeServerToolResult(
                callId, serverToolName, compactJson(payload), {},
                item.value(QStringLiteral("status")).toString()
                    == QStringLiteral("failed")));
        }
        return mapped;
    }

    return {};
}

QList<ProviderItem> outputItemsFromResponse(const QJsonArray &output)
{
    QList<ProviderItem> items;
    for (const QJsonValue &outputValue : output) {
        items.append(providerItemsFromResponseItem(outputValue.toObject()));
    }
    return items;
}

QStringList reasoningTextsFromItem(const QJsonObject &item)
{
    QStringList texts;
    const QJsonArray summary = item.value(QStringLiteral("summary")).toArray();
    for (const QJsonValue &summaryValue : summary) {
        const QString text = summaryValue.toObject().value(QStringLiteral("text")).toString();
        if (!text.isEmpty()) {
            texts.append(text);
        }
    }

    const QString directText = item.value(QStringLiteral("text")).toString();
    if (!directText.isEmpty()) {
        texts.append(directText);
    }

    return texts;
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

ProviderImageOutput imageOutputFromContentItem(const QString &messageId,
                                               const int partIndex,
                                               const QJsonObject &contentItem)
{
    ProviderImageOutput imageOutput;
    imageOutput.base.messageId = messageId;
    imageOutput.base.partIndex = partIndex;
    imageOutput.image.uri = contentItem.value(QStringLiteral("image_url")).toString();
    imageOutput.image.mimeType = contentItem.value(QStringLiteral("mime_type")).toString();
    imageOutput.image.altText = contentItem.value(QStringLiteral("alt_text")).toString();
    const QString imageBase64 = contentItem.value(QStringLiteral("image_base64")).toString();
    if (!imageBase64.isEmpty()) {
        imageOutput.image.data = QByteArray::fromBase64(imageBase64.toLatin1());
    }
    return imageOutput;
}

} // namespace

// ---- ResponsesProvider ----

ResponsesProvider::ResponsesProvider(QObject *parent)
    : AbstractProvider(QStringLiteral("responses"), parent)
{
}

ResponsesProvider::~ResponsesProvider() = default;

QUrl ResponsesProvider::buildModelsUrl(const QString &baseUrl) const
{
    QUrl url;
    if (baseUrl.endsWith(QStringLiteral("/models")) || baseUrl.contains(QStringLiteral("/models?"))) {
        url = QUrl(baseUrl);
    } else if (baseUrl.endsWith(QStringLiteral("/v1"))) {
        url = QUrl(baseUrl + QStringLiteral("/models"));
    } else if (baseUrl.endsWith(QLatin1Char('/'))) {
        url = QUrl(baseUrl + QStringLiteral("v1/models"));
    } else {
        url = QUrl(baseUrl + QStringLiteral("/v1/models"));
    }
    return url;
}


QList<ModelCapabilities> ResponsesProvider::parseModelsPayload(const QByteArray &body,
                                                            QString *errorMessage) const
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Responses 模型列表不是有效 JSON。");
        }
        return {};
    }

    QList<ModelCapabilities> models;
    const QJsonArray data = document.object().value(QStringLiteral("data")).toArray();
    for (const QJsonValue &value : data) {
        if (!value.isObject()) {
            continue;
        }
        const ModelCapabilities capabilities = capabilitiesFromObject(value.toObject());
        if (!capabilities.modelId.isEmpty()) {
            models.append(capabilities);
        }
    }

    if (models.isEmpty() && errorMessage) {
        *errorMessage = QStringLiteral("Responses 模型列表为空。");
    }
    return models;
}

ProviderError ResponsesProvider::validateProviderRequest(const ProviderRequest &request) const
{
    if (request.protocolFamily != ProviderProtocolFamily::Auto
        && request.protocolFamily != ProviderProtocolFamily::OpenAiResponses) {
        return ProviderError{QStringLiteral("protocol_family_mismatch"),
                             QStringLiteral("Responses 适配器不能处理指定的协议族。")};
    }
    const QString modelName = m_auth.modelName;
    if (request.hasVideoInput()) {
        return ProviderError{QStringLiteral("video_input_not_supported"),
                             QStringLiteral("Responses API 不支持视频输入。")};
    }
    if (request.sampling.topK >= 0 || request.sampling.seed >= 0
        || !request.sampling.stop.isEmpty()
        || request.sampling.penaltiesRequested) {
        return ProviderError{QStringLiteral("sampling_option_not_supported"),
                             QStringLiteral("Responses 适配器不支持 top_k、seed、stop 或 penalties。")};
    }
    if (request.reasoning.budgetTokens > 0) {
        return ProviderError{QStringLiteral("reasoning_budget_not_supported"),
                             QStringLiteral("Responses API 不支持 reasoning token budget。")};
    }
    if (!request.providerCachedContentId.isEmpty()
        || !request.mediaResolution.isEmpty()) {
        return ProviderError{
            QStringLiteral("request_option_not_supported"),
            QStringLiteral("Responses API 不支持 Gemini cachedContent 或请求级 mediaResolution。")};
    }
    // 如果模型列表尚未加载（如 AutoRename 等轻量场景），跳过能力校验，由 API 报错兜底
    if (availableModels().isEmpty()) {
        return {};
    }
    const std::optional<ModelCapabilities> capabilities = capabilitiesForModel(modelName);
    if (!capabilities.has_value()) {
        return ProviderError{QStringLiteral("model_not_found"),
                             QStringLiteral("未找到模型 %1。").arg(modelName)};
    }
    QString capabilityError;
    if (!request.validate(&capabilityError, &capabilities.value(), -1)) {
        return ProviderError{QStringLiteral("provider_capability_mismatch"),
                             capabilityError};
    }

    if (requestHasTextInput(request) && !capabilities->supportsTextInput()) {
        return ProviderError{QStringLiteral("text_input_not_supported"),
                             QStringLiteral("模型 %1 不支持文本输入。").arg(modelName)};
    }

    if (request.desiredOutput.textEnabled && !capabilities->supportsTextOutput()) {
        return ProviderError{QStringLiteral("text_output_not_supported"),
                             QStringLiteral("模型 %1 不支持文本输出。").arg(modelName)};
    }
    if (request.desiredOutput.imageEnabled && !capabilities->supportsImageOutput()) {
        return ProviderError{QStringLiteral("image_output_not_supported"),
                             QStringLiteral("模型 %1 不支持图片输出。").arg(modelName)};
    }
    if (request.desiredOutput.audioEnabled && !capabilities->supportsAudioOutput()) {
        return ProviderError{QStringLiteral("audio_output_not_supported"),
                             QStringLiteral("模型 %1 不支持音频输出。").arg(modelName)};
    }

    if (!request.tools.isEmpty() && !capabilities->supportsToolCalling()) {
        return ProviderError{QStringLiteral("tool_calling_not_supported"),
                             QStringLiteral("模型 %1 不支持工具调用。").arg(modelName)};
    }

    if (request.toolChoice.isExplicit() && !capabilities->supportsToolChoice()) {
        return ProviderError{QStringLiteral("tool_choice_not_supported"),
                             QStringLiteral("模型 %1 不支持 tool choice。").arg(modelName)};
    }

    if (request.toolChoice.mode == ProviderToolChoiceMode::Named
        && request.toolChoice.toolName.trimmed().isEmpty()) {
        return ProviderError{QStringLiteral("tool_choice_invalid"),
                             QStringLiteral("Named tool choice 需要指定工具名。")};
    }

    if (request.toolChoice.isExplicit() && request.tools.isEmpty()) {
        return ProviderError{QStringLiteral("tool_choice_requires_tools"),
                             QStringLiteral("设置 tool choice 时必须同时提供 tools。")};
    }

    if (request.maxOutputTokens > 0 && !capabilities->supportsMaxOutputTokens()) {
        return ProviderError{QStringLiteral("max_output_tokens_not_supported"),
                             QStringLiteral("模型 %1 不支持 max output tokens。").arg(modelName)};
    }

    if (request.reasoning.enabled && !capabilities->supportsReasoning()) {
        return ProviderError{QStringLiteral("reasoning_not_supported"),
                             QStringLiteral("模型 %1 不支持 reasoning。").arg(modelName)};
    }

    return ProviderError{};
}

ProviderTransportRequest ResponsesProvider::buildProviderTransportRequest(const ProviderRequest &request) const
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), m_auth.modelName);
    body.insert(QStringLiteral("stream"), request.stream);
    if (request.storeServerState != ProviderTriState::Unset)
        body.insert(QStringLiteral("store"),
                    request.storeServerState == ProviderTriState::Yes);
    // 仅显式 continuationId 时发 previous_response_id；默认空 = 全量 input 回放。
    if (!request.continuationId.isEmpty())
        body.insert(QStringLiteral("previous_response_id"), request.continuationId);

    if (!request.conversationId.isEmpty()) {
        body.insert(QStringLiteral("prompt_cache_key"), request.conversationId);
    }
    if (!request.providerConversationId.isEmpty())
        body.insert(QStringLiteral("conversation"), request.providerConversationId);
    if (!request.metadata.isEmpty())
        body.insert(QStringLiteral("metadata"),
                    QJsonObject::fromVariantMap(request.metadata));

    const QString instructions = request.systemPrompt.trimmed();
    if (!instructions.isEmpty()) {
        body.insert(QStringLiteral("instructions"), instructions);
    }

    QJsonArray modalities;
    if (request.desiredOutput.imageEnabled) {
        if (request.desiredOutput.textEnabled) {
            modalities.append(QStringLiteral("text"));
        }
        modalities.append(QStringLiteral("image"));
    }
    if (request.desiredOutput.audioEnabled)
        modalities.append(QStringLiteral("audio"));
    if (!modalities.isEmpty()) {
        body.insert(QStringLiteral("modalities"), modalities);
    }
    if (request.desiredOutput.audioEnabled || request.audio.isExplicit()) {
        QJsonObject audio;
        if (!request.audio.voice.isEmpty())
            audio.insert(QStringLiteral("voice"), request.audio.voice);
        if (!request.audio.outputFormat.isEmpty())
            audio.insert(QStringLiteral("format"), request.audio.outputFormat);
        if (request.audio.requestTranscript)
            audio.insert(QStringLiteral("transcript"), true);
        body.insert(QStringLiteral("audio"), audio);
    }

    if (request.maxOutputTokens > 0) {
        body.insert(QStringLiteral("max_output_tokens"), request.maxOutputTokens);
    }


    const QJsonValue toolChoice = toolChoiceJsonValue(request.toolChoice);
    if (!toolChoice.isUndefined()) {
        body.insert(QStringLiteral("tool_choice"), toolChoice);
    }

    if (request.reasoning.enabled) {
        QJsonObject reasoning;
        if (request.reasoning.includeSummary)
            reasoning.insert(QStringLiteral("summary"), QStringLiteral("auto"));
        const QString effort = toString(request.reasoning.effort);
        if (!effort.isEmpty())
            reasoning.insert(QStringLiteral("effort"), effort);
        body.insert(QStringLiteral("reasoning"), reasoning);
    }
    if (!request.responseInclude.isEmpty())
        body.insert(QStringLiteral("include"), QJsonArray::fromStringList(request.responseInclude));
    if (request.backgroundExecution != ProviderTriState::Unset)
        body.insert(QStringLiteral("background"),
                    request.backgroundExecution == ProviderTriState::Yes);
    if (request.temperature >= 0.0)
        body.insert(QStringLiteral("temperature"), request.temperature);
    if (request.sampling.topP >= 0.0)
        body.insert(QStringLiteral("top_p"), request.sampling.topP);
    if (request.responseFormat.kind == ProviderResponseFormatKind::JsonObject) {
        body.insert(QStringLiteral("text"),
                    QJsonObject{{QStringLiteral("format"),
                                 QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}}}});
    } else if (request.responseFormat.kind == ProviderResponseFormatKind::JsonSchema) {
        body.insert(QStringLiteral("text"),
                    QJsonObject{{QStringLiteral("format"),
                                 QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                                             {QStringLiteral("name"), request.responseFormat.schemaName},
                                             {QStringLiteral("schema"), request.responseFormat.jsonSchema}}}});
    }

    const QList<ProviderItem> encodedItems =
        !request.continuationId.isEmpty() && !request.items.isEmpty()
            ? QList<ProviderItem>{request.items.last()} : request.items;
    QJsonArray input = buildItemsInput(encodedItems);
    if (!input.isEmpty()) {
        body.insert(QStringLiteral("input"), input);
    }

    if (!request.tools.isEmpty()) {
        QJsonArray tools;
        for (const ProviderToolSpecification &tool : request.tools) {
            QJsonObject item;
            item.insert(QStringLiteral("type"), QStringLiteral("function"));
            item.insert(QStringLiteral("name"), tool.name);
            item.insert(QStringLiteral("description"), tool.description);
            item.insert(QStringLiteral("parameters"), tool.inputSchema);
            if (!tool.outputSchema.isEmpty())
                item.insert(QStringLiteral("output_schema"), tool.outputSchema);
            if (tool.strictSchema)
                item.insert(QStringLiteral("strict"), true);
            if (tool.deferLoading)
                item.insert(QStringLiteral("defer_loading"), true);
            if (!tool.allowedCallers.isEmpty()) {
                QJsonArray callers;
                for (const ProviderCallerKind caller : tool.allowedCallers)
                    callers.append(toString(caller));
                item.insert(QStringLiteral("allowed_callers"), callers);
            }
            tools.append(item);
        }
        body.insert(QStringLiteral("tools"), tools);
    }

    return buildStandardTransport(body, request.stream);
}

QList<ProviderEvent> ResponsesProvider::parseProviderTransportPayload(const ProviderTransportPayload &payload)
{
    const QJsonObject &document = payload.document;
    const QJsonObject errorObject = document.value(QStringLiteral("error")).toObject();
    if (!errorObject.isEmpty()) {
        return handlePayloadError(document);
    }
    // 非流式 /responses 直接返回 Response 对象，而不是 response.completed 信封。
    if (document.contains(QStringLiteral("output"))
        && document.contains(QStringLiteral("status"))) {
        const QString status = document.value(QStringLiteral("status")).toString();
        const QJsonObject envelope{{QStringLiteral("response"), document}};
        if (status == QStringLiteral("failed"))
            return handleFailed(envelope);
        if (status == QStringLiteral("incomplete"))
            return handleIncomplete(envelope);
        return handleCompleted(envelope);
    }

    static const QHash<QString, QList<ProviderEvent> (ResponsesProvider::*)(const QJsonObject &)> handlers = {
        {QStringLiteral("response.output_text.delta"), &ResponsesProvider::handleOutputTextDelta},
        {QStringLiteral("response.reasoning_summary_text.delta"), &ResponsesProvider::handleReasoningDelta},
        {QStringLiteral("response.reasoning_text.delta"), &ResponsesProvider::handleReasoningDelta},
        {QStringLiteral("response.output_item.added"), &ResponsesProvider::handleOutputItemAdded},
        {QStringLiteral("response.output_item.done"), &ResponsesProvider::handleOutputItemDone},
        {QStringLiteral("response.completed"), &ResponsesProvider::handleCompleted},
        {QStringLiteral("response.failed"), &ResponsesProvider::handleFailed},
        {QStringLiteral("response.incomplete"), &ResponsesProvider::handleIncomplete},
    };

    const QString type = document.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("response.output_audio.delta")) {
        ProviderAudioDelta delta;
        delta.base.messageId = currentMessageId();
        delta.base.partIndex = document.value(QStringLiteral("content_index")).toInt(-1);
        delta.audio.data = QByteArray::fromBase64(
            document.value(QStringLiteral("delta")).toString().toLatin1());
        delta.audio.mimeType = document.value(QStringLiteral("mime_type")).toString();
        return {ProviderEvent::fromAudioDelta(delta)};
    }
    if (type == QStringLiteral("response.output_audio_transcript.delta")) {
        ProviderTranscriptDelta delta;
        delta.base.messageId = currentMessageId();
        delta.base.partIndex = document.value(QStringLiteral("content_index")).toInt(-1);
        delta.text = document.value(QStringLiteral("delta")).toString();
        return {ProviderEvent::fromTranscriptDelta(delta)};
    }
    const auto it = handlers.constFind(type);
    if (it != handlers.constEnd()) {
        return (this->*(it.value()))(document);
    }

    return {};
}

void ResponsesProvider::resetProviderTurnState()
{
    m_activeReasoningPartIndex = -1;
}

bool ResponsesProvider::startProviderTransportRequest(const ProviderTransportRequest &request,
                                                   ProviderError *error)
{
    QString base = m_auth.baseUrl.trimmed();
    QUrl url;
    if (base.endsWith(QStringLiteral("/responses")) || base.contains(QStringLiteral("/responses?"))) {
        url = QUrl(base);
    } else if (base.endsWith(QStringLiteral("/v1"))) {
        url = QUrl(base + QStringLiteral("/responses"));
    } else if (base.endsWith(QLatin1Char('/'))) {
        url = QUrl(base + QStringLiteral("v1/responses"));
    } else {
        url = QUrl(base + QStringLiteral("/v1/responses"));
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

QList<ProviderEvent> ResponsesProvider::handlePayloadError(const QJsonObject &payload)
{
    ProviderError error;
    error.code = QStringLiteral("responses_error");
    const QJsonValue errorValue = payload.value(QStringLiteral("error"));
    const QJsonObject errorObject = errorValue.toObject();
    error.message = errorObject.value(QStringLiteral("message")).toString();
    if (error.message.isEmpty()) {
        error.message = payload.value(QStringLiteral("message")).toString();
    }
    if (error.message.isEmpty()) {
        error.message = QStringLiteral("Responses 返回了错误响应。");
    }
    error.providerRaw = payload;
    const ProviderRetry::Classification cls =
        ProviderRetry::classifyApiErrorValue(errorValue);
    error.retryable = cls.retryable;
    emitErrorOccurred(error);
    return {};
}

QList<ProviderEvent> ResponsesProvider::handleOutputTextDelta(const QJsonObject &payload)
{
    QList<ProviderEvent> events;

    ProviderTextDelta delta;
    delta.base.messageId = currentMessageId();
    delta.text = payload.value(QStringLiteral("delta")).toString();
    events.append(ProviderEvent::fromTextDelta(delta));
    return events;
}

QList<ProviderEvent> ResponsesProvider::handleReasoningDelta(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    turnState().reasoningStreamed = true;
    ProviderReasoningDelta delta;
    delta.base.messageId = currentMessageId();
    delta.text = payload.value(QStringLiteral("delta")).toString();
    events.append(ProviderEvent::fromReasoningDelta(delta));
    return events;
}


QList<ProviderEvent> ResponsesProvider::handleOutputItemAdded(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
    const QList<ProviderItem> parsed = providerItemsFromResponseItem(item);
    if (parsed.isEmpty()
        || (parsed.first().kind != ProviderItemKind::FunctionCall
            && parsed.first().kind != ProviderItemKind::ServerToolCall)) {
        return events;
    }
    const ProviderItem &toolCall = parsed.first();


    ProviderToolCallStart call;
    call.base.messageId = currentMessageId();
    call.base.partIndex = payload.value(QStringLiteral("output_index")).toInt(-1);
    call.toolCallId = toolCall.callId;
    call.toolName = toolCall.name;
    call.isServerTool = toolCall.kind == ProviderItemKind::ServerToolCall;

    events.append(ProviderEvent::toolCallStarted(call));
    return events;
}

QList<ProviderEvent> ResponsesProvider::handleOutputItemDone(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
    const QList<ProviderItem> parsed = providerItemsFromResponseItem(item);
    if (parsed.isEmpty()
        || (parsed.first().kind != ProviderItemKind::FunctionCall
            && parsed.first().kind != ProviderItemKind::ServerToolCall)) {
        return events;
    }
    const ProviderItem &toolCall = parsed.first();


    ProviderToolCallEnd callEnd;
    callEnd.base.messageId = currentMessageId();
    callEnd.base.partIndex = payload.value(QStringLiteral("output_index")).toInt(-1);
    callEnd.toolCallId = toolCall.callId;
    callEnd.toolName = toolCall.name;
    callEnd.isServerTool = toolCall.kind == ProviderItemKind::ServerToolCall;
    callEnd.arguments = toolCall.arguments;
    callEnd.parseFailed = !toolCall.rawArguments.isEmpty() && toolCall.arguments.isEmpty();
    callEnd.rawArguments = toolCall.rawArguments;

    events.append(ProviderEvent::toolCallCompleted(callEnd));
    return events;
}


QList<ProviderEvent> ResponsesProvider::handleCompleted(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const QJsonObject responseObject = payload.value(QStringLiteral("response")).toObject();
    const QJsonObject usageObject = responseObject.value(QStringLiteral("usage")).toObject();
    const QJsonArray output = responseObject.value(QStringLiteral("output")).toArray();

    if (turnState().activeRequestId.isEmpty()) {
        turnState().activeRequestId = responseObject.value(QStringLiteral("id")).toString();
    }

    ProviderResponseMetadata metadata;
    metadata.providerResponseId = currentMessageId(responseObject);
    metadata.portableUsage = usageFromJson(usageObject);
    events.append(ProviderEvent::responseMetadataUpdated(metadata));

    for (const QJsonValue &outputValue : output) {
        const QJsonObject item = outputValue.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();

        if (type.startsWith(QStringLiteral("reasoning"))) {
            // 流式已通过 reasoning_*.delta 送达时，禁止在 completed 再整段回放
            if (turnState().reasoningStreamed) {
                continue;
            }
            const QStringList reasoningTexts = reasoningTextsFromItem(item);
            if (!reasoningTexts.isEmpty()) {
                for (const QString &reasoningText : reasoningTexts) {
                    ProviderReasoningDelta delta;
                    delta.base.messageId = currentMessageId(responseObject);
                    delta.text = reasoningText;
                    events.append(ProviderEvent::fromReasoningDelta(delta));
                }
            }
            continue;
        }

        if (type == QStringLiteral("message")) {
            const QJsonArray content = item.value(QStringLiteral("content")).toArray();
            for (const QJsonValue &contentValue : content) {
                const QJsonObject contentItem = contentValue.toObject();
                const QString contentType = contentItem.value(QStringLiteral("type")).toString();

                if (contentType == QStringLiteral("output_text")) {
                    // 流式模式下文本已通过 output_text.delta 逐字符送达，
                    // 不再从 response.done 重复发射全文，避免重复追加
                    if (turnState().messageStarted) continue;

                    ProviderTextDelta delta;
                    delta.base.messageId = currentMessageId(responseObject);
                    delta.text = contentItem.value(QStringLiteral("text")).toString();
                    events.append(ProviderEvent::fromTextDelta(delta));
                    continue;
                }

                if (contentType.contains(QStringLiteral("image"))) {
                    ProviderImageOutput imageOutput =
                        imageOutputFromContentItem(currentMessageId(responseObject),
                                                   item.value(QStringLiteral("output_index")).toInt(-1),
                                                   contentItem);
                    events.append(ProviderEvent::fromImageOutput(imageOutput));
                }
            }
            continue;
        }

        if (type == QStringLiteral("function_call")
            || type == QStringLiteral("custom_tool_call")) {
            const ToolCall toolCall = toolCallFromItem(item);
            if (toolCall.id.isEmpty()) {
                continue;
            }

            // 流式 output_item.added/done 已发射过 Started/Completed 时禁止再发，
            // 否则 GUI 会对同一 toolUseId 二次 EventToolCallBegin → tools 翻倍。
            if (turnState().fallbackFunctionCallIndices.contains(toolCall.id)) {
                continue;
            }

            ProviderToolCallStart callStart;
            callStart.base.messageId = currentMessageId(responseObject);
            callStart.base.partIndex = item.value(QStringLiteral("output_index")).toInt(-1);
            callStart.toolCallId = toolCall.id;
            callStart.toolName = toolCall.toolName;
            events.append(ProviderEvent::toolCallStarted(callStart));

            ProviderToolCallEnd callEnd;
            callEnd.base.messageId = currentMessageId(responseObject);
            callEnd.base.partIndex = item.value(QStringLiteral("output_index")).toInt(-1);
            callEnd.toolCallId = toolCall.id;
            callEnd.toolName = toolCall.toolName;
            callEnd.arguments = toolCall.input;
            callEnd.parseFailed = !toolCall.rawInputJson.isEmpty() && toolCall.input.isEmpty();
            callEnd.rawArguments = toolCall.rawInputJson;
            events.append(ProviderEvent::toolCallCompleted(callEnd));
        }
    }

    const ProviderUsage usage = usageFromJson(usageObject);
    if (!usageObject.isEmpty()) {
        events.append(ProviderEvent::usageUpdated(usage));
    }

    ProviderMessageEnd messageEnd;
    messageEnd.messageId = currentMessageId(responseObject);
    messageEnd.stopReason = StopReason::EndTurn;
    messageEnd.finalUsage = usage;
    messageEnd.outputItems = completedOutputItems(responseObject);
    messageEnd.logprobs = responseObject.value(QStringLiteral("logprobs")).toObject();
    if (messageEnd.logprobs.isEmpty()) {
        QJsonArray contentLogprobs;
        for (const QJsonValue &outputValue : output) {
            for (const QJsonValue &contentValue :
                 outputValue.toObject().value(QStringLiteral("content")).toArray()) {
                const QJsonValue logprobs =
                    contentValue.toObject().value(QStringLiteral("logprobs"));
                if (logprobs.isArray())
                    for (const QJsonValue &value : logprobs.toArray())
                        contentLogprobs.append(value);
            }
        }
        if (!contentLogprobs.isEmpty())
            messageEnd.logprobs.insert(QStringLiteral("content"), contentLogprobs);
    }
    events.append(ProviderEvent::messageCompleted(messageEnd));
    return events;
}

QList<ProviderEvent> ResponsesProvider::handleFailed(const QJsonObject &payload)
{
    ProviderError error;
    error.code = QStringLiteral("responses_error");
    const QJsonObject response = payload.value(QStringLiteral("response")).toObject();
    const QJsonValue errorValue = response.value(QStringLiteral("error"));
    const QJsonObject errorObject = errorValue.toObject();
    error.message = errorObject.value(QStringLiteral("message")).toString();
    if (error.message.isEmpty()) {
        error.message = QStringLiteral("Responses 请求失败。");
    }
    error.providerRaw = payload;
    const ProviderRetry::Classification cls =
        ProviderRetry::classifyApiErrorValue(errorValue);
    error.retryable = cls.retryable;
    emitErrorOccurred(error);
    return {};
}

QList<ProviderEvent> ResponsesProvider::handleIncomplete(const QJsonObject &payload)
{
    QList<ProviderEvent> events;
    const QJsonObject response = payload.value(QStringLiteral("response")).toObject();
    const QJsonObject usageObject = response.value(QStringLiteral("usage")).toObject();
    const ProviderUsage usage = usageFromJson(usageObject);

    ProviderResponseMetadata metadata;
    metadata.providerResponseId = currentMessageId(response);
    metadata.portableUsage = usage;
    events.append(ProviderEvent::responseMetadataUpdated(metadata));
    if (!usageObject.isEmpty())
        events.append(ProviderEvent::usageUpdated(usage));

    ProviderMessageEnd end;
    end.messageId = currentMessageId(response);
    const QString incompleteReason =
        response.value(QStringLiteral("incomplete_details")).toObject()
            .value(QStringLiteral("reason")).toString();
    end.stopReason = incompleteReason.contains(QStringLiteral("token"))
        ? StopReason::MaxTokens : StopReason::Incomplete;
    end.finalUsage = usage;
    end.outputItems = completedOutputItems(response);
    events.append(ProviderEvent::messageCompleted(end));
    return events;
}

ProviderUsage ResponsesProvider::usageFromJson(const QJsonObject &usageObject) const
{
    ProviderUsage usage;
    usage.inputTokens = usageObject.value(QStringLiteral("input_tokens")).toInt();
    usage.outputTokens = usageObject.value(QStringLiteral("output_tokens")).toInt();
    usage.cacheReadTokens = usageObject.value(QStringLiteral("input_tokens_details"))
                                .toObject()
                                .value(QStringLiteral("cached_tokens"))
                                .toInt();
    usage.cacheWriteTokens = usageObject.value(QStringLiteral("output_tokens_details"))
                                 .toObject()
                                 .value(QStringLiteral("cached_tokens"))
                                 .toInt();
    usage.thoughtTokens = usageObject.value(QStringLiteral("output_tokens_details"))
                              .toObject()
                              .value(QStringLiteral("reasoning_tokens"))
                              .toInt();
    return usage;
}

QString ResponsesProvider::currentMessageId(const QJsonObject &responseObject) const
{
    const QString responseId = responseObject.value(QStringLiteral("id")).toString().trimmed();
    if (!responseId.isEmpty()) {
        return responseId;
    }
    if (!turnState().activeRequestId.trimmed().isEmpty()) {
        return turnState().activeRequestId;
    }
    return {};
}

QList<ProviderItem> ResponsesProvider::completedOutputItems(const QJsonObject &responseObject) const
{
    const QJsonArray output = responseObject.value(QStringLiteral("output")).toArray();
    return output.isEmpty() ? turnState().fallbackOutputItems
                            : outputItemsFromResponse(output);
}


std::optional<ModelCapabilities> ResponsesProvider::capabilitiesForModel(const QString &modelId) const
{
    for (const ModelCapabilities &capabilities : availableModels()) {
        if (capabilities.modelId == modelId) {
            return capabilities;
        }
    }
    return std::nullopt;
}
