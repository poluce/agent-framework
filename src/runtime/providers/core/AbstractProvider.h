#pragma once

#include "providers/ProviderTypes/ProviderTypes.h"
#include "providers/ProviderTypes/ProviderAdapterTypes.h"
#include "logging/LogManager.h"
#include "providers/core/ProviderRetryPolicy.h"

#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QNetworkReply>
#include <QTimer>

#include <memory>
#include <optional>

class HttpSseChannel;

struct ProviderAuth {
    QString baseUrl;
    QString apiKey;
    QString modelName;
};

class AbstractProvider : public QObject
{
    Q_OBJECT
    friend class ProviderRetryTests;

public:
    explicit AbstractProvider(const QString &providerType, QObject *parent = nullptr);
    ~AbstractProvider() override;

    static constexpr int providerProtocolVersion()
    {
        return kProviderProtocolVersion;
    }

    virtual void setAuth(const ProviderAuth &auth);
    [[nodiscard]] virtual ProviderAuth auth() const;

    [[nodiscard]] virtual QString baseUrl() const;

    [[nodiscard]] virtual QString apiKey() const;

    [[nodiscard]] virtual QString currentModel() const;

    void sendRequest(const ProviderRequest &request);
    void sendRequestWithoutModelRefresh(const ProviderRequest &request);
    void requestModelRefresh();
    void cancel();
    [[nodiscard]] QList<ModelCapabilities> availableModels() const;
    void seedAvailableModels(const QList<ModelCapabilities> &models);
    void setLogContext(const AgentLogContext &logContext);
    [[nodiscard]] AgentLogContext logContext() const;

    /// 本 turn 瞬时错误重试策略（默认关闭；CompactEngine/AutoRename 不调即零重试）
    void setRetryPolicy(const ProviderRetryPolicy &policy);

signals:
    void modelRefreshFinished();
    void modelRefreshFailed(const QString &error);

    void eventEmitted(const ProviderEvent &event);

protected:
    [[nodiscard]] virtual ProviderError validateProviderRequest(const ProviderRequest &request) const = 0;
    [[nodiscard]] virtual ProviderTransportRequest buildProviderTransportRequest(const ProviderRequest &request) const = 0;
    [[nodiscard]] virtual QList<ProviderEvent> parseProviderTransportPayload(const ProviderTransportPayload &payload) = 0;
    virtual void resetProviderTurnState() = 0;
    virtual bool startProviderTransportRequest(const ProviderTransportRequest &request,
                                               ProviderError *error = nullptr) = 0;

    // 定制化 API 端点与解析
    [[nodiscard]] virtual QUrl buildModelsUrl(const QString &baseUrl) const = 0;
    [[nodiscard]] virtual QList<ModelCapabilities> parseModelsPayload(const QByteArray &body,
                                                                      QString *errorMessage) const = 0;

    void cancelProviderRequest();
    bool startModelRefresh(QString *errorMessage = nullptr);
    void cancelModelRefresh();
    void onModelFetchFinished(QNetworkReply *reply);

    void beginProviderTurn();
    void emitEvents(const QList<ProviderEvent> &events);
    void processProviderPayload(const ProviderTransportPayload &payload);
    ProviderTurnState &turnState();
    const ProviderTurnState &turnState() const;
    void completeModelRefresh(const QList<ModelCapabilities> &models);
    void failModelRefresh(const QString &errorMessage);
    void invalidateModelCatalog();

    [[nodiscard]] ProviderTransportRequest buildStandardTransport(
        const QJsonObject &body, bool expectsEventStream) const;

    // 流式 Content Part 辅助函数
    [[nodiscard]] QString resolveMessageId(const QString &candidate) const;
    int resolvePartIndex(int requestedIndex = -1);
    void ensureMessageStarted(const QString &messageId);
    void ensureTextPartStarted(const QString &messageId);
    void ensureReasoningPartStarted(const QString &messageId, int requestedIndex = -1);
    void ensureToolPartStarted(const QString &toolCallId, const QString &messageId, int requestedIndex = -1);
    void completeTextPartIfOpen(const QString &messageId);
    void completeReasoningPartIfOpen(const QString &messageId);
    void completeToolPartIfOpen(const QString &toolCallId, const QString &messageId);

    template <typename Transport>
    void attachTransport(Transport *transport)
    {
        connect(transport, &Transport::payloadReceived, this,
                [this](const ProviderTransportPayload &payload) {
                    processProviderPayload(payload);
                });
        connect(transport, &Transport::failed, this, [this](const ProviderError &error) {
            handleTransportFailed(error);
        });
    }

    void emitTextDelta(ProviderTextDelta delta);
    void emitReasoningDelta(ProviderReasoningDelta delta);
    void emitErrorOccurred(ProviderError error);
    void emitCancelled();
    void emitProviderEvent(ProviderEvent event);

private:
    void ensureModelsReadyOrRefresh(const ProviderRequest &request);
    void dispatchPendingRequest(const ProviderRequest &request);
    qint64 nextEventSequence();
    void stampEventSequence(ProviderEvent &event);

    // ── 瞬时错误自动重试（首字节前）──
    void handleTransportFailed(const ProviderError &error);
    /// true = 已接管（不 emit 错误）
    bool maybeScheduleRetry(ProviderError &error);
    /// 退避到期：重发原请求（跳过 refresh / begin / 校验）
    void retryPendingTurn();

    qint64 m_nextEventSequence = 1;
    bool m_turnErrorEmitted = false;
    bool m_modelRefreshInFlight = false;
    bool m_modelsLoaded = false;
    ProviderTurnState m_turnState;
    QList<ModelCapabilities> m_availableModels;
    std::optional<ProviderRequest> m_pendingRequestAfterModelRefresh;
    ProviderRetryPolicy m_retryPolicy;
    ProviderRequest m_retryRequest;  ///< 启动成功后缓存，供退避重发
    int m_retryAttempts = 0;
    QTimer m_retryTimer;
protected:
    ProviderAuth m_auth;
    AgentLogContext m_logContext;
    std::unique_ptr<HttpSseChannel> m_channel;
    QNetworkAccessManager m_modelFetchAccessManager;
    QPointer<QNetworkReply> m_modelFetchReply;
    int m_activeReasoningPartIndex = -1;
};

