#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

/**
 * GitIgnoreMatcher — 解析 .gitignore 规则并判断路径是否被排除。
 *
 * 功能：
 *   - 加载 .gitignore 文件（支持标准 gitignore 语法）
 *   - 内建默认排除目录（build/、.git/、node_modules/ 等）
 *   - 作为 .gitignore 不存在时的回退
 */
class GitIgnoreMatcher
{
public:
    struct Rule
    {
        QString pattern;
        QRegularExpression regex;
        bool negate = false;   // ! 前缀，表示重新包含
        bool dirOnly = false;  // 尾部 /，仅匹配目录
        bool anchored = false; // 包含 / 的模式，锚定到 .gitignore 所在位置
    };

    GitIgnoreMatcher();

    /// 从 .gitignore 文件加载规则。文件不存在时不报错。
    /// scopePath: 子目录路径（如 "subdir"），规则仅对该目录有效。
    bool loadFromFile(const QString &filePath, const QString &scopePath = {});

    /// 从原始文本内容加载规则。
    /// scopePath: 子目录路径，规则仅对该目录有效。
    void loadFromContent(const QString &content, const QString &scopePath = {});

    /// 添加一条始终排除的目录前缀（例如 "build"）。
    void addBuiltinExclusion(const QString &dirName);

    /// 判断相对路径是否被排除。
    /// relativePath: 相对工作区根的路径，使用正斜杠分隔。
    /// isDir:        true 表示该路径是一个目录。
    bool isExcluded(const QString &relativePath, bool isDir) const;

private:
    QList<Rule> m_rules;
    QStringList m_builtinExclusions; // 简单前缀匹配

    Rule parseRule(const QString &line, const QString &scopePath = {}) const;
    static QRegularExpression patternToRegex(const QString &pattern, bool anchored);
    static QString globToRegexPattern(const QString &glob);
};
