#pragma once

#include "../core/AbstractProvider.h"

class AnthropicProvider final : public AbstractProvider
{
    Q_OBJECT
    friend class ProviderAdapterFixtureTests;
    friend class ProviderRetryTests;

public:
    explicit AnthropicProvider(QObject *parent = nullptr);
    ~AnthropicProvider() override;

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
    [[nodiscard]] QJsonArray buildMessages(const ProviderRequest &request) const;
    [[nodiscard]] QJsonObject buildToolDefinition(const ProviderToolSpecification &tool) const;
    [[nodiscard]] QJsonValue buildToolChoice(const ProviderToolChoice &toolChoice) const;

    // SSE 事件处理
    [[nodiscard]] QList<ProviderEvent> handleMessageStart(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleContentBlockStart(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleContentBlockDelta(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleContentBlockStop(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleMessageDelta(const QJsonObject &payload);
    [[nodiscard]] QList<ProviderEvent> handleMessageStop(const QJsonObject &payload);

    // 辅助
    [[nodiscard]] ProviderUsage usageFromJson(const QJsonObject &usageObject) const;
    [[nodiscard]] StopReason stopReasonFromString(const QString &reason) const;
    [[nodiscard]] QString currentMessageId() const;

    // 状态
    QString m_currentMessageId;
    QString m_containerId;                    // code execution 容器，跨 turn 续用
    QHash<int, QString> m_blockTypes;         // index → content_block type
    QHash<int, QString> m_toolUseIds;         // index → tool_use id
    QHash<int, QString> m_toolUseNames;       // index → tool_use name
    QHash<QString, QString> m_toolArgsBuffer; // tool_use_id → 累积的 partial_json
    QHash<QString, ProviderCallerKind> m_toolCallerKinds;
    QHash<QString, QString> m_toolCallerIds;
    QString m_stopReason;
    ProviderUsage m_lastUsage;
};
