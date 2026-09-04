#include <QtTest>

#include "agent/AbstractOrchestration.h"
#include "agent/Agent.h"
#include "agent/AgentSession.h"
#include "config/SessionRuntime.h"
#include "providers/core/AbstractProvider.h"
#include "providers/ProviderTypes/ProviderAdapterTypes.h"
#include "providers/ProviderTypes/ProviderTypes.h"
#include "providers/service/ProviderCredential.h"
#include "skills/FileSkill.h"
#include "skills/FileSkillLoader.h"
#include "skills/SkillService.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

/**
 * skills 模块：FileSkill 辅助、SKILL.md frontmatter 解析、目录加载/查找、
 * 可见性裁剪与提示词块、SkillService 斜杠命令提交。
 */
class SkillTests final : public QObject
{
    Q_OBJECT

private slots:
    void fileSkill_helpers();
    void parseSkillFile_valid();
    void parseSkillFile_noFrontmatter();
    void parseSkillFile_multilineAndQuotes();
    void parseSkillFile_flags();
    void parseSkillFile_allowedTools();
    void parseSkillFile_missingFile();
    void loader_loadAndFind();
    void loader_userInvocableAndPromptBlock();
    void loader_removeAndCaseInsensitive();
    void buildSkillsPromptBlock_pure();
    void perUnitSkillsBlock_filteredBySkillVisible();
    void service_visibleSkillMessage();
    void service_submitWithSkill();
};

namespace {

QString skillMd(const QString &frontmatter, const QString &body)
{
    if (frontmatter.isEmpty())
        return body;
    return QStringLiteral("---\n%1\n---\n%2").arg(frontmatter, body);
}

bool writeSkillFile(const QString &dirPath, const QString &fileName, const QString &content)
{
    if (!QDir().mkpath(dirPath))
        return false;
    QFile file(QDir(dirPath).absoluteFilePath(fileName));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    file.write(content.toUtf8());
    return true;
}

// 假 Provider：捕获最后一次请求，异步完成一轮
class SkillFakeProvider final : public AbstractProvider
{
public:
    static ProviderRequest s_lastRequest;

    SkillFakeProvider()
        : AbstractProvider(QStringLiteral("skill-test"), nullptr)
    {
        seedAvailableModels({}); // 跳过模型刷新（避免真实 HTTP）
    }

    void setAuth(const ProviderAuth &auth) override
    {
        // 基类 setAuth 在 baseUrl/apiKey 变化时会 invalidateModelCatalog，
        // 触发真实 HTTP 模型刷新；测试里直接赋值跳过。
        m_auth = auth;
    }

protected:
    ProviderError validateProviderRequest(const ProviderRequest &) const override { return {}; }
    ProviderTransportRequest buildProviderTransportRequest(const ProviderRequest &request) const override
    {
        s_lastRequest = request;
        ProviderTransportRequest t;
        t.body = "{}";
        return t;
    }
    QList<ProviderEvent> parseProviderTransportPayload(const ProviderTransportPayload &) override { return {}; }
    void resetProviderTurnState() override {}
    bool startProviderTransportRequest(const ProviderTransportRequest &, ProviderError *) override
    {
        QTimer::singleShot(0, this, [this]() {
            ProviderMessageEnd end;
            end.messageId = QStringLiteral("skill-msg");
            end.stopReason = StopReason::EndTurn;
            emitProviderEvent(ProviderEvent::messageCompleted(end));
        });
        return true;
    }
    QUrl buildModelsUrl(const QString &) const override { return {}; }
    QList<ModelCapabilities> parseModelsPayload(const QByteArray &, QString *) const override { return {}; }
};

ProviderRequest SkillFakeProvider::s_lastRequest;

// 最小编排：skillVisible 按白名单裁剪
class TestOrchestration final : public AbstractOrchestration
{
public:
    QStringList visibleSkills;

    AbstractToolSource *toolSource() override { return nullptr; }
    void attach(AgentSession *session) override { m_session = session; }
    void detach() override { m_session = nullptr; }
    bool skillVisible(const Agent *unit, const QString &skillName) const override
    {
        Q_UNUSED(unit);
        return visibleSkills.contains(skillName);
    }

    AgentSession *m_session = nullptr;
};

} // namespace

