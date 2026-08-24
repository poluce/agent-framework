#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * 从 https://models.dev/api.json 拉取 provider→model→limit。
 *
 * 当前解析 limit.context（窗口）与 limit.output（最大输出），仅填充元数据，
 * 不替代 baseUrl listModels 的 id 列表。
 * 失败保留旧 cache；内存中缓存整表以便多次 provider 查询不重复下载。
 */
class ModelsDevClient final : public QObject
{
    Q_OBJECT

public:
    explicit ModelsDevClient(QObject *parent = nullptr);
    ~ModelsDevClient() override;

    /// 单个模型的 models.dev 上限元数据（任一为 0 表示未提供该键）
    struct ModelLimits {
        qint64 contextWindow = 0;
        qint64 maxOutputTokens = 0;
    };

    /**
     * 为给定 modelId 列表解析上下文窗口与最大输出。
     * 若本地尚无 api.json 缓存则先异步拉取；完成后 emit windowsResolved。
     * providerType 为本仓 id（responses/chat-completions/…），内部映射到 models.dev provider key。
     */
    void requestWindows(const QString &providerType, const QStringList &modelIds);

    /// 本仓 providerType → models.dev 顶层 provider 键
    [[nodiscard]] static QStringList modelsDevProviderKeys(const QString &providerType);

signals:
    /**
     * @param providerType 请求时的本仓 providerType
     * @param windows modelId → contextWindow（仅命中且 >0）
     * @param maxOutputTokens modelId → 最大输出 token（仅命中且 >0；模型未提供该键则为空）
     */
    void windowsResolved(const QString &providerType,
                         const QHash<QString, qint64> &windows,
                         const QHash<QString, qint64> &maxOutputTokens);
    void fetchFailed(const QString &message);

private:
    struct PendingRequest {
        QString providerType;
        QStringList modelIds;
    };

    void ensureCatalogLoaded();
    void onCatalogReplyFinished();
    void flushPending();
    [[nodiscard]] QHash<QString, qint64> lookupWindows(const QString &providerType,
                                                       const QStringList &modelIds) const;
    [[nodiscard]] QHash<QString, qint64> lookupMaxOutputTokens(const QString &providerType,
                                                               const QStringList &modelIds) const;
    /// 按字段（窗口或最大输出）统一查表
    [[nodiscard]] QHash<QString, qint64> lookupLimits(const QString &providerType,
                                                      const QStringList &modelIds,
                                                      qint64 ModelLimits::*field) const;

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_activeReply = nullptr;
    /// models.dev providerKey → (modelId → 上限元数据)
    QHash<QString, QHash<QString, ModelLimits>> m_catalog;
    bool m_catalogReady = false;
    bool m_catalogLoading = false;
    QList<PendingRequest> m_pending;
};
