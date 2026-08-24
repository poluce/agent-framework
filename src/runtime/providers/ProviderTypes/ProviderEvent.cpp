#include "ProviderEvent.h"

namespace {

/// 工厂统一：写入 kind + sequence 镜像
ProviderEvent makeEvent(ProviderEventKind kind, qint64 sequence)
{
    ProviderEvent event;
    event.kind = kind;
    event.sequence = sequence;
    return event;
}

/// delta 类事件：payload + base.sequence
ProviderEvent makeDeltaEvent(ProviderEventKind kind, const ProviderDeltaPayload &payload)
{
    ProviderEvent event = makeEvent(kind, payload.base.sequence);
    event.deltaPayload = payload;
    return event;
}

} // namespace

// ── ProviderEvent 工厂 ──

ProviderEvent ProviderEvent::messageStarted(const ProviderMessageStart &messageStart)
{
    ProviderEvent event = makeEvent(ProviderEventKind::MessageStarted, messageStart.sequence);
    event.messageStart = messageStart;
    // 故意不写 providerResponseId：messageId 是本地消息 id，不是厂商 continuation id
    return event;
}

ProviderEvent ProviderEvent::messageCompleted(const ProviderMessageEnd &messageEnd)
{
    ProviderEvent event = makeEvent(ProviderEventKind::MessageCompleted, messageEnd.sequence);
    event.messageEnd = messageEnd;
    // 不把 messageId 写入 providerResponseId
    return event;
}

ProviderEvent ProviderEvent::contentPartStarted(const ProviderContentPartStart &part)
{
    return makeDeltaEvent(ProviderEventKind::ContentPartStarted, part);
}

ProviderEvent ProviderEvent::contentPartCompleted(const ProviderContentPartEnd &part)
{
    return makeDeltaEvent(ProviderEventKind::ContentPartCompleted, part);
}

ProviderEvent ProviderEvent::fromTextDelta(const ProviderTextDelta &textDelta)
{
    return makeDeltaEvent(ProviderEventKind::TextDelta, textDelta);
}

ProviderEvent ProviderEvent::fromTextDelta(const QString &text)
{
    ProviderTextDelta delta;
    delta.text = text;
    delta.partKind = ProviderStreamPartKind::Text;
    return ProviderEvent::fromTextDelta(delta);
}

ProviderEvent ProviderEvent::fromReasoningDelta(const ProviderReasoningDelta &reasoningDelta)
{
    return makeDeltaEvent(ProviderEventKind::ReasoningDelta, reasoningDelta);
}

ProviderEvent ProviderEvent::toolCallStarted(const ProviderToolCallStart &toolCallStart)
{
    return makeDeltaEvent(ProviderEventKind::ToolCallStarted, toolCallStart);
}

ProviderEvent ProviderEvent::toolCallCompleted(const ProviderToolCallEnd &toolCallEnd)
{
    return makeDeltaEvent(ProviderEventKind::ToolCallCompleted, toolCallEnd);
}

ProviderEvent ProviderEvent::fromImageOutput(const ProviderImageOutput &imageOutput)
{
    return makeDeltaEvent(ProviderEventKind::ImageOutput, imageOutput);
}

ProviderEvent ProviderEvent::fromAudioDelta(const ProviderAudioDelta &audioDelta)
{
    return makeDeltaEvent(ProviderEventKind::AudioDelta, audioDelta);
}

ProviderEvent ProviderEvent::fromTranscriptDelta(const ProviderTranscriptDelta &transcriptDelta)
{
    return makeDeltaEvent(ProviderEventKind::TranscriptDelta, transcriptDelta);
}

ProviderEvent ProviderEvent::fromTranscriptDelta(const QString &text)
{
    ProviderTranscriptDelta delta;
    delta.text = text;
    delta.partKind = ProviderStreamPartKind::Transcript;
    return ProviderEvent::fromTranscriptDelta(delta);
}

ProviderEvent ProviderEvent::usageUpdated(const ProviderUsage &usage)
{
    ProviderEvent event = makeEvent(ProviderEventKind::UsageUpdated, usage.sequence);
    event.usage = usage;
    return event;
}

ProviderEvent ProviderEvent::responseMetadataUpdated(const ProviderResponseMetadata &metadata)
{
    ProviderEvent event = makeEvent(ProviderEventKind::ResponseMetadata, metadata.sequence);
    event.responseMetadata = metadata;
    event.providerResponseId = metadata.providerResponseId;
    return event;
}

ProviderEvent ProviderEvent::fromError(const ProviderError &error)
{
    ProviderEvent event = makeEvent(ProviderEventKind::Error, error.sequence);
    event.error = error;
    return event;
}

ProviderEvent ProviderEvent::fromError(const QString &code, const QString &message)
{
    ProviderError error;
    error.code = code;
    error.message = message;
    return ProviderEvent::fromError(error);
}

ProviderEvent ProviderEvent::cancelled()
{
    return makeEvent(ProviderEventKind::Cancelled, 0);
}

bool ProviderMessageEnd::validate(QString *error,
                                  const bool strictItems,
                                  const int maxInlineAssetBytes) const
{
    const auto fail = [error](const QString &msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };

    // logprobs 扩展位白名单
    if (!validateLogprobsObject(logprobs, error)) {
        return false;
    }

    if (!strictItems) {
        return true;
    }

    for (int i = 0; i < outputItems.size(); ++i) {
        QString itemError;
        // 完成态写账本：reasoning 单轨 + 内联策略
        if (!outputItems.at(i).validate(&itemError, true, maxInlineAssetBytes)) {
            return fail(QStringLiteral("outputItems[%1]: %2").arg(i).arg(itemError));
        }
    }
    return true;
}

bool ProviderResponseMetadata::validate(QString *error, const bool forbidVendorUsageRaw) const
{
    return validateVendorUsageRawObject(vendorUsageRaw, error, forbidVendorUsageRaw);
}