void SkillTests::fileSkill_helpers()
{
    FileSkill skill;
    skill.dirName = QStringLiteral("git--bash");
    QCOMPARE(skill.displayName(), QStringLiteral("git--bash")); // name 空 → dirName
    QCOMPARE(skill.slashName(), QStringLiteral("bash"));
    QCOMPARE(skill.slash(), QStringLiteral("/bash"));

    skill.name = QStringLiteral("Git 助手");
    QCOMPARE(skill.displayName(), QStringLiteral("Git 助手"));
}

void SkillTests::parseSkillFile_valid()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path() + QStringLiteral("/git");
    QVERIFY(writeSkillFile(dir, QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("name: git\n"
                              "description: Git 操作助手\n"
                              "allowed-tools: [bash, git]\n"
                              "user-invocable: true"),
                QStringLiteral("## 用法\n执行 git 命令。"))));

    const auto skill = FileSkillLoader::parseSkillFile(dir + QStringLiteral("/SKILL.md"));
    QVERIFY(skill.has_value());
    QCOMPARE(skill->dirName, QStringLiteral("git"));
    QCOMPARE(skill->name, QStringLiteral("git"));
    QCOMPARE(skill->description, QStringLiteral("Git 操作助手"));
    QCOMPARE(skill->body, QStringLiteral("## 用法\n执行 git 命令。"));
    QCOMPARE(skill->allowedTools, QStringList({QStringLiteral("bash"), QStringLiteral("git")}));
    QVERIFY(skill->userInvocable);
    QVERIFY(!skill->disableModelInvocation);
}

void SkillTests::parseSkillFile_noFrontmatter()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path() + QStringLiteral("/plain");
    QVERIFY(writeSkillFile(dir, QStringLiteral("SKILL.md"), QStringLiteral("没有 frontmatter 的正文")));

    const auto skill = FileSkillLoader::parseSkillFile(dir + QStringLiteral("/SKILL.md"));
    QVERIFY(skill.has_value());
    QCOMPARE(skill->name, QString());
    QCOMPARE(skill->body, QStringLiteral("没有 frontmatter 的正文"));
    QVERIFY(skill->userInvocable); // 默认 true
}

void SkillTests::parseSkillFile_multilineAndQuotes()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path() + QStringLiteral("/multi");
    QVERIFY(writeSkillFile(dir, QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("name: \"带引号的名字\"\n"
                              "description: >-\n"
                              "  第一行\n"
                              "  第二行"),
                QStringLiteral("正文"))));

    const auto skill = FileSkillLoader::parseSkillFile(dir + QStringLiteral("/SKILL.md"));
    QVERIFY(skill.has_value());
    QCOMPARE(skill->name, QStringLiteral("带引号的名字"));
    QCOMPARE(skill->description, QStringLiteral("第一行 第二行"));
}

void SkillTests::parseSkillFile_flags()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path() + QStringLiteral("/flags");
    QVERIFY(writeSkillFile(dir, QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("user-invocable: false\n"
                              "disable-model-invocation: true"),
                QStringLiteral("正文"))));

    const auto skill = FileSkillLoader::parseSkillFile(dir + QStringLiteral("/SKILL.md"));
    QVERIFY(skill.has_value());
    QVERIFY(!skill->userInvocable);
    QVERIFY(skill->disableModelInvocation);
}

void SkillTests::parseSkillFile_allowedTools()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path() + QStringLiteral("/tools");
    QVERIFY(writeSkillFile(dir, QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("allowed-tools: bash, git,  "), QStringLiteral("正文"))));

    const auto skill = FileSkillLoader::parseSkillFile(dir + QStringLiteral("/SKILL.md"));
    QVERIFY(skill.has_value());
    QCOMPARE(skill->allowedTools, QStringList({QStringLiteral("bash"), QStringLiteral("git")}));
}

void SkillTests::parseSkillFile_missingFile()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto skill = FileSkillLoader::parseSkillFile(tmp.path() + QStringLiteral("/nope/SKILL.md"));
    QVERIFY(!skill.has_value());
}

