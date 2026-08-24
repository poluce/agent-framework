#pragma once

#include "tools/BuiltinToolRuntime.h"
#include "helpers/WorkspaceFileIterator.h"
#include "helpers/WorkspaceHelper.h"
#include "helpers/FileTypeFilter.h"
#include "helpers/WriteGuardHelper.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

#include "tools/AbstractBuiltinTool.h"

class GrepTool : public AbstractBuiltinTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    [[nodiscard]] QString progressKind() const override { return QStringLiteral("searching"); }

    ToolResult execute(const ToolCall &call,
                       const QString &workspaceRoot,
                       const QString &workingDirectory,
                       const QVariantMap &threadSafeContext) override;
};

inline ToolSpec GrepTool::spec() const
{
    return ToolSpecBuilder("grep", QStringLiteral("在工作目录内搜索文本模式。"))
        .requiredInput("pattern", "string", QStringLiteral("要匹配的正则或普通文本（与 patterns 互斥，二选一）"))
        .input("patterns", "stringArray", QStringLiteral("多个正则/文本模式（与 pattern 互斥，任一匹配即中），可为空"))
        .input("rootPath", "string", QStringLiteral("搜索起始路径，可为空"))
        .input("glob", "string", QStringLiteral("文件过滤 glob，可为空"))
        .input("excludeGlob", "string", QStringLiteral("排除匹配此 glob 的文件，可为空"))
        .input("type", "string", FileTypeFilter::typeFilterDescription())
        .input("mode", "string", "content/filesWithMatches/count，可为空")
        .input("ignoreCase", "boolean", QStringLiteral("是否忽略大小写"))
        .input("before", "integer", QStringLiteral("每个匹配前显示的上下文行数"))
        .input("after", "integer", QStringLiteral("每个匹配后显示的上下文行数"))
        .input("context", "integer", QStringLiteral("前后统一上下文行数"))
        .input("showLineNumbers", "boolean", QStringLiteral("是否显示行号"))
        .input("headLimit", "integer", QStringLiteral("最多返回多少条结果，可为空"))
        .input("offset", "integer", QStringLiteral("跳过前多少条结果，可为空"))
        .input("multiline", "boolean", QStringLiteral("是否启用多行匹配"))
        .output("mode", "string", "content/filesWithMatches/count")
        .output("matches", "array", QStringLiteral("匹配结果列表"))
        .output("truncated", "boolean", QStringLiteral("结果是否截断"))
        .output("totalMatches", "integer", QStringLiteral("匹配总数（全量统计，截断不影响该值）"))
        .output("skippedFiles", "stringArray", QStringLiteral("因文件过大（>1MB）跳过搜索的文件路径列表"))
        .build();
}

