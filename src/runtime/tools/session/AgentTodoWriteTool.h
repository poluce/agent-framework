#pragma once

#include "agent/Agent.h"

#include "tools/AbstractSessionTool.h"

class AgentTodoWriteTool : public AbstractSessionTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    ToolResult execute(Agent *caller,
                       const ToolCall &call,
                       const QString &workingDirectory) override;
};

namespace {

QJsonObject todoItemSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("content"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"), QStringLiteral("todo 内容")},
            }},
            {QStringLiteral("status"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"), QStringLiteral("todo 状态，通常为 pending / in_progress / completed")},
            }},
            {QStringLiteral("activeForm"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"), QStringLiteral("todo 的进行时描述")},
            }},
        }},
        {QStringLiteral("required"), QJsonArray{
            QStringLiteral("content"),
            QStringLiteral("status"),
            QStringLiteral("activeForm"),
        }},
    };
}

QJsonObject todoListSchema()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), todoItemSchema()},
    };
}

} // namespace

inline ToolSpec AgentTodoWriteTool::spec() const
{
    return ToolSpecBuilder("agent_todo_write", QStringLiteral("更新当前 agent 的 todo 清单。"))
        .requiredInput("todos", todoListSchema(), QStringLiteral("新的 todo 列表"))
        .output("todos", todoListSchema(), QStringLiteral("当前 todo 列表"))
        .build();
}

inline ToolResult AgentTodoWriteTool::execute(Agent *caller, const ToolCall &call, const QString &workingDirectory)
{
    Q_UNUSED(workingDirectory);
    const QJsonArray todos = call.input.value(QStringLiteral("todos")).toArray();
    if (todos.isEmpty()) {
        return makeError(call, QStringLiteral("agent_todo_write 缺少 todos。"));
    }

    bool allCompleted = true;
    for (const QJsonValue &value : todos) {
        const QJsonObject todo = value.toObject();
        if (todo.value(QStringLiteral("content")).toString().trimmed().isEmpty()
            || todo.value(QStringLiteral("status")).toString().trimmed().isEmpty()
            || todo.value(QStringLiteral("activeForm")).toString().trimmed().isEmpty()) {
            return makeError(call, QStringLiteral("每个 todo 条目都必须包含 content、status 和 activeForm。"));
        }
        if (todo.value(QStringLiteral("status")).toString() != QStringLiteral("completed")) {
            allCompleted = false;
        }
    }

    const int previousSize = caller->taskManager()->todos().size();
    caller->taskManager()->setTodos(allCompleted ? QJsonArray{} : todos);

    return makeSuccess(call,
                       QStringLiteral("已更新 todo 列表：%1 项（之前 %2 项）。").arg(todos.size()).arg(previousSize),
                       QStringLiteral("agentTodoWriteResult"),
                       QJsonObject{{QStringLiteral("todos"), caller->taskManager()->todos()}});
}
