#include "OrchestrationRegistry.h"

#include "agent/AbstractOrchestration.h"

void OrchestrationRegistry::add(const QString &id, Factory factory)
{
    const QString key = id.trimmed();
    if (key.isEmpty() || !factory) {
        return;
    }
    m_factories.insert(key, std::move(factory));
}

std::unique_ptr<AbstractOrchestration> OrchestrationRegistry::create(const QString &id,
                                                                     QObject *parent) const
{
    const auto it = m_factories.constFind(id.trimmed());
    if (it == m_factories.cend() || !(*it)) {
        return nullptr;
    }
    return (*it)(parent);
}

bool OrchestrationRegistry::contains(const QString &id) const
{
    return m_factories.contains(id.trimmed());
}

QStringList OrchestrationRegistry::ids() const
{
    return m_factories.keys();
}