namespace {

/** 文件级匹配判定：任一 pattern 命中即中；可选收集命中行（含上下文）与总匹配数。 */
inline bool fileMatches(const QString &content, const QStringList &lines,
                        const QList<QRegularExpression> &regexes,
                        const QStringList &plainTexts, bool ignoreCase,
                        bool multiline, int contextBefore, int contextAfter,
                        QSet<int> *includedLines, int *matchCount)
{
    int total = 0;
    const int lineCount = lines.size();
    for (int i = 0; i < regexes.size(); ++i) {
        const QRegularExpression &regex = regexes.at(i);
        int entryCount = 0;
        if (multiline && regex.isValid()) {
            // 多行模式：按匹配起止偏移换算行号，并带上下文收集命中行
            const auto lineNumberForOffset = [&content](const int charOffset) {
                int lineNumber = 1;
                for (int j = 0; j < charOffset && j < content.size(); ++j) {
                    if (content.at(j) == QLatin1Char('\n'))
                        ++lineNumber;
                }
                return lineNumber;
            };
            QRegularExpressionMatchIterator it = regex.globalMatch(content);
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                ++entryCount;
                if (includedLines) {
                    const int startLine = lineNumberForOffset(match.capturedStart());
                    const int endLine = lineNumberForOffset(qMax(match.capturedEnd() - 1, match.capturedStart()));
                    for (int lineIndex = qMax(1, startLine - contextBefore);
                         lineIndex <= qMin(lineCount, endLine + contextAfter);
                         ++lineIndex) {
                        includedLines->insert(lineIndex);
                    }
                }
            }
        } else {
            // 单行模式：逐行匹配（仅非 multiline 时编译失败可退化为 contains）
            const QString &plain = plainTexts.at(i);
            const auto matchesLine = [&](const QString &line) {
                if (regex.isValid())
                    return regex.match(line).hasMatch();
                return !multiline && line.contains(plain, ignoreCase ? Qt::CaseInsensitive
                                                                     : Qt::CaseSensitive);
            };
            for (int lineNumber = 1; lineNumber <= lineCount; ++lineNumber) {
                if (!matchesLine(lines.at(lineNumber - 1)))
                    continue;
                ++entryCount;
                if (includedLines) {
                    for (int contextLine = qMax(1, lineNumber - contextBefore);
                         contextLine <= qMin(lineCount, lineNumber + contextAfter);
                         ++contextLine) {
                        includedLines->insert(contextLine);
                    }
                }
            }
        }
        total += entryCount;
    }
    if (matchCount)
        *matchCount = total;
    return total > 0;
}

} // namespace

