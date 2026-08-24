#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QHash>
#include <optional>
#include <memory>

#include "tools/BuiltinToolRuntime.h"
#include "tools/AbstractSessionTool.h"

class AgentSession;
class Agent;

class SessionToolRuntime : public QObject
{
    Q_OBJECT
public:
    explicit SessionToolRuntime(AgentSession *session, QObject *parent = nullptr);
    ~SessionToolRuntime() override;

    using Completion = BuiltinToolRuntime::Completion;

    void handleToolCall(const QString &agentId, const ToolCall &call,
                        const QString &workingDirectory,
                        Completion completion);

    /// 返回所有已注册会话工具实例
    QList<std::shared_ptr<AbstractSessionTool>> sessionTools() const;

    /// 返回工具实例（按名称），可能为空
    std::shared_ptr<AbstractSessionTool> findByName(const QString &name) const;

    AgentSession *session() const { return m_session; }

private:
    void registerTool(std::shared_ptr<AbstractSessionTool> tool);

    AgentSession *m_session;
    QHash<QString, std::shared_ptr<AbstractSessionTool>> m_tools;
};
