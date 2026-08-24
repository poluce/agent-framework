#include "ModelContextMetaStore.h"

#include "logging/LogManager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

using ModelMeta = ModelContextMetaStore::ModelMeta;
using NestedMeta = QHash<QString, QHash<QString, ModelMeta>>;

/// 解析单模型条目：对象形态（新格式）读 contextWindow/maxOutputTokens；
/// 裸数字（旧格式，window 语义）仅窗口，最大输出不误读
ModelMeta parseModelMeta(const QJsonValue &value)
{
    ModelMeta meta;
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        meta.window = static_cast<qint64>(
            obj.value(QStringLiteral("contextWindow")).toDouble(0));
        meta.maxOutput = static_cast<qint64>(
            obj.value(QStringLiteral("maxOutputTokens")).toDouble(0));
    } else if (value.isDouble()) {
        meta.window = static_cast<qint64>(value.toDouble());
    }
    return meta;
}

NestedMeta readNestedMeta(const QJsonObject &root, const QString &topKey)
{
    NestedMeta out;
    const QJsonObject top = root.value(topKey).toObject();
    for (auto it = top.begin(); it != top.end(); ++it) {
        const QString outer = it.key().trimmed();
        if (outer.isEmpty() || !it.value().isObject()) {
            continue;
        }
        QHash<QString, ModelMeta> inner;
        const QJsonObject models = it.value().toObject();
        for (auto m = models.begin(); m != models.end(); ++m) {
            const QString modelId = m.key().trimmed();
            const ModelMeta meta = parseModelMeta(m.value());
            if (!modelId.isEmpty() && (meta.window > 0 || meta.maxOutput > 0)) {
                inner.insert(modelId, meta);
            }
        }
        if (!inner.isEmpty()) {
            out.insert(outer, inner);
        }
    }
    return out;
}

QJsonObject writeNestedMeta(const NestedMeta &data)
{
    QJsonObject top;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        QJsonObject models;
        for (auto m = it.value().constBegin(); m != it.value().constEnd(); ++m) {
            const auto &meta = m.value();
            if (meta.window <= 0 && meta.maxOutput <= 0) {
                continue;
            }
            QJsonObject entry;
            if (meta.window > 0) {
                entry.insert(QStringLiteral("contextWindow"), static_cast<double>(meta.window));
            }
            if (meta.maxOutput > 0) {
                entry.insert(QStringLiteral("maxOutputTokens"),
                             static_cast<double>(meta.maxOutput));
            }
            models.insert(m.key(), entry);
        }
        if (!models.isEmpty()) {
            top.insert(it.key(), models);
        }
    }
    return top;
}

bool writeJsonFile(const QString &path, const QJsonObject &root)
{
    if (path.isEmpty()) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOGE(LogCat::Model) << "写入模型上下文元数据失败"
            << logf("path", path)
            << logf("error", file.errorString());
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

QJsonObject readJsonFile(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        if (file.exists()) {
            LOGW(LogCat::Model) << "读取模型上下文元数据失败"
                << logf("path", path)
                << logf("error", file.errorString());
        }
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

/// 嵌套表查找：outer→model → 元数据；未命中返回空结构
ModelMeta lookupMeta(const NestedMeta &table,
                     const QString &outer,
                     const QString &model)
{
    if (outer.isEmpty() || model.isEmpty()) {
        return {};
    }
    const auto outerIt = table.constFind(outer);
    if (outerIt == table.cend()) {
        return {};
    }
    const auto modelIt = outerIt->constFind(model);
    if (modelIt == outerIt->cend()) {
        return {};
    }
    return modelIt.value();
}

void writeVersionedRoot(const QString &path,
                        const QString &bucketKey,
                        const NestedMeta &data)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(bucketKey, writeNestedMeta(data));
    writeJsonFile(path, root);
}

} // namespace

ModelContextMetaStore::ModelContextMetaStore(QObject *parent)
    : ModelContextMetaStore({}, {}, parent)
{
}

