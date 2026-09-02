#pragma once

#include <QObject>
#include <QString>

template<typename T> class QFutureWatcher;

struct AgentPromptContext
{
    QString agentId;
    QString displayName;
    QString parentAgentId;
    /// 角色模板文件名（如 role_leader.md）。空 = 不拼角色块。由编排填写。
    QString rolePromptFile;
    /// 模式覆盖模板文件名（如 plan.md）。空 = 不拼模式块。由模式策略填写。
    QString modePromptFile;
    QString workspacePath;
};

class SystemPromptBuilder : public QObject
{
    Q_OBJECT

public:
    /// 产品槽位路径；空串 = 不读写磁盘（仅内置 qrc 模板）。
    struct PromptPaths {
        QString userPromptFile;
        QString compactOverlayFile;
        QString segmentOverlayFile;
    };

    explicit SystemPromptBuilder(QObject *parent = nullptr);
    explicit SystemPromptBuilder(PromptPaths paths, QObject *parent = nullptr);

    /// 加载用户槽位 + 预热环境块（组合根在注入路径后调用）。
    void prepare();

    // 分段数据源
    void setUserCustomPrompt(const QString &text);
    QString baseBehavior() const { return m_baseBehavior; }
    QString userCustomPrompt() const { return m_userCustomPrompt; }

    // Skill 列表提示（注入到系统提示，模型无需调 skill_list() 即可感知可用 skill）
    void setAvailableSkills(const QString &skillsBlock);
    QString availableSkills() const { return m_availableSkills; }

    // 核心拼接（模式文案走 ctx.modePromptFile，不认 AgentMode）
    [[nodiscard]] QString buildPrompt(const AgentPromptContext &ctx) const;

    /// 大压缩 system：内置 compact.md + 注入槽位追加为补充
    [[nodiscard]] QString compactSystemPrompt() const;
    [[nodiscard]] static QString builtinCompactSystemPrompt();
    /// 段摘要 system：内置 summary.md + 注入槽位追加为补充
    [[nodiscard]] QString segmentSystemPrompt() const;
    [[nodiscard]] static QString builtinSegmentSystemPrompt();

    void invalidateCache();

    // 提示词文件读写（路径来自 PromptPaths.userPromptFile）
    QString loadPromptFile() const;
    bool savePromptFile(const QString &content);

signals:
    /// 异步环境检测完成（仅当 prepare() 启动异步检测后发出）。
    void environmentReady();

private:
    void invalidateStableCache() const;
    [[nodiscard]] QString loadBaseBehavior() const;
    [[nodiscard]] QString loadUserPromptFile() const;

    [[nodiscard]] QString assembleBaseBlock(const QString &modePromptFile) const;
    [[nodiscard]] static QString assembleEnvBlock();
    [[nodiscard]] QString assembleUserBlock() const;
    [[nodiscard]] QString assembleRoleBlock(const AgentPromptContext &ctx) const;
    [[nodiscard]] QString loadNamedPromptTemplate(const QString &fileName) const;

    PromptPaths m_paths;

    // 数据源
    QString m_baseBehavior;
    QString m_userCustomPrompt;
    QString m_availableSkills;
    // 稳定段缓存
    mutable QString m_cachedEnvBlock;
    mutable QString m_cachedUserBlock;
    mutable bool m_stableCacheValid = false;
    // 异步环境检测
    QFutureWatcher<QString> *m_envWatcher = nullptr;
};
