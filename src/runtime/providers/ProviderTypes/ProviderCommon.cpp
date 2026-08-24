#include "ProviderCommon.h"

#include <cstddef>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

namespace {

void setParseOk(bool *ok, bool value)
{
    if (ok) {
        *ok = value;
    }
}

/// wire 字符串与 toString() 一致时的通用解析（小写；空串匹配 Unset 的空 toString）
template<typename Enum, std::size_t N>
Enum parseProviderEnum(const QString &text, const Enum (&values)[N], Enum fallback, bool *ok)
{
    const QString key = text.trimmed().toLower();
    for (Enum value : values) {
        if (toString(value) == key) {
            setParseOk(ok, true);
            return value;
        }
    }
    setParseOk(ok, false);
    return fallback;
}

/// 白名单键：非空 + 可选 x-/ext./adapter. 前缀 + 精确集合
bool isAllowedByPrefixOrSet(const QString &key,
                            const QSet<QString> &allow,
                            bool allowExtAdapterPrefix = false)
{
    const QString k = key.trimmed();
    if (k.isEmpty()) {
        return false;
    }
    if (k.startsWith(QStringLiteral("x-"), Qt::CaseInsensitive)) {
        return true;
    }
    if (allowExtAdapterPrefix
        && (k.startsWith(QStringLiteral("ext."), Qt::CaseInsensitive)
            || k.startsWith(QStringLiteral("adapter."), Qt::CaseInsensitive))) {
        return true;
    }
    return allow.contains(k);
}

/// 浅校验 JSON 对象顶层键均在 isAllowed 白名单内
bool validateObjectKeys(const QJsonObject &object,
                        bool (*isAllowed)(const QString &),
                        QString *error,
                        const QString &label)
{
    if (object.isEmpty()) {
        return true;
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!isAllowed(it.key())) {
            if (error) {
                *error = QStringLiteral("%1 key not allowed: %2").arg(label, it.key());
            }
            return false;
        }
    }
    return true;
}

/// fromBlob 时若 scheme 未指定则补为 Blob
ProviderBlobRef withBlobScheme(ProviderBlobRef blob)
{
    if (blob.scheme == ProviderUriScheme::Unset) {
        blob.scheme = ProviderUriScheme::Blob;
    }
    return blob;
}

} // namespace

// ── ProviderBlobRef ──

bool ProviderBlobRef::isEmpty() const
{
    return blobId.trimmed().isEmpty()
           && contentHash.trimmed().isEmpty()
           && byteSize <= 0
           && scheme == ProviderUriScheme::Unset;
}

bool ProviderBlobRef::hasBlobId() const
{
    return !blobId.trimmed().isEmpty();
}

// ── ProviderImageAsset ──

