#include "agent/AbstractOrchestration.h"
#include "agent/Agent.h"
#include "agent/AgentSession.h"
#include "agent/compact/CompactPipeline.h"
#include "config/SessionRuntime.h"
#include "tools/AbstractBuiltinTool.h"
#include "tools/BuiltinToolRuntime.h"
#include "tools/ToolCoordinator.h"
#include "types/ConversationMessage.h"
#include "types/MediaAsset.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

class TitleOrch final : public AbstractOrchestration
{
public:
    AbstractToolSource *toolSource() override { return nullptr; }
    void attach(AgentSession *session) override { m_session = session; }
    void detach() override { m_session = nullptr; }
    bool ownsSessionTitle(const Agent *) const override { return true; }

private:
    AgentSession *m_session = nullptr;
};

class RestoreOrch final : public AbstractOrchestration
{
public:
    AbstractToolSource *toolSource() override { return nullptr; }
    void attach(AgentSession *session) override { m_session = session; }
    void detach() override { m_session = nullptr; }
    Agent *createUnit(const UnitCreateRequest &request) override
    {
        createdIds.append(request.agentId);
        if (!m_session || request.agentId.isEmpty()) {
            return nullptr;
        }
        return m_session->insertUnit(request.agentId,
                                     request.displayName.isEmpty() ? request.agentId
                                                                   : request.displayName);
    }
    QStringList createdIds;

private:
    AgentSession *m_session = nullptr;
};

SessionRuntime makeRuntime(const QString &workDir)
{
    SessionRuntime runtime;
    runtime.workingDirectory = workDir;
    runtime.systemPrompt = QStringLiteral("boundary");
    runtime.compactEnabled = false;
    runtime.summaryEnabled = false;
    return runtime;
}

} // namespace

class KernelBoundaryTests final : public QObject
{
    Q_OBJECT

private slots:
    void applyRuntime_reachesAllUnits();
    void titleGenerator_runsWhenUntitled();
    void titleGenerator_skipsHardcodedNewSession();
    void importLedger_goesThroughCreateUnit();
    void notifyFileWritten_invalidatesOtherUnitCache();
    void emptyBuiltinTools_hidesGrep();
    void compactPipeline_cancelWhenIdle();
    void mediaAssets_audioVideoRoundTrip();
};

void KernelBoundaryTests::applyRuntime_reachesAllUnits()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    SessionRuntime defaults = makeRuntime(tmp.path());
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    AgentSession session(cfg);
    session.setRuntime(defaults);
    Agent *a = session.insertUnit(QStringLiteral("a"), QStringLiteral("A"));
    Agent *b = session.insertUnit(QStringLiteral("b"), QStringLiteral("B"));
    QVERIFY(a);
    QVERIFY(b);

    QVERIFY(session.setRuntimeField(QStringLiteral("modelName"), QStringLiteral("m-all")));
    QCOMPARE(a->runtime().modelName, QStringLiteral("m-all"));
    QCOMPARE(b->runtime().modelName, QStringLiteral("m-all"));
}

void KernelBoundaryTests::titleGenerator_runsWhenUntitled()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    SessionRuntime defaults = makeRuntime(tmp.path());
    TitleOrch orch;
    QString captured;
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    cfg.orchestration = &orch;
    cfg.titleGenerator = [&](const QString &userText, AgentSessionConfig::TitleReady done) {
        captured = userText;
        done(QStringLiteral("生成标题"));
    };
    AgentSession session(cfg);
    session.setRuntime(defaults);
    Agent *unit = session.insertUnit(QStringLiteral("agent-0"), QStringLiteral("Main"));
    QVERIFY(unit);

    ConversationMessage user;
    user.kind = ConversationMessage::Kind::UserText;
    user.text = QStringLiteral("hello title");
    user.createdAtMs = 10;
    unit->loop()->appendExternalMessage(user);
    ConversationMessage assistant;
    assistant.kind = ConversationMessage::Kind::AssistantText;
    assistant.text = QStringLiteral("ok");
    assistant.createdAtMs = 11;
    unit->loop()->appendExternalMessage(assistant);

    unit->setManagerStatus(core_ir::AgentStatus::Queued);
    unit->setManagerStatus(core_ir::AgentStatus::Idle);
    QCOMPARE(captured, QStringLiteral("hello title"));
    QCOMPARE(session.title(), QStringLiteral("生成标题"));
}

