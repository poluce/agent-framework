#include "RunCommandTool.h"

#include "tools/AbstractSession.h"
#include "tools/AbstractUnit.h"
#include "tools/BuiltinToolRuntime.h"
#include "logging/LogManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <memory>

namespace {

constexpr int kCommandTimeoutMs = 60000;

} // namespace

QString RunCommandTool::stripAnsi(QString text)
{
    // 剥 ANSI 转义序列（颜色/样式码；对任意 shell 输出通用，统一兜底）。
    // 覆盖 CSI 序列（ESC [ <params> <final>，如 [32;1m、[?1049h）与
    // OSC 序列（ESC ] ... BEL/ST，如超链接）。
    static const QRegularExpression kAnsiEscape(
        QStringLiteral("\\x1B\\[[0-9;?]*[ -/]*[@-~]|\\x1B\\][^\\x07\\x1B]*(?:\\x07|\\x1B\\\\)"));
    text.remove(kAnsiEscape);
    return text;
}

namespace {

QString sanitizeProcessText(QString text)
{
    text.remove(QChar('\0'));
    return RunCommandTool::stripAnsi(text);
}

void setToolPayload(ToolResult *result, const QString &payloadType, const QJsonObject &payload)
{
    if (!result) return;
    result->payloadType = payloadType;
    result->payload = payload;
}

#ifdef Q_OS_WIN
// PowerShell 默认解析 npm.ps1/npx.ps1，易被 ExecutionPolicy 拦截。
// 只在「命令位置」且引号外改写为 .cmd，避免改动字符串字面量 / 变量赋值内容。
bool isShellTokenChar(const QChar c)
{
    return c.isLetterOrNumber()
        || c == QLatin1Char('_')
        || c == QLatin1Char('.')
        || c == QLatin1Char('-')
        || c == QLatin1Char('\\')
        || c == QLatin1Char('/');
}

// 匹配独立的 npm/npx token，已是 .cmd 则跳过。
int matchWindowsNodeLauncher(const QString &command, int index)
{
    static const QStringList kLaunchers = {
        QStringLiteral("npm"),
        QStringLiteral("npx"),
    };
    for (const QString &name : kLaunchers) {
        if (index + name.size() > command.size()) {
            continue;
        }
        if (command.mid(index, name.size()).compare(name, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const int end = index + name.size();
        if (end + 4 <= command.size()
            && command.mid(end, 4).compare(QStringLiteral(".cmd"), Qt::CaseInsensitive) == 0) {
            return 0;
        }
        if (end < command.size() && isShellTokenChar(command.at(end))) {
            return 0;
        }
        return name.size();
    }
    return 0;
}

QString rewriteWindowsNodeLaunchers(const QString &command)
{
    QString result;
    result.reserve(command.size() + 8);

    enum class Quote : quint8 { None, Single, Double };
    Quote quote = Quote::None;
    bool atCommandPosition = true;
    const int n = command.size();

    for (int i = 0; i < n; ) {
        const QChar c = command.at(i);

        if (quote == Quote::Single) {
            result += c;
            if (c == QLatin1Char('\'')) {
                quote = Quote::None;
            }
            ++i;
            continue;
        }

        if (quote == Quote::Double) {
            result += c;
            // PowerShell 双引号内 ` 转义下一字符
            if (c == QLatin1Char('`') && i + 1 < n) {
                result += command.at(i + 1);
                i += 2;
                continue;
            }
            if (c == QLatin1Char('"')) {
                quote = Quote::None;
            }
            ++i;
            continue;
        }

        // 引号外
        if (c == QLatin1Char('\'')) {
            quote = Quote::Single;
            atCommandPosition = false;
            result += c;
            ++i;
            continue;
        }
        if (c == QLatin1Char('"')) {
            quote = Quote::Double;
            atCommandPosition = false;
            result += c;
            ++i;
            continue;
        }

        // 空白不改变 command position（前导空白后仍是命令）
        if (c.isSpace()) {
            result += c;
            ++i;
            continue;
        }

        // 语句/管道分隔后进入新的命令位置
        if (c == QLatin1Char(';') || c == QLatin1Char('\n') || c == QLatin1Char('\r')
            || c == QLatin1Char('|') || c == QLatin1Char('&')) {
            result += c;
            // 吞掉 && / ||
            if ((c == QLatin1Char('&') || c == QLatin1Char('|'))
                && i + 1 < n && command.at(i + 1) == c) {
                result += command.at(i + 1);
                ++i;
            }
            atCommandPosition = true;
            ++i;
            continue;
        }

        if (atCommandPosition) {
            if (const int matched = matchWindowsNodeLauncher(command, i); matched > 0) {
                result += command.mid(i, matched).toLower() + QStringLiteral(".cmd");
                i += matched;
                atCommandPosition = false;
                continue;
            }
            atCommandPosition = false;
        }

        result += c;
        ++i;
    }
    return result;
}
#endif

} // namespace

// ------------------------------------------------------------------
// 工具规格 (RunCommandTool)
// ------------------------------------------------------------------
ToolSpec RunCommandTool::spec() const
{
    return ToolSpecBuilder("run_command", QStringLiteral("在工作目录内执行 shell 命令。默认使用用户配置的 shell，可通过 shell 参数指定使用的 shell。"), ToolPermissionKind::Command)
        .requiredInput("command", "string", QStringLiteral("要执行的命令"))
        .input("description", "string", QStringLiteral("命令用途说明，可为空"))
        .input("shell", "string", "pwsh/bash/powershell，不填则使用默认")
        .input("runInBackground", "boolean", QStringLiteral("是否后台运行"))
        .input("timeoutMs", "integer", QStringLiteral("超时时间，毫秒，可为空"))
        .input("workdir", "string", QStringLiteral("相对当前工作目录的子目录，可为空"))
        .output("shell", "string", "pwsh/bash/powershell")
        .output("command", "string", QStringLiteral("执行的命令"))
        .output("exitCode", "integer", QStringLiteral("退出码"))
        .output("stdout", "string", QStringLiteral("标准输出"))
        .output("stderr", "string", QStringLiteral("标准错误"))
        .output("timedOut", "boolean", QStringLiteral("是否超时"))
        .output("canceled", "boolean", QStringLiteral("是否取消"))
        .build();
}

// ------------------------------------------------------------------
// Shell 解析 (静态)
// ------------------------------------------------------------------
QString RunCommandTool::findBash()
{
#ifdef Q_OS_WIN
    const QString envPath = qEnvironmentVariable("CLAUDE_CODE_GIT_BASH_PATH");
    if (!envPath.isEmpty() && QFileInfo::exists(envPath)) {
        return QDir::toNativeSeparators(envPath);
    }
    static const QStringList kLocations = {
        QStringLiteral("C:/Program Files/Git/bin/bash.exe"),
        QStringLiteral("C:/Program Files (x86)/Git/bin/bash.exe"),
    };
    for (const QString &loc : kLocations) {
        if (QFileInfo::exists(loc)) {
            return loc;
        }
    }
    const QString gitPath = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (!gitPath.isEmpty()) {
        QDir d = QFileInfo(gitPath).absoluteDir();
        if (d.cd(QStringLiteral("../bin"))) {
            const QString bp = d.absoluteFilePath(QStringLiteral("bash.exe"));
            if (QFileInfo::exists(bp)) {
                return QDir::toNativeSeparators(bp);
            }
            d.cdUp();
        }
        if (d.cd(QStringLiteral("../../bin"))) {
            const QString bp = d.absoluteFilePath(QStringLiteral("bash.exe"));
            if (QFileInfo::exists(bp)) {
                return QDir::toNativeSeparators(bp);
            }
        }
    }
    return {};
#else
    return QStandardPaths::findExecutable(QStringLiteral("bash"));
#endif
}

RunCommandTool::ShellConfig RunCommandTool::resolveShellCommand(
    const QString &requestedShell,
    const QString &defaultShell,
    const QString &command)
{
    ShellConfig config;
    QString resolvedShell = requestedShell.trimmed().toLower();
    if (resolvedShell.isEmpty()) {
        resolvedShell = defaultShell.trimmed().toLower();
    }

    if (resolvedShell == QStringLiteral("powershell") || resolvedShell == QStringLiteral("pwsh")) {
        config.program = QStandardPaths::findExecutable(resolvedShell);
        if (config.program.isEmpty()) {
            config.error = QStringLiteral("未找到 %1 可执行文件。").arg(resolvedShell);
            return config;
        }
        QString shellCommand = command;
#ifdef Q_OS_WIN
        shellCommand = rewriteWindowsNodeLaunchers(command);
#endif
        // 按 shell 版本分治处理 ANSI：对象输出的表头颜色码会污染捕获结果。
        QString prefix = QStringLiteral("[Console]::OutputEncoding=[Text.Encoding]::UTF8;");
        if (resolvedShell == QStringLiteral("pwsh")) {
            // PowerShell 7+：官方开关从源头关颜色（重定向时输出纯文本），不延迟输出
            prefix += QStringLiteral("$PSStyle.OutputRendering='PlainText';");
        } else {
            // PowerShell 5.1：无 $PSStyle；Out-String 强制对象字符串化
            //（表头变纯文本行，不产生 ANSI）；残余 ANSI 由 sanitizeProcessText 兜底
            shellCommand += QStringLiteral(" | Out-String -Width 4096");
        }
        config.arguments = QStringList{
            QStringLiteral("-NoProfile"),
#ifdef Q_OS_WIN
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
#endif
            QStringLiteral("-Command"),
            prefix + shellCommand,
        };
        config.taskType = QStringLiteral("local_%1").arg(resolvedShell);
    } else {
        config.program = findBash();
        if (config.program.isEmpty()) {
            config.program = QStandardPaths::findExecutable(QStringLiteral("bash"));
        }
        if (config.program.isEmpty()) {
            config.error = QStringLiteral("未找到 bash 可执行文件，请改用 PowerShell 或安装 Bash。");
            return config;
        }
        config.arguments = QStringList{
            QStringLiteral("-lc"),
            command,
        };
        config.taskType = QStringLiteral("local_bash");
    }
    return config;
}

// ------------------------------------------------------------------
// 构造 / 析构
// ------------------------------------------------------------------
RunCommandTool::RunCommandTool(QObject *parent, AbstractSession *session)
    : QObject(parent)
    , m_session(session)
{
}

RunCommandTool::~RunCommandTool()
{
    cancel();
}

// ------------------------------------------------------------------
// 配置
// ------------------------------------------------------------------
void RunCommandTool::setDefaultShell(const QString &shell)
{
    const QString normalized = shell.trimmed().toLower();
    if (normalized == QStringLiteral("pwsh") || normalized == QStringLiteral("powershell")) {
        m_defaultShell = normalized;
    } else {
        m_defaultShell = QStringLiteral("bash");
    }

    if (QStandardPaths::findExecutable(m_defaultShell).isEmpty()) {
        LOGW(LogCat::Tool, m_logContext) << "默认 shell 在系统中未找到，执行时可能失败"
            << logf("shell", m_defaultShell);
    }
}

void RunCommandTool::setLogContext(const AgentLogContext &logContext)
{
    m_logContext = logContext;
}

QString RunCommandTool::defaultShell() const
{
    return m_defaultShell;
}

QStringList RunCommandTool::availableShells() const
{
    QStringList shells;

    if (!QStandardPaths::findExecutable(QStringLiteral("powershell")).isEmpty()) {
        shells << QStringLiteral("powershell");
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("pwsh")).isEmpty()) {
        shells << QStringLiteral("pwsh");
    }
    if (!findBash().isEmpty()) {
        shells << QStringLiteral("bash");
    }

    return shells;
}

bool RunCommandTool::isRunning() const
{
    return !m_foregroundProcess.isNull();
}

void RunCommandTool::cancel()
{
    if (m_foregroundProcess) {
        LOGI(LogCat::Tool, m_logContext) << "取消命令执行";
        m_cancelRequested = true;
        m_foregroundProcess->kill();
    }
}

// ------------------------------------------------------------------
// 前台执行
// ------------------------------------------------------------------

namespace {

// 根据进程退出状态构建 ToolResult，消除 finished 信号处理器中的深层嵌套
ToolResult buildProcessResult(bool canceled, bool timedOut, int exitCode,
                               QProcess::ExitStatus exitStatus,
                               const QString &stdoutText, const QString &stderrText,
                               const QString &mergedOutput,
                               const QJsonObject &shellPayload,
                               const ToolCall &call, int timeoutMs,
                               const AgentLogContext &logContext)
{
    ToolResult result;

    if (canceled) {
        LOGI(LogCat::Tool, logContext) << "命令已取消";
        result = BuiltinToolRuntime::makeCanceledResult(
            call, QStringLiteral("工具执行已取消。"), logContext);
    } else if (timedOut) {
        LOGE(LogCat::Tool, logContext) << "命令执行超时"
            << logf("cmd", call.input.value(QStringLiteral("command")).toString().left(120));
        result = BuiltinToolRuntime::makeErrorResult(call,
            QStringLiteral("命令执行超时（%1s）。").arg(timeoutMs / 1000), logContext);
    } else if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        LOGW(LogCat::Tool, logContext) << "命令执行失败"
            << logf("exitCode", exitCode)
            << logf("cmd", call.input.value(QStringLiteral("command")).toString().left(120));
        QString message = QStringLiteral("命令执行失败，exitCode=%1").arg(exitCode);
        if (!mergedOutput.isEmpty()) {
            message.append(QStringLiteral("\n")).append(mergedOutput);
        }
        result = BuiltinToolRuntime::makeErrorResult(call, message, logContext);
    } else {
        LOGI(LogCat::Tool, logContext) << "命令执行成功"
            << logf("exitCode", 0)
            << logf("cmd", call.input.value(QStringLiteral("command")).toString().left(120));
        const QString output = !stdoutText.trimmed().isEmpty()
                                   ? stdoutText.trimmed()
                                   : mergedOutput;
        result = BuiltinToolRuntime::makeSuccessResult(call, output,
            QStringLiteral("shellResult"), shellPayload);
        return result; // 已设置 payloadType/payload，无需重复设置
    }

