#include "ProviderRunLedger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>


namespace {

QString nextLedgerEntryId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QSet<QString> collectResolvedToolUseIds(const QList<ConversationMessage> &entries)
{
    QSet<QString> resolvedIds;
    for (const ConversationMessage &entry : entries) {
        if (entry.kind == ConversationMessage::Kind::ToolResult && !entry.toolUseId.isEmpty()) {
            resolvedIds.insert(entry.toolUseId);
        }
    }
    return resolvedIds;
}

/// 仅非空 incoming 覆盖；空值不抹已有内容（防 MessageCompleted 空 fallback 抹参）。
void preferIncoming(QString &target, const QString &incoming)
{
    if (!incoming.isEmpty())
        target = incoming;
}

void preferIncoming(QJsonObject &target, const QJsonObject &incoming)
{
    if (!incoming.isEmpty())
        target = incoming;
}

void preferIncomingTrimmed(QString &target, const QString &incoming)
{
    if (!incoming.trimmed().isEmpty())
        target = incoming;
}

QString providerBlobRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/provider-blobs");
}

template<typename Asset>
void externalizeAsset(Asset &asset)
{
    if (asset.data.isEmpty() || asset.blobRef.hasBlobId())
        return;
    const QByteArray digest =
        QCryptographicHash::hash(asset.data, QCryptographicHash::Sha256).toHex();
    const QString blobId = QString::fromLatin1(digest);
    QDir root(providerBlobRoot());
    if (!root.exists() && !root.mkpath(QStringLiteral(".")))
        return;
    const QString path = root.filePath(blobId);
    if (!QFileInfo::exists(path)) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)
            || file.write(asset.data) != asset.data.size()
            || !file.commit()) {
            return;
        }
    }
    asset.blobRef.blobId = blobId;
    asset.blobRef.contentHash = QStringLiteral("sha256:") + blobId;
    asset.blobRef.byteSize = asset.data.size();
    asset.blobRef.scheme = ProviderUriScheme::Blob;
    asset.data.clear();
}

template<typename Asset>
void hydrateAsset(Asset &asset)
{
    if (!asset.data.isEmpty() || !asset.blobRef.hasBlobId()
        || asset.blobRef.scheme != ProviderUriScheme::Blob) {
        return;
    }
    QFile file(QDir(providerBlobRoot()).filePath(asset.blobRef.blobId));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray data = file.readAll();
    if (asset.blobRef.byteSize > 0 && data.size() != asset.blobRef.byteSize)
        return;
    if (asset.blobRef.contentHash.startsWith(QStringLiteral("sha256:"))) {
        const QString actual = QStringLiteral("sha256:")
            + QString::fromLatin1(
                QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        if (actual != asset.blobRef.contentHash)
            return;
    }
    asset.data = data;
}

void externalizePart(ProviderMessagePart &part)
{
    switch (part.kind) {
    case ProviderPartKind::Image: externalizeAsset(part.image); break;
    case ProviderPartKind::Audio: externalizeAsset(part.audio); break;
    case ProviderPartKind::Document: externalizeAsset(part.document); break;
    case ProviderPartKind::Video: externalizeAsset(part.video); break;
    case ProviderPartKind::Text: break;
    }
}

void hydratePart(ProviderMessagePart &part)
{
    switch (part.kind) {
    case ProviderPartKind::Image: hydrateAsset(part.image); break;
    case ProviderPartKind::Audio: hydrateAsset(part.audio); break;
    case ProviderPartKind::Document: hydrateAsset(part.document); break;
    case ProviderPartKind::Video: hydrateAsset(part.video); break;
    case ProviderPartKind::Text: break;
    }
}

void externalizeProviderItem(ProviderItem &item)
{
    for (ProviderMessagePart &part : item.parts)
        externalizePart(part);
    for (ProviderMessagePart &part : item.outputParts)
        externalizePart(part);
}

ProviderItem hydratedProviderItem(ProviderItem item)
{
    for (ProviderMessagePart &part : item.parts)
        hydratePart(part);
    for (ProviderMessagePart &part : item.outputParts)
        hydratePart(part);
    return item;
}

QString statusToString(const ConversationMessage::Status status)
{
    return ConversationMessageText::storageStatus(status);
}

ConversationMessage::Status statusFromString(const QString &value)
{
    return ConversationMessageText::statusFromStorage(value);
}

QString resultCategoryToString(const ConversationMessage::ResultCategory category)
{
    return ConversationMessageText::storageResultCategory(category);
}

ConversationMessage::ResultCategory resultCategoryFromString(const QString &value)
{
    return ConversationMessageText::resultCategoryFromStorage(value);
}

QJsonObject toolCallToJson(const ToolCall &toolCall)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), toolCall.id);
    object.insert(QStringLiteral("name"), toolCall.toolName);
    object.insert(QStringLiteral("input"), toolCall.input);
    object.insert(QStringLiteral("rawInputJson"), toolCall.rawInputJson);
    object.insert(QStringLiteral("callerType"), toolCall.callerType);
    object.insert(QStringLiteral("callerId"), toolCall.callerId);
    return object;
}

ToolCall toolCallFromJson(const QJsonObject &object)
{
    ToolCall toolCall;
    toolCall.id = object.value(QStringLiteral("id")).toString();
    toolCall.toolName = object.value(QStringLiteral("name")).toString();
    toolCall.input = object.value(QStringLiteral("input")).toObject();
    toolCall.rawInputJson = object.value(QStringLiteral("rawInputJson")).toString();
    toolCall.callerType = object.value(QStringLiteral("callerType")).toString();
    toolCall.callerId = object.value(QStringLiteral("callerId")).toString();
    return toolCall;
}

QJsonObject blobRefToJson(const ProviderBlobRef &blob)
{
    return {{QStringLiteral("blobId"), blob.blobId},
            {QStringLiteral("contentHash"), blob.contentHash},
            {QStringLiteral("byteSize"), QString::number(blob.byteSize)},
            {QStringLiteral("expiresAtMs"), QString::number(blob.expiresAtMs)},
            {QStringLiteral("scheme"), static_cast<int>(blob.scheme)}};
}

ProviderBlobRef blobRefFromJson(const QJsonObject &object)
{
    ProviderBlobRef blob;
    blob.blobId = object.value(QStringLiteral("blobId")).toString();
    blob.contentHash = object.value(QStringLiteral("contentHash")).toString();
    blob.byteSize = object.value(QStringLiteral("byteSize")).toString().toLongLong();
    blob.expiresAtMs = object.value(QStringLiteral("expiresAtMs")).toString().toLongLong();
    blob.scheme = static_cast<ProviderUriScheme>(object.value(QStringLiteral("scheme")).toInt());
    return blob;
}

