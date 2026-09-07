#pragma once

#include <QString>
#include <QVariant>

class AbstractUnit;
class FileSkillLoader;
class WriteCoordinator;
struct SessionRuntime;

/**
 * @brief 会话的窄视图（工具层与 Loop 消费；agent/AgentSession 实现）
 *
 * 查单元、编排可见性、技能、运行时配置、写协调、压缩/角色查询。
 * 调用方不依赖 agent/ 具体类。
 */
class AbstractSession
{
public:
    virtual ~AbstractSession() = default;

    /// 按 id 查执行单元；不存在返回 nullptr。
    virtual AbstractUnit *findUnit(const QString &agentId) const = 0;
    /// 编排可见性裁剪；无编排 = 全可见。
    virtual bool toolVisible(AbstractUnit *unit,
                             const QString &sourceId,
                             const QString &toolName) const = 0;
    virtual FileSkillLoader *skillLoader() const = 0;
    virtual const SessionRuntime &runtime() const = 0;
    /// 单字段更新（规范化）；返回是否实际变更。
    virtual bool setRuntimeField(const QString &key, const QVariant &value) = 0;
    virtual void setSessionWorkingDirectory(const QString &workingDirectory) = 0;
    virtual QString userCustomPrompt() const = 0;
    virtual void setUserCustomPrompt(const QString &text) = 0;

    /// 会话级 per-file 写互斥；无则空。
    [[nodiscard]] virtual WriteCoordinator *writeCoordinator() const { return nullptr; }
    /// 无编排时表中第一个为 true。
    [[nodiscard]] virtual bool isPrimaryUnit(const QString &agentId) const
    {
        Q_UNUSED(agentId);
        return false;
    }
    [[nodiscard]] virtual bool hasOrchestration() const { return false; }
    [[nodiscard]] virtual QString rolePromptFile(const QString &agentId) const
    {
        Q_UNUSED(agentId);
        return {};
    }
    [[nodiscard]] virtual bool skillVisible(const QString &agentId, const QString &skillName) const
    {
        Q_UNUSED(agentId);
        Q_UNUSED(skillName);
        return true;
    }
    [[nodiscard]] virtual bool usesSegmentSummary(const QString &agentId) const
    {
        Q_UNUSED(agentId);
        return false;
    }
    [[nodiscard]] virtual bool remainsIdleAfterTurn(const QString &agentId) const
    {
        Q_UNUSED(agentId);
        return true;
    }
    /// 某单元写成功后，通知同会话其他单元使读缓存失效。
    virtual void notifyFileWritten(const QString &writerAgentId, const QString &absPath)
    {
        Q_UNUSED(writerAgentId);
        Q_UNUSED(absPath);
    }
};
