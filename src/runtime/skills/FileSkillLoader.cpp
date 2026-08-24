#include "FileSkillLoader.h"

#include "logging/LogManager.h"

#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QTimer>

namespace {

// 共享 frontmatter 正则（避免重复编译）
const QRegularExpression &frontmatterRegex()
{
    static const QRegularExpression re(R"(^---\s*\n(.*?)\n---\s*\n)", QRegularExpression::DotMatchesEverythingOption);
    return re;
}

struct FrontmatterResult
{
    QHash<QString, QString> fields;
    QString body;
};

FrontmatterResult parseFrontmatter(const QString &text)
{
    FrontmatterResult result;

    // 匹配 YAML frontmatter 块（--- ... ---）
    const QRegularExpressionMatch match = frontmatterRegex().match(text);
    if (!match.hasMatch()) {
        result.body = text;
        return result;
    }

    // 逐行解析 frontmatter 内的键值对
    const QString yaml = match.captured(1);
    const QStringList lines = yaml.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines[i];
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) continue;
        const QString key = line.left(colon).trimmed();
        QString value = line.mid(colon + 1).trimmed();

        // 简单处理多行文本 (> 或 >-)
        if (value == QLatin1String(">") || value == QLatin1String(">-")) {
            value.clear();
            while (i + 1 < lines.size()) {
                const QString &nextLine = lines[i + 1];
                if (nextLine.isEmpty() || nextLine.startsWith(QLatin1Char(' '))) {
                    if (!value.isEmpty() && !nextLine.trimmed().isEmpty()) {
                        value += QLatin1Char(' ');
                    }
                    value += nextLine.trimmed();
                    ++i;
                } else {
                    break;
                }
            }
        } else {
            // 去除引号包裹
            if (value.size() >= 2 &&
                ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) ||
                 (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))) {
                value = value.mid(1, value.size() - 2);
            }
        }
        result.fields.insert(key, value);
    }

    // frontmatter 之后的部分为正文
    result.body = text.mid(match.capturedLength());
    return result;
}

// 解析 YAML 列表值如 "[bash, git]"
QStringList parseYamlList(const QString &value)
{
    QString clean = value.trimmed();
    if (clean.startsWith(QLatin1Char('[')) && clean.endsWith(QLatin1Char(']'))) {
        clean = clean.mid(1, clean.size() - 2);
    }
    QStringList result;
    const QStringList parts = clean.split(QLatin1Char(','));
    for (QString part : parts) {
        part = part.trimmed();
        if (!part.isEmpty()) result.append(part);
    }
    return result;
}

} // namespace

std::optional<FileSkill> FileSkillLoader::parseSkillFile(const QString &filePath)
{
    // 读取文件内容
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};

    const QString content = QString::fromUtf8(file.readAll());

    // 解析 frontmatter 与正文
    const FrontmatterResult fm = parseFrontmatter(content);

    // 填充 FileSkill 各字段
    FileSkill skill;
    skill.dirPath = QFileInfo(filePath).absolutePath();
    skill.dirName = QDir(skill.dirPath).dirName();
    skill.name = fm.fields.value(QStringLiteral("name")).trimmed();
    skill.description = fm.fields.value(QStringLiteral("description")).trimmed();
    skill.body = fm.body.trimmed();
    skill.userInvocable = fm.fields.value(QStringLiteral("user-invocable"), QStringLiteral("true")).trimmed() != QStringLiteral("false");

    const QString disableModel = fm.fields.value(QStringLiteral("disable-model-invocation")).trimmed();
    skill.disableModelInvocation = (disableModel == QStringLiteral("true"));

    const QString toolsStr = fm.fields.value(QStringLiteral("allowed-tools")).trimmed();
    if (!toolsStr.isEmpty()) {
        skill.allowedTools = parseYamlList(toolsStr);
    }

    return skill;
}

// ---- FileSkillLoader ----

FileSkillLoader::FileSkillLoader(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_changeDebounce(new QTimer(this))
    , m_cacheDirty(true)
{
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &FileSkillLoader::onDirectoryChanged);

    // 文件系统事件常成批连发（如 git checkout），用单发定时器合并后再通知
    m_changeDebounce->setSingleShot(true);
    m_changeDebounce->setInterval(200);
    connect(m_changeDebounce, &QTimer::timeout,
            this, &FileSkillLoader::skillsChanged);
}

FileSkillLoader::~FileSkillLoader() = default;

void FileSkillLoader::addSkillDirectory(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty()) return;
    if (!QDir(clean).exists()) return;

    {
        QMutexLocker locker(&m_cacheMutex);
        if (m_directories.contains(clean)) return;
        m_directories.append(clean);
        m_cacheDirty = true;
    }
    watchDirectory(clean);
    emit skillsChanged();
}

