#pragma once

#include "FileLineEnding.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <vector>

/**
 * @brief 结构化补丁（真 diff）：行级 LCS diff + hunk 化输出。
 *
 * 旧版把旧行全标 '-'、新行全标 '+'（假 diff），改一行也整文件红绿，
 * 误导模型对改动范围的判断。本版：
 * - 内部先 \r\n→\n 归一行尾（CRLF/混合行尾不污染 diff，自包含）
 * - 共同前缀/后缀修剪缩小问题域
 * - 行级 LCS（对 ≤ 数千行文件毫秒级；用 DP 只保留 2 行滚动窗口 + 回溯表）
 * - hunk 化输出（context=3，相邻变化间隔 ≤6 合并），行号 1-based
 * - 退化保护：中段 >20 万行或中段行数超 LCS 预算 → 合法单块（块头位置真实）
 *
 * 输出格式兼容旧版：{oldStart, oldLines, newStart, newLines, lines} 数组；
 * lines 元素前缀 '-'/'+'（变化行）与 ' '（context 行）；
 * oldLines/newLines 为 hunk 覆盖的旧/新侧总行数（含 context）。
 */

namespace {

// 共同前缀/后缀修剪：把 diff 问题缩小到中段。
// 保留 kContext 行不修剪，作为 hunk 的前/后置 context（否则改 1 行也整文件单块）。
struct TrimmedRange
{
    int prefixLen = 0;
    int suffixLen = 0;
};

inline TrimmedRange trimCommonEdges(const QStringList &oldLines, const QStringList &newLines)
{
    constexpr int kKeepContext = 3;
    const int minLen = std::min(oldLines.size(), newLines.size());
    int p = 0;
    // 前缀：保留至少 kKeepContext 行给前置 context
    while (p < minLen - kKeepContext && oldLines[p] == newLines[p])
        ++p;
    int s = 0;
    // 后缀：保留至少 kKeepContext 行给后置 context（且不越过已修剪的前缀）
    while (s < minLen - kKeepContext - p
           && oldLines[oldLines.size() - 1 - s] == newLines[newLines.size() - 1 - s])
        ++s;
    return {p, s};
}

// LCS 行级 diff：返回 edit script（'='/'-'/'+'，从前到后）。
// DP 用 2 行滚动窗口（O(m) 空间），回溯表用 uint8 逐格（O(n*m) 空间，上限受预算保护）。
// 预算：n*m > kMaxLcsCells（2000 万格 ≈ 20MB）→ 返回空 script（调用方退化单块）。
inline std::vector<char> lcsEditScript(const QStringList &a, const QStringList &b,
                                       int n, int m, bool &budgetExceeded)
{
    budgetExceeded = false;
    if (n == 0)
        return std::vector<char>(static_cast<std::size_t>(m), '+');
    if (m == 0)
        return std::vector<char>(static_cast<std::size_t>(n), '-');

    constexpr qint64 kMaxLcsCells = 20'000'000;
    if (static_cast<qint64>(n) * m > kMaxLcsCells) {
        budgetExceeded = true;
        return {};
    }

    // 回溯表：1 = 来自上方（delete），2 = 来自左方（insert），3 = 来自对角（keep）
    // 先算 LCS 长度（2 行滚动），再填回溯表（逐格 uint8）
    std::vector<uint8_t> back(static_cast<std::size_t>(n) * m, 0);
    std::vector<int> prevRow(static_cast<std::size_t>(m) + 1, 0);
    std::vector<int> curRow(static_cast<std::size_t>(m) + 1, 0);
    for (int i = 1; i <= n; ++i) {
        curRow[0] = 0;
        for (int j = 1; j <= m; ++j) {
            if (a[i - 1] == b[j - 1]) {
                curRow[j] = prevRow[j - 1] + 1;
                back[static_cast<std::size_t>(i - 1) * m + (j - 1)] = 3;
            } else if (prevRow[j] >= curRow[j - 1]) {
                curRow[j] = prevRow[j];
                back[static_cast<std::size_t>(i - 1) * m + (j - 1)] = 1; // delete
            } else {
                curRow[j] = curRow[j - 1];
                back[static_cast<std::size_t>(i - 1) * m + (j - 1)] = 2; // insert
            }
        }
        std::swap(prevRow, curRow);
    }

    // 回溯生成 script（反向 → 反转）
    std::vector<char> script;
    script.reserve(static_cast<std::size_t>(n + m));
    int i = n, j = m;
    while (i > 0 && j > 0) {
        const uint8_t dir = back[static_cast<std::size_t>(i - 1) * m + (j - 1)];
        if (dir == 3) {
            script.push_back('=');
            --i;
            --j;
        } else if (dir == 1) {
            script.push_back('-');
            --i;
        } else {
            script.push_back('+');
            --j;
        }
    }
    while (i > 0) {
        script.push_back('-');
        --i;
    }
    while (j > 0) {
        script.push_back('+');
        --j;
    }
    std::reverse(script.begin(), script.end());
    return script;
}

// 整段单块（退化/超预算时用；块头位置真实）
inline QJsonObject wholeBlock(const QStringList &oldLines, const QStringList &newLines,
                              int oldStart, int newStart)
{
    QJsonArray lines;
    for (const QString &s : oldLines)
        lines.append(QLatin1Char('-') + s);
    for (const QString &s : newLines)
        lines.append(QLatin1Char('+') + s);
    return QJsonObject{
        {QStringLiteral("oldStart"), oldStart},
        {QStringLiteral("oldLines"), static_cast<int>(oldLines.size())},
        {QStringLiteral("newStart"), newStart},
        {QStringLiteral("newLines"), static_cast<int>(newLines.size())},
        {QStringLiteral("lines"), lines},
    };
}

// 由 edit script 组装 hunks（context=3；中段坐标 → 全文件 1-based 用 prefixLen）
inline QJsonArray hunksFromScript(const QStringList &oldLines, const QStringList &newLines,
                                  const std::vector<char> &script,
                                  int prefixLen, int n, int m)
{
    constexpr int kContext = 3;

    // 收集变化点（中段 0-based 坐标）
    struct Op
    {
        int oldLine = 0;
        int newLine = 0;
        char op = 0;
    };
    std::vector<Op> ops;
    int curOld = 0, curNew = 0;
    for (const char c : script) {
        if (c == '=') {
            ++curOld;
            ++curNew;
        } else if (c == '-') {
            ops.push_back({curOld, curNew, '-'});
            ++curOld;
        } else {
            ops.push_back({curOld, curNew, '+'});
            ++curNew;
        }
    }
    if (ops.empty())
        return {};

    // 分组：相邻变化间隔（两侧 keep 行数的最小值）≤ 2*context 时合并
    std::vector<std::vector<Op>> groups;
    for (const Op &op : ops) {
        if (groups.empty()) {
            groups.push_back({op});
            continue;
        }
        auto &last = groups.back();
        const Op &prev = last.back();
        const int gapOld = op.oldLine - (prev.oldLine + (prev.op == '-' ? 1 : 0));
        const int gapNew = op.newLine - (prev.newLine + (prev.op == '+' ? 1 : 0));
        const int gap = std::min(gapOld, gapNew);
        if (gap <= 2 * kContext) {
            last.push_back(op);
        } else {
            groups.push_back({op});
        }
    }

    QJsonArray result;
    for (const auto &group : groups) {
        const Op &first = group.front();
        int lastOld = first.oldLine, lastNew = first.newLine;
        for (const Op &op : group) {
            lastOld = std::max(lastOld, op.oldLine);
            lastNew = std::max(lastNew, op.newLine);
        }
        const int ctxStartOld = std::max(0, first.oldLine - kContext);
        const int ctxStartNew = std::max(0, first.newLine - kContext);
        const int ctxEndOld = std::min(n - 1, lastOld + kContext);
        const int ctxEndNew = std::min(m - 1, lastNew + kContext);

        QJsonArray lines;
        int oi = ctxStartOld, ni = ctxStartNew;
        // 前置 context + 变化（old/new 侧分别推进；oldLines/newLines 已是中段子表）
        for (const Op &op : group) {
            while (oi < op.oldLine && oi < n) {
                lines.append(QLatin1Char(' ') + oldLines[oi]);
                ++oi;
            }
            while (ni < op.newLine && ni < m) {
                lines.append(QLatin1Char(' ') + newLines[ni]);
                ++ni;
            }
            if (op.op == '-') {
                lines.append(QLatin1Char('-') + oldLines[op.oldLine]);
                ++oi;
            } else {
                lines.append(QLatin1Char('+') + newLines[op.newLine]);
                ++ni;
            }
        }
        // 后置 context：两侧各自推进
        while (oi < n && oi <= ctxEndOld) {
            lines.append(QLatin1Char(' ') + oldLines[oi]);
            ++oi;
        }
        while (ni < m && ni <= ctxEndNew) {
            lines.append(QLatin1Char(' ') + newLines[ni]);
            ++ni;
        }

        // 块头：oldLines/newLines = hunk 覆盖的旧/新侧总行数（context 计入两侧）；
        // '-'/' ' 计入旧侧，'+'/' ' 计入新侧
        int oldCount = 0, newCount = 0;
        for (const QJsonValue &lv : lines) {
            const QString s = lv.toString();
            if (s.startsWith(QLatin1Char('+')))
                ++newCount;
            else {
                ++oldCount;
                if (!s.startsWith(QLatin1Char('-')))
                    ++newCount;
            }
        }
        result.append(QJsonObject{
            {QStringLiteral("oldStart"), prefixLen + ctxStartOld + 1},
            {QStringLiteral("oldLines"), oldCount},
            {QStringLiteral("newStart"), prefixLen + ctxStartNew + 1},
            {QStringLiteral("newLines"), newCount},
            {QStringLiteral("lines"), lines},
        });
    }
    return result;
}

} // namespace

