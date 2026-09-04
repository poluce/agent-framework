#pragma once

#include <QJsonArray>
#include <QString>

/**
 * @brief 执行单元的窄视图（工具层消费方定义；agent/Agent 实现）
 *
 * 工具层只依赖本接口，不依赖 agent/ 具体类。单元与会话同生命周期，
 * 异步路径可捕获本接口指针；但不要捕获 SessionToolContext 本身（栈对象）。
 */
class AbstractUnit
{
public:
    virtual ~AbstractUnit() = default;

    virtual QString agentId() const = 0;
    virtual QJsonArray todos() const = 0;
    virtual void setTodos(const QJsonArray &todos) = 0;
    virtual void appendSessionEvent(const QString &text) = 0;
};
