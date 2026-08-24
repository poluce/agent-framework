#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

// 从 SKILL.md frontmatter 解析出的运行时 skill 数据
struct FileSkill
{
    QString dirName;        // 目录名 = 命令名，如 "git" → /git
    QString dirPath;        // skill 目录绝对路径
    QString name;           // frontmatter name，为空时等于 dirName
    QString description;    // frontmatter description
    QString body;           // SKILL.md 正文（YAML frontmatter 之后的部分）
    QStringList allowedTools;
    bool disableModelInvocation = false;
    bool userInvocable = true;

    QString displayName() const { return name.isEmpty() ? dirName : name; }
    QString slashName() const {
        int idx = dirName.lastIndexOf(QStringLiteral("--"));
        return (idx != -1) ? dirName.mid(idx + 2) : dirName;
    }
    QString slash() const { return QStringLiteral("/%1").arg(slashName()); }
};
