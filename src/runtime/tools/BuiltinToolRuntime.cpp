#include "BuiltinToolRuntime.h"

#include "AbstractBuiltinTool.h"
#include "BuiltinTools/RunCommandTool.h"
#include "logging/LogManager.h"
#include "tools/BuiltinTools/helpers/ContentEditHelper.h"
#include "tools/BuiltinTools/helpers/WorkspaceHelper.h"
#include "tools/WriteCoordinator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTimer>
#include <QThread>
#include <QThreadPool>
#include <QReadLocker>
#include <QWriteLocker>

#include <algorithm>

namespace {

const int kDefaultMaxResultSizeChars = 50000;
const int kMaxToolResultTokens = 100000;
const int kBytesPerToken = 4;
const int kMaxToolResultBytes = kMaxToolResultTokens * kBytesPerToken;
const int kPersistPreviewBytes = 2000;
// 卡片标题旁的调用对象（命令/路径）；对齐 TUI 一行 headline，勿再截成 50 字看不全
const int kToolSummaryMaxLength = 96;

QString firstNonEmptyInput(const QJsonObject &input, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QString value = input.value(QLatin1String(key)).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

QString compactSummary(const QString &text)
{
    const QString simplified = text.simplified();
    if (simplified.size() <= kToolSummaryMaxLength) return simplified;
    return simplified.left(kToolSummaryMaxLength) + QStringLiteral("...");
}

/**
 * 从工具入参提取「调用对象」摘要（路径 / 命令 / pattern…）。
 * 不含工具名：UI 标题已单独画 toolName，summary 再带名会叠成
 * `run_command  run_command git status`。
 *
 * description **不是**调用对象——它是用途说明（注释），绝不能抢标题。
 * 显示层若要展示 description，应作为次行，不进 summaryText。
 */
QString summarizePathInput(const QJsonObject &input)
{
    // 1) 路径类主目标
    if (const QString path = firstNonEmptyInput(input, {"rootPath", "filePath", "notebookPath", "path"});
        !path.isEmpty()) {
        return compactSummary(path);
    }

    // 2) shell：标题必须是实际命令（优先于 description）
    if (const QString command = firstNonEmptyInput(input, {"command", "cmd", "script", "CommandLine", "commandLine"});
        !command.isEmpty()) {
        return compactSummary(command);
    }

    // 3) 搜索/提问类主目标
    if (const QString text = firstNonEmptyInput(input, {"query", "pattern", "question"});
        !text.isEmpty()) {
        return compactSummary(text);
    }

    // 4) 会话类 action/name
    const QString action = input.value(QStringLiteral("action")).toString().trimmed();
    const QString name = input.value(QStringLiteral("name")).toString().trimmed();
    if (!action.isEmpty()) {
        return compactSummary(name.isEmpty() ? action : QStringLiteral("%1 %2").arg(action, name));
    }
    if (!name.isEmpty()) {
        return compactSummary(name);
    }

    // 5) description 故意不进 summary —— 留给次行展示
    return {};
}

QString defaultResultStoreDirectory()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("artifacts/tool-results"));
}

QString previewFromUtf8Bytes(const QString &text, int maxBytes = kPersistPreviewBytes)
{
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= maxBytes) return text;
    return QString::fromUtf8(utf8.left(maxBytes));
}

QString trimToByteLimit(const QString &text, bool *trimmed)
{
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= kMaxToolResultBytes) {
        if (trimmed) *trimmed = false;
        return text;
    }
    if (trimmed) *trimmed = true;
    return QString::fromUtf8(utf8.left(kMaxToolResultBytes));
}

QString resultStorePathFor(const QString &rootDirectory, const ToolResult &result)
{
    return QDir(rootDirectory).filePath(
        QStringLiteral("%1_%2_%3.txt")
            .arg(result.toolName, result.toolUseId,
                 QString::number(QDateTime::currentMSecsSinceEpoch())));
}

