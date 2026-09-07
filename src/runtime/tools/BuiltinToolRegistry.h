#pragma once

#include "AbstractBuiltinTool.h"
#include "config/AgentMode.h"

#include <QHash>
#include <QList>
#include <QString>

#include <memory>
#include <optional>

class BuiltinToolRegistry
{
public:
    /// 未传表或空表 = 无内置工具。编码默认集用 defaultTools()。
    explicit BuiltinToolRegistry(
        std::optional<QList<std::shared_ptr<AbstractBuiltinTool>>> tools = std::nullopt);

    [[nodiscard]] static QList<std::shared_ptr<AbstractBuiltinTool>> defaultTools();

    QList<ToolSpec> specs() const;
    ToolSpec specForName(const QString &toolName) const;
    ToolPermissionDecision evaluatePermission(const QString &toolName,
                                              ToolScope toolScope,
                                              ApprovalMode approvalMode) const;

    [[nodiscard]] std::shared_ptr<AbstractBuiltinTool> builtinTool(const QString &toolName) const;

private:
    QHash<QString, std::shared_ptr<AbstractBuiltinTool>> m_builtinTools;
};
