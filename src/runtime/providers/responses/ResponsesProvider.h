#pragma once

#include "../core/AbstractProvider.h"

#include <optional>

class ResponsesProvider final : public AbstractProvider
{
    Q_OBJECT
    friend class ProviderAdapterFixtureTests;
    friend class ProviderRetryTests;

public:
    explicit ResponsesProvider(QObject *parent = nullptr);
    ~ResponsesProvider() override;

protected:
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

private:
    [[nodiscard]] std::optional<ModelCapabilities> capabilitiesForModel(const QString &modelId) const;
    [[nodiscard]] QList<ProviderEvent> handlePayloadError(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleOutputTextDelta(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleReasoningDelta(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleOutputItemAdded(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleOutputItemDone(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleCompleted(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleFailed(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleIncomplete(const QJsonObject &payload);
    [[nodiscard]] ProviderUsage usageFromJson(const QJsonObject &usageObject) const;
    [[nodiscard]] QString currentMessageId(const QJsonObject &responseObject = {}) const;
    [[nodiscard]] QList<ProviderItem> completedOutputItems(const QJsonObject &responseObject) const;
};
