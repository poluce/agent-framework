#include "ScriptToolSource.h"

#include "tools/AbstractSession.h"
#include "tools/AbstractUnit.h"
#include "logging/LogManager.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#include <functional>
#include <utility>

namespace {

constexpr int kManifestScanLines = 20;
constexpr qint64 kManifestScanBytes = 4096;

ToolResult okResult(const QString &toolName, const QString &text, const QString &toolUseId)
{
    ToolResult tr;
    tr.toolName = toolName;
    tr.toolUseId = toolUseId;
    tr.success = true;
    tr.isError = false;
    tr.category = ToolResultCategory::Success;
    tr.text = text;
    return tr;
}

ToolResult errorResult(const QString &toolName, const QString &text, const QString &toolUseId)
{
    ToolResult tr;
    tr.toolName = toolName;
    tr.toolUseId = toolUseId;
    tr.success = false;
    tr.isError = true;
    tr.category = ToolResultCategory::Error;
    tr.text = text;
    return tr;
}

} // namespace

// ====================================================================
// ScriptProcess — 单脚本长驻进程（JSON 行协议）
// ====================================================================

class ScriptToolSource::ScriptProcess : public QObject
{
public:
    using ResultCallback = std::function<void(ToolResult)>;
    using EventCallback = std::function<void(const QJsonObject &)>;

    ScriptProcess(const ScriptTool &tool, const QString &runtimeCommand, QObject *parent)
        : QObject(parent)
        , m_tool(tool)
        , m_runtimeCommand(runtimeCommand)
        , m_idleTimer(this)
    {
        m_idleTimer.setSingleShot(true);
        connect(&m_idleTimer, &QTimer::timeout, this, [this] { stop(); });
        connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] { handleStdout(); });
        connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
            const QByteArray err = m_process.readAllStandardError();
            appendStderr(err);
            LOGD(LogCat::Tool) << "脚本 stderr"
                << logf("tool", m_tool.spec.name)
                << QString::fromUtf8(err).trimmed();
        });
        connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
            failPending(formatErrorWithStderr(QStringLiteral("脚本进程错误（%1）").arg(int(err))));
            for (QTimer *t : std::as_const(m_invokeTimers)) {
                t->deleteLater();
            }
            m_invokeTimers.clear();
            m_idleTimer.stop();
        });
        connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int exitCode, QProcess::ExitStatus) {
            drainStdout();
            failPending(formatErrorWithStderr(QStringLiteral("脚本进程退出（exit=%1）").arg(exitCode)));
            for (QTimer *t : std::as_const(m_invokeTimers)) {
                t->deleteLater();
            }
            m_invokeTimers.clear();
            m_idleTimer.stop();
        });
    }

    ~ScriptProcess() override
    {
        // QProcess 析构会 waitForFinished 并派发事件；先断开，避免回调触碰已析构成员。
        disconnect(&m_process, nullptr, this, nullptr);
        m_idleTimer.stop();
        for (QTimer *t : std::as_const(m_invokeTimers)) {
            t->deleteLater();
        }
        m_invokeTimers.clear();
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
        }
    }

    void start()
    {
        m_process.setWorkingDirectory(QFileInfo(m_tool.filePath).absolutePath());
        m_process.start(m_runtimeCommand, {m_tool.filePath});
    }

    void stop()
    {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
        }
        m_idleTimer.stop();
    }

    /// 失败全部在途调用（工具被删/注销时；避免调用方永久挂起）。
    void failAllPending(const QString &reason) { failPending(reason); }

    bool isRunning() const { return m_process.state() != QProcess::NotRunning; }

    void invoke(const QString &callId, const QJsonObject &args,
                const QString &workingDirectory, ResultCallback done)
    {
        if (m_process.state() == QProcess::NotRunning) {
            start();
        }
        if (m_pending.isEmpty()) {
            m_stderrBuffer.clear();
        }
        const QString reqId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_pending.insert(reqId, PendingInvoke{callId, std::move(done)});

        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, reqId, timer]() {
            m_invokeTimers.remove(reqId);
            timer->deleteLater();
            const auto it = m_pending.find(reqId);
            if (it == m_pending.end()) {
                return;
            }
            PendingInvoke pending = it.value();
            m_pending.erase(it);
            const QString reason = formatErrorWithStderr(
                QStringLiteral("脚本工具调用超时（%1）").arg(m_tool.spec.name));
            pending.callback(errorResult(m_tool.spec.name, reason, pending.callId));
            failPending(reason);
            stop();
        });
        timer->start(m_invokeTimeoutMs);
        m_invokeTimers.insert(reqId, timer);

        QJsonObject req;
        req.insert(QStringLiteral("type"), QStringLiteral("invoke"));
        req.insert(QStringLiteral("id"), reqId);
        req.insert(QStringLiteral("callId"), callId);
        req.insert(QStringLiteral("tool"), m_tool.spec.name);
        req.insert(QStringLiteral("args"), args);
        if (!workingDirectory.isEmpty()) {
            req.insert(QStringLiteral("workingDirectory"), workingDirectory);
        }
        sendLine(req);
        resetIdleTimer();
    }

    void setEventCallback(EventCallback cb) { m_eventCallback = std::move(cb); }
    void setIdleTimeoutMs(int ms) { m_idleTimeoutMs = ms; }
    void setInvokeTimeoutMs(int ms) { m_invokeTimeoutMs = ms; }

    void resetIdleTimer()
    {
        if (m_tool.pushMode || m_idleTimeoutMs <= 0) {
            m_idleTimer.stop();
            return;
        }
        m_idleTimer.start(m_idleTimeoutMs);
    }

