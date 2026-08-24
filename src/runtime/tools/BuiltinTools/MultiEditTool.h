#pragma once

#include "tools/BuiltinToolRuntime.h"
#include "tools/AbstractBuiltinTool.h"
#include "helpers/WorkspaceHelper.h"
#include "helpers/PatchUtils.h"
#include "helpers/ContentEditHelper.h"
#include "helpers/WriteGuardHelper.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

/**
 * @brief multi_edit：一次调用编辑多个文件的多个位置。
 *
 * - edits 数组按顺序应用；**同文件多项链式**（第 N 项基于第 N-1 项结果）
 * - **同文件某项失败 → 跳过该文件后续所有项**；不同文件失败不中断（部分成功）
 * - 每项支持 oldString 字符串替换（可带 useRegex/fuzzy）或行号区段（editMode/startLine/endLine）
 * - 部分失败 → success=false + text 摘要（防模型误判全成功）
 * - 多键锁：调度层 tryAcquireAll 全部成功才执行（见 BuiltinToolRuntime::executeImmediate）
 */
class MultiEditTool : public AbstractBuiltinTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    [[nodiscard]] QString writeTargetPathForInput(const QJsonObject &input) const override;
    [[nodiscard]] QStringList writeTargetPathsForInput(const QJsonObject &input) const override;
    [[nodiscard]] QString progressKind() const override { return QStringLiteral("applying_patch"); }
    [[nodiscard]] QString summarizeCall(const ToolCall &call) const override;

    ToolResult execute(const ToolCall &call,
                       const QString &workspaceRoot,
                       const QString &workingDirectory,
                       const QVariantMap &threadSafeContext) override;
};

inline ToolSpec MultiEditTool::spec() const
{
    return ToolSpecBuilder("multi_edit", QStringLiteral("一次调用编辑多个文件的多个位置。每项支持 oldString 字符串替换或行号区段编辑（editMode/startLine/endLine）。同文件多项链式应用；任一文件失败不影响其他文件（部分成功，success=false + results 明细）。"), ToolPermissionKind::Write)
        .requiredInput("edits", "jsonArray", QStringLiteral("编辑项数组；每项: {filePath(必), oldString?, newString?, replaceAll?, editMode?, startLine?, endLine?, useRegex?, fuzzy?}；oldString 与 startLine 二选一"))
        .output("results", "jsonArray", QStringLiteral("每文件结果: {filePath, success, error?, structuredPatch?}"))
        .build();
}

inline QString MultiEditTool::writeTargetPathForInput(const QJsonObject &input) const
{
    const QJsonArray edits = input.value(QStringLiteral("edits")).toArray();
    return edits.isEmpty() ? QString{}
                           : edits.first().toObject().value(QStringLiteral("filePath")).toString();
}

inline QStringList MultiEditTool::writeTargetPathsForInput(const QJsonObject &input) const
{
    // 去重：同文件多项只占一把锁（重复 tryAcquire 会自锁失败）
    QStringList paths;
    QSet<QString> seen;
    const QJsonArray edits = input.value(QStringLiteral("edits")).toArray();
    for (const QJsonValue &v : edits) {
        const QString p = v.toObject().value(QStringLiteral("filePath")).toString().trimmed();
        if (p.isEmpty() || seen.contains(p))
            continue;
        seen.insert(p);
        paths.append(p);
    }
    return paths;
}

inline QString MultiEditTool::summarizeCall(const ToolCall &call) const
{
    const QJsonArray edits = call.input.value(QStringLiteral("edits")).toArray();
    return QStringLiteral("multi_edit %1 files").arg(edits.size());
}

