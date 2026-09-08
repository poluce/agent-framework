#include <QtTest>

#include <QJsonObject>

#include "agent/compact/CompactPolicy.h"
#include "agent/compact/CompactToolPair.h"
#include "agent/ProviderRunLedger.h"

class CompactPolicyTests final : public QObject
{
    Q_OBJECT

private slots:
    void pressureThreshold_usesWindowRatio();
    void retainTokens_isSixteenPercent();
    void pruneToolResult_keepsHeadAndTail();
    void pruneLedger_shortensOversizedToolResult();
    void frameCheckpoint_wrapsPlainSummary();
    void frameCheckpoint_skipsAlreadyTagged();
    void selectPrefix_keepsRecentTail();
    void overflow_detectsContextLength();
    void summaryShrinks_requiresStrictlySmaller();
};

void CompactPolicyTests::pressureThreshold_usesWindowRatio()
{
    QCOMPARE(CompactPolicy::pressureThreshold(100000, 0, 0), 80000);
    QCOMPARE(CompactPolicy::pressureThreshold(100000, 50000, 0), 50000);
    QCOMPARE(CompactPolicy::pressureThreshold(100000, 0, 10000), 70000);
}

void CompactPolicyTests::retainTokens_isSixteenPercent()
{
    QCOMPARE(CompactPolicy::retainTokens(100000), 16000);
}

void CompactPolicyTests::pruneToolResult_keepsHeadAndTail()
{
    const QString body = QString(9000, QLatin1Char('a'));
    const QString pruned = CompactPolicy::pruneToolResultText(body, 8192, 100, 50);
    QVERIFY(pruned.size() < body.size());
    QVERIFY(pruned.startsWith(QString(100, QLatin1Char('a'))));
    QVERIFY(pruned.endsWith(QString(50, QLatin1Char('a'))));
    QVERIFY(pruned.contains(QStringLiteral("middle pruned")));
}

void CompactPolicyTests::pruneLedger_shortensOversizedToolResult()
{
    ProviderRunLedger ledger;
    const QString userId = ledger.appendProviderItem(
        ProviderItem::makeUserText(QStringLiteral("go")));
    Q_UNUSED(userId);
    ProviderItem call = ProviderItem::makeFunctionCall(
        QStringLiteral("c1"), QStringLiteral("bash"), QJsonObject{}, QStringLiteral("{}"));
    ledger.appendProviderItem(call);
    ProviderItem result = ProviderItem::makeFunctionCallOutput(
        QStringLiteral("c1"), QStringLiteral("bash"), QString(9000, QLatin1Char('x')));
    const QString resultId = ledger.appendProviderItem(result);

    QCOMPARE(CompactPolicy::pruneOversizedToolResults(ledger), 1);
    const ConversationMessage *entry = ledger.findById(resultId);
    QVERIFY(entry);
    QVERIFY(entry->wasTruncated);
    QVERIFY(entry->text.size() < 9000);
    QVERIFY(entry->text.contains(QStringLiteral("middle pruned")));
}

void CompactPolicyTests::frameCheckpoint_wrapsPlainSummary()
{
    const QString framed = CompactPolicy::frameCheckpoint(QStringLiteral("did the work"));
    QVERIFY(framed.contains(QStringLiteral("<compacted-summary>")));
    QVERIFY(framed.contains(QStringLiteral("did the work")));
    QVERIFY(framed.contains(QStringLiteral("</compacted-summary>")));
}

void CompactPolicyTests::frameCheckpoint_skipsAlreadyTagged()
{
    const QString raw = QStringLiteral("<compacted-summary>\nold\n</compacted-summary>");
    QCOMPARE(CompactPolicy::frameCheckpoint(raw), raw);
    const QString labeled = QStringLiteral("[最近用户输入]\nhi");
    QCOMPARE(CompactPolicy::frameCheckpoint(labeled), labeled);
}

void CompactPolicyTests::selectPrefix_keepsRecentTail()
{
    QList<ConversationMessage> entries;
    auto add = [&](const QString &id, ConversationMessage::Kind kind, const QString &text) {
        ConversationMessage m;
        m.id = id;
        m.kind = kind;
        m.text = text;
        m.submittedToModel = true;
        entries.append(m);
    };
    add(QStringLiteral("u1"), ConversationMessage::Kind::UserText, QString(400, QLatin1Char('a')));
    add(QStringLiteral("a1"), ConversationMessage::Kind::AssistantText, QString(400, QLatin1Char('b')));
    add(QStringLiteral("u2"), ConversationMessage::Kind::UserText, QString(400, QLatin1Char('c')));
    add(QStringLiteral("a2"), ConversationMessage::Kind::AssistantText, QString(400, QLatin1Char('d')));

    const QList<QString> selected = CompactToolPair::selectPrefixToCompact(entries, 50);
    QVERIFY(selected.contains(QStringLiteral("u1")));
    QVERIFY(selected.contains(QStringLiteral("a1")));
    QVERIFY(!selected.contains(QStringLiteral("a2")));
}

void CompactPolicyTests::overflow_detectsContextLength()
{
    ProviderError hit;
    hit.message = QStringLiteral("This model's maximum context length is 128000 tokens");
    QVERIFY(CompactPolicy::isContextWindowExceeded(hit));
    ProviderError miss;
    miss.message = QStringLiteral("rate limit exceeded");
    QVERIFY(!CompactPolicy::isContextWindowExceeded(miss));
}

void CompactPolicyTests::summaryShrinks_requiresStrictlySmaller()
{
    QVERIFY(CompactPolicy::summaryShrinks(100, 40));
    QVERIFY(!CompactPolicy::summaryShrinks(100, 100));
    QVERIFY(!CompactPolicy::summaryShrinks(100, 120));
    QVERIFY(CompactPolicy::summaryShrinks(0, 10));
}

QTEST_MAIN(CompactPolicyTests)
#include "CompactPolicyTests.moc"