ModelContextMetaStore::ModelContextMetaStore(const QString &cachePath,
                                             const QString &metaPath,
                                             QObject *parent)
    : QObject(parent)
    , m_cachePath(cachePath.trimmed())
    , m_metaPath(metaPath.trimmed())
{
}

void ModelContextMetaStore::load()
{
    m_cache = readNestedMeta(readJsonFile(m_cachePath), QStringLiteral("providers"));
    m_userMeta = readNestedMeta(readJsonFile(m_metaPath), QStringLiteral("instances"));
    LOGD(LogCat::Model) << "模型上下文元数据已加载"
        << logf("cacheProviders", m_cache.size())
        << logf("metaInstances", m_userMeta.size());
}

void ModelContextMetaStore::saveCache() const
{
    writeVersionedRoot(m_cachePath, QStringLiteral("providers"), m_cache);
}

void ModelContextMetaStore::saveMeta() const
{
    writeVersionedRoot(m_metaPath, QStringLiteral("instances"), m_userMeta);
}

QString ModelContextMetaStore::normProvider(const QString &providerType)
{
    return providerType.trimmed().toLower();
}

QString ModelContextMetaStore::normModel(const QString &modelId)
{
    return modelId.trimmed();
}

QString ModelContextMetaStore::normInstance(const QString &instanceId)
{
    return instanceId.trimmed();
}

qint64 ModelContextMetaStore::defaultMaxOutputTokensForProvider(const QString &providerType)
{
    // 已知输出上限收窄：anthropic 旗舰 32K、OpenAI 兼容 32K（GPT-4.1）；
    // deepseek / google / responses 系 64K 级（deepseek 384K、gemini 65K、gpt-5 128K）
    const QString t = providerType.trimmed().toLower();
    if (t == QStringLiteral("anthropic")
        || t == QStringLiteral("chat-completions")) {
        return 32768;
    }
    if (t == QStringLiteral("deepseek")
        || t == QStringLiteral("google")
        || t == QStringLiteral("google-interactions")
        || t == QStringLiteral("responses")) {
        return 65536;
    }
    return kDefaultMaxOutputTokens;
}

ModelContextMetaStore::ResolveResult
ModelContextMetaStore::resolve(const QString &providerType,
                               const QString &instanceId,
                               const QString &modelId) const
{
    ResolveResult result;
    const QString model = normModel(modelId);
    if (model.isEmpty()) {
        return result;
    }
    const QString provider = normProvider(providerType);
    const QString instance = normInstance(instanceId);
    const ModelMeta user = lookupMeta(m_userMeta, instance, model);
    const ModelMeta cache = lookupMeta(m_cache, provider, model);

    // 窗口：user(instance) > cache(provider) > kDefaultContextWindow
    if (user.window > 0) {
        result.contextWindow = user.window;
        result.contextWindowSource = Source::User;
    } else if (cache.window > 0) {
        result.contextWindow = cache.window;
        result.contextWindowSource = Source::Cache;
    }

    // 最大输出：user > cache > default（default 按 provider 查表）
    if (user.maxOutput > 0) {
        result.maxOutputTokens = user.maxOutput;
        result.maxOutputSource = Source::User;
    } else if (cache.maxOutput > 0) {
        result.maxOutputTokens = cache.maxOutput;
        result.maxOutputSource = Source::Cache;
    } else {
        result.maxOutputTokens = defaultMaxOutputTokensForProvider(provider);
    }

    return result;
}

qint64 ModelContextMetaStore::resolveWindow(const QString &providerType,
                                            const QString &instanceId,
                                            const QString &modelId) const
{
    return resolve(providerType, instanceId, modelId).contextWindow;
}

qint64 ModelContextMetaStore::resolveMaxOutputTokens(const QString &providerType,
                                                     const QString &instanceId,
                                                     const QString &modelId) const
{
    return resolve(providerType, instanceId, modelId).maxOutputTokens;
}

