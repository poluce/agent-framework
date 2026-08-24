#include "GitIgnoreMatcher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>

// ── 内建默认排除目录 ──
static const QStringList kBuiltinExclusions = {
    QStringLiteral(".git"),
    QStringLiteral("node_modules"),
    QStringLiteral("__pycache__"),
    QStringLiteral(".svn"),
    QStringLiteral(".hg"),
};

// ── GitIgnoreMatcher ──

GitIgnoreMatcher::GitIgnoreMatcher() = default;

bool GitIgnoreMatcher::loadFromFile(const QString &filePath, const QString &scopePath)
{
    QFile file(filePath);
    if (!file.exists())
        return false;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream stream(&file);
    loadFromContent(stream.readAll(), scopePath);
    return true;
}

void GitIgnoreMatcher::loadFromContent(const QString &content, const QString &scopePath)
{
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        m_rules.append(parseRule(line, scopePath));
    }
}

void GitIgnoreMatcher::addBuiltinExclusion(const QString &dirName)
{
    if (!m_builtinExclusions.contains(dirName))
        m_builtinExclusions.append(dirName);
}

bool GitIgnoreMatcher::isExcluded(const QString &relativePath, bool isDir) const
{
    // 1. 检查内建排除（硬编码的目录名）
    for (const QString &excl : m_builtinExclusions) {
        // 匹配 "dir"、  "dir/"、  "dir/anything"、  "anything/dir/"、  "anything/dir/anything"
        if (relativePath == excl
            || relativePath.startsWith(excl + QLatin1Char('/'))
            || relativePath.contains(QLatin1Char('/') + excl + QLatin1Char('/'))
            || relativePath.endsWith(QLatin1Char('/') + excl))
        {
            return true;
        }
    }

    // 2. 遍历 .gitignore 规则（按顺序，后面的覆盖前面的）
    bool ignored = false;
    for (const Rule &rule : m_rules) {
        // 目录规则只匹配目录
        if (rule.dirOnly && !isDir)
            continue;

        if (rule.regex.match(relativePath).hasMatch()) {
            ignored = !rule.negate; // negate → 重新包含
        }
    }

    return ignored;
}

// ── 私有方法 ──

GitIgnoreMatcher::Rule GitIgnoreMatcher::parseRule(const QString &line, const QString &scopePath) const
{
    Rule rule;
    QString pattern = line;

    // 处理 ! 前缀（取消排除）
    if (pattern.startsWith(QLatin1Char('!'))) {
        rule.negate = true;
        pattern = pattern.mid(1);
    }

    // 处理尾部 /（仅匹配目录）
    if (pattern.endsWith(QLatin1Char('/')) && pattern.size() > 1) {
        rule.dirOnly = true;
        pattern.chop(1); // 去掉尾部的 /
    }

    // 如果指定了 scopePath，将规则作用域限定到该子目录
    // 方法：在 pattern 前加上 scopePath/，使其成为锚定模式
    if (!scopePath.isEmpty()) {
        pattern = scopePath + QLatin1Char('/') + pattern;
        // 加了 scopePath 后自动变为锚定，无需再判断
        rule.anchored = true;
    } else {
        // 判断是否锚定：如果 pattern 包含 /（不是开头也不是结尾），则锚定
        // 或者以 / 开头也是锚定
        if (pattern.startsWith(QLatin1Char('/'))) {
            rule.anchored = true;
            pattern = pattern.mid(1); // 去掉开头的 /
        } else if (pattern.contains(QLatin1Char('/'))) {
            rule.anchored = true;
        }
    }

    rule.pattern = pattern;
    rule.regex = patternToRegex(pattern, rule.anchored);
    return rule;
}

QRegularExpression GitIgnoreMatcher::patternToRegex(const QString &pattern, bool anchored)
{
    const QString regexStr = globToRegexPattern(pattern);

    // 根据是否锚定构建完整正则
    QString fullRegex;
    if (anchored) {
        // 锚定：从路径开头匹配
        fullRegex = QStringLiteral("^") + regexStr + QStringLiteral("$");
    } else {
        // 非锚定：可以出现在路径的任意位置（目录边界处）
        // 用 .*/ 前缀允许匹配任意深度（包括零层深度）
        fullRegex = QStringLiteral("(?:^|.*/)") + regexStr + QStringLiteral("$");
    }

    QRegularExpression regex(fullRegex);
    regex.optimize();
    return regex;
}

QString GitIgnoreMatcher::globToRegexPattern(const QString &glob)
{
    // 分两步转换：
    // 1. 用占位符保护 **
    // 2. 转换 * 和 ? 以及转义正则特殊字符
    // 3. 将 ** 占位符替换为 .+

    QString result;
    result.reserve(glob.size() * 2);

    for (int i = 0; i < glob.size(); ++i) {
        const QChar c = glob[i];

        if (c == QLatin1Char('*') && i + 1 < glob.size() && glob[i + 1] == QLatin1Char('*')) {
            // ** — 使用 .+ 可匹配路径分隔符
            result.append(QStringLiteral("__DOUBLESTAR__"));
            ++i; // 跳过第二个 *
        } else if (c == QLatin1Char('*')) {
            // 单 * — 不匹配 /
            result.append(QStringLiteral("[^/]*"));
        } else if (c == QLatin1Char('?')) {
            // ? — 单个字符，不匹配 /
            result.append(QStringLiteral("[^/]"));
        } else if (c == QLatin1Char('.') || c == QLatin1Char('+')
                   || c == QLatin1Char('^') || c == QLatin1Char('$')
                   || c == QLatin1Char('(') || c == QLatin1Char(')')
                   || c == QLatin1Char('[') || c == QLatin1Char(']')
                   || c == QLatin1Char('{') || c == QLatin1Char('}')
                   || c == QLatin1Char('|') || c == QLatin1Char('\\')) {
            // 正则特殊字符，需要转义
            result.append(QLatin1Char('\\'));
            result.append(c);
        } else {
            result.append(c);
        }
    }

    // 将 ** 占位符替换为 .* （匹配零个或多个路径段，可跨目录边界）
    result.replace(QStringLiteral("__DOUBLESTAR__"), QStringLiteral(".*"));

    return result;
}
