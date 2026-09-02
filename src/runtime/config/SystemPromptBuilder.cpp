#include "SystemPromptBuilder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QtConcurrent>

namespace {

// 运行命令并返回版本字符串（超时 3 秒）
QString toolVersion(const QString &program, const QStringList &args)
{
    QProcess proc;
    proc.setProgram(program);
    proc.setArguments(args);
    proc.start();
    if (!proc.waitForFinished(3000)) {
        proc.kill();
        proc.waitForFinished(1000);
        return {};
    }
    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
    const QString first = out.left(out.indexOf(u'\n')).trimmed();
    return first.isEmpty() ? out : first;
}

struct ToolEntry {
    QString name;
    QString exe;
    QStringList args;     // 版本参数
    QStringList winFallbackArgs; // Windows 下备用参数
};

// 配置文件目录：内置 qrc 优先，外部应用目录 config/ 追加（同名 exe 不覆盖内置）。
QStringList toolConfigDirectories()
{
    QStringList dirs;
    dirs << QStringLiteral(":/config");
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        dirs << QDir(appDir).filePath(QStringLiteral("config"));
    }
    return dirs;
}

// 从 common_tools.json 加载工具清单；内置 qrc 为基底，外部文件只追加新工具。
QList<ToolEntry> loadCommonTools()
{
    QList<ToolEntry> tools;
    QSet<QString> seenExes;
    for (const QString &dirPath : toolConfigDirectories()) {
        QFile file(QDir(dirPath).filePath(QStringLiteral("common_tools.json")));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        const QJsonArray arr = doc.object().value(QStringLiteral("tools")).toArray();
        for (const QJsonValue &value : arr) {
            const QJsonObject obj = value.toObject();
            ToolEntry entry;
            entry.name = obj.value(QStringLiteral("name")).toString().trimmed();
            entry.exe = obj.value(QStringLiteral("exe")).toString().trimmed();
            const QJsonArray args = obj.value(QStringLiteral("args")).toArray();
            for (const QJsonValue &arg : args) {
                entry.args << arg.toString();
            }
            const QJsonArray fallbackArgs = obj.value(QStringLiteral("winFallbackArgs")).toArray();
            for (const QJsonValue &arg : fallbackArgs) {
                entry.winFallbackArgs << arg.toString();
            }
            if (entry.name.isEmpty() || entry.exe.isEmpty() || seenExes.contains(entry.exe)) {
                continue;
            }
            seenExes.insert(entry.exe);
            tools << entry;
        }
    }
    return tools;
}

// 运行时检测可用工具
QString detectTools()
{
    QStringList available;
    const QList<ToolEntry> tools = loadCommonTools();
    for (const ToolEntry &entry : tools) {
        const QString exe = entry.exe;
        if (QStandardPaths::findExecutable(exe).isEmpty())
            continue;
        QString ver = toolVersion(exe, entry.args);
        if (ver.isEmpty() && !entry.winFallbackArgs.empty())
            ver = toolVersion(exe, entry.winFallbackArgs);
        if (!ver.isEmpty())
            available << (ver.toLower().contains(entry.name)
                ? ver : entry.name + QStringLiteral(" (") + ver + QStringLiteral(")"));
        else
            available << entry.name;
    }
    return available.join(QStringLiteral(", "));
}