void SkillTests::loader_loadAndFind()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(writeSkillFile(tmp.path() + QStringLiteral("/git"), QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("name: git\ndescription: Git 助手"), QStringLiteral("正文"))));
    QVERIFY(writeSkillFile(tmp.path() + QStringLiteral("/bash"), QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("name: bash\ndescription: Shell 助手"), QStringLiteral("正文"))));

    FileSkillLoader loader;
    QVERIFY(loader.loadAll().isEmpty()); // 未加目录
    loader.addSkillDirectory(tmp.path());
    QCOMPARE(loader.skillDirectories(), QStringList({QDir::cleanPath(tmp.path())}));

    const QList<FileSkill> all = loader.loadAll();
    QCOMPARE(all.size(), 2);

    const auto git = loader.findByDirName(QStringLiteral("/git"));
    QVERIFY(git.has_value());
    QCOMPARE(git->description, QStringLiteral("Git 助手"));
    const auto bash = loader.findByDirName(QStringLiteral("bash"));
    QVERIFY(bash.has_value());
    QVERIFY(!loader.findByDirName(QStringLiteral("/nope")).has_value());
    QVERIFY(!loader.findByDirName(QString()).has_value());
}

void SkillTests::loader_userInvocableAndPromptBlock()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(writeSkillFile(tmp.path() + QStringLiteral("/visible"), QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("name: visible\ndescription: 可见技能"), QStringLiteral("正文"))));
    QVERIFY(writeSkillFile(tmp.path() + QStringLiteral("/hidden"), QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("name: hidden\nuser-invocable: false"), QStringLiteral("正文"))));

    FileSkillLoader loader;
    loader.addSkillDirectory(tmp.path());
    const QList<FileSkill> visible = loader.userInvocableSkills();
    QCOMPARE(visible.size(), 1);
    QCOMPARE(visible.first().dirName, QStringLiteral("visible"));

    const QString block = loader.availableSkillsPromptBlock();
    QVERIFY(block.contains(QStringLiteral("<available_skills>")));
    QVERIFY(block.contains(QStringLiteral("/visible — 可见技能")));
    QVERIFY(!block.contains(QStringLiteral("hidden")));

    FileSkillLoader empty;
    QCOMPARE(empty.availableSkillsPromptBlock(), QString());
}

void SkillTests::loader_removeAndCaseInsensitive()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(writeSkillFile(tmp.path() + QStringLiteral("/a"), QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("name: a"), QStringLiteral("正文"))));
    QVERIFY(writeSkillFile(tmp.path() + QStringLiteral("/b"), QStringLiteral("skill.md"), // 小写回退
        skillMd(QStringLiteral("name: b"), QStringLiteral("正文"))));

    FileSkillLoader loader;
    loader.addSkillDirectory(tmp.path());
    QCOMPARE(loader.loadAll().size(), 2);

    loader.removeSkillDirectory(tmp.path());
    QVERIFY(loader.loadAll().isEmpty());
}

void SkillTests::buildSkillsPromptBlock_pure()
{
    QCOMPARE(FileSkillLoader::buildSkillsPromptBlock({}), QString());

    FileSkill git;
    git.dirName = QStringLiteral("git");
    git.description = QStringLiteral("Git 助手");
    FileSkill bash;
    bash.dirName = QStringLiteral("bash");
    bash.description = QStringLiteral("Shell 助手");

    const QString block = FileSkillLoader::buildSkillsPromptBlock({git, bash});
    QVERIFY(block.contains(QStringLiteral("<available_skills>")));
    QVERIFY(block.contains(QStringLiteral("/git — Git 助手")));
    QVERIFY(block.contains(QStringLiteral("/bash — Shell 助手")));
    QVERIFY(block.contains(QStringLiteral("</available_skills>")));
}

