#include "WorkspaceFileIterator.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QList>

static const QStringList kDefaultExcludes = {
    QStringLiteral(".git"),
    QStringLiteral("build"),
    QStringLiteral("CMakeFiles"),
    QStringLiteral(".cmake"),
    QStringLiteral("target"),
    QStringLiteral("dist"),
    QStringLiteral("node_modules"),
    QStringLiteral("__pycache__"),
    QStringLiteral("vendor"),
    QStringLiteral(".venv"),
};

WorkspaceFileIterator::WorkspaceFileIterator(const QString &workspaceRoot,
                                             const QString &rootPath)
    : m_workspaceRoot(workspaceRoot)
    , m_rootPath(rootPath)
{
    for (const QString &dir : kDefaultExcludes)
        m_gitignore.addBuiltinExclusion(dir);

    const QString gitignorePath = QDir(m_workspaceRoot)
                                      .absoluteFilePath(QStringLiteral(".gitignore"));
    if (QFileInfo::exists(gitignorePath)) {
        m_gitignore.loadFromFile(gitignorePath);
    }
}

void WorkspaceFileIterator::forEach(const FileVisitor &visitor)
{
    struct DirEntry {
        QString absolutePath;
        QString relativePath;
    };

    QList<DirEntry> stack;
    const QString startRelative = QDir(m_workspaceRoot).relativeFilePath(m_rootPath);
    stack.append({m_rootPath, startRelative == QStringLiteral(".") ? QString() : startRelative});

    while (!stack.isEmpty()) {
        const DirEntry current = stack.takeLast();

        // 加载当前目录的 .gitignore
        loadGitIgnoreRules(current.absolutePath, current.relativePath);

        QDir dir(current.absolutePath);
        const QFileInfoList entries = dir.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
            QDir::Name | QDir::DirsFirst);

        for (const QFileInfo &info : entries) {
            QString relPath = current.relativePath.isEmpty()
                                  ? info.fileName()
                                  : current.relativePath + QLatin1Char('/') + info.fileName();
            relPath = QDir::fromNativeSeparators(relPath);

            if (info.isDir()) {
                const QString dirRelPath = relPath + QLatin1Char('/');
                if (m_gitignore.isExcluded(dirRelPath, true))
                    continue;
                stack.append({info.absoluteFilePath(), relPath});
            } else {
                if (m_gitignore.isExcluded(relPath, false))
                    continue;

                Entry entry;
                entry.absolutePath = info.absoluteFilePath();
                entry.relativePath = relPath;

                // visitor 返回 false 表示停止遍历
                if (!visitor(entry))
                    return;
            }
        }
    }
}

void WorkspaceFileIterator::loadGitIgnoreRules(const QString &dirAbsolutePath,
                                               const QString &dirRelativePath)
{
    const QString localGiPath = QDir(dirAbsolutePath)
                                    .absoluteFilePath(QStringLiteral(".gitignore"));
    if (QFileInfo::exists(localGiPath)) {
        m_gitignore.loadFromFile(localGiPath, dirRelativePath);
    }
}
