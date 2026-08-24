#pragma once

#include "ToolTypes.h"

#include <functional>

class Agent;

namespace session_tool_detail {

/** static 工厂不能调虚 summarizeCall；从 action/name 粗提调用对象。 */
inline QString callTargetSummary(const ToolCall &call)
{
    const QString action = call.input.value(QStringLiteral("action")).toString().trimmed();
    const QString name = call.input.value(QStringLiteral("name")).toString().trimmed();
    if (!action.isEmpty() && !name.isEmpty())
        return QStringLiteral("%1 %2").arg(action, name);
    if (!action.isEmpty())
        return action;
    if (!name.isEmpty())
        return name;
    return call.toolName;
}

inline void fillCallOutcome(ToolResult *result, const ToolCall &call, const QString &text)
{
    result->summaryText = callTargetSummary(call);
    result->previewText = text.simplified().left(80);
}

} // namespace session_tool_detail

/**
 * @brief 会话级工具基类
 *
 * 所有操作 Agent/AgentSession/Team 的会话工具继承此基类。
 * 永远在主线程执行，不需要线程安全快照，不需要路径安全。
 * 子类必须实现 spec() 和 execute()。
 * 耗时操作可覆写 tryExecuteAsync()，在主线程异步完成后回调，避免阻塞 UI。
 */
class AbstractSessionTool
{
public:
    using AsyncCompletion = std::function<void(ToolResult)>;

    virtual ~AbstractSessionTool() = default;

    [[nodiscard]] virtual ToolSpec spec() const = 0;

    virtual ToolResult execute(Agent *caller,
                               const ToolCall &call,
                               const QString &workingDirectory) = 0;

    /// 默认不接管；返回 true 表示已异步完成并回调 completion，调用方不得再走 sync execute。
    virtual bool tryExecuteAsync(Agent * /*caller*/,
                                 const ToolCall & /*call*/,
                                 const QString & /*workingDirectory*/,
                                 AsyncCompletion /*completion*/)
    {
        return false;
    }

    [[nodiscard]] virtual QString summarizeCall(const ToolCall &call) const
    {
        return session_tool_detail::callTargetSummary(call);
    }

protected:
    static ToolResult makeSuccess(const ToolCall &call, const QString &text,
                                  const QString &payloadType = {},
                                  const QJsonObject &payload = {});
    static ToolResult makeError(const ToolCall &call, const QString &text);
};

inline ToolResult AbstractSessionTool::makeSuccess(const ToolCall &call, const QString &text,
                                            const QString &payloadType, const QJsonObject &payload)
{
    ToolResult result;
    result.toolName = call.toolName;
    result.toolUseId = call.id;
    result.success = true;
    result.isError = false;
    result.category = ToolResultCategory::Success;
    result.text = text;
    result.payloadType = payloadType;
    result.payload = payload;
    session_tool_detail::fillCallOutcome(&result, call, text);
    return result;
}

inline ToolResult AbstractSessionTool::makeError(const ToolCall &call, const QString &text)
{
    ToolResult result;
    result.toolName = call.toolName;
    result.toolUseId = call.id;
    result.success = false;
    result.isError = true;
    result.category = ToolResultCategory::Error;
    result.text = text;
    session_tool_detail::fillCallOutcome(&result, call, text);
    return result;
}