private:
    struct PendingInvoke
    {
        QString callId;
        ResultCallback callback;
    };

    void handleStdout()
    {
        m_buffer += m_process.readAllStandardOutput();
        consumeCompleteLines();
    }

    /// 退出前把 stdout 剩余半行也收掉（一次性脚本常不打末尾换行）。
    void drainStdout()
    {
        handleStdout();
        const QByteArray rest = m_buffer.trimmed();
        m_buffer.clear();
        if (!rest.isEmpty()) {
            handleJsonLine(rest);
        }
    }

    void appendStderr(const QByteArray &data)
    {
        constexpr int kMaxStderrBytes = 16384;
        m_stderrBuffer += data;
        if (m_stderrBuffer.size() > kMaxStderrBytes) {
            m_stderrBuffer = m_stderrBuffer.right(kMaxStderrBytes);
        }
    }

    void drainStderr()
    {
        const QByteArray rest = m_process.readAllStandardError();
        if (!rest.isEmpty()) {
            appendStderr(rest);
        }
    }

    QString formatErrorWithStderr(const QString &baseMessage)
    {
        drainStderr();
        const QString stderrText = QString::fromUtf8(m_stderrBuffer).trimmed();
        if (stderrText.isEmpty()) {
            return baseMessage;
        }
        return QStringLiteral("%1：\n%2").arg(baseMessage, stderrText);
    }

    void consumeCompleteLines()
    {
        int nl = 0;
        while ((nl = m_buffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_buffer.left(nl).trimmed();
            m_buffer.remove(0, nl + 1);
            if (!line.isEmpty()) {
                handleJsonLine(line);
            }
        }
    }

    void handleJsonLine(const QByteArray &line)
    {
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            LOGW(LogCat::Tool) << "脚本输出非 JSON 行，忽略"
                << logf("tool", m_tool.spec.name)
                << QString::fromUtf8(line.left(200));
            return;
        }
        const QJsonObject obj = doc.object();
        const QString type = obj.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("result")) {
            handleResult(obj);
            return;
        }
        if (type == QStringLiteral("event")) {
            if (m_eventCallback) {
                m_eventCallback(obj);
            }
            return;
        }
        // sync：无 type 的一行 JSON 视为这次调用的结果。push 仍要信封。
        if (type.isEmpty() && !m_tool.pushMode) {
            handleResult(obj);
            return;
        }
        failProtocolMismatch();
    }

    void handleResult(const QJsonObject &obj)
    {
        const QString reqId = obj.value(QStringLiteral("id")).toString();
        auto it = m_pending.find(reqId);
        if (it == m_pending.end()
            && reqId.isEmpty()
            && !m_tool.pushMode
            && m_pending.size() == 1) {
            it = m_pending.begin();
        }
        if (it == m_pending.end()) {
            failProtocolMismatch();
            return;
        }
        PendingInvoke pending = it.value();
        const QString matchedId = it.key();
        m_pending.erase(it);
        if (QTimer *t = m_invokeTimers.take(matchedId)) {
            t->deleteLater();
        }

        ToolResult tr;
        tr.toolName = m_tool.spec.name;
        tr.toolUseId = pending.callId;
        const bool ok = obj.value(QStringLiteral("ok")).toBool(false);
        if (ok) {
            tr.success = true;
            tr.isError = false;
            tr.category = ToolResultCategory::Success;
            tr.text = obj.value(QStringLiteral("text")).toString();
            tr.payload = obj.value(QStringLiteral("structured")).toObject();
        } else {
            tr.success = false;
            tr.isError = true;
            tr.category = ToolResultCategory::Error;
            tr.text = obj.value(QStringLiteral("error")).toString();
            if (tr.text.isEmpty()) {
                tr.text = obj.value(QStringLiteral("text")).toString();
            }
            drainStderr();
            const QString stderrText = QString::fromUtf8(m_stderrBuffer).trimmed();
            if (!stderrText.isEmpty()) {
                if (!tr.text.isEmpty()) {
                    tr.text += QStringLiteral("：\n") + stderrText;
                } else {
                    tr.text = stderrText;
                }
            }
        }
        pending.callback(std::move(tr));
    }

    void failProtocolMismatch()
    {
        failPending(formatErrorWithStderr(QStringLiteral("需要 type=result 且带回请求 id")));
    }

    void failPending(const QString &reason)
    {
        if (m_pending.isEmpty()) {
            return;
        }
        const auto pending = m_pending.values();
        m_pending.clear();
        for (QTimer *t : std::as_const(m_invokeTimers)) {
            t->deleteLater();
        }
        m_invokeTimers.clear();
        for (const PendingInvoke &p : pending) {
            p.callback(errorResult(m_tool.spec.name, reason, p.callId));
        }
    }

    void sendLine(const QJsonObject &obj)
    {
        m_process.write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
    }

    ScriptTool m_tool;
    QString m_runtimeCommand;
    QProcess m_process;
    QByteArray m_buffer;
    QByteArray m_stderrBuffer;
    QHash<QString, PendingInvoke> m_pending;
    QHash<QString, QTimer *> m_invokeTimers;
    QTimer m_idleTimer;
    EventCallback m_eventCallback;
    int m_idleTimeoutMs = 60000;
    int m_invokeTimeoutMs = 60000;
};

