#pragma once

#include "ToolTypes.h"
#include "config/AgentMode.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class AbstractSession;
class BuiltinToolRuntime;

struct ToolInvokeContext
{
    QString agentId;
    QString workingDirectory;
    /// 会话窄视图（异步推送路径用；可能为空）。
    AbstractSession *session = nullptr;
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

    /// 会话关闭通知：ToolCoordinator 析构（= 会话销毁）时对每个已登记源调用。
    /// 有进程/临时资源的源在此收尾（杀进程、删临时文件）；默认空。
    virtual void sessionClosing() {}

    /// 会话清空通知：AgentSession::clear()（单元全删、会话继续）时调用。
    /// 源应丢弃单元级状态（订阅、临时工具、进程）；持久资源保留。默认空。
    virtual void sessionCleared() {}

    /// 该 agent 可见的逻辑分组（ToolSpec::group）集合。空 = 不限（所有分组可见）。
    /// 与 toolVisible 的关系：toolVisible 在目录期按「单工具名」裁剪（specsForAgent 列不列）；
    /// 本口在调用期按「分组」强制（dispatch 时 group 不在本集合内的工具直接拒绝，不进 invoke）。
    /// 两者 AND：工具要被实际调用，必须同时通过 toolVisible（目录里有）与分组校验（能调到）。
    /// 适合表达「整个服务器/分组对该 agent 不可见」的语义（如 MCP per-role 服务器裁剪）。
    [[nodiscard]] virtual QStringList visibleGroups(const QString &agentId) const
    {
        Q_UNUSED(agentId);
        return {};
    }

signals:
    void toolsChanged();
};
