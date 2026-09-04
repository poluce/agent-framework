#pragma once

#include "ToolTypes.h"
#include "logging/LogManager.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QReadWriteLock>

#include <functional>
#include <memory>

class AbstractBuiltinTool;
class AbstractSession;
class RunCommandTool;
class WriteCoordinator;

class BuiltinToolRuntime : public QObject
{
    Q_OBJECT

public:
    using Completion = std::function<void(ToolResult)>;

    struct ReadFileState
    {
        qint64 timestampMs = 0;
        bool partialView = false;
        int offset = -1;
        int limit = -1;
        QString content;
    };

    explicit BuiltinToolRuntime(QObject *parent = nullptr);
    ~BuiltinToolRuntime() override;

    // ---- 执行 ----
    void execute(const QString &agentId,
                 const ToolCall &call,
                 const QString &workingDirectory,
                 std::shared_ptr<AbstractBuiltinTool> tool,
                 Completion completion);
    void cancelActiveExecution();
    [[nodiscard]] bool isRunning() const;

    // ---- 配置 ----
    void setDefaultShell(const QString &shell);
    [[nodiscard]] QString defaultShell() const;
    [[nodiscard]] QStringList availableShells() const;
    void setLogContext(const AgentLogContext &logContext);
    void clearReadFileStates();
    void setResultStoreDirectory(const QString &directoryPath);
    void setSession(AbstractSession *session);
    /** 接入会话级写协调器（per-file 互斥）；nullptr 退化为无协调（零开销）。 */
    void setWriteCoordinator(WriteCoordinator *coordinator);

    // ---- 读缓存 ----
    ReadFileState readFileState(const QString &filePath) const;
    void setReadFileState(const QString &filePath, const ReadFileState &state);
    /** 使某文件的读缓存失效（跨 Agent 广播用）：下次写按「未读」要求重新读。 */
    void invalidateReadFileState(const QString &filePath);

    // ---- 访问子组件 ----
    RunCommandTool *runCommandTool() const { return m_runCommandTool.get(); }
    AgentLogContext logContext() const { return m_logContext; }

    // ---- 静态工具方法 ----
    static QString normalizeWorkspacePath(const QString &workingDirectory);
    static QString resolveWorkspacePath(const QString &workingDirectory,
                                        const QString &rawPath,
                                        QString *errorMessage);
    static QString summarizeToolCall(const ToolCall &call);

    // ---- 结果工厂 ----
    static ToolResult makeErrorResult(const ToolCall &call, const QString &message,
                                      const AgentLogContext &logContext = {});
    static ToolResult makeRejectedResult(const ToolCall &call, const QString &message,
                                         const AgentLogContext &logContext = {});
    static ToolResult makeCanceledResult(const ToolCall &call, const QString &message,
                                         const AgentLogContext &logContext = {});
    static ToolResult makeSuccessResult(const ToolCall &call,
                                        const QString &text,
                                        const QString &payloadType = {},
                                        const QJsonObject &payload = {});
    bool validateWriteAccess(const QString &filePath,
                             const QString &workspaceRoot,
                             const ToolCall &call,
                             const std::function<void(const ToolResult &)> &callback,
                             const QString &verb = QStringLiteral("writing to"),
                             bool exemptReadRequirement = false);

signals:
    void toolProgress(const QString &toolUseId,
                      const QString &progressKind,
                      const QString &message);

private:
    void executeImmediate(const QString &agentId,
                          const ToolCall &call,
                          const QString &workingDirectory,
                          std::shared_ptr<AbstractBuiltinTool> tool,
                          const std::function<void(const ToolResult &)> &callback);

    bool m_running = false;
    QString m_resultStoreDirectory;
    QHash<QString, ReadFileState> m_readFileStates;
    mutable QReadWriteLock m_cacheLock;
    AgentLogContext m_logContext;
    WriteCoordinator *m_writeCoordinator = nullptr;

    std::unique_ptr<RunCommandTool> m_runCommandTool;
};
