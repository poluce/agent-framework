#pragma once

#include "config/ModelTokenDefaults.h"

#include <QHash>
#include <QObject>
#include <QString>

/**
 * 模型上下文窗口 + 最大输出元数据：两文件 + 代码默认。
 *
 * resolve 优先级（窗口与最大输出各自独立 resolve，Source 一致）：
 *   user(model_meta.json, instanceId+modelId)
 *   > cache(model_context_cache.json, providerType+modelId)
 *   > default（窗口 kDefaultContextWindow；最大输出按 provider 查表）
 *
 * cache 按 provider 全局；user 覆盖仅写 meta，永不被 models.dev 自动覆盖。
 *
 * kDefaultContextWindow / kDefaultMaxOutputTokens 是默认值的唯一字面量源；
 * runtime 字段默认与客户端缺字段回落均引用此常量，禁止再抄数字。
 */
class ModelContextMetaStore final : public QObject
{
    Q_OBJECT

public:
    /// 未知模型时的回落窗口（token）；字面量源见 ModelTokenDefaults
    static constexpr qint64 kDefaultContextWindow = ModelTokenDefaults::kContextWindow;
    /// 未知 provider 时的回落最大输出（token）；已知 provider 按表更精确
    static constexpr qint64 kDefaultMaxOutputTokens = ModelTokenDefaults::kMaxOutputTokens;

    enum class Source {
        User,
        Cache,
        Default,
    };

    struct ResolveResult {
        qint64 contextWindow = kDefaultContextWindow;
        qint64 maxOutputTokens = kDefaultMaxOutputTokens;
        Source contextWindowSource = Source::Default;
        Source maxOutputSource = Source::Default;
    };

    explicit ModelContextMetaStore(QObject *parent = nullptr);
    /// 空串路径 = 不落盘（内存-only）；组合根应传入产品路径。
    explicit ModelContextMetaStore(const QString &cachePath,
                                   const QString &metaPath,
                                   QObject *parent = nullptr);

    void load();
    void saveCache() const;
    void saveMeta() const;

    [[nodiscard]] ResolveResult resolve(const QString &providerType,
                                        const QString &instanceId,
                                        const QString &modelId) const;

    [[nodiscard]] qint64 resolveWindow(const QString &providerType,
                                       const QString &instanceId,
                                       const QString &modelId) const;

    /// 仅解析最大输出 token（窗口缺省）；优先级 user>cache>default（default 按 provider 查表）
    [[nodiscard]] qint64 resolveMaxOutputTokens(const QString &providerType,
                                                const QString &instanceId,
                                                const QString &modelId) const;

    /**
     * 用户覆盖；contextWindow<=0 清除窗口覆盖，maxOutputTokens<=0 清除输出覆盖；
     * 任一参数默认 -1 = 不改该键。两键独立。
     */
    bool setUserOverride(const QString &instanceId,
                         const QString &modelId,
                         qint64 contextWindow = -1,
                         qint64 maxOutputTokens = -1);

    /// 删除某凭据实例下全部用户覆盖
    void removeInstance(const QString &instanceId);

    /**
     * 将 models.dev 等来源的窗口/最大输出写入 provider 全局 cache。
     * 仅写入/更新 cache；不碰 user meta。任一 map 为空则不改对应键。
     * @return 实际变更的条目数（窗口或输出任一变化计 1）
     */
    int mergeProviderCache(const QString &providerType,
                           const QHash<QString, qint64> &modelWindows,
                           const QHash<QString, qint64> &modelMaxOutputTokens = {});

    [[nodiscard]] static QString sourceToString(Source source);

    /// 单模型元数据（窗口 + 最大输出；0=未提供）
    struct ModelMeta {
        qint64 window = 0;
        qint64 maxOutput = 0;
    };

signals:
    void metaChanged();

private:
    /// 按 providerType 查已知最大输出上限；未知回落 kDefaultMaxOutputTokens
    static qint64 defaultMaxOutputTokensForProvider(const QString &providerType);

    static QString normProvider(const QString &providerType);
    static QString normModel(const QString &modelId);
    static QString normInstance(const QString &instanceId);

    QString m_cachePath;
    QString m_metaPath;
    /// provider → (modelId → 元数据)
    QHash<QString, QHash<QString, ModelMeta>> m_cache;
    /// instanceId → (modelId → 元数据)
    QHash<QString, QHash<QString, ModelMeta>> m_userMeta;
};
