#pragma once

#include "tools/BuiltinToolRuntime.h"
#include "helpers/WorkspaceFileIterator.h"
#include "helpers/WorkspaceHelper.h"
#include "helpers/FileTypeFilter.h"
#include "helpers/GlobBraceExpansion.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <vector>

#include "tools/AbstractBuiltinTool.h"

class GlobTool : public AbstractBuiltinTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    [[nodiscard]] QString progressKind() const override { return QStringLiteral("searching"); }

    ToolResult execute(const ToolCall &call,
                       const QString &workspaceRoot,
                       const QString &workingDirectory,
                       const QVariantMap &threadSafeContext) override;
};

inline ToolSpec GlobTool::spec() const
{
    return ToolSpecBuilder("glob", QStringLiteral("按 glob 模式查找工作目录中的文件。"))
        .requiredInput("pattern", "string", QStringLiteral("glob 模式，例如 **/*.cpp；支持 {a,b} 花括号多分支"))
        .input("rootPath", "string", QStringLiteral("搜索起始目录，可为空"))
        .input("limit", "integer", QStringLiteral("最多返回多少条结果（配合 offset 分页），默认 200，可为空"))
        .input("offset", "integer", QStringLiteral("跳过前多少条结果（分页用），默认 0，可为空"))
        .input("type", "string", FileTypeFilter::typeFilterDescription())
        .input("caseSensitive", "boolean", QStringLiteral("是否区分大小写，默认 false"))
        .input("absolute", "boolean", QStringLiteral("是否返回绝对路径，默认 false 返回相对路径"))
        .output("files", "stringArray", QStringLiteral("匹配到的文件列表"))
        .output("truncated", "boolean", QStringLiteral("结果是否截断"))
        .output("total", "integer", QStringLiteral("匹配总数（未应用 offset/limit 前的全量统计）"))
        .build();
}

inline ToolResult GlobTool::execute(const ToolCall &call,
                             const QString &workspaceRoot,
                             const QString &workingDirectory,
                             const QVariantMap &threadSafeContext)
{
    Q_UNUSED(threadSafeContext);
    const QString patternText = call.input.value(QStringLiteral("pattern")).toString().trimmed();
    const int limit = qMax(1, call.input.value(QStringLiteral("limit")).toInt(200));
    const int offset = qMax(0, call.input.value(QStringLiteral("offset")).toInt(0));
    const bool caseSensitive = call.input.value(QStringLiteral("caseSensitive")).toBool(false);
    const bool absolute = call.input.value(QStringLiteral("absolute")).toBool(false);
    const QString typeFilter = call.input.value(QStringLiteral("type")).toString().trimmed();

    if (patternText.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("pattern 不能为空。"));
    }

    QString errorMessage;
    const QString rootPath = BuiltinToolRuntime::resolveWorkspacePath(workingDirectory,
                                                                call.input.value(QStringLiteral("rootPath")).toString(),
                                                                &errorMessage);
    if (rootPath.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, errorMessage);
    }
    if (!QDir(rootPath).exists()) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("目录不存在: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, rootPath)));
    }

    // brace expansion：顶层 {a,b} 展开为多 pattern
    const QStringList patterns = GlobBraceExpansion::expand(patternText);
    if (patterns.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(
            call, QStringLiteral("brace 展开数量超过上限（%1），请拆分为多次调用。").arg(GlobBraceExpansion::kMaxExpandedPatterns));
    }

    // 编译全部 pattern（含 `**/` 前缀的去前缀变体——fromWildcard 的 `**` 要求含 `/`，
    // 根目录文件相对路径无 `/`，会漏掉 → 额外匹配一次去前缀变体）
    const auto cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QList<QRegularExpression> regexes;
    regexes.reserve(patterns.size() * 2);
    for (const QString &p : patterns) {
        regexes.append(GlobBraceExpansion::toRegex(p, cs));
        if (p.startsWith(QStringLiteral("**/")))
            regexes.append(GlobBraceExpansion::toRegex(p.mid(3), cs));
    }

    // 全量收集（含 absolutePath）→ 排序 → 分页（稳定分页必需）
    struct SortableEntry
    {
        QString relPath;
        QString absPath;
    };
    std::vector<SortableEntry> collected;
    WorkspaceFileIterator iterator(workspaceRoot, rootPath);
    iterator.forEach([&](const WorkspaceFileIterator::Entry &entry) -> bool {
        if (FileTypeFilter::matches(entry.absolutePath, typeFilter)) {
            const QString rel = QDir::fromNativeSeparators(entry.relativePath);
            const auto hit = [&] {
                for (const QRegularExpression &re : regexes) {
                    if (re.match(rel).hasMatch()
                        || re.match(QFileInfo(rel).fileName()).hasMatch()) {
                        return true;
                    }
                }
                return false;
            };
            if (hit())
                collected.push_back({rel, entry.absolutePath});
        }
        // 上限保护：防病态仓库整树扫描
        return collected.size() < 100000;
    });
    std::sort(collected.begin(), collected.end(),
              [](const SortableEntry &a, const SortableEntry &b) { return a.relPath < b.relPath; });

    const int total = static_cast<int>(collected.size());
    QStringList matches;
    const int start = qMin(offset, total);
    const int end = qMin(start + limit, total);
    for (int i = start; i < end; ++i) {
        matches.append(absolute ? collected[static_cast<std::size_t>(i)].absPath
                                : collected[static_cast<std::size_t>(i)].relPath);
    }
    const bool truncated = end < total;

    QJsonObject payload{
        {QStringLiteral("files"), QJsonArray::fromStringList(matches)},
        {QStringLiteral("truncated"), truncated},
        {QStringLiteral("total"), total},
    };
    // 截断提示只进文本不进 files 数组（files 须保持纯匹配列表，供客户端分页判断）
    if (truncated) {
        matches.append(QStringLiteral("...(Results are truncated. Use limit/offset to page through, or a more specific pattern.)"));
    }
    return BuiltinToolRuntime::makeSuccessResult(call, matches.join(QLatin1Char('\n')),
                                           QStringLiteral("globResult"), payload);
}
