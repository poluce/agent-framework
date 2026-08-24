#include "LogManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QMutexLocker>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <cstdio>
#include <cstdlib>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// 同步运行 git 命令，完全避免 QProcess 事件循环的 Windows 定时器开销
int runGit(const QString &workingDir, const QStringList &args, int timeoutMs = 5000)
{
#ifdef Q_OS_WIN
    // Win32 直接调用，不经过 Qt 事件循环
    QString cmdLine = QStringLiteral("git");
    for (const QString &arg : args) {
        cmdLine += QStringLiteral(" \"") + arg + QLatin1Char('"');
    }
    std::wstring wCmdLine = cmdLine.toStdWString();
    std::wstring wWorkDir = QDir::toNativeSeparators(workingDir).toStdWString();

    // CreateProcessW 会修改命令行缓冲区，需要可写副本
    std::vector<wchar_t> cmdBuf(wCmdLine.size() + 1);
    wcscpy_s(cmdBuf.data(), cmdBuf.size(), wCmdLine.c_str());

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    const BOOL ok = CreateProcessW(
        nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr,
        wWorkDir.empty() ? nullptr : wWorkDir.c_str(),
        &si, &pi);

    if (!ok)
        return -1;

    WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutMs));

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
#else
    QProcess proc;
    proc.setWorkingDirectory(workingDir);
    proc.start(QStringLiteral("git"), args);
    proc.waitForFinished(timeoutMs);
    return proc.exitCode();
#endif
}

// 按日期 + 进程 PID 命名：core_<日期>_<PID>.log，与 gui/tui 的 client_*.log 分开。
QString logFilePathForDate(const QString &logDir, const QString &dateKey)
{
    return logDir + QStringLiteral("/core_") + dateKey
        + QStringLiteral("_") + QString::number(QCoreApplication::applicationPid())
        + QStringLiteral(".log");
}

} // namespace

// ======== 级别名称表 ========
static const char *kLevelNames[] = {
    "TRACE",   // Trace
    "DEBUG",   // Debug
    "INFO ",   // Info  (右侧补空格对齐)
    "WARN ",   // Warning
    "ERROR",   // Error
    "FATAL"    // Fatal
};

static_assert(sizeof(kLevelNames) / sizeof(kLevelNames[0]) == 6,
              "kLevelNames must match LogLevel enum");

// ======== ANSI 颜色表 ========
static const char *kLevelColors[] = {
    "\033[90m",      // Trace  → 灰色
    "\033[32m",      // Debug  → 绿色
    "\033[34m",      // Info   → 蓝色
    "\033[33m",      // Warn   → 黄色
    "\033[31m",      // Error  → 红色
    "\033[1;31m"     // Fatal  → 红加粗
};

static const char *kColorReset = "\033[0m";

// ======== Qt 消息类型 → LogLevel ========
static LogLevel qtMsgTypeToLevel(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return LogLevel::Debug;
    case QtInfoMsg:     return LogLevel::Info;
    case QtWarningMsg:  return LogLevel::Warning;
    case QtCriticalMsg: return LogLevel::Error;
    case QtFatalMsg:    return LogLevel::Fatal;
    }
    return LogLevel::Debug;
}

// ======================================================================
// LogManager
// ======================================================================

LogManager &LogManager::instance()
{
    static LogManager s_instance;
    return s_instance;
}

LogManager::LogManager()
    : QObject()
{
}

LogManager::~LogManager()
{
    shutdown();
}

void LogManager::init(const QString &logDir)
{
    QMutexLocker lock(&m_mutex);

    if (m_initialized)
        return;

    // 目录由调用方注入；空则只走控制台 / Qt 消息处理器，不回落本产品 AppData
    m_logDir = logDir;
    m_logFilePath.clear();
    m_currentDateKey.clear();
    if (!m_logDir.isEmpty()) {
        QDir().mkpath(m_logDir);

        // 按日期 + 进程 PID 打开 core_<日期>_<PID>.log：两进程各写各的，避免互清 / 混写
        m_currentDateKey = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd"));
        m_logFilePath = logFilePathForDate(m_logDir, m_currentDateKey);

        m_logFile.setFileName(m_logFilePath);
        if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            m_fileStream.setDevice(&m_logFile);
        }
    }

    // 安装 Qt 全局消息处理器
    m_oldHandler = qInstallMessageHandler(qtMessageHandler);

    // 控制台输出是否开启由调用方在 init 前 setConsoleOutputEnabled 决定
    // （TUI 经 ApplicationComposition(consoleLogging=false) 关闭，避免污染 FTXUI）

    m_initialized = true;
    lock.unlock();

    LOGI(LogCat::System) << "日志系统初始化完成"
        << logf("version", QCoreApplication::applicationVersion());
    LOGI(LogCat::System) << "日志目录"
        << logf("dir", m_logDir);
}