QJsonObject messagePartToJson(const ProviderMessagePart &part)
{
    QJsonObject object{
        {QStringLiteral("kind"), static_cast<int>(part.kind)},
        {QStringLiteral("text"), part.text},
        {QStringLiteral("cachePolicy"), static_cast<int>(part.cachePolicy)},
        {QStringLiteral("mediaResolution"), part.mediaResolution}};
    object.insert(QStringLiteral("image"), QJsonObject{
        {QStringLiteral("uri"), part.image.uri},
        {QStringLiteral("data"), QString::fromLatin1(part.image.data.toBase64())},
        {QStringLiteral("mimeType"), part.image.mimeType},
        {QStringLiteral("altText"), part.image.altText},
        {QStringLiteral("blobRef"), blobRefToJson(part.image.blobRef)}});
    object.insert(QStringLiteral("audio"), QJsonObject{
        {QStringLiteral("uri"), part.audio.uri},
        {QStringLiteral("data"), QString::fromLatin1(part.audio.data.toBase64())},
        {QStringLiteral("mimeType"), part.audio.mimeType},
        {QStringLiteral("transcript"), part.audio.transcript},
        {QStringLiteral("durationMs"), part.audio.durationMs},
        {QStringLiteral("sampleRate"), part.audio.sampleRate},
        {QStringLiteral("voice"), part.audio.voice},
        {QStringLiteral("blobRef"), blobRefToJson(part.audio.blobRef)}});
    object.insert(QStringLiteral("document"), QJsonObject{
        {QStringLiteral("uri"), part.document.uri},
        {QStringLiteral("data"), QString::fromLatin1(part.document.data.toBase64())},
        {QStringLiteral("mimeType"), part.document.mimeType},
        {QStringLiteral("title"), part.document.title},
        {QStringLiteral("context"), part.document.context},
        {QStringLiteral("blobRef"), blobRefToJson(part.document.blobRef)}});
    object.insert(QStringLiteral("video"), QJsonObject{
        {QStringLiteral("uri"), part.video.uri},
        {QStringLiteral("data"), QString::fromLatin1(part.video.data.toBase64())},
        {QStringLiteral("mimeType"), part.video.mimeType},
        {QStringLiteral("altText"), part.video.altText},
        {QStringLiteral("startMs"), part.video.startMs},
        {QStringLiteral("endMs"), part.video.endMs},
        {QStringLiteral("fps"), part.video.fps},
        {QStringLiteral("blobRef"), blobRefToJson(part.video.blobRef)}});
    QJsonArray citations;
    for (const ProviderCitation &citation : part.citations) {
        citations.append(QJsonObject{
            {QStringLiteral("url"), citation.url},
            {QStringLiteral("title"), citation.title},
            {QStringLiteral("snippet"), citation.snippet},
            {QStringLiteral("startIndex"), citation.startIndex},
            {QStringLiteral("endIndex"), citation.endIndex}});
    }
    object.insert(QStringLiteral("citations"), citations);
    return object;
}

ProviderMessagePart messagePartFromJson(const QJsonObject &object)
{
    ProviderMessagePart part;
    part.kind = static_cast<ProviderPartKind>(object.value(QStringLiteral("kind")).toInt());
    part.text = object.value(QStringLiteral("text")).toString();
    part.cachePolicy = static_cast<ProviderCachePolicy>(
        object.value(QStringLiteral("cachePolicy")).toInt());
    part.mediaResolution = object.value(QStringLiteral("mediaResolution")).toString();
    const QJsonObject image = object.value(QStringLiteral("image")).toObject();
    part.image.uri = image.value(QStringLiteral("uri")).toString();
    part.image.data = QByteArray::fromBase64(image.value(QStringLiteral("data")).toString().toLatin1());
    part.image.mimeType = image.value(QStringLiteral("mimeType")).toString();
    part.image.altText = image.value(QStringLiteral("altText")).toString();
    part.image.blobRef = blobRefFromJson(image.value(QStringLiteral("blobRef")).toObject());
    const QJsonObject audio = object.value(QStringLiteral("audio")).toObject();
    part.audio.uri = audio.value(QStringLiteral("uri")).toString();
    part.audio.data = QByteArray::fromBase64(audio.value(QStringLiteral("data")).toString().toLatin1());
    part.audio.mimeType = audio.value(QStringLiteral("mimeType")).toString();
    part.audio.transcript = audio.value(QStringLiteral("transcript")).toString();
    part.audio.durationMs = audio.value(QStringLiteral("durationMs")).toInt();
    part.audio.sampleRate = audio.value(QStringLiteral("sampleRate")).toInt();
    part.audio.voice = audio.value(QStringLiteral("voice")).toString();
    part.audio.blobRef = blobRefFromJson(audio.value(QStringLiteral("blobRef")).toObject());
    const QJsonObject document = object.value(QStringLiteral("document")).toObject();
    part.document.uri = document.value(QStringLiteral("uri")).toString();
    part.document.data = QByteArray::fromBase64(
        document.value(QStringLiteral("data")).toString().toLatin1());
    part.document.mimeType = document.value(QStringLiteral("mimeType")).toString();
    part.document.title = document.value(QStringLiteral("title")).toString();
    part.document.context = document.value(QStringLiteral("context")).toString();
    part.document.blobRef = blobRefFromJson(document.value(QStringLiteral("blobRef")).toObject());
    const QJsonObject video = object.value(QStringLiteral("video")).toObject();
    part.video.uri = video.value(QStringLiteral("uri")).toString();
    part.video.data = QByteArray::fromBase64(video.value(QStringLiteral("data")).toString().toLatin1());
    part.video.mimeType = video.value(QStringLiteral("mimeType")).toString();
    part.video.altText = video.value(QStringLiteral("altText")).toString();
    part.video.startMs = video.value(QStringLiteral("startMs")).toInt();
    part.video.endMs = video.value(QStringLiteral("endMs")).toInt();
    part.video.fps = video.value(QStringLiteral("fps")).toDouble();
    part.video.blobRef = blobRefFromJson(video.value(QStringLiteral("blobRef")).toObject());
    for (const QJsonValue &value : object.value(QStringLiteral("citations")).toArray()) {
        const QJsonObject c = value.toObject();
        part.citations.append(ProviderCitation{
            c.value(QStringLiteral("url")).toString(),
            c.value(QStringLiteral("title")).toString(),
            c.value(QStringLiteral("snippet")).toString(),
            c.value(QStringLiteral("startIndex")).toInt(-1),
            c.value(QStringLiteral("endIndex")).toInt(-1)});
    }
    return part;
}

QJsonArray messagePartsToJson(const QList<ProviderMessagePart> &parts)
{
    QJsonArray array;
    for (const ProviderMessagePart &part : parts)
        array.append(messagePartToJson(part));
    return array;
}

QList<ProviderMessagePart> messagePartsFromJson(const QJsonArray &array)
{
    QList<ProviderMessagePart> parts;
    for (const QJsonValue &value : array)
        if (value.isObject())
            parts.append(messagePartFromJson(value.toObject()));
    return parts;
}

