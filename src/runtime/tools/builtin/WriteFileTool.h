#pragma once

#include "tools/BuiltinToolRuntime.h"
#include "helpers/WorkspaceHelper.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#include "tools/AbstractBuiltinTool.h"
#include "helpers/PatchUtils.h"
#include "helpers/WriteGuardHelper.h"

class WriteFileTool : public AbstractBuiltinTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    [[nodiscard]] QString writeTargetPathForInput(const QJsonObject &input) const override;
    [[nodiscard]] QString progressKind() const override { return QStringLiteral("writing"); }

    ToolResult execute(const ToolCall &call,
                       const QString &workspaceRoot,
                       const QString &workingDirectory,
                       const QVariantMap &threadSafeContext) override;
};


inline ToolSpec WriteFileTool::spec() const
{
    return ToolSpecBuilder("write_file", QStringLiteral("创建或完整重写工作目录中的文本文件。"), ToolPermissionKind::Write)
        .requiredInput("filePath", "string", QStringLiteral("目标文件路径，必须是包含具体文件名与后缀的绝对或相对路径（例如 'report.md'），严禁传入目录路径如 '.'"))
        .requiredInput("content", "string", QStringLiteral("完整文件内容"))
        .input("append", "boolean", QStringLiteral("若为 true 且文件已存在，则将 content 直接追加到文件末尾（不自动插入换行，如需新行请在 content 前加 \\n）；文件不存在时报错；append 不需要先读文件"))
        .input("dryRun", "boolean", QStringLiteral("若为 true 则不落盘，仅返回将产生的 structuredPatch 预览（仍做先读校验）"))
        .output("kind", "string", "create/update")
        .output("filePath", "string", QStringLiteral("文件路径"))
        .output("structuredPatch", "array", QStringLiteral("结构化补丁"))
        .build();
}

inline QString WriteFileTool::writeTargetPathForInput(const QJsonObject &input) const
{
    return input.value(QStringLiteral("filePath")).toString();
}

inline ToolResult WriteFileTool::execute(const ToolCall &call,
                                  const QString &workspaceRoot,
                                  const QString &workingDirectory,
                                  const QVariantMap &threadSafeContext)
{
    Q_UNUSED(threadSafeContext);
    const QString pathValue = call.input.value(QStringLiteral("filePath")).toString();

    QString errorMessage;
    const QString filePath = BuiltinToolRuntime::resolveWorkspacePath(workingDirectory, pathValue, &errorMessage);
    if (filePath.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, errorMessage);
    }

    const QString content = call.input.value(QStringLiteral("content")).toString();
    const bool append = call.input.value(QStringLiteral("append")).toBool();
    QFileInfo info(filePath);
    const bool existedBeforeWrite = info.exists();

    // 目录校验 + 大小上限
    if (const ToolResult guard = WriteGuardHelper::validateNotDirectory(filePath, call); !guard.text.isEmpty()) {
        return guard;
    }
    if (const ToolResult guard = WriteGuardHelper::validateContentSize(content, call); !guard.text.isEmpty()) {
        return guard;
    }

    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath())) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("无法创建父目录: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, info.dir().absolutePath())));
    }

    // append 与 diff 基准都需要原内容：文件存在时先读（append 豁免的是「先读校验」，不是物理读取）
    QString originalContent;
    if (existedBeforeWrite) {
        QFile file(filePath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            originalContent = QString::fromUtf8(file.readAll());
        }
    }

    // append：纯拼接，不自动补换行；文件不存在报错
    QString writeContent = content;
    if (append) {
        if (!existedBeforeWrite) {
            return BuiltinToolRuntime::makeErrorResult(
                call, QStringLiteral("append 模式要求文件已存在: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, filePath)));
        }
        writeContent = originalContent + content;
    }

    // dry-run 不落盘，仅返回结构化补丁预览
    if (call.input.value(QStringLiteral("dryRun")).toBool()) {
        const QJsonObject preview{
            {QStringLiteral("dryRun"), true},
            {QStringLiteral("filePath"), WorkspaceHelper::relativeFilePathForResult(workspaceRoot, filePath)},
            {QStringLiteral("structuredPatch"), buildStructuredPatch(originalContent, writeContent)},
        };
        return BuiltinToolRuntime::makeSuccessResult(
            call, QStringLiteral("dry-run 预览（未写入）: %1").arg(QDir::toNativeSeparators(filePath)),
            QStringLiteral("writeResult"), preview);
    }

    const auto writeStatus = WorkspaceHelper::writeTextFileAtomically(filePath, writeContent);
    if (writeStatus == WorkspaceHelper::AtomicWriteStatus::OpenFailed) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("无法写入文件: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, filePath)));
    }
    if (writeStatus == WorkspaceHelper::AtomicWriteStatus::WriteFailed) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("文件写入不完整: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, filePath)));
    }

    // 写后乐观冲突校验：防止并发窗口内被其他代理覆盖（WriteCoordinator 锁内）
    if (existedBeforeWrite && !WorkspaceHelper::fileContentMatches(filePath, writeContent)) {
        return BuiltinToolRuntime::makeErrorResult(
            call, QStringLiteral("文件在写入期间被其他代理修改（内容不一致）。请重新读取文件后再写入。"));
    }

    QJsonObject payloadObject;
    payloadObject.insert(QStringLiteral("kind"), existedBeforeWrite ? QStringLiteral("update") : QStringLiteral("create"));
    payloadObject.insert(QStringLiteral("filePath"), WorkspaceHelper::relativeFilePathForResult(workspaceRoot, filePath));
    payloadObject.insert(QStringLiteral("structuredPatch"), buildStructuredPatch(originalContent, writeContent));

    return BuiltinToolRuntime::makeSuccessResult(call,
                                           QStringLiteral("已写入 %1").arg(QDir::toNativeSeparators(filePath)),
                                           QStringLiteral("writeResult"), payloadObject);
}