// ====================================================================
// ScriptToolSource
// ====================================================================

ScriptToolSource::ScriptToolSource(QObject *parent)
    : AbstractToolSource(parent)
{
    m_runtimeCommands.insert(QStringLiteral("py"), QStringLiteral("python3"));
    m_runtimeCommands.insert(QStringLiteral("js"), QStringLiteral("node"));
    m_runtimeCommands.insert(QStringLiteral("ts"), QStringLiteral("ts-node"));
}

ScriptToolSource::~ScriptToolSource()
{
    sessionClosing();
}

void ScriptToolSource::sessionClosing()
{
    resetSessionState();
    m_session = nullptr;
}

void ScriptToolSource::sessionCleared()
{
    resetSessionState();
}

void ScriptToolSource::resetSessionState()
{
    for (ScriptProcess *proc : std::as_const(m_processes)) {
        proc->stop();
    }
    m_processes.clear();
    m_subscribers.clear();
    // 临时/会话工具：注销并删文件（项目级与全局级持久工具保留，重启扫描加载）
    for (auto it = m_tools.begin(); it != m_tools.end();) {
        if (it->ephemeral || it->scope == QStringLiteral("session")) {
            QFile::remove(it->filePath);
            it = m_tools.erase(it);
        } else {
            ++it;
        }
    }
}

void ScriptToolSource::setToolDirectory(const QString &dir)
{
    m_toolDir = dir;
    rescan();
}

void ScriptToolSource::setGlobalDirectory(const QString &dir)
{
    setToolDirectory(dir);
}

void ScriptToolSource::setProjectDirectory(const QString &dir)
{
    m_projectDir = dir;
    rescan();
}

void ScriptToolSource::setEphemeralDirectory(const QString &dir)
{
    m_ephemeralDir = dir;
    rescan();
}

void ScriptToolSource::setRuntimeCommand(const QString &language, const QString &command)
{
    m_runtimeCommands.insert(language, command);
}

void ScriptToolSource::setIdleTimeoutMs(int ms)
{
    m_idleTimeoutMs = ms;
}

void ScriptToolSource::setInvokeTimeoutMs(int ms)
{
    m_invokeTimeoutMs = ms;
}

QList<ToolSpec> ScriptToolSource::specs() const
{
    QList<ToolSpec> out;
    out.append(createToolSpec());
    out.append(deleteToolSpec());
    out.append(inspectToolSpec());
    QStringList names = m_tools.keys();
    names.sort();
    for (const QString &name : names) {
        ToolSpec spec = m_tools.value(name).spec;
        const ScriptTool &tool = m_tools.value(name);
        const QString modeStr = tool.pushMode ? QStringLiteral("push") : QStringLiteral("sync");
        if (!spec.description.startsWith(QStringLiteral("[自建工具"))) {
            spec.description = QStringLiteral("[自建工具 | 作用域: %1 | %2/%3] %4（可通过 inspect_tool 查看源码，create_tool 覆盖更新，delete_tool 删除）")
                .arg(tool.scope, tool.language, modeStr, spec.description.trimmed());
        }
        out.append(spec);
    }
    return out;
}

