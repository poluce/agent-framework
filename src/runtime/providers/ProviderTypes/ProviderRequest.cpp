#include "ProviderRequest.h"

#include <algorithm>

// ── OutputSpec / ToolChoice / Reasoning / Audio ──

ProviderOutputSpec ProviderOutputSpec::textOnly()
{
    return ProviderOutputSpec{true, false, false};
}

ProviderOutputSpec ProviderOutputSpec::textAndImages()
{
    return ProviderOutputSpec{true, true, false};
}

ProviderOutputSpec ProviderOutputSpec::textAndAudio()
{
    return ProviderOutputSpec{true, false, true};
}

ProviderOutputSpec ProviderOutputSpec::multimodal()
{
    return ProviderOutputSpec{true, true, true};
}

bool ProviderToolChoice::isExplicit() const
{
    return mode != ProviderToolChoiceMode::ProviderDefault
           || allowParallel != ProviderTriState::Unset;
}

ProviderToolChoice ProviderToolChoice::none()
{
    ProviderToolChoice choice;
    choice.mode = ProviderToolChoiceMode::None;
    return choice;
}

ProviderToolChoice ProviderToolChoice::autoChoice()
{
    ProviderToolChoice choice;
    choice.mode = ProviderToolChoiceMode::Auto;
    return choice;
}

ProviderToolChoice ProviderToolChoice::required()
{
    ProviderToolChoice choice;
    choice.mode = ProviderToolChoiceMode::Required;
    return choice;
}

ProviderToolChoice ProviderToolChoice::named(const QString &toolName)
{
    ProviderToolChoice choice;
    choice.mode = ProviderToolChoiceMode::Named;
    choice.toolName = toolName.trimmed();
    return choice;
}

bool ProviderReasoningOptions::isExplicit() const
{
    return requested || budgetTokens > 0 || includeSummary
           || effort != ProviderReasoningEffort::Unset;
}

ProviderReasoningOptions ProviderReasoningOptions::enabledOption()
{
    ProviderReasoningOptions options;
    options.requested = true;
    options.enabled = true;
    return options;
}

ProviderReasoningOptions ProviderReasoningOptions::disabledOption()
{
    ProviderReasoningOptions options;
    options.requested = true;
    options.enabled = false;
    return options;
}

bool ProviderAudioOptions::isExplicit() const
{
    return !voice.trimmed().isEmpty()
           || !inputFormat.trimmed().isEmpty()
           || !outputFormat.trimmed().isEmpty()
           || requestTranscript;
}

bool ProviderResponseFormat::isExplicit() const
{
    return kind != ProviderResponseFormatKind::None;
}

ProviderResponseFormat ProviderResponseFormat::none()
{
    return ProviderResponseFormat{};
}

ProviderResponseFormat ProviderResponseFormat::jsonObject()
{
    ProviderResponseFormat format;
    format.kind = ProviderResponseFormatKind::JsonObject;
    return format;
}

ProviderResponseFormat ProviderResponseFormat::fromJsonSchema(const QJsonObject &schema,
                                                              const QString &name)
{
    ProviderResponseFormat format;
    format.kind = ProviderResponseFormatKind::JsonSchema;
    format.jsonSchema = schema;
    format.schemaName = name;
    return format;
}

bool ProviderSamplingOptions::hasTopP() const
{
    return topP >= 0.0;
}

bool ProviderSamplingOptions::hasTopK() const
{
    return topK >= 0;
}

bool ProviderSamplingOptions::hasSeed() const
{
    return seed >= 0;
}

bool ProviderSamplingOptions::hasStop() const
{
    return !stop.isEmpty();
}

bool ProviderSamplingOptions::isExplicit() const
{
    return hasTopP() || hasTopK() || hasSeed() || hasStop() || penaltiesRequested
           || presencePenalty != 0.0 || frequencyPenalty != 0.0;
}

// ── ProviderRequest ──

bool ProviderRequest::hasTemperature() const
{
    return temperature >= 0.0;
}

bool ProviderRequest::hasMaxOutputTokens() const
{
    return maxOutputTokens >= 0;
}