QJsonObject providerItemToJson(const ProviderItem &item)
{
    return {
        {QStringLiteral("kind"), static_cast<int>(item.kind)},
        {QStringLiteral("itemId"), item.itemId},
        {QStringLiteral("parts"), messagePartsToJson(item.parts)},
        {QStringLiteral("callId"), item.callId},
        {QStringLiteral("name"), item.name},
        {QStringLiteral("arguments"), item.arguments},
        {QStringLiteral("rawArguments"), item.rawArguments},
        {QStringLiteral("output"), item.output},
        {QStringLiteral("outputParts"), messagePartsToJson(item.outputParts)},
        {QStringLiteral("isError"), item.isError},
        {QStringLiteral("wasTruncated"), item.wasTruncated},
        {QStringLiteral("details"), item.details},
        {QStringLiteral("reasoningText"), item.reasoningText},
        {QStringLiteral("reasoningSignature"), item.reasoningSignature},
        {QStringLiteral("reasoningRedacted"), item.reasoningRedacted},
        {QStringLiteral("reasoningMustReplay"), item.reasoningMustReplay},
        {QStringLiteral("assistantPhase"), item.assistantPhase},
        {QStringLiteral("callerKind"), static_cast<int>(item.callerKind)},
        {QStringLiteral("callerId"), item.callerId},
        {QStringLiteral("serverLabel"), item.serverLabel},
        {QStringLiteral("programCode"), item.programCode},
        {QStringLiteral("programFingerprint"), item.programFingerprint},
        {QStringLiteral("status"), static_cast<int>(item.status)},
        {QStringLiteral("approved"), item.approved},
        {QStringLiteral("approvalReason"), item.approvalReason},
        {QStringLiteral("approvalRequestId"), item.approvalRequestId},
        {QStringLiteral("compactionSummary"), item.compactionSummary}};
}

ProviderItem providerItemFromJson(const QJsonObject &object)
{
    ProviderItem item;
    item.kind = static_cast<ProviderItemKind>(object.value(QStringLiteral("kind")).toInt());
    item.itemId = object.value(QStringLiteral("itemId")).toString();
    item.parts = messagePartsFromJson(object.value(QStringLiteral("parts")).toArray());
    item.callId = object.value(QStringLiteral("callId")).toString();
    item.name = object.value(QStringLiteral("name")).toString();
    item.arguments = object.value(QStringLiteral("arguments")).toObject();
    item.rawArguments = object.value(QStringLiteral("rawArguments")).toString();
    item.output = object.value(QStringLiteral("output")).toString();
    item.outputParts = messagePartsFromJson(object.value(QStringLiteral("outputParts")).toArray());
    item.isError = object.value(QStringLiteral("isError")).toBool();
    item.wasTruncated = object.value(QStringLiteral("wasTruncated")).toBool();
    item.details = object.value(QStringLiteral("details")).toObject();
    item.reasoningText = object.value(QStringLiteral("reasoningText")).toString();
    item.reasoningSignature = object.value(QStringLiteral("reasoningSignature")).toString();
    item.reasoningRedacted = object.value(QStringLiteral("reasoningRedacted")).toBool();
    item.reasoningMustReplay = object.value(QStringLiteral("reasoningMustReplay")).toBool();
    item.assistantPhase = object.value(QStringLiteral("assistantPhase")).toString();
    item.callerKind = static_cast<ProviderCallerKind>(
        object.value(QStringLiteral("callerKind")).toInt());
    item.callerId = object.value(QStringLiteral("callerId")).toString();
    item.serverLabel = object.value(QStringLiteral("serverLabel")).toString();
    item.programCode = object.value(QStringLiteral("programCode")).toString();
    item.programFingerprint = object.value(QStringLiteral("programFingerprint")).toString();
    item.status = static_cast<ProviderItemStatus>(object.value(QStringLiteral("status")).toInt());
    item.approved = object.value(QStringLiteral("approved")).toBool();
    item.approvalReason = object.value(QStringLiteral("approvalReason")).toString();
    item.approvalRequestId = object.value(QStringLiteral("approvalRequestId")).toString();
    item.compactionSummary = object.value(QStringLiteral("compactionSummary")).toString();
    return item;
}

QList<ProviderMessagePart> messagePartsForEntry(const ConversationMessage &entry)
{
    QList<ProviderMessagePart> parts{ProviderMessagePart::makeText(entry.text)};
    if (entry.kind != ConversationMessage::Kind::UserText || entry.attachedFilePaths.isEmpty())
        return parts;

    static const QStringList imageExts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("bmp")
    };
    static const QStringList textExts = {
        QStringLiteral("txt"), QStringLiteral("md"), QStringLiteral("json"),
        QStringLiteral("xml"), QStringLiteral("csv"), QStringLiteral("yaml"),
        QStringLiteral("yml"), QStringLiteral("toml"), QStringLiteral("ini"),
        QStringLiteral("cfg"), QStringLiteral("log"), QStringLiteral("py"),
        QStringLiteral("cpp"), QStringLiteral("c"), QStringLiteral("h"),
        QStringLiteral("hpp"), QStringLiteral("js"), QStringLiteral("ts"),
        QStringLiteral("jsx"), QStringLiteral("tsx"), QStringLiteral("qml"),
        QStringLiteral("css"), QStringLiteral("html"), QStringLiteral("sql"),
        QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("ps1"),
        QStringLiteral("cmake"), QStringLiteral("make"),
    };
    static constexpr qint64 kMaxTextFileBytes = 2 * 1024 * 1024;
    QMimeDatabase mimeDb;
    QStringList skippedNames;
    for (const QString &path : entry.attachedFilePaths) {
        const QFileInfo fi(path);
        if (!fi.exists() || !fi.isFile())
            continue;
        const QString suffix = fi.suffix().toLower();
        QFile file(path);
        if (imageExts.contains(suffix) && file.open(QIODevice::ReadOnly)) {
            parts.append(ProviderMessagePart::makeImage(
                ProviderImageAsset::fromBytes(file.readAll(), mimeDb.mimeTypeForFile(path).name(),
                                              fi.fileName())));
        } else if (textExts.contains(suffix) && file.size() <= kMaxTextFileBytes
                   && file.open(QIODevice::ReadOnly)) {
            parts.append(ProviderMessagePart::makeText(
                QStringLiteral("【文件：%1】\n%2").arg(fi.fileName(),
                                                    QString::fromUtf8(file.readAll()))));
        } else {
            skippedNames.append(fi.fileName());
        }
    }
    if (!skippedNames.isEmpty()) {
        parts.append(ProviderMessagePart::makeText(
            QStringLiteral("【用户附加了以下文件（未读取内容）：%1】")
                .arg(skippedNames.join(QStringLiteral("、")))));
    }
    return parts;
}