bool ScriptToolSource::owns(const QString &toolName) const
{
    const QString name = toolName.trimmed();
    return name == QStringLiteral("create_tool")
        || name == QStringLiteral("delete_tool")
        || name == QStringLiteral("inspect_tool")
        || m_tools.contains(name);
}

void ScriptToolSource::invoke(const ToolCall &call, const ToolInvokeContext &ctx, Completion done)
{
    const QString name = call.toolName.trimmed();
    if (name == QStringLiteral("create_tool")) {
        handleCreateTool(call, ctx, std::move(done));
        return;
    }
    if (name == QStringLiteral("delete_tool")) {
        handleDeleteTool(call, ctx, std::move(done));
        return;
    }
    if (name == QStringLiteral("inspect_tool")) {
        handleInspectTool(call, ctx, std::move(done));
        return;
    }
    if (!m_tools.contains(name)) {
        done(errorResult(name, QStringLiteral("脚本工具不存在：%1").arg(name), call.id));
        return;
    }
    m_session = ctx.session;
    m_subscribers.insert(name, ctx.agentId);
    ScriptProcess *proc = processFor(name);
    if (!proc) {
        done(errorResult(name, QStringLiteral("脚本进程启动失败（运行时未配置或命令不可用）"), call.id));
        return;
    }
    proc->invoke(call.id, call.input, ctx.workingDirectory, std::move(done));
}

// ── 元工具 ──

ToolSpec ScriptToolSource::createToolSpec()
{
    ToolSpecBuilder b(QStringLiteral("create_tool"),
                      QStringLiteral("创建或覆盖更新自建脚本工具：写入脚本文件并注册，后续轮次即可调用。更新已有工具前建议先使用 inspect_tool 查看当前源码。"),
                      ToolPermissionKind::Write);
    b.requiredInput(QStringLiteral("name"), QStringLiteral("string"),
                    QStringLiteral("工具名（字母/数字/下划线，不能以数字开头）"));
    b.requiredInput(QStringLiteral("description"), QStringLiteral("string"),
                    QStringLiteral("工具描述（给模型的说明）"));
    b.requiredInput(QStringLiteral("code"), QStringLiteral("string"),
                    QStringLiteral("脚本代码。请求 JSON 包含 args（调用入参）与 workingDirectory（当前工作区路径；访问相对路径或工程文件时务必读取或切换至此路径）。"
                                   "sync（缺省）：stdin 读一行 JSON 请求，stdout 写一行 JSON 结果即可"
                                   "（推荐带 type=result 和请求 id；无 type 时视为这次调用的结果）。"
                                   "push：长驻，请求 type=invoke，回复必须 type=result 且带回同一 id，事件 type=event"));
    b.input(QStringLiteral("language"), QStringLiteral("string"),
            QStringLiteral("py / js / ts，缺省 py"));
    b.input(QStringLiteral("mode"), QStringLiteral("string"),
            QStringLiteral("sync（同步返回，一行结果即可）/ push（常驻，须用 invoke/result/event 信封），缺省 sync"));
    b.input(QStringLiteral("scope"), QStringLiteral("string"),
            QStringLiteral("作用域：project（项目级，存入当前工作区 .agent/tools，随工程持久；缺省）/ global（全局级，存入用户全局目录，所有工程共享）/ session（会话临时，会话结束自动销毁）"));
    b.input(QStringLiteral("input_schema"), QStringLiteral("object"),
            QStringLiteral("工具入参 JSON Schema"));
    b.input(QStringLiteral("ephemeral"), QStringLiteral("boolean"),
            QStringLiteral("true=临时工具（等价于 scope=session），缺省 false（优先推荐使用 scope）"));
    return b.build();
}

ToolSpec ScriptToolSource::deleteToolSpec()
{
    ToolSpecBuilder b(QStringLiteral("delete_tool"),
                      QStringLiteral("删除或暂停自建脚本工具（暂停=保留文件但注销）。可通过 inspect_tool 确认已有自建工具。"),
                      ToolPermissionKind::Write);
    b.requiredInput(QStringLiteral("name"), QStringLiteral("string"),
                    QStringLiteral("要删除的脚本工具名"));
    b.input(QStringLiteral("keep_file"), QStringLiteral("boolean"),
            QStringLiteral("true=只注销不删文件（暂停），缺省 false"));
    return b.build();
}