bool ProviderRequest::validate(QString *error,
                               const ModelCapabilities *capabilities,
                               const int maxInlineAssetBytes) const
{
    const auto fail = [error](const QString &msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };

    // metadata 键白名单（禁止承载对话正文）
    for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it) {
        if (!isAllowedRequestMetadataKey(it.key())) {
            return fail(QStringLiteral("metadata key not allowed: %1").arg(it.key()));
        }
    }

    // 思考强度与工具 allowedCallers 合法性
    if (reasoning.isExplicit() && reasoning.effort != ProviderReasoningEffort::Unset
        && !isKnownReasoningEffort(reasoning.effort)) {
        return fail(QStringLiteral("unknown reasoning.effort"));
    }

    for (int ti = 0; ti < tools.size(); ++ti) {
        for (const ProviderCallerKind caller : tools.at(ti).allowedCallers) {
            if (caller == ProviderCallerKind::Unset
                || !isKnownCallerKind(caller)) {
                return fail(QStringLiteral("tools[%1].allowedCallers contains invalid enum")
                                .arg(ti));
            }
        }
    }

    // 逐条 item（默认 reasoning 单轨 + 内联策略）
    for (int i = 0; i < items.size(); ++i) {
        QString itemError;
        if (!items.at(i).validate(&itemError, true, maxInlineAssetBytes)) {
            return fail(QStringLiteral("items[%1]: %2").arg(i).arg(itemError));
        }
    }

    if (!capabilities) {
        return true;
    }

    // 可选能力门控：多模态输入 / 客户端与服务端工具
    if (hasImageInput() && !capabilities->supportsImageInput()) {
        return fail(QStringLiteral("capabilities: image input not supported"));
    }
    if (hasAudioInput() && !capabilities->supportsAudioInput()) {
        return fail(QStringLiteral("capabilities: audio input not supported"));
    }
    if (hasVideoInput() && !capabilities->supportsVideoInput()) {
        return fail(QStringLiteral("capabilities: video input not supported"));
    }
    if (hasDocumentInput() && !capabilities->supportsDocumentInput()) {
        return fail(QStringLiteral("capabilities: document input not supported"));
    }
    if (hasFunctionCallOutput() && !capabilities->supportsToolCalling()) {
        return fail(QStringLiteral("capabilities: tool calling not supported"));
    }
    if (hasServerToolResult() && !capabilities->supportsServerTools()) {
        return fail(QStringLiteral("capabilities: server tools not supported"));
    }

    // 扩展 kind：Program / Approval / Compaction / 服务端工具短名
    for (const ProviderItem &item : items) {
        if (item.kind == ProviderItemKind::Program
            || item.kind == ProviderItemKind::ProgramOutput) {
            if (!capabilities->supportsProgrammaticToolCalling()) {
                return fail(QStringLiteral("capabilities: programmatic tool calling not supported"));
            }
        }
        if (item.isApprovalRelated() && !capabilities->supportsMcpApproval()) {
            return fail(QStringLiteral("capabilities: MCP approval items not supported"));
        }
        if (item.kind == ProviderItemKind::Compaction && !capabilities->supportsCompaction()) {
            return fail(QStringLiteral("capabilities: compaction not supported"));
        }
        if (item.isServerToolRelated()
            && !capabilities->supportsServerToolName(item.name)) {
            return fail(QStringLiteral("capabilities: server tool not supported: %1")
                            .arg(item.name));
        }
    }

    return true;
}

bool ProviderRequest::validateForLedger(QString *error,
                                        const ModelCapabilities *capabilities) const
{
    // 账本：禁止内联字节，强制 blob/uri 引用
    return validate(error, capabilities, /*maxInlineAssetBytes=*/0);
}

bool ProviderRequest::hasFunctionCallOutput() const
{
    return std::any_of(items.cbegin(), items.cend(), [](const ProviderItem &item) {
        return item.kind == ProviderItemKind::FunctionCallOutput;
    });
}

bool ProviderRequest::hasServerToolResult() const
{
    return std::any_of(items.cbegin(), items.cend(), [](const ProviderItem &item) {
        return item.kind == ProviderItemKind::ServerToolResult;
    });
}

bool ProviderRequest::hasImageInput() const
{
    return std::any_of(items.cbegin(), items.cend(), [](const ProviderItem &item) {
        return item.hasImageParts();
    });
}

bool ProviderRequest::hasAudioInput() const
{
    return std::any_of(items.cbegin(), items.cend(), [](const ProviderItem &item) {
        return item.hasAudioParts();
    });
}

bool ProviderRequest::hasVideoInput() const
{
    return std::any_of(items.cbegin(), items.cend(), [](const ProviderItem &item) {
        return item.hasVideoParts();
    });
}

bool ProviderRequest::hasDocumentInput() const
{
    return std::any_of(items.cbegin(), items.cend(), [](const ProviderItem &item) {
        return item.hasDocumentParts();
    });
}

QString ProviderRequest::joinedUserText() const
{
    QString text;
    for (const ProviderItem &item : items) {
        if (item.kind != ProviderItemKind::UserMessage) {
            continue;
        }
        for (const ProviderMessagePart &part : item.parts) {
            // 文本 part 直接拼接；音频 part 仅在有 transcript 时纳入
            if (part.kind == ProviderPartKind::Text && !part.text.isEmpty()) {
                if (!text.isEmpty()) {
                    text += QLatin1Char('\n');
                }
                text += part.text;
            } else if (part.kind == ProviderPartKind::Audio
                       && !part.audio.transcript.trimmed().isEmpty()) {
                if (!text.isEmpty()) {
                    text += QLatin1Char('\n');
                }
                text += part.audio.transcript;
            }
        }
    }
    return text;
}