inline ToolResult MultiEditTool::execute(const ToolCall &call,
                                         const QString &workspaceRoot,
                                         const QString &workingDirectory,
                                         const QVariantMap &threadSafeContext)
{
    Q_UNUSED(threadSafeContext);
    Q_UNUSED(workspaceRoot);
    const QJsonArray edits = call.input.value(QStringLiteral("edits")).toArray();
    if (edits.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("edits 不能为空。"));
    }

    // 同文件链式缓存：读一次后逐项应用
    QHash<QString, QString> chainedContent;  // normalized absPath -> 当前内容
    QHash<QString, QString> chainedOriginal; // normalized absPath -> 原始内容（diff 基准）
    QSet<QString> failedFiles;

    QJsonArray results;
    int okCount = 0;
    QString firstError;

    auto appendFailure = [&](const QString &rawPath, const QString &error,
                             const QString &normKey = {}) {
        results.append(QJsonObject{
            {QStringLiteral("filePath"), rawPath},
            {QStringLiteral("success"), false},
            {QStringLiteral("error"), error},
        });
        if (firstError.isEmpty())
            firstError = error;
        if (!normKey.isEmpty())
            failedFiles.insert(normKey);
    };

    auto appendSkipped = [&](const QString &rawPath) {
        appendFailure(rawPath, QStringLiteral("同文件前序编辑失败，已跳过。"));
    };

    for (const QJsonValue &editValue : edits) {
        const QJsonObject edit = editValue.toObject();
        const QString rawPath = edit.value(QStringLiteral("filePath")).toString().trimmed();

        QString resolveErr;
        const QString absPath = BuiltinToolRuntime::resolveWorkspacePath(workingDirectory, rawPath, &resolveErr);
        if (absPath.isEmpty()) {
            appendFailure(rawPath, resolveErr);
            continue;
        }

        const QString normKey = WorkspaceHelper::normalizedPath(absPath);
        if (failedFiles.contains(normKey)) {
            appendSkipped(rawPath);
            continue;
        }

        if (const ToolResult guard = WriteGuardHelper::validateNotDirectory(absPath, call); !guard.text.isEmpty()) {
            appendFailure(rawPath, guard.text, normKey);
            continue;
        }

        // 链式读取：首次读盘，后续用内存累积
        QString content;
        QString originalContent;
        if (chainedContent.contains(normKey)) {
            content = chainedContent.value(normKey);
            originalContent = chainedOriginal.value(normKey);
        } else {
            QFileInfo info(absPath);
            if (!info.exists()) {
                // multi_edit 无「创建」语义
                appendFailure(rawPath, QStringLiteral("File does not exist: %1").arg(rawPath), normKey);
                continue;
            }
            if (const ToolResult guard = WriteGuardHelper::validateReadSize(absPath, call); !guard.text.isEmpty()) {
                appendFailure(rawPath, guard.text, normKey);
                continue;
            }
            QFile f(absPath);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                appendFailure(rawPath, QStringLiteral("无法读取文件: %1").arg(rawPath), normKey);
                continue;
            }
            content = QString::fromUtf8(f.readAll());
            originalContent = content;
        }

        if (const QString mutexErr = ContentEditHelper::mutualExclusionError(edit); !mutexErr.isEmpty()) {
            appendFailure(rawPath, mutexErr, normKey);
            continue;
        }
        const ContentEditHelper::Result applied = ContentEditHelper::applyFromObject(content, edit);
        if (!applied.ok) {
            appendFailure(rawPath, applied.error, normKey);
            continue;
        }
        content = applied.content;

        const auto writeStatus = WorkspaceHelper::writeTextFileAtomically(absPath, content);
        if (writeStatus == WorkspaceHelper::AtomicWriteStatus::OpenFailed) {
            appendFailure(rawPath, QStringLiteral("无法写入文件: %1").arg(rawPath), normKey);
            continue;
        }
        if (writeStatus == WorkspaceHelper::AtomicWriteStatus::WriteFailed) {
            appendFailure(rawPath, QStringLiteral("文件写入不完整: %1").arg(rawPath), normKey);
            continue;
        }
        if (!WorkspaceHelper::fileContentMatches(absPath, content)) {
            appendFailure(rawPath, QStringLiteral("文件在写入期间被其他代理修改（内容不一致）"), normKey);
            continue;
        }

        chainedContent.insert(normKey, content);
        chainedOriginal.insert(normKey, originalContent);
        ++okCount;
        results.append(QJsonObject{
            {QStringLiteral("filePath"), rawPath},
            {QStringLiteral("success"), true},
            {QStringLiteral("structuredPatch"), buildStructuredPatch(originalContent, content)},
        });
    }

    if (okCount == edits.size()) {
        return BuiltinToolRuntime::makeSuccessResult(
            call, QStringLiteral("已编辑 %1 个文件。").arg(okCount),
            QStringLiteral("multiEditResult"),
            QJsonObject{{QStringLiteral("results"), results}});
    }
    return BuiltinToolRuntime::makeErrorResult(
        call, QStringLiteral("%1 个文件成功，%2 个失败：%3")
                  .arg(okCount)
                  .arg(edits.size() - okCount)
                  .arg(firstError));
}