    result.payloadType = QStringLiteral("shellResult");
    result.payload     = shellPayload;
    return result;
}

} // namespace

void RunCommandTool::executeForeground(const ToolCall &call,
                                       const QString &commandWorkdir,
                                       const std::function<void(const ToolResult &)> &callback)
{
    m_cancelRequested = false;

    const QString command = call.input.value(QStringLiteral("command")).toString();
    LOGI(LogCat::Tool, m_logContext) << "执行命令"
        << logf("cmd", command.left(200));

    const int timeoutMs = call.input.value(QStringLiteral("timeoutMs")).toInt() > 0
                              ? call.input.value(QStringLiteral("timeoutMs")).toInt()
                              : kCommandTimeoutMs;
    if (command.trimmed().isEmpty()) {
        callback(BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("command 不能为空。"), m_logContext));
        return;
    }

    ShellConfig shellConfig = resolveShellCommand(
        call.input.value(QStringLiteral("shell")).toString(), m_defaultShell, command);

    if (!shellConfig.error.isEmpty()) {
        callback(BuiltinToolRuntime::makeErrorResult(call, shellConfig.error, m_logContext));
        return;
    }

    executeProcess(shellConfig.program, shellConfig.arguments, commandWorkdir, call, callback, timeoutMs);
}

void RunCommandTool::executeProcess(const QString &program,
                                    const QStringList &arguments,
                                    const QString &workdir,
                                    const ToolCall &call,
                                    const std::function<void(const ToolResult &)> &callback,
                                    const int timeoutMs)
{
    if (m_foregroundProcess) {
        callback(BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("已有进行中的命令执行。"), m_logContext));
        return;
    }

    // 创建进程并设置工作目录
    auto *process = new QProcess(this);
    m_foregroundProcess = process;
    process->setWorkingDirectory(workdir);

    // 执行状态追踪
    struct ExecutionState {
        bool timedOut = false;
        int timeoutMs = 0;
        QByteArray stdoutBuffer;
        QByteArray stderrBuffer;
    };
    auto state = std::make_shared<ExecutionState>();
    state->timeoutMs = timeoutMs;

    // 超时定时器
    auto *timeoutTimer = new QTimer(process);
    timeoutTimer->setSingleShot(true);

    // 实时进度发射
    auto emitLatestChunk = [this, call](const QByteArray &chunk) {
        const QString text = QString::fromUtf8(chunk).trimmed();
        if (!text.isEmpty()) {
            emit progress(call.id, QStringLiteral("stdout"), text);
        }
    };

    // 超时 → 杀进程
    connect(timeoutTimer, &QTimer::timeout, process, [this, process, state]() {
        if (m_foregroundProcess == process) {
            state->timedOut = true;
            process->kill();
        }
    });

    // stdout / stderr 实时捕获
    connect(process, &QProcess::readyReadStandardOutput, process,
            [process, state, emitLatestChunk]() {
        state->stdoutBuffer.append(process->readAllStandardOutput());
        emitLatestChunk(state->stdoutBuffer);
    });
    connect(process, &QProcess::readyReadStandardError, process,
            [process, state, emitLatestChunk]() {
        state->stderrBuffer.append(process->readAllStandardError());
        emitLatestChunk(state->stderrBuffer);
    });

    // 进程结束 → 构建结果
    connect(process, &QProcess::finished, process,
            [this, process, timeoutTimer, state, call, callback]
            (int exitCode, QProcess::ExitStatus exitStatus) {
        timeoutTimer->stop();
        state->stdoutBuffer.append(process->readAllStandardOutput());
        state->stderrBuffer.append(process->readAllStandardError());

        // 收集输出
        const QString stdoutText = sanitizeProcessText(QString::fromUtf8(state->stdoutBuffer));
        const QString stderrText = sanitizeProcessText(QString::fromUtf8(state->stderrBuffer));
        const QString mergedOutput = (stdoutText + stderrText).trimmed();

        // 解析 shell 名称
        QString shellName = call.input.value(QStringLiteral("shell")).toString().trimmed().toLower();
        if (shellName.isEmpty()) shellName = m_defaultShell;

        // 构建负载与结果
        const QJsonObject shellPayload{
            {QStringLiteral("shell"),    shellName},
            {QStringLiteral("command"),  call.input.value(QStringLiteral("command")).toString()},
            {QStringLiteral("exitCode"), exitCode},
            {QStringLiteral("stdout"),   stdoutText},
            {QStringLiteral("stderr"),   stderrText},
            {QStringLiteral("timedOut"), state->timedOut},
            {QStringLiteral("canceled"), m_cancelRequested},
        };

        const bool canceled = m_cancelRequested;
        const bool timedOut = state->timedOut;
        const int effectiveTimeout = state->timeoutMs;

        ToolResult result = buildProcessResult(canceled, timedOut, exitCode, exitStatus,
            stdoutText, stderrText, mergedOutput, shellPayload, call,
            effectiveTimeout, m_logContext);

        // 清理前台状态
        if (m_foregroundProcess == process) {
            m_foregroundProcess = nullptr;
        }
        m_cancelRequested = false;
        callback(result);
        process->deleteLater();
    });

    // 启动进程
    LOGD(LogCat::Tool, m_logContext) << "启动进程"
        << logf("program", program);
    process->start(program, arguments);
    if (!process->waitForStarted(3000)) {
        timeoutTimer->stop();
        const QString message = QStringLiteral("无法启动命令：%1").arg(program);
        LOGE(LogCat::Tool, m_logContext) << "无法启动命令"
            << logf("program", program);
        if (m_foregroundProcess == process) {
            m_foregroundProcess = nullptr;
        }
        process->deleteLater();
        callback(BuiltinToolRuntime::makeErrorResult(call, message, m_logContext));
        return;
    }

    LOGD(LogCat::Tool, m_logContext) << "进程已启动"
        << logf("program", program)
        << logf("timeoutMs", timeoutMs);
    timeoutTimer->start(timeoutMs);
}