// 检测可用 Shell（平台特定）
QString detectShells()
{
    QStringList shells;
#if defined(Q_OS_WIN)
    const QString comspec = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("COMSPEC"), QStringLiteral("C:\\Windows\\system32\\cmd.exe"));
    const QString cmdVer = toolVersion(comspec, {QStringLiteral("/c"), QStringLiteral("ver")});
    shells << (cmdVer.isEmpty() ? QStringLiteral("cmd") : QStringLiteral("cmd (%1)").arg(cmdVer));

    const QString psPath = QStringLiteral("C:\\Windows\\system32\\WindowsPowerShell\\v1.0\\powershell.exe");
    if (QFileInfo::exists(psPath)) {
        QString psVer = toolVersion(psPath, {QStringLiteral("-Command"), QStringLiteral("$PSVersionTable.PSVersion.ToString()")});
        if (psVer.isEmpty()) psVer = toolVersion(psPath, {QStringLiteral("-Command"), QStringLiteral("$host.Version.ToString()")});
        shells << (psVer.isEmpty() ? QStringLiteral("powershell") : QStringLiteral("powershell (%1)").arg(psVer));
    }

    if (!QStandardPaths::findExecutable(QStringLiteral("pwsh")).isEmpty()) {
        const QString pwshVer = toolVersion(QStringLiteral("pwsh"), {QStringLiteral("--version")});
        shells << (pwshVer.isEmpty() ? QStringLiteral("pwsh") : pwshVer.trimmed());
    }

    if (!QStandardPaths::findExecutable(QStringLiteral("bash")).isEmpty()) {
        const QString bashVer = toolVersion(QStringLiteral("bash"), {QStringLiteral("--version")});
        const QString first = bashVer.left(bashVer.indexOf(u'\n')).trimmed();
        shells << (first.isEmpty() ? QStringLiteral("bash") : first);
    }

#elif defined(Q_OS_MACOS)
    for (const char *name : {"bash", "zsh"}) {
        const QString exe = QString::fromUtf8(name);
        if (!QStandardPaths::findExecutable(exe).isEmpty())
            shells << exe;
    }
#else
    if (!QStandardPaths::findExecutable(QStringLiteral("bash")).isEmpty())
        shells << QStringLiteral("bash");
#endif
    return shells.join(QStringLiteral(", "));
}

bool isSafePromptBasename(const QString &fileName)
{
    const QString name = fileName.trimmed();
    return !name.isEmpty()
        && !name.contains(QLatin1Char('/'))
        && !name.contains(QLatin1Char('\\'));
}

QStringList promptTemplateDirectories()
{
    QStringList dirs;
    dirs << QStringLiteral(":/system_prompts");
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        dirs << QDir(appDir).filePath(QStringLiteral("system_prompts"));
    }
    return dirs;
}

QString loadPromptTemplate(const QString &fileName)
{
    // 内置 qrc 在前作为基底，外部 system_prompts 目录同名文件只追加、不覆盖。
    QStringList parts;
    for (const QString &dirPath : promptTemplateDirectories()) {
        QFile file(QDir(dirPath).filePath(fileName));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        const QString content = QString::fromUtf8(file.readAll()).trimmed();
        if (!content.isEmpty()) {
            parts << content;
        }
    }
    return parts.join(QStringLiteral("\n\n"));
}

QString applyRolePlaceholders(QString text, const AgentPromptContext &ctx)
{
    text.replace(QStringLiteral("{agentId}"), ctx.agentId);
    text.replace(QStringLiteral("{displayName}"), ctx.displayName);
    text.replace(QStringLiteral("{parentAgentId}"), ctx.parentAgentId);
    return text;
}

QString loadUserPromptOverlay(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

/// 内置模板为基底，用户槽位文件追加为补充（非替换）
QString assembleOverlayPrompt(const QString &builtinFileName, const QString &overlayPath)
{
    QString prompt = loadPromptTemplate(builtinFileName);
    const QString extra = loadUserPromptOverlay(overlayPath);
    if (!extra.isEmpty()) {
        if (!prompt.isEmpty()) {
            prompt += QStringLiteral("\n\n");
        }
        prompt += extra;
    }
    return prompt;
}

} // namespace

SystemPromptBuilder::SystemPromptBuilder(QObject *parent)
    : SystemPromptBuilder(PromptPaths{}, parent)
{
}

SystemPromptBuilder::SystemPromptBuilder(PromptPaths paths, QObject *parent)
    : QObject(parent)
    , m_paths(std::move(paths))
{
    m_envWatcher = new QFutureWatcher<QString>(this);
    connect(m_envWatcher, &QFutureWatcher<QString>::finished, this, [this]() {
        m_cachedEnvBlock = m_envWatcher->result();
        emit environmentReady();
    });
}

