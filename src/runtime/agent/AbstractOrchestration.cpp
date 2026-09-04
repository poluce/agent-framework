#include "AbstractOrchestration.h"

#include "Agent.h"

// 默认实现放 .cpp：QPointer<Agent> 需要完整 Agent 类型（QObject 基类转换）。

void AbstractOrchestration::onUnitsClearing()
{
    m_primaryUnit.clear();
}

void AbstractOrchestration::onUnitInserted(Agent *unit)
{
    if (unit && !m_primaryUnit) {
        m_primaryUnit = unit;
    }
}

Agent *AbstractOrchestration::primaryUnit() const
{
    return m_primaryUnit;
}

bool AbstractOrchestration::isPrimary(const Agent *unit) const
{
    return unit && m_primaryUnit == unit;
}
