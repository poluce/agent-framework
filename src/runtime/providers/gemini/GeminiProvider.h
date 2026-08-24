#pragma once

#include "../core/AbstractProvider.h"

class GeminiProvider final : public AbstractProvider
{
    Q_OBJECT
    friend class ProviderAdapterFixtureTests;
    friend class ProviderRetryTests;

public:
    explicit GeminiProvider(
        ProviderProtocolFamily protocolFamily = ProviderProtocolFamily::GeminiGenerateContent,
        QObject *parent = nullptr);
    ~GeminiProvider() override;

protected:
    [[nodiscard]] ProviderError validateProviderRequest(const ProviderRequest &request) const override;
    [[nodiscard]] ProviderTransportRequest buildProviderTransportRequest(const ProviderRequest &request) const override;
    [[nodiscard]] QList<ProviderEvent> parseProviderTransportPayload(const ProviderTransportPayload &payload) override;
    void resetProviderTurnState() override;
    bool startProviderTransportRequest(const ProviderTransportRequest &request,
                                       ProviderError *error = nullptr) override;

    [[nodiscard]] QUrl buildModelsUrl(const QString &baseUrl) const override;
    [[nodiscard]] QList<ModelCapabilities> parseModelsPayload(const QByteArray &body,
                                                              QString *errorMessage) const override;

private:
    // 请求构造
    [[nodiscard]] QJsonArray buildToolDefinitions(const ProviderRequest &request) const;
    [[nodiscard]] ProviderTransportRequest buildInteractionsRequest(
        const ProviderRequest &request) const;
    [[nodiscard]] ProviderTransportRequest buildGenerateContentRequest(
        const ProviderRequest &request) const;
    [[nodiscard]] QUrl buildRequestUrl(
        const ProviderTransportRequest &request) const;
    [[nodiscard]] QList<ProviderEvent> parseGenerateContentPayload(
        const QJsonObject &payload);

    // SSE 事件处理
    [[nodiscard]] QList<ProviderEvent> handleInteractionCreated(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleInteractionInProgress(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleStepStart(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleStepDelta(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleStepStop(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleInteractionRequiresAction(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleInteractionCompleted(const QJsonObject &payload);

    // 辅助
    [[nodiscard]] ProviderUsage usageFromJson(const QJsonObject &usageObject) const;
    [[nodiscard]] QString currentMessageId() const;

    // 状态
    ProviderProtocolFamily m_protocolFamily = ProviderProtocolFamily::GeminiGenerateContent;
    QString m_interactionId;
    QHash<int, QString> m_stepTypes;          // index → step type
    QHash<int, QString> m_functionCallIds;    // index → function call id
    QHash<int, QString> m_functionCallNames;  // index → function name
    QHash<QString, QString> m_argsBuffer;     // call_id → 累积的 arguments
    bool m_requiresAction = false;
    ProviderUsage m_lastUsage;
    QList<ProviderItem> m_generateOutputItems;
    QString m_generateMessageId;
};
