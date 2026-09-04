#include "tools/session/ConfigTool.h"
#include "tools/SessionToolContext.h"
#include "agent/Agent.h"
#include "agent/AgentSession.h"
#include "config/SystemPromptBuilder.h"

#include <QFile>
#include <QTemporaryDir>
#include <QUuid>

#include <QtTest>

namespace {

ToolResult runConfig(AgentSession &session, Agent *agent, const QJsonObject &input)
{
    SessionToolContext ctx(&session, agent);
    ConfigTool tool;
    ToolCall call;
    call.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    call.toolName = QStringLiteral("config");
    call.input = input;
    return tool.execute(&ctx, call, session.runtime().workingDirectory);
}

} // namespace

class ConfigToolTests : public QObject
{
    Q_OBJECT

private slots:
    void whitelist_blocksDangerousKeys();
    void readOnlyProjections_notSettable();
    void systemPrompt_persistsToFile();
    void systemPromptAppend_appendsAndPersists();
    void get_returnsCurrent();
};

void ConfigToolTests::whitelist_blocksDangerousKeys()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    SessionRuntime defaults;
    defaults.workingDirectory = tmp.path();
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    AgentSession session(cfg);
    Agent *agent = session.insertUnit(QStringLiteral("agent-0"), QStringLiteral("测试"));
    QVERIFY(agent);

    // 危险键：自我提权 / 宿主级决策，不可由 agent 修改
    for (const QString &key : {QStringLiteral("approvalMode"), QStringLiteral("toolScope"),
                               QStringLiteral("providerType"), QStringLiteral("workingDirectory")}) {
        const ToolResult r = runConfig(session, agent, {
            {QStringLiteral("setting"), key},
            {QStringLiteral("value"), QStringLiteral("x")},
        });
        QVERIFY2(r.isError, qPrintable(QStringLiteral("应拒绝修改 %1").arg(key)));
    }
    // 安全键可写
    const ToolResult r = runConfig(session, agent, {
        {QStringLiteral("setting"), QStringLiteral("modelName")},
        {QStringLiteral("value"), QStringLiteral("deepseek-chat")},
    });
    QVERIFY(r.success);
    QVERIFY(!r.isError);
    QCOMPARE(session.runtime().modelName, QStringLiteral("deepseek-chat"));
}

void ConfigToolTests::readOnlyProjections_notSettable()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    SessionRuntime defaults;
    defaults.workingDirectory = tmp.path();
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    AgentSession session(cfg);
    Agent *agent = session.insertUnit(QStringLiteral("agent-0"), QStringLiteral("测试"));
    QVERIFY(agent);

    // 只读投影（Core resolve 后写入）：不可由 agent 修改
    for (const QString &key : {QStringLiteral("contextWindow"), QStringLiteral("maxOutputTokens"),
                               QStringLiteral("maxOutputTokensSource")}) {
        const ToolResult r = runConfig(session, agent, {
            {QStringLiteral("setting"), key},
            {QStringLiteral("value"), QStringLiteral("1000")},
        });
        QVERIFY2(r.isError, qPrintable(QStringLiteral("应拒绝修改 %1").arg(key)));
    }
    // 读取不受限
    const ToolResult r = runConfig(session, agent, {
        {QStringLiteral("setting"), QStringLiteral("approvalMode")},
    });
    QVERIFY(r.success);
    QCOMPARE(r.payload.value(QStringLiteral("operation")).toString(), QStringLiteral("get"));
}

void ConfigToolTests::systemPrompt_persistsToFile()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString promptFile = tmp.path() + QStringLiteral("/user_prompt.md");
    SystemPromptBuilder builder(SystemPromptBuilder::PromptPaths{promptFile, {}, {}});

    SessionRuntime defaults;
    defaults.workingDirectory = tmp.path();
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    cfg.promptBuilder = &builder;
    AgentSession session(cfg);
    Agent *agent = session.insertUnit(QStringLiteral("agent-0"), QStringLiteral("测试"));
    QVERIFY(agent);

    const ToolResult r = runConfig(session, agent, {
        {QStringLiteral("setting"), QStringLiteral("systemPrompt")},
        {QStringLiteral("value"), QStringLiteral("记住：构建用 cmake.exe")},
    });
    QVERIFY(r.success);
    QCOMPARE(session.userCustomPrompt(), QStringLiteral("记住：构建用 cmake.exe"));
    // 持久化到用户提示词文件（Text 模式读取：CRLF 还原为 LF，与写侧一致）
    QVERIFY(QFile::exists(promptFile));
    QFile f(promptFile);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(QString::fromUtf8(f.readAll()), QStringLiteral("记住：构建用 cmake.exe"));
}

void ConfigToolTests::systemPromptAppend_appendsAndPersists()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString promptFile = tmp.path() + QStringLiteral("/user_prompt.md");
    SystemPromptBuilder builder(SystemPromptBuilder::PromptPaths{promptFile, {}, {}});

    SessionRuntime defaults;
    defaults.workingDirectory = tmp.path();
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    cfg.promptBuilder = &builder;
    AgentSession session(cfg);
    Agent *agent = session.insertUnit(QStringLiteral("agent-0"), QStringLiteral("测试"));
    QVERIFY(agent);

    ToolResult r = runConfig(session, agent, {
        {QStringLiteral("setting"), QStringLiteral("systemPrompt")},
        {QStringLiteral("value"), QStringLiteral("基础约定")},
    });
    QVERIFY(r.success);
    r = runConfig(session, agent, {
        {QStringLiteral("setting"), QStringLiteral("systemPromptAppend")},
        {QStringLiteral("value"), QStringLiteral("坑：QProcess 析构会派发事件")},
    });
    QVERIFY(r.success);
    QCOMPARE(session.userCustomPrompt(),
             QStringLiteral("基础约定\n坑：QProcess 析构会派发事件"));
    // 追加后文件同步（Text 模式读取）
    QFile f(promptFile);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QCOMPARE(QString::fromUtf8(f.readAll()),
             QStringLiteral("基础约定\n坑：QProcess 析构会派发事件"));
    // 追加键读取
    r = runConfig(session, agent, {
        {QStringLiteral("setting"), QStringLiteral("systemPromptAppend")},
    });
    QVERIFY(r.success);
    QCOMPARE(r.payload.value(QStringLiteral("operation")).toString(), QStringLiteral("get"));
}

void ConfigToolTests::get_returnsCurrent()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    SessionRuntime defaults;
    defaults.workingDirectory = tmp.path();
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    AgentSession session(cfg);
    Agent *agent = session.insertUnit(QStringLiteral("agent-0"), QStringLiteral("测试"));
    QVERIFY(agent);

    const ToolResult r = runConfig(session, agent, {
        {QStringLiteral("setting"), QStringLiteral("modelName")},
    });
    QVERIFY(r.success);
    QCOMPARE(r.payload.value(QStringLiteral("operation")).toString(), QStringLiteral("get"));
    QCOMPARE(r.payload.value(QStringLiteral("setting")).toString(), QStringLiteral("modelName"));
}

QTEST_MAIN(ConfigToolTests)
#include "ConfigToolTests.moc"