QString persistedOutputText(const QString &path, const QString &preview)
{
    return QStringLiteral("<persisted-output>\n%1\n\n%2\n[...已省略...]")
        .arg(QDir::toNativeSeparators(path), preview);
}

void finalizeToolResultForDisplay(const QString &resultStoreDirectory, ToolResult *result)
{
    if (!result) return;

    bool trimmedToBudget = false;
    QString text = trimToByteLimit(result->text, &trimmedToBudget);
    const int originalByteCount = text.toUtf8().size();

    // summaryText = 调用对象（命令/路径）；outcome 走 text/preview。
    // 若工厂未填，才用正文 compact 兜底，避免卡片空白。
    if (result->summaryText.trimmed().isEmpty())
        result->summaryText = compactSummary(text);
    if (result->previewText.trimmed().isEmpty() && !text.trimmed().isEmpty())
        result->previewText = compactSummary(text);

    const bool shouldPersist = trimmedToBudget
        || text.size() > kDefaultMaxResultSizeChars
        || originalByteCount > kMaxToolResultBytes;
    if (!shouldPersist) {
        result->text = text;
        result->wasPersisted = false;
        result->wasTruncated = trimmedToBudget;
        return;
    }

    const QString storageRoot = resultStoreDirectory.trimmed().isEmpty()
        ? defaultResultStoreDirectory()
        : QDir::cleanPath(resultStoreDirectory.trimmed());
    const QString preview = previewFromUtf8Bytes(text);
    result->previewText = preview;
    result->wasTruncated = true;

    if (!QDir().mkpath(storageRoot)) {
        result->text = persistedOutputText(QStringLiteral("[persist-failed]"), preview);
        return;
    }

    const QString path = resultStorePathFor(storageRoot, *result);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        result->text = persistedOutputText(QStringLiteral("[persist-failed]"), preview);
        return;
    }

    const QByteArray payload = text.toUtf8();
    if (file.write(payload) != payload.size() || !file.commit()) {
        result->text = persistedOutputText(QStringLiteral("[persist-failed]"), preview);
        return;
    }

    result->wasPersisted = true;
    result->persistedPath = path;
    result->text = persistedOutputText(path, preview);
}

} // namespace

// ====================================================================
// BuiltinToolRuntime
// ====================================================================

BuiltinToolRuntime::BuiltinToolRuntime(QObject *parent)
    : QObject(parent)
    , m_runCommandTool(std::make_unique<RunCommandTool>(this, nullptr))
{
    connect(m_runCommandTool.get(), &RunCommandTool::progress,
            this, &BuiltinToolRuntime::toolProgress);
}

BuiltinToolRuntime::~BuiltinToolRuntime()
{
    cancelActiveExecution();
}

void BuiltinToolRuntime::setResultStoreDirectory(const QString &directoryPath)
{
    m_resultStoreDirectory = directoryPath.trimmed().isEmpty()
        ? QString()
        : QDir::cleanPath(directoryPath.trimmed());
}

void BuiltinToolRuntime::setDefaultShell(const QString &shell)
{
    if (m_runCommandTool)
        m_runCommandTool->setDefaultShell(shell);
}

QString BuiltinToolRuntime::defaultShell() const
{
    return m_runCommandTool ? m_runCommandTool->defaultShell() : QString();
}

void BuiltinToolRuntime::setLogContext(const AgentLogContext &logContext)
{
    m_logContext = logContext;
    if (m_runCommandTool)
        m_runCommandTool->setLogContext(logContext);
}

QStringList BuiltinToolRuntime::availableShells() const
{
    return m_runCommandTool ? m_runCommandTool->availableShells() : QStringList();
}

void BuiltinToolRuntime::clearReadFileStates()
{
    m_readFileStates.clear();
}