std::optional<ProviderItem> providerItemFromUiIngress(
    const ConversationMessage &entry)
{
    ProviderItem item;
    switch (entry.kind) {
    case ConversationMessage::Kind::SystemPrompt:
    case ConversationMessage::Kind::UserText:
    case ConversationMessage::Kind::AgentTask:
    case ConversationMessage::Kind::SkillInvoke:
        item = ProviderItem::makeUserMessage(messagePartsForEntry(entry));
        break;
    case ConversationMessage::Kind::SessionEvent:
        // 纯 UI/会话通知，不得进 Provider 线路（插在 tool pair 间会 400）
        return std::nullopt;
    case ConversationMessage::Kind::AssistantText:
        item = ProviderItem::makeAssistantText(entry.text);
        break;
    case ConversationMessage::Kind::AssistantReasoning:
        item = ProviderItem::makeReasoning(
            entry.reasoningContent.isEmpty() ? entry.text : entry.reasoningContent,
            entry.reasoningSignature, entry.reasoningRedacted, entry.reasoningMustReplay);
        break;
    case ConversationMessage::Kind::ToolCall:
        if (effectiveToolCallArgumentsJson(entry.toolCall).isEmpty())
            return std::nullopt;
        item = ProviderItem::makeFunctionCall(entry.toolCall);
        break;
    case ConversationMessage::Kind::ToolResult:
        item = ProviderItem::makeFunctionCallOutput(
            entry.toolUseId, entry.toolName, entry.text, entry.isError,
            entry.wasTruncated);
        break;
    case ConversationMessage::Kind::Summary:
        // 本地摘要用 UserText 回灌，避免 OpenAI compaction wire 能力校验失败
        item = ProviderItem::makeUserText(entry.text);
        break;
    case ConversationMessage::Kind::ApprovalRequest:
    case ConversationMessage::Kind::Error:
        return std::nullopt;
    }
    if (item.itemId.isEmpty())
        item.itemId = entry.id;
    return item;
}

ConversationMessage conversationEntryForItem(const ProviderItem &item,
                                              const QString &turnId,
                                              const QString &continuationId)
{
    ConversationMessage entry;
    entry.id = item.itemId;
    entry.status = ConversationMessage::Status::Completed;
    entry.turnId = turnId;
    entry.providerContinuationId = continuationId;
    entry.submittedToModel = true;
    entry.wasTruncated = item.wasTruncated;
    entry.isError = item.isError;
    switch (item.kind) {
    case ProviderItemKind::UserMessage:
        entry.kind = ConversationMessage::Kind::UserText;
        entry.text = joinedText(item.parts);
        break;
    case ProviderItemKind::AssistantMessage:
        entry.kind = ConversationMessage::Kind::AssistantText;
        entry.text = joinedText(item.parts);
        for (const ProviderMessagePart &part : item.parts) {
            if (part.kind == ProviderPartKind::Image) {
                entry.imageOutput = part.image;
                break;
            }
        }
        break;
    case ProviderItemKind::Reasoning:
        entry.kind = ConversationMessage::Kind::AssistantReasoning;
        entry.reasoningContent = item.reasoningText;
        entry.reasoningSignature = item.reasoningSignature;
        entry.reasoningRedacted = item.reasoningRedacted;
        entry.reasoningMustReplay = item.reasoningMustReplay;
        break;
    case ProviderItemKind::FunctionCall:
        entry.kind = ConversationMessage::Kind::ToolCall;
        entry.toolUseId = item.callId;
        entry.toolName = item.name;
        entry.toolCall = ToolCall{item.callId, item.name, item.arguments,
                                  item.rawArguments, toString(item.callerKind),
                                  item.callerId};
        break;
    case ProviderItemKind::FunctionCallOutput:
        entry.kind = ConversationMessage::Kind::ToolResult;
        entry.toolUseId = item.callId;
        entry.toolName = item.name;
        entry.text = item.output;
        break;
    case ProviderItemKind::Compaction:
        entry.kind = ConversationMessage::Kind::Summary;
        entry.text = item.compactionSummary;
        break;
    case ProviderItemKind::ServerToolCall:
    case ProviderItemKind::Program:
    case ProviderItemKind::ApprovalRequest:
        entry.kind = ConversationMessage::Kind::ToolCall;
        entry.toolUseId = item.callId;
        entry.toolName = item.name;
        entry.text = item.kind == ProviderItemKind::Program
            ? item.programCode : item.output;
        entry.toolCall = ToolCall{item.callId, item.name, item.arguments,
                                  item.rawArguments, toString(item.callerKind),
                                  item.callerId};
        break;
    case ProviderItemKind::ServerToolResult:
    case ProviderItemKind::ProgramOutput:
    case ProviderItemKind::ApprovalResponse:
        entry.kind = ConversationMessage::Kind::ToolResult;
        entry.toolUseId = item.callId.isEmpty() ? item.approvalRequestId : item.callId;
        entry.toolName = item.name;
        entry.text = item.output.isEmpty() ? item.approvalReason : item.output;
        break;
    }
    entry.reasoningSignature = item.reasoningSignature;
    entry.reasoningRedacted = item.reasoningRedacted;
    entry.reasoningMustReplay = item.reasoningMustReplay;
    entry.providerMustReplay =
        item.reasoningMustReplay
        || item.isProgramRelated()
        || item.isApprovalRelated()
        || item.isServerToolRelated()
        || item.kind == ProviderItemKind::Compaction;
    return entry;
}

/// 厂商要求 reasoning 有 content 或 signature；redacted 可空。
[[nodiscard]] bool isReplayableReasoningItem(const ProviderItem &item)
{
    if (item.kind != ProviderItemKind::Reasoning)
        return true;
    if (item.reasoningRedacted)
        return true;
    return !item.reasoningText.trimmed().isEmpty()
           || !item.reasoningSignature.trimmed().isEmpty();
}

[[nodiscard]] bool isCjkCodeUnit(const ushort u)
{
    // 常用 CJK / 全角 / 日文假名等；足够区分中英混排，不必完整 Unicode 脚本表
    return (u >= 0x3000 && u <= 0x303F)
        || (u >= 0x3040 && u <= 0x30FF)
        || (u >= 0x3400 && u <= 0x4DBF)
        || (u >= 0x4E00 && u <= 0x9FFF)
        || (u >= 0xF900 && u <= 0xFAFF)
        || (u >= 0xFF00 && u <= 0xFFEF);
}

qint64 estimateTokenCountForAssetBytes(const qint64 byteSize, const qint64 fallbackTokens)
{
    if (byteSize > 0) {
        // 多模态无 tokenizer：按体积给保守上限，避免低估导致晚压缩
        return qBound(64LL, (byteSize + 512) / 512, 4096LL);
    }
    return fallbackTokens;
}

/// blob 优先；否则回落内联 data.size()
qint64 assetByteSize(const qint64 blobByteSize, const qsizetype inlineSize)
{
    return blobByteSize > 0 ? blobByteSize : static_cast<qint64>(inlineSize);
}

} // namespace

qint64 estimateContextTokensForText(const QString &text)
{
    if (text.isEmpty())
        return 0;

    qint64 cjk = 0;
    qint64 other = 0;
    for (const QChar ch : text) {
        if (isCjkCodeUnit(ch.unicode()))
            ++cjk;
        else
            ++other;
    }
    // CJK ≈ 1.5 字/token → tokens ≈ ceil(cjk * 2/3)；其它 ≈ 4 字符/token
    const qint64 cjkTokens = (cjk * 2 + 2) / 3;
    const qint64 otherTokens = (other + 3) / 4;
    return qMax<qint64>(1, cjkTokens + otherTokens);
}

