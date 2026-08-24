#include "WorkspaceHelper.h"
#include "FileLineEnding.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace WorkspaceHelper {

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString normalizedPath(const QString &path)
{
    QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    cleaned = cleaned.toLower();
#endif
    return cleaned;
}

bool isWithinWorkspace(const QString &workspaceRoot, const QString &candidatePath)
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

QString posixPathToWindowsPath(const QString &posixPath)
{
#ifdef Q_OS_WIN
    // /cygdrive/c/... → C:/...
    if (posixPath.startsWith(QStringLiteral("/cygdrive/")) && posixPath.size() > 10) {
        const QChar driveLetter = posixPath.at(10).toUpper();
        QString rest = posixPath.mid(11);
        return driveLetter + QStringLiteral(":") + QString(rest).replace(QLatin1Char('/'), QLatin1Char('\\'));
    }
    // /mnt/c/... → C:/...  (WSL 默认挂载)
    if (posixPath.startsWith(QStringLiteral("/mnt/")) && posixPath.size() > 5) {
        const QChar driveLetter = posixPath.at(5).toUpper();
        QString rest = posixPath.mid(6);
        return driveLetter + QStringLiteral(":") + QString(rest).replace(QLatin1Char('/'), QLatin1Char('\\'));
    }
    // /c/... → C:/...  (MSYS2 / Git Bash)
    if (posixPath.size() >= 2
        && posixPath.at(0) == QLatin1Char('/')
        && posixPath.at(1).isLetter()
        && (posixPath.size() == 2 || posixPath.at(2) == QLatin1Char('/'))) {
        const QChar driveLetter = posixPath.at(1).toUpper();
        QString rest = posixPath.mid(2);
        return driveLetter + QStringLiteral(":") + QString(rest).replace(QLatin1Char('/'), QLatin1Char('\\'));
    }
#endif
    return posixPath;
}

QString relativeToWorkspace(const QString &workspaceRoot, const QString &absolutePath)
{
    return QDir(workspaceRoot).relativeFilePath(absolutePath);
}

QString relativeFilePathForResult(const QString &workspaceRoot, const QString &absolutePath)
{
    return QDir::fromNativeSeparators(relativeToWorkspace(workspaceRoot, absolutePath));
}

AtomicWriteStatus writeTextFileAtomically(const QString &filePath, const QString &content)
{
    // F：读原文件行尾并还原（CRLF 文件保持 CRLF；新文件默认 \n）
    QList<QString> endings;
    {
        QFile existing(filePath);
        if (existing.exists() && existing.open(QIODevice::ReadOnly)) {
            endings = FileLineEnding::lineEndingsOf(existing.readAll());
            existing.close();
        }
    }
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) // F：不用 Text（避免 \n→\r\n 双重转换），行尾由 restoreLineEndings 掌控
        return AtomicWriteStatus::OpenFailed;
    const QByteArray bytes = endings.isEmpty()
        ? content.toUtf8()
        : FileLineEnding::restoreLineEndings(content, endings);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return AtomicWriteStatus::WriteFailed;
    return AtomicWriteStatus::Ok;
}

bool fileContentMatches(const QString &filePath, const QString &expectedContent)
{
    QFile verify(filePath);
    if (!verify.open(QIODevice::ReadOnly))
        return true;
    // F：读原样字节 → 归一 \n 后比对（与写路径的行尾处理一致）
    return FileLineEnding::normalizeToLf(verify.readAll()) == expectedContent;
}

} // namespace WorkspaceHelper
