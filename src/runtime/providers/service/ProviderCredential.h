#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * API keys deliberately have a three-state update contract.  In particular,
 * an empty editor field does not accidentally erase a saved key.
 */
enum class ApiKeyUpdateMode {
    Preserve,
    Replace,
    Clear,
};

/**
 * Persistent provider credentials are Core data, not a QML model.
 *
 * The only component allowed to turn these records into client-facing data is
 * CoreApplicationService.  In particular `getInstance()` is intentionally a
 * Core-only API because it contains the API key.
 */
struct ModelInstance
{
    QString id;
    QString name;
    QString providerType;
    QString baseUrl;
    QString apiKey;

    QJsonObject toJson() const;
    static ModelInstance fromJson(const QJsonObject &obj);
    [[nodiscard]] bool isValid() const { return !id.isEmpty() && !providerType.isEmpty(); }
};

class ProviderCredential
{
public:
    ProviderCredential() = default;
    ProviderCredential(const ProviderCredential &) = delete;
    ProviderCredential &operator=(const ProviderCredential &) = delete;

    // ── Core CRUD ──
    QString createInstance(const QString &providerType,
                           const QString &name = {},
                           const QString &baseUrl = {},
                           const QString &apiKey = {});
    bool removeInstance(const QString &id);
    bool updateInstance(const QString &id,
                        const QString &providerType,
                        const QString &name,
                        const QString &baseUrl,
                        const QString &apiKey,
                        ApiKeyUpdateMode apiKeyUpdate);

    /// Contains the API key.  Never expose this map through ProtocolEvent.
    QVariantMap getInstance(const QString &id) const;
    [[nodiscard]] QList<ModelInstance> instances() const { return m_instances; }
    [[nodiscard]] bool contains(const QString &id) const;
    // ── Persistence ──
    void load(const QString &filePath);
    void save(const QString &filePath) const;

private:
    int indexOf(const QString &id) const;

    QList<ModelInstance> m_instances;
};
