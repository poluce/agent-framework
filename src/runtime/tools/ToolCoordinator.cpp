#include "ToolCoordinator.h"

#include "AbstractBuiltinTool.h"
#include "AbstractToolSource.h"
#include "BuiltinToolRuntime.h"
#include "SessionToolRuntime.h"
#include "logging/LogManager.h"

#include <QSet>
#include <memory>

#include "builtin/GlobTool.h"
#include "builtin/ReadFileTool.h"
#include "builtin/GrepTool.h"
#include "builtin/WriteFileTool.h"
#include "builtin/EditTool.h"
#include "builtin/NotebookEditTool.h"
#include "builtin/RunCommandTool.h"
#include "builtin/SkillListTool.h"
#include "builtin/AskQuestionTool.h"
#include "builtin/MultiEditTool.h"

namespace {

QList<std::shared_ptr<AbstractBuiltinTool>> allBuiltinTools()
{
    QList<std::shared_ptr<AbstractBuiltinTool>> tools;
    tools.append(std::make_shared<GlobTool>());
    tools.append(std::make_shared<ReadFileTool>());
    tools.append(std::make_shared<GrepTool>());
    tools.append(std::make_shared<WriteFileTool>());
    tools.append(std::make_shared<EditTool>());
    tools.append(std::make_shared<NotebookEditTool>());
    tools.append(std::make_shared<RunCommandTool>());
    tools.append(std::make_shared<SkillListTool>());
    tools.append(std::make_shared<AskQuestionTool>());
    tools.append(std::make_shared<MultiEditTool>());
    return tools;
}

} // namespace

// ====================================================================
// BuiltinToolRegistry
// ====================================================================

BuiltinToolRegistry::BuiltinToolRegistry()
{
    for (const auto &tool : allBuiltinTools()) {
        const ToolSpec spec = tool->spec();
        const QString canonicalName = spec.name.trimmed();
        if (canonicalName.isEmpty()) continue;
        m_builtinTools.insert(canonicalName, tool);
    }
}

QList<ToolSpec> BuiltinToolRegistry::specs() const
{
    QList<ToolSpec> result;
    for (const auto &tool : m_builtinTools) {
        result.append(tool->spec());
    }
    return result;
}

ToolSpec BuiltinToolRegistry::specForName(const QString &toolName) const
{
    const QString trimmed = toolName.trimmed();
    const auto tool = m_builtinTools.value(trimmed);
    return tool ? tool->spec() : ToolSpec();
}

std::shared_ptr<AbstractBuiltinTool> BuiltinToolRegistry::builtinTool(const QString &toolName) const
{
    return m_builtinTools.value(toolName.trimmed());
}

ToolPermissionDecision BuiltinToolRegistry::evaluatePermission(
    const QString &toolName,
    const ToolScope toolScope,
    const ApprovalMode approvalMode) const
{
    // 1) Auto（含 bypass 别名，已在 parseApprovalMode 收口）→ 全放
    // 2) 工具本身只读 → 放行
    // 3) ReadOnly 作用域 → 硬拒非只读；否则人工审批
    if (approvalMode == ApprovalMode::Auto) {
        return ToolPermissionDecision::Allow;
    }
    if (specForName(toolName).permissionKind == ToolPermissionKind::ReadOnly) {
        return ToolPermissionDecision::Allow;
    }
    if (toolScope == ToolScope::ReadOnly) {
        return ToolPermissionDecision::Deny;
    }
    return ToolPermissionDecision::NeedsApproval;
}

// ====================================================================
// 内置 / 会话源（内核自带，先于外部源登记）
// ====================================================================

class BuiltinToolSource final : public AbstractToolSource
{
public:
    explicit BuiltinToolSource(BuiltinToolRegistry *registry, QObject *parent)
        : AbstractToolSource(parent)
        , m_registry(registry)
    {
    }

    QString id() const override { return QStringLiteral("builtin"); }

    QList<ToolSpec> specs() const override { return m_registry->specs(); }

