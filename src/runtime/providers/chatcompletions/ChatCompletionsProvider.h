#pragma once

#include "../core/AbstractProvider.h"
#include <optional>

class ChatCompletionsProvider : public AbstractProvider
{
    Q_OBJECT
    friend class ProviderAdapterFixtureTests;
    friend class ProviderRetryTests;

public:
    explicit ChatCompletionsProvider(QObject *parent = nullptr);
    ~ChatCompletionsProvider() override;

    [[nodiscard]] QString currentMessageId() const;

protected:
    ChatCompletionsProvider(const QString &providerType, QObject *parent = nullptr);

    [[nodiscard]] ProviderError validateProviderRequest(const ProviderRequest &request) const override;
    [[nodiscard]] ProviderTransportRequest buildProviderTransportRequest(const ProviderRequest &request) const override;
    [[nodiscard]] QList<ProviderEvent> parseProviderTransportPayload(const ProviderTransportPayload &payload) override;
    void resetProviderTurnState() override;
    bool startProviderTransportRequest(const ProviderTransportRequest &request,
                                       ProviderError *error = nullptr) override;

    // 基类定制化接口实现
    [[nodiscard]] QUrl buildModelsUrl(const QString &baseUrl) const override;
    [[nodiscard]] QList<ModelCapabilities> parseModelsPayload(const QByteArray &body,
                                                              QString *errorMessage) const override;

    // 可供子类扩展/重写的接口
    [[nodiscard]] virtual QJsonObject buildRequestBody(const ProviderRequest &request) const;
    [[nodiscard]] virtual QJsonArray buildMessages(const ProviderRequest &request) const;
    [[nodiscard]] virtual QJsonObject buildAssistantMessageForToolCall(const ProviderItem &item, const ProviderRequest &request) const;
    [[nodiscard]] virtual QJsonArray buildTools(const ProviderRequest &request) const;
    [[nodiscard]] virtual QList<ProviderEvent> handleChunk(const QJsonObject &chunk);
    [[nodiscard]] virtual QList<ProviderEvent> handleChunkChoice(const QJsonObject &choice);
    [[nodiscard]] virtual QList<ProviderEvent> handleDeltaContent(const QJsonObject &delta);
    [[nodiscard]] virtual QList<ProviderEvent> handleDeltaContentParts(const QJsonObject &delta);
    [[nodiscard]] virtual QList<ProviderEvent> handleDeltaToolCalls(const QJsonObject &delta);
    [[nodiscard]] virtual QList<ProviderEvent> handleFinishReason(const QString &reason);
    [[nodiscard]] virtual ProviderUsage usageFromChunk(const QJsonObject &chunk) const;
    void handleTransportFinished();

    [[nodiscard]] std::optional<ModelCapabilities> capabilitiesForModel(const QString &modelId) const;

    // 状态变量，由子类继承访问
    QString m_currentMessageId;
    QJsonObject m_logprobs;
    StopReason m_stopReason = StopReason::EndTurn;
    QHash<int, QString> m_pendingToolCallIds;
    QHash<int, QString> m_pendingToolCallNames;
    QHash<int, QString> m_pendingToolCallArgs;
};