namespace {

qint64 estimateContextTokensForMessageParts(const QList<ProviderMessagePart> &parts)
{
    qint64 total = 0;
    for (const ProviderMessagePart &part : parts) {
        switch (part.kind) {
        case ProviderPartKind::Text:
            total += estimateContextTokensForText(part.text);
            break;
        case ProviderPartKind::Image:
            total += estimateTokenCountForAssetBytes(
                assetByteSize(part.image.blobRef.byteSize, part.image.data.size()), 256);
            break;
        case ProviderPartKind::Audio:
            total += estimateContextTokensForText(part.audio.transcript);
            total += estimateTokenCountForAssetBytes(
                assetByteSize(part.audio.blobRef.byteSize, part.audio.data.size()), 128);
            break;
        case ProviderPartKind::Document:
            total += estimateContextTokensForText(part.document.title + part.document.context);
            total += estimateTokenCountForAssetBytes(
                assetByteSize(part.document.blobRef.byteSize, part.document.data.size()), 512);
            break;
        case ProviderPartKind::Video:
            total += estimateTokenCountForAssetBytes(
                assetByteSize(part.video.blobRef.byteSize, part.video.data.size()), 512);
            break;
        }
    }
    return total;
}

} // namespace

qint64 estimateContextTokensForItem(const ProviderItem &item)
{
    // 每条消息/工具结构开销（role 分隔等）——固定小额，避免全 0 低估
    constexpr qint64 kItemOverhead = 4;
    switch (item.kind) {
    case ProviderItemKind::UserMessage:
    case ProviderItemKind::AssistantMessage:
        return estimateContextTokensForMessageParts(item.parts) + kItemOverhead;
    case ProviderItemKind::FunctionCall: {
        qint64 n = estimateContextTokensForText(item.name) + kItemOverhead;
        // 与 effectiveToolCallArgumentsJson 同序：raw 优先，否则 compact JSON
        if (!item.rawArguments.trimmed().isEmpty())
            n += estimateContextTokensForText(item.rawArguments);
        else if (!item.arguments.isEmpty())
            n += estimateContextTokensForText(
                QString::fromUtf8(QJsonDocument(item.arguments).toJson(QJsonDocument::Compact)));
        return n;
    }
    case ProviderItemKind::FunctionCallOutput:
        return estimateContextTokensForText(item.name)
            + estimateContextTokensForText(item.output) + kItemOverhead;
    case ProviderItemKind::Reasoning:
        return estimateContextTokensForText(item.reasoningText)
            + estimateContextTokensForText(item.reasoningSignature) + kItemOverhead;
    case ProviderItemKind::Compaction:
        return estimateContextTokensForText(item.compactionSummary)
            + estimateContextTokensForText(item.output) + kItemOverhead;
    default:
        return estimateContextTokensForText(item.output + item.compactionSummary) + kItemOverhead;
    }
}

qint64 estimateContextTokensForToolSpecs(const QList<ProviderToolSpecification> &tools)
{
    qint64 total = 0;
    for (const ProviderToolSpecification &tool : tools) {
        total += estimateContextTokensForText(tool.name);
        total += estimateContextTokensForText(tool.description);
        if (!tool.inputSchema.isEmpty()) {
            total += estimateContextTokensForText(
                QString::fromUtf8(QJsonDocument(tool.inputSchema).toJson(QJsonDocument::Compact)));
        }
        total += 8; // tool 封装开销
    }
    return total;
}

QString effectiveToolCallArgumentsJson(const ToolCall &toolCall)
{
    const QString rawArguments = toolCall.rawInputJson.trimmed();
    if (!rawArguments.isEmpty()) {
        return rawArguments;
    }

    if (!toolCall.input.isEmpty()) {
        return QString::fromUtf8(QJsonDocument(toolCall.input).toJson(QJsonDocument::Compact));
    }

    return {};
}

void ProviderRunLedger::clear()
{
    m_entries.clear();
    m_providerRecords.clear();
    m_lastProviderInputTokens = 0;
}

const QList<ConversationMessage> &ProviderRunLedger::entries() const
{
    return m_entries;
}

QString ProviderRunLedger::appendUiIngress(ConversationMessage entry)
{
    if (entry.id.trimmed().isEmpty()) {
        entry.id = nextLedgerEntryId();
    }
    if (entry.createdAtMs == 0) {
        entry.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    }
    m_entries.append(entry);
    if (const auto item = providerItemFromUiIngress(entry); item.has_value()) {
        // 空推理只进 UI（过程卡），不进线路；内容到达后再 setProviderItemForEntry。
        // 否则 ensureStreamingReasoningEntry 会立刻落一条非法 ProviderRecord，
        // 而流式 delta 只改 ConversationMessage，下一步请求仍带空 reasoning。
        if (!isReplayableReasoningItem(*item))
            return entry.id;
        ProviderItem storedItem = *item;
        externalizeProviderItem(storedItem);
        ProviderRecord record;
        record.item = std::move(storedItem);
        record.entryId = entry.id;
        record.submitted = entry.submittedToModel;
        record.compacted = entry.wasCompacted;
        record.continuationId = entry.providerContinuationId;
        refreshTokenEstimate(record);
        m_providerRecords.append(std::move(record));
    }
    return entry.id;
}

QString ProviderRunLedger::appendProviderItem(ProviderItem item,
                                              const QString &turnId,
                                              const QString &continuationId)
{
    if (item.itemId.trimmed().isEmpty())
        item.itemId = nextLedgerEntryId();
    ConversationMessage entry = conversationEntryForItem(item, turnId, continuationId);
    if (entry.id.isEmpty())
        entry.id = item.itemId;
    if (entry.createdAtMs == 0)
        entry.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    m_entries.append(entry);
    externalizeProviderItem(item);
    ProviderRecord record;
    record.item = std::move(item);
    record.entryId = entry.id;
    record.submitted = true;
    record.continuationId = continuationId;
    refreshTokenEstimate(record);
    m_providerRecords.append(std::move(record));
    return entry.id;
}