void SystemPromptBuilder::prepare()
{
    m_baseBehavior = loadBaseBehavior();
    m_userCustomPrompt = loadUserPromptFile();
    invalidateStableCache();
    // 环境块异步检测（QtConcurrent），完成后发 environmentReady()，不阻塞主线程。
    if (!m_envWatcher->isRunning()) {
        m_envWatcher->setFuture(QtConcurrent::run(&SystemPromptBuilder::assembleEnvBlock));
    }
}

// ── 数据源 setter ──

void SystemPromptBuilder::setAvailableSkills(const QString &skillsBlock)
{
    // skill 块在 buildPrompt 中独立注入，不属于稳定缓存段，无需失效缓存
    m_availableSkills = skillsBlock.trimmed();
}

void SystemPromptBuilder::setUserCustomPrompt(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_userCustomPrompt == trimmed) return;
    m_userCustomPrompt = trimmed;
    invalidateStableCache();
}

// ── 核心拼接 ──

QString SystemPromptBuilder::builtinCompactSystemPrompt()
{
    return loadPromptTemplate(QStringLiteral("compact.md"));
}

QString SystemPromptBuilder::compactSystemPrompt() const
{
    return assembleOverlayPrompt(QStringLiteral("compact.md"), m_paths.compactOverlayFile);
}

QString SystemPromptBuilder::builtinSegmentSystemPrompt()
{
    return loadPromptTemplate(QStringLiteral("summary.md"));
}

QString SystemPromptBuilder::segmentSystemPrompt() const
{
    return assembleOverlayPrompt(QStringLiteral("summary.md"), m_paths.segmentOverlayFile);
}

QString SystemPromptBuilder::buildPrompt(const AgentPromptContext &ctx) const
{
    // 稳定段缓存填充（环境块已在初始化时预先组装，不依赖该标记）
    if (!m_stableCacheValid) {
        m_cachedUserBlock = assembleUserBlock();
        m_stableCacheValid = true;
    }

    // 环境块：静态检测内容 + 动态 DefaultShell（反映构建时刻的默认终端，不随运行中切换变化）。
    QString envBlock = m_cachedEnvBlock;
    const QString defaultShell = ctx.defaultShell.trimmed().isEmpty()
        ? QStringLiteral("bash")
        : ctx.defaultShell.trimmed();
    if (envBlock.isEmpty()) {
        envBlock = QStringLiteral("<env>\nDefaultShell: %1\n</env>").arg(defaultShell);
    } else {
        const QString closing = QStringLiteral("</env>");
        const int idx = envBlock.lastIndexOf(closing);
        if (idx >= 0) {
            envBlock.insert(idx, QStringLiteral("DefaultShell: %1\n").arg(defaultShell));
        } else {
            envBlock += QStringLiteral("\nDefaultShell: %1").arg(defaultShell);
        }
    }

    QStringList parts;
    parts << assembleBaseBlock(ctx.modePromptFile) << envBlock;

    // Skill 列表（动态更新，不参与稳定缓存）
    if (!m_availableSkills.isEmpty())
        parts << m_availableSkills;

    parts << m_cachedUserBlock;

    // 动态段：角色块（不缓存）
    const QString roleBlock = assembleRoleBlock(ctx);
    if (!roleBlock.isEmpty())
        parts.append(roleBlock);

    QString prompt = parts.join(QStringLiteral("\n\n"));
    prompt.replace(QStringLiteral("{workspacePath}"), ctx.workspacePath);
    return prompt;
}

// ── 缓存 ──

void SystemPromptBuilder::invalidateCache()
{
    invalidateStableCache();
}

void SystemPromptBuilder::invalidateStableCache() const
{
    m_stableCacheValid = false;
}

// ── 稳定段组装 ──