ToolSpec ScriptToolSource::inspectToolSpec()
{
    ToolSpecBuilder b(QStringLiteral("inspect_tool"),
                      QStringLiteral("检视自建脚本工具：查看自建工具清单或读取指定工具的完整源码，以便评估逻辑或进行修改改造。"),
                      ToolPermissionKind::ReadOnly);
    b.input(QStringLiteral("name"), QStringLiteral("string"),
            QStringLiteral("要查看的自建工具名。留空则列出当前所有自建工具的摘要清单。"));
    return b.build();
}

void ScriptToolSource::handleCreateTool(const ToolCall &call, const ToolInvokeContext &ctx,
                                        Completion done)
{
    const QJsonObject in = call.input;
    const QString name = in.value(QStringLiteral("name")).toString().trimmed();
    const QString description = in.value(QStringLiteral("description")).toString().trimmed();
    const QString code = in.value(QStringLiteral("code")).toString();
    QString language = in.value(QStringLiteral("language")).toString().trimmed();
    QString mode = in.value(QStringLiteral("mode")).toString().trimmed();
    QString scope = in.value(QStringLiteral("scope")).toString().trimmed().toLower();
    const bool ephemeral = in.value(QStringLiteral("ephemeral")).toBool(false);
    const QJsonObject inputSchema = in.value(QStringLiteral("input_schema")).toObject();

    static const QRegularExpression nameRe(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    if (!nameRe.match(name).hasMatch()) {
        done(errorResult(call.toolName,
                         QStringLiteral("工具名非法：%1（须字母/数字/下划线，不能以数字开头）").arg(name),
                         call.id));
        return;
    }
    if (name == QStringLiteral("create_tool")
        || name == QStringLiteral("delete_tool")
        || name == QStringLiteral("inspect_tool")) {
        done(errorResult(call.toolName, QStringLiteral("工具名是保留名：%1").arg(name), call.id));
        return;
    }
    if (code.isEmpty()) {
        done(errorResult(call.toolName, QStringLiteral("code 不能为空"), call.id));
        return;
    }
    if (language.isEmpty()) {
        language = QStringLiteral("py");
    }
    if (language != QStringLiteral("py") && language != QStringLiteral("js")
        && language != QStringLiteral("ts")) {
        done(errorResult(call.toolName,
                         QStringLiteral("language 仅支持 py/js/ts：%1").arg(language),
                         call.id));
        return;
    }
    if (mode.isEmpty()) {
        mode = QStringLiteral("sync");
    }
    if (mode != QStringLiteral("sync") && mode != QStringLiteral("push")) {
        done(errorResult(call.toolName, QStringLiteral("mode 仅支持 sync/push：%1").arg(mode), call.id));
        return;
    }
    if (scope.isEmpty()) {
        scope = ephemeral ? QStringLiteral("session") : QStringLiteral("project");
    }
    if (scope != QStringLiteral("project")
        && scope != QStringLiteral("global")
        && scope != QStringLiteral("session")) {
        done(errorResult(call.toolName,
                         QStringLiteral("scope 仅支持 project/global/session：%1").arg(scope),
                         call.id));
        return;
    }

    QString baseDir;
    if (scope == QStringLiteral("session")) {
        baseDir = m_ephemeralDir;
    } else if (scope == QStringLiteral("global")) {
        baseDir = m_toolDir;
    } else { // project
        if (m_projectDir.isEmpty() && !ctx.workingDirectory.isEmpty()) {
            m_projectDir = QDir(ctx.workingDirectory).filePath(QStringLiteral(".agent/tools"));
        }
        baseDir = m_projectDir;
        if (baseDir.isEmpty()) {
            baseDir = m_toolDir;
            scope = QStringLiteral("global");
        }
    }
    if (baseDir.isEmpty()) {
        done(errorResult(call.toolName,
                         QStringLiteral("存储目录未配置（scope=%1）").arg(scope),
                         call.id));
        return;
    }

    // 同名更新：旧文件（可能在其他 agent 子目录或旧路径）先移除，新文件归当前调用者
    const QString newPath = baseDir + QLatin1Char('/') + ctx.agentId
        + QLatin1Char('/') + name + QLatin1Char('.') + language;
    if (m_tools.contains(name) && m_tools.value(name).filePath != newPath) {
        QFile::remove(m_tools.value(name).filePath);
    }

    const QString subdir = baseDir + QLatin1Char('/') + ctx.agentId;
    if (!QDir().mkpath(subdir)) {
        done(errorResult(call.toolName, QStringLiteral("无法创建工具目录：%1").arg(subdir), call.id));
        return;
    }
    const QString filePath = subdir + QLatin1Char('/') + name + QLatin1Char('.') + language;
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        done(errorResult(call.toolName, QStringLiteral("无法写入工具文件：%1").arg(filePath), call.id));
        return;
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("name"), name);
    manifest.insert(QStringLiteral("description"), description);
    manifest.insert(QStringLiteral("language"), language);
    manifest.insert(QStringLiteral("mode"), mode);
    manifest.insert(QStringLiteral("scope"), scope);
    manifest.insert(QStringLiteral("input_schema"), inputSchema);
    manifest.insert(QStringLiteral("ephemeral"), (scope == QStringLiteral("session")));
    const QByteArray comment = language == QStringLiteral("py") ? "# " : "// ";
    file.write(comment + "@tool "
               + QJsonDocument(manifest).toJson(QJsonDocument::Compact) + "\n");
    file.write(code.toUtf8());
    file.close();

    // 同名更新必须换进程：ScriptProcess 里还是旧脚本映像。
    dropProcess(name, QStringLiteral("工具已更新：%1").arg(name));
    rescan();
    emit toolsChanged();
    done(okResult(call.toolName,
                  QStringLiteral("工具已创建：%1（%2，%3，作用域：%4）")
                      .arg(name, language, mode, scope),
                  call.id));
}

