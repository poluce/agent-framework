#pragma once

#include "tools/AbstractBuiltinTool.h"
#include "tools/ToolTypes.h"
#include "logging/LogManager.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class AbstractSession;

class RunCommandTool : public QObject, public AbstractBuiltinTool
{
    Q_OBJECT

public:
    struct ShellConfig
    {
        QString program;
        QStringList arguments;
        QString taskType;
        QString error;
    };

    static ShellConfig resolveShellCommand(const QString &requestedShell,
                                           const QString &defaultShell,
                                           const QString &command);
    static QString findBash();

    /** 剥除进程输出中的 ANSI 转义序列（颜色/样式码；供测试与复用）。 */
    static QString stripAnsi(QString text);

    /// session 仅在需要后台任务通知时传入，前台执行可传 nullptr
    explicit RunCommandTool(QObject *parent = nullptr, AbstractSession *session = nullptr);
    ~RunCommandTool() override;

    // ---- AbstractBuiltinTool 接口 ----
    [[nodiscard]] bool isHeavyweight() const override { return false; }
    [[nodiscard]] ToolSpec spec() const override;
    [[nodiscard]] QString progressKind() const override { return QStringLiteral("running_command"); }

    // ---- 前台执行 ----
    void executeForeground(const ToolCall &call,
                           const QString &commandWorkdir,
                           const std::function<void(const ToolResult &)> &callback);
    void cancel();
    [[nodiscard]] bool isRunning() const;

    // ---- 后台执行 ----
    std::optional<ToolResult> handleBackgroundCommand(const QString &agentId,
                                                      const ToolCall &call,
                                                      const QString &workingDirectory);

    // ---- 配置 ----
    void setDefaultShell(const QString &shell);
    [[nodiscard]] QString defaultShell() const;
    [[nodiscard]] QStringList availableShells() const;
    void setLogContext(const AgentLogContext &logContext);
    void setSession(AbstractSession *session) { m_session = session; }

    // ---- 统一入口 (前台/后台自动分发) ----
    void executeCommand(const QString &agentId,
                        const ToolCall &call,
                        const QString &workingDirectory,
                        const std::function<void(const ToolResult &)> &callback);

signals:
    void progress(const QString &toolUseId,
                  const QString &progressKind,
                  const QString &message);

private:
    // 前台进程
    void executeProcess(const QString &program,
                        const QStringList &arguments,
                        const QString &workdir,
                        const ToolCall &call,
                        const std::function<void(const ToolResult &)> &callback,
                        int timeoutMs);

    // 后台任务
    struct BackgroundTask
    {
        QString id;
        QString toolName;
        QString taskType;
        QString status = QStringLiteral("running");
        QString description;
        QString command;
        QString workingDirectory;
        QString outputPath;
        QString stdoutText;
        QString stderrText;
        bool interrupted = false;
        bool notified = false;
        int exitCode = 0;
        QPointer<QProcess> process;
    };
    QString allocateBackgroundTaskId();
    QString backgroundTaskOutputDirectory() const;

    // 前台状态
    QPointer<QProcess> m_foregroundProcess;
    bool m_cancelRequested = false;

    // 后台状态
    AbstractSession *m_session = nullptr;
    QHash<QString, BackgroundTask> m_backgroundTasks;
    int m_nextBackgroundTaskIndex = 1;

    // 共享
#ifdef Q_OS_WIN
    QString m_defaultShell = QStringLiteral("pwsh");
#else
    QString m_defaultShell = QStringLiteral("bash");
#endif
    AgentLogContext m_logContext;
};
