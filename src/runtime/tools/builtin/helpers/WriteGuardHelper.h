#pragma once

#include "tools/ToolTypes.h"

#include <QByteArray>
#include <QFileInfo>
#include <QString>

/**
 * @brief 写工具前置防护（目录/大小校验），三个写工具共用。
 */
namespace WriteGuardHelper {

/** 读取/写入内容上限（1MB），写工具与 read_file/grep 共用。 */
inline constexpr qint64 kMaxReadBytes = 1 * 1024 * 1024;

/** 返回超限错误消息；未超限返回空。 */
inline QString readLimitError(const qint64 size)
{
    if (size <= kMaxReadBytes)
        return {};
    return QStringLiteral("文件过大（%1 字节，上限 %2 字节），请用 run_command 或其他方式处理。")
        .arg(size)
        .arg(kMaxReadBytes);
}

/** 二进制检测：前 8KB 探测。
 * NUL 是硬判据；非空白控制字节（<0x20 且非 \n\r\t）≥10% 也判二进制。
 * 只探测前 8KB，大文本文件不全扫。 */
inline bool isBinary(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return false;
    const int scanned = qMin(bytes.size(), 8192);
    int nul = 0, control = 0;
    for (int i = 0; i < scanned; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes.at(i));
        if (c == 0)
            ++nul;
        else if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
            ++control;
    }
    return nul > 0 || control * 10 >= scanned;
}

/** 校验失败（目录/超限/读取前大小检查）时构造统一错误 ToolResult。 */
inline ToolResult makeError(const ToolCall &call, const QString &text)
{
    ToolResult r;
    r.toolName = call.toolName;
    r.toolUseId = call.id;
    r.success = false;
    r.isError = true;
    r.text = text;
    return r;
}

/** 目标路径是目录 → 返回错误 ToolResult；否则返回空 ToolResult。 */
inline ToolResult validateNotDirectory(const QString &resolvedPath, const ToolCall &call)
{
    if (!QFileInfo(resolvedPath).isDir())
        return {};
    return makeError(call, QStringLiteral("目标路径是目录，不是文件: %1").arg(resolvedPath));
}

/** content 超过上限（1MB）→ 返回错误 ToolResult；否则空。 */
inline ToolResult validateContentSize(const QString &content, const ToolCall &call)
{
    constexpr qint64 kMaxWriteContentBytes = 1 * 1024 * 1024;
    const qint64 size = content.toUtf8().size();
    if (size <= kMaxWriteContentBytes)
        return {};
    return makeError(call,
                     QStringLiteral("内容过大（%1 字节，上限 %2 字节），请分块写入或用 run_command。")
                         .arg(size)
                         .arg(kMaxWriteContentBytes));
}

/** 读取原文件前检查大小（1MB），超限报错。 */
inline ToolResult validateReadSize(const QString &resolvedPath, const ToolCall &call)
{
    const qint64 size = QFileInfo(resolvedPath).size();
    const QString error = readLimitError(size);
    if (error.isEmpty())
        return {};
    return makeError(call, error);
}

} // namespace WriteGuardHelper
