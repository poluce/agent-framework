// 工具分组可见性测试（issue #30 方向）：
// ToolSpec::group + AbstractToolSource::visibleGroups(agentId) + dispatch 调用期强制。
// 语义：toolVisible 目录期按单工具名裁；visibleGroups 按「分组」在目录期(specsForAgent)
// 与调用期(dispatch)同时强制——列得出 = 调得到。默认空 = 不限，零破坏。

#include "tools/AbstractToolSource.h"
#include "tools/AbstractUnit.h"
#include "tools/ToolCoordinator.h"
#include "tools/BuiltinToolRuntime.h"
#include "config/SessionRuntime.h"
#include "agent/AgentSession.h"

#include <QJsonObject>
#include <QSignalSpy>
#include <QUuid>

#include <QtTest>

namespace {

int specCount(const QList<ToolSpec> &specs, const QString &name)
{
    int count = 0;
    for (const ToolSpec &spec : specs) {
        if (spec.name == name) {
            ++count;
        }
    }
    return count;
}

/// 可配置分组的假外部源：工具 a_tool（组 "serverA"）、b_tool（组 "serverB"）、plain（无组）。
class GroupedToolSource final : public AbstractToolSource
{
public:
    explicit GroupedToolSource(QObject *parent = nullptr)
        : AbstractToolSource(parent)
    {
    }

    QString id() const override { return QStringLiteral("grouped"); }

    QList<ToolSpec> specs() const override
    {
        ToolSpec a;
        a.name = QStringLiteral("a_tool");
        a.group = QStringLiteral("serverA");
        ToolSpec b;
        b.name = QStringLiteral("b_tool");
        b.group = QStringLiteral("serverB");
        ToolSpec plain;
        plain.name = QStringLiteral("plain_tool");
        return {a, b, plain};
    }

    bool owns(const QString &toolName) const override
    {
        return toolName == QStringLiteral("a_tool")
            || toolName == QStringLiteral("b_tool")
            || toolName == QStringLiteral("plain_tool");
    }

    void invoke(const ToolCall &call, const ToolInvokeContext &, Completion done) override
    {
        ToolResult tr;
        tr.toolUseId = call.id;
        tr.toolName = call.toolName;
        tr.success = true;
        tr.text = QStringLiteral("ok:") + call.toolName;
        done(std::move(tr));
    }

    /// 可编程：agentId → 可见分组。测试直接改这个表。
    QHash<QString, QStringList> visible;
    QStringList visibleGroups(const QString &agentId) const override
    {
        return visible.value(agentId);
    }
};

ToolResult dispatchSync(ToolCoordinator &coordinator, const QString &agentId,
                        const QString &toolName)
{
    ToolResult result;
    bool done = false;
    ToolCall call;
    call.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    call.toolName = toolName;
    call.input = QJsonObject{};
    BuiltinToolRuntime runtime;
    coordinator.dispatch(agentId, call, QString(), runtime,
                         [&](ToolResult tr) {
                             result = std::move(tr);
                             done = true;
                         });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 5000) {
        QTest::qWait(10);
    }
    return result;
}

} // namespace

class ToolGroupVisibilityTests final : public QObject
{
    Q_OBJECT

private slots:
    void dispatch_defaultNoGroups_allowsEverything();
    void dispatch_groupRestricted_rejectsOutsideGroup();
    void dispatch_groupAllowed_invokes();
    void dispatch_plainTool_unaffectedByGroupRestriction();
    void specsForAgent_filtersByGroup_consistentWithDispatch();
    void allSpecs_unaffectedByGroupRestriction();
};

void ToolGroupVisibilityTests::dispatch_defaultNoGroups_allowsEverything()
{
    // 源须先于 coordinator 声明：coordinator 析构会遍历 sources() 调 sessionClosing()，
    // 若源先析构则悬垂（AGENTS.md 所有权契约：源存活到会话/协调器之后）。
    GroupedToolSource source;
    ToolCoordinator coordinator(nullptr);
    coordinator.addSource(&source, QString());

    // 默认 visibleGroups 为空 = 不限 → 全放行
    const ToolResult a = dispatchSync(coordinator, QStringLiteral("agent-0"), QStringLiteral("a_tool"));
    QVERIFY(a.success);
    const ToolResult plain = dispatchSync(coordinator, QStringLiteral("agent-0"), QStringLiteral("plain_tool"));
    QVERIFY(plain.success);
}