void LogManager::shutdown()
{
    QMutexLocker lock(&m_mutex);

    if (!m_initialized)
        return;

    if (m_oldHandler)
        qInstallMessageHandler(m_oldHandler);

    m_initialized = false;
    lock.unlock();

    LOGI(LogCat::System) << "日志系统关闭";
    m_logFile.close();
}

void LogManager::setConsoleOutputEnabled(bool enabled)
{
    QMutexLocker lock(&m_mutex);
    m_consoleOutputEnabled = enabled;
}

void LogManager::setDiagnosticMode(const bool enabled)
{
    QMutexLocker lock(&m_mutex);
    m_diagnosticMode = enabled;
}

bool LogManager::diagnosticMode() const
{
    QMutexLocker lock(&m_mutex);
    return m_diagnosticMode;
}

void LogManager::setRequestBodiesDirectory(const QString &dir)
{
    QMutexLocker lock(&m_mutex);
    m_requestBodiesDir = dir;
}

QString LogManager::requestBodiesDirectory() const
{
    QMutexLocker lock(&m_mutex);
    return m_requestBodiesDir;
}

// ======== 写入 ========

void LogManager::write(LogLevel level, const QString &category,
                       const QString &message, const char *file, int line)
{
    writeImpl(level, category, message, nullptr, file, line);
}

void LogManager::write(LogLevel level, const QString &category, const QString &message,
                       const AgentLogContext &agentCtx,
                       const char *file, const int line)
{
    writeImpl(level, category, message, &agentCtx, file, line);
}

void LogManager::writeImpl(const LogLevel level,
                           const QString &category,
                           const QString &message,
                           const AgentLogContext *agentCtx,
                           const char *file,
                           const int line)
{
    QMutexLocker lock(&m_mutex);

    // 级别过滤（仅控制台受限制）
    const bool consoleEnabled = (level >= m_level) && m_consoleOutputEnabled;
    const bool fileEnabled = shouldWriteToFile(level, message);

    const QString fileName = file ? stripPath(QString::fromUtf8(file)) : QString();
    const QString formatted = formatMessage(level, category, message, fileName, line, agentCtx);

    if (consoleEnabled)
        writeToConsole(formatted, level);

    if (fileEnabled)
        writeToFile(formatted);

    lock.unlock();
}

// ======== 快捷方法 ========

// ======== 流式 LogRecord ========

void LogRecord::commit()
{
    if (!m_active) {
        return;
    }
    m_active = false;

    QString message = m_event;
    if (!m_detail.isEmpty()) {
        if (!message.isEmpty()) {
            message += QLatin1Char(' ');
        }
        message += m_detail;
    }
    if (!m_fields.isEmpty()) {
        if (!message.isEmpty()) {
            message += QLatin1Char(' ');
        }
        message += m_fields.join(QLatin1Char(' '));
    }
    if (message.isEmpty()) {
        message = QStringLiteral("(empty)");
    }

    if (m_hasAgentCtx) {
        LogManager::instance().write(m_level, m_category, message, m_agentCtx, m_file, m_line);
    } else {
        LogManager::instance().write(m_level, m_category, message, m_file, m_line);
    }
}

// ======== 全局消息处理器 ========

void LogManager::qtMessageHandler(QtMsgType type,
                                  const QMessageLogContext &ctx,
                                  const QString &msg)
{
    LogManager::instance().write(
        qtMsgTypeToLevel(type),
        LogCat::System,
        msg,
        ctx.file ? ctx.file : "",
        ctx.line);

    if (type == QtFatalMsg)
        std::abort();
}

// ======== 内部方法 ========

