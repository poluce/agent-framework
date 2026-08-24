#include "InstanceModelCatalogStore.h"

#include "logging/LogManager.h"
#include "providers/core/AbstractProvider.h"

#include <QSet>

#include <utility>

InstanceModelCatalogStore::InstanceModelCatalogStore(QObject *parent)
    : QObject(parent)
{
}

InstanceModelCatalogStore::~InstanceModelCatalogStore()
{
    for (const auto provider : std::as_const(m_refreshProviders)) {
        if (provider) {
            provider->deleteLater();
        }
    }
}

void InstanceModelCatalogStore::setProviderFactory(ProviderFactory factory)
{
    m_providerFactory = std::move(factory);
}

ModelCatalogState InstanceModelCatalogStore::stateForInstance(const QString &instanceId) const
{
    return m_entries.value(instanceId.trimmed());
}

QStringList InstanceModelCatalogStore::availableModels(const QString &instanceId) const
{
    return stateForInstance(instanceId).models;
}

bool InstanceModelCatalogStore::isLoading(const QString &instanceId) const
{
    return stateForInstance(instanceId).loading;
}

QString InstanceModelCatalogStore::error(const QString &instanceId) const
{
    return stateForInstance(instanceId).error;
}

void InstanceModelCatalogStore::requestAvailableModels(const QString &instanceId,
                                                       const QString &providerType,
                                                       const QString &baseUrl,
                                                       const QString &apiKey)
{
    const QString id = instanceId.trimmed();
    const QString type = providerType.trimmed().toLower();
    const QString url = baseUrl.trimmed();
    const QString key = apiKey.trimmed();
    if (id.isEmpty()) {
        return;
    }

    if (type.isEmpty() || url.isEmpty() || key.isEmpty()) {
        ModelCatalogState state = stateForInstance(id);
        state.loading = false;
        state.error.clear();
        LOGW(LogCat::Model) << "模型目录刷新跳过：凭据字段不完整"
            << logf("instance", id)
            << logf("providerType", type)
            << logf("hasUrl", !url.isEmpty())
            << logf("hasKey", !key.isEmpty());
        setState(id, state);
        return;
    }

    if (const auto activeProvider = m_refreshProviders.value(id); activeProvider && isLoading(id)) {
        LOGD(LogCat::Model) << "模型目录刷新合并：已有进行中的请求"
            << logf("instance", id);
        return;
    }

    if (const auto oldProvider = m_refreshProviders.take(id)) {
        oldProvider->deleteLater();
    }

    std::unique_ptr<AbstractProvider> provider;
    if (m_providerFactory) {
        provider = m_providerFactory(type);
    }
    if (!provider) {
        ModelCatalogState state = stateForInstance(id);
        state.loading = false;
        state.error = QStringLiteral("无法为模型拉取创建 provider: %1").arg(type);
        LOGE(LogCat::Model) << "模型目录刷新失败：无法创建 provider"
            << logf("instance", id)
            << logf("providerType", type);
        setState(id, state);
        return;
    }

    AbstractProvider *rawProvider = provider.release();
    rawProvider->setParent(this);
    rawProvider->setAuth({url, key, {}});
    m_refreshProviders.insert(id, rawProvider);

    ModelCatalogState loadingState = stateForInstance(id);
    loadingState.loading = true;
    loadingState.error.clear();
    LOGD(LogCat::Model) << "模型目录刷新开始"
        << logf("instance", id)
        << logf("providerType", type)
        << logf("baseUrl", url)
        << logf("prevSize", loadingState.models.size());
    setState(id, loadingState);

    connect(rawProvider, &AbstractProvider::modelRefreshFinished, this,
            [this, id, rawProvider]() {
                if (m_refreshProviders.value(id) != rawProvider) {
                    return;
                }
                QStringList models;
                QSet<QString> seen;
                for (const ModelCapabilities &capabilities : rawProvider->availableModels()) {
                    const QString modelId = capabilities.modelId.trimmed();
                    if (!modelId.isEmpty() && !seen.contains(modelId)) {
                        seen.insert(modelId);
                        models.append(modelId);
                    }
                }
                ModelCatalogState state = stateForInstance(id);
                state.models = models;
                state.loading = false;
                state.error.clear();
                LOGD(LogCat::Model) << "模型目录刷新完成"
                    << logf("instance", id)
                    << logf("size", models.size());
                setState(id, state);
                m_refreshProviders.remove(id);
                rawProvider->deleteLater();
            });

    connect(rawProvider, &AbstractProvider::modelRefreshFailed, this,
            [this, id, rawProvider](const QString &message) {
                if (m_refreshProviders.value(id) != rawProvider) {
                    return;
                }
                ModelCatalogState state = stateForInstance(id);
                state.loading = false;
                state.error = message;
                LOGE(LogCat::Model) << "模型目录刷新失败"
                    << logf("instance", id)
                    << logf("error", message)
                    << logf("keptSize", state.models.size());
                setState(id, state);
                m_refreshProviders.remove(id);
                rawProvider->deleteLater();
            });

    rawProvider->requestModelRefresh();
}

void InstanceModelCatalogStore::removeInstance(const QString &instanceId)
{
    const QString id = instanceId.trimmed();
    if (id.isEmpty()) {
        return;
    }
    if (const auto active = m_refreshProviders.take(id)) {
        active->deleteLater();
    }
    if (m_entries.remove(id) > 0) {
        emit catalogChanged(id);
    }
}

void InstanceModelCatalogStore::setState(const QString &instanceId, const ModelCatalogState &state)
{
    const QString id = instanceId.trimmed();
    if (id.isEmpty()) {
        return;
    }
    if (const auto it = m_entries.constFind(id); it != m_entries.cend()) {
        if (it->models == state.models && it->loading == state.loading && it->error == state.error) {
            return;
        }
    }
    m_entries.insert(id, state);
    emit catalogChanged(id);
}