void BuiltinToolRuntime::execute(const QString &agentId,
                                  const ToolCall &call,
                                  const QString &workingDirectory,
                                  std::shared_ptr<AbstractBuiltinTool> tool,
                                  Completion completion)
{
    if (m_running) {
        LOGW(LogCat::Tool, m_logContext) << "工具执行失败：已有进行中的命令"
            << logf("tool", call.toolName)
            << logf("toolId", call.id);
        completion(makeErrorResult(call, QStringLiteral("已有进行中的命令执行。"), m_logContext));
        return;
    }

    LOGI(LogCat::Tool, m_logContext) << "开始执行工具"
        << logf("tool", call.toolName)
        << logf("toolId", call.id);

    m_running = true;
    if (tool) {
        emit toolProgress(call.id, tool->progressKind(), {});
    }

    const auto finishCallback = [this, completion = std::move(completion)](ToolResult result) mutable {
        finalizeToolResultForDisplay(m_resultStoreDirectory, &result);
        m_running = false;
        completion(std::move(result));
    };

    QTimer::singleShot(0, this, [this, agentId, call, workingDirectory, tool, finishCallback]() {
        executeImmediate(agentId, call, workingDirectory, tool, finishCallback);
    });
}

void BuiltinToolRuntime::cancelActiveExecution()
{
    if (m_runCommandTool)
        m_runCommandTool->cancel();
}

bool BuiltinToolRuntime::isRunning() const
{
    return m_running || (m_runCommandTool && m_runCommandTool->isRunning());
}

void BuiltinToolRuntime::setSession(AgentSession *session)
{
    m_runCommandTool->setSession(session);
}

void BuiltinToolRuntime::setWriteCoordinator(WriteCoordinator *coordinator)
{
    m_writeCoordinator = coordinator;
}

// ---- 读缓存 ----

void BuiltinToolRuntime::invalidateReadFileState(const QString &filePath)
{
    QWriteLocker locker(&m_cacheLock);
    m_readFileStates.remove(WorkspaceHelper::normalizedPath(filePath));
}

// ---- 静态工具方法 ----

QString BuiltinToolRuntime::normalizeWorkspacePath(const QString &workingDirectory)
{
    const QString raw = workingDirectory.trimmed().isEmpty()
        ? QDir::currentPath()
        : workingDirectory.trimmed();
    return WorkspaceHelper::normalizedPath(QDir(raw).absolutePath());
}

QString BuiltinToolRuntime::resolveWorkspacePath(const QString &workingDirectory,
                                                   const QString &rawPath,
                                                   QString *errorMessage)
{
    const QString workspaceRoot = normalizeWorkspacePath(workingDirectory);
    const QString trimmedPath = rawPath.trimmed();
    const QFileInfo inputInfo(trimmedPath);
    const QString candidate = trimmedPath.isEmpty()
        ? workspaceRoot
        : inputInfo.isAbsolute()
            ? inputInfo.absoluteFilePath()
            : QDir(workspaceRoot).absoluteFilePath(trimmedPath);

#ifdef Q_OS_WIN
    const QString resolved = WorkspaceHelper::posixPathToWindowsPath(candidate);
#else
    const QString resolved = candidate;
#endif
    const QString normalizedCandidate = WorkspaceHelper::normalizedPath(resolved);
    if (!WorkspaceHelper::isWithinWorkspace(workspaceRoot, normalizedCandidate)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("目标路径超出当前工作目录范围: %1").arg(rawPath);
        return {};
    }
    if (errorMessage) errorMessage->clear();
    return normalizedCandidate;
}

QString BuiltinToolRuntime::summarizeToolCall(const ToolCall &call)
{
    // 只返回调用对象；工具名由 UI 单独展示。空入参时退回工具名，避免空白卡。
    const QString target = summarizePathInput(call.input);
    if (!target.isEmpty())
        return target;
    return call.toolName;
}

// ---- 结果工厂（私有辅助） ----