void SkillTests::perUnitSkillsBlock_filteredBySkillVisible()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(writeSkillFile(tmp.path() + QStringLiteral("/git"), QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("name: git\ndescription: Git 助手"), QStringLiteral("正文"))));
    QVERIFY(writeSkillFile(tmp.path() + QStringLiteral("/bash"), QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("name: bash\ndescription: Shell 助手"), QStringLiteral("正文"))));

    FileSkillLoader loader;
    loader.addSkillDirectory(tmp.path());

    TestOrchestration orch;
    orch.visibleSkills = {QStringLiteral("git")};

    ProviderCredential cred;
    const QString instanceId = cred.createInstance(QStringLiteral("skill-test"),
                                                   QStringLiteral("skill"),
                                                   QStringLiteral("https://example.test"),
                                                   QStringLiteral("key"));
    QVERIFY(!instanceId.isEmpty());

    SessionRuntime defaults;
    defaults.workingDirectory = tmp.path();
    defaults.systemPrompt = QStringLiteral("技能测试");
    defaults.compactEnabled = false;
    defaults.providerType = QStringLiteral("skill-test");
    defaults.credentialInstanceId = instanceId;

    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    cfg.credentialStore = &cred;
    cfg.skillLoader = &loader;
    cfg.orchestration = &orch;
    cfg.providerFactory = [](const QString &) {
        return std::make_unique<SkillFakeProvider>();
    };

    AgentSession session(cfg);
    session.setRuntime(defaults);
    Agent *agent = session.insertUnit(QStringLiteral("agent-0"), QStringLiteral("测试"));
    QVERIFY(agent);

    // 开一轮 → 假 Provider 捕获请求 → 系统提示词只含 git 技能块
    SkillFakeProvider::s_lastRequest = ProviderRequest{};
    QVERIFY(agent->submitUserDelivery(QStringLiteral("hello"), {},
                                      AbstractLoop::UserDelivery::NextTurn));
    QTRY_VERIFY_WITH_TIMEOUT(!SkillFakeProvider::s_lastRequest.items.isEmpty(), 3000);

    const QString systemPrompt = SkillFakeProvider::s_lastRequest.systemPrompt;
    QVERIFY(!systemPrompt.isEmpty());
    QVERIFY(systemPrompt.contains(QStringLiteral("<available_skills>")));
    QVERIFY(systemPrompt.contains(QStringLiteral("/git — Git 助手")));
    QVERIFY(!systemPrompt.contains(QStringLiteral("bash")));
}

void SkillTests::service_visibleSkillMessage()
{
    FileSkill skill;
    skill.dirName = QStringLiteral("git");
    QCOMPARE(SkillService::visibleSkillMessage(skill, QString()), QStringLiteral("/git"));
    QCOMPARE(SkillService::visibleSkillMessage(skill, QStringLiteral("  status  ")),
             QStringLiteral("/git status"));
}

void SkillTests::service_submitWithSkill()
{
    // 空指针 → false
    FileSkillLoader loader;
    QVERIFY(!SkillService::submitWithSkill(nullptr, nullptr, QStringLiteral("/x"), {}, {}));
    QVERIFY(!SkillService::submitWithSkill(&loader, nullptr, QStringLiteral("/x"), {}, {}));

    // 构造技能目录 + Agent
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(writeSkillFile(tmp.path() + QStringLiteral("/git"), QStringLiteral("SKILL.md"),
        skillMd(QStringLiteral("name: git\ndescription: Git 助手"), QStringLiteral("## 用法\ngit 命令"))));

    FileSkillLoader skillLoader;
    skillLoader.addSkillDirectory(tmp.path());

    // Provider 配置：凭据实例 + 工厂（canStartTurn 要求 credentialInstanceId + store + factory）
    ProviderCredential cred;
    const QString instanceId = cred.createInstance(QStringLiteral("skill-test"),
                                                   QStringLiteral("skill"),
                                                   QStringLiteral("https://example.test"),
                                                   QStringLiteral("key"));
    QVERIFY(!instanceId.isEmpty());

    SessionRuntime rt;
    rt.workingDirectory = tmp.path();
    rt.systemPrompt = QStringLiteral("技能测试");
    rt.compactEnabled = false;
    rt.providerType = QStringLiteral("skill-test");
    rt.credentialInstanceId = instanceId;
    Agent agent(QStringLiteral("agent-0"), QStringLiteral("测试"), rt);
    agent.setCredentialStore(&cred);
    agent.setProviderFactory([](const QString &) {
        return std::make_unique<SkillFakeProvider>();
    });

    // 未找到技能 → false
    QVERIFY(!SkillService::submitWithSkill(&skillLoader, &agent, QStringLiteral("/nope"),
                                           QStringLiteral("text"), {}));

    // 找到技能 → 提交成功，假 Provider 收到含 "/git status" 的请求
    SkillFakeProvider::s_lastRequest = ProviderRequest{};
    QVERIFY(SkillService::submitWithSkill(&skillLoader, &agent, QStringLiteral("/git"),
                                          QStringLiteral("status"), {}));
    QTRY_VERIFY_WITH_TIMEOUT(!SkillFakeProvider::s_lastRequest.items.isEmpty(), 3000);
    bool foundMessage = false;
    for (const ProviderItem &item : SkillFakeProvider::s_lastRequest.items) {
        for (const ProviderMessagePart &part : item.parts) {
            if (part.text.contains(QStringLiteral("/git status"))) {
                foundMessage = true;
            }
        }
    }
    QVERIFY(foundMessage);
}

QTEST_MAIN(SkillTests)
#include "SkillTests.moc"
