#include "ProviderCredential.h"

#include "providers/service/ProviderService.h"
#include "logging/LogManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

QJsonObject ModelInstance::toJson() const
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("providerType"), providerType},
        {QStringLiteral("baseUrl"), baseUrl},
        {QStringLiteral("apiKey"), apiKey},
    };
}

ModelInstance ModelInstance::fromJson(const QJsonObject &obj)
{
    ModelInstance inst;
    inst.id = obj.value(QStringLiteral("id")).toString().trimmed();
    inst.name = obj.value(QStringLiteral("name")).toString().trimmed();
    inst.providerType = ProviderService::normalizeProviderType(obj.value(QStringLiteral("providerType")).toString());
    inst.baseUrl = obj.value(QStringLiteral("baseUrl")).toString().trimmed();
    inst.apiKey = obj.value(QStringLiteral("apiKey")).toString().trimmed();
    if (inst.id.isEmpty()) {
        inst.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    return inst;
}

QString ProviderCredential::createInstance(const QString &providerType,
                                           const QString &name,
                                           const QString &baseUrl,
                                           const QString &apiKey)
{
    ModelInstance inst;
    inst.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    inst.providerType = ProviderService::normalizeProviderType(providerType);
    inst.name = name.trimmed();
    inst.baseUrl = baseUrl.trimmed();
    inst.apiKey = apiKey.trimmed();
    m_instances.append(inst);

    LOGI(LogCat::Config) << "创建凭据实例"
        << logf("id", inst.id)
        << logf("providerType", inst.providerType);
    return inst.id;
}

bool ProviderCredential::removeInstance(const QString &id)
{
    const int row = indexOf(id);
    if (row < 0) {
        return false;
    }

    LOGI(LogCat::Config) << "删除凭据实例"
        << logf("id", id);
    m_instances.removeAt(row);
    return true;
}

bool ProviderCredential::updateInstance(const QString &id,
                                        const QString &providerType,
                                        const QString &name,
                                        const QString &baseUrl,
                                        const QString &apiKey,
                                        const ApiKeyUpdateMode apiKeyUpdate)
{
    const int row = indexOf(id);
    if (row < 0) {
        return false;
    }

    ModelInstance &inst = m_instances[row];
    const QString normalizedProviderType = ProviderService::normalizeProviderType(providerType);
    if (!normalizedProviderType.isEmpty()) {
        inst.providerType = normalizedProviderType;
    }
    inst.name = name.trimmed();
    inst.baseUrl = baseUrl.trimmed();

    switch (apiKeyUpdate) {
    case ApiKeyUpdateMode::Preserve:
        break;
    case ApiKeyUpdateMode::Replace:
        inst.apiKey = apiKey.trimmed();
        break;
    case ApiKeyUpdateMode::Clear:
        inst.apiKey.clear();
        break;
    }

    LOGD(LogCat::Config) << "更新凭据实例"
        << logf("id", id);
    return true;
}

QVariantMap ProviderCredential::getInstance(const QString &id) const
{
    const int row = indexOf(id);
    if (row < 0) {
        return {};
    }

    const ModelInstance &inst = m_instances.at(row);
    return {
        {QStringLiteral("instanceId"), inst.id},
        {QStringLiteral("name"), inst.name},
        {QStringLiteral("providerType"), inst.providerType},
        {QStringLiteral("providerTypeDisplay"), ProviderService::providerDisplayName(inst.providerType)},
        {QStringLiteral("baseUrl"), inst.baseUrl},
        {QStringLiteral("apiKey"), inst.apiKey},
    };
}

bool ProviderCredential::contains(const QString &id) const
{
    return indexOf(id) >= 0;
}

void ProviderCredential::load(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        LOGD(LogCat::Storage) << "凭据文件不存在，跳过加载"
            << logf("path", filePath);
        return;
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
        LOGW(LogCat::Storage) << "凭据文件 JSON 解析失败"
            << logf("path", filePath);
        return;
    }

    m_instances.clear();
    const QJsonArray arr = doc.array();
    m_instances.reserve(arr.size());
    for (const QJsonValue &value : arr) {
        const ModelInstance instance = ModelInstance::fromJson(value.toObject());
        if (instance.isValid()) {
            m_instances.append(instance);
        }
    }
    LOGD(LogCat::Storage) << "加载凭据"
        << logf("path", filePath)
        << logf("count", m_instances.size());
}

void ProviderCredential::save(const QString &filePath) const
{
    QJsonArray instances;
    for (const ModelInstance &inst : m_instances) {
        instances.append(inst.toJson());
    }

    const QFileInfo info(filePath);
    QDir().mkpath(info.absolutePath());
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOGW(LogCat::Storage) << "无法写入凭据文件"
            << logf("path", filePath);
        return;
    }
    file.write(QJsonDocument(instances).toJson(QJsonDocument::Indented));
    LOGD(LogCat::Storage) << "凭据已持久化"
        << logf("path", filePath);
}

int ProviderCredential::indexOf(const QString &id) const
{
    for (int index = 0; index < m_instances.size(); ++index) {
        if (m_instances.at(index).id == id) {
            return index;
        }
    }
    return -1;
}
