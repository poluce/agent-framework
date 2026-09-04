#pragma once

#include <QString>
#include <QVariant>

class AbstractUnit;
class FileSkillLoader;
struct SessionRuntime;

/**
 * @brief 会话的窄视图（工具层消费方定义；agent/AgentSession 实现）
 *
 * 只暴露工具需要的会话能力：查单元、编排可见性、技能加载、运行时配置。
 * 工具层不依赖 agent/ 具体类。
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
};
