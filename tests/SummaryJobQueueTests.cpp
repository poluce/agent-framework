#include <QtTest>

#include "agent/compact/CompactEngine.h"
#include "agent/compact/SummaryJobQueue.h"
#include "types/ConversationMessage.h"
#include "providers/core/AbstractProvider.h"
#include "providers/service/ProviderCredential.h"

#include <QTimer>
#include <QUuid>

/**
 * SummaryJobQueue + CompactEngine::startSummaryOnly（假 Provider）。
 * 覆盖：成功出队、失败保留、abort≠Failed、串行。
 */
class SummaryJobQueueTests final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void success_removesJobAndEmits();
    void fail_keepsJobAsFailed();
    void abortRunning_keepsPendingNotFailed();
    void serial_twoJobs();
};

namespace {

constexpr auto kSummaryQueueProvider = "summary-queue-test";

int g_createCount = 0;
int g_startCount = 0;
bool g_forceFail = false;
bool g_forceHang = false; // start 后不回事件，便于 abort

class SummaryQueueFakeProvider final : public AbstractProvider
{
public:
    SummaryQueueFakeProvider()
        : AbstractProvider(QString::fromLatin1(kSummaryQueueProvider), nullptr)
    {
        ++g_createCount;
    }

protected:
    ProviderError validateProviderRequest(const ProviderRequest &) const override { return {}; }
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
        if (g_forceHang) {
            return true;
        }
        QTimer::singleShot(0, this, [this]() {
            if (g_forceFail) {
                ProviderError err;
                err.message = QStringLiteral("summary-fail");
                emitProviderEvent(ProviderEvent::fromError(err));
                return;
            }
            // 须过 validateSummaryText：≥80 字且非 DSML；带 ## 小节
            emitProviderEvent(ProviderEvent::fromTextDelta(QStringLiteral(
                "## 目标\n完成段摘要队列单测。\n"
                "## 约束与偏好\n无。\n"
                "## 已完成\n假 Provider 返回合法交接文。\n"
                "## 关键决策\n无。\n"
                "## 关键路径/命令/数据\n无。\n"
                "## 待办\n无。\n"
                "## 开放问题\n无")));
            ProviderMessageEnd end;
            end.messageId = QStringLiteral("sum-msg");
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

ConversationMessage makeSnapEntry(const QString &text)
{
    ConversationMessage m;
    m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m.kind = ConversationMessage::Kind::UserText;
    m.text = text;
    m.submittedToModel = true;
    return m;
}

struct QueueFixture {
    CompactEngine engine;
    SummaryJobQueue queue;
    ProviderCredential cred;
    QString instanceId;

    bool init(const QString &nameSuffix)
    {
        instanceId = cred.createInstance(
            QString::fromLatin1(kSummaryQueueProvider),
            QStringLiteral("sq-%1").arg(nameSuffix),
            QStringLiteral("https://example.test"),
            QStringLiteral("key"));
        if (instanceId.isEmpty())
            return false;
        queue.setCompactEngine(&engine);
        queue.setProviderContext(
            instanceId,
            &cred,
            [](const QString &) { return std::make_unique<SummaryQueueFakeProvider>(); },
            QStringLiteral("test-model"),
            nullptr);
        CompactConfig cfg;
        cfg.maxRetries = 0; // 失败立刻 Failed，便于测保留
        cfg.maxOutputTokens = 256;
        queue.setCompactConfig(cfg);
        return true;
    }
};

} // namespace

void SummaryJobQueueTests::initTestCase()
{
}

void SummaryJobQueueTests::cleanup()
{
    g_createCount = 0;
    g_startCount = 0;
    g_forceFail = false;
    g_forceHang = false;
}

void SummaryJobQueueTests::success_removesJobAndEmits()
{
    QueueFixture fx;
    QVERIFY(fx.init(QStringLiteral("ok")));

    QString finishedText;
    bool finishedOk = false;
    QObject::connect(&fx.queue, &SummaryJobQueue::jobFinished, this,
                     [&](const QString &, const bool ok, const QString &text, const QList<QString> &) {
                         finishedOk = ok;
                         finishedText = text;
                     });

    const ConversationMessage e = makeSnapEntry(QStringLiteral("成功段"));
    QVERIFY(!fx.queue.enqueue({e.id}, {e}).isEmpty());
    QCOMPARE(fx.queue.jobCount(), 1);
    fx.queue.kick();

    QTRY_COMPARE_WITH_TIMEOUT(fx.queue.jobCount(), 0, 3000);
    QVERIFY(finishedOk);
    QVERIFY(finishedText.contains(QStringLiteral("## 目标")));
    QVERIFY(fx.queue.isIdle());
}

void SummaryJobQueueTests::fail_keepsJobAsFailed()
{
    QueueFixture fx;
    QVERIFY(fx.init(QStringLiteral("fail")));

    g_forceFail = true;
    bool finishedOk = true;
    QObject::connect(&fx.queue, &SummaryJobQueue::jobFinished, this,
                     [&](const QString &, const bool ok, const QString &, const QList<QString> &) {
                         finishedOk = ok;
                     });

    const ConversationMessage e = makeSnapEntry(QStringLiteral("失败段"));
    QVERIFY(!fx.queue.enqueue({e.id}, {e}).isEmpty());
    fx.queue.kick();

    QTRY_VERIFY_WITH_TIMEOUT(fx.queue.hasFailed(), 3000);
    QVERIFY(!finishedOk);
    QCOMPARE(fx.queue.jobCount(), 1);
    QCOMPARE(fx.queue.jobs().first().state, SummaryJobState::Failed);
    QVERIFY(fx.queue.hasPendingOrRunning()); // Failed 仍算未完成
}

void SummaryJobQueueTests::abortRunning_keepsPendingNotFailed()
{
    QueueFixture fx;
    QVERIFY(fx.init(QStringLiteral("abort")));

    int finishCount = 0;
    QObject::connect(&fx.queue, &SummaryJobQueue::jobFinished, this,
                     [&](const QString &, bool, const QString &, const QList<QString> &) {
                         ++finishCount;
                     });

    g_forceHang = true;
    const ConversationMessage e = makeSnapEntry(QStringLiteral("挂起中的段"));
    QVERIFY(!fx.queue.enqueue({e.id}, {e}).isEmpty());
    fx.queue.kick();

    QTRY_COMPARE_WITH_TIMEOUT(g_startCount, 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(fx.queue.hasRunning(), 3000);

    fx.queue.abortRunning();

    // abort 不得把 job 标 Failed，也不得 emit jobFinished
    QCOMPARE(finishCount, 0);
    QCOMPARE(fx.queue.jobCount(), 1);
    QVERIFY(!fx.queue.hasFailed());
    QCOMPARE(fx.queue.jobs().first().state, SummaryJobState::Pending);
    QVERIFY(fx.queue.hasPendingOrRunning());
    QVERIFY(!fx.queue.hasRunning());
}

void SummaryJobQueueTests::serial_twoJobs()
{
    QueueFixture fx;
    QVERIFY(fx.init(QStringLiteral("serial")));

    int successCount = 0;
    QObject::connect(&fx.queue, &SummaryJobQueue::jobFinished, this,
                     [&](const QString &, const bool ok, const QString &, const QList<QString> &) {
                         if (ok)
                             ++successCount;
                     });

    // 相邻 Pending 会合并；要测串行两 job，须在第一段 Running 后再入第二段
    g_forceHang = true;
    const ConversationMessage e1 = makeSnapEntry(QStringLiteral("第一段"));
    const ConversationMessage e2 = makeSnapEntry(QStringLiteral("第二段"));
    QVERIFY(!fx.queue.enqueue({e1.id}, {e1}).isEmpty());
    fx.queue.kick();
    QTRY_VERIFY_WITH_TIMEOUT(fx.queue.hasRunning(), 3000);

    // 第一段 Running 时第二段入队 → 不合并，队列=2
    QVERIFY(!fx.queue.enqueue({e2.id}, {e2}).isEmpty());
    QCOMPARE(fx.queue.jobCount(), 2);

    // 放行：abort 在飞（回 Pending，不 Failed）后 kick
    // jobs=[Pending job1, Pending job2]；合并只看末尾，不会回并 job1
    g_forceHang = false;
    g_startCount = 0;
    fx.queue.abortRunning();
    QCOMPARE(fx.queue.jobCount(), 2);
    fx.queue.kick();

    QTRY_COMPARE_WITH_TIMEOUT(successCount, 2, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(g_startCount, 2, 5000);
    QCOMPARE(fx.queue.jobCount(), 0);
    QVERIFY(fx.queue.isIdle());
}

QTEST_MAIN(SummaryJobQueueTests)
#include "SummaryJobQueueTests.moc"
