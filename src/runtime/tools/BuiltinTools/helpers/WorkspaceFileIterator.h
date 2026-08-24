#pragma once

#include "GitIgnoreMatcher.h"

#include <QString>

#include <functional>

class WorkspaceFileIterator
{
public:
    struct Entry {
        QString absolutePath;
        QString relativePath;
    };

    using FileVisitor = std::function<bool(const Entry &entry)>;

    explicit WorkspaceFileIterator(const QString &workspaceRoot,
                                   const QString &rootPath);

    void forEach(const FileVisitor &visitor);

private:
    void loadGitIgnoreRules(const QString &dirAbsolutePath,
                            const QString &dirRelativePath);

    QString m_workspaceRoot;
    QString m_rootPath;
    GitIgnoreMatcher m_gitignore;
};