QString LogManager::formatMessage(LogLevel level, const QString &category,
                                  const QString &message, const QString &fileName,
                                  int line,
                                  const AgentLogContext *agentCtx) const
{
    const QString timestamp = QDateTime::currentDateTime()
                                  .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString loc = (line > 0)
                            ? QStringLiteral("%1:%2").arg(fileName).arg(line)
                            : QStringLiteral("---");

    const QString sessionShortId = (agentCtx && !agentCtx->sessionShortId.isEmpty())
                                       ? agentCtx->sessionShortId
                                       : (agentCtx ? agentCtx->sessionUuid.left(8) : QString());
    const QString agentCtxText = (agentCtx && !sessionShortId.isEmpty()
                                  && !agentCtx->agentType.isEmpty()
                                  && !agentCtx->agentId.isEmpty())
        ? QStringLiteral(" [%1] [%2:%3]")
              .arg(sessionShortId,
                   agentCtx->agentType,
                   agentCtx->agentId)
        : QString();

    return QStringLiteral("[%1] [%2] [%3] [%4]%5 %6")
        .arg(timestamp, levelPrefix(level), category, loc, agentCtxText, message);
}

void LogManager::writeToConsole(const QString &formatted, LogLevel level)
{
    const int idx = static_cast<int>(level);
    std::fprintf(stderr, "%s%s%s\n",
                 kLevelColors[idx],
                 formatted.toLocal8Bit().constData(),
                 kColorReset);
    std::fflush(stderr);
}

void LogManager::writeToFile(const QString &formatted)
{
    if (!m_logFile.isOpen())
        return;

    checkDateChange();
    rotateIfNeeded();

    m_fileStream << formatted << Qt::endl;
    m_fileStream.flush();
}

bool LogManager::shouldWriteToFile(const LogLevel level, const QString &message) const
{
    if (!m_diagnosticMode) {
        return true;
    }

    if (level >= LogLevel::Warning) {
        return true;
    }

    return isDiagnosticMessage(message);
}

bool LogManager::isDiagnosticMessage(const QString &message)
{
    return message.contains(QStringLiteral("[diag]"));
}

void LogManager::checkDateChange()
{
    const QString today = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd"));
    if (today == m_currentDateKey)
        return;

    // 日期变化，关闭当前文件，打开新文件
    m_fileStream.flush();
    m_logFile.close();

    m_currentDateKey = today;
    m_logFilePath = logFilePathForDate(m_logDir, today);

    m_logFile.setFileName(m_logFilePath);
    if (m_logFile.open(QIODevice::Append | QIODevice::Text)) {
        m_fileStream.setDevice(&m_logFile);
    }
}

void LogManager::rotateIfNeeded()
{
    if (m_logFile.size() < m_maxFileSize)
        return;

    m_fileStream.flush();
    m_logFile.close();

    // 删除最旧的备份
    const QString oldest = m_logFilePath + QStringLiteral(".%1").arg(m_maxBackupFiles);
    QFile::remove(oldest);

    // 依次 rename .N → .N+1
    for (int i = m_maxBackupFiles - 1; i >= 1; --i) {
        const QString from = m_logFilePath + QStringLiteral(".%1").arg(i);
        const QString to   = m_logFilePath + QStringLiteral(".%1").arg(i + 1);
        QFile::rename(from, to);
    }

    // 当前文件 → .1
    QFile::rename(m_logFilePath, m_logFilePath + QStringLiteral(".1"));

    // 重建当前文件
    if (m_logFile.open(QIODevice::Append | QIODevice::Text)) {
        m_fileStream.setDevice(&m_logFile);
    }
}

// ======== 请求体记录 ========

QString LogManager::ensureRequestRepo(const QString &sessionUuid)
{
    if (m_requestBodiesDir.isEmpty())
        return {};

    const QString repoDir = m_requestBodiesDir + QLatin1Char('/') + sessionUuid;
    QDir().mkpath(repoDir);

    const QString gitDir = repoDir + QStringLiteral("/.git");
    if (!QDir().exists(gitDir)) {
        runGit(repoDir, {QStringLiteral("init")}, 5000);
    }
    return repoDir;
}

