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

ToolResult okResult(const QString &toolName, const QString &text)
{
    ToolResult tr;
    tr.toolName = toolName;
    tr.success = true;
    tr.isError = false;
    tr.category = ToolResultCategory::Success;
    tr.text = text;
    return tr;
}

ToolResult errorResult(const QString &toolName, const QString &text)
{
    ToolResult tr;
    tr.toolName = toolName;
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
            LOGD(LogCat::Tool) << "脚本 stderr"
                << logf("tool", m_tool.spec.name)
                << QString::fromUtf8(m_process.readAllStandardError()).trimmed();
        });
        connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
            failPending(QStringLiteral("脚本进程错误（%1）").arg(int(err)));
            for (QTimer *t : std::as_const(m_invokeTimers)) {
                t->deleteLater();
            }
            m_invokeTimers.clear();
            m_idleTimer.stop();
        });
        connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int exitCode, QProcess::ExitStatus) {
            failPending(QStringLiteral("脚本进程退出（exit=%1）").arg(exitCode));
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
        const QString reqId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_pending.insert(reqId, PendingInvoke{callId, std::move(done)});

        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, reqId, timer]() {
            m_pending.remove(reqId);
            m_invokeTimers.remove(reqId);
            timer->deleteLater();
            failPending(QStringLiteral("脚本工具调用超时（%1）").arg(m_tool.spec.name));
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
        int nl = 0;
        while ((nl = m_buffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_buffer.left(nl).trimmed();
            m_buffer.remove(0, nl + 1);
            if (line.isEmpty()) {
                continue;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) {
                LOGW(LogCat::Tool) << "脚本输出非 JSON 行，忽略"
                    << logf("tool", m_tool.spec.name)
                    << QString::fromUtf8(line.left(200));
                continue;
            }
            const QJsonObject obj = doc.object();
            const QString type = obj.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("result")) {
                handleResult(obj);
            } else if (type == QStringLiteral("event")) {
                if (m_eventCallback) {
                    m_eventCallback(obj);
                }
            } else {
                LOGW(LogCat::Tool) << "脚本输出未知消息类型，忽略"
                    << logf("tool", m_tool.spec.name)
                    << logf("type", type);
            }
        }
    }

    void handleResult(const QJsonObject &obj)
    {
        const QString reqId = obj.value(QStringLiteral("id")).toString();
        const auto it = m_pending.find(reqId);
        if (it == m_pending.end()) {
            LOGW(LogCat::Tool) << "脚本返回未知请求 id，忽略"
                << logf("tool", m_tool.spec.name)
                << logf("id", reqId);
            return;
        }
        PendingInvoke pending = it.value();
        m_pending.erase(it);
        if (QTimer *t = m_invokeTimers.take(reqId)) {
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
        }
        pending.callback(std::move(tr));
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
            ToolResult tr = errorResult(m_tool.spec.name, reason);
            tr.toolUseId = p.callId;
            p.callback(std::move(tr));
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
    // 临时工具：注销并删文件（持久工具保留，重启扫描加载）
    for (auto it = m_tools.begin(); it != m_tools.end();) {
        if (it->ephemeral) {
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
    QStringList names = m_tools.keys();
    names.sort();
    for (const QString &name : names) {
        out.append(m_tools.value(name).spec);
    }
    return out;
}

bool ScriptToolSource::owns(const QString &toolName) const
{
    const QString name = toolName.trimmed();
    return name == QStringLiteral("create_tool")
        || name == QStringLiteral("delete_tool")
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
    if (!m_tools.contains(name)) {
        done(errorResult(name, QStringLiteral("脚本工具不存在：%1").arg(name)));
        return;
    }
    m_session = ctx.session;
    m_subscribers.insert(name, ctx.agentId);
    ScriptProcess *proc = processFor(name);
    if (!proc) {
        done(errorResult(name, QStringLiteral("脚本进程启动失败（运行时未配置或命令不可用）")));
        return;
    }
    proc->invoke(call.id, call.input, ctx.workingDirectory, std::move(done));
}

// ── 元工具 ──

ToolSpec ScriptToolSource::createToolSpec()
{
    ToolSpecBuilder b(QStringLiteral("create_tool"),
                      QStringLiteral("创建或更新一个脚本工具：写入脚本文件并注册，后续轮次即可调用。"),
                      ToolPermissionKind::Write);
    b.requiredInput(QStringLiteral("name"), QStringLiteral("string"),
                    QStringLiteral("工具名（字母/数字/下划线，不能以数字开头）"));
    b.requiredInput(QStringLiteral("description"), QStringLiteral("string"),
                    QStringLiteral("工具描述（给模型的说明）"));
    b.requiredInput(QStringLiteral("code"), QStringLiteral("string"),
                    QStringLiteral("脚本代码：从 stdin 读 JSON 行请求，向 stdout 写 JSON 行结果"));
    b.input(QStringLiteral("language"), QStringLiteral("string"),
            QStringLiteral("py / js / ts，缺省 py"));
    b.input(QStringLiteral("mode"), QStringLiteral("string"),
            QStringLiteral("sync（同步返回）/ push（常驻，事件异步推送），缺省 sync"));
    b.input(QStringLiteral("input_schema"), QStringLiteral("object"),
            QStringLiteral("工具入参 JSON Schema"));
    b.input(QStringLiteral("ephemeral"), QStringLiteral("boolean"),
            QStringLiteral("true=临时工具（会话结束删除），缺省 false"));
    return b.build();
}

ToolSpec ScriptToolSource::deleteToolSpec()
{
    ToolSpecBuilder b(QStringLiteral("delete_tool"),
                      QStringLiteral("删除或暂停一个脚本工具（暂停=保留文件但注销）。"),
                      ToolPermissionKind::Write);
    b.requiredInput(QStringLiteral("name"), QStringLiteral("string"),
                    QStringLiteral("要删除的脚本工具名"));
    b.input(QStringLiteral("keep_file"), QStringLiteral("boolean"),
            QStringLiteral("true=只注销不删文件（暂停），缺省 false"));
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
    const bool ephemeral = in.value(QStringLiteral("ephemeral")).toBool(false);
    const QJsonObject inputSchema = in.value(QStringLiteral("input_schema")).toObject();

    static const QRegularExpression nameRe(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    if (!nameRe.match(name).hasMatch()) {
        done(errorResult(call.toolName,
                         QStringLiteral("工具名非法：%1（须字母/数字/下划线，不能以数字开头）").arg(name)));
        return;
    }
    if (name == QStringLiteral("create_tool") || name == QStringLiteral("delete_tool")) {
        done(errorResult(call.toolName, QStringLiteral("工具名是保留名：%1").arg(name)));
        return;
    }
    if (code.isEmpty()) {
        done(errorResult(call.toolName, QStringLiteral("code 不能为空")));
        return;
    }
    if (language.isEmpty()) {
        language = QStringLiteral("py");
    }
    if (language != QStringLiteral("py") && language != QStringLiteral("js")
        && language != QStringLiteral("ts")) {
        done(errorResult(call.toolName,
                         QStringLiteral("language 仅支持 py/js/ts：%1").arg(language)));
        return;
    }
    if (mode.isEmpty()) {
        mode = QStringLiteral("sync");
    }
    if (mode != QStringLiteral("sync") && mode != QStringLiteral("push")) {
        done(errorResult(call.toolName, QStringLiteral("mode 仅支持 sync/push：%1").arg(mode)));
        return;
    }
    const QString baseDir = ephemeral ? m_ephemeralDir : m_toolDir;
    if (baseDir.isEmpty()) {
        done(errorResult(call.toolName,
                         ephemeral ? QStringLiteral("临时工具目录未配置")
                                   : QStringLiteral("工具目录未配置")));
        return;
    }

    // 同名更新：旧文件（可能在其他 agent 子目录）先移除，新文件归当前调用者
    const QString newPath = baseDir + QLatin1Char('/') + ctx.agentId
        + QLatin1Char('/') + name + QLatin1Char('.') + language;
    if (m_tools.contains(name) && m_tools.value(name).filePath != newPath) {
        QFile::remove(m_tools.value(name).filePath);
    }

    const QString subdir = baseDir + QLatin1Char('/') + ctx.agentId;
    if (!QDir().mkpath(subdir)) {
        done(errorResult(call.toolName, QStringLiteral("无法创建工具目录：%1").arg(subdir)));
        return;
    }
    const QString filePath = subdir + QLatin1Char('/') + name + QLatin1Char('.') + language;
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        done(errorResult(call.toolName, QStringLiteral("无法写入工具文件：%1").arg(filePath)));
        return;
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("name"), name);
    manifest.insert(QStringLiteral("description"), description);
    manifest.insert(QStringLiteral("mode"), mode);
    manifest.insert(QStringLiteral("input_schema"), inputSchema);
    manifest.insert(QStringLiteral("ephemeral"), ephemeral);
    const QByteArray comment = language == QStringLiteral("py") ? "# " : "// ";
    file.write(comment + "@tool "
               + QJsonDocument(manifest).toJson(QJsonDocument::Compact) + "\n");
    file.write(code.toUtf8());
    file.close();

    rescan();
    emit toolsChanged();
    done(okResult(call.toolName,
                  QStringLiteral("工具已创建：%1（%2，%3）")
                      .arg(name, language, mode)));
}

void ScriptToolSource::handleDeleteTool(const ToolCall &call, const ToolInvokeContext &ctx,
                                        Completion done)
{
    const QString name = call.input.value(QStringLiteral("name")).toString().trimmed();
    const bool keepFile = call.input.value(QStringLiteral("keep_file")).toBool(false);
    if (name == QStringLiteral("create_tool") || name == QStringLiteral("delete_tool")) {
        done(errorResult(call.toolName, QStringLiteral("工具名是保留名：%1").arg(name)));
        return;
    }
    if (!m_tools.contains(name)) {
        done(errorResult(call.toolName, QStringLiteral("脚本工具不存在：%1").arg(name)));
        return;
    }
    const ScriptTool tool = m_tools.value(name);
    if (ScriptProcess *proc = m_processes.take(name)) {
        proc->failAllPending(QStringLiteral("工具已删除：%1").arg(name));
        proc->stop();
        proc->deleteLater();
    }
    m_tools.remove(name);
    m_subscribers.remove(name);
    if (!keepFile) {
        QFile::remove(tool.filePath);
    }
    emit toolsChanged();
    done(okResult(call.toolName,
                  keepFile ? QStringLiteral("工具已暂停：%1（文件保留）").arg(name)
                           : QStringLiteral("工具已删除：%1").arg(name)));
}

// ── 目录扫描 ──

void ScriptToolSource::rescan()
{
    m_tools.clear();
    scanDir(m_toolDir);
    scanDir(m_ephemeralDir);
}

void ScriptToolSource::scanDir(const QString &dir)
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
        ScriptTool tool;
        tool.spec.name = name;
        tool.spec.description = manifest.value(QStringLiteral("description")).toString();
        tool.spec.inputSchema = manifest.value(QStringLiteral("input_schema")).toObject();
        tool.spec.permissionKind = ToolPermissionKind::Write;
        tool.filePath = filePath;
        tool.language = ext;
        tool.ephemeral = (dir == m_ephemeralDir);
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
