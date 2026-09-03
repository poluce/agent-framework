#include "agent/Agent.h"
#include "config/SessionRuntime.h"
#include "types/CoreEvent.h"

#include <QTemporaryDir>
#include <QtTest>

/**
 * 邮箱两阶段投递语义测试：
 * take 只标 in-flight，投递成功 ack 才移除，失败 requeue 可重试。
 */
class InboxDeliveryTests final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void enqueueTakeAck_removesMessage();
    void enqueueAck_emitsObservabilityEvents();
    void requeue_returnsToPending();
    void take_ordersByPriorityThenTimestamp();
    void capacityLimit_rejectsAndEmitsDropped();
    void sizeLimit_rejectsAndEmitsDropped();
    void clearInbox_emitsDroppedForRemaining();
    void enqueueAgentTask_emptyReturnsFalse();
    void ackRequeue_unknownIdsAreNoOp();

private:
    QTemporaryDir m_dir;
    SessionRuntime m_runtime;
};

void InboxDeliveryTests::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_runtime.workingDirectory = m_dir.path();
    m_runtime.systemPrompt = QStringLiteral("inbox test");
    m_runtime.compactEnabled = false;
}

void InboxDeliveryTests::enqueueTakeAck_removesMessage()
{
    Agent agent(QStringLiteral("agent-0"), QStringLiteral("Main"), m_runtime);

    AgentInboxMessage msg;
    msg.id = QStringLiteral("m1");
    msg.fromAgentId = QStringLiteral("agent-1");
    msg.content = QStringLiteral("hello");

    QVERIFY(agent.enqueueInboxMessage(msg));
    QVERIFY(agent.hasPendingInboxMessages());

    const auto taken = agent.takePendingInboxMessages();
    QCOMPARE(taken.size(), 1);
    QCOMPARE(taken.first().id, QStringLiteral("m1"));
    QVERIFY(!agent.hasPendingInboxMessages());

    agent.ackInboxMessages({QStringLiteral("m1")});
    QVERIFY(agent.takePendingInboxMessages().isEmpty());
}

void InboxDeliveryTests::enqueueAck_emitsObservabilityEvents()
{
    Agent agent(QStringLiteral("agent-0"), QStringLiteral("Main"), m_runtime);

    int enqueued = 0;
    int delivered = 0;
    agent.addEventHandler([&](const core_ir::Event &event,
                              const core_ir::EventContext &,
                              const core_ir::SubmissionId &) {
        if (std::get_if<core_ir::EventInboxMessageEnqueued>(&event)) {
            ++enqueued;
        }
        if (std::get_if<core_ir::EventInboxMessageDelivered>(&event)) {
            ++delivered;
        }
    });

    AgentInboxMessage msg;
    msg.id = QStringLiteral("obs");
    msg.fromAgentId = QStringLiteral("agent-1");
    msg.content = QStringLiteral("observe me");

    QVERIFY(agent.enqueueInboxMessage(msg));
    QCOMPARE(enqueued, 1);

    agent.takePendingInboxMessages();
    agent.ackInboxMessages({QStringLiteral("obs")});
    QCOMPARE(delivered, 1);
}

void InboxDeliveryTests::requeue_returnsToPending()
{
    Agent agent(QStringLiteral("agent-0"), QStringLiteral("Main"), m_runtime);

    AgentInboxMessage msg;
    msg.id = QStringLiteral("m2");
    msg.fromAgentId = QStringLiteral("agent-1");
    msg.content = QStringLiteral("retry me");

    QVERIFY(agent.enqueueInboxMessage(msg));
    const auto taken = agent.takePendingInboxMessages();
    QCOMPARE(taken.size(), 1);
    QVERIFY(!agent.hasPendingInboxMessages());

    agent.requeueInboxMessages({QStringLiteral("m2")});
    QVERIFY(agent.hasPendingInboxMessages());

    const auto again = agent.takePendingInboxMessages();
    QCOMPARE(again.size(), 1);
    QCOMPARE(again.first().id, QStringLiteral("m2"));
    agent.ackInboxMessages({QStringLiteral("m2")});
}

void InboxDeliveryTests::take_ordersByPriorityThenTimestamp()
{
    Agent agent(QStringLiteral("agent-0"), QStringLiteral("Main"), m_runtime);

    AgentInboxMessage low;
    low.id = QStringLiteral("low");
    low.fromAgentId = QStringLiteral("agent-1");
    low.content = QStringLiteral("low");
    low.priority = core_ir::InboxPriority::Low;
    low.timestamp = QDateTime::currentDateTime().addSecs(1);

    AgentInboxMessage normal;
    normal.id = QStringLiteral("normal");
    normal.fromAgentId = QStringLiteral("agent-1");
    normal.content = QStringLiteral("normal");
    normal.priority = core_ir::InboxPriority::Normal;
    normal.timestamp = QDateTime::currentDateTime();

    AgentInboxMessage urgent;
    urgent.id = QStringLiteral("urgent");
    urgent.fromAgentId = QStringLiteral("agent-1");
    urgent.content = QStringLiteral("urgent");
    urgent.priority = core_ir::InboxPriority::Urgent;
    urgent.timestamp = QDateTime::currentDateTime().addSecs(2);

    QVERIFY(agent.enqueueInboxMessage(normal));
    QVERIFY(agent.enqueueInboxMessage(urgent));
    QVERIFY(agent.enqueueInboxMessage(low));

    const auto taken = agent.takePendingInboxMessages();
    QCOMPARE(taken.size(), 3);
    QCOMPARE(taken.at(0).id, QStringLiteral("urgent"));
    QCOMPARE(taken.at(1).id, QStringLiteral("normal"));
    QCOMPARE(taken.at(2).id, QStringLiteral("low"));

    agent.ackInboxMessages({QStringLiteral("urgent"), QStringLiteral("normal"), QStringLiteral("low")});
}

