#pragma once

#include <QString>

namespace WorkspaceHelper {

Qt::CaseSensitivity pathCaseSensitivity();
QString normalizedPath(const QString &path);
bool isWithinWorkspace(const QString &workspaceRoot, const QString &candidatePath);
QString posixPathToWindowsPath(const QString &posixPath);
QString relativeToWorkspace(const QString &workspaceRoot, const QString &absolutePath);
QString relativeFilePathForResult(const QString &workspaceRoot, const QString &absolutePath);

/** QSaveFile 原子写文本结果（保留写工具原错误语义：打开失败 / 写不完整）。 */
enum class AtomicWriteStatus {
    Ok,
    OpenFailed,
    WriteFailed,
};

/** QSaveFile 原子写文本。 */
AtomicWriteStatus writeTextFileAtomically(const QString &filePath, const QString &content);

/**
 * 读盘比对期望内容。
 * 打开失败时返回 true（与写工具原语义一致：无法校验不挡成功路径）；
 * 仅在内容可读且不一致时返回 false。
 */
bool fileContentMatches(const QString &filePath, const QString &expectedContent);

} // namespace WorkspaceHelper