void LogManager::saveRequestBody(const QString &provider,
                                 const QByteArray &body,
                                 const AgentLogContext &agentCtx)
{
    // 在锁外收集错误消息，避免 LOGW 在 mutex 内死锁
    QStringList warnings;

    {
        QMutexLocker lock(&m_mutex);

        if (m_requestBodiesDir.isEmpty() || agentCtx.sessionUuid.trimmed().isEmpty())
            return;

        const QString repoDir = ensureRequestRepo(agentCtx.sessionUuid);
        if (repoDir.isEmpty())
            return;

        // 子目录: main/ 或 sub/{agentId}/
        QString subDir = agentCtx.agentType;
        if (agentCtx.agentType == QStringLiteral("sub") && !agentCtx.agentId.isEmpty()) {
            subDir += QLatin1Char('/') + agentCtx.agentId;
        }
        const QString dirPath = repoDir + QLatin1Char('/') + subDir;
        QDir().mkpath(dirPath);

        // 固定文件名，每次覆写，借助 git 追踪每轮变化
        const QString fileName = QStringLiteral("request.json");
        const QString filePath = dirPath + QLatin1Char('/') + fileName;

        // 解析并格式化输出（缩进排版），确保 git diff 可逐行对比
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        const QByteArray formatted = (parseError.error == QJsonParseError::NoError)
            ? formatJsonForDiff(doc)
            : body;

        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            warnings.append(QStringLiteral("无法写入请求体文件: %1").arg(filePath));
            lock.unlock();
            for (const QString &w : warnings)
                LOGW(LogCat::Storage, agentCtx) << w;
            return;
        }
        file.write(formatted);
        file.close();

        // 提取元信息
        const QString userMsg = extractUserMessage(body, provider);
        const int toolCount = extractToolCount(body, provider);
        const QString commitMsg = formatCommitMessage(provider, agentCtx.agentType,
                                                      body.size(), toolCount, userMsg);

        // git add: 使用相对于 repoDir 的路径
        const QString gitAddPath = subDir + QLatin1Char('/') + fileName;
        const int addRc = runGit(repoDir, {QStringLiteral("add"), gitAddPath}, 5000);
        if (addRc != 0) {
            warnings.append(QStringLiteral("git add 失败 (exit=%1): %2")
                                .arg(addRc).arg(gitAddPath));
        }
        // git commit
        if (!warnings.isEmpty()) {
            // git add 失败时跳过 commit，避免无意义的失败
        } else {
            const int commitRc = runGit(repoDir, {
                QStringLiteral("-c"), QStringLiteral("user.name=agent_auto"),
                QStringLiteral("-c"), QStringLiteral("user.email=agent_auto@local"),
                QStringLiteral("commit"), QStringLiteral("-m"), commitMsg}, 10000);
            if (commitRc != 0) {
                warnings.append(QStringLiteral("git commit 失败 (exit=%1)").arg(commitRc));
            }
        }
    }

    // 在锁外输出警告
    for (const QString &w : warnings)
        LOGW(LogCat::System, agentCtx) << w;
}

QString LogManager::extractUserMessage(const QByteArray &body, const QString &provider)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return QStringLiteral("(空)");

    if (provider == QStringLiteral("deepseek")) {
        const QJsonArray messages = doc.object().value(QStringLiteral("messages")).toArray();
        for (int i = messages.size() - 1; i >= 0; --i) {
            const QJsonObject msg = messages[i].toObject();
            if (msg.value(QStringLiteral("role")).toString() == QStringLiteral("user")) {
                const QJsonValue content = msg.value(QStringLiteral("content"));
                QString text;
                if (content.isString()) {
                    text = content.toString();
                } else if (content.isArray()) {
                    for (const QJsonValue &part : content.toArray()) {
                        const QJsonObject partObj = part.toObject();
                        if (partObj.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                            text += partObj.value(QStringLiteral("text")).toString();
                        }
                    }
                }
                if (text.length() > 200)
                    text = text.left(200) + QStringLiteral("...(截断)");
                return text.isEmpty() ? QStringLiteral("(空)") : text;
            }
        }
        return QStringLiteral("(空)");
    }

    if (provider == QStringLiteral("responses")) {
        const QJsonArray input = doc.object().value(QStringLiteral("input")).toArray();
        for (int i = input.size() - 1; i >= 0; --i) {
            const QJsonObject item = input[i].toObject();
            if (item.value(QStringLiteral("type")).toString() == QStringLiteral("message")
                && item.value(QStringLiteral("role")).toString() == QStringLiteral("user")) {
                const QJsonArray content = item.value(QStringLiteral("content")).toArray();
                QString text;
                for (const QJsonValue &part : content) {
                    const QJsonObject partObj = part.toObject();
                    const QString partType = partObj.value(QStringLiteral("type")).toString();
                    if (partType == QStringLiteral("input_text") || partType == QStringLiteral("text")) {
                        text += partObj.value(QStringLiteral("text")).toString();
                    }
                }
                if (text.length() > 200)
                    text = text.left(200) + QStringLiteral("...(截断)");
                return text.isEmpty() ? QStringLiteral("(空)") : text;
            }
        }
        return QStringLiteral("(空)");
    }

    return QStringLiteral("(空)");
}

int LogManager::extractToolCount(const QByteArray &body, const QString &provider)
{
    Q_UNUSED(provider);
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return 0;

    const QJsonArray tools = doc.object().value(QStringLiteral("tools")).toArray();
    return tools.size();
}

