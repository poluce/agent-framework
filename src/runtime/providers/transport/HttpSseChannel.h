#pragma once

#include "providers/ProviderTypes/ProviderTypes.h"
#include "providers/ProviderTypes/ProviderAdapterTypes.h"
#include "providers/transport/ProviderSseParser.h"
#include "logging/LogManager.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <memory>

class QNetworkReply;

class HttpSseChannel final : public QObject
{
    Q_OBJECT

public:
    enum class AuthMode {
        Bearer,      // Authorization: Bearer <key>  (OpenAI 系)
        ApiKey,      // x-api-key: <key>              (Anthropic)
        GoogApiKey   // x-goog-api-key: <key>         (Google Gemini)
    };

    struct StartResult
    {
        bool accepted = false;
        ProviderError error;
    };

    explicit HttpSseChannel(const QString &providerType, QObject *parent = nullptr);
    ~HttpSseChannel() override;

    [[nodiscard]] StartResult start(const QUrl &url,
                                    const QString &apiKey,
                                    const ProviderTransportRequest &request);
    void cancel();
    void setLogContext(const AgentLogContext &logContext);

    void setAuthMode(AuthMode mode);
    void setVersionHeader(const QByteArray &name, const QByteArray &value);
    void applyAuthHeaders(QNetworkRequest &request, const QString &apiKey) const;

signals:
    void payloadReceived(const ProviderTransportPayload &payload);
    void finished();
    void failed(const ProviderError &error);

private slots:
    void onReadyRead();
    void onReplyFinished();
    void onErrorOccurred();

private:
    void handleSseMessage(const QString &eventName, const QByteArray &data);
    void finalizeAsJson(const QByteArray &payload);
    void cleanupReply();
    /// 释放 m_activeReply（可选 abort）；不重置 parser/buffer
    void disposeActiveReply(bool abort);
    /// 丢弃当前 active reply 且不向业务层发 failed（用于 start 替换旧连接）
    void abandonActiveReply();
    void trySwitchToSseMode();
    void processSseEvents();
    [[nodiscard]] bool looksLikeSsePayload(const QByteArray &buffer) const;

    QString m_providerType;
    QNetworkAccessManager m_networkAccessManager;
    QPointer<QNetworkReply> m_activeReply = nullptr;

    std::unique_ptr<ProviderSseParser> m_parser;
    QByteArray m_rawBuffer;
    bool m_sseMode = false;
    AgentLogContext m_logContext;

    AuthMode m_authMode = AuthMode::Bearer;
    QByteArray m_versionHeaderName;
    QByteArray m_versionHeaderValue;
};