namespace {

void logResultEvent(ToolResultCategory category,
                    const ToolCall &call, const QString &message,
                    const AgentLogContext *logContext)
{
    QString label;
    switch (category) {
    case ToolResultCategory::Error:    label = QStringLiteral("工具执行错误"); break;
    case ToolResultCategory::Rejected: label = QStringLiteral("工具被拒绝");   break;
    case ToolResultCategory::Canceled: label = QStringLiteral("工具已取消");   break;
    default: return;
    }

    if (logContext && logContext->isValid()) {
        switch (category) {
        case ToolResultCategory::Error:
            LOGE(LogCat::Tool, *logContext) << label
                << logf("tool", call.toolName) << logf("toolId", call.id) << logf("msg", message);
            break;
        case ToolResultCategory::Rejected:
            LOGW(LogCat::Tool, *logContext) << label
                << logf("tool", call.toolName) << logf("toolId", call.id) << logf("msg", message);
            break;
        case ToolResultCategory::Canceled:
            LOGI(LogCat::Tool, *logContext) << label
                << logf("tool", call.toolName) << logf("toolId", call.id) << logf("msg", message);
            break;
        default:
            break;
        }
        return;
    }

    switch (category) {
    case ToolResultCategory::Error:
        LOGE(LogCat::Tool) << label
            << logf("tool", call.toolName) << logf("toolId", call.id) << logf("msg", message);
        break;
    case ToolResultCategory::Rejected:
        LOGW(LogCat::Tool) << label
            << logf("tool", call.toolName) << logf("toolId", call.id) << logf("msg", message);
        break;
    case ToolResultCategory::Canceled:
        LOGI(LogCat::Tool) << label
            << logf("tool", call.toolName) << logf("toolId", call.id) << logf("msg", message);
        break;
    default:
        break;
    }
}

ToolResult buildCategoryResult(const ToolCall &call, const QString &message,
                               ToolResultCategory category,
                               const AgentLogContext *logContext)
{
    logResultEvent(category, call, message, logContext);

    ToolResult result;
    result.toolName    = call.toolName;
    result.toolUseId   = call.id;
    result.success     = false;
    result.isError     = (category == ToolResultCategory::Error);
    result.category    = category;
    result.text        = message;
    // summary = 调用对象（如实际 command）；错误正文留在 text / preview
    result.summaryText = BuiltinToolRuntime::summarizeToolCall(call);
    result.previewText = compactSummary(message);
    return result;
}

} // namespace

// ---- 结果工厂 ----

ToolResult BuiltinToolRuntime::makeErrorResult(const ToolCall &call, const QString &message,
                                                 const AgentLogContext &logContext)
{
    return buildCategoryResult(call, message, ToolResultCategory::Error, &logContext);
}

ToolResult BuiltinToolRuntime::makeRejectedResult(const ToolCall &call, const QString &message,
                                                    const AgentLogContext &logContext)
{
    return buildCategoryResult(call, message, ToolResultCategory::Rejected, &logContext);
}

ToolResult BuiltinToolRuntime::makeCanceledResult(const ToolCall &call, const QString &message,
                                                    const AgentLogContext &logContext)
{
    return buildCategoryResult(call, message, ToolResultCategory::Canceled, &logContext);
}

ToolResult BuiltinToolRuntime::makeSuccessResult(const ToolCall &call,
                                                   const QString &text,
                                                   const QString &payloadType,
                                                   const QJsonObject &payload)
{
    ToolResult result;
    result.toolName    = call.toolName;
    result.toolUseId   = call.id;
    result.success     = true;
    result.isError     = false;
    result.category    = ToolResultCategory::Success;
    result.payloadType = payloadType;
    result.payload     = payload;
    result.text        = text;
    // summary = 调用对象；stdout 等结果只进 text / preview，不抢标题
    result.summaryText = summarizeToolCall(call);
    result.previewText = compactSummary(text);
    return result;
}

// ---- 读缓存 ----