QString SystemPromptBuilder::assembleBaseBlock(const QString &modePromptFile) const
{
    QStringList blocks;
    const QString base = (m_baseBehavior.isEmpty() ? loadBaseBehavior() : m_baseBehavior).trimmed();
    if (!base.isEmpty()) {
        blocks << base;
    }
    const QString modeTemplate = loadNamedPromptTemplate(modePromptFile).trimmed();
    if (!modeTemplate.isEmpty()) {
        blocks << modeTemplate;
    }
    return blocks.join(QStringLiteral("\n\n"));
}

QString SystemPromptBuilder::assembleEnvBlock()
{
    QStringList lines;
    lines << QStringLiteral("<env>");

#if defined(Q_OS_WIN)
    // OS 信息
    const QString osVersion = QSysInfo::productType() + QStringLiteral(" ") + QSysInfo::productVersion();
    lines << QStringLiteral("OS: Windows (%1), kernel %2").arg(osVersion, QSysInfo::kernelVersion());

    // 系统架构
    lines << QStringLiteral("Arch: %1").arg(QSysInfo::currentCpuArchitecture());

    // 编码
    lines << QStringLiteral("TextEncoding: UTF-8");

    // 当前用户
    lines << QStringLiteral("User: %1").arg(QProcessEnvironment::systemEnvironment().value(QStringLiteral("USERNAME"), "?"));

    // 可用 Shell
    lines << QStringLiteral("Shells: %1").arg(detectShells());

    // 可用工具
    lines << QStringLiteral("Tools: %1").arg(detectTools());

#elif defined(Q_OS_MACOS)
    lines << QStringLiteral("OS: macOS %1, kernel %2").arg(QSysInfo::productVersion(), QSysInfo::kernelVersion());
    lines << QStringLiteral("Arch: %1").arg(QSysInfo::currentCpuArchitecture());
    lines << QStringLiteral("User: %1").arg(QProcessEnvironment::systemEnvironment().value(QStringLiteral("USER"), "?"));
    lines << QStringLiteral("Shells: bash, zsh");

#else
    lines << QStringLiteral("OS: Linux %1, kernel %2").arg(QSysInfo::productVersion(), QSysInfo::kernelVersion());
    lines << QStringLiteral("Arch: %1").arg(QSysInfo::currentCpuArchitecture());
    lines << QStringLiteral("User: %1").arg(QProcessEnvironment::systemEnvironment().value(QStringLiteral("USER"), "?"));
    lines << QStringLiteral("Shells: bash");
#endif

    lines << QStringLiteral("</env>");
    return lines.join(QStringLiteral("\n"));
}

QString SystemPromptBuilder::assembleUserBlock() const
{
    return m_userCustomPrompt;
}

QString SystemPromptBuilder::assembleRoleBlock(const AgentPromptContext &ctx) const
{
    return applyRolePlaceholders(loadNamedPromptTemplate(ctx.rolePromptFile), ctx);
}

QString SystemPromptBuilder::loadNamedPromptTemplate(const QString &fileName) const
{
    if (!isSafePromptBasename(fileName)) {
        return {};
    }
    return loadPromptTemplate(fileName.trimmed());
}

QString SystemPromptBuilder::loadBaseBehavior() const
{
    return loadPromptTemplate(QStringLiteral("base.md"));
}

QString SystemPromptBuilder::loadUserPromptFile() const
{
    const QString promptPath = m_paths.userPromptFile;
    if (promptPath.isEmpty()) {
        return {};
    }
    QFile file(promptPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

QString SystemPromptBuilder::loadPromptFile() const
{
    return loadUserPromptFile();
}

bool SystemPromptBuilder::savePromptFile(const QString &content)
{
    const QString path = m_paths.userPromptFile;
    if (path.isEmpty()) {
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    const qint64 written = file.write(content.toUtf8());
    file.close();
    if (written > 0) {
        setUserCustomPrompt(content.trimmed());
        return true;
    }
    return false;
}