void ProviderRunLedger::setProviderItemForEntry(const QString &entryId,
                                                ProviderItem item,
                                                const QString &continuationId)
{
    if (item.itemId.isEmpty())
        item.itemId = entryId;
    if (ConversationMessage *entry = findById(entryId)) {
        const ConversationMessage projection =
            conversationEntryForItem(item, entry->turnId, continuationId);
        if (entry->text.isEmpty())
            entry->text = projection.text;
        if (projection.imageOutput.hasUri()
            || projection.imageOutput.hasInlineData()
            || projection.imageOutput.blobRef.hasBlobId()) {
            entry->imageOutput = projection.imageOutput;
        }
        if (item.kind == ProviderItemKind::Reasoning) {
            // 流式路径：UI 可能已累积 reasoningContent，完成态 item 却双空。
            // 不得用空投影覆盖已有正文/签名。
            if (!projection.reasoningContent.trimmed().isEmpty()
                || entry->reasoningContent.trimmed().isEmpty()) {
                entry->reasoningContent = projection.reasoningContent;
            }
            if (!projection.reasoningSignature.trimmed().isEmpty()
                || entry->reasoningSignature.trimmed().isEmpty()) {
                entry->reasoningSignature = projection.reasoningSignature;
            }
            entry->reasoningRedacted = projection.reasoningRedacted || entry->reasoningRedacted;
            entry->reasoningMustReplay =
                projection.reasoningMustReplay || entry->reasoningMustReplay;
            // 若合并后 UI 已可回放而 item 仍空，用 UI 回填线路 item。
            if (!isReplayableReasoningItem(item)
                && (entry->reasoningRedacted
                    || !entry->reasoningContent.trimmed().isEmpty()
                    || !entry->reasoningSignature.trimmed().isEmpty())) {
                item = ProviderItem::makeReasoning(entry->reasoningContent,
                                                   entry->reasoningSignature,
                                                   entry->reasoningRedacted,
                                                   entry->reasoningMustReplay);
                item.itemId = entryId;
            }
        }
        entry->providerMustReplay = projection.providerMustReplay;
        if (projection.kind == ConversationMessage::Kind::ToolCall) {
            // 流式 ToolCallCompleted 可能已写入完整 input/raw；MessageCompleted 的
            // fallback FunctionCall 若仍空参，不得覆盖（否则下一轮 arguments=""）。
            preferIncoming(entry->toolCall.id, projection.toolCall.id);
            preferIncoming(entry->toolCall.toolName, projection.toolCall.toolName);
            preferIncoming(entry->toolCall.input, projection.toolCall.input);
            preferIncomingTrimmed(entry->toolCall.rawInputJson, projection.toolCall.rawInputJson);
            preferIncoming(entry->toolCall.callerType, projection.toolCall.callerType);
            preferIncoming(entry->toolCall.callerId, projection.toolCall.callerId);
            preferIncoming(entry->toolUseId, projection.toolUseId);
            preferIncoming(entry->toolName, projection.toolName);
            if (entry->toolInput.isEmpty() && !entry->toolCall.input.isEmpty())
                entry->toolInput = entry->toolCall.input;
            // 线路 item 若空参而 UI 已有完整 toolCall，用 UI 回填再落 ProviderRecord。
            if (item.rawArguments.trimmed().isEmpty() && item.arguments.isEmpty()
                && (!entry->toolCall.rawInputJson.trimmed().isEmpty()
                    || !entry->toolCall.input.isEmpty())) {
                item = ProviderItem::makeFunctionCall(entry->toolCall);
                item.itemId = entryId;
            }
        } else if (projection.kind == ConversationMessage::Kind::ToolResult) {
            entry->toolUseId = projection.toolUseId;
            entry->toolName = projection.toolName;
        }
        if (!continuationId.isEmpty())
            entry->providerContinuationId = continuationId;
    }
    // 不可回放的空推理：清掉线路记录，保留 UI。
    if (!isReplayableReasoningItem(item)) {
        clearProviderRecord(entryId);
        return;
    }
    externalizeProviderItem(item);
    if (ProviderRecord *existing = findProviderRecord(entryId)) {
        existing->item = std::move(item);
        existing->submitted = true;
        if (!continuationId.isEmpty())
            existing->continuationId = continuationId;
        refreshTokenEstimate(*existing);
    } else {
        ProviderRecord record;
        record.item = std::move(item);
        record.entryId = entryId;
        record.submitted = true;
        record.continuationId = continuationId;
        refreshTokenEstimate(record);
        m_providerRecords.append(std::move(record));
    }
}

bool ProviderRunLedger::clearProviderRecord(const QString &entryId)
{
    bool removed = false;
    for (qsizetype i = m_providerRecords.size() - 1; i >= 0; --i) {
        if (m_providerRecords.at(i).entryId == entryId) {
            m_providerRecords.removeAt(i);
            removed = true;
        }
    }
    return removed;
}

bool ProviderRunLedger::removeEntry(const QString &entryId)
{
    bool removed = false;
    for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
        if (m_entries.at(i).id == entryId) {
            m_entries.removeAt(i);
            removed = true;
        }
    }
    for (qsizetype i = m_providerRecords.size() - 1; i >= 0; --i) {
        if (m_providerRecords.at(i).entryId == entryId)
            m_providerRecords.removeAt(i);
    }
    return removed;
}

const QList<ProviderItem> ProviderRunLedger::providerItems() const
{
    QList<ProviderItem> items;
    items.reserve(m_providerRecords.size());
    for (const ProviderRecord &record : m_providerRecords)
        if (!record.compacted)
            items.append(record.item);
    return items;
}

ConversationMessage *ProviderRunLedger::findLatestReasoningForTurn(const QString &turnId)
{
    for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
        ConversationMessage &entry = m_entries[i];
        if (entry.kind == ConversationMessage::Kind::AssistantReasoning
            && (turnId.isEmpty() || entry.turnId == turnId)) {
            return &entry;
        }
    }
    return nullptr;
}

const ConversationMessage *ProviderRunLedger::findLatestReasoningForTurn(const QString &turnId) const
{
    for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
        const ConversationMessage &entry = m_entries.at(i);
        if (entry.kind == ConversationMessage::Kind::AssistantReasoning
            && (turnId.isEmpty() || entry.turnId == turnId)) {
            return &entry;
        }
    }
    return nullptr;
}

ProviderRunLedger::ProviderRecord *ProviderRunLedger::findProviderRecord(const QString &entryId)
{
    for (ProviderRecord &record : m_providerRecords)
        if (record.entryId == entryId)
            return &record;
    return nullptr;
}

const ProviderRunLedger::ProviderRecord *ProviderRunLedger::findProviderRecord(
    const QString &entryId) const
{
    for (const ProviderRecord &record : m_providerRecords)
        if (record.entryId == entryId)
            return &record;
    return nullptr;
}

ConversationMessage *ProviderRunLedger::findById(const QString &entryId)
{
    for (ConversationMessage &entry : m_entries) {
        if (entry.id == entryId) {
            return &entry;
        }
    }
    return nullptr;
}

const ConversationMessage *ProviderRunLedger::findById(const QString &entryId) const
{
    for (const ConversationMessage &entry : m_entries) {
        if (entry.id == entryId) {
            return &entry;
        }
    }
    return nullptr;
}

ConversationMessage *ProviderRunLedger::findToolCallByUseId(const QString &toolUseId)
{
    for (ConversationMessage &entry : m_entries) {
        if (entry.kind == ConversationMessage::Kind::ToolCall
            && entry.toolUseId == toolUseId) {
            return &entry;
        }
    }
    return nullptr;
}

const ConversationMessage *ProviderRunLedger::findToolCallByUseId(const QString &toolUseId) const
{
    for (const ConversationMessage &entry : m_entries) {
        if (entry.kind == ConversationMessage::Kind::ToolCall
            && entry.toolUseId == toolUseId) {
            return &entry;
        }
    }
    return nullptr;
}

bool ProviderRunLedger::hasUnresolvedToolCalls() const
{
    const QSet<QString> resolvedIds = collectResolvedToolUseIds(m_entries);
    for (const ConversationMessage &entry : m_entries) {
        if (entry.kind == ConversationMessage::Kind::ToolCall
            && !entry.toolUseId.isEmpty()
            && !resolvedIds.contains(entry.toolUseId)) {
            return true;
        }
    }
    return false;
}

