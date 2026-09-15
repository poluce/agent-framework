#include <QtTest>

#include "agent/AbstractLoop.h"
#include "agent/Agent.h"
#include "agent/ProviderRunLedger.h"
#include "agent/compact/CompactEngine.h"
#include "agent/compact/CompactPipeline.h"
#include "agent/compact/CompactPolicy.h"
#include "providers/core/AbstractProvider.h"
#include "providers/service/ProviderCredential.h"

#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

class CompactEngineReplayTests final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void replayItems_skipsCompacted();
    void buildBulkReplay_prefixThenLedgerThenInstruction();
    void buildBulkReplay_emptyWhenNoProviderRecords();
    void start_sendsReplayPrefixNotDocument();
    void start_fallsBackToDocumentWithoutReplayItems();
    void estimatedTokensForEntries_matchesItemEstimates();
    void pruneToolResults_skipsCompactionWhenBelowThreshold();
    void continueAfterCompaction_failsWhenStillOverWindow();
    void overflowRecovery_triggersCompactionAndRetriesTurn();
};

namespace {

constexpr auto kReplayProvider = "compact-replay-test";

ProviderRequest g_lastRequest;
int g_startCount = 0;

QString itemText(const ProviderItem &item)
{
    if (!item.parts.isEmpty()) {
        return item.parts.first().text;
    }
    return item.output;
}

class ReplayFakeProvider final : public AbstractProvider
{
public:
    ReplayFakeProvider()
        : AbstractProvider(QString::fromLatin1(kReplayProvider), nullptr)
    {
        seedAvailableModels({});
    }

protected:
    ProviderError validateProviderRequest(const ProviderRequest &request) const override
    {
        g_lastRequest = request;
        return {};
    }
    ProviderTransportRequest buildProviderTransportRequest(const ProviderRequest &) const override
    {
        ProviderTransportRequest t;
        t.body = "{}";
        return t;
    }
    QList<ProviderEvent> parseProviderTransportPayload(const ProviderTransportPayload &) override
    {
        return {};
    }
    void resetProviderTurnState() override {}
    bool startProviderTransportRequest(const ProviderTransportRequest &, ProviderError *) override
    {
        ++g_startCount;
        QTimer::singleShot(0, this, [this]() {
            emitProviderEvent(ProviderEvent::fromTextDelta(QStringLiteral(
                "## Primary Request and Intent\n- compact replay unit test\n"
                "## Key Technical Concepts\n- (none)\n"
                "## Files and Code\n- (none)\n"
                "## Errors and Fixes\n- (none)\n"
                "## Pending Jobs\n- (none)\n"
                "## Current Work\n- fake provider returned a valid checkpoint\n"
                "## Next Step\n- (none)\n"
                "## Critical Context\n- (none)")));
            ProviderMessageEnd end;
            end.messageId = QStringLiteral("replay-msg");
            end.stopReason = StopReason::EndTurn;
            emitProviderEvent(ProviderEvent::messageCompleted(end));
        });
        return true;
    }
    QUrl buildModelsUrl(const QString &) const override { return {}; }
    QList<ModelCapabilities> parseModelsPayload(const QByteArray &, QString *) const override
    {
        return {};
    }
};

class OverflowRecoveryFakeProvider final : public AbstractProvider
{
public:
    explicit OverflowRecoveryFakeProvider(int *turnRequestCount)
        : AbstractProvider(QString::fromLatin1(kReplayProvider), nullptr)
        , m_turnRequestCount(turnRequestCount)
    {
        setAuth({QStringLiteral("https://example.test"), QStringLiteral("key"), QStringLiteral("test-model")});
        ModelCapabilities caps;
        caps.modelId = QStringLiteral("test-model");
        caps.enable(ProviderCapability::TextInput).enable(ProviderCapability::TextOutput);
        seedAvailableModels({caps});
    }

protected:
    ProviderError validateProviderRequest(const ProviderRequest &req) const override
    {
        m_lastRequestIsCompaction = !req.items.isEmpty()
            && itemText(req.items.last()).contains(QStringLiteral("Primary Request and Intent"));
        return {};
    }
    ProviderTransportRequest buildProviderTransportRequest(const ProviderRequest &) const override
    {
        ProviderTransportRequest t;
        t.body = "{}";
        return t;
    }
    QList<ProviderEvent> parseProviderTransportPayload(const ProviderTransportPayload &) override { return {}; }
    void resetProviderTurnState() override {}
    bool startProviderTransportRequest(const ProviderTransportRequest &, ProviderError *) override
    {
        QTimer::singleShot(0, this, [this]() {
            if (m_lastRequestIsCompaction) {
                emitProviderEvent(ProviderEvent::fromTextDelta(QStringLiteral(
                    "## Primary Request and Intent\n- recover from overflow\n"
                    "## Key Technical Concepts\n- (none)\n"
                    "## Files and Code\n- (none)\n"
                    "## Errors and Fixes\n- (none)\n"
                    "## Pending Jobs\n- (none)\n"
                    "## Current Work\n- compacted\n"
                    "## Next Step\n- (none)\n"
                    "## Critical Context\n- (none)")));
                ProviderMessageEnd end;
                end.messageId = QStringLiteral("compact-end");
                end.stopReason = StopReason::EndTurn;
                emitProviderEvent(ProviderEvent::messageCompleted(end));
                return;
            }

            if (m_turnRequestCount) {
                ++(*m_turnRequestCount);
                if (*m_turnRequestCount == 1) {
                    ProviderError err;
                    err.code = QString::fromLatin1(ProviderErrorCodes::ContextWindowExceeded);
                    err.message = QStringLiteral("maximum context length exceeded");
                    emitProviderEvent(ProviderEvent::fromError(err));
                    return;
                }
            }

            emitProviderEvent(ProviderEvent::fromTextDelta(QStringLiteral("ok turn done")));
            ProviderMessageEnd end;
            end.messageId = QStringLiteral("turn-end");
            end.stopReason = StopReason::EndTurn;
            emitProviderEvent(ProviderEvent::messageCompleted(end));
        });
        return true;
    }
    QUrl buildModelsUrl(const QString &) const override { return {}; }
    QList<ModelCapabilities> parseModelsPayload(const QByteArray &, QString *) const override { return {}; }

private:
    int *m_turnRequestCount = nullptr;
    mutable bool m_lastRequestIsCompaction = false;
};

QString makeCredential(ProviderCredential *cred)
{
    return cred->createInstance(
        QString::fromLatin1(kReplayProvider),
        QStringLiteral("replay"),
        QStringLiteral("https://example.test"),
        QStringLiteral("key"));
}

} // namespace

