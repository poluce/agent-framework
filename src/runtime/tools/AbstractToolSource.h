#pragma once

#include "ToolTypes.h"
#include "config/AgentMode.h"

#include <QObject>
#include <QString>

#include <functional>

class BuiltinToolRuntime;

struct ToolInvokeContext
{
    QString agentId;
    QString workingDirectory;
    BuiltinToolRuntime *builtinRuntime = nullptr;
};

/// 内核工具总线的扩展口：声明 ToolSpec、执行后回调 ToolResult。
/// 内置 / 会话 / 外部（如 MCP 适配器）都是源；内核不认识 MCP。
/// specs() 报全量目录；谁可见由编排 AbstractOrchestration::toolVisible 裁。
class AbstractToolSource : public QObject
{
    Q_OBJECT

public:
    using Completion = std::function<void(ToolResult)>;

    explicit AbstractToolSource(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~AbstractToolSource() override = default;

    [[nodiscard]] virtual QString id() const = 0;
    [[nodiscard]] virtual QList<ToolSpec> specs() const = 0;
    [[nodiscard]] virtual bool owns(const QString &toolName) const = 0;
    virtual void invoke(const ToolCall &call,
                        const ToolInvokeContext &ctx,
                        Completion done) = 0;

    /// 默认：Auto 全放；ReadOnly 规格放行；ReadOnly 作用域硬拒；否则审批。
    [[nodiscard]] virtual ToolPermissionDecision evaluatePermission(
        const QString &toolName,
        ToolScope toolScope,
        ApprovalMode approvalMode) const
    {
        if (approvalMode == ApprovalMode::Auto) {
            return ToolPermissionDecision::Allow;
        }
        ToolSpec spec;
        for (const ToolSpec &item : specs()) {
            if (item.name == toolName) {
                spec = item;
                break;
            }
        }
        if (spec.permissionKind == ToolPermissionKind::ReadOnly) {
            return ToolPermissionDecision::Allow;
        }
        if (toolScope == ToolScope::ReadOnly) {
            return ToolPermissionDecision::Deny;
        }
        return ToolPermissionDecision::NeedsApproval;
    }

signals:
    void toolsChanged();
};
