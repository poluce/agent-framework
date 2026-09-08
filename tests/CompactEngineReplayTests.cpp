#include <QtTest>

#include "agent/compact/CompactEngine.h"
#include "agent/ProviderRunLedger.h"
#include "providers/core/AbstractProvider.h"
#include "providers/service/ProviderCredential.h"

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

QTEST_MAIN(CompactEngineReplayTests)
#include "CompactEngineReplayTests.moc"
