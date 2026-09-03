#include "SessionToolRuntime.h"
#include "session/AgentTodoWriteTool.h"
#include "session/ConfigTool.h"
#include "agent/AgentSession.h"
#include "agent/Agent.h"
#include "BuiltinToolRuntime.h"

SessionToolRuntime::SessionToolRuntime(AgentSession *session, QObject *parent)
    : QObject(parent)
    , m_session(session)
{
    // 仅会话/单元工具。spawn/team 由组合根注入的编排提供。
    registerTool(std::make_shared<ConfigTool>());
    registerTool(std::make_shared<AgentTodoWriteTool>());
}

SessionToolRuntime::~SessionToolRuntime() = default;

void SessionToolRuntime::registerTool(std::shared_ptr<AbstractSessionTool> tool)
{
    const QString name = tool->spec().name.trimmed();
    if (name.isEmpty()) return;
    m_tools.insert(name, std::move(tool));
}

QList<std::shared_ptr<AbstractSessionTool>> SessionToolRuntime::sessionTools() const
{
    return m_tools.values();
}

std::shared_ptr<AbstractSessionTool> SessionToolRuntime::findByName(const QString &name) const
{
    return m_tools.value(name.trimmed());
}

void SessionToolRuntime::handleToolCall(const QString &agentId, const ToolCall &call,
                                         const QString &workingDirectory,
                                         Completion completion)
{
    Agent *caller = m_session->findById(agentId);
    if (!caller) {
        completion(BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("Agent 不存在。")));
        return;
    }

    auto it = m_tools.find(call.toolName.trimmed());
    if (it == m_tools.end()) {
        completion(BuiltinToolRuntime::makeErrorResult(
            call, QStringLiteral("未注册的会话工具: %1").arg(call.toolName)));
        return;
    }

    AbstractSessionTool *tool = it->get();

    // 优先异步通道（如 mcp_server add 等待 Ready），避免主线程阻塞。
    // 包装 completion：异步结果同样补齐 summary/preview（标题=调用对象）
    auto finish = [tool, call, completion = std::move(completion)](ToolResult result) {
        if (result.summaryText.trimmed().isEmpty())
            result.summaryText = tool->summarizeCall(call);
        if (result.previewText.trimmed().isEmpty() && !result.text.trimmed().isEmpty())
            result.previewText = result.text.simplified().left(80);
        completion(std::move(result));
    };

    if (tool->tryExecuteAsync(caller, call, workingDirectory, finish)) {
        return;
    }

    ToolResult result = tool->execute(caller, call, workingDirectory);
    finish(std::move(result));
}
