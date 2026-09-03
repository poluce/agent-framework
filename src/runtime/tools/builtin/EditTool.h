#pragma once

#include "tools/BuiltinToolRuntime.h"
#include "tools/AbstractBuiltinTool.h"
#include "helpers/WorkspaceHelper.h"
#include "helpers/PatchUtils.h"
#include "helpers/ContentEditHelper.h"
#include "helpers/WriteGuardHelper.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>

class EditTool : public AbstractBuiltinTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    [[nodiscard]] QString writeTargetPathForInput(const QJsonObject &input) const override;
    [[nodiscard]] QString progressKind() const override { return QStringLiteral("applying_patch"); }

    ToolResult execute(const ToolCall &call,
                       const QString &workspaceRoot,
                       const QString &workingDirectory,
                       const QVariantMap &threadSafeContext) override;
};

inline ToolSpec EditTool::spec() const
{
    return ToolSpecBuilder("edit", QStringLiteral("在工作目录内编辑文件内容。优先用 oldString 精确定位替换；已知行号时用行号模式（editMode/startLine）；useRegex 用于模式替换；fuzzy 用于缩进/空白差异。"), ToolPermissionKind::Write)
        .requiredInput("filePath", "string", QStringLiteral("目标文件路径，必须包含具体文件名与后缀（例如 'src/main.cpp'），严禁传入目录路径"))
        .input("oldString", "string", QStringLiteral("要替换的原文本（与行号模式互斥，传了则走字符串替换）"))
        .input("newString", "string", QStringLiteral("替换后的新文本"))
        .input("replaceAll", "boolean", QStringLiteral("是否替换所有匹配项"))
        .input("editMode", "string", QStringLiteral("replace/insert/delete（不传 oldString 时生效，默认 replace）；行号模式"))
        .input("startLine", "integer", QStringLiteral("1-based 文件全局起始行号（与 read_file 的 offset 无关；行号模式用，0 非法）"))
        .input("endLine", "integer", QStringLiteral("1-based 结束行号（replace/delete 用，闭区间，缺省 = startLine）"))
        .input("useRegex", "boolean", QStringLiteral("将 oldString 作为正则匹配（pattern 限 256 字符，不支持嵌套量词/回溯密集模式，无反向引用）；与 fuzzy 互斥"))
        .input("fuzzy", "boolean", QStringLiteral("按行尾空白归一后匹配（缩进/空白差异仍命中）；与 useRegex 互斥"))
        .input("dryRun", "boolean", QStringLiteral("若为 true 则不落盘，仅返回将产生的 structuredPatch 预览（仍做先读校验）"))
        .output("filePath", "string", QStringLiteral("文件路径"))
        .output("structuredPatch", "array", QStringLiteral("结构化补丁"))
        .build();
}

inline QString EditTool::writeTargetPathForInput(const QJsonObject &input) const
{
    return input.value(QStringLiteral("filePath")).toString();
}

inline ToolResult EditTool::execute(const ToolCall &call,
                                    const QString &workspaceRoot,
                                    const QString &workingDirectory,
                                    const QVariantMap &threadSafeContext)
{
    Q_UNUSED(threadSafeContext);
    const QString rawFilePath = call.input.value(QStringLiteral("filePath")).toString();

    QString errorMessage;
    const QString filePath = BuiltinToolRuntime::resolveWorkspacePath(workingDirectory, rawFilePath, &errorMessage);
    if (filePath.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, errorMessage);
    }

    // 目录校验 + 读上限（读入前检查文件大小）
    if (const ToolResult guard = WriteGuardHelper::validateNotDirectory(filePath, call); !guard.text.isEmpty()) {
        return guard;
    }
    if (QFileInfo(filePath).exists()) {
        if (const ToolResult guard = WriteGuardHelper::validateReadSize(filePath, call); !guard.text.isEmpty()) {
            return guard;
        }
    }

    // 互斥校验（与调度层预检同源）
    if (const QString mutexErr = ContentEditHelper::mutualExclusionError(call.input); !mutexErr.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, mutexErr);
    }

    const QString oldString = call.input.value(QStringLiteral("oldString")).toString();
    QFileInfo info(filePath);
    QString content;
    QString originalContent;
    if (info.exists()) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return BuiltinToolRuntime::makeErrorResult(
                call, QStringLiteral("无法读取文件: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, filePath)));
        }
        content = QString::fromUtf8(file.readAll());
        originalContent = content;
    }

    if (!info.exists()) {
        // 创建语义仅限「有 oldString」（旧行为：文件不存在时用 newString 直接建内容）
        if (oldString.isEmpty()) {
            return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("File does not exist."));
        }
        content = call.input.value(QStringLiteral("newString")).toString();
    } else {
        // 行号模式（无 oldString）/ 字符串替换（有 oldString）统一走编辑助手
        const ContentEditHelper::Result r = ContentEditHelper::applyFromObject(content, call.input);
        if (!r.ok) {
            return BuiltinToolRuntime::makeErrorResult(call, r.error);
        }
        content = r.content;
    }

    // dry-run 不落盘，仅返回结构化补丁预览
    if (call.input.value(QStringLiteral("dryRun")).toBool()) {
        QJsonObject preview{
            {QStringLiteral("dryRun"), true},
            {QStringLiteral("filePath"), WorkspaceHelper::relativeFilePathForResult(workspaceRoot, filePath)},
            {QStringLiteral("structuredPatch"), buildStructuredPatch(originalContent, content)},
        };
        return BuiltinToolRuntime::makeSuccessResult(
            call, QStringLiteral("dry-run 预览（未写入）: %1").arg(QDir::toNativeSeparators(filePath)),
            QStringLiteral("editResult"), preview);
    }

    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath())) {
        return BuiltinToolRuntime::makeErrorResult(
            call, QStringLiteral("无法创建父目录: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, info.dir().absolutePath())));
    }

    const auto writeStatus = WorkspaceHelper::writeTextFileAtomically(filePath, content);
    if (writeStatus == WorkspaceHelper::AtomicWriteStatus::OpenFailed) {
        return BuiltinToolRuntime::makeErrorResult(
            call, QStringLiteral("无法写入文件: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, filePath)));
    }
    if (writeStatus == WorkspaceHelper::AtomicWriteStatus::WriteFailed) {
        return BuiltinToolRuntime::makeErrorResult(
            call, QStringLiteral("文件写入不完整: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, filePath)));
    }

    // 写后乐观冲突校验：防止并发窗口内被其他代理覆盖
    if (info.exists() && !WorkspaceHelper::fileContentMatches(filePath, content)) {
        return BuiltinToolRuntime::makeErrorResult(
            call, QStringLiteral("文件在写入期间被其他代理修改（内容不一致）。请重新读取文件后再编辑。"));
    }

    const QJsonObject payloadObject{
        {QStringLiteral("filePath"), WorkspaceHelper::relativeFilePathForResult(workspaceRoot, filePath)},
        {QStringLiteral("structuredPatch"), buildStructuredPatch(originalContent, content)},
    };
    return BuiltinToolRuntime::makeSuccessResult(
        call, QStringLiteral("已编辑 %1").arg(QDir::toNativeSeparators(filePath)),
        QStringLiteral("editResult"), payloadObject);
}