inline QJsonArray buildStructuredPatch(const QString &oldContent, const QString &newContent)
{
    if (oldContent == newContent)
        return {};

    // 内部先归一行尾（CRLF 不污染 diff；自包含不依赖调用方）
    const QString oldNorm = FileLineEnding::normalizeToLf(oldContent.toUtf8());
    const QString newNorm = FileLineEnding::normalizeToLf(newContent.toUtf8());
    if (oldNorm == newNorm)
        return {};

    const QStringList oldLines = oldNorm.split(QLatin1Char('\n'));
    const QStringList newLines = newNorm.split(QLatin1Char('\n'));

    // 大文件保护：总行数超 20 万 → 直接整文件单块（块头真实）
    if (oldLines.size() + newLines.size() > 200000)
        return {wholeBlock(oldLines, newLines, 1, 1)};

    const TrimmedRange t = trimCommonEdges(oldLines, newLines);
    const int n = oldLines.size() - t.prefixLen - t.suffixLen;
    const int m = newLines.size() - t.prefixLen - t.suffixLen;
    if (n <= 0 && m <= 0)
        return {};

    const QStringList subOld = oldLines.mid(t.prefixLen, n);
    const QStringList subNew = newLines.mid(t.prefixLen, m);

    bool budgetExceeded = false;
    const std::vector<char> script = lcsEditScript(subOld, subNew, n, m, budgetExceeded);
    if (budgetExceeded)
        return {wholeBlock(subOld, subNew, t.prefixLen + 1, t.prefixLen + 1)};

    return hunksFromScript(subOld, subNew, script, t.prefixLen, n, m);
}