void ToolGroupVisibilityTests::dispatch_groupRestricted_rejectsOutsideGroup()
{
    // 源须先于 coordinator 声明：coordinator 析构会遍历 sources() 调 sessionClosing()，
    // 若源先析构则悬垂（AGENTS.md 所有权契约：源存活到会话/协调器之后）。
    GroupedToolSource source;
    ToolCoordinator coordinator(nullptr);
    coordinator.addSource(&source, QString());
    source.visible.insert(QStringLiteral("agent-0"), {QStringLiteral("serverA")});

    // 只放行 serverA：b_tool（serverB）被拒
    const ToolResult b = dispatchSync(coordinator, QStringLiteral("agent-0"), QStringLiteral("b_tool"));
    QVERIFY(!b.success);
    QVERIFY(b.isError);
    QCOMPARE(b.category, ToolResultCategory::Rejected);

    // a_tool（serverA）放行并真正 invoke（源返回 ok:）
    const ToolResult a = dispatchSync(coordinator, QStringLiteral("agent-0"), QStringLiteral("a_tool"));
    QVERIFY(a.success);
    QCOMPARE(a.text, QStringLiteral("ok:a_tool"));
}

void ToolGroupVisibilityTests::dispatch_groupAllowed_invokes()
{
    // 源须先于 coordinator 声明：coordinator 析构会遍历 sources() 调 sessionClosing()，
    // 若源先析构则悬垂（AGENTS.md 所有权契约：源存活到会话/协调器之后）。
    GroupedToolSource source;
    ToolCoordinator coordinator(nullptr);
    coordinator.addSource(&source, QString());
    source.visible.insert(QStringLiteral("agent-0"),
                          {QStringLiteral("serverA"), QStringLiteral("serverB")});

    // 两分组都放行 → 都真调
    const ToolResult a = dispatchSync(coordinator, QStringLiteral("agent-0"), QStringLiteral("a_tool"));
    QVERIFY(a.success);
    const ToolResult b = dispatchSync(coordinator, QStringLiteral("agent-0"), QStringLiteral("b_tool"));
    QVERIFY(b.success);
}

void ToolGroupVisibilityTests::dispatch_plainTool_unaffectedByGroupRestriction()
{
    // 源须先于 coordinator 声明：coordinator 析构会遍历 sources() 调 sessionClosing()，
    // 若源先析构则悬垂（AGENTS.md 所有权契约：源存活到会话/协调器之后）。
    GroupedToolSource source;
    ToolCoordinator coordinator(nullptr);
    coordinator.addSource(&source, QString());
    source.visible.insert(QStringLiteral("agent-0"), {QStringLiteral("serverA")});

    // 无分组的普通工具不受分组限制
    const ToolResult plain = dispatchSync(coordinator, QStringLiteral("agent-0"), QStringLiteral("plain_tool"));
    QVERIFY(plain.success);
}

void ToolGroupVisibilityTests::specsForAgent_filtersByGroup_consistentWithDispatch()
{
    // 源须先于 coordinator 声明：coordinator 析构会遍历 sources() 调 sessionClosing()，
    // 若源先析构则悬垂（AGENTS.md 所有权契约：源存活到会话/协调器之后）。
    GroupedToolSource source;
    ToolCoordinator coordinator(nullptr);
    coordinator.addSource(&source, QString());

    // 不限分组：三个都在
    QCOMPARE(specCount(coordinator.specsForAgent(QStringLiteral("agent-0")), QStringLiteral("a_tool")), 1);
    QCOMPARE(specCount(coordinator.specsForAgent(QStringLiteral("agent-0")), QStringLiteral("b_tool")), 1);

    // 只放行 serverA：b_tool 不在目录（列不出），a_tool/plain 在
    source.visible.insert(QStringLiteral("agent-0"), {QStringLiteral("serverA")});
    QCOMPARE(specCount(coordinator.specsForAgent(QStringLiteral("agent-0")), QStringLiteral("a_tool")), 1);
    QCOMPARE(specCount(coordinator.specsForAgent(QStringLiteral("agent-0")), QStringLiteral("b_tool")), 0);
    QCOMPARE(specCount(coordinator.specsForAgent(QStringLiteral("agent-0")), QStringLiteral("plain_tool")), 1);
}

void ToolGroupVisibilityTests::allSpecs_unaffectedByGroupRestriction()
{
    // 源须先于 coordinator 声明：coordinator 析构会遍历 sources() 调 sessionClosing()，
    // 若源先析构则悬垂（AGENTS.md 所有权契约：源存活到会话/协调器之后）。
    GroupedToolSource source;
    ToolCoordinator coordinator(nullptr);
    coordinator.addSource(&source, QString());
    source.visible.insert(QStringLiteral("agent-0"), {QStringLiteral("serverA")});

    // allSpecs 是登记/测试用全量目录，不做分组过滤
    QCOMPARE(specCount(coordinator.allSpecs(), QStringLiteral("a_tool")), 1);
    QCOMPARE(specCount(coordinator.allSpecs(), QStringLiteral("b_tool")), 1);
    QCOMPARE(specCount(coordinator.allSpecs(), QStringLiteral("plain_tool")), 1);
}

QTEST_MAIN(ToolGroupVisibilityTests)
#include "ToolGroupVisibilityTests.moc"
