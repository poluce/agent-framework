#include "ProviderItem.h"

#include <QUuid>

// ── ProviderDocumentAsset ──

ProviderDocumentAsset ProviderDocumentAsset::fromUrl(const QString &uri,
                                                     const QString &mimeType,
                                                     const QString &title)
{
    ProviderDocumentAsset asset;
    asset.uri = uri;
    asset.mimeType = mimeType;
    asset.title = title;
    return asset;
}

ProviderDocumentAsset ProviderDocumentAsset::fromBytes(const QByteArray &data,
                                                       const QString &mimeType,
                                                       const QString &title)
{
    ProviderDocumentAsset asset;
    asset.data = data;
    asset.mimeType = mimeType;
    asset.title = title;
    return asset;
}

ProviderDocumentAsset ProviderDocumentAsset::fromBlob(const ProviderBlobRef &blob,
                                                      const QString &mimeType,
                                                      const QString &title)
{
    ProviderDocumentAsset asset;
    asset.blobRef = blob;
    if (asset.blobRef.scheme == ProviderUriScheme::Unset) {
        asset.blobRef.scheme = ProviderUriScheme::Blob;
    }
    asset.mimeType = mimeType;
    asset.title = title;
    return asset;
}

bool ProviderDocumentAsset::hasUri() const
{
    return !uri.trimmed().isEmpty();
}

bool ProviderDocumentAsset::hasInlineData() const
{
    return !data.isEmpty();
}

bool ProviderDocumentAsset::hasBlobRef() const
{
    return blobRef.hasBlobId();
}

bool ProviderDocumentAsset::isEmpty() const
{
    return !hasUri() && !hasInlineData() && !hasBlobRef();
}

// ── ProviderMessagePart ──

ProviderMessagePart ProviderMessagePart::makeText(const QString &text,
                                                  const QList<ProviderCitation> &citations)
{
    ProviderMessagePart part;
    part.kind = ProviderPartKind::Text;
    part.text = text;
    part.citations = citations;
    return part;
}

ProviderMessagePart ProviderMessagePart::makeImage(const ProviderImageAsset &image)
{
    ProviderMessagePart part;
    part.kind = ProviderPartKind::Image;
    part.image = image;
    return part;
}

ProviderMessagePart ProviderMessagePart::makeAudio(const ProviderAudioAsset &audio)
{
    ProviderMessagePart part;
    part.kind = ProviderPartKind::Audio;
    part.audio = audio;
    return part;
}

ProviderMessagePart ProviderMessagePart::makeDocument(const ProviderDocumentAsset &document)
{
    ProviderMessagePart part;
    part.kind = ProviderPartKind::Document;
    part.document = document;
    return part;
}

ProviderMessagePart ProviderMessagePart::makeVideo(const ProviderVideoAsset &video)
{
    ProviderMessagePart part;
    part.kind = ProviderPartKind::Video;
    part.video = video;
    return part;
}

QString ProviderMessagePart::toDebugString() const
{
    switch (kind) {
    case ProviderPartKind::Image:
        return image.hasUri() ? QStringLiteral("image(%1)").arg(image.uri)
                              : QStringLiteral("image(<inline>)");
    case ProviderPartKind::Audio:
        if (audio.hasUri()) {
            return QStringLiteral("audio(%1)").arg(audio.uri);
        }
        if (!audio.transcript.isEmpty()) {
            return QStringLiteral("audio(<inline>, transcript=%1 chars)")
                .arg(audio.transcript.size());
        }
        return QStringLiteral("audio(<inline>, %1 bytes)").arg(audio.data.size());
    case ProviderPartKind::Document:
        if (document.hasUri()) {
            return QStringLiteral("document(%1)").arg(document.uri);
        }
        return QStringLiteral("document(<inline>, %1 bytes)").arg(document.data.size());
    case ProviderPartKind::Video:
        if (video.hasUri()) {
            return QStringLiteral("video(%1)").arg(video.uri);
        }
        return QStringLiteral("video(<inline>, %1 bytes)").arg(video.data.size());
    case ProviderPartKind::Text:
        break;
    }
    if (citations.isEmpty()) {
        return QStringLiteral("text(%1)").arg(text);
    }
    return QStringLiteral("text(%1, %2 citations)").arg(text).arg(citations.size());
}

