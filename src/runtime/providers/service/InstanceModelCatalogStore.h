#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class AbstractProvider;

struct ModelCatalogState
{
    QStringList models;
    bool loading = false;
    QString error;
};

/**
 * Core-only asynchronous model catalog cache.
 *
 * Entries are keyed by credential instance id.  Authentication material is
 * used only while creating a provider and is never retained in the cache or
 * emitted through a client-facing signal.
 */
class InstanceModelCatalogStore : public QObject
{
    Q_OBJECT

public:
    using ProviderFactory = std::function<std::unique_ptr<AbstractProvider>(const QString &providerType)>;

    explicit InstanceModelCatalogStore(QObject *parent = nullptr);
    ~InstanceModelCatalogStore() override;

    void setProviderFactory(ProviderFactory factory);

    [[nodiscard]] ModelCatalogState stateForInstance(const QString &instanceId) const;
    [[nodiscard]] QStringList availableModels(const QString &instanceId) const;
    [[nodiscard]] bool isLoading(const QString &instanceId) const;
    [[nodiscard]] QString error(const QString &instanceId) const;

    void requestAvailableModels(const QString &instanceId,
                                const QString &providerType,
                                const QString &baseUrl,
                                const QString &apiKey);
    void removeInstance(const QString &instanceId);

signals:
    void catalogChanged(const QString &instanceId);

private:
    void setState(const QString &instanceId, const ModelCatalogState &state);

    ProviderFactory m_providerFactory;
    QHash<QString, ModelCatalogState> m_entries;
    QHash<QString, QPointer<AbstractProvider>> m_refreshProviders;
};