BuiltinToolRuntime::ReadFileState BuiltinToolRuntime::readFileState(const QString &filePath) const
{
    QReadLocker locker(&m_cacheLock);
    return m_readFileStates.value(filePath);
}

void BuiltinToolRuntime::setReadFileState(const QString &filePath, const ReadFileState &state)
{
    QWriteLocker locker(&m_cacheLock);
    m_readFileStates.insert(filePath, state);
}

// ---- 写权限校验 ----

namespace {

/** 豁免「需完整读」判定：写工具按自身入参语义决定是否放行未读/局部读文件。
 *  - edit：oldString 非空 → 放行（匹配成功即证明模型读到要改内容）
 *  - write_file：append → 放行（追加不需要知道原内容）
 *  - multi_edit：全部项 oldString 非空 → 放行；含行号项 → 要求全视图 */
bool requiresFullView(const ToolCall &call)
{
    if (call.toolName == QStringLiteral("edit"))
        return call.input.value(QStringLiteral("oldString")).toString().trimmed().isEmpty();
    if (call.toolName == QStringLiteral("write_file"))
        return !call.input.value(QStringLiteral("append")).toBool();
    if (call.toolName == QStringLiteral("multi_edit")) {
        const QJsonArray edits = call.input.value(QStringLiteral("edits")).toArray();
        for (const QJsonValue &v : edits) {
            if (v.toObject().value(QStringLiteral("oldString")).toString().trimmed().isEmpty())
                return true; // 含行号项
        }
        return false;
    }
    return true;
}

} // namespace

bool BuiltinToolRuntime::validateWriteAccess(const QString &filePath,
                                               const QString &workspaceRoot,
                                               const ToolCall &call,
                                               const std::function<void(const ToolResult &)> &callback,
                                               const QString &verb,
                                               bool exemptReadRequirement)
{
    Q_UNUSED(workspaceRoot);
    const QFileInfo info(filePath);
    if (!info.exists()) return true;

    const QString normalizedFilePath = WorkspaceHelper::normalizedPath(filePath);
    const ReadFileState readState = readFileState(normalizedFilePath);
    const bool hasFullView = readState.timestampMs > 0 && !readState.partialView;

    // 三档区分（豁免判定在 executeImmediate 统一算好传入）：
    // - exemptReadRequirement=true（edit oldString / write_file append）：partialView/未读放行，
    //   oldString 匹配成功本身就是「模型读到要改内容」的证明（匹配不上 edit 自己报 not found）
    // - 否则（edit 行号 / write_file 全量 / multi_edit 含行号项）：需完整视图
    if (!hasFullView && !exemptReadRequirement) {
        callback(makeErrorResult(call, QStringLiteral("需先完整读取文件（不带 offset/limit）后才能%1它。").arg(verb)));
        return false;
    }

    // 防外部修改：只在缓存有记录（timestampMs>0）时才比对 mtime——
    // 无缓存记录时 timestampMs==0，mtime>0 恒真会误拒；此时靠 oldString/内容校验兜底
    if (readState.timestampMs > 0
        && info.lastModified().toMSecsSinceEpoch() > readState.timestampMs) {
        QFile currentFile(filePath);
        QString currentContent;
        if (currentFile.open(QIODevice::ReadOnly | QIODevice::Text))
            currentContent = QString::fromUtf8(currentFile.readAll());
        if (readState.content.isEmpty() || currentContent != readState.content) {
            callback(makeErrorResult(call, QStringLiteral("File has been modified since read, either by the user or by a linter. Read it again before %1 it.").arg(verb)));
            return false;
        }
    }
    return true;
}

// ====================================================================
// executeImmediate — 核心调度
// ====================================================================

