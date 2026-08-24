#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMutex>
#include <QFile>
#include <QTextStream>
#include <QByteArray>

#include <utility>

// ======== 日志级别 ========
enum class LogLevel : quint8 {
    Trace   = 0,
    Debug   = 1,
    Info    = 2,
    Warning = 3,
    Error   = 4,
    Fatal   = 5
};

// ======== 日志类别 ========
namespace LogCat {
    inline const QString Agent    = QStringLiteral("agent");
    inline const QString Provider = QStringLiteral("provider");
    inline const QString Config   = QStringLiteral("config");
    inline const QString Storage  = QStringLiteral("storage");
    inline const QString Model    = QStringLiteral("model");
    inline const QString Tool     = QStringLiteral("tool");
    inline const QString Network  = QStringLiteral("network");
    inline const QString Session  = QStringLiteral("session");
    inline const QString System   = QStringLiteral("system");
}

// ======== Agent 日志上下文 ========
struct AgentLogContext {
    QString sessionUuid;
    QString agentId;
    QString agentType;
    QString sessionShortId;

    bool isValid() const { return !sessionShortId.isEmpty() && !agentId.isEmpty() && !agentType.isEmpty(); }
};

// ======== 流式字段（key=value，便于 rg / 流程串联）========
struct LogField {
    QString key;
    QString value;
};

namespace log_detail {

inline QString valueToString(const QString &v) { return v; }
inline QString valueToString(QStringView v) { return v.toString(); }
inline QString valueToString(const char *v) { return QString::fromUtf8(v ? v : ""); }
inline QString valueToString(QLatin1String v) { return QString(v); }
inline QString valueToString(int v) { return QString::number(v); }
inline QString valueToString(uint v) { return QString::number(v); }
inline QString valueToString(long v) { return QString::number(static_cast<qint64>(v)); }
inline QString valueToString(ulong v) { return QString::number(static_cast<quint64>(v)); }
inline QString valueToString(qint64 v) { return QString::number(v); }
inline QString valueToString(quint64 v) { return QString::number(v); }
inline QString valueToString(double v) { return QString::number(v); }
inline QString valueToString(bool v)
{
    return v ? QStringLiteral("true") : QStringLiteral("false");
}

/** 含空格 / '=' / '"' 时加引号，保证字段可机读。 */
inline QString escapeFieldValue(QString value)
{
    if (value.isEmpty()) {
        return QStringLiteral("\"\"");
    }
    const bool needQuote = value.contains(QLatin1Char(' '))
        || value.contains(QLatin1Char('='))
        || value.contains(QLatin1Char('"'))
        || value.contains(QLatin1Char('\n'))
        || value.contains(QLatin1Char('\t'));
    if (!needQuote) {
        return value;
    }
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return QStringLiteral("\"%1\"").arg(value);
}

} // namespace log_detail

/**
 * @brief 结构化字段：`logf("sid", id)` → 消息里的 `sid=...`
 * @note 值会按需加引号；空串写成 `key=""`。
 */
template<typename T>
inline LogField logf(const char *key, const T &value)
{
    return LogField{QString::fromUtf8(key), log_detail::valueToString(value)};
}

template<typename T>
inline LogField logf(const QString &key, const T &value)
{
    return LogField{key, log_detail::valueToString(value)};
}

/**
 * @brief 流式日志记录：析构时落盘
 *
 * 用法：
 *   LOGW(LogCat::System) << "HostBus 命令被拒绝"
 *       << logf("kind", kind) << logf("cmdId", id);
 *   LOGI(LogCat::Agent, logContext()) << "开始 Turn"
 *       << logf("sessionUuid", sessionUuid);
 *
 * 规则：第一条字符串是中文可读消息；其后的字符串拼到正文；LogField 追加为 key=value。
 * 正文用中文，字段键用稳定英文（sid/cmdId/kind…）方便检索。
 */
class LogRecord
{
public:
    LogRecord(LogLevel level, QString category, const char *file, int line)
        : m_level(level)
        , m_category(std::move(category))
        , m_file(file)
        , m_line(line)
    {
    }