void ProviderRunLedger::rollbackUncommittedTurn(const QString &turnId)
{
    if (turnId.isEmpty()) {
        return;
    }

    // 已有 ToolResult 的 toolUse 视为已提交，取消时保留；其余未提交条目物理抹除
    const QSet<QString> resolvedToolUseIds = collectResolvedToolUseIds(m_entries);
    QSet<QString> removedIds;
    for (qsizetype i = m_entries.size() - 1; i >= 0; --i) {
        const ConversationMessage &entry = m_entries.at(i);
        if (entry.turnId != turnId) {
            continue;
        }

        bool shouldRemove = false;
        if (entry.kind == ConversationMessage::Kind::ToolCall) {
            shouldRemove = entry.toolUseId.isEmpty()
                           || !resolvedToolUseIds.contains(entry.toolUseId);
        } else if (entry.kind == ConversationMessage::Kind::AssistantText
                   || entry.kind == ConversationMessage::Kind::AssistantReasoning) {
            shouldRemove = !entry.submittedToModel;
        }
        if (!shouldRemove) {
            continue;
        }

        removedIds.insert(entry.id);
        m_entries.removeAt(i);
    }

    if (removedIds.isEmpty()) {
        return;
    }

    for (qsizetype i = m_providerRecords.size() - 1; i >= 0; --i) {
        if (removedIds.contains(m_providerRecords.at(i).entryId)) {
            m_providerRecords.removeAt(i);
        }
    }
}

void ProviderRunLedger::markSubmitted(const QList<QString> &entryIds)
{
    for (const QString &entryId : entryIds) {
        if (ConversationMessage *entry = findById(entryId)) {
            entry->submittedToModel = true;
        }
        if (ProviderRecord *record = findProviderRecord(entryId))
            record->submitted = true;
    }
}

void ProviderRunLedger::markEntriesCompacted(const QList<QString> &entryIds)
{
    if (entryIds.isEmpty()) {
        return;
    }
    for (const QString &entryId : entryIds) {
        if (ConversationMessage *entry = findById(entryId)) {
            entry->wasCompacted = true;
        }
        if (ProviderRecord *record = findProviderRecord(entryId)) {
            record->compacted = true;
            record->tokenEstimate = 0;
        }
    }
    // 压缩后上一轮厂商 input 反映的是压缩前占用，作下界会永久抬高估算
    m_lastProviderInputTokens = 0;
}

ProviderRequestBuild ProviderRunLedger::buildRequest(const QList<ProviderToolSpecification> &tools,
                                                     const ProviderOutputSpec &desiredOutput,
                                                     const QString &conversationId) const
{
    ProviderRequestBuild build;
    build.request.conversationId = conversationId;
    build.request.tools = tools;
    build.request.desiredOutput = desiredOutput;
    for (const ProviderRecord &record : m_providerRecords) {
        if (record.compacted)
            continue;
        // 空推理条目不可回放，必须在此丢弃（最后一道闸）。
        // 成因：ensureStreamingReasoningEntry / MessageCompleted 可能留下双空
        // Reasoning 记录；下一步请求带上它 ⇒ invalid_provider_request
        // 「reasoning requires content or signature」，工具轮次 step≥2 整体失败。
        if (!isReplayableReasoningItem(record.item))
            continue;
        build.request.items.append(hydratedProviderItem(record.item));
        if (!record.submitted)
            build.submittedEntryIds.append(record.entryId);
        // 默认无状态全量回放：账本可保留 continuationId 元数据，但请求不自动提升。
        // 有状态续跑（previous_response_id / previous_interaction_id）仅允许调用方显式
        // 写入 build.request.continuationId；adapter 见空则编码全部 items。
    }
    return build;
}

qint64 ProviderRunLedger::estimatedContextTokens(const qint64 requestOverheadTokens) const
{
    // 直接扫 providerRecords：不 buildRequest、不 hydrate blob——估算只读文本/元数据
    qint64 total = qMax<qint64>(0, requestOverheadTokens);
    for (const ProviderRecord &record : m_providerRecords) {
        if (record.compacted)
            continue;
        if (!isReplayableReasoningItem(record.item))
            continue;
        qint64 tokens = record.tokenEstimate;
        if (tokens < 0) {
            tokens = estimateContextTokensForItem(record.item);
            record.tokenEstimate = tokens;
        }
        total += tokens;
    }
    // 厂商上一轮 input_tokens 作下界：本地若系统性低估，至少不低于已证实的占用
    if (m_lastProviderInputTokens > 0) {
        total = qMax(total, m_lastProviderInputTokens);
    }
    return total;
}

void ProviderRunLedger::noteProviderInputTokens(const qint64 inputTokens)
{
    if (inputTokens > 0) {
        m_lastProviderInputTokens = inputTokens;
    }
}

void ProviderRunLedger::refreshTokenEstimate(ProviderRecord &record)
{
    if (!isReplayableReasoningItem(record.item) || record.compacted) {
        record.tokenEstimate = 0;
        return;
    }
    record.tokenEstimate = estimateContextTokensForItem(record.item);
}

QList<ConversationMessage> ProviderRunLedger::projectMessages() const
{
    QList<ConversationMessage> messages;
    messages.reserve(m_entries.size());

    for (const ConversationMessage &entry : m_entries) {
        if (entry.kind == ConversationMessage::Kind::SystemPrompt
            || entry.kind == ConversationMessage::Kind::Summary
            || entry.kind == ConversationMessage::Kind::SkillInvoke) {
            continue;
        }
        ConversationMessage message = entry;
        messages.append(message);
    }

    return messages;
}