void KernelBoundaryTests::titleGenerator_skipsHardcodedNewSession()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    SessionRuntime defaults = makeRuntime(tmp.path());
    TitleOrch orch;
    bool called = false;
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    cfg.orchestration = &orch;
    cfg.titleGenerator = [&](const QString &, AgentSessionConfig::TitleReady done) {
        called = true;
        done(QStringLiteral("x"));
    };
    AgentSession session(cfg);
    session.setRuntime(defaults);
    session.setTitle(QStringLiteral("新会话"));
    Agent *unit = session.insertUnit(QStringLiteral("agent-0"), QStringLiteral("Main"));
    QVERIFY(unit);

    ConversationMessage user;
    user.kind = ConversationMessage::Kind::UserText;
    user.text = QStringLiteral("hello");
    user.createdAtMs = 10;
    unit->loop()->appendExternalMessage(user);
    ConversationMessage assistant;
    assistant.kind = ConversationMessage::Kind::AssistantText;
    assistant.text = QStringLiteral("ok");
    assistant.createdAtMs = 11;
    unit->loop()->appendExternalMessage(assistant);
    unit->setManagerStatus(core_ir::AgentStatus::Queued);
    unit->setManagerStatus(core_ir::AgentStatus::Idle);
    QVERIFY(!called);
    QCOMPARE(session.title(), QStringLiteral("新会话"));
}

void KernelBoundaryTests::importLedger_goesThroughCreateUnit()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    SessionRuntime defaults = makeRuntime(tmp.path());
    RestoreOrch orch;
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    cfg.orchestration = &orch;
    AgentSession session(cfg);
    session.setRuntime(defaults);

    QJsonObject agentObj;
    agentObj.insert(QStringLiteral("agentId"), QStringLiteral("restored-7"));
    agentObj.insert(QStringLiteral("displayName"), QStringLiteral("Restored"));
    agentObj.insert(QStringLiteral("isPrimary"), true);
    QJsonObject root;
    QJsonArray agents;
    agents.append(agentObj);
    root.insert(QStringLiteral("agents"), agents);

    session.importLedger(root);
    QCOMPARE(orch.createdIds, QStringList{QStringLiteral("restored-7")});
    QVERIFY(session.findById(QStringLiteral("restored-7")));
}

void KernelBoundaryTests::notifyFileWritten_invalidatesOtherUnitCache()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    SessionRuntime defaults = makeRuntime(tmp.path());
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    AgentSession session(cfg);
    session.setRuntime(defaults);
    Agent *writer = session.insertUnit(QStringLiteral("w"), QStringLiteral("W"));
    Agent *reader = session.insertUnit(QStringLiteral("r"), QStringLiteral("R"));
    QVERIFY(writer);
    QVERIFY(reader);

    const QString path = tmp.filePath(QStringLiteral("x.txt"));
    BuiltinToolRuntime::ReadFileState state;
    state.content = QStringLiteral("stale");
    state.timestampMs = 1;
    reader->loop()->builtinRuntime().setReadFileState(path, state);
    QCOMPARE(reader->loop()->builtinRuntime().readFileState(path).content,
             QStringLiteral("stale"));

    session.notifyFileWritten(writer->agentId(), path);
    QVERIFY(reader->loop()->builtinRuntime().readFileState(path).content.isEmpty());
}

void KernelBoundaryTests::emptyBuiltinTools_hidesGrep()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    SessionRuntime defaults = makeRuntime(tmp.path());
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    cfg.builtinTools = QList<std::shared_ptr<AbstractBuiltinTool>>{};
    AgentSession session(cfg);
    session.setRuntime(defaults);
    QVERIFY(session.insertUnit(QStringLiteral("agent-0"), QStringLiteral("Main")));

    bool sawGrep = false;
    for (const ToolSpec &spec : session.coordinator()->allSpecs()) {
        if (spec.name == QLatin1String("grep")) {
            sawGrep = true;
        }
    }
    QVERIFY(!sawGrep);
}

void KernelBoundaryTests::compactPipeline_cancelWhenIdle()
{
    CompactPipeline pipeline(QStringLiteral("agent-0"));
    QVERIFY(!pipeline.cancelBoundaryWait());
    pipeline.cancelInFlight();
    QVERIFY(!pipeline.requestManualCompaction());
    QVERIFY(!pipeline.hasQueue());
    pipeline.ensureInstalled(true);
    QVERIFY(pipeline.hasQueue());
}

void KernelBoundaryTests::mediaAssets_audioVideoRoundTrip()
{
    const auto audio = ProviderAudioAsset::fromUrl(QStringLiteral("https://ex/a.wav"),
                                                   QStringLiteral("audio/wav"),
                                                   QStringLiteral("hi"));
    QVERIFY(audio.hasUri());
    QVERIFY(!audio.isEmpty());
    const auto video = ProviderVideoAsset::fromBytes(QByteArray("mp4"),
                                                     QStringLiteral("video/mp4"));
    QVERIFY(video.hasInlineData());
    ProviderBlobRef blob;
    blob.blobId = QStringLiteral("b1");
    const auto fromBlob = ProviderAudioAsset::fromBlob(blob, QStringLiteral("audio/mpeg"));
    QVERIFY(fromBlob.hasBlobRef());
    QCOMPARE(fromBlob.blobRef.scheme, ProviderUriScheme::Blob);
}

QTEST_MAIN(KernelBoundaryTests)
#include "KernelBoundaryTests.moc"