bool ModelContextMetaStore::setUserOverride(const QString &instanceId,
                                            const QString &modelId,
                                            const qint64 contextWindow,
                                            const qint64 maxOutputTokens)
{
    const QString instance = normInstance(instanceId);
    const QString model = normModel(modelId);
    if (instance.isEmpty() || model.isEmpty()) {
        return false;
    }

    // 语义：-1=不改该键；<=0=清除该键；>0=写入
    const bool touchWindow = contextWindow != -1;
    const bool touchMaxOutput = maxOutputTokens != -1;
    if (!touchWindow && !touchMaxOutput) {
        return true; // 双 -1：无操作
    }

    // 该键全部清除 → 整 model 条目移除
    const bool clearWindow = touchWindow && contextWindow <= 0;
    const bool clearMaxOutput = touchMaxOutput && maxOutputTokens <= 0;
    const bool clearBoth = clearWindow && clearMaxOutput;
    auto instIt = m_userMeta.find(instance);
    const bool hasModel = instIt != m_userMeta.end() && instIt->contains(model);
    if (clearBoth) {
        if (!hasModel) {
            return true;
        }
        instIt->remove(model);
        if (instIt->isEmpty()) {
            m_userMeta.erase(instIt);
        }
        saveMeta();
        emit metaChanged();
        LOGI(LogCat::Model) << "已清除模型用户覆盖"
            << logf("instance", instance)
            << logf("model", model);
        return true;
    }

    ModelMeta &meta = m_userMeta[instance][model];
    if (touchWindow) {
        meta.window = clearWindow ? 0 : contextWindow;
    }
    if (touchMaxOutput) {
        meta.maxOutput = clearMaxOutput ? 0 : maxOutputTokens;
    }
    saveMeta();
    emit metaChanged();
    LOGI(LogCat::Model) << "已写入模型用户覆盖"
        << logf("instance", instance)
        << logf("model", model)
        << logf("window", meta.window)
        << logf("maxOutput", meta.maxOutput);
    return true;
}

void ModelContextMetaStore::removeInstance(const QString &instanceId)
{
    const QString instance = normInstance(instanceId);
    if (instance.isEmpty() || m_userMeta.remove(instance) == 0) {
        return;
    }
    saveMeta();
    emit metaChanged();
    LOGI(LogCat::Model) << "已删除实例模型覆盖"
        << logf("instance", instance);
}

int ModelContextMetaStore::mergeProviderCache(const QString &providerType,
                                              const QHash<QString, qint64> &modelWindows,
                                              const QHash<QString, qint64> &modelMaxOutputTokens)
{
    const QString provider = normProvider(providerType);
    if (provider.isEmpty()
        || (modelWindows.isEmpty() && modelMaxOutputTokens.isEmpty())) {
        return 0;
    }

    int changed = 0;
    auto merge = [&](const QHash<QString, qint64> &values, qint64 ModelMeta::*field) {
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            const QString model = normModel(it.key());
            if (model.isEmpty() || it.value() <= 0) {
                continue;
            }
            ModelMeta &meta = m_cache[provider][model];
            if (meta.*field != it.value()) {
                meta.*field = it.value();
                ++changed;
            }
        }
    };

    merge(modelWindows, &ModelMeta::window);
    merge(modelMaxOutputTokens, &ModelMeta::maxOutput);

    if (changed > 0) {
        saveCache();
        emit metaChanged();
        LOGI(LogCat::Model) << "已合并 models.dev 缓存"
            << logf("provider", provider)
            << logf("changed", changed)
            << logf("bucketSize", m_cache.value(provider).size());
    }
    return changed;
}

QString ModelContextMetaStore::sourceToString(const Source source)
{
    switch (source) {
    case Source::User:
        return QStringLiteral("user");
    case Source::Cache:
        return QStringLiteral("cache");
    case Source::Default:
        return QStringLiteral("default");
    }
    return QStringLiteral("default");
}
