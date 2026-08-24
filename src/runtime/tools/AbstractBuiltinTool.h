#pragma once

#include "ToolTypes.h"
#include <QString>
#include <QStringList>
#include <QList>
#include <QVariantMap>
#include <memory>

/**
 * @brief 内置核心工具基类
 *
 * 所有内置工具（文件系统/进程操作）继承此基类。
 * 子类必须实现 spec()，可选覆盖 isHeavyweight()/writeTargetPathForInput()/execute()。
 */
class AbstractBuiltinTool
{
public:
    virtual ~AbstractBuiltinTool() = default;

    [[nodiscard]] virtual ToolSpec spec() const = 0;

    [[nodiscard]] virtual bool isHeavyweight() const { return true; }

    virtual ToolResult execute(const ToolCall &call,
                               const QString &workspaceRoot,
                               const QString &workingDirectory,
                               const QVariantMap &threadSafeContext)
    {
        Q_UNUSED(call);
        Q_UNUSED(workspaceRoot);
        Q_UNUSED(workingDirectory);
        Q_UNUSED(threadSafeContext);
        return {};
    }

    [[nodiscard]] virtual QString writeTargetPathForInput(const QJsonObject &input) const
    {
        Q_UNUSED(input);
        return {};
    }

    /**
     * 写工具多目标路径（multi_edit 等）；默认回退到单键 writeTargetPathForInput。
     * 单键工具零改动走同一路径（返回 {单键} 或空列表）。
     */
    [[nodiscard]] virtual QStringList writeTargetPathsForInput(const QJsonObject &input) const
    {
        const QString single = writeTargetPathForInput(input);
        return single.isEmpty() ? QStringList{} : QStringList{single};
    }

    /// 稳定进度键（searching / writing / …）；展示文案不在工具里。
    [[nodiscard]] virtual QString progressKind() const { return QStringLiteral("running"); }

    /**
     * 调用对象摘要（路径/命令/pattern…），**不含**工具名。
     * UI 标题行已单独画 toolName；再带名前缀会叠字。
     * 生产主路径走 BuiltinToolRuntime::summarizeToolCall；本虚函数保留给工具自描述
     * 与会话工具同类语义。字段优先级与 Runtime 的 summarizePathInput 对齐。
     */
    [[nodiscard]] virtual QString summarizeCall(const ToolCall &call) const
    {
        auto first = [&](std::initializer_list<const char *> keys) -> QString {
            for (const char *key : keys) {
                const QString value = call.input.value(QLatin1String(key)).toString().trimmed();
                if (!value.isEmpty())
                    return value;
            }
            return {};
        };

        if (const QString path = first({"rootPath", "filePath", "notebookPath", "path"});
            !path.isEmpty()) {
            return path;
        }
        if (const QString command = first({"command", "cmd", "script"}); !command.isEmpty()) {
            const QString simplified = command.simplified();
            return simplified.size() <= 80 ? simplified
                                           : simplified.left(80) + QStringLiteral("...");
        }
        if (const QString target = first({"query", "pattern", "question"}); !target.isEmpty())
            return target;

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
};
