#pragma once

#include "tools/BuiltinToolRuntime.h"
#include "helpers/WorkspaceHelper.h"
#include "helpers/WriteGuardHelper.h"
#include "helpers/TextEncoding.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include "tools/AbstractBuiltinTool.h"

class ReadFileTool : public AbstractBuiltinTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    [[nodiscard]] QString progressKind() const override { return QStringLiteral("reading"); }

    ToolResult execute(const ToolCall &call,
                       const QString &workspaceRoot,
                       const QString &workingDirectory,
                       const QVariantMap &threadSafeContext) override;
};

namespace {

QString formatReadResult(const QString &content, int startLine)
{
    if (content.isEmpty()) {
        return QStringLiteral("[empty file]");
    }
    const QStringList lines = content.split(QLatin1Char('\n'));
    QStringList output;
    output.reserve(lines.size());
    for (int index = 0; index < lines.size(); ++index) {
        output.append(QStringLiteral("%1\t%2")
                          .arg(startLine + index)
                          .arg(lines.at(index)));
    }
    return output.join(QLatin1Char('\n'));
}

} // namespace

inline ToolSpec ReadFileTool::spec() const
{
    return ToolSpecBuilder("read_file", QStringLiteral("读取工作目录中文本文件内容。"))
        .requiredInput("filePath", "string", QStringLiteral("要读取的路径，支持工作目录内绝对或相对路径"))
        .input("offset", "integer", QStringLiteral("起始行号（1-based），可为空"))
        .input("limit", "integer", QStringLiteral("最多读取多少行，可为空"))
        .input("encoding", "string", QStringLiteral("文件编码，默认 UTF-8；支持 UTF-16/Latin-1/GBK/GB2312/GB18030 等，可为空"))
        .output("kind", "string", "text/fileUnchanged")
        .output("filePath", "string", QStringLiteral("文件路径"))
        .output("content", "string", QStringLiteral("读取到的文本内容"))
        .output("totalLines", "integer", QStringLiteral("文件总行数"))
        .output("partial", "boolean", QStringLiteral("是否为部分读取"))
        .output("stubKind", "string", QStringLiteral("kind=fileUnchanged 时的子类型（fileUnchanged/empty）"))
        .output("hint", "string", QStringLiteral("提示信息"))
        .build();
}

inline ToolResult ReadFileTool::execute(const ToolCall &call,
                                 const QString &workspaceRoot,
                                 const QString &workingDirectory,
                                 const QVariantMap &threadSafeContext)
{
    const QString pathValue = call.input.value(QStringLiteral("filePath")).toString();

    QString errorMessage;
    const QString filePath = BuiltinToolRuntime::resolveWorkspacePath(workingDirectory, pathValue, &errorMessage);
    if (filePath.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, errorMessage);
    }

    QFile file(filePath);
    if (!file.exists()) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("文件不存在: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, filePath)));
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("无法读取文件: %1").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, filePath)));
    }

    // 尺寸校验：1MB 上限（读入前，防 readAll 吃满内存）
    if (const QString sizeError = WriteGuardHelper::readLimitError(file.size());
        !sizeError.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, sizeError);
    }

    const int offset = qMax(0, call.input.value(QStringLiteral("offset")).toInt());
    int limit = call.input.value(QStringLiteral("limit")).toInt();
    const bool explicitOffset = call.input.contains(QStringLiteral("offset"));
    const bool explicitLimit = call.input.contains(QStringLiteral("limit"));
    const bool requestedFullRead = !explicitOffset && !explicitLimit;

    // 从线程安全上下文读取缓存
    const qint64 cacheTimestampMs = threadSafeContext.value(QStringLiteral("cacheTimestampMs")).toLongLong();
    const bool cachePartialView = threadSafeContext.value(QStringLiteral("cachePartialView")).toBool();

    const qint64 lastModifiedMs = QFileInfo(filePath).lastModified().toMSecsSinceEpoch();
    if (requestedFullRead
        && cacheTimestampMs > 0
        && !cachePartialView
        && lastModifiedMs <= cacheTimestampMs) {
        QJsonObject payload;
        payload.insert(QStringLiteral("kind"), QStringLiteral("fileUnchanged"));
        payload.insert(QStringLiteral("filePath"), WorkspaceHelper::relativeFilePathForResult(workspaceRoot, filePath));
        payload.insert(QStringLiteral("stubKind"), QStringLiteral("fileUnchanged"));
        payload.insert(QStringLiteral("hint"), QStringLiteral("文件自上次读取后未变更，请直接引用会话中较早的 read_file 结果，无需重新读取。"));
        return BuiltinToolRuntime::makeSuccessResult(call,
                                               QStringLiteral("文件未变更（%1）。请使用会话中较早的 read_file 结果，无需重新读取。").arg(WorkspaceHelper::relativeFilePathForResult(workspaceRoot, filePath)),
                                               QStringLiteral("readResult"),
                                               payload);
    }

    const QByteArray raw = file.readAll();
    // 二进制检测：解码前字节层面探测（NUL / 高比例控制字节）
    if (WriteGuardHelper::isBinary(raw)) {
        return BuiltinToolRuntime::makeErrorResult(
            call, QStringLiteral("文件为二进制文件，不读取内容（%1）。请用 run_command 或其他方式处理。").arg(WorkspaceHelper::relativeToWorkspace(workspaceRoot, filePath)));
    }

    // 解码：encoding 参数（默认 UTF-8；GBK 系走 Windows 原生）
    QString decodeError;
    const QString content = TextEncoding::decodeBytes(
        raw, call.input.value(QStringLiteral("encoding")).toString(), &decodeError);
    if (!decodeError.isEmpty()) {
        return BuiltinToolRuntime::makeErrorResult(call, decodeError);
    }
    const QStringList allLines = content.split(QLatin1Char('\n'));
    if (limit <= 0) {
        limit = allLines.size() - offset;
    }
    const QStringList slicedLines = allLines.mid(offset, limit);
    const QString rendered = formatReadResult(slicedLines.join(QLatin1Char('\n')),
                                              offset + 1);

    // partialView = 未覆盖到文件结尾（读到结尾即视为已覆盖全文件，offset>0 但读满也算）
    const bool partialView = (offset + slicedLines.size()) < allLines.size();

    QJsonObject payload;
    payload.insert(QStringLiteral("kind"), QStringLiteral("text"));
    payload.insert(QStringLiteral("filePath"), WorkspaceHelper::relativeFilePathForResult(workspaceRoot, filePath));
    payload.insert(QStringLiteral("content"), content);
    payload.insert(QStringLiteral("totalLines"), allLines.size());
    payload.insert(QStringLiteral("partial"), partialView);
    if (content.isEmpty()) {
        payload.insert(QStringLiteral("stubKind"), QStringLiteral("empty"));
        payload.insert(QStringLiteral("hint"), QStringLiteral("文件为空。"));
    }
    return BuiltinToolRuntime::makeSuccessResult(call, rendered, QStringLiteral("readResult"), payload);
}
