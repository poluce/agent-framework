#include "AgentTaskManager.h"

AgentTaskManager::AgentTaskManager(QObject *parent)
    : QObject(parent)
{
}

const QJsonArray &AgentTaskManager::todos() const
{
    return m_todos;
}

void AgentTaskManager::setTodos(const QJsonArray &todos)
{
    m_todos = todos;
    emit stateChanged();
}