void FileSkillLoader::removeSkillDirectory(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    {
        QMutexLocker locker(&m_cacheMutex);
        m_directories.removeAll(clean);
        m_cacheDirty = true;
    }
    m_watcher->removePath(clean);
    emit skillsChanged();
}

QStringList FileSkillLoader::skillDirectories() const
{
    QMutexLocker locker(&m_cacheMutex);
    return m_directories;
}

QList<FileSkill> FileSkillLoader::loadAll() const
{
    QMutexLocker locker(&m_cacheMutex);

    // 缓存命中：直接返回
    if (!m_cacheDirty) {
        return m_cachedSkills;
    }

    // 缓存失效：重新扫描所有目录
    QList<FileSkill> all;
    for (const QString &dirPath : m_directories) {
        all.append(loadFromDirectory(dirPath));
    }

    // 更新缓存
    m_cachedSkills = all;
    m_cacheDirty = false;
    return all;
}

QList<FileSkill> FileSkillLoader::loadFromDirectory(const QString &path) const
{
    QList<FileSkill> skills;
    QDir root(path);
    if (!root.exists()) return skills;

    // 遍历子目录，查找并解析 SKILL.md
    const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        const QDir skillDir(root.absoluteFilePath(entry));
        const QString skillMdPath = findSkillMd(skillDir);
        if (skillMdPath.isEmpty()) continue;

        std::optional<FileSkill> skill = parseSkillFile(skillMdPath);
        if (skill.has_value()) {
            skills.append(skill.value());
        }
    }
    return skills;
}

void FileSkillLoader::onDirectoryChanged(const QString &path)
{
    // 扫描新子目录并加入监听
    QDir dir(path);
    if (dir.exists()) {
        const QStringList subs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        const QStringList watchedDirs = m_watcher->directories();
        for (const QString &sub : subs) {
            const QString full = dir.absoluteFilePath(sub);
            if (!watchedDirs.contains(full)) {
                m_watcher->addPath(full);
            }
        }
    }

    // 使缓存失效并触发去抖通知
    invalidateCache();
    m_changeDebounce->start();
}

void FileSkillLoader::invalidateCache() const
{
    QMutexLocker locker(&m_cacheMutex);
    m_cacheDirty = true;
}

void FileSkillLoader::watchDirectory(const QString &path)
{
    // 监听目录本身及其所有子目录
    m_watcher->addPath(path);
    QDir dir(path);
    const QStringList subs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &sub : subs) {
        m_watcher->addPath(dir.absoluteFilePath(sub));
    }
}

std::optional<FileSkill> FileSkillLoader::findByDirName(const QString &slashOrName) const
{
    QString target = slashOrName.trimmed();
    if (target.startsWith(QLatin1Char('/'))) {
        target = target.mid(1);
    }
    if (target.isEmpty()) {
        return std::nullopt;
    }
    for (const FileSkill &skill : loadAll()) {
        if (skill.slashName() == target || skill.dirName == target) {
            return skill;
        }
    }
    return std::nullopt;
}

QList<FileSkill> FileSkillLoader::userInvocableSkills() const
{
    QList<FileSkill> visible;
    for (const FileSkill &skill : loadAll()) {
        if (skill.userInvocable) {
            visible.append(skill);
        }
    }
    return visible;
}

QString FileSkillLoader::availableSkillsPromptBlock() const
{
    const QList<FileSkill> skills = userInvocableSkills();
    if (skills.isEmpty()) {
        return {};
    }

    QStringList lines;
    lines << QStringLiteral("<available_skills>");
    for (const FileSkill &skill : skills) {
        lines << QStringLiteral("  %1 — %2").arg(skill.slash(), skill.description);
    }
    lines << QStringLiteral(
        "若当前任务与以上某个技能匹配，调用 skill_list(name=\"<name>\") "
        "加载其完整指令并执行。用户可通过 / 命令触发。");
    lines << QStringLiteral("</available_skills>");
    return lines.join(QStringLiteral("\n"));
}

QString FileSkillLoader::findSkillMd(const QDir &dir)
{
    // 精确匹配 SKILL.md
    const QString exact = dir.absoluteFilePath(QStringLiteral("SKILL.md"));
    if (QFile::exists(exact)) return exact;

    // 不区分大小写回退
    const QStringList entries = dir.entryList(QDir::Files);
    for (const QString &entry : entries) {
        if (entry.compare(QStringLiteral("SKILL.md"), Qt::CaseInsensitive) == 0) {
            return dir.absoluteFilePath(entry);
        }
    }
    return {};
}