namespace {

// ---- 工具执行后同步读缓存（写入类工具共用） ----
void syncReadStateAfterWrite(BuiltinToolRuntime *self,
                              const QString &workingDirectory,
                              const QString &rawPath)
{
    QString resolveErr;
    const QString absPath = BuiltinToolRuntime::resolveWorkspacePath(
        workingDirectory, rawPath, &resolveErr);
    if (absPath.isEmpty()) return;

    BuiltinToolRuntime::ReadFileState state;
    state.timestampMs = QFileInfo(absPath).lastModified().toMSecsSinceEpoch();
    state.partialView = false;
    state.offset      = -1;
    state.limit       = -1;

    QFile file(absPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        state.content = QString::fromUtf8(file.readAll());

    self->setReadFileState(absPath, state);
}

} // namespace

void BuiltinToolRuntime::executeImmediate(
    const QString &agentId,
    const ToolCall &call,
    const QString &workingDirectory,
    std::shared_ptr<AbstractBuiltinTool> tool,
    const std::function<void(const ToolResult &)> &callback)
{
    // 工具未注册
    if (!tool) {
        callback(makeErrorResult(call, QStringLiteral("未识别的工具: %1").arg(call.toolName), m_logContext));
        return;
    }

    const QString workspaceRoot = normalizeWorkspacePath(workingDirectory);

    // 轻量工具（当前仅 run_command），统一由 RunCommandTool 调度前台/后台
    if (!tool->isHeavyweight()) {
        m_runCommandTool->executeCommand(agentId, call, workingDirectory, callback);
        return;
    }

    // ---- 主线程前置校验 ----
    const ToolSpec spec = tool->spec();
    QStringList writeLockKeys; // 写工具：规范化绝对路径（per-file 互斥用）
    if (spec.permissionKind == ToolPermissionKind::Write) {
        const bool requireFullView = requiresFullView(call);
        const QStringList targetPaths = tool->writeTargetPathsForInput(call.input);
        for (const QString &targetPath : targetPaths) {
            if (targetPath.isEmpty())
                continue;
            QString resolveErr;
            const QString resolved = resolveWorkspacePath(workingDirectory, targetPath, &resolveErr);
            if (resolved.isEmpty()) {
                callback(makeErrorResult(call, resolveErr, m_logContext));
                return;
            }
            // 目标路径是目录 → 明确报错（在 validateWriteAccess 之前）
            if (QFileInfo(resolved).isDir()) {
                callback(makeErrorResult(call, QStringLiteral("目标路径是目录，不是文件: %1").arg(targetPath), m_logContext));
                return;
            }
            // edit 互斥预检（顶层字段；multi_edit 在 execute 内按 edits[] 校验）
            if (call.toolName == QStringLiteral("edit")) {
                const QString mutexErr = ContentEditHelper::mutualExclusionError(call.input);
                if (!mutexErr.isEmpty()) {
                    callback(makeErrorResult(call, mutexErr, m_logContext));
                    return;
                }
            }
            if (!validateWriteAccess(resolved, workspaceRoot, call, callback,
                                     QStringLiteral("writing to"), !requireFullView))
                return;
            // 同路径去重：multi_edit 经不同相对路径指向同一文件时，重复 tryAcquire 会自锁
            const QString lockKey = WorkspaceHelper::normalizedPath(resolved);
            if (!writeLockKeys.contains(lockKey))
                writeLockKeys.append(lockKey);
        }
    }

    // 线程安全快照：read_file 需要缓存状态以判断文件是否变更
    QVariantMap threadSafeContext;
    if (call.toolName == QStringLiteral("read_file")) {
        const QString rawPath = call.input.value(QStringLiteral("filePath")).toString();
        QString resolveErr;
        const QString absPath = resolveWorkspacePath(workingDirectory, rawPath, &resolveErr);
        if (!absPath.isEmpty()) {
            const ReadFileState cache = readFileState(absPath);
            threadSafeContext.insert(QStringLiteral("cacheTimestampMs"), cache.timestampMs);
            threadSafeContext.insert(QStringLiteral("cachePartialView"), cache.partialView);
            threadSafeContext.insert(QStringLiteral("cacheContent"), cache.content);
        }
    }

    // ---- 子线程执行 ----
    // 独立线程池：文件工具为 I/O 密集型，允许超额订阅；
    // 避免多 Agent 并发时与全局池互相挤占导致工具长时间排队
    static QThreadPool *toolPool = [] {
        auto *pool = new QThreadPool();
        pool->setMaxThreadCount(qMax(16, QThread::idealThreadCount() * 2));
        return pool;
    }();

    QPointer<BuiltinToolRuntime> self(this);
    toolPool->start([self, tool, call, workspaceRoot, workingDirectory, threadSafeContext,
                     writeLockKeys, callback]() {
        // 写工具 per-file 互斥：非阻塞 tryAcquire；被其他代理持有则立即报错让模型重试。
        // 不等待、不阻塞主线程（同 Agent 内 m_running 门闩已保证不会自锁）。
        // 多键（multi_edit）：全部成功才执行，任一失败释放已获。
        WriteCoordinator *coordinator = (self && !writeLockKeys.isEmpty())
            ? self->m_writeCoordinator
            : nullptr;
        const bool locked = !coordinator || coordinator->tryAcquireAll(writeLockKeys);
        if (!locked) {
            callback(BuiltinToolRuntime::makeErrorResult(
                call, QStringLiteral("文件正被其他代理写入，请稍后重试。"), self->m_logContext));
            return;
        }

        ToolResult result = tool->execute(call, workspaceRoot, workingDirectory, threadSafeContext);
        if (coordinator)
            coordinator->releaseAll(writeLockKeys);
        if (!self)
            return;

        QMetaObject::invokeMethod(self, [self, result, call, workingDirectory, callback]() {
            // 工具执行成功后更新读缓存，供后续写校验使用
            if (result.success) {
                QStringList writtenPaths;
                if (call.toolName == QStringLiteral("read_file")
                    && result.payload.value(QStringLiteral("kind")).toString() == QStringLiteral("text")) {
                    const QString rawPath = call.input.value(QStringLiteral("filePath")).toString();
                    QString resolveErr;
                    const QString absPath = BuiltinToolRuntime::resolveWorkspacePath(
                        workingDirectory, rawPath, &resolveErr);
                    if (!absPath.isEmpty()) {
                        ReadFileState state;
                        state.timestampMs = QFileInfo(absPath).lastModified().toMSecsSinceEpoch();
                        state.partialView = result.payload.value(QStringLiteral("partial")).toBool();
                        state.content     = result.payload.value(QStringLiteral("content")).toString();
                        self->setReadFileState(absPath, state);
                    }
                } else if (call.toolName == QStringLiteral("write_file")
                           || call.toolName == QStringLiteral("edit")) {
                    writtenPaths.append(call.input.value(QStringLiteral("filePath")).toString().trimmed());
                } else if (call.toolName == QStringLiteral("notebook_edit")) {
                    writtenPaths.append(call.input.value(QStringLiteral("notebookPath")).toString().trimmed());
                } else if (call.toolName == QStringLiteral("multi_edit")) {
                    // 逐 edits 项收集目标路径（同文件去重），成功后再统一同步读缓存，
                    // 否则缓存陈旧 → 下次写校验误报「被修改」
                    const QJsonArray edits = call.input.value(QStringLiteral("edits")).toArray();
                    for (const QJsonValue &v : edits) {
                        const QString rawPath = v.toObject().value(QStringLiteral("filePath")).toString().trimmed();
                        if (!rawPath.isEmpty() && !writtenPaths.contains(rawPath))
                            writtenPaths.append(rawPath);
                    }
                }
                for (const QString &rawPath : writtenPaths)
                    syncReadStateAfterWrite(self, workingDirectory, rawPath);
            }
            callback(result);
        }, Qt::QueuedConnection);
    });
}