QString LogManager::formatCommitMessage(const QString &provider,
                                         const QString &agentType,
                                         int bodySize, int toolCount,
                                         const QString &userMsg)
{
    // 格式化文件大小
    const QString sizeStr = bodySize >= 1024
        ? QStringLiteral("%1KB").arg(bodySize / 1024.0, 0, 'f', 1)
        : QStringLiteral("%1B").arg(bodySize);

    const QString timestamp = QDateTime::currentDateTime()
                                  .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    return QStringLiteral("[%1] %2 | %3 | %4 | %5 tools\n\n用户: %6")
        .arg(provider, timestamp, agentType, sizeStr)
        .arg(toolCount)
        .arg(userMsg);
}

// ======== 辅助方法 ========

QString LogManager::levelPrefix(LogLevel level)
{
    const int idx = static_cast<int>(level);
    return (idx >= 0 && idx < 6)
               ? QString::fromUtf8(kLevelNames[idx])
               : QStringLiteral("?????");
}

const char *LogManager::levelAnsiColor(LogLevel level)
{
    const int idx = static_cast<int>(level);
    return (idx >= 0 && idx < 6) ? kLevelColors[idx] : kColorReset;
}

QString LogManager::stripPath(const QString &filePath)
{
    const int pos = filePath.lastIndexOf('/');
    const int pos2 = filePath.lastIndexOf('\\');
    const int start = (pos > pos2) ? pos : pos2;
    return (start >= 0) ? filePath.mid(start + 1) : filePath;
}

// ======== JSON 格式化（content 字段紧凑输出，其余缩进）========

/// 将任意 QJsonValue 序列化为紧凑 JSON 片段（无换行、无缩进）
static QByteArray valueToCompact(const QJsonValue &val)
{
    if (val.isObject())
        return QJsonDocument(val.toObject()).toJson(QJsonDocument::Compact);
    if (val.isArray())
        return QJsonDocument(val.toArray()).toJson(QJsonDocument::Compact);
    // 基本类型：裹进单元素数组再剥壳
    QJsonArray arr;
    arr.append(val);
    const QByteArray json = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    return json.mid(1, json.size() - 2); // 去头尾 []
}

static void writeIndent(QByteArray &out, int level)
{
    for (int i = 0; i < level; ++i)
        out.append("    ");
}

static void writeVal(QByteArray &out, const QJsonValue &val, int level, bool forceCompact);

static void writeObj(QByteArray &out, const QJsonObject &obj, int level, bool forceCompact)
{
    if (forceCompact || obj.isEmpty()) {
        out.append(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        return;
    }

    out.append("{\n");
    QStringList keys = obj.keys();
    keys.sort(); // 稳定键序，避免 git diff 出现无意义变化
    for (int i = 0; i < keys.size(); ++i) {
        const QString &k = keys[i];
        writeIndent(out, level + 1);
        out.append(valueToCompact(QJsonValue(k)));
        out.append(": ");
        const bool contentKey = (k == QLatin1String("content"));
        writeVal(out, obj.value(k), level + 1, contentKey);
        if (i < keys.size() - 1)
            out.append(',');
        out.append('\n');
    }
    writeIndent(out, level);
    out.append('}');
}

static void writeArr(QByteArray &out, const QJsonArray &arr, int level, bool forceCompact)
{
    if (forceCompact || arr.isEmpty()) {
        out.append(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        return;
    }

    out.append("[\n");
    for (int i = 0; i < arr.size(); ++i) {
        writeIndent(out, level + 1);
        writeVal(out, arr[i], level + 1, false);
        if (i < arr.size() - 1)
            out.append(',');
        out.append('\n');
    }
    writeIndent(out, level);
    out.append(']');
}

static void writeVal(QByteArray &out, const QJsonValue &val, int level, bool forceCompact)
{
    if (val.isObject()) {
        writeObj(out, val.toObject(), level, forceCompact);
    } else if (val.isArray()) {
        writeArr(out, val.toArray(), level, forceCompact);
    } else {
        out.append(valueToCompact(val));
    }
}

QByteArray LogManager::formatJsonForDiff(const QJsonDocument &doc)
{
    QByteArray out;
    if (doc.isObject()) {
        writeObj(out, doc.object(), 0, false);
    } else if (doc.isArray()) {
        writeArr(out, doc.array(), 0, false);
    }
    out.append('\n');
    return out;
}
