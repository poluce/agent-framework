#pragma once

#include "AgentMode.h"
#include "tools/ToolTypes.h"

#include <optional>

struct AgentPromptContext;

/// 执行模式策略口。内核只认这张口；Planning / AutoDebug 等具体策略由组合根注入。
class AgentModePolicy
{
public:
    virtual ~AgentModePolicy() = default;

    [[nodiscard]] virtual AgentMode mode() const = 0;
    [[nodiscard]] virtual QString buildPrompt(const AgentPromptContext &ctx) = 0;

    // 返回值有值表示本次工具调用被策略拦截，AbstractLoop 应直接回填 ToolResult。
    [[nodiscard]] virtual std::optional<ToolResult> beforeToolCall(const ToolCall &toolCall) = 0;

    // 工具被 Loop 接受但尚未产生 ToolResult 时推进状态，例如 ask_question 进入等待态。
    virtual void afterToolAccepted(const ToolCall &toolCall) = 0;

    // 工具真实完成后推进状态。
    virtual void afterToolCall(const ToolCall &toolCall, const ToolResult &result) = 0;

    // 模型本轮输出完成后的 checkpoint。返回隐藏注入消息表示需要静默开启下一轮。
    [[nodiscard]] virtual std::optional<QString> afterMessageCompleted() = 0;

    // 工作目录变化时刷新策略内部路径；返回隐藏系统消息时由 Loop 注入账本。
    [[nodiscard]] virtual std::optional<QString> onWorkspaceChanged(const QString &oldPath,
                                                                    const QString &newPath) = 0;
};
