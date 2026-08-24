#pragma once

#include <QHash>
#include <QStringList>

#include <functional>
#include <memory>

class AbstractProvider;

/// 内置 Provider 注册表：凭据 / 循环 / 工厂共用同一套 type id。
/// 由组合根持有并注入；运行时不得再走进程级单例。
class ProviderService
{
public:
    using FactoryFunc = std::function<std::unique_ptr<AbstractProvider>()>;

    ProviderService() = default;
    ProviderService(const ProviderService &) = delete;
    ProviderService &operator=(const ProviderService &) = delete;

    void registerProvider(const QString &type, FactoryFunc factory);
    void registerBuiltins();
    std::unique_ptr<AbstractProvider> create(const QString &type, const char *callSite = nullptr) const;
    bool supportsProvider(const QString &type) const;
    /// 稳定排序，供凭据 UI / 协议快照。
    QStringList availableProviderTypes() const;

    /// trim + lower；空串保持空（不默认填 responses）。
    static QString normalizeProviderType(const QString &type);
    static QString providerDisplayName(const QString &type);

private:
    QHash<QString, FactoryFunc> m_factories;
};