QString joinedText(const QList<ProviderMessagePart> &parts)
{
    QString text;
    for (const ProviderMessagePart &part : parts) {
        if (part.kind == ProviderPartKind::Text)
            text += part.text;
    }
    return text;
}

// ── ProviderItem ──

namespace {

QList<ProviderMessagePart> partsWithOptionalCaption(ProviderMessagePart mediaPart,
                                                    const QString &caption)
{
    QList<ProviderMessagePart> parts;
    const QString trimmed = caption.trimmed();
    if (!trimmed.isEmpty()) {
        parts.append(ProviderMessagePart::makeText(trimmed));
    }
    parts.append(std::move(mediaPart));
    return parts;
}

/// 工厂统一写入稳定 itemId（未指定时生成 UUID）
void ensureItemId(ProviderItem *item)
{
    if (item && item->itemId.trimmed().isEmpty()) {
        item->itemId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
}

bool hasPartKind(const QList<ProviderMessagePart> &parts, ProviderPartKind kind)
{
    for (const ProviderMessagePart &part : parts) {
        if (part.kind == kind) {
            return true;
        }
    }
    return false;
}

bool requireNonEmpty(const QString &value, const QString &msg, QString *error)
{
    if (value.trimmed().isEmpty()) {
        if (error) {
            *error = msg;
        }
        return false;
    }
    return true;
}

bool checkPartsInlineLimit(const QList<ProviderMessagePart> &partList,
                           int maxInlineAssetBytes,
                           QString *error)
{
    const auto exceeds = [maxInlineAssetBytes](const QByteArray &data) {
        return maxInlineAssetBytes >= 0 && data.size() > maxInlineAssetBytes;
    };
    const auto fail = [error](const QString &msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };

    for (const ProviderMessagePart &part : partList) {
        if (part.kind == ProviderPartKind::Image
            && exceeds(part.image.data)
            && !part.image.hasBlobRef()) {
            return fail(QStringLiteral("image inline data exceeds limit; use ProviderBlobRef"));
        }
        if (part.kind == ProviderPartKind::Audio
            && exceeds(part.audio.data)
            && !part.audio.hasBlobRef()) {
            return fail(QStringLiteral("audio inline data exceeds limit; use ProviderBlobRef"));
        }
        if (part.kind == ProviderPartKind::Video
            && exceeds(part.video.data)
            && !part.video.hasBlobRef()) {
            return fail(QStringLiteral("video inline data exceeds limit; use ProviderBlobRef"));
        }
        if (part.kind == ProviderPartKind::Document
            && exceeds(part.document.data)
            && !part.document.hasBlobRef()) {
            return fail(QStringLiteral("document inline data exceeds limit; use ProviderBlobRef"));
        }
    }
    return true;
}

} // namespace

ProviderItem ProviderItem::makeUserMessage(const QList<ProviderMessagePart> &parts)
{
    ProviderItem item;
    item.kind = ProviderItemKind::UserMessage;
    item.parts = parts;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeUserText(const QString &text)
{
    return makeUserMessage({ProviderMessagePart::makeText(text)});
}

ProviderItem ProviderItem::makeUserImage(const ProviderImageAsset &image,
                                         const QString &caption)
{
    return makeUserMessage(partsWithOptionalCaption(ProviderMessagePart::makeImage(image), caption));
}

ProviderItem ProviderItem::makeUserAudio(const ProviderAudioAsset &audio,
                                         const QString &caption)
{
    return makeUserMessage(partsWithOptionalCaption(ProviderMessagePart::makeAudio(audio), caption));
}

ProviderItem ProviderItem::makeUserDocument(const ProviderDocumentAsset &document,
                                            const QString &caption)
{
    return makeUserMessage(
        partsWithOptionalCaption(ProviderMessagePart::makeDocument(document), caption));
}

ProviderItem ProviderItem::makeUserVideo(const ProviderVideoAsset &video,
                                         const QString &caption)
{
    return makeUserMessage(
        partsWithOptionalCaption(ProviderMessagePart::makeVideo(video), caption));
}

ProviderItem ProviderItem::makeAssistantMessage(const QList<ProviderMessagePart> &parts)
{
    ProviderItem item;
    item.kind = ProviderItemKind::AssistantMessage;
    item.parts = parts;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeAssistantText(const QString &text)
{
    return makeAssistantMessage({ProviderMessagePart::makeText(text)});
}

ProviderItem ProviderItem::makeAssistantImage(const ProviderImageAsset &image,
                                              const QString &caption)
{
    return makeAssistantMessage(partsWithOptionalCaption(ProviderMessagePart::makeImage(image), caption));
}

ProviderItem ProviderItem::makeAssistantAudio(const ProviderAudioAsset &audio,
                                              const QString &caption)
{
    return makeAssistantMessage(partsWithOptionalCaption(ProviderMessagePart::makeAudio(audio), caption));
}

ProviderItem ProviderItem::makeFunctionCall(const QString &callId,
                                            const QString &name,
                                            const QJsonObject &arguments,
                                            const QString &rawArguments)
{
    ProviderItem item;
    item.kind = ProviderItemKind::FunctionCall;
    item.callId = callId;
    item.name = name;
    item.arguments = arguments;
    item.rawArguments = rawArguments;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeFunctionCall(const ToolCall &toolCall)
{
    ProviderItem item = makeFunctionCall(toolCall.id,
                                         toolCall.toolName,
                                         toolCall.input,
                                         toolCall.rawInputJson);
    // 保留 programmatic / code_execution 调用者，便于 FunctionCallOutput 原样回灌
    item.callerKind = parseCallerKind(toolCall.callerType);
    item.callerId = toolCall.callerId;
    return item;
}

ProviderItem ProviderItem::makeFunctionCallOutput(const QString &callId,
                                                  const QString &name,
                                                  const QString &output,
                                                  const bool isError,
                                                  const bool wasTruncated,
                                                  const QList<ProviderMessagePart> &outputParts)
{
    ProviderItem item;
    item.kind = ProviderItemKind::FunctionCallOutput;
    item.callId = callId;
    item.name = name;
    item.output = output;
    item.outputParts = outputParts;
    item.isError = isError;
    item.wasTruncated = wasTruncated;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeFunctionCallOutput(const ToolResult &result)
{
    return functionCallOutputFromToolResult(result);
}

ProviderItem ProviderItem::makeReasoning(const QString &content,
                                         const QString &signature,
                                         const bool redacted,
                                         const bool mustReplay)
{
    ProviderItem item;
    item.kind = ProviderItemKind::Reasoning;
    item.reasoningText = content;
    item.reasoningSignature = signature;
    item.reasoningRedacted = redacted;
    item.reasoningMustReplay = mustReplay;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeServerToolCall(const QString &callId,
                                              const QString &name,
                                              const QJsonObject &arguments,
                                              const QString &rawArguments)
{
    ProviderItem item;
    item.kind = ProviderItemKind::ServerToolCall;
    item.callId = callId;
    item.name = name;
    item.arguments = arguments;
    item.rawArguments = rawArguments;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeServerToolResult(const QString &callId,
                                                const QString &name,
                                                const QString &output,
                                                const QJsonObject &details,
                                                const bool isError,
                                                const QList<ProviderMessagePart> &outputParts)
{
    ProviderItem item;
    item.kind = ProviderItemKind::ServerToolResult;
    item.callId = callId;
    item.name = name;
    item.output = output;
    item.details = details;
    item.outputParts = outputParts;
    item.isError = isError;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeProgram(const QString &callId,
                                       const QString &code,
                                       const QString &fingerprint)
{
    ProviderItem item;
    item.kind = ProviderItemKind::Program;
    item.callId = callId;
    item.programCode = code;
    item.programFingerprint = fingerprint;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeProgramOutput(const QString &callId,
                                             const QString &result,
                                             const QString &status)
{
    ProviderItem item;
    item.kind = ProviderItemKind::ProgramOutput;
    item.callId = callId;
    item.output = result;
    if (status.trimmed().isEmpty()) {
        item.status = ProviderItemStatus::Completed;
    } else {
        bool ok = false;
        item.status = parseItemStatus(status, &ok);
        if (!ok) {
            item.status = ProviderItemStatus::Unset;
        }
    }
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeProgramOutput(const QString &callId,
                                             const QString &result,
                                             const ProviderItemStatus status)
{
    ProviderItem item;
    item.kind = ProviderItemKind::ProgramOutput;
    item.callId = callId;
    item.output = result;
    item.status = status == ProviderItemStatus::Unset ? ProviderItemStatus::Completed
                                                      : status;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeApprovalRequest(const QString &requestId,
                                               const QString &name,
                                               const QString &argumentsJson,
                                               const QString &serverLabel)
{
    ProviderItem item;
    item.kind = ProviderItemKind::ApprovalRequest;
    item.callId = requestId;
    item.name = name;
    item.rawArguments = argumentsJson;
    item.serverLabel = serverLabel;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeApprovalResponse(const QString &responseId,
                                                const QString &approvalRequestId,
                                                const bool approved,
                                                const QString &reason)
{
    ProviderItem item;
    item.kind = ProviderItemKind::ApprovalResponse;
    item.callId = responseId;
    item.approvalRequestId = approvalRequestId;
    item.approved = approved;
    item.approvalReason = reason;
    ensureItemId(&item);
    return item;
}

ProviderItem ProviderItem::makeCompaction(const QString &summary, const QString &itemId)
{
    ProviderItem item;
    item.kind = ProviderItemKind::Compaction;
    item.compactionSummary = summary;
    item.itemId = itemId;
    ensureItemId(&item);
    return item;
}

QString ProviderItem::toDebugString() const
{
    switch (kind) {
    case ProviderItemKind::UserMessage:
        return QStringLiteral("user(%1 parts)").arg(parts.size());
    case ProviderItemKind::AssistantMessage:
        return QStringLiteral("assistant(%1 parts)").arg(parts.size());
    case ProviderItemKind::FunctionCall:
        return QStringLiteral("function_call(%1:%2)").arg(name, callId);
    case ProviderItemKind::FunctionCallOutput:
        return QStringLiteral("function_call_output(%1:%2)").arg(name, callId);
    case ProviderItemKind::Reasoning:
        return reasoningRedacted
                   ? QStringLiteral("reasoning(<redacted>)")
                   : QStringLiteral("reasoning(%1 chars)").arg(reasoningText.size());
    case ProviderItemKind::ServerToolCall:
        return QStringLiteral("server_tool_call(%1:%2)").arg(name, callId);
    case ProviderItemKind::ServerToolResult:
        return QStringLiteral("server_tool_result(%1:%2)").arg(name, callId);
    case ProviderItemKind::Program:
        return QStringLiteral("program(%1)").arg(callId);
    case ProviderItemKind::ProgramOutput:
        return QStringLiteral("program_output(%1:%2)").arg(callId, toString(status));
    case ProviderItemKind::ApprovalRequest:
        return QStringLiteral("approval_request(%1:%2)").arg(name, callId);
    case ProviderItemKind::ApprovalResponse:
        return QStringLiteral("approval_response(%1 approved=%2)")
            .arg(approvalRequestId)
            .arg(approved ? QStringLiteral("true") : QStringLiteral("false"));
    case ProviderItemKind::Compaction:
        return QStringLiteral("compaction(%1 chars)").arg(compactionSummary.size());
    }
    return {};
}

bool ProviderItem::isConversational() const
{
    return kind == ProviderItemKind::UserMessage
           || kind == ProviderItemKind::AssistantMessage;
}

bool ProviderItem::isToolRelated() const
{
    return kind == ProviderItemKind::FunctionCall
           || kind == ProviderItemKind::FunctionCallOutput
           || isServerToolRelated()
           || isProgramRelated()
           || isApprovalRelated();
}

bool ProviderItem::isServerToolRelated() const
{
    return kind == ProviderItemKind::ServerToolCall
           || kind == ProviderItemKind::ServerToolResult;
}

bool ProviderItem::isProgramRelated() const
{
    return kind == ProviderItemKind::Program
           || kind == ProviderItemKind::ProgramOutput;
}

bool ProviderItem::isApprovalRelated() const
{
    return kind == ProviderItemKind::ApprovalRequest
           || kind == ProviderItemKind::ApprovalResponse;
}

bool ProviderItem::hasImageParts() const
{
    return hasPartKind(parts, ProviderPartKind::Image);
}

bool ProviderItem::hasAudioParts() const
{
    return hasPartKind(parts, ProviderPartKind::Audio);
}

bool ProviderItem::hasDocumentParts() const
{
    return hasPartKind(parts, ProviderPartKind::Document);
}

bool ProviderItem::hasVideoParts() const
{
    return hasPartKind(parts, ProviderPartKind::Video);
}

bool ProviderItem::validate(QString *error,
                            const bool strictReasoningSingleTrack,
                            const int maxInlineAssetBytes) const
{
    const auto fail = [error](const QString &msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };

    // 公共前置：callerKind 合法性 + reasoning 单轨
    if (!isKnownCallerKind(callerKind)) {
        return fail(QStringLiteral("unknown callerKind"));
    }

    if (strictReasoningSingleTrack
        && kind != ProviderItemKind::Reasoning
        && !reasoningText.trimmed().isEmpty()) {
        return fail(QStringLiteral(
            "reasoningText on non-Reasoning kind forbidden under single-track policy"));
    }

    // 内联字节上限：账本应优先 blobRef
    const auto checkPartsInline = [&](const QList<ProviderMessagePart> &partList) -> bool {
        return checkPartsInlineLimit(partList, maxInlineAssetBytes, error);
    };

    // 按 kind 校验必填字段与扩展约束
    switch (kind) {
    case ProviderItemKind::UserMessage:
    case ProviderItemKind::AssistantMessage:
        if (parts.isEmpty()) {
            return fail(QStringLiteral("message requires non-empty parts"));
        }
        return checkPartsInline(parts);

    case ProviderItemKind::FunctionCall:
    case ProviderItemKind::ServerToolCall:
        if (!requireNonEmpty(callId, QStringLiteral("tool call requires callId"), error)) {
            return false;
        }
        if (!requireNonEmpty(name, QStringLiteral("tool call requires name"), error)) {
            return false;
        }
        if (kind == ProviderItemKind::ServerToolCall && !isKnownServerToolName(name)
            && !name.startsWith(QStringLiteral("x_"))) {
            // 未知短名：允许 x_ 扩展，其它应登记 ProviderServerToolName
            return fail(QStringLiteral("unknown server tool name: %1").arg(name));
        }
        return true;

    case ProviderItemKind::FunctionCallOutput:
        if (!requireNonEmpty(callId,
                             QStringLiteral("function call output requires callId"),
                             error)) {
            return false;
        }
        {
            QString detailsError;
            if (!validateToolDetailsObject(details, &detailsError)) {
                return fail(detailsError);
            }
        }
        return checkPartsInline(outputParts);

    case ProviderItemKind::ServerToolResult:
        if (!requireNonEmpty(callId,
                             QStringLiteral("server tool result requires callId"),
                             error)) {
            return false;
        }
        if (!requireNonEmpty(name,
                             QStringLiteral("server tool result requires name"),
                             error)) {
            return false;
        }
        {
            QString detailsError;
            if (!validateToolDetailsObject(details, &detailsError)) {
                return fail(detailsError);
            }
        }
        return checkPartsInline(outputParts);

    case ProviderItemKind::Reasoning:
        // redacted 时正文可空；非 redacted 须有 content 或 signature
        if (!reasoningRedacted && reasoningText.trimmed().isEmpty()
            && reasoningSignature.trimmed().isEmpty()) {
            return fail(QStringLiteral("reasoning requires content or signature"));
        }
        return true;

    case ProviderItemKind::Program:
        if (!requireNonEmpty(callId, QStringLiteral("program requires callId"), error)) {
            return false;
        }
        if (!requireNonEmpty(programFingerprint,
                             QStringLiteral("program requires programFingerprint"),
                             error)) {
            return false;
        }
        return true;

    case ProviderItemKind::ProgramOutput:
        if (!requireNonEmpty(callId,
                             QStringLiteral("program_output requires callId"),
                             error)) {
            return false;
        }
        return true;

    case ProviderItemKind::ApprovalRequest:
        if (!requireNonEmpty(callId,
                             QStringLiteral("approval request requires callId (request id)"),
                             error)) {
            return false;
        }
        if (!requireNonEmpty(name,
                             QStringLiteral("approval request requires name"),
                             error)) {
            return false;
        }
        return true;

    case ProviderItemKind::ApprovalResponse:
        if (!requireNonEmpty(approvalRequestId,
                             QStringLiteral("approval response requires approvalRequestId"),
                             error)) {
            return false;
        }
        return true;

    case ProviderItemKind::Compaction:
        if (!requireNonEmpty(compactionSummary,
                             QStringLiteral("compaction requires compactionSummary"),
                             error)) {
            return false;
        }
        if (!callId.trimmed().isEmpty()) {
            return fail(QStringLiteral("compaction must not use callId; use itemId"));
        }
        return true;
    }

    return fail(QStringLiteral("unknown ProviderItemKind"));
}

ProviderToolSpecification toProviderToolSpecification(const ToolSpec &spec)
{
    ProviderToolSpecification out;
    out.name = spec.name;
    out.description = spec.description;
    out.inputSchema = spec.inputSchema;
    out.outputSchema = spec.outputSchema;
    out.strictSchema = spec.strictSchema;
    out.deferLoading = spec.deferLoading;
    for (const QString &caller : spec.allowedCallers) {
        bool ok = false;
        const ProviderCallerKind kind = parseCallerKind(caller, &ok);
        if (ok && kind != ProviderCallerKind::Unset)
            out.allowedCallers.append(kind);
    }
    return out;
}

ProviderItem functionCallOutputFromToolResult(const ToolResult &result)
{
    ProviderItem item = ProviderItem::makeFunctionCallOutput(result.toolUseId,
                                                             result.toolName,
                                                             result.text,
                                                             result.isError,
                                                             result.wasTruncated);
    // 可移植摘要进 details（非 raw 整包 payload）
    QJsonObject details;
    if (!result.summaryText.trimmed().isEmpty()) {
        details.insert(QStringLiteral("summary"), result.summaryText);
    }
    if (!result.payloadType.trimmed().isEmpty()) {
        details.insert(QStringLiteral("payloadType"), result.payloadType);
    }
    if (!result.payload.isEmpty()) {
        // 仅保留浅层可移植提示，避免整包厂商/运行时私货
        details.insert(QStringLiteral("hasPayload"), true);
    }
    if (result.category == ToolResultCategory::Rejected) {
        details.insert(QStringLiteral("status"), QStringLiteral("rejected"));
    } else if (result.category == ToolResultCategory::Canceled) {
        details.insert(QStringLiteral("status"), QStringLiteral("canceled"));
    }
    item.details = details;
    return item;
}
