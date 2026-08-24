#include "DeepSeekProvider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

QString normalizeChatModelName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return QStringLiteral("DeepSeek-V4-Flash");
    return trimmed;
}

QJsonValue sanitizeLoneSurrogates(const QJsonValue &value)
{
    if (value.isString()) {
        QString str = value.toString();
        QString cleaned;
        cleaned.reserve(str.length());
        for (int i = 0; i < str.length(); ++i) {
            QChar ch = str.at(i);
            if (ch.isHighSurrogate()) {
                if (i + 1 < str.length() && str.at(i + 1).isLowSurrogate()) {
                    cleaned.append(ch);
                    cleaned.append(str.at(i + 1));
                    i++;
                } else {
                    cleaned.append(QChar(0xFFFD));
                }
            } else if (ch.isLowSurrogate()) {
                cleaned.append(QChar(0xFFFD));
            } else {
                cleaned.append(ch);
            }
        }
        return cleaned;
    } else if (value.isArray()) {
        QJsonArray arr = value.toArray();
        QJsonArray cleanedArr;
        for (const QJsonValue &val : arr) {
            cleanedArr.append(sanitizeLoneSurrogates(val));
        }
        return cleanedArr;
    } else if (value.isObject()) {
        QJsonObject obj = value.toObject();
        QJsonObject cleanedObj;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            cleanedObj.insert(it.key(), sanitizeLoneSurrogates(it.value()));
        }
        return cleanedObj;
    }
    return value;
}

} // namespace

// ---- DeepSeekProvider ----

DeepSeekProvider::DeepSeekProvider(QObject *parent)
    : ChatCompletionsProvider(QStringLiteral("deepseek"), parent)
{
}

DeepSeekProvider::~DeepSeekProvider() = default;

ProviderError DeepSeekProvider::validateProviderRequest(const ProviderRequest &request) const
{
    if (request.protocolFamily != ProviderProtocolFamily::Auto
        && request.protocolFamily != ProviderProtocolFamily::DeepSeekChatCompletions) {
        return {QStringLiteral("protocol_family_mismatch"),
                QStringLiteral("DeepSeek 适配器不能处理指定的协议族。")};
    }
    return ChatCompletionsProvider::validateProviderRequest(request);
}

void DeepSeekProvider::setAuth(const ProviderAuth &auth)
{
    ProviderAuth normalized = auth;
    normalized.modelName = normalizeChatModelName(auth.modelName);
    ChatCompletionsProvider::setAuth(normalized);
}

QJsonObject DeepSeekProvider::buildRequestBody(const ProviderRequest &request) const
{
    QJsonObject body = ChatCompletionsProvider::buildRequestBody(request);

    // reasoning_effort + thinking — DeepSeek 推理强度控制
    if (request.reasoning.enabled
        && request.reasoning.effort != ProviderReasoningEffort::Unset) {
        // 官方思考模式文档：仅 high/max 两档有效。架构层档位（低/中/高）在此执行
        // 位置折叠：Minimal/Low/Medium/High → high，Max/XHigh → max。
        const ProviderReasoningEffort effort = request.reasoning.effort;
        const bool maxEffort = effort == ProviderReasoningEffort::Max
                               || effort == ProviderReasoningEffort::XHigh;
        body.insert(QStringLiteral("reasoning_effort"),
                    maxEffort ? QStringLiteral("max") : QStringLiteral("high"));
    }
    body.insert(QStringLiteral("thinking"),
                QJsonObject{{QStringLiteral("type"),
                             request.reasoning.enabled ? QStringLiteral("enabled")
                                                       : QStringLiteral("disabled")}});

    body = sanitizeLoneSurrogates(body).toObject();

    return body;
}

QJsonObject DeepSeekProvider::buildAssistantMessageForToolCall(const ProviderItem &item, const ProviderRequest &request) const
{
    QJsonObject msg = ChatCompletionsProvider::buildAssistantMessageForToolCall(item, request);

    if (request.reasoning.enabled || !item.reasoningText.isEmpty()) {
        msg.insert(QStringLiteral("reasoning_content"), item.reasoningText);
    }

    return msg;
}

QList<ProviderEvent> DeepSeekProvider::handleDeltaContent(const QJsonObject &delta)
{
    // 调用父类处理常规 content 文本
    QList<ProviderEvent> events = ChatCompletionsProvider::handleDeltaContent(delta);

    // reasoning_content — DeepSeek 推理模型的思考内容（思维链）
    const QJsonValue reasoningValue = delta.value(QStringLiteral("reasoning_content"));
    if (reasoningValue.isString()) {
        const QString reasoningContent = reasoningValue.toString();
        if (!reasoningContent.isEmpty()) {
            ProviderReasoningDelta rDelta;
            rDelta.base.messageId = currentMessageId();
            rDelta.text = reasoningContent;
            events.append(ProviderEvent::fromReasoningDelta(rDelta));
        }
    }

    return events;
}
