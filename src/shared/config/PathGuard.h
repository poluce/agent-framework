#pragma once

#include <QDir>
#include <QString>

/** 工作区路径边界（与 WorkspaceHelper 同语义，无 runtime 依赖）。 */
namespace PathGuard {

[[nodiscard]] inline Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

[[nodiscard]] inline QString normalizedPath(const QString &path)
{
    QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    cleaned = cleaned.toLower();
#endif
    return cleaned;
}

[[nodiscard]] inline bool isWithinWorkspace(const QString &workspaceRoot, const QString &candidatePath)
{
    const QString normalizedRoot = normalizedPath(workspaceRoot);
    const QString normalizedCandidate = normalizedPath(candidatePath);
    if (normalizedRoot.compare(normalizedCandidate, pathCaseSensitivity()) == 0) {
        return true;
    }
    const QString prefix = normalizedRoot.endsWith(QLatin1Char('/'))
                               ? normalizedRoot
                               : normalizedRoot + QLatin1Char('/');
    return normalizedCandidate.startsWith(prefix, pathCaseSensitivity());
}

} // namespace PathGuard