    LogRecord(LogLevel level, QString category, AgentLogContext agentCtx,
              const char *file, int line)
        : m_level(level)
        , m_category(std::move(category))
        , m_agentCtx(std::move(agentCtx))
        , m_hasAgentCtx(true)
        , m_file(file)
        , m_line(line)
    {
    }

    ~LogRecord()
    {
        commit();
    }

    LogRecord(const LogRecord &) = delete;
    LogRecord &operator=(const LogRecord &) = delete;

    LogRecord(LogRecord &&other) noexcept
        : m_active(other.m_active)
        , m_level(other.m_level)
        , m_category(std::move(other.m_category))
        , m_agentCtx(std::move(other.m_agentCtx))
        , m_hasAgentCtx(other.m_hasAgentCtx)
        , m_file(other.m_file)
        , m_line(other.m_line)
        , m_event(std::move(other.m_event))
        , m_detail(std::move(other.m_detail))
        , m_fields(std::move(other.m_fields))
    {
        other.m_active = false;
    }

    LogRecord &operator=(LogRecord &&other) noexcept
    {
        if (this != &other) {
            commit();
            m_active = other.m_active;
            m_level = other.m_level;
            m_category = std::move(other.m_category);
            m_agentCtx = std::move(other.m_agentCtx);
            m_hasAgentCtx = other.m_hasAgentCtx;
            m_file = other.m_file;
            m_line = other.m_line;
            m_event = std::move(other.m_event);
            m_detail = std::move(other.m_detail);
            m_fields = std::move(other.m_fields);
            other.m_active = false;
        }
        return *this;
    }

    LogRecord &operator<<(const QString &text)
    {
        if (m_event.isEmpty()) {
            m_event = text;
        } else if (!text.isEmpty()) {
            if (!m_detail.isEmpty()) {
                m_detail += QLatin1Char(' ');
            }
            m_detail += text;
        }
        return *this;
    }

    LogRecord &operator<<(const char *text)
    {
        return (*this) << QString::fromUtf8(text ? text : "");
    }

    LogRecord &operator<<(QStringView text)
    {
        return (*this) << text.toString();
    }

    LogRecord &operator<<(const LogField &field)
    {
        if (field.key.isEmpty()) {
            return *this;
        }
        m_fields.append(field.key + QLatin1Char('=')
                        + log_detail::escapeFieldValue(field.value));
        return *this;
    }

private:
    void commit();

    bool m_active = true;
    LogLevel m_level = LogLevel::Info;
    QString m_category;
    AgentLogContext m_agentCtx;
    bool m_hasAgentCtx = false;
    const char *m_file = nullptr;
    int m_line = -1;
    QString m_event;
    QString m_detail;
    QStringList m_fields;
};

// ======== LogManager ========
class LogManager : public QObject
{
    Q_OBJECT

public:
    static LogManager &instance();

    /// 打开文件日志。logDir 为空则不写文件、不回落本产品 AppData。
    void init(const QString &logDir = QString());
    void shutdown();

    // --- 级别控制 ---
    void setConsoleOutputEnabled(bool enabled);

    // --- 文件控制 ---
    void setFileMaxSize(qint64 bytes) { m_maxFileSize = bytes; }
    void setMaxBackupFiles(int count) { m_maxBackupFiles = count; }
    void setDiagnosticMode(bool enabled);
    bool diagnosticMode() const;

    /// 请求体 git 仓库根。空则 saveRequestBody 为空操作。由组合根注入。
    void setRequestBodiesDirectory(const QString &dir);
    QString requestBodiesDirectory() const;

    // --- 写入（底层；业务代码请用 LOGI/LOGW/… 流式宏）---
    void write(LogLevel level, const QString &category, const QString &message,
               const char *file = nullptr, int line = -1);
    void write(LogLevel level, const QString &category, const QString &message,
               const AgentLogContext &agentCtx,
               const char *file = nullptr, int line = -1);