// ------------------------------------------------------------------
// 后台执行
// ------------------------------------------------------------------
std::optional<ToolResult> RunCommandTool::handleBackgroundCommand(
    const QString &agentId,
    const ToolCall &call,
    const QString &workingDirectory)
{
    ToolResult result;
    result.toolName = call.toolName;
    result.toolUseId = call.id;
    // 标题用调用对象（命令），启动结果文案只进 text
    result.summaryText = BuiltinToolRuntime::summarizeToolCall(call);

    const QString command = call.input.value(QStringLiteral("command")).toString().trimmed();
    if (command.isEmpty()) {
        LOGE(LogCat::Tool, m_logContext) << "后台命令执行失败：command 为空";
        result.success = false;
        result.isError = true;
        result.text = QStringLiteral("command 不能为空。");
        result.previewText = result.text;
        return result;
    }

    const QString taskId = allocateBackgroundTaskId();
    const QString outputDirectory = backgroundTaskOutputDirectory();
    if (!QDir().mkpath(outputDirectory)) {
        result.success = false;
        result.isError = true;
        result.text = QStringLiteral("无法创建后台任务输出目录。");
        result.previewText = result.text;
        return result;
    }

    auto *process = new QProcess(this);
    process->setWorkingDirectory(workingDirectory);

    ShellConfig shellConfig = resolveShellCommand(
        call.input.value(QStringLiteral("shell")).toString(),
        m_defaultShell,
        command);

    if (!shellConfig.error.isEmpty()) {
        process->deleteLater();
        result.success = false;
        result.isError = true;
        result.text = shellConfig.error;
        result.previewText = result.text;
        return result;
    }

    QString taskType = shellConfig.taskType;
    process->start(shellConfig.program, shellConfig.arguments);

    if (!process->waitForStarted(3000)) {
        process->deleteLater();
        LOGE(LogCat::Tool, m_logContext) << "后台命令启动失败"
            << logf("cmd", command.left(120));
        result.success = false;
        result.isError = true;
        result.text = QStringLiteral("无法启动后台命令。");
        result.previewText = result.text;
        return result;
    }

    BackgroundTask task;
    task.id = taskId;
    task.toolName = call.toolName;
    task.taskType = taskType;
    task.description = call.input.value(QStringLiteral("description")).toString().trimmed();
    if (task.description.isEmpty()) {
        task.description = command;
    }
    task.command = command;
    task.workingDirectory = workingDirectory;
    task.outputPath = QDir(outputDirectory).filePath(QStringLiteral("%1.txt").arg(taskId));
    task.process = process;
    m_backgroundTasks.insert(taskId, task);
    LOGI(LogCat::Tool, m_logContext) << "后台命令已启动"
        << logf("taskId", taskId)
        << logf("cmd", command.left(120));

    connect(process, &QProcess::readyReadStandardOutput, this, [this, taskId, process]() {
        auto it = m_backgroundTasks.find(taskId);
        if (it == m_backgroundTasks.end()) return;
        it->stdoutText.append(QString::fromUtf8(process->readAllStandardOutput()));
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, taskId, process]() {
        auto it = m_backgroundTasks.find(taskId);
        if (it == m_backgroundTasks.end()) return;
        it->stderrText.append(QString::fromUtf8(process->readAllStandardError()));
    });
    connect(process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this, agentId, taskId, process](int exitCode, QProcess::ExitStatus exitStatus) {
        auto it = m_backgroundTasks.find(taskId);
        if (it == m_backgroundTasks.end()) {
            process->deleteLater();
            return;
        }

        it->stdoutText.append(QString::fromUtf8(process->readAllStandardOutput()));
        it->stderrText.append(QString::fromUtf8(process->readAllStandardError()));
        it->exitCode = exitCode;
        const bool wasCanceled = it->status == QStringLiteral("canceled");
        it->interrupted = wasCanceled || (exitStatus != QProcess::NormalExit || exitCode != 0);
        it->status = wasCanceled ? QStringLiteral("canceled")
                                 : (it->interrupted ? QStringLiteral("failed")
                                                    : QStringLiteral("completed"));
        it->process.clear();

        QFile outputFile(it->outputPath);
        if (outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            outputFile.write((it->stdoutText + it->stderrText).toUtf8());
            outputFile.close();
        }

        if (!it->notified && m_session) {
            if (AbstractUnit *unit = m_session->findUnit(agentId)) {
                unit->appendSessionEvent(QStringLiteral("后台任务 %1 已结束，可用 agent_status 查看状态。").arg(taskId));
            }
            LOGI(LogCat::Tool, m_logContext) << "后台命令结束"
                << logf("taskId", taskId)
                << logf("exitCode", exitCode)
                << logf("status", it->status);
            it->notified = true;
        }
        process->deleteLater();
    });

    result.success = true;
    result.text = QStringLiteral("Started background task %1. Output path: %2")
                      .arg(taskId, QDir::toNativeSeparators(task.outputPath));
    result.previewText = result.text;
    setToolPayload(&result,
                   QStringLiteral("backgroundTaskResult"),
                   QJsonObject{
                       {QStringLiteral("taskId"), taskId},
                       {QStringLiteral("taskType"), task.taskType},
                       {QStringLiteral("outputPath"), task.outputPath},
                   });
    return result;
}