void CompactEngineReplayTests::initTestCase()
{
    Q_INIT_RESOURCE(system_prompts);
}

void CompactEngineReplayTests::replayItems_skipsCompacted()
{
    ProviderRunLedger ledger;
    const QString keepId = ledger.appendProviderItem(
        ProviderItem::makeUserText(QStringLiteral("keep-me")));
    const QString dropId = ledger.appendProviderItem(
        ProviderItem::makeAssistantText(QStringLiteral("drop-me")));
    QVERIFY(ledger.markEntriesCompacted({dropId}));

    const QList<ProviderItem> items = ledger.replayItemsForEntries({keepId, dropId});
    QCOMPARE(items.size(), 1);
    QCOMPARE(itemText(items.first()), QStringLiteral("keep-me"));
}

void CompactEngineReplayTests::buildBulkReplay_prefixThenLedgerThenInstruction()
{
    ProviderRunLedger ledger;
    const QString userId = ledger.appendProviderItem(
        ProviderItem::makeUserText(QStringLiteral("hello user")));
    const QString asstId = ledger.appendProviderItem(
        ProviderItem::makeAssistantText(QStringLiteral("hello assistant")));

    const QList<ProviderItem> items = CompactEngine::buildBulkReplayItems(
        ledger,
        {userId, asstId},
        {QStringLiteral("prior summary")},
        QStringLiteral("COMPACT-INSTRUCTION"));

    QCOMPARE(items.size(), 4);
    QVERIFY(itemText(items.at(0)).contains(QStringLiteral("<compacted-summary>")));
    QVERIFY(itemText(items.at(0)).contains(QStringLiteral("prior summary")));
    QCOMPARE(itemText(items.at(1)), QStringLiteral("hello user"));
    QCOMPARE(itemText(items.at(2)), QStringLiteral("hello assistant"));
    QCOMPARE(itemText(items.at(3)), QStringLiteral("COMPACT-INSTRUCTION"));
}

void CompactEngineReplayTests::buildBulkReplay_emptyWhenNoProviderRecords()
{
    ProviderRunLedger ledger;
    const QList<ProviderItem> items = CompactEngine::buildBulkReplayItems(
        ledger,
        {QStringLiteral("missing")},
        {QStringLiteral("prefix")},
        QStringLiteral("INSTRUCTION"));
    QVERIFY(items.isEmpty());
}