    // --- 请求体记录 ---
    void saveRequestBody(const QString &provider,
                         const QByteArray &body,
                         const AgentLogContext &agentCtx);

    // --- Qt 全局消息处理器 ---
    static void qtMessageHandler(QtMsgType type,
                                 const QMessageLogContext &ctx,
                                 const QString &msg);

    QString logFilePath() const { return m_logFilePath; }

private:
    LogManager();
    ~LogManager() override;
    Q_DISABLE_COPY(LogManager)

    QString formatMessage(LogLevel level, const QString &category,
                          const QString &message, const QString &fileName,
                          int line,
                          const AgentLogContext *agentCtx) const;
    void writeImpl(LogLevel level,
                   const QString &category,
                   const QString &message,
                   const AgentLogContext *agentCtx,
                   const char *file,
                   int line);
    void writeToConsole(const QString &formatted, LogLevel level);
    void writeToFile(const QString &formatted);
    void rotateIfNeeded();
    void checkDateChange();
    bool shouldWriteToFile(LogLevel level, const QString &message) const;
    static bool isDiagnosticMessage(const QString &message);

    // 请求体记录
    QString ensureRequestRepo(const QString &sessionUuid);
    static QString extractUserMessage(const QByteArray &body, const QString &provider);
    static int extractToolCount(const QByteArray &body, const QString &provider);
    static QString formatCommitMessage(const QString &provider,
                                       const QString &agentType,
                                       int bodySize, int toolCount,
                                       const QString &userMsg);
    static QByteArray formatJsonForDiff(const QJsonDocument &doc);

    static QString levelPrefix(LogLevel level);
    static const char *levelAnsiColor(LogLevel level);
    static QString stripPath(const QString &filePath);

    mutable QMutex m_mutex;
    LogLevel m_level = LogLevel::Debug;

    // 文件输出
    QFile m_logFile;
    QTextStream m_fileStream;
    QString m_logDir;
    QString m_logFilePath;
    QString m_requestBodiesDir;
    QString m_currentDateKey;   // 当前文件对应的日期 key (yyyyMMdd)
    qint64 m_maxFileSize = 10 * 1024 * 1024; // 10 MB
    int m_maxBackupFiles = 5;
    bool m_diagnosticMode = false;
    bool m_consoleOutputEnabled = true;

    bool m_initialized = false;
    QtMessageHandler m_oldHandler = nullptr;
};

// ======== 流式便捷宏 ========
// LOGI(cat) << "中文说明" << logf("k", v);
// LOGI(cat, agentCtx) << "中文说明" << logf("k", v);
// 正文用中文；字段键用稳定英文（sid/cmdId/kind…）方便检索。
#define LOGT(cat, ...) LogRecord(LogLevel::Trace, cat, ##__VA_ARGS__, __FILE__, __LINE__)
#define LOGD(cat, ...) LogRecord(LogLevel::Debug, cat, ##__VA_ARGS__, __FILE__, __LINE__)
#define LOGI(cat, ...) LogRecord(LogLevel::Info, cat, ##__VA_ARGS__, __FILE__, __LINE__)
#define LOGW(cat, ...) LogRecord(LogLevel::Warning, cat, ##__VA_ARGS__, __FILE__, __LINE__)
#define LOGE(cat, ...) LogRecord(LogLevel::Error, cat, ##__VA_ARGS__, __FILE__, __LINE__)
#define LOGF(cat, ...) LogRecord(LogLevel::Fatal, cat, ##__VA_ARGS__, __FILE__, __LINE__)

// 临时插桩（定位用，非系统固有日志）
// 复用 LOGD；正文固定前缀 [probe]。修完即删：rg LOG_PROBE / rg "\[probe\]"
// 用法：LOG_PROBE(LogCat::System) << "进入 placeSession" << logf("sid", id);
#define LOG_PROBE(cat, ...) LOGD(cat, ##__VA_ARGS__) << QStringLiteral("[probe]")
