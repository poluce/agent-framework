#include "ModelsDevClient.h"

#include "logging/LogManager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

constexpr char kModelsDevApiUrl[] = "https://models.dev/api.json";

qint64 extractContextWindow(const QJsonObject &modelObj)
{
    // 主路径：limit.context；少数条目扁平 contextWindow
    const QJsonObject limit = modelObj.value(QStringLiteral("limit")).toObject();
    const qint64 fromLimit = static_cast<qint64>(limit.value(QStringLiteral("context")).toDouble(0));
    if (fromLimit > 0) {
        return fromLimit;
    }
    const qint64 flat = static_cast<qint64>(
        modelObj.value(QStringLiteral("contextWindow")).toDouble(0));
    return flat > 0 ? flat : 0;
}

/// models.dev limit.output —— 模型最大输出上限（部分旧条目缺失该键）
qint64 extractOutputLimit(const QJsonObject &modelObj)
{
    const QJsonObject limit = modelObj.value(QStringLiteral("limit")).toObject();
    const qint64 fromLimit = static_cast<qint64>(limit.value(QStringLiteral("output")).toDouble(0));
    return fromLimit > 0 ? fromLimit : 0;
}

/// 在单个 models.dev provider 桶内找 modelId 的某上限（先精确后大小写不敏感）
qint64 findInBucket(const QHash<QString, ModelsDevClient::ModelLimits> &bucket,
                    const QString &modelId,
                    qint64 ModelsDevClient::ModelLimits::*field)
{
    if (const auto exact = bucket.constFind(modelId);
        exact != bucket.cend() && (*exact).*field > 0) {
        return (*exact).*field;
    }
    for (auto it = bucket.constBegin(); it != bucket.constEnd(); ++it) {
        if (it.key().compare(modelId, Qt::CaseInsensitive) == 0 && (*it).*field > 0) {
            return (*it).*field;
        }
    }
    return 0;
}

} // namespace

ModelsDevClient::ModelsDevClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

ModelsDevClient::~ModelsDevClient()
{
    if (!m_activeReply) {
        return;
    }
    m_activeReply->abort();
    m_activeReply->deleteLater();
    m_activeReply = nullptr;
}

QStringList ModelsDevClient::modelsDevProviderKeys(const QString &providerType)
{
    const QString t = providerType.trimmed().toLower();
    // 本仓协议族 → models.dev 顶层 provider（可多键，按序查）
    if (t == QStringLiteral("anthropic") || t == QStringLiteral("deepseek")) {
        return {t};
    }
    if (t == QStringLiteral("google") || t == QStringLiteral("google-interactions")) {
        return {QStringLiteral("google"), QStringLiteral("google-vertex")};
    }
    if (t == QStringLiteral("responses") || t == QStringLiteral("chat-completions")) {
        return {QStringLiteral("openai")};
    }
    return t.isEmpty() ? QStringList{} : QStringList{t};
}

void ModelsDevClient::requestWindows(const QString &providerType, const QStringList &modelIds)
{
    const QString provider = providerType.trimmed().toLower();
    if (provider.isEmpty() || modelIds.isEmpty()) {
        return;
    }

    if (m_catalogReady) {
        emit windowsResolved(provider,
                             lookupWindows(provider, modelIds),
                             lookupMaxOutputTokens(provider, modelIds));
        return;
    }
    m_pending.append(PendingRequest{provider, modelIds});
    ensureCatalogLoaded();
}

void ModelsDevClient::ensureCatalogLoaded()
{
    if (m_catalogReady || m_catalogLoading) {
        return;
    }
    m_catalogLoading = true;

    QNetworkRequest request{QUrl(QString::fromLatin1(kModelsDevApiUrl))};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("agent-qt/1.0 (model-context-meta)"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    LOGI(LogCat::Model) << "开始拉取 models.dev 目录"
        << logf("url", QString::fromLatin1(kModelsDevApiUrl));

    m_activeReply = m_nam->get(request);
    connect(m_activeReply, &QNetworkReply::finished, this, &ModelsDevClient::onCatalogReplyFinished);
}

void ModelsDevClient::onCatalogReplyFinished()
{
    QNetworkReply *reply = m_activeReply;
    m_activeReply = nullptr;
    m_catalogLoading = false;
    if (!reply) {
        return;
    }
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const QString message = reply->errorString();
        LOGE(LogCat::Model) << "models.dev 拉取失败"
            << logf("error", message);
        m_pending.clear();
        emit fetchFailed(message);
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        LOGE(LogCat::Model) << "models.dev 响应不是 JSON 对象";
        m_pending.clear();
        emit fetchFailed(QStringLiteral("models.dev 响应无效"));
        return;
    }

    m_catalog.clear();
    const QJsonObject root = doc.object();
    int modelCount = 0;
    for (auto pit = root.begin(); pit != root.end(); ++pit) {
        if (!pit.value().isObject()) {
            continue;
        }
        const QJsonObject providerObj = pit.value().toObject();
        const QJsonObject modelsObj = providerObj.value(QStringLiteral("models")).toObject();
        if (modelsObj.isEmpty()) {
            continue;
        }
        QHash<QString, ModelLimits> models;
        for (auto mit = modelsObj.begin(); mit != modelsObj.end(); ++mit) {
            if (!mit.value().isObject()) {
                continue;
            }
            const QJsonObject modelObj = mit.value().toObject();
            ModelLimits limits;
            limits.contextWindow = extractContextWindow(modelObj);
            limits.maxOutputTokens = extractOutputLimit(modelObj);
            if (limits.contextWindow > 0 || limits.maxOutputTokens > 0) {
                models.insert(mit.key(), limits);
                ++modelCount;
            }
        }
        if (!models.isEmpty()) {
            m_catalog.insert(pit.key(), models);
        }
    }

    m_catalogReady = true;
    LOGI(LogCat::Model) << "models.dev 目录已就绪"
        << logf("providers", m_catalog.size())
        << logf("models", modelCount);
    flushPending();
}

void ModelsDevClient::flushPending()
{
    const QList<PendingRequest> pending = std::move(m_pending);
    m_pending.clear();
    for (const PendingRequest &req : pending) {
        emit windowsResolved(req.providerType,
                             lookupWindows(req.providerType, req.modelIds),
                             lookupMaxOutputTokens(req.providerType, req.modelIds));
    }
}

QHash<QString, qint64> ModelsDevClient::lookupWindows(const QString &providerType,
                                                      const QStringList &modelIds) const
{
    return lookupLimits(providerType, modelIds, &ModelLimits::contextWindow);
}

QHash<QString, qint64> ModelsDevClient::lookupMaxOutputTokens(const QString &providerType,
                                                              const QStringList &modelIds) const
{
    return lookupLimits(providerType, modelIds, &ModelLimits::maxOutputTokens);
}

QHash<QString, qint64> ModelsDevClient::lookupLimits(const QString &providerType,
                                                     const QStringList &modelIds,
                                                     qint64 ModelLimits::*field) const
{
    QHash<QString, qint64> out;
    const QStringList keys = modelsDevProviderKeys(providerType);
    for (const QString &modelId : modelIds) {
        const QString id = modelId.trimmed();
        if (id.isEmpty() || out.contains(id)) {
            continue;
        }
        for (const QString &key : keys) {
            const auto pit = m_catalog.constFind(key);
            if (pit == m_catalog.cend()) {
                continue;
            }
            if (const qint64 value = findInBucket(*pit, id, field); value > 0) {
                out.insert(id, value);
                break;
            }
        }
    }
    return out;
}