void InboxDeliveryTests::capacityLimit_rejectsAndEmitsDropped()
{
    SessionRuntime runtime = m_runtime;
    runtime.maxInboxMessages = 1;
    Agent agent(QStringLiteral("agent-0"), QStringLiteral("Main"), runtime);

    int dropped = 0;
    QString droppedReason;
    agent.addEventHandler([&](const core_ir::Event &event,
                              const core_ir::EventContext &,
                              const core_ir::SubmissionId &) {
        if (const auto *d = std::get_if<core_ir::EventInboxMessageDropped>(&event)) {
            ++dropped;
            droppedReason = d->reason;
        }
    });

    AgentInboxMessage first;
    first.id = QStringLiteral("first");
    first.fromAgentId = QStringLiteral("agent-1");
    first.content = QStringLiteral("first");
    QVERIFY(agent.enqueueInboxMessage(first));

    AgentInboxMessage second;
    second.id = QStringLiteral("second");
    second.fromAgentId = QStringLiteral("agent-1");
    second.content = QStringLiteral("second");
    QVERIFY(!agent.enqueueInboxMessage(second));

    QCOMPARE(dropped, 1);
    QCOMPARE(droppedReason, QStringLiteral("capacity"));
    QVERIFY(agent.hasPendingInboxMessages());
    agent.ackInboxMessages({QStringLiteral("first")});
}

void InboxDeliveryTests::sizeLimit_rejectsAndEmitsDropped()
{
    SessionRuntime runtime = m_runtime;
    runtime.maxInboxMessageSize = 5;
    Agent agent(QStringLiteral("agent-0"), QStringLiteral("Main"), runtime);

    int dropped = 0;
    QString droppedReason;
    agent.addEventHandler([&](const core_ir::Event &event,
                              const core_ir::EventContext &,
                              const core_ir::SubmissionId &) {
        if (const auto *d = std::get_if<core_ir::EventInboxMessageDropped>(&event)) {
            ++dropped;
            droppedReason = d->reason;
        }
    });

    AgentInboxMessage ok;
    ok.id = QStringLiteral("ok");
    ok.fromAgentId = QStringLiteral("agent-1");
    ok.content = QStringLiteral("12345");
    QVERIFY(agent.enqueueInboxMessage(ok));

    AgentInboxMessage tooBig;
    tooBig.id = QStringLiteral("too-big");
    tooBig.fromAgentId = QStringLiteral("agent-1");
    tooBig.content = QStringLiteral("123456");
    QVERIFY(!agent.enqueueInboxMessage(tooBig));

    QCOMPARE(dropped, 1);
    QCOMPARE(droppedReason, QStringLiteral("size"));
    agent.ackInboxMessages({QStringLiteral("ok")});
}

void InboxDeliveryTests::clearInbox_emitsDroppedForRemaining()
{
    Agent agent(QStringLiteral("agent-0"), QStringLiteral("Main"), m_runtime);

    int dropped = 0;
    QStringList reasons;
    agent.addEventHandler([&](const core_ir::Event &event,
                              const core_ir::EventContext &,
                              const core_ir::SubmissionId &) {
        if (const auto *d = std::get_if<core_ir::EventInboxMessageDropped>(&event)) {
            ++dropped;
            reasons.append(d->reason);
        }
    });

    AgentInboxMessage a;
    a.id = QStringLiteral("a");
    a.fromAgentId = QStringLiteral("agent-1");
    a.content = QStringLiteral("a");
    AgentInboxMessage b;
    b.id = QStringLiteral("b");
    b.fromAgentId = QStringLiteral("agent-1");
    b.content = QStringLiteral("b");
    QVERIFY(agent.enqueueInboxMessage(a));
    QVERIFY(agent.enqueueInboxMessage(b));

    agent.clearInbox(QStringLiteral("test_clear"));
    QCOMPARE(dropped, 2);
    QCOMPARE(reasons.size(), 2);
    QCOMPARE(reasons.at(0), QStringLiteral("test_clear"));
    QCOMPARE(reasons.at(1), QStringLiteral("test_clear"));
    QVERIFY(!agent.hasPendingInboxMessages());
}

void InboxDeliveryTests::enqueueAgentTask_emptyReturnsFalse()
{
    Agent agent(QStringLiteral("agent-0"), QStringLiteral("Main"), m_runtime);
    QVERIFY(!agent.loop()->enqueueAgentTask(QString()));
    QVERIFY(!agent.loop()->enqueueAgentTask(QStringLiteral("   ")));
    QVERIFY(agent.loop()->enqueueAgentTask(QStringLiteral("task")));
}

void InboxDeliveryTests::ackRequeue_unknownIdsAreNoOp()
{
    Agent agent(QStringLiteral("agent-0"), QStringLiteral("Main"), m_runtime);
    agent.ackInboxMessages({QStringLiteral("missing")});
    agent.requeueInboxMessages({QStringLiteral("missing")});
    QVERIFY(!agent.hasPendingInboxMessages());
}

QTEST_MAIN(InboxDeliveryTests)
#include "InboxDeliveryTests.moc"
