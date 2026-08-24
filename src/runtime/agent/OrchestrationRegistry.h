#pragma once

#include <functional>
#include <memory>

#include <QHash>
#include <QString>
#include <QStringList>

class AbstractOrchestration;
class QObject;

/// 按 id 创建编排配方。内核只提供表；内置配方名与工厂在产品壳登记。
class OrchestrationRegistry
{
public:
    using Factory = std::function<std::unique_ptr<AbstractOrchestration>(QObject *parent)>;

    void add(const QString &id, Factory factory);
    [[nodiscard]] std::unique_ptr<AbstractOrchestration> create(const QString &id,
                                                                QObject *parent = nullptr) const;
    [[nodiscard]] bool contains(const QString &id) const;
    [[nodiscard]] QStringList ids() const;

private:
    QHash<QString, Factory> m_factories;
};
