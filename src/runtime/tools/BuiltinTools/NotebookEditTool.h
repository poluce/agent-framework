#pragma once

#include "tools/BuiltinToolRuntime.h"
#include "helpers/WorkspaceHelper.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "tools/AbstractBuiltinTool.h"

class NotebookEditTool : public AbstractBuiltinTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    [[nodiscard]] QString writeTargetPathForInput(const QJsonObject &input) const override;
    [[nodiscard]] QString progressKind() const override { return QStringLiteral("editing_notebook"); }

    ToolResult execute(const ToolCall &call,
                       const QString &workspaceRoot,
                       const QString &workingDirectory,
                       const QVariantMap &threadSafeContext) override;
};

inline ToolSpec NotebookEditTool::spec() const
{
    return ToolSpecBuilder("notebook_edit", QStringLiteral("编辑工作目录中的 Jupyter Notebook 单元。"), ToolPermissionKind::Write)
        .requiredInput("notebookPath", "string", QStringLiteral("Notebook 文件路径"))
        .requiredInput("newSource", "string", QStringLiteral("新的单元内容"))
        .input("cellId", "string", QStringLiteral("要编辑的单元 id，可为空"))
        .input("cellType", "string", "code/markdown，可为空")
        .input("editMode", "string", "replace/insert/delete，可为空")
        .output("notebookPath", "string", QStringLiteral("Notebook 文件路径"))
        .output("editMode", "string", "replace/insert/delete")
        .output("cellId", "string", QStringLiteral("单元 id"))
        .output("cellType", "string", QStringLiteral("单元类型"))
        .output("newSource", "string", QStringLiteral("新的单元内容"))
        .output("error", "string", QStringLiteral("错误信息"))
        .build();
}

inline QString NotebookEditTool::writeTargetPathForInput(const QJsonObject &input) const
{
    return input.value(QStringLiteral("notebookPath")).toString();
}