void CompactEngineReplayTests::start_sendsReplayPrefixNotDocument()
{
    g_lastRequest = {};
    g_startCount = 0;

    ProviderRunLedger ledger;
    ledger.appendProviderItem(ProviderItem::makeUserText(QStringLiteral("user turn")));
    ledger.appendProviderItem(ProviderItem::makeAssistantText(QStringLiteral("assistant turn")));

    ProviderCredential cred;
    const QString instanceId = makeCredential(&cred);
    QVERIFY(!instanceId.isEmpty());

    CompactBulkReplay replay;
    replay.systemPrompt = QStringLiteral("CONVERSATION-SYSTEM");
    ProviderToolSpecification tool;
    tool.name = QStringLiteral("bash");
    tool.description = QStringLiteral("run a command");
    replay.tools = {tool};
    replay.modelViewPrefixTexts = {QStringLiteral("[上下文摘要]\nlabeled")};

    CompactEngine engine;
    engine.config.retainTokenCount = 0;
    engine.config.maxRetries = 0;
    bool finished = false;
    QObject::connect(&engine, &CompactEngine::compactionFinished, this,
                     [&](bool) { finished = true; });

    engine.start(
        &ledger,
        instanceId,
        &cred,
        [](const QString &) { return std::make_unique<ReplayFakeProvider>(); },
        QStringLiteral("test-model"),
        nullptr,
        replay);

    QTRY_VERIFY_WITH_TIMEOUT(finished, 3000);
    QCOMPARE(g_startCount, 1);
    QCOMPARE(g_lastRequest.systemPrompt, QStringLiteral("CONVERSATION-SYSTEM"));
    QCOMPARE(g_lastRequest.tools.size(), 1);
    QCOMPARE(g_lastRequest.tools.first().name, QStringLiteral("bash"));
    QVERIFY(g_lastRequest.items.size() >= 3);
    QCOMPARE(itemText(g_lastRequest.items.first()), QStringLiteral("[上下文摘要]\nlabeled"));
    QCOMPARE(itemText(g_lastRequest.items.at(1)), QStringLiteral("user turn"));
    const QString last = itemText(g_lastRequest.items.last());
    QVERIFY(last.contains(QStringLiteral("Primary Request and Intent")));
    QVERIFY(last.contains(QStringLiteral("compacted-summary")));
    QVERIFY(!itemText(g_lastRequest.items.at(1)).startsWith(QStringLiteral("## ")));
}

void CompactEngineReplayTests::start_fallsBackToDocumentWithoutReplayItems()
{
    g_lastRequest = {};
    g_startCount = 0;

    ProviderRunLedger ledger;
    ConversationMessage orphan;
    orphan.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    orphan.kind = ConversationMessage::Kind::Error;
    orphan.text = QStringLiteral("only ui projection");
    orphan.submittedToModel = true;
    ledger.appendUiIngress(orphan);
    ledger.appendProviderItem(ProviderItem::makeUserText(QStringLiteral("tail user")));

    ProviderCredential cred;
    const QString instanceId = makeCredential(&cred);
    QVERIFY(!instanceId.isEmpty());

    CompactEngine engine;
    engine.config.retainTokenCount = 0;
    engine.config.maxRetries = 0;
    bool finished = false;
    QObject::connect(&engine, &CompactEngine::compactionFinished, this,
                     [&](bool) { finished = true; });

    engine.start(
        &ledger,
        instanceId,
        &cred,
        [](const QString &) { return std::make_unique<ReplayFakeProvider>(); },
        QStringLiteral("test-model"));

    QTRY_VERIFY_WITH_TIMEOUT(finished, 3000);
    QCOMPARE(g_startCount, 1);
    QVERIFY(g_lastRequest.tools.isEmpty());
    QCOMPARE(g_lastRequest.items.size(), 1);
    QVERIFY(g_lastRequest.systemPrompt.contains(QStringLiteral("Primary Request and Intent")));
    QVERIFY(g_lastRequest.systemPrompt.contains(QStringLiteral("compacted-summary")));
}

void CompactEngineReplayTests::estimatedTokensForEntries_matchesItemEstimates()
{
    ProviderRunLedger ledger;
    const QString uId = ledger.appendProviderItem(
        ProviderItem::makeUserText(QString(200, QLatin1Char('u'))));
    const QString callId = ledger.appendProviderItem(
        ProviderItem::makeFunctionCall(QStringLiteral("c1"), QStringLiteral("tool"),
                                       QJsonObject{}, QString(400, QLatin1Char('a'))));

    const qint64 total = ledger.estimatedTokensForEntries({uId, callId});
    QVERIFY(total > 100);

    const qint64 callOnly = ledger.estimatedTokensForEntries({callId});
    QVERIFY(callOnly > 60);
    QVERIFY(callOnly < total);
}

