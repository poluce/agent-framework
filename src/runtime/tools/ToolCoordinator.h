#pragma once

#include "ToolTypes.h"
#include "AbstractSession.h"
#include "config/AgentMode.h"
#include "logging/LogManager.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

#include "SessionToolRuntime.h"

class AbstractBuiltinTool;
class AbstractToolSource;
class AbstractUnit;
class BuiltinToolRuntime;

// ====================================================================
// BuiltinToolRegistry — 内置工具目录
// ====================================================================
class BuiltinToolRegistry
{
public:
    BuiltinToolRegistry();

    QList<ToolSpec> specs() const;
    ToolSpec specForName(const QString &toolName) const;
    ToolPermissionDecision evaluatePermission(const QString &toolName,
                                              ToolScope toolScope,
                                              ApprovalMode approvalMode) const;

    [[nodiscard]] std::shared_ptr<AbstractBuiltinTool> builtinTool(const QString &toolName) const;

private:
    QHash<QString, std::shared_ptr<AbstractBuiltinTool>> m_builtinTools;
};

// ====================================================================
// ToolCoordinator — 工具源表：内置 / 会话 / 外部（先登记的赢）
// ====================================================================
class ToolCoordinator : public QObject
{
    Q_OBJECT

public:
    using Completion = std::function<void(ToolResult)>;

    explicit ToolCoordinator(AbstractSession *session,
                             AbstractToolSource *externalSource = nullptr,
                             QObject *parent = nullptr);
    ~ToolCoordinator() override;

    /// 追加外部源（非拥有）。同一指针不重复登记。
    void addSource(AbstractToolSource *source);

    /// 全量目录（登记 / 测试）；不含编排裁剪。
    QList<ToolSpec> allSpecs() const;
    /// 该单元实际可调用的目录（编排 toolVisible 裁剪后）。
    QList<ToolSpec> specsForAgent(const QString &agentId) const;
    [[nodiscard]] ToolSpec specForName(const QString &toolName) const;

    void dispatch(const QString &agentId, const ToolCall &call,
                  const QString &workingDirectory,
                  BuiltinToolRuntime &agentRuntime,
                  Completion completion);

    ToolPermissionDecision evaluatePermission(const QString &toolName,
                                               ToolScope toolScope,
                                               ApprovalMode approvalMode) const;

    BuiltinToolRegistry *registry() { return &m_registry; }
    SessionToolRuntime *sessionRuntime() const { return m_sessionRuntime.get(); }
    AbstractSession *session() const { return m_session; }

signals:
    void toolsUpdated();

private:
    [[nodiscard]] QList<AbstractToolSource *> sources() const;
    [[nodiscard]] AbstractToolSource *sourceOwning(const QString &toolName) const;
    [[nodiscard]] QList<ToolSpec> collectSpecs(AbstractSession *session, AbstractUnit *unit) const;

    AbstractSession *m_session;
    BuiltinToolRegistry m_registry;
    std::unique_ptr<SessionToolRuntime> m_sessionRuntime;
    AbstractToolSource *m_builtinSource = nullptr;
    AbstractToolSource *m_sessionSource = nullptr;
    QList<AbstractToolSource *> m_externalSources;
};