// ------------------------------------------------------------------
// 统一入口 (前台/后台自动分发)
// ------------------------------------------------------------------
void RunCommandTool::executeCommand(const QString &agentId,
                                     const ToolCall &call,
                                     const QString &workingDirectory,
                                     const std::function<void(const ToolResult &)> &callback)
{
    if (call.input.value(QStringLiteral("runInBackground")).toBool(false)) {
        auto result = handleBackgroundCommand(agentId, call, workingDirectory);
        if (result.has_value()) {
            callback(result.value());
        } else {
            callback(BuiltinToolRuntime::makeErrorResult(
                call, QStringLiteral("后台命令启动失败。"), m_logContext));
        }
        return;
    }

    // 前台执行
    const QString rawWorkdir = call.input.value(QStringLiteral("workdir")).toString().trimmed();
    QString workdirError;
    const QString commandWorkdir = BuiltinToolRuntime::resolveWorkspacePath(
        workingDirectory, rawWorkdir, &workdirError);
    if (commandWorkdir.isEmpty()) {
        callback(BuiltinToolRuntime::makeErrorResult(call, workdirError, m_logContext));
        return;
    }
    executeForeground(call, commandWorkdir, callback);
}

QString RunCommandTool::allocateBackgroundTaskId()
{
    while (true) {
        const QString candidate = QStringLiteral("bg-%1").arg(m_nextBackgroundTaskIndex++);
        if (!m_backgroundTasks.contains(candidate)) {
            return candidate;
        }
    }
}

QString RunCommandTool::backgroundTaskOutputDirectory() const
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("artifacts/task-output"));
}
