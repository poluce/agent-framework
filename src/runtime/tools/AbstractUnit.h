#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

/**
 * @brief 工具/外部源投递到单元邮箱的窄报文（工具层定义）
 *
 * 完整报文（AgentInboxMessage）在 agent 层，含内核内部状态与 core_ir 类型；
 * 工具层只声明来源与载荷，Agent 在边界补全 id/时间戳/优先级。
 */
struct UnitInboxMessage
{
    /// 来源标识（工具名 / agentId）；配方按此解释报文。
    QString fromAgentId;
    /// 人类可读兜底正文。
    QString content;
    /// 结构化消息类型（tool_event / task / status / ...），由配方自定义。
    QString type;
    /// 结构化载荷；content 保留为兜底。
    QJsonObject payload;
};

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
    /// 异步推送口：把事件投进本单元邮箱（容量/大小超限返回 false）。
    virtual bool enqueueInboxMessage(const UnitInboxMessage &msg) = 0;
};