    bool owns(const QString &toolName) const override
    {
        return static_cast<bool>(m_registry->builtinTool(toolName));
    }

    void invoke(const ToolCall &call, const ToolInvokeContext &ctx, Completion done) override
    {
        auto tool = m_registry->builtinTool(call.toolName);
        if (!tool || !ctx.builtinRuntime) {
            ToolResult tr;
            tr.toolUseId = call.id;
            tr.toolName = call.toolName;
            tr.isError = true;
            tr.text = QStringLiteral("Tool not found: %1").arg(call.toolName);
            done(std::move(tr));
            return;
        }
        ctx.builtinRuntime->execute(ctx.agentId, call, ctx.workingDirectory, tool, std::move(done));
    }

    ToolPermissionDecision evaluatePermission(const QString &toolName,
                                              ToolScope toolScope,
                                              ApprovalMode approvalMode) const override
    {
        return m_registry->evaluatePermission(toolName, toolScope, approvalMode);
    }

private:
    BuiltinToolRegistry *m_registry = nullptr;
};

class SessionToolSource final : public AbstractToolSource
{
public:
    explicit SessionToolSource(SessionToolRuntime *runtime, QObject *parent)
        : AbstractToolSource(parent)
        , m_runtime(runtime)
    {
    }

    QString id() const override { return QStringLiteral("session"); }

    QList<ToolSpec> specs() const override
    {
        QList<ToolSpec> out;
        for (const auto &tool : m_runtime->sessionTools()) {
            out.append(tool->spec());
        }
        return out;
    }

    bool owns(const QString &toolName) const override
    {
        return static_cast<bool>(m_runtime->findByName(toolName));
    }

    void invoke(const ToolCall &call, const ToolInvokeContext &ctx, Completion done) override
    {
        m_runtime->handleToolCall(ctx.agentId, call, ctx.workingDirectory, std::move(done));
    }

    ToolPermissionDecision evaluatePermission(const QString &,
                                              ToolScope,
                                              ApprovalMode) const override
    {
        return ToolPermissionDecision::Allow;
    }

private:
    SessionToolRuntime *m_runtime = nullptr;
};

// ====================================================================
// ToolCoordinator
// ====================================================================

