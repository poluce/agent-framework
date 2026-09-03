#pragma once

#include "LineRangeEdit.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

/**
 * @brief 单次内容编辑（字符串 / 正则 / fuzzy / 行号），EditTool 与 MultiEditTool 共用。
 *
 * 输入字段与 edit 工具一致：oldString / newString / replaceAll / editMode /
 * startLine / endLine / useRegex / fuzzy。互斥校验在此统一做。
 */
namespace ContentEditHelper {

struct Result
{
    bool ok = false;
    QString content;
    QString error;
};

inline Result applyFromParams(const QString &content,
                              const QString &oldString,
                              const QString &newString,
                              bool replaceAll,
                              const QString &editMode,
                              int startLine,
                              int endLine,
                              bool useRegex,
                              bool fuzzy)
{
    // 互斥：fuzzy 与 useRegex
    if (useRegex && fuzzy) {
        return {false, {}, QStringLiteral("useRegex 与 fuzzy 互斥，请二选一。")};
    }

    // 行号模式（无 oldString）
    if (oldString.isEmpty()) {
        const QString mode = editMode.isEmpty() ? QStringLiteral("replace") : editMode;
        if (mode == QStringLiteral("replace") && newString.isEmpty()) {
            return {false, {}, QStringLiteral("replace 模式需要 newString")};
        }
        const LineRangeEdit::Result r = LineRangeEdit::apply(content, mode, startLine, endLine, newString);
        return r.ok ? Result{true, r.content, {}} : Result{false, {}, r.error};
    }

    if (oldString == newString) {
        return {false, {}, QStringLiteral("No changes to make: old_string and new_string are exactly the same.")};
    }

    if (useRegex) {
        // ReDoS 防护：pattern 长度 + 嵌套量词预检
        if (oldString.size() > 256) {
            return {false, {}, QStringLiteral("正则 pattern 超过 256 字符上限。")};
        }
        if (oldString.contains(QRegularExpression(QStringLiteral("\\([^)]*[+*][^)]*\\)[+*]")))) {
            return {false, {}, QStringLiteral("正则包含嵌套量词（如 (a+)+），可能存在灾难性回溯，已拒绝。")};
        }
        const QRegularExpression re(oldString);
        if (!re.isValid()) {
            return {false, {}, QStringLiteral("正则编译失败: %1").arg(re.errorString())};
        }
        QString next = content;
        if (replaceAll) {
            next.replace(re, newString); // 无反向引用：newString 按字面替换
        } else {
            const QRegularExpressionMatch match = re.match(next);
            if (!match.hasMatch()) {
                return {false, {}, QStringLiteral("old_string (regex) not found in file.")};
            }
            next.replace(match.capturedStart(), match.capturedLength(), newString);
        }
        return {true, next, {}};
    }

    if (fuzzy) {
        // 按行 trim 后匹配连续区段；应用时首行缩进保留目标文件实际缩进
        const QStringList contentLines = content.split(QLatin1Char('\n'));
        const QStringList oldLines = oldString.split(QLatin1Char('\n'));
        const int oldLineCount = oldLines.size();

        auto linesMatchAt = [&](int start) -> bool {
            for (int j = 0; j < oldLineCount; ++j) {
                if (contentLines[start + j].trimmed() != oldLines[j].trimmed())
                    return false;
            }
            return true;
        };

        QList<int> starts;
        for (int i = 0; i + oldLineCount <= contentLines.size(); ++i) {
            if (!linesMatchAt(i))
                continue;
            starts.append(i);
            if (!replaceAll)
                break;
            i += oldLineCount - 1;
        }
        if (starts.isEmpty()) {
            return {false, {}, QStringLiteral("old_string not found in file (fuzzy).")};
        }

        const QStringList toInsertBase = newString.split(QLatin1Char('\n'));
        QStringList resultLines = contentLines;
        for (int idx = starts.size() - 1; idx >= 0; --idx) {
            const int start = starts.at(idx);
            const QString actualIndent = contentLines.at(start).left(
                contentLines.at(start).size() - contentLines.at(start).trimmed().size());
            QStringList toInsert = toInsertBase;
            if (!toInsert.isEmpty())
                toInsert[0] = actualIndent + toInsert.first().trimmed();
            resultLines.erase(resultLines.cbegin() + start,
                              resultLines.cbegin() + start + oldLineCount);
            for (int k = toInsert.size() - 1; k >= 0; --k)
                resultLines.insert(start, toInsert.at(k));
        }
        return {true, resultLines.join(QLatin1Char('\n')), {}};
    }

    // 精确字符串替换
    QString next = content;
    const int occurrences = next.count(oldString);
    if (occurrences == 0) {
        return {false, {}, QStringLiteral("old_string not found in file.")};
    }
    if (replaceAll) {
        next.replace(oldString, newString);
    } else if (occurrences == 1) {
        next.replace(next.indexOf(oldString), oldString.size(), newString);
    } else {
        // 多 occurrence：startLine 消歧，选行号最接近的
        if (startLine <= 0) {
            return {false, {},
                    QStringLiteral("old_string appears multiple times. Set replace_all=true, make the string more specific, or pass startLine to disambiguate.")};
        }
        int best = -1;
        int bestDist = INT_MAX;
        int searchFrom = 0;
        for (int n = 0; n < occurrences; ++n) {
            const int pos = next.indexOf(oldString, searchFrom);
            const int lineNo = next.left(pos).count(QLatin1Char('\n')) + 1;
            const int dist = qAbs(lineNo - startLine);
            if (dist < bestDist) {
                bestDist = dist;
                best = pos;
            }
            searchFrom = pos + oldString.size();
        }
        next.replace(best, oldString.size(), newString);
    }
    return {true, next, {}};
}

/** 从 edit 项 JSON（edit 顶层 input 或 multi_edit.edits[i]）应用一次编辑。 */
inline Result applyFromObject(const QString &content, const QJsonObject &edit)
{
    return applyFromParams(
        content,
        edit.value(QStringLiteral("oldString")).toString(),
        edit.value(QStringLiteral("newString")).toString(),
        edit.value(QStringLiteral("replaceAll")).toBool(),
        edit.value(QStringLiteral("editMode")).toString().trimmed(),
        edit.value(QStringLiteral("startLine")).toInt(),
        edit.value(QStringLiteral("endLine")).toInt(),
        edit.value(QStringLiteral("useRegex")).toBool(),
        edit.value(QStringLiteral("fuzzy")).toBool());
}

/** 顶层互斥：oldString 与行号参数不可同时出现。空串 = 通过。 */
inline QString mutualExclusionError(const QJsonObject &edit)
{
    const QString oldString = edit.value(QStringLiteral("oldString")).toString();
    const QString editMode = edit.value(QStringLiteral("editMode")).toString().trimmed();
    const int startLine = edit.value(QStringLiteral("startLine")).toInt();
    const int endLine = edit.value(QStringLiteral("endLine")).toInt();
    const bool useRegex = edit.value(QStringLiteral("useRegex")).toBool();
    const bool fuzzy = edit.value(QStringLiteral("fuzzy")).toBool();
    const bool lineModeRequested = !editMode.isEmpty() || startLine > 0 || endLine > 0;
    if (!oldString.isEmpty() && lineModeRequested) {
        return QStringLiteral("oldString 与行号参数（editMode/startLine/endLine）互斥，请二选一。");
    }
    if (useRegex && fuzzy) {
        return QStringLiteral("useRegex 与 fuzzy 互斥，请二选一。");
    }
    return {};
}

} // namespace ContentEditHelper