inline ToolResult NotebookEditTool::execute(const ToolCall &call,
                                     const QString &workspaceRoot,
                                     const QString &workingDirectory,
                                     const QVariantMap &threadSafeContext)
{
    Q_UNUSED(threadSafeContext);
    const QString rawNotebookPath = call.input.value(QStringLiteral("notebookPath")).toString();

    QString errorMessage;
    const QString notebookPath = BuiltinToolRuntime::resolveWorkspacePath(workingDirectory, rawNotebookPath, &errorMessage);
    const QString newSource = call.input.value(QStringLiteral("newSource")).toString();
    QString cellId = call.input.value(QStringLiteral("cellId")).toString();
    QString cellType = call.input.value(QStringLiteral("cellType")).toString();
    QString editMode = call.input.value(QStringLiteral("editMode")).toString().trimmed();
    if (editMode.isEmpty()) {
        editMode = QStringLiteral("replace");
    }
    if (notebookPath.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, errorMessage);
    }

    QFile file(notebookPath);
    if (!file.exists()) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("Notebook 文件不存在: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, notebookPath)));
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("无法读取 Notebook: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, notebookPath)));
    }
    const QString originalContent = QString::fromUtf8(file.readAll());
    file.close();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(originalContent.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return BuiltinToolRuntime::makeSuccessResult(call,
                                                 QStringLiteral("Notebook 不是有效 JSON。"),
                                                 QStringLiteral("notebookEditResult"),
                                                 QJsonObject{
                                                     {QStringLiteral("notebookPath"), WorkspaceHelper::relativeFilePathForResult(workspaceRoot, notebookPath)},
                                                     {QStringLiteral("editMode"), editMode},
                                                     {QStringLiteral("cellId"), cellId},
                                                     {QStringLiteral("cellType"), cellType},
                                                     {QStringLiteral("newSource"), newSource},
                                                     {QStringLiteral("error"), QStringLiteral("Notebook is not valid JSON.")},
                                                 });
    }

    QJsonObject notebook = document.object();
    QJsonArray cells = notebook.value(QStringLiteral("cells")).toArray();
    int cellIndex = -1;
    if (!cellId.isEmpty()) {
        for (int i = 0; i < cells.size(); ++i) {
            const QJsonObject cell = cells.at(i).toObject();
            if (cell.value(QStringLiteral("id")).toString() == cellId) {
                cellIndex = i;
                break;
            }
        }
    }

    if (cellIndex < 0 && editMode != QStringLiteral("insert")) {
        cellIndex = cells.isEmpty() ? -1 : 0;
        if (!cells.isEmpty()) {
            cellId = cells.at(cellIndex).toObject().value(QStringLiteral("id")).toString();
        }
    }

    if (editMode == QStringLiteral("delete")) {
        if (cellIndex < 0 || cellIndex >= cells.size()) {
            return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("未找到要删除的 notebook cell。"));
        }
        const QJsonObject cell = cells.at(cellIndex).toObject();
        if (cellType.isEmpty()) {
            cellType = cell.value(QStringLiteral("cell_type")).toString();
        }
        cells.removeAt(cellIndex);
    } else if (editMode == QStringLiteral("insert")) {
        if (cellType.isEmpty()) {
            cellType = QStringLiteral("code");
        }
        if (cellId.isEmpty()) {
            cellId = QStringLiteral("cell-%1").arg(QDateTime::currentMSecsSinceEpoch());
        }
        QJsonObject newCell{
            {QStringLiteral("cell_type"), cellType},
            {QStringLiteral("id"), cellId},
            {QStringLiteral("metadata"), QJsonObject{}},
            {QStringLiteral("source"), newSource},
        };
        if (cellType == QStringLiteral("code")) {
            newCell.insert(QStringLiteral("execution_count"), QJsonValue::Null);
            newCell.insert(QStringLiteral("outputs"), QJsonArray{});
        }
        cells.append(newCell);
    } else {
        if (cellIndex < 0 || cellIndex >= cells.size()) {
            return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("未找到要编辑的 notebook cell。"));
        }
        QJsonObject cell = cells.at(cellIndex).toObject();
        cell.insert(QStringLiteral("source"), newSource);
        if (!cellType.isEmpty()) {
            cell.insert(QStringLiteral("cell_type"), cellType);
        } else {
            cellType = cell.value(QStringLiteral("cell_type")).toString();
        }
        if (cellType == QStringLiteral("code")) {
            cell.insert(QStringLiteral("execution_count"), QJsonValue::Null);
            cell.insert(QStringLiteral("outputs"), QJsonArray{});
        }
        if (cellId.isEmpty()) {
            cellId = cell.value(QStringLiteral("id")).toString();
        }
        cells.replace(cellIndex, cell);
    }

    notebook.insert(QStringLiteral("cells"), cells);
    const QString updatedContent = QString::fromUtf8(QJsonDocument(notebook).toJson(QJsonDocument::Indented));

    const auto writeStatus = WorkspaceHelper::writeTextFileAtomically(notebookPath, updatedContent);
    if (writeStatus == WorkspaceHelper::AtomicWriteStatus::OpenFailed) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("无法写回 Notebook: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, notebookPath)));
    }
    if (writeStatus == WorkspaceHelper::AtomicWriteStatus::WriteFailed) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("Notebook 写入不完整: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, notebookPath)));
    }

    // 写后乐观冲突校验：防止并发窗口内被其他代理覆盖
    if (!WorkspaceHelper::fileContentMatches(notebookPath, updatedContent)) {
        return BuiltinToolRuntime::makeErrorResult(
            call, QStringLiteral("Notebook 在写入期间被其他代理修改（内容不一致）。请重新读取文件后再编辑。"));
    }

    return BuiltinToolRuntime::makeSuccessResult(call,
                                           QStringLiteral("已编辑 Notebook %1").arg(QDir::toNativeSeparators(notebookPath)),
                                           QStringLiteral("notebookEditResult"),
                                           QJsonObject{
                                               {QStringLiteral("notebookPath"), WorkspaceHelper::relativeFilePathForResult(workspaceRoot, notebookPath)},
                                               {QStringLiteral("editMode"), editMode},
                                               {QStringLiteral("cellId"), cellId},
                                               {QStringLiteral("cellType"), cellType},
                                               {QStringLiteral("newSource"), newSource},
                                               {QStringLiteral("error"), QString()},
                                           });
}