inline ToolResult GrepTool::execute(const ToolCall &call,
                             const QString &workspaceRoot,
                             const QString &workingDirectory,
                             const QVariantMap &threadSafeContext)
{
    Q_UNUSED(threadSafeContext);

    QString errorMessage;
    const QString patternText = call.input.value(QStringLiteral("pattern")).toString();
    const QString rootPath = BuiltinToolRuntime::resolveWorkspacePath(workingDirectory,
                                                                call.input.value(QStringLiteral("rootPath")).toString(),
                                                                &errorMessage);
    if (rootPath.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, errorMessage);
    }

    const QString globFilter = call.input.value(QStringLiteral("glob")).toString().trimmed();
    const QString excludeGlob = call.input.value(QStringLiteral("excludeGlob")).toString().trimmed();
    const QString outputMode = call.input.value(QStringLiteral("mode")).toString(QStringLiteral("filesWithMatches")).trimmed();
    const QString typeFilter = call.input.value(QStringLiteral("type")).toString().trimmed();
    const int headLimit = qMax(0, call.input.value(QStringLiteral("headLimit")).toInt(250));
    const int offset = qMax(0, call.input.value(QStringLiteral("offset")).toInt(0));
    const bool multiline = call.input.value(QStringLiteral("multiline")).toBool(false);
    const bool ignoreCase = call.input.value(QStringLiteral("ignoreCase")).toBool(false);
    const bool showLineNumbers = !call.input.contains(QStringLiteral("showLineNumbers"))
                                 || call.input.value(QStringLiteral("showLineNumbers")).toBool(true);
    int contextBefore = qMax(0, call.input.value(QStringLiteral("before")).toInt(0));
    int contextAfter = qMax(0, call.input.value(QStringLiteral("after")).toInt(0));
    const int sharedContext = call.input.value(QStringLiteral("context")).toInt(-1);
    if (sharedContext >= 0) {
        contextBefore = sharedContext;
        contextAfter = sharedContext;
    }

    // 收集有效 pattern（pattern 与 patterns 互斥；全部为空则报错）
    const QJsonArray patternsArr = call.input.value(QStringLiteral("patterns")).toArray();
    const bool hasPatterns = !patternsArr.isEmpty();
    if (hasPatterns && !patternText.trimmed().isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("pattern 与 patterns 互斥，请只传一个。"));
    }
    QStringList activePatterns;
    if (hasPatterns) {
        for (const QJsonValue &v : patternsArr) {
            const QString p = v.toString().trimmed();
            if (!p.isEmpty())
                activePatterns.append(p);
        }
    } else if (!patternText.trimmed().isEmpty()) {
        activePatterns.append(patternText.trimmed());
    }
    if (activePatterns.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("pattern 不能为空。"));
    }

    // 每个 pattern 独立编译；编译失败且非 multiline → 该 pattern 用 contains 匹配
    const auto caseOption = ignoreCase ? QRegularExpression::CaseInsensitiveOption
                                       : QRegularExpression::NoPatternOption;
    const auto multiOption = multiline ? QRegularExpression::DotMatchesEverythingOption
                                       : QRegularExpression::NoPatternOption;
    QList<QRegularExpression> regexes;
    QStringList plainTexts;
    regexes.reserve(activePatterns.size());
    plainTexts.reserve(activePatterns.size());
    for (const QString &p : activePatterns) {
        const QRegularExpression regex(p, caseOption | multiOption);
        regexes.append(regex);
        plainTexts.append(regex.isValid() ? QString() : p);
    }
    const QRegularExpression globRegex = globFilter.isEmpty()
                                             ? QRegularExpression()
                                             : QRegularExpression::fromWildcard(globFilter,
                                                                                Qt::CaseInsensitive,
                                                                                QRegularExpression::UnanchoredWildcardConversion);
    const QRegularExpression excludeRegex = excludeGlob.isEmpty()
                                                ? QRegularExpression()
                                                : QRegularExpression::fromWildcard(excludeGlob,
                                                                                   Qt::CaseInsensitive,
                                                                                   QRegularExpression::UnanchoredWildcardConversion);
    // excludeGlob 的 `**/` 前缀变体（根目录文件无 `/`，fromWildcard 的 `**` 要求含 `/`）
    const QRegularExpression excludeRegexRoot = (!excludeGlob.isEmpty() && excludeGlob.startsWith(QStringLiteral("**/")))
                                                    ? QRegularExpression::fromWildcard(excludeGlob.mid(3),
                                                                                       Qt::CaseInsensitive,
                                                                                       QRegularExpression::UnanchoredWildcardConversion)
                                                    : QRegularExpression();
    QStringList matches;
    QStringList skippedFiles;
    int totalMatches = 0;

    WorkspaceFileIterator fileIterator(workspaceRoot, rootPath);
    fileIterator.forEach([&](const WorkspaceFileIterator::Entry &entry) -> bool {
        if (!FileTypeFilter::matches(entry.absolutePath, typeFilter))
            return true;

        const QString relPath = QDir::fromNativeSeparators(entry.relativePath);
        // glob 过滤：相对路径或 basename 命中即通过
        if (!globFilter.isEmpty()
            && !globRegex.match(relPath).hasMatch()
            && !globRegex.match(QFileInfo(entry.relativePath).fileName()).hasMatch()) {
            return true;
        }
        // excludeGlob：匹配（相对路径或 basename，含根目录变体）则跳过
        if (!excludeGlob.isEmpty()
            && (excludeRegex.match(relPath).hasMatch()
                || excludeRegex.match(QFileInfo(entry.relativePath).fileName()).hasMatch()
                || (!excludeRegexRoot.pattern().isEmpty() && excludeRegexRoot.match(relPath).hasMatch()))) {
            return true;
        }

        QFile file(entry.absolutePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return true;

        // 1MB 上限：超限跳过并记录（不中断搜索）；二进制文件静默跳过
        if (file.size() > WriteGuardHelper::kMaxReadBytes) {
            skippedFiles.append(relPath);
            return true;
        }
        const QByteArray raw = file.readAll();
        if (WriteGuardHelper::isBinary(raw))
            return true;

        const QString content = QString::fromUtf8(raw);
        const QStringList lines = content.split(QLatin1Char('\n'));
        int matchCount = 0;

        if (outputMode == QStringLiteral("content")) {
            QSet<int> includedLines;
            if (fileMatches(content, lines, regexes, plainTexts, ignoreCase, multiline,
                            contextBefore, contextAfter, &includedLines, &matchCount)
                && matchCount > 0) {
                totalMatches += matchCount;
                QList<int> orderedLines = includedLines.values();
                std::sort(orderedLines.begin(), orderedLines.end());
                for (const int lineNumber : orderedLines) {
                    const QString prefix = showLineNumbers
                                               ? QStringLiteral("%1:%2: ").arg(entry.relativePath).arg(lineNumber)
                                               : QStringLiteral("%1: ").arg(entry.relativePath);
                    matches.append(prefix + lines.at(lineNumber - 1).trimmed());
                }
            }
        } else if (outputMode == QStringLiteral("count")) {
            if (fileMatches(content, lines, regexes, plainTexts, ignoreCase, multiline,
                            contextBefore, contextAfter, nullptr, &matchCount)
                && matchCount > 0) {
                totalMatches += matchCount;
                matches.append(QStringLiteral("%1:%2").arg(entry.relativePath).arg(matchCount));
            }
        } else if (fileMatches(content, lines, regexes, plainTexts, ignoreCase, multiline,
                               contextBefore, contextAfter, nullptr, &matchCount)) {
            ++totalMatches;
            matches.append(entry.relativePath);
        }
        return true;
    });

    QStringList finalMatches = matches;
    if (offset > 0 && offset < finalMatches.size()) {
        finalMatches = finalMatches.mid(offset);
    } else if (offset >= finalMatches.size()) {
        finalMatches.clear();
    }

    int appliedLimit = 0;
    if (headLimit > 0 && finalMatches.size() > headLimit) {
        finalMatches = finalMatches.mid(0, headLimit);
        appliedLimit = headLimit;
    }

    QJsonArray matchesPayload;
    if (outputMode == QStringLiteral("content")) {
        for (const QString &line : finalMatches) {
            const int firstColon = line.indexOf(QLatin1Char(':'));
            const int secondColon = firstColon >= 0 ? line.indexOf(QLatin1Char(':'), firstColon + 1) : -1;
            QString filePath;
            int lineNumber = 0;
            QString text = line;
            if (firstColon > 0) {
                filePath = line.left(firstColon);
                if (showLineNumbers && secondColon > firstColon) {
                    lineNumber = line.mid(firstColon + 1, secondColon - firstColon - 1).trimmed().toInt();
                    text = line.mid(secondColon + 1).trimmed();
                } else {
                    text = line.mid(firstColon + 1).trimmed();
                }
            }
            QJsonObject entry{
                {QStringLiteral("filePath"), filePath},
                {QStringLiteral("text"), text},
            };
            if (lineNumber > 0) {
                entry.insert(QStringLiteral("lineNumber"), lineNumber);
            }
            matchesPayload.append(entry);
        }
    } else if (outputMode == QStringLiteral("count")) {
        for (const QString &line : finalMatches) {
            const int lastColon = line.lastIndexOf(QLatin1Char(':'));
            if (lastColon <= 0)
                continue;
            matchesPayload.append(QJsonObject{
                {QStringLiteral("filePath"), line.left(lastColon)},
                {QStringLiteral("count"), line.mid(lastColon + 1).toInt()},
            });
        }
    } else {
        for (const QString &line : finalMatches) {
            matchesPayload.append(QJsonObject{
                {QStringLiteral("filePath"), line},
            });
        }
    }

    QJsonObject payload{
        {QStringLiteral("mode"), outputMode},
        {QStringLiteral("matches"), matchesPayload},
        {QStringLiteral("truncated"), appliedLimit > 0},
        {QStringLiteral("totalMatches"), totalMatches},
        {QStringLiteral("skippedFiles"), QJsonArray::fromStringList(skippedFiles)},
    };

    return BuiltinToolRuntime::makeSuccessResult(call, finalMatches.join(QLatin1Char('\n')),
                                           QStringLiteral("grepResult"), payload);
}