void ScriptToolSource::handleDeleteTool(const ToolCall &call, const ToolInvokeContext &ctx,
                                        Completion done)
{
    const QString name = call.input.value(QStringLiteral("name")).toString().trimmed();
    const bool keepFile = call.input.value(QStringLiteral("keep_file")).toBool(false);
    if (name == QStringLiteral("create_tool")
        || name == QStringLiteral("delete_tool")
        || name == QStringLiteral("inspect_tool")) {
        done(errorResult(call.toolName, QStringLiteral("工具名是保留名：%1").arg(name), call.id));
        return;
    }
    if (!m_tools.contains(name)) {
        done(errorResult(call.toolName, QStringLiteral("脚本工具不存在：%1").arg(name), call.id));
        return;
    }
    const ScriptTool tool = m_tools.value(name);
    dropProcess(name, QStringLiteral("工具已删除：%1").arg(name));
    m_tools.remove(name);
    m_subscribers.remove(name);
    if (!keepFile) {
        QFile::remove(tool.filePath);
    }
    emit toolsChanged();
    done(okResult(call.toolName,
                  keepFile ? QStringLiteral("工具已暂停：%1（文件保留）").arg(name)
                           : QStringLiteral("工具已删除：%1").arg(name),
                  call.id));
}

void ScriptToolSource::handleInspectTool(const ToolCall &call, const ToolInvokeContext &ctx,
                                         Completion done)
{
    Q_UNUSED(ctx);
    const QString name = call.input.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        QStringList names = m_tools.keys();
        names.sort();
        if (names.isEmpty()) {
            done(okResult(call.toolName, QStringLiteral("当前没有任何自建工具。可通过 create_tool 创建。"), call.id));
            return;
        }
        QJsonArray list;
        QStringList textLines;
        textLines << QStringLiteral("### 当前已注册的自建脚本工具（共 %1 个）：").arg(names.size());
        for (const QString &tName : names) {
            const ScriptTool &tool = m_tools.value(tName);
            const QString modeStr = tool.pushMode ? QStringLiteral("push") : QStringLiteral("sync");
            QJsonObject item;
            item.insert(QStringLiteral("name"), tName);
            item.insert(QStringLiteral("language"), tool.language);
            item.insert(QStringLiteral("mode"), modeStr);
            item.insert(QStringLiteral("ephemeral"), tool.ephemeral);
            item.insert(QStringLiteral("description"), tool.spec.description);
            list.append(item);

            textLines << QStringLiteral("- **%1** (作用域: %2, %3, %4%5): %6")
                             .arg(tName, tool.scope, tool.language, modeStr,
                                  tool.ephemeral ? QStringLiteral(", 临时") : QString(),
                                  tool.spec.description);
        }
        textLines << QString();
        textLines << QStringLiteral("提示：调用 inspect_tool(name=\"工具名\") 可查看其完整源码；修改后使用 create_tool 同名覆盖更新。");
        ToolResult tr = okResult(call.toolName, textLines.join(QLatin1Char('\n')), call.id);
        tr.payload = QJsonObject{{QStringLiteral("tools"), list}};
        done(std::move(tr));
        return;
    }

    if (name == QStringLiteral("create_tool")
        || name == QStringLiteral("delete_tool")
        || name == QStringLiteral("inspect_tool")) {
        done(errorResult(call.toolName,
                         QStringLiteral("“%1” 是系统元工具，不是自建脚本工具，不可通过 inspect_tool 查看。").arg(name),
                         call.id));
        return;
    }

    if (!m_tools.contains(name)) {
        done(errorResult(call.toolName,
                         QStringLiteral("自建工具不存在：%1。可通过 inspect_tool() 查看所有已有自建工具。").arg(name),
                         call.id));
        return;
    }

    const ScriptTool tool = m_tools.value(name);
    QFile file(tool.filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        done(errorResult(call.toolName,
                         QStringLiteral("无法读取自建工具文件：%1").arg(tool.filePath),
                         call.id));
        return;
    }
    const QByteArray content = file.readAll();
    file.close();

    // 提取纯代码（剥离第 1 行的 @tool manifest）
    const int firstNewline = content.indexOf('\n');
    const QString code = (firstNewline >= 0)
        ? QString::fromUtf8(content.mid(firstNewline + 1))
        : QString::fromUtf8(content);

    const QString modeStr = tool.pushMode ? QStringLiteral("push") : QStringLiteral("sync");
    QString scopeDesc;
    if (tool.scope == QStringLiteral("project")) {
        scopeDesc = QStringLiteral("项目级（project，存放于当前工作区，仅当前工程有效，可随代码库提交）");
    } else if (tool.scope == QStringLiteral("global")) {
        scopeDesc = QStringLiteral("全局级（global，存放于用户全局目录，所有工程共享，修改将影响全局）");
    } else {
        scopeDesc = QStringLiteral("会话级（session，存放于临时目录，会话结束自动清理）");
    }

    QStringList lines;
    lines << QStringLiteral("### 自建工具: %1").arg(name);
    lines << QStringLiteral("- **作用域**: %1").arg(scopeDesc);
    lines << QStringLiteral("- **存储路径**: `%1`").arg(tool.filePath);
    lines << QStringLiteral("- **语言**: %1 | **模式**: %2").arg(tool.language, modeStr);
    lines << QStringLiteral("- **功能说明**: %1").arg(tool.spec.description);
    if (!tool.spec.inputSchema.isEmpty()) {
        lines << QStringLiteral("- **入参规范 (input_schema)**: `%1`")
                     .arg(QString::fromUtf8(QJsonDocument(tool.spec.inputSchema).toJson(QJsonDocument::Compact)));
    }
    lines << QStringLiteral("- **当前源码**:");
    lines << QStringLiteral("```%1\n%2\n```").arg(tool.language, code.trimmed());
    lines << QStringLiteral("- **修改指引**: 在上述代码基础上修改后，调用 `create_tool(name=\"%1\", code=\"<修改后的代码>\", description=\"%2\", language=\"%3\", mode=\"%4\", scope=\"%5\")` 即可完成热更新。")
                 .arg(name, tool.spec.description, tool.language, modeStr, tool.scope);

    ToolResult tr = okResult(call.toolName, lines.join(QLatin1Char('\n')), call.id);
    QJsonObject payload;
    payload.insert(QStringLiteral("name"), name);
    payload.insert(QStringLiteral("scope"), tool.scope);
    payload.insert(QStringLiteral("filePath"), tool.filePath);
    payload.insert(QStringLiteral("language"), tool.language);
    payload.insert(QStringLiteral("mode"), modeStr);
    payload.insert(QStringLiteral("ephemeral"), tool.ephemeral);
    payload.insert(QStringLiteral("description"), tool.spec.description);
    payload.insert(QStringLiteral("input_schema"), tool.spec.inputSchema);
    payload.insert(QStringLiteral("code"), code);
    tr.payload = payload;
    done(std::move(tr));
}