QJsonArray ProviderRunLedger::toJson() const
{
    QJsonArray array;
    for (const ConversationMessage &entry : m_entries) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), entry.id);
        object.insert(QStringLiteral("kind"), ConversationMessageText::storageKind(entry.kind));
        object.insert(QStringLiteral("status"), statusToString(entry.status));
        object.insert(QStringLiteral("text"), entry.text);
        object.insert(QStringLiteral("toolName"), entry.toolName);
        object.insert(QStringLiteral("toolUseId"), entry.toolUseId);
        object.insert(QStringLiteral("toolInput"), entry.toolInput);
        object.insert(QStringLiteral("toolPayloadType"), entry.toolPayloadType);
        object.insert(QStringLiteral("toolPayload"), entry.toolPayload);
        object.insert(QStringLiteral("summaryText"), entry.summaryText);
        object.insert(QStringLiteral("progressText"), entry.progressText);
        object.insert(QStringLiteral("previewText"), entry.previewText);
        object.insert(QStringLiteral("persistedPath"), entry.persistedPath);
        object.insert(QStringLiteral("groupKey"), entry.groupKey);
        object.insert(QStringLiteral("turnId"), entry.turnId);
        object.insert(QStringLiteral("responseId"), entry.responseId);
        object.insert(QStringLiteral("inputTokens"), entry.inputTokens);
        object.insert(QStringLiteral("outputTokens"), entry.outputTokens);
        object.insert(QStringLiteral("cacheReadTokens"), entry.cacheReadTokens);
        object.insert(QStringLiteral("cacheCreationTokens"), entry.cacheCreationTokens);
        object.insert(QStringLiteral("thoughtTokens"), entry.thoughtTokens);
        object.insert(QStringLiteral("isError"), entry.isError);
        object.insert(QStringLiteral("wasPersisted"), entry.wasPersisted);
        object.insert(QStringLiteral("wasTruncated"), entry.wasTruncated);
        object.insert(QStringLiteral("resultCategory"), resultCategoryToString(entry.resultCategory));
        object.insert(QStringLiteral("createdAtMs"), QString::number(entry.createdAtMs));
        object.insert(QStringLiteral("reasoningContent"), entry.reasoningContent);
        object.insert(QStringLiteral("reasoningSignature"), entry.reasoningSignature);
        object.insert(QStringLiteral("reasoningRedacted"), entry.reasoningRedacted);
        object.insert(QStringLiteral("reasoningMustReplay"), entry.reasoningMustReplay);
        object.insert(QStringLiteral("providerMustReplay"), entry.providerMustReplay);
        object.insert(QStringLiteral("providerContinuationId"), entry.providerContinuationId);
        object.insert(QStringLiteral("providerLogprobs"), entry.providerLogprobs);
        object.insert(QStringLiteral("submittedToModel"), entry.submittedToModel);
        object.insert(QStringLiteral("wasCompacted"), entry.wasCompacted);
        object.insert(QStringLiteral("toolCall"), toolCallToJson(entry.toolCall));
        object.insert(QStringLiteral("attachedFilePaths"),
                      QJsonArray::fromStringList(entry.attachedFilePaths));
        if (const ProviderRecord *record = findProviderRecord(entry.id)) {
            QString ledgerError;
            if (record->item.validate(&ledgerError, true, 0)) {
                object.insert(QStringLiteral("providerProtocolVersion"), kProviderProtocolVersion);
                object.insert(QStringLiteral("providerProtocolRevision"), kProviderProtocolRevision);
                object.insert(QStringLiteral("providerItem"), providerItemToJson(record->item));
            } else {
                qWarning().noquote()
                    << QStringLiteral("ProviderRunLedger 拒绝持久化非法 ProviderItem：%1")
                           .arg(ledgerError);
            }
        }
        array.append(object);
    }
    return array;
}

void ProviderRunLedger::fromJson(const QJsonArray &entriesArray)
{
    m_entries.clear();
    m_providerRecords.clear();
    for (const QJsonValue &value : entriesArray) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        ConversationMessage entry;
        entry.id = object.value(QStringLiteral("id")).toString();
        entry.kind = ConversationMessageText::kindFromStorage(object.value(QStringLiteral("kind")).toString());
        entry.status = statusFromString(object.value(QStringLiteral("status")).toString());
        entry.text = object.value(QStringLiteral("text")).toString();
        entry.toolName = object.value(QStringLiteral("toolName")).toString();
        entry.toolUseId = object.value(QStringLiteral("toolUseId")).toString();
        entry.toolInput = object.value(QStringLiteral("toolInput")).toObject();
        entry.toolPayloadType = object.value(QStringLiteral("toolPayloadType")).toString();
        entry.toolPayload = object.value(QStringLiteral("toolPayload")).toObject();
        entry.summaryText = object.value(QStringLiteral("summaryText")).toString();
        entry.progressText = object.value(QStringLiteral("progressText")).toString();
        entry.previewText = object.value(QStringLiteral("previewText")).toString();
        entry.persistedPath = object.value(QStringLiteral("persistedPath")).toString();
        entry.groupKey = object.value(QStringLiteral("groupKey")).toString();
        entry.turnId = object.value(QStringLiteral("turnId")).toString();
        entry.responseId = object.value(QStringLiteral("responseId")).toString();
        entry.inputTokens = object.value(QStringLiteral("inputTokens")).toInt();
        entry.outputTokens = object.value(QStringLiteral("outputTokens")).toInt();
        entry.cacheReadTokens = object.value(QStringLiteral("cacheReadTokens")).toInt();
        entry.cacheCreationTokens = object.value(QStringLiteral("cacheCreationTokens")).toInt();
        entry.thoughtTokens = object.value(QStringLiteral("thoughtTokens")).toInt();
        entry.isError = object.value(QStringLiteral("isError")).toBool(false);
        entry.wasPersisted = object.value(QStringLiteral("wasPersisted")).toBool(false);
        entry.wasTruncated = object.value(QStringLiteral("wasTruncated")).toBool(false);
        entry.resultCategory = resultCategoryFromString(object.value(QStringLiteral("resultCategory")).toString());
        entry.createdAtMs = object.value(QStringLiteral("createdAtMs")).toString().toLongLong();
        entry.reasoningContent = object.value(QStringLiteral("reasoningContent")).toString();
        entry.reasoningSignature = object.value(QStringLiteral("reasoningSignature")).toString();
        entry.reasoningRedacted = object.value(QStringLiteral("reasoningRedacted")).toBool(false);
        entry.reasoningMustReplay = object.value(QStringLiteral("reasoningMustReplay")).toBool(false);
        entry.providerMustReplay = object.value(QStringLiteral("providerMustReplay")).toBool(false);
        entry.providerContinuationId = object.value(QStringLiteral("providerContinuationId")).toString();
        entry.providerLogprobs = object.value(QStringLiteral("providerLogprobs")).toObject();
        entry.submittedToModel = object.value(QStringLiteral("submittedToModel")).toBool(false);
        entry.wasCompacted = object.value(QStringLiteral("wasCompacted")).toBool(false);
        entry.toolCall = toolCallFromJson(object.value(QStringLiteral("toolCall")).toObject());
        const QJsonArray attachedArr = object.value(QStringLiteral("attachedFilePaths")).toArray();
        for (const QJsonValue &v : attachedArr) {
            if (v.isString()) entry.attachedFilePaths.append(v.toString());
        }
        // providerItem 是协议历史的唯一权威源。旧记录若没有 providerItem，
        // 仍可作为 UI 投影展示，但不得再从 ConversationMessage 反向猜测线路语义。
        if (entry.id.trimmed().isEmpty())
            entry.id = nextLedgerEntryId();
        if (entry.createdAtMs == 0)
            entry.createdAtMs = QDateTime::currentMSecsSinceEpoch();
        // SessionEvent 只服务 UI；恢复时丢弃历史误写的 providerItem，避免切开 tool pair
        const bool refuseProviderWire =
            entry.kind == ConversationMessage::Kind::SessionEvent;
        const QString entryId = entry.id;
        m_entries.append(std::move(entry));
        const QJsonObject providerItemObject =
            object.value(QStringLiteral("providerItem")).toObject();
        if (!refuseProviderWire && !providerItemObject.isEmpty()) {
            ProviderItem item = providerItemFromJson(providerItemObject);
            setProviderItemForEntry(
                entryId, std::move(item),
                object.value(QStringLiteral("providerContinuationId")).toString());
            if (ProviderRecord *record = findProviderRecord(entryId)) {
                record->submitted = object.value(QStringLiteral("submittedToModel")).toBool(false);
                record->compacted = object.value(QStringLiteral("wasCompacted")).toBool(false);
                // setProviderItem 已刷新估算；若历史条目已压缩则归零
                if (record->compacted) {
                    record->tokenEstimate = 0;
                }
            }
        }
    }
}