ProviderImageAsset ProviderImageAsset::fromUrl(const QString &uri,
                                               const QString &mimeType,
                                               const QString &altText)
{
    ProviderImageAsset asset;
    asset.uri = uri;
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

ProviderImageAsset ProviderImageAsset::fromBytes(const QByteArray &data,
                                                 const QString &mimeType,
                                                 const QString &altText)
{
    ProviderImageAsset asset;
    asset.data = data;
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

ProviderImageAsset ProviderImageAsset::fromBlob(const ProviderBlobRef &blob,
                                                const QString &mimeType,
                                                const QString &altText)
{
    ProviderImageAsset asset;
    asset.blobRef = withBlobScheme(blob);
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

bool ProviderImageAsset::hasUri() const
{
    return !uri.trimmed().isEmpty();
}

bool ProviderImageAsset::hasInlineData() const
{
    return !data.isEmpty();
}

bool ProviderImageAsset::hasBlobRef() const
{
    return blobRef.hasBlobId();
}

bool ProviderImageAsset::isEmpty() const
{
    return !hasUri() && !hasInlineData() && !hasBlobRef();
}

// ── ProviderAudioAsset ──

ProviderAudioAsset ProviderAudioAsset::fromUrl(const QString &uri,
                                               const QString &mimeType,
                                               const QString &transcript)
{
    ProviderAudioAsset asset;
    asset.uri = uri;
    asset.mimeType = mimeType;
    asset.transcript = transcript;
    return asset;
}

ProviderAudioAsset ProviderAudioAsset::fromBytes(const QByteArray &data,
                                                 const QString &mimeType,
                                                 const QString &transcript)
{
    ProviderAudioAsset asset;
    asset.data = data;
    asset.mimeType = mimeType;
    asset.transcript = transcript;
    return asset;
}

ProviderAudioAsset ProviderAudioAsset::fromBlob(const ProviderBlobRef &blob,
                                                const QString &mimeType,
                                                const QString &transcript)
{
    ProviderAudioAsset asset;
    asset.blobRef = withBlobScheme(blob);
    asset.mimeType = mimeType;
    asset.transcript = transcript;
    return asset;
}

bool ProviderAudioAsset::hasUri() const
{
    return !uri.trimmed().isEmpty();
}

bool ProviderAudioAsset::hasInlineData() const
{
    return !data.isEmpty();
}

bool ProviderAudioAsset::hasBlobRef() const
{
    return blobRef.hasBlobId();
}

bool ProviderAudioAsset::isEmpty() const
{
    return !hasUri() && !hasInlineData() && !hasBlobRef();
}

// ── ProviderVideoAsset ──

ProviderVideoAsset ProviderVideoAsset::fromUrl(const QString &uri,
                                               const QString &mimeType,
                                               const QString &altText)
{
    ProviderVideoAsset asset;
    asset.uri = uri;
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

ProviderVideoAsset ProviderVideoAsset::fromBytes(const QByteArray &data,
                                                 const QString &mimeType,
                                                 const QString &altText)
{
    ProviderVideoAsset asset;
    asset.data = data;
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

ProviderVideoAsset ProviderVideoAsset::fromBlob(const ProviderBlobRef &blob,
                                                const QString &mimeType,
                                                const QString &altText)
{
    ProviderVideoAsset asset;
    asset.blobRef = withBlobScheme(blob);
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

bool ProviderVideoAsset::hasUri() const
{
    return !uri.trimmed().isEmpty();
}

bool ProviderVideoAsset::hasInlineData() const
{
    return !data.isEmpty();
}

bool ProviderVideoAsset::hasBlobRef() const
{
    return blobRef.hasBlobId();
}

bool ProviderVideoAsset::isEmpty() const
{
    return !hasUri() && !hasInlineData() && !hasBlobRef();
}

// ── ProviderError ──

bool ProviderError::isValid() const
{
    return !code.trimmed().isEmpty() || !message.trimmed().isEmpty();
}

// ── 工具函数 ──

QString extractApiErrorMessage(const QByteArray &body)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        // 非 JSON 时截一段正文，避免只剩 Qt 的 "server replied: Bad Request"
        const QByteArray trimmed = body.trimmed();
        if (trimmed.isEmpty())
            return {};
        const QString text = QString::fromUtf8(trimmed.left(400)).simplified();
        return text;
    }

    // 优先 error 对象的 message，其次 error 字符串，最后顶层 message/msg
    const QJsonObject root = document.object();
    const QJsonValue errorValue = root.value(QStringLiteral("error"));
    if (errorValue.isObject()) {
        const QJsonObject err = errorValue.toObject();
        QString message = err.value(QStringLiteral("message")).toString().trimmed();
        if (message.isEmpty())
            message = err.value(QStringLiteral("msg")).toString().trimmed();
        // type 优先，否则 code；已写进 message 则不重复附加
        QString detail = err.value(QStringLiteral("type")).toString().trimmed();
        if (detail.isEmpty())
            detail = err.value(QStringLiteral("code")).toString().trimmed();
        if (!message.isEmpty() && !detail.isEmpty() && !message.contains(detail))
            message = QStringLiteral("%1（%2）").arg(message, detail);
        return message;
    }
    if (errorValue.isString())
        return errorValue.toString().trimmed();

    QString top = root.value(QStringLiteral("message")).toString().trimmed();
    if (top.isEmpty())
        top = root.value(QStringLiteral("msg")).toString().trimmed();
    return top;
}

QString compactJson(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isArray())
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    if (value.isObject())
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    return value.toVariant().toString();
}

// ── 推断 / 白名单 / 能力 ──

ProviderUriScheme inferUriScheme(const QString &uri)
{
    const QString trimmed = uri.trimmed();
    if (trimmed.isEmpty()) {
        return ProviderUriScheme::Unset;
    }
    if (trimmed.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        return ProviderUriScheme::Https;
    }
    if (trimmed.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) {
        return ProviderUriScheme::Http;
    }
    if (trimmed.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
        return ProviderUriScheme::File;
    }
    if (trimmed.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive)) {
        return ProviderUriScheme::Data;
    }
    // 厂商 file id 通常无 scheme；由调用方显式设 ProviderFile / Blob
    return ProviderUriScheme::Unset;
}

bool isKnownServerToolName(const QString &name)
{
    static const QSet<QString> kNames{
        QStringLiteral("web_search"),
        QStringLiteral("file_search"),
        QStringLiteral("code_interpreter"),
        QStringLiteral("computer"),
        QStringLiteral("web_fetch"),
        QStringLiteral("image_generation"),
        QStringLiteral("mcp"),
        QStringLiteral("mcp_list_tools"),
        QStringLiteral("tool_search"),
        QStringLiteral("local_shell"),
        QStringLiteral("shell"),
        QStringLiteral("apply_patch"),
        QStringLiteral("advisor"),
        QStringLiteral("url_context"),
        QStringLiteral("google_maps"),
    };
    return kNames.contains(name.trimmed());
}

QString toString(ProviderCallerKind kind)
{
    switch (kind) {
    case ProviderCallerKind::Unset:
        return {};
    case ProviderCallerKind::Direct:
        return QStringLiteral("direct");
    case ProviderCallerKind::Program:
        return QStringLiteral("program");
    case ProviderCallerKind::CodeExecution:
        return QStringLiteral("code_execution");
    }
    return {};
}

ProviderCallerKind parseCallerKind(const QString &text, bool *ok)
{
    static const ProviderCallerKind kAll[] = {
        ProviderCallerKind::Unset,
        ProviderCallerKind::Direct,
        ProviderCallerKind::Program,
        ProviderCallerKind::CodeExecution,
    };
    // Unset 的 toString 为空串，故空输入合法映射到 Unset
    return parseProviderEnum(text, kAll, ProviderCallerKind::Unset, ok);
}

bool isKnownCallerKind(ProviderCallerKind kind)
{
    switch (kind) {
    case ProviderCallerKind::Unset:
    case ProviderCallerKind::Direct:
    case ProviderCallerKind::Program:
    case ProviderCallerKind::CodeExecution:
        return true;
    }
    return false;
}

QString toString(ProviderReasoningEffort effort)
{
    switch (effort) {
    case ProviderReasoningEffort::Unset:
        return {};
    case ProviderReasoningEffort::Minimal:
        return QStringLiteral("minimal");
    case ProviderReasoningEffort::Low:
        return QStringLiteral("low");
    case ProviderReasoningEffort::Medium:
        return QStringLiteral("medium");
    case ProviderReasoningEffort::High:
        return QStringLiteral("high");
    case ProviderReasoningEffort::Max:
        return QStringLiteral("max");
    case ProviderReasoningEffort::XHigh:
        return QStringLiteral("xhigh");
    }
    return {};
}

ProviderReasoningEffort parseReasoningEffort(const QString &text, bool *ok)
{
    static const ProviderReasoningEffort kAll[] = {
        ProviderReasoningEffort::Unset,
        ProviderReasoningEffort::Minimal,
        ProviderReasoningEffort::Low,
        ProviderReasoningEffort::Medium,
        ProviderReasoningEffort::High,
        ProviderReasoningEffort::Max,
        ProviderReasoningEffort::XHigh,
    };
    return parseProviderEnum(text, kAll, ProviderReasoningEffort::Unset, ok);
}

bool isKnownReasoningEffort(ProviderReasoningEffort effort)
{
    switch (effort) {
    case ProviderReasoningEffort::Unset:
    case ProviderReasoningEffort::Minimal:
    case ProviderReasoningEffort::Low:
    case ProviderReasoningEffort::Medium:
    case ProviderReasoningEffort::High:
    case ProviderReasoningEffort::Max:
    case ProviderReasoningEffort::XHigh:
        return true;
    }
    return false;
}

ProviderProtocolFamily protocolFamilyForProviderType(const QString &providerType)
{
    const QString key = providerType.trimmed().toLower();
    if (key == QStringLiteral("responses"))
        return ProviderProtocolFamily::OpenAiResponses;
    if (key == QStringLiteral("chat-completions"))
        return ProviderProtocolFamily::OpenAiChatCompletions;
    if (key == QStringLiteral("deepseek"))
        return ProviderProtocolFamily::DeepSeekChatCompletions;
    if (key == QStringLiteral("anthropic"))
        return ProviderProtocolFamily::AnthropicMessages;
    if (key == QStringLiteral("google"))
        return ProviderProtocolFamily::GeminiGenerateContent;
    if (key == QStringLiteral("google-interactions"))
        return ProviderProtocolFamily::GeminiInteractions;
    return ProviderProtocolFamily::Auto;
}

QString toString(ProviderItemStatus status)
{
    switch (status) {
    case ProviderItemStatus::Unset:
        return {};
    case ProviderItemStatus::InProgress:
        return QStringLiteral("in_progress");
    case ProviderItemStatus::Completed:
        return QStringLiteral("completed");
    case ProviderItemStatus::Incomplete:
        return QStringLiteral("incomplete");
    case ProviderItemStatus::Failed:
        return QStringLiteral("failed");
    case ProviderItemStatus::Calling:
        return QStringLiteral("calling");
    }
    return {};
}

ProviderItemStatus parseItemStatus(const QString &text, bool *ok)
{
    static const ProviderItemStatus kAll[] = {
        ProviderItemStatus::Unset,
        ProviderItemStatus::InProgress,
        ProviderItemStatus::Completed,
        ProviderItemStatus::Incomplete,
        ProviderItemStatus::Failed,
        ProviderItemStatus::Calling,
    };
    return parseProviderEnum(text, kAll, ProviderItemStatus::Unset, ok);
}

bool isAllowedRequestMetadataKey(const QString &key)
{
    static const QSet<QString> kAllow{
        QStringLiteral("trace_id"),
        QStringLiteral("client_request_id"),
        QStringLiteral("debug"),
        QStringLiteral("priority"),
    };
    return isAllowedByPrefixOrSet(key, kAllow, /*allowExtAdapterPrefix=*/true);
}

bool isAllowedServerToolDetailKey(const QString &key)
{
    static const QSet<QString> kAllow{
        QStringLiteral("query"),
        QStringLiteral("results"),
        QStringLiteral("url"),
        QStringLiteral("title"),
        QStringLiteral("snippet"),
        QStringLiteral("code"),
        QStringLiteral("output"),
        QStringLiteral("status"),
        QStringLiteral("error"),
        QStringLiteral("sources"),
        QStringLiteral("language"),
    };
    return isAllowedByPrefixOrSet(key, kAllow);
}

bool ModelCapabilities::has(ProviderCapability c) const
{
    return flags.contains(c);
}

ModelCapabilities &ModelCapabilities::enable(ProviderCapability c)
{
    flags.insert(c);
    return *this;
}

ModelCapabilities &ModelCapabilities::disable(ProviderCapability c)
{
    flags.remove(c);
    return *this;
}

ModelCapabilities &ModelCapabilities::set(ProviderCapability c, const bool on)
{
    if (on) {
        flags.insert(c);
    } else {
        flags.remove(c);
    }
    return *this;
}

bool ModelCapabilities::supportsTextInput() const
{
    return has(ProviderCapability::TextInput);
}
bool ModelCapabilities::supportsImageInput() const
{
    return has(ProviderCapability::ImageInput);
}
bool ModelCapabilities::supportsAudioInput() const
{
    return has(ProviderCapability::AudioInput);
}
bool ModelCapabilities::supportsDocumentInput() const
{
    return has(ProviderCapability::DocumentInput);
}
bool ModelCapabilities::supportsVideoInput() const
{
    return has(ProviderCapability::VideoInput);
}
bool ModelCapabilities::supportsTextOutput() const
{
    return has(ProviderCapability::TextOutput);
}
bool ModelCapabilities::supportsImageOutput() const
{
    return has(ProviderCapability::ImageOutput);
}
bool ModelCapabilities::supportsAudioOutput() const
{
    return has(ProviderCapability::AudioOutput);
}
bool ModelCapabilities::supportsToolCalling() const
{
    return has(ProviderCapability::ToolCalling);
}
bool ModelCapabilities::supportsServerTools() const
{
    return has(ProviderCapability::ServerTools);
}
bool ModelCapabilities::supportsReasoning() const
{
    return has(ProviderCapability::Reasoning);
}
bool ModelCapabilities::supportsToolChoice() const
{
    return has(ProviderCapability::ToolChoice);
}
bool ModelCapabilities::supportsMaxOutputTokens() const
{
    return has(ProviderCapability::MaxOutputTokens);
}
bool ModelCapabilities::supportsCitations() const
{
    return has(ProviderCapability::Citations);
}
bool ModelCapabilities::supportsResponseFormat() const
{
    return has(ProviderCapability::ResponseFormat);
}
bool ModelCapabilities::supportsSamplingTopP() const
{
    return has(ProviderCapability::SamplingTopP);
}
bool ModelCapabilities::supportsSamplingSeed() const
{
    return has(ProviderCapability::SamplingSeed);
}
bool ModelCapabilities::supportsSamplingStop() const
{
    return has(ProviderCapability::SamplingStop);
}
bool ModelCapabilities::supportsPresencePenalty() const
{
    return has(ProviderCapability::PresencePenalty);
}
bool ModelCapabilities::supportsFrequencyPenalty() const
{
    return has(ProviderCapability::FrequencyPenalty);
}
bool ModelCapabilities::supportsPromptCache() const
{
    return has(ProviderCapability::PromptCache);
}
bool ModelCapabilities::supportsLogprobs() const
{
    return has(ProviderCapability::Logprobs);
}
bool ModelCapabilities::supportsBackgroundExecution() const
{
    return has(ProviderCapability::BackgroundExecution);
}
bool ModelCapabilities::supportsResponseInclude() const
{
    return has(ProviderCapability::ResponseInclude);
}
bool ModelCapabilities::supportsProgrammaticToolCalling() const
{
    return has(ProviderCapability::ProgrammaticToolCalling);
}
bool ModelCapabilities::supportsToolSearch() const
{
    return has(ProviderCapability::ToolSearch);
}
bool ModelCapabilities::supportsMcpApproval() const
{
    return has(ProviderCapability::McpApproval);
}
bool ModelCapabilities::supportsCompaction() const
{
    return has(ProviderCapability::Compaction);
}
bool ModelCapabilities::supportsTopK() const
{
    return has(ProviderCapability::TopK);
}
bool ModelCapabilities::supportsCachedContent() const
{
    return has(ProviderCapability::CachedContent);
}
bool ModelCapabilities::supportsMediaResolution() const
{
    return has(ProviderCapability::MediaResolution);
}
bool ModelCapabilities::supportsUrlContext() const
{
    return has(ProviderCapability::UrlContext);
}
bool ModelCapabilities::supportsGoogleMapsGrounding() const
{
    return has(ProviderCapability::GoogleMapsGrounding);
}
bool ModelCapabilities::supportsStatelessHistory() const
{
    return has(ProviderCapability::StatelessHistory);
}
bool ModelCapabilities::supportsContinuation() const
{
    return has(ProviderCapability::Continuation);
}

bool ModelCapabilities::supportsServerToolName(const QString &name) const
{
    if (!supportsServerTools()) {
        return false;
    }
    if (supportedServerTools.isEmpty()) {
        return true;
    }
    return supportedServerTools.contains(name.trimmed());
}

ModelCapabilities ModelCapabilities::textToolsBaseline(const QString &modelId)
{
    ModelCapabilities caps;
    caps.modelId = modelId;
    caps.flags = {
        ProviderCapability::TextInput,
        ProviderCapability::TextOutput,
        ProviderCapability::ToolCalling,
        ProviderCapability::ToolChoice,
        ProviderCapability::MaxOutputTokens,
        ProviderCapability::SamplingTopP,
        ProviderCapability::SamplingStop,
        ProviderCapability::StatelessHistory,
    };
    return caps;
}

ModelCapabilities ModelCapabilities::agentMultimodalBaseline(const QString &modelId)
{
    ModelCapabilities caps = textToolsBaseline(modelId);
    caps.enable(ProviderCapability::ImageInput)
        .enable(ProviderCapability::DocumentInput)
        .enable(ProviderCapability::AudioInput)
        .enable(ProviderCapability::VideoInput)
        .enable(ProviderCapability::Reasoning)
        .enable(ProviderCapability::ServerTools)
        .enable(ProviderCapability::Citations)
        .enable(ProviderCapability::ResponseFormat)
        .enable(ProviderCapability::Continuation);
    return caps;
}

bool isAllowedLogprobsKey(const QString &key)
{
    static const QSet<QString> kAllow{
        QStringLiteral("content"),
        QStringLiteral("token"),
        QStringLiteral("logprob"),
        QStringLiteral("top"),
        QStringLiteral("bytes"),
        QStringLiteral("top_logprobs"),
    };
    return isAllowedByPrefixOrSet(key, kAllow);
}

bool validateLogprobsObject(const QJsonObject &logprobs, QString *error)
{
    if (logprobs.isEmpty()) {
        return true;
    }
    bool hasContent = false;
    bool hasExt = false;
    for (auto it = logprobs.constBegin(); it != logprobs.constEnd(); ++it) {
        if (!isAllowedLogprobsKey(it.key())) {
            if (error) {
                *error = QStringLiteral("logprobs key not allowed: %1").arg(it.key());
            }
            return false;
        }
        if (it.key() == QStringLiteral("content")) {
            hasContent = true;
        }
        if (it.key().startsWith(QStringLiteral("x-"), Qt::CaseInsensitive)) {
            hasExt = true;
        }
    }
    // 非空 logprobs 须含 content 或 x- 扩展键（防空壳诊断对象）
    if (!hasContent && !hasExt) {
        if (error) {
            *error = QStringLiteral(
                "logprobs non-empty object must contain content or an x- extension key");
        }
        return false;
    }
    return true;
}

bool isAllowedVendorUsageRawKey(const QString &key)
{
    static const QSet<QString> kAllow{
        QStringLiteral("input_tokens"),
        QStringLiteral("output_tokens"),
        QStringLiteral("total_tokens"),
        QStringLiteral("prompt_tokens"),
        QStringLiteral("completion_tokens"),
        QStringLiteral("cache_read_tokens"),
        QStringLiteral("cache_write_tokens"),
        QStringLiteral("cached_tokens"),
        QStringLiteral("thoughts_tokens"),
        QStringLiteral("total_thought_tokens"),
        QStringLiteral("reasoning_tokens"),
    };
    return isAllowedByPrefixOrSet(key, kAllow);
}

bool validateVendorUsageRawObject(const QJsonObject &raw,
                                  QString *error,
                                  const bool forbidNonEmpty)
{
    if (raw.isEmpty()) {
        return true;
    }
    if (forbidNonEmpty) {
        if (error) {
            *error = QStringLiteral(
                "vendorUsageRaw must be empty on ledger/UI path; use portableUsage");
        }
        return false;
    }
    return validateObjectKeys(raw, isAllowedVendorUsageRawKey, error, QStringLiteral("vendorUsageRaw"));
}

bool isAllowedToolDetailsKey(const QString &key)
{
    if (isAllowedServerToolDetailKey(key)) {
        return true;
    }
    const QString k = key.trimmed();
    return k == QStringLiteral("summary")
           || k == QStringLiteral("payloadType")
           || k == QStringLiteral("hasPayload");
}

bool validateToolDetailsObject(const QJsonObject &details, QString *error)
{
    return validateObjectKeys(details, isAllowedToolDetailsKey, error, QStringLiteral("details"));
}
