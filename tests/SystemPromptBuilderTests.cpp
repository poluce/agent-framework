#include "config/SystemPromptBuilder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryFile>
#include <QtTest>

class SystemPromptBuilderTests : public QObject
{
    Q_OBJECT

private slots:
    void roleCustomPrompt_appendsAfterTemplate();
    void roleCustomPrompt_emptyDoesNotAdd();
    void roleCustomPrompt_doesNotUseUserSlot();
    void roleCustomPrompt_appliesPlaceholders();
    void baseBehavior_defaultNeutral();
    void baseBehavior_setBaseBehaviorOverridesDefault();
    void baseBehavior_promptPathsBaseFileOverridesDefault();
    void baseBehavior_externalDirectoryOverridesBuiltin();
    void baseBehavior_invalidPathFallsBackToDefault();
};

void SystemPromptBuilderTests::roleCustomPrompt_appendsAfterTemplate()
{
    const QString dir = QCoreApplication::applicationDirPath()
        + QStringLiteral("/system_prompts");
    QVERIFY(QDir().mkpath(dir));
    QFile file(dir + QStringLiteral("/role_card.md"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    file.write("TEMPLATE_MARK {agentId}");
    file.close();

    SystemPromptBuilder builder;
    builder.setRoleCustomPrompt(QStringLiteral("角色卡正文"));
    AgentPromptContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    ctx.rolePromptFile = QStringLiteral("role_card.md");
    const QString prompt = builder.buildPrompt(ctx);
    const int tmpl = prompt.indexOf(QStringLiteral("TEMPLATE_MARK agent-0"));
    const int custom = prompt.indexOf(QStringLiteral("角色卡正文"));
    QVERIFY(tmpl >= 0);
    QVERIFY(custom >= 0);
    QVERIFY(tmpl < custom);
}

void SystemPromptBuilderTests::roleCustomPrompt_emptyDoesNotAdd()
{
    SystemPromptBuilder builder;
    builder.setRoleCustomPrompt(QStringLiteral("   "));
    QCOMPARE(builder.roleCustomPrompt(), QString());
    AgentPromptContext ctx;
    const QString prompt = builder.buildPrompt(ctx);
    QVERIFY(!prompt.contains(QStringLiteral("角色卡正文")));
}

void SystemPromptBuilderTests::roleCustomPrompt_doesNotUseUserSlot()
{
    SystemPromptBuilder builder;
    builder.setUserCustomPrompt(QStringLiteral("USER_SLOT"));
    builder.setRoleCustomPrompt(QStringLiteral("ROLE_SLOT"));
    AgentPromptContext ctx;
    const QString prompt = builder.buildPrompt(ctx);
    const int user = prompt.indexOf(QStringLiteral("USER_SLOT"));
    const int role = prompt.indexOf(QStringLiteral("ROLE_SLOT"));
    QVERIFY(user >= 0);
    QVERIFY(role >= 0);
    QVERIFY(user < role);
    QCOMPARE(builder.userCustomPrompt(), QStringLiteral("USER_SLOT"));
    QCOMPARE(builder.roleCustomPrompt(), QStringLiteral("ROLE_SLOT"));
}

void SystemPromptBuilderTests::roleCustomPrompt_appliesPlaceholders()
{
    SystemPromptBuilder builder;
    builder.setRoleCustomPrompt(QStringLiteral("卡 {agentId}/{displayName}"));
    AgentPromptContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    ctx.displayName = QStringLiteral("主单元");
    const QString prompt = builder.buildPrompt(ctx);
    QVERIFY(prompt.contains(QStringLiteral("卡 agent-0/主单元")));
    QVERIFY(!prompt.contains(QStringLiteral("{agentId}")));
}

void SystemPromptBuilderTests::baseBehavior_defaultNeutral()
{
    SystemPromptBuilder builder;
    AgentPromptContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    const QString prompt = builder.buildPrompt(ctx);

    QVERIFY(!prompt.contains(QStringLiteral("agent_qt")));
    QVERIFY(prompt.contains(QStringLiteral("你是桌面执行型智能体")));
    QVERIFY(prompt.contains(QStringLiteral("工作原则")));
    QVERIFY(prompt.contains(QStringLiteral("直接行动")));
}

void SystemPromptBuilderTests::baseBehavior_setBaseBehaviorOverridesDefault()
{
    SystemPromptBuilder builder;
    builder.setBaseBehavior(QStringLiteral("CUSTOM_IN_MEMORY_BASE"));
    builder.prepare();

    AgentPromptContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    const QString prompt = builder.buildPrompt(ctx);

    QVERIFY(prompt.contains(QStringLiteral("CUSTOM_IN_MEMORY_BASE")));
    QVERIFY(!prompt.contains(QStringLiteral("你是桌面执行型智能体")));
    QCOMPARE(builder.baseBehavior(), QStringLiteral("CUSTOM_IN_MEMORY_BASE"));
}

void SystemPromptBuilderTests::baseBehavior_promptPathsBaseFileOverridesDefault()
{
    QTemporaryFile tempFile;
    QVERIFY(tempFile.open());
    tempFile.write("CUSTOM_FILE_BASE_BEHAVIOR");
    tempFile.flush();

    SystemPromptBuilder::PromptPaths paths;
    paths.basePromptFile = tempFile.fileName();
    SystemPromptBuilder builder(paths);

    AgentPromptContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    const QString prompt = builder.buildPrompt(ctx);

    QVERIFY(prompt.contains(QStringLiteral("CUSTOM_FILE_BASE_BEHAVIOR")));
    QVERIFY(!prompt.contains(QStringLiteral("你是桌面执行型智能体")));
}

void SystemPromptBuilderTests::baseBehavior_externalDirectoryOverridesBuiltin()
{
    const QString dir = QCoreApplication::applicationDirPath()
        + QStringLiteral("/system_prompts");
    QVERIFY(QDir().mkpath(dir));
    const QString externalBasePath = dir + QStringLiteral("/base.md");

    QFile file(externalBasePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    file.write("EXTERNAL_OVERRIDE_BASE");
    file.close();

    SystemPromptBuilder builder;
    AgentPromptContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    const QString prompt = builder.buildPrompt(ctx);

    QFile::remove(externalBasePath);

    QVERIFY(prompt.contains(QStringLiteral("EXTERNAL_OVERRIDE_BASE")));
    QVERIFY(!prompt.contains(QStringLiteral("你是桌面执行型智能体")));
}

void SystemPromptBuilderTests::baseBehavior_invalidPathFallsBackToDefault()
{
    SystemPromptBuilder::PromptPaths paths;
    paths.basePromptFile = QStringLiteral("/non_existent_dir/non_existent_file.md");
    SystemPromptBuilder builder(paths);

    AgentPromptContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    const QString prompt = builder.buildPrompt(ctx);

    QVERIFY(prompt.contains(QStringLiteral("你是桌面执行型智能体")));
}

QTEST_MAIN(SystemPromptBuilderTests)
#include "SystemPromptBuilderTests.moc"