// ── 目录扫描 ──

void ScriptToolSource::rescan()
{
    m_tools.clear();
    // 优先级：session > project > global
    scanDir(m_ephemeralDir, QStringLiteral("session"));
    scanDir(m_projectDir, QStringLiteral("project"));
    scanDir(m_toolDir, QStringLiteral("global"));
}

void ScriptToolSource::scanDir(const QString &dir, const QString &defaultScope)
{
    if (dir.isEmpty()) {
        return;
    }
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    QStringList filePaths;
    while (it.hasNext()) {
        filePaths.append(it.next());
    }
    filePaths.sort();
    for (const QString &filePath : filePaths) {
        const QString ext = QFileInfo(filePath).suffix();
        if (ext != QStringLiteral("py") && ext != QStringLiteral("js")
            && ext != QStringLiteral("ts")) {
            continue;
        }
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray head = f.read(kManifestScanBytes);
        f.close();
        const QJsonObject manifest = parseManifest(head);
        if (manifest.isEmpty()) {
            continue;
        }
        const QString name = manifest.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || m_tools.contains(name)) {
            continue;
        }
        QString scope = manifest.value(QStringLiteral("scope")).toString().trimmed();
        if (scope.isEmpty()) {
            scope = manifest.value(QStringLiteral("ephemeral")).toBool(false)
                ? QStringLiteral("session")
                : defaultScope;
        }
        ScriptTool tool;
        tool.spec.name = name;
        tool.spec.description = manifest.value(QStringLiteral("description")).toString();
        tool.spec.inputSchema = manifest.value(QStringLiteral("input_schema")).toObject();
        tool.spec.permissionKind = ToolPermissionKind::Write;
        tool.filePath = filePath;
        tool.language = ext;
        tool.scope = scope;
        tool.ephemeral = (scope == QStringLiteral("session"));
        tool.pushMode = manifest.value(QStringLiteral("mode")).toString() == QStringLiteral("push");
        m_tools.insert(name, tool);
    }
}

