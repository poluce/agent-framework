#pragma once

#include <QString>
#include <QStringList>

/**
 * @brief 行号区段编辑（1-based 全局行号），EditTool 与 MultiEditTool 共用。
 *
 * - 行号 = 文件全局行号（与 read_file 的 offset 无关），从 1 开始
 * - 尾部空串（"a\nb\nc\n" split 后 ["a","b","c",""]）不占行号（内容行 N=3）
 * - insert: 在第 startLine 行之后插入 newString（startLine ∈ [1, N]；startLine=0 非法）
 * - replace: [startLine, endLine] 闭区间替换为 newString（endLine 缺省 = startLine）
 * - delete: 删除 [startLine, endLine] 闭区间（newString 忽略）
 * - 空文件 insert = 直接建内容；其余报「文件为空」
 * - 越界 → 明确中文错误（含合法范围）
 */
namespace LineRangeEdit {

struct Result
{
    bool ok = false;
    QString content;
    QString error;
};

inline Result apply(const QString &content, const QString &mode,
                    int startLine, int endLine, const QString &newString)
{
    QStringList lines = content.split(QLatin1Char('\n'));
    // 尾部空串 = 文件末尾换行标记，不占行号；内容行数 = 去掉该标记后的行数
    const bool trailingNewline = !lines.isEmpty() && lines.last().isEmpty();
    const int contentLines = lines.size() - (trailingNewline ? 1 : 0);
    if (contentLines == 0) {
        if (mode == QStringLiteral("insert") && !trailingNewline) {
            return {true, newString, {}}; // 空文件 insert = 直接建内容
        }
        return {false, {}, QStringLiteral("文件为空，无法按行号编辑")};
    }

    if (startLine < 1 || startLine > contentLines) {
        return {false, {},
                QStringLiteral("startLine %1 超出文件行数范围 1..%2").arg(startLine).arg(contentLines)};
    }

    auto insertLinesAt = [&](int index, const QStringList &toInsert) {
        // QList 无 insert(int, QStringList)，逆序逐个插入保持顺序
        for (int i = toInsert.size() - 1; i >= 0; --i)
            lines.insert(index, toInsert.at(i));
    };

    if (mode == QStringLiteral("insert")) {
        // 在第 startLine 行之后插入（startLine ∈ [1, N]）
        insertLinesAt(startLine, newString.split(QLatin1Char('\n')));
        return {true, lines.join(QLatin1Char('\n')), {}};
    }

    const int e = endLine <= 0 ? startLine : endLine;
    if (e < startLine || e > contentLines) {
        return {false, {},
                QStringLiteral("endLine %1 超出文件行数范围 %2..%3").arg(e).arg(startLine).arg(contentLines)};
    }

    // replace / delete 都先删除 [startLine, endLine]，replace 再原位插入新内容
    lines.erase(lines.cbegin() + (startLine - 1), lines.cbegin() + e);
    if (mode == QStringLiteral("replace")) {
        insertLinesAt(startLine - 1, newString.split(QLatin1Char('\n')));
    } else if (mode != QStringLiteral("delete")) {
        return {false, {}, QStringLiteral("未知 editMode: %1").arg(mode)};
    }
    return {true, lines.join(QLatin1Char('\n')), {}};
}

} // namespace LineRangeEdit