ToolCoordinator::ToolCoordinator(AbstractSession *session,
                                 AbstractToolSource *externalSource,
                                 QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_sessionRuntime(std::make_unique<SessionToolRuntime>(session, this))
{
    m_builtinSource = new BuiltinToolSource(&m_registry, this);
    m_sessionSource = new SessionToolSource(m_sessionRuntime.get(), this);
    addSource(externalSource, QString());
    if (session) {
        if (auto skillList = std::dynamic_pointer_cast<SkillListTool>(
                m_registry.builtinTool(QStringLiteral("skill_list")))) {
            skillList->setSkillLoader(session->skillLoader());
        }
    }
}

ToolCoordinator::~ToolCoordinator()
{
    // 会话销毁：通知每个已登记源收尾（杀进程/删临时文件）。
    // 源由登记方（宿主）持有，按契约存活到会话之后；此处只通知不拥有。
    for (AbstractToolSource *source : sources()) {
        if (source) {
            source->sessionClosing();
        }
    }
}

void ToolCoordinator::addSource(AbstractToolSource *source, const QString &ownerAgentId)
{
    if (!source || m_externalSources.contains(source)) {
        return;
    }
    m_externalSources.append(source);
    m_sourceOwners.insert(source, ownerAgentId);
    connect(source, &AbstractToolSource::toolsChanged, this, &ToolCoordinator::toolsUpdated);
}

void ToolCoordinator::removeSource(AbstractToolSource *source)
{
    if (!source || !m_externalSources.removeOne(source)) {
        return;
    }
    m_sourceOwners.remove(source);
    disconnect(source, &AbstractToolSource::toolsChanged, this, &ToolCoordinator::toolsUpdated);
    emit toolsUpdated();
}

QString ToolCoordinator::sourceOwner(AbstractToolSource *source) const
{
    return m_sourceOwners.value(source);
}

void ToolCoordinator::notifySessionCleared()
{
    for (AbstractToolSource *source : sources()) {
        if (source) {
            source->sessionCleared();
        }
    }
}

QList<AbstractToolSource *> ToolCoordinator::sources() const
{
    QList<AbstractToolSource *> out;
    out.append(m_builtinSource);
    out.append(m_sessionSource);
    out.append(m_externalSources);
    return out;
}

AbstractToolSource *ToolCoordinator::sourceOwning(const QString &toolName) const
{
    const QString name = toolName.trimmed();
    for (AbstractToolSource *source : sources()) {
        if (source && source->owns(name)) {
            return source;
        }
    }
    return nullptr;
}

QList<ToolSpec> ToolCoordinator::allSpecs() const
{
    return collectSpecs(nullptr, nullptr);
}

QList<ToolSpec> ToolCoordinator::specsForAgent(const QString &agentId) const
{
    AbstractUnit *unit = m_session ? m_session->findUnit(agentId) : nullptr;
    return collectSpecs(m_session, unit);
}

QList<ToolSpec> ToolCoordinator::collectSpecs(AbstractSession *session, AbstractUnit *unit) const
{
    QList<ToolSpec> specs;
    QSet<QString> seen;
    for (AbstractToolSource *source : sources()) {
        if (!source) {
            continue;
        }
        for (const ToolSpec &spec : source->specs()) {
            const QString name = spec.name.trimmed();
            if (name.isEmpty() || seen.contains(name)) {
                continue;
            }
            if (session && !session->toolVisible(unit, source->id(), name)) {
                continue;
            }
            seen.insert(name);
            specs.append(spec);
        }
    }
    return specs;
}

ToolSpec ToolCoordinator::specForName(const QString &toolName) const
{
    const QString name = toolName.trimmed();
    AbstractToolSource *source = sourceOwning(name);
    if (!source) {
        return {};
    }
    for (const ToolSpec &spec : source->specs()) {
        if (spec.name == name) {
            return spec;
        }
    }
    return {};
}

void ToolCoordinator::dispatch(const QString &agentId, const ToolCall &call,
                                const QString &workingDirectory,
                                BuiltinToolRuntime &agentRuntime,
                                Completion completion)
{
    AbstractToolSource *source = sourceOwning(call.toolName);
    if (!source) {
        ToolResult tr;
        tr.toolUseId = call.id;
        tr.toolName = call.toolName;
        tr.isError = true;
        tr.text = "Tool not found: " + call.toolName;
        completion(tr);
        return;
    }

    if (m_session) {
        AbstractUnit *unit = m_session->findUnit(agentId);
        if (!m_session->toolVisible(unit, source->id(), call.toolName)) {
            ToolResult tr;
            tr.toolName = call.toolName;
            tr.toolUseId = call.id;
            tr.success = false;
            tr.isError = true;
            tr.category = ToolResultCategory::Error;
            tr.text = QStringLiteral("只有主 agent 可以使用此工具。");
            completion(tr);
            return;
        }
    }

    ToolInvokeContext ctx;
    ctx.agentId = agentId;
    ctx.workingDirectory = workingDirectory;
    ctx.session = m_session;
    ctx.builtinRuntime = &agentRuntime;
    source->invoke(call, ctx, std::move(completion));
}

ToolPermissionDecision ToolCoordinator::evaluatePermission(
    const QString &toolName,
    const ToolScope toolScope,
    const ApprovalMode approvalMode) const
{
    if (AbstractToolSource *source = sourceOwning(toolName)) {
        return source->evaluatePermission(toolName, toolScope, approvalMode);
    }
    if (approvalMode == ApprovalMode::Auto) {
        return ToolPermissionDecision::Allow;
    }
    return ToolPermissionDecision::NeedsApproval;
}
