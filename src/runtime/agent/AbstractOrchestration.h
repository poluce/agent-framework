#pragma once

#include <QObject>
#include <QString>

class AbstractToolSource;
class Agent;
class AgentSession;

/// Host / 组合根创建单元时的请求。字段全可选；配方自己解释。
struct UnitCreateRequest
{
    QString displayName;
    QString parentAgentId;
    QString workingDirectory;
    QString modelName;
    QString approvalMode;
};

/// 编排口：内核只登记执行单元并询问「这个单元有哪些能力」。
/// 树 / spawn / 团队 / 收件箱是某一种配方的实现，不出现在本口。
/// 由组合根注入；空 = 自由单元（无 spawn/team、无角色块、无段摘要）。
class AbstractOrchestration : public QObject
{
    Q_OBJECT

public:
    explicit AbstractOrchestration(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~AbstractOrchestration() override = default;

    /// 本配方提供的工具源。可空。
    [[nodiscard]] virtual AbstractToolSource *toolSource() = 0;

    /// 接到会话（非拥有）。此后经 Session::insertUnit 登记单元。
    virtual void attach(AgentSession *session) = 0;
    virtual void detach() = 0;

    /// 该单元是否可见该源上的该工具。默认全可见。
    [[nodiscard]] virtual bool toolVisible(const Agent *unit,
                                           const QString &sourceId,
                                           const QString &toolName) const
    {
        Q_UNUSED(unit);
        Q_UNUSED(sourceId);
        Q_UNUSED(toolName);
        return true;
    }

    /// 系统提示角色模板文件名（仅 basename）。空 = 不拼角色块。
    [[nodiscard]] virtual QString rolePromptFile(const Agent *unit) const
    {
        Q_UNUSED(unit);
        return {};
    }

    /// 该单元空闲时是否负责会话自动改标题。默认否。
    [[nodiscard]] virtual bool ownsSessionTitle(const Agent *unit) const
    {
        Q_UNUSED(unit);
        return false;
    }

    /// 该单元是否安装段摘要管线。默认否。
    [[nodiscard]] virtual bool usesSegmentSummary(const Agent *unit) const
    {
        Q_UNUSED(unit);
        return false;
    }

    /// 回合 Completed 后是否回到 Idle。默认是。
    [[nodiscard]] virtual bool remainsIdleAfterTurn(const Agent *unit) const
    {
        Q_UNUSED(unit);
        return true;
    }

    /// 会话清空单元表前。
    virtual void onUnitsClearing() {}

    /// `AgentSession::start()` 清空后：配方可在此插入首批单元。
    /// 默认不插；无编排的会话保持空表。
    virtual void onSessionStarted() {}

    /// 单元刚登记进会话表（尚未 setCoordinator）。
    virtual void onUnitInserted(Agent *unit) { Q_UNUSED(unit); }

    /// 单元状态变化（内核不解释含义；配方可在此拉排队等）。
    virtual void onUnitStateChanged(Agent *unit) { Q_UNUSED(unit); }

    /// 本配方的主单元；空 = 无。
    [[nodiscard]] virtual Agent *primaryUnit() const { return nullptr; }
    [[nodiscard]] virtual bool isPrimary(const Agent *unit) const
    {
        Q_UNUSED(unit);
        return false;
    }

    /// Host / 组合根创建单元。默认拒绝。
    /// `parentAgentId` 只是可选元数据，口本身不规定必须成树。
    virtual Agent *createUnit(const UnitCreateRequest &request)
    {
        Q_UNUSED(request);
        return nullptr;
    }

    /// Host / 组合根关闭单元。默认拒绝。返回已从表移除的指针（调用方 deleteLater）。
    virtual Agent *closeUnit(const QString &agentId)
    {
        Q_UNUSED(agentId);
        return nullptr;
    }
};
