#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

/**
 * @brief glob brace expansion（顶层 {a,b} 展开为多 pattern）。
 *
 * QRegularExpression::fromWildcard 不支持 brace expansion（{a,b} 原样当字面量）。
 * 本 helper 只展开顶层 {a,b}（不递归嵌套，防产物指数爆炸）；
 * 无 brace 时返回 {原 pattern}；展开数超 kMaxExpandedPatterns 返回空（调用方报错）。
 *
 * 例：src/**.{cpp,h} 展开为 [src/**.cpp, src/**.h]
 */
namespace GlobBraceExpansion {

inline constexpr int kMaxExpandedPatterns = 16;

/** 展开顶层 {a,b} 为多个 pattern；返回空 = 展开数超上限或 pattern 为空。 */
inline QStringList expand(const QString &pattern)
{
    QStringList result{pattern};
    bool anyExpanded = true;
    // 每轮展开一个 brace；无 brace 可展或达到上限轮次即停（防病态输入）
    for (int round = 0; round < 4 && anyExpanded; ++round) {
        QStringList next;
        anyExpanded = false;
        for (const QString &p : result) {
            const int open = p.indexOf(QLatin1Char('{'));
            const int close = open >= 0 ? p.indexOf(QLatin1Char('}'), open + 1) : -1;
            if (close < 0) {
                next.append(p); // 无闭合 brace，按字面
                continue;
            }
            anyExpanded = true;
            const QString prefix = p.left(open);
            const QString suffix = p.mid(close + 1);
            const QStringList branches = p.mid(open + 1, close - open - 1).split(QLatin1Char(','));
            for (const QString &branch : branches) {
                if (branch.isEmpty())
                    continue; // 空分支跳过（{a,} 的尾部空）
                next.append(prefix + branch + suffix);
                if (next.size() > kMaxExpandedPatterns)
                    return {}; // 超上限
            }
        }
        result = next;
    }
    return result;
}

/** 把单个 glob pattern 编译为正则；大小写由调用方传入。 */
inline QRegularExpression toRegex(const QString &pattern, Qt::CaseSensitivity cs)
{
    return QRegularExpression::fromWildcard(pattern, cs,
                                            QRegularExpression::UnanchoredWildcardConversion);
}

} // namespace GlobBraceExpansion
