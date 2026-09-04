#pragma once

#include "FileSkill.h"

#include <QDir>
#include <QFileSystemWatcher>
#include <QMutex>
#include <QObject>
#include <QString>

#include <optional>

class QTimer;

// 从指定目录加载所有子目录中的 SKILL.md，并监听文件变更
class FileSkillLoader : public QObject
{
    Q_OBJECT

public:
    explicit FileSkillLoader(QObject *parent = nullptr);
    ~FileSkillLoader() override;

    // 添加/移除 skill 搜索目录
    void addSkillDirectory(const QString &path);
    void removeSkillDirectory(const QString &path);
    QStringList skillDirectories() const;

    // 加载/重载
    QList<FileSkill> loadAll() const;
    QList<FileSkill> loadFromDirectory(const QString &path) const;

    /// 按目录名查找，接受 "/name" 或 "name"
    [[nodiscard]] std::optional<FileSkill> findByDirName(const QString &slashOrName) const;
    [[nodiscard]] QList<FileSkill> userInvocableSkills() const;
    /// 组装 <available_skills> 系统提示块；无可用技能时返回空串
    [[nodiscard]] QString availableSkillsPromptBlock() const;
    /// 从给定技能列表组装提示块（供按单元过滤后复用；空列表返回空串）
    [[nodiscard]] static QString buildSkillsPromptBlock(const QList<FileSkill> &skills);

    // 解析单个 SKILL.md
    static std::optional<FileSkill> parseSkillFile(const QString &filePath);

signals:
    void skillsChanged();

private slots:
    void onDirectoryChanged(const QString &path);

private:
    void invalidateCache() const;
    void watchDirectory(const QString &path);
    static QString findSkillMd(const QDir &dir);

    QFileSystemWatcher *m_watcher;
    QTimer *m_changeDebounce;
    QStringList m_directories;
    // skill_list 工具可能在 QThreadPool 工作线程调用 loadAll()，缓存字段必须加锁
    mutable QMutex m_cacheMutex;
    mutable QList<FileSkill> m_cachedSkills;
    mutable bool m_cacheDirty = true;
};