QJsonObject ScriptToolSource::parseManifest(const QByteArray &head)
{
    const QList<QByteArray> lines = head.split('\n');
    const int limit = qMin(lines.size(), kManifestScanLines);
    for (int i = 0; i < limit; ++i) {
        const QByteArray &line = lines.at(i);
        const int marker = line.indexOf("@tool");
        if (marker < 0) {
            continue;
        }
        const int brace = line.indexOf('{', marker);
        if (brace < 0) {
            continue;
        }
        const int end = line.lastIndexOf('}');
        if (end <= brace) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(line.mid(brace, end - brace + 1));
        if (doc.isObject()) {
            return doc.object();
        }
    }
    return {};
}

// ── 进程与事件 ──

void ScriptToolSource::dropProcess(const QString &name, const QString &reason)
{
    if (ScriptProcess *proc = m_processes.take(name)) {
        proc->failAllPending(reason);
        proc->stop();
        proc->deleteLater();
    }
}

ScriptToolSource::ScriptProcess *ScriptToolSource::processFor(const QString &toolName)
{
    if (ScriptProcess *proc = m_processes.value(toolName)) {
        if (proc->isRunning()) {
            return proc;
        }
        proc->start();
        return proc->isRunning() ? proc : nullptr;
    }
    const ScriptTool tool = m_tools.value(toolName);
    const QString command = m_runtimeCommands.value(tool.language);
    if (command.isEmpty()) {
        return nullptr;
    }
    auto *proc = new ScriptProcess(tool, command, this);
    proc->setEventCallback([this, toolName](const QJsonObject &event) {
        handleEvent(toolName, event);
    });
    proc->setIdleTimeoutMs(m_idleTimeoutMs);
    proc->setInvokeTimeoutMs(m_invokeTimeoutMs);
    proc->start();
    m_processes.insert(toolName, proc);
    return proc->isRunning() ? proc : nullptr;
}

void ScriptToolSource::handleEvent(const QString &toolName, const QJsonObject &event)
{
    if (!m_session) {
        LOGW(LogCat::Tool) << "脚本事件丢弃：无会话"
            << logf("tool", toolName);
        return;
    }
    QString target = event.value(QStringLiteral("targetAgentId")).toString();
    if (target.isEmpty()) {
        target = m_subscribers.value(toolName);
    }
    if (target.isEmpty()) {
        LOGW(LogCat::Tool) << "脚本事件丢弃：无投递目标"
            << logf("tool", toolName);
        return;
    }
    AbstractUnit *unit = m_session->findUnit(target);
    if (!unit) {
        LOGW(LogCat::Tool) << "脚本事件丢弃：单元不存在"
            << logf("tool", toolName)
            << logf("agentId", target);
        return;
    }
    UnitInboxMessage msg;
    msg.fromAgentId = toolName;
    msg.content = event.value(QStringLiteral("text")).toString();
    msg.type = QStringLiteral("tool_event");
    msg.payload = event.value(QStringLiteral("payload")).toObject();
    msg.payload.insert(QStringLiteral("tool"), toolName);
    if (!unit->enqueueInboxMessage(msg)) {
        LOGW(LogCat::Tool) << "脚本事件入队失败（容量/大小超限）"
            << logf("tool", toolName)
            << logf("agentId", target);
    }
}
