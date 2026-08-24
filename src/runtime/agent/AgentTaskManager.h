#pragma once

#include <QJsonArray>
#include <QObject>

class AgentTaskManager : public QObject
{
    Q_OBJECT

public:
    explicit AgentTaskManager(QObject *parent = nullptr);
    ~AgentTaskManager() override = default;

    const QJsonArray &todos() const;
    void setTodos(const QJsonArray &todos);

signals:
    void stateChanged();

private:
    QJsonArray m_todos;
};
