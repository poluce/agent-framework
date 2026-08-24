#pragma once

#include "skills/FileSkill.h"
#include "skills/FileSkillLoader.h"
#include "tools/AbstractBuiltinTool.h"
#include "tools/BuiltinToolRuntime.h"

#include <QJsonArray>
#include <QJsonObject>

#include <optional>

class SkillListTool : public AbstractBuiltinTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    [[nodiscard]] QString progressKind() const override { return QStringLiteral("listing_skills"); }
    void setSkillLoader(FileSkillLoader *loader) { m_loader = loader; }
    ToolResult execute(const ToolCall &call,
                       const QString &workspaceRoot,
                       const QString &workingDirectory,
                       const QVariantMap &threadSafeContext) override;

private:
    FileSkillLoader *m_loader = nullptr;
};

inline ToolSpec SkillListTool::spec() const
{
    return ToolSpecBuilder("skill_list", QStringLiteral("列出可用技能。无参数返回所有技能摘要，name 参数调用指定技能。"))
        .input("name", "string", QStringLiteral("可选，技能目录名，与 / 后输入的名称一致"))
        .output("skills", "jsonArray", QStringLiteral("技能清单"))
        .output("note", "string", QStringLiteral("补充说明"))
        .build();
}

inline ToolResult SkillListTool::execute(const ToolCall &call,
                                         const QString &workspaceRoot,
                                         const QString &workingDirectory,
                                         const QVariantMap &threadSafeContext)
{
    Q_UNUSED(workspaceRoot);
    Q_UNUSED(threadSafeContext);
    Q_UNUSED(workingDirectory);

    const QString targetName = call.input.value(QStringLiteral("name")).toString().trimmed();

    QJsonArray skills;
    QString note;

    if (targetName.isEmpty()) {
        // 无参数：返回所有可调用技能摘要（模型仅能看到 userInvocable 的技能）
        const QList<FileSkill> visible = m_loader
            ? m_loader->userInvocableSkills()
            : QList<FileSkill>{};
        for (const FileSkill &s : visible) {
            QJsonObject item;
            item.insert(QStringLiteral("name"), s.dirName);
            item.insert(QStringLiteral("displayName"), s.displayName());
            item.insert(QStringLiteral("description"), s.description);
            item.insert(QStringLiteral("slash"), s.slash());
            skills.append(item);
        }
        note = QStringLiteral("共 %1 个可用技能。使用 skill_list(name=\"<name>\") 调用。")
                   .arg(skills.size());

        QStringList textLines;
        for (const QJsonValue &v : skills) {
            const QJsonObject obj = v.toObject();
            textLines.append(QStringLiteral("  %1 — %2")
                .arg(obj.value(QStringLiteral("slash")).toString(),
                     obj.value(QStringLiteral("description")).toString()));
        }
        const QString text = textLines.join(QStringLiteral("\n"));

        QJsonObject payload;
        payload.insert(QStringLiteral("skills"), skills);
        payload.insert(QStringLiteral("note"), note);

        return BuiltinToolRuntime::makeSuccessResult(call, text,
            QStringLiteral("skillListResult"), payload);
    }

    // 加载指定技能 → 系统级注入（与用户 / 命令同一通道）
    const std::optional<FileSkill> found = m_loader
        ? m_loader->findByDirName(targetName)
        : std::optional<FileSkill>{};
    if (!found.has_value() || !found->userInvocable) {
        return BuiltinToolRuntime::makeErrorResult(call,
            QStringLiteral("未找到技能: %1。使用 skill_list() 查看所有可用技能。").arg(targetName));
    }

    if (found->disableModelInvocation) {
        return BuiltinToolRuntime::makeErrorResult(call,
            QStringLiteral("技能 '%1' 仅限用户通过 / 命令调用。").arg(found->displayName()));
    }

    QJsonObject item;
    item.insert(QStringLiteral("name"), found->dirName);
    item.insert(QStringLiteral("displayName"), found->displayName());
    item.insert(QStringLiteral("description"), found->description);
    item.insert(QStringLiteral("slash"), found->slash());
    item.insert(QStringLiteral("body"), found->body);
    if (!found->allowedTools.isEmpty()) {
        item.insert(QStringLiteral("allowedTools"),
                    QJsonArray::fromStringList(found->allowedTools));
    }
    skills.append(item);
    note = QStringLiteral("技能 '%1' 已加载。请按照 body 中的指令执行。").arg(found->displayName());

    QStringList textLines;
    for (const QJsonValue &v : skills) {
        const QJsonObject obj = v.toObject();
        if (obj.contains(QStringLiteral("body"))) {
            textLines.append(obj.value(QStringLiteral("body")).toString());
        }
    }
    const QString text = textLines.join(QStringLiteral("\n"));

    QJsonObject payload;
    payload.insert(QStringLiteral("skills"), skills);
    payload.insert(QStringLiteral("note"), note);

    return BuiltinToolRuntime::makeSuccessResult(call, text,
        QStringLiteral("skillListResult"), payload);
}