void CompactEngineReplayTests::pruneToolResults_skipsCompactionWhenBelowThreshold()
{
    AbstractLoop loop;
    CompactPipeline pipeline(QStringLiteral("test-agent"));
    pipeline.setLoop(&loop);

    SessionRuntime runtime;
    runtime.workingDirectory = QStringLiteral("/tmp");
    runtime.compactEnabled = true;
    runtime.summaryEnabled = false;
    runtime.contextWindow = 10000;
    runtime.providerType = QString::fromLatin1(kReplayProvider);
    loop.activateConfig(runtime);
    loop.setProviderFactory([](const QString &) {
        return std::make_unique<ReplayFakeProvider>();
    });
    pipeline.setRuntime(runtime);

    loop.ledger().appendProviderItem(ProviderItem::makeUserText(QStringLiteral("hello")));
    loop.ledger().appendProviderItem(ProviderItem::makeFunctionCall(
        QStringLiteral("c1"), QStringLiteral("bash"), QJsonObject{}, QStringLiteral("{}")));
    loop.ledger().appendProviderItem(ProviderItem::makeFunctionCallOutput(
        QStringLiteral("c1"), QStringLiteral("bash"), QString(9000, QLatin1Char('x'))));

    const qint64 unpruned = loop.ledger().estimatedContextTokens();
    const qint64 threshold = unpruned - 200;

    pipeline.onCompactionRequested(unpruned, threshold);

    QVERIFY(!pipeline.isCompacting());
    QVERIFY(loop.ledger().estimatedContextTokens() <= threshold);
}

void CompactEngineReplayTests::continueAfterCompaction_failsWhenStillOverWindow()
{
    AbstractLoop loop;
    SessionRuntime runtime;
    runtime.workingDirectory = QStringLiteral("/tmp");
    runtime.compactEnabled = true;
    runtime.contextWindow = 50;

    loop.activateConfig(runtime);
    loop.ledger().appendProviderItem(ProviderItem::makeUserText(QString(400, QLatin1Char('z'))));

    loop.continueAfterCompaction();

    QCOMPARE(loop.phase(), AbstractLoop::Phase::Failed);
    QVERIFY(loop.lastError().contains(QStringLiteral("超出模型窗口")));
}

void CompactEngineReplayTests::overflowRecovery_triggersCompactionAndRetriesTurn()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    SessionRuntime runtime;
    runtime.workingDirectory = tmp.path();
    runtime.compactEnabled = true;
    runtime.summaryEnabled = false;
    runtime.contextWindow = 500000;
    runtime.providerType = QString::fromLatin1(kReplayProvider);
    runtime.modelName = QStringLiteral("test-model");

    ProviderCredential cred;
    const QString instanceId = makeCredential(&cred);
    runtime.credentialInstanceId = instanceId;

    int turnCount = 0;
    Agent agent(QStringLiteral("overflow-agent"), QStringLiteral("Test"), runtime);
    agent.setCredentialStore(&cred);
    agent.setProviderFactory([&](const QString &) {
        return std::make_unique<OverflowRecoveryFakeProvider>(&turnCount);
    });

    // 塞入前置轮次（供超窗大压选型回放）
    agent.loop()->ledger().appendProviderItem(
        ProviderItem::makeUserText(QString(200, QLatin1Char('u'))));
    agent.loop()->ledger().appendProviderItem(
        ProviderItem::makeAssistantText(QString(200, QLatin1Char('a'))));

    // 发起新一轮任务
    agent.loop()->enqueueUserMessage(QStringLiteral("do work"));
    agent.loop()->start(runtime);

    // 首次请求命中超窗 -> 触发 overflowCompaction -> 强制大压 -> 自动继续当前轮并成功
    QTRY_VERIFY_WITH_TIMEOUT(agent.loop()->phase() == AbstractLoop::Phase::Completed, 5000);
    QCOMPARE(turnCount, 2);
    QVERIFY(!agent.loop()->modelViewPrefixTexts().isEmpty());
    QVERIFY(agent.loop()->modelViewPrefixTexts().first().contains(QStringLiteral("recover from overflow")));
}

QTEST_MAIN(CompactEngineReplayTests)
#include "CompactEngineReplayTests.moc"
