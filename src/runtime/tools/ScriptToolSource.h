#pragma once

#include "tools/AbstractToolSource.h"

#include <QHash>
#include <QJsonObject>
#include <QString>

class AbstractSession;

/**
 * @brief 脚本工具桥：把磁盘上的脚本（py/js/ts）变成内核工具。
 *
 * 每个脚本一个长驻进程，JSON 行协议双通道：
 *  - sync：invoke 请求 → 一行 JSON 结果（推荐 type=result 带回 id；无 type 时绑定这次调用）
 *  - push：须用信封（invoke / result / event）；event 投进目标单元邮箱
 *
 * 目录：setToolDirectory（持久，重启扫描加载）/ setEphemeralDirectory（临时，析构清理）。
 * 文件按 <dir>/<agentId>/<name>.<ext> 组织；首行 manifest 头声明 spec。
 * 元工具 create_tool / delete_tool 由本源内置（agent 自加/删工具入口）。
 */
class ScriptToolSource : public AbstractToolSource
{
    Q_OBJECT

public:
    struct ScriptTool
    {
        ToolSpec spec;
        QString filePath;
        QString language;   // py / js / ts
        QString scope = QStringLiteral("project"); // project / global / session
        bool ephemeral = false;
        bool pushMode = false;
    };

    explicit ScriptToolSource(QObject *parent = nullptr);
    ~ScriptToolSource() override;

    /// 全局/持久工具目录（跨项目复用，重启扫描加载）。设置即触发扫描。
    void setToolDirectory(const QString &dir);
    /// 全局工具目录显式设置（等价于 setToolDirectory）。
    void setGlobalDirectory(const QString &dir);
    /// 项目级工具目录（当前工程内工具，随代码库持久）。设置即触发扫描。
    void setProjectDirectory(const QString &dir);
    /// 临时/会话级工具目录（本源析构或会话清理时删除其中的工具文件）。
    void setEphemeralDirectory(const QString &dir);
    /// 语言 → 运行时命令（缺省 py=python3 / js=node / ts=ts-node）。
    void setRuntimeCommand(const QString &language, const QString &command);
    /// sync 型进程空闲回收毫秒数（缺省 60000；push 型常驻）。
    void setIdleTimeoutMs(int ms);
    /// 单次调用超时毫秒数（缺省 60000；超时杀进程并报错）。
    void setInvokeTimeoutMs(int ms);

    QString id() const override { return QStringLiteral("script"); }
    QList<ToolSpec> specs() const override;
    bool owns(const QString &toolName) const override;
    void invoke(const ToolCall &call, const ToolInvokeContext &ctx, Completion done) override;
    /// 会话关闭：杀全部脚本进程、删临时工具文件、清会话态（订阅/会话指针）。
    void sessionClosing() override;
    /// 会话清空：杀全部脚本进程、注销临时工具（删文件）、清订阅；持久工具保留。
    void sessionCleared() override;

    // ── 探针（测试/宿主）──
    int processCount() const { return m_processes.size(); }
    bool hasTool(const QString &toolName) const { return m_tools.contains(toolName); }
    QString toolFilePath(const QString &toolName) const
    {
        return m_tools.value(toolName).filePath;
    }

private:
    class ScriptProcess;

    void handleCreateTool(const ToolCall &call, const ToolInvokeContext &ctx, Completion done);
    void handleDeleteTool(const ToolCall &call, const ToolInvokeContext &ctx, Completion done);
    void handleInspectTool(const ToolCall &call, const ToolInvokeContext &ctx, Completion done);
    void dropProcess(const QString &name, const QString &reason);
    void rescan();
    void scanDir(const QString &dir, const QString &defaultScope);
    ScriptProcess *processFor(const QString &toolName);
    void handleEvent(const QString &toolName, const QJsonObject &event);
    /// 杀全部进程、清订阅、注销临时工具（删文件）；持久工具保留。
    void resetSessionState();
    static ToolSpec createToolSpec();
    static ToolSpec deleteToolSpec();
    static ToolSpec inspectToolSpec();
    static QJsonObject parseManifest(const QByteArray &head);

    QString m_toolDir;       // global tools
    QString m_projectDir;    // project tools
    QString m_ephemeralDir;  // session tools
    QHash<QString, QString> m_runtimeCommands;
    int m_idleTimeoutMs = 60000;
    int m_invokeTimeoutMs = 60000;
    QHash<QString, ScriptTool> m_tools;
    QHash<QString, ScriptProcess *> m_processes;
    /// toolName → 最近调用者 agentId（push 事件缺省投递目标）。
    QHash<QString, QString> m_subscribers;
    AbstractSession *m_session = nullptr;
};
