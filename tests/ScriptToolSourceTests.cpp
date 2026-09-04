#include "tools/ScriptToolSource.h"
#include "tools/ToolCoordinator.h"
#include "agent/Agent.h"
#include "agent/AgentSession.h"

#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <QtTest>

namespace {

QString manifestLine(const QString &name, const QString &description,
                     const QString &mode, bool ephemeral = false)
{
    QJsonObject m;
    m.insert(QStringLiteral("name"), name);
    m.insert(QStringLiteral("description"), description);
    m.insert(QStringLiteral("mode"), mode);
    m.insert(QStringLiteral("input_schema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}});
    m.insert(QStringLiteral("ephemeral"), ephemeral);
    return QStringLiteral("# @tool ")
        + QString::fromUtf8(QJsonDocument(m).toJson(QJsonDocument::Compact));
}

bool writeScript(const QString &path, const QString &content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(content.toUtf8());
    return true;
}

QString findPython()
{
    for (const QString &name : {QStringLiteral("python3"), QStringLiteral("python"), QStringLiteral("py")}) {
        const QString path = QStandardPaths::findExecutable(name);
        if (!path.isEmpty()) {
            return path;
        }
    }
    return {};
}

ToolResult invokeSync(ScriptToolSource &source, const QString &toolName,
                      const QJsonObject &input, const ToolInvokeContext &ctx)
{
    ToolResult result;
    bool done = false;
    ToolCall call;
    call.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    call.toolName = toolName;
    call.input = input;
    source.invoke(call, ctx, [&](ToolResult tr) {
        result = std::move(tr);
        done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 15000) {
        QTest::qWait(20);
    }
    return result;
}

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

} // namespace

class ScriptToolSourceTests : public QObject
{
    Q_OBJECT

private slots:
    void scan_persistentAndEphemeral();
    void createTool_writesAndRegisters();
    void createTool_validation();
    void createTool_updateOverwrites();
    void deleteTool_removesAndPauses();
    void syncInvoke_endToEnd();
    void pushEvent_deliversToMailbox();
    void sessionClose_killsProcessesAndCleansEphemeral();
    void coordinator_removeSourceAndOwner();
};

void ScriptToolSourceTests::scan_persistentAndEphemeral()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString toolDir = tmp.path() + QStringLiteral("/tools");
    const QString ephDir = tmp.path() + QStringLiteral("/eph");
    QVERIFY(QDir().mkpath(toolDir + QStringLiteral("/agent-0")));
    QVERIFY(QDir().mkpath(ephDir + QStringLiteral("/agent-1")));
    QVERIFY(writeScript(toolDir + QStringLiteral("/agent-0/hello.py"),
                        manifestLine(QStringLiteral("hello"), QStringLiteral("打招呼"), QStringLiteral("sync"))
                            + QStringLiteral("\nprint('hi')\n")));
    QVERIFY(writeScript(ephDir + QStringLiteral("/agent-1/tmp.js"),
                        QStringLiteral("// @tool ")
                            + QString::fromUtf8(QJsonDocument(QJsonObject{
                                {QStringLiteral("name"), QStringLiteral("tmp")},
                                {QStringLiteral("description"), QStringLiteral("临时")},
                                {QStringLiteral("mode"), QStringLiteral("push")},
                            }).toJson(QJsonDocument::Compact))
                            + QStringLiteral("\nconsole.log(1)\n")));
    // 无 manifest 的文件应被忽略
    QVERIFY(writeScript(toolDir + QStringLiteral("/agent-0/plain.py"),
                        QStringLiteral("print(1)\n")));

    const QString ephFile = ephDir + QStringLiteral("/agent-1/tmp.js");
    {
        ScriptToolSource source;
        source.setToolDirectory(toolDir);
        source.setEphemeralDirectory(ephDir);
        QVERIFY(source.hasTool(QStringLiteral("hello")));
        QVERIFY(source.hasTool(QStringLiteral("tmp")));
        QVERIFY(!source.hasTool(QStringLiteral("plain")));
        QCOMPARE(source.toolFilePath(QStringLiteral("hello")),
                 toolDir + QStringLiteral("/agent-0/hello.py"));
        QCOMPARE(source.toolFilePath(QStringLiteral("tmp")), ephFile);
        // specs = 元工具 + 扫描工具
        const QList<ToolSpec> specs = source.specs();
        QCOMPARE(specCount(specs, QStringLiteral("create_tool")), 1);
        QCOMPARE(specCount(specs, QStringLiteral("delete_tool")), 1);
        QCOMPARE(specCount(specs, QStringLiteral("hello")), 1);
        QCOMPARE(specCount(specs, QStringLiteral("tmp")), 1);
        // 扫描工具默认 Write 权限（任意代码）
        for (const ToolSpec &spec : specs) {
            if (spec.name == QStringLiteral("hello")) {
                QCOMPARE(spec.permissionKind, ToolPermissionKind::Write);
            }
        }
    }
    // 析构：临时工具文件删除，持久工具保留
    QVERIFY(!QFile::exists(ephFile));
    QVERIFY(QFile::exists(toolDir + QStringLiteral("/agent-0/hello.py")));
}

void ScriptToolSourceTests::createTool_writesAndRegisters()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    QSignalSpy spy(&source, &AbstractToolSource::toolsChanged);
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");

    const ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("hello")},
        {QStringLiteral("description"), QStringLiteral("打招呼")},
        {QStringLiteral("code"), QStringLiteral("print('hi')")},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);
    QVERIFY(!r.isError);
    QVERIFY(source.hasTool(QStringLiteral("hello")));
    const QString filePath = source.toolFilePath(QStringLiteral("hello"));
    QCOMPARE(filePath, tmp.path() + QStringLiteral("/tools/agent-0/hello.py"));
    QVERIFY(QFile::exists(filePath));
    QCOMPARE(spy.count(), 1);
    // 文件首行是 manifest 头
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(f.readLine()).contains(QStringLiteral("@tool")));
}

void ScriptToolSourceTests::createTool_validation()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");

    // 非法名（含空格）
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("bad name")},
        {QStringLiteral("description"), QStringLiteral("d")},
        {QStringLiteral("code"), QStringLiteral("x")},
    }, ctx);
    QVERIFY(r.isError);
    // 保留名
    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("create_tool")},
        {QStringLiteral("description"), QStringLiteral("d")},
        {QStringLiteral("code"), QStringLiteral("x")},
    }, ctx);
    QVERIFY(r.isError);
    // 空 code
    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("ok")},
        {QStringLiteral("description"), QStringLiteral("d")},
        {QStringLiteral("code"), QString()},
    }, ctx);
    QVERIFY(r.isError);
    // 非法 language
    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("ok")},
        {QStringLiteral("description"), QStringLiteral("d")},
        {QStringLiteral("code"), QStringLiteral("x")},
        {QStringLiteral("language"), QStringLiteral("ruby")},
    }, ctx);
    QVERIFY(r.isError);
    // 非法 mode
    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("ok")},
        {QStringLiteral("description"), QStringLiteral("d")},
        {QStringLiteral("code"), QStringLiteral("x")},
        {QStringLiteral("mode"), QStringLiteral("stream")},
    }, ctx);
    QVERIFY(r.isError);
    // 目录未配置
    ScriptToolSource noDir;
    r = invokeSync(noDir, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("ok")},
        {QStringLiteral("description"), QStringLiteral("d")},
        {QStringLiteral("code"), QStringLiteral("x")},
    }, ctx);
    QVERIFY(r.isError);
    // 全部失败后目录里没有工具
    QVERIFY(!source.hasTool(QStringLiteral("ok")));
}

void ScriptToolSourceTests::createTool_updateOverwrites()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");

    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("hello")},
        {QStringLiteral("description"), QStringLiteral("v1")},
        {QStringLiteral("code"), QStringLiteral("print(1)")},
    }, ctx);
    QVERIFY(r.success);
    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("hello")},
        {QStringLiteral("description"), QStringLiteral("v2")},
        {QStringLiteral("code"), QStringLiteral("print(2)")},
    }, ctx);
    QVERIFY(r.success);
    QVERIFY(source.hasTool(QStringLiteral("hello")));
    QCOMPARE(specCount(source.specs(), QStringLiteral("hello")), 1);
    QFile f(source.toolFilePath(QStringLiteral("hello")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(f.readAll()).contains(QStringLiteral("print(2)")));
}

void ScriptToolSourceTests::deleteTool_removesAndPauses()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");

    invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("a")},
        {QStringLiteral("description"), QStringLiteral("d")},
        {QStringLiteral("code"), QStringLiteral("x")},
    }, ctx);
    invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("b")},
        {QStringLiteral("description"), QStringLiteral("d")},
        {QStringLiteral("code"), QStringLiteral("x")},
    }, ctx);
    const QString pathA = source.toolFilePath(QStringLiteral("a"));
    const QString pathB = source.toolFilePath(QStringLiteral("b"));

    // 删除 a：工具与文件都消失
    ToolResult r = invokeSync(source, QStringLiteral("delete_tool"), {
        {QStringLiteral("name"), QStringLiteral("a")},
    }, ctx);
    QVERIFY(r.success);
    QVERIFY(!source.hasTool(QStringLiteral("a")));
    QVERIFY(!QFile::exists(pathA));
    // 暂停 b：工具注销但文件保留
    r = invokeSync(source, QStringLiteral("delete_tool"), {
        {QStringLiteral("name"), QStringLiteral("b")},
        {QStringLiteral("keep_file"), true},
    }, ctx);
    QVERIFY(r.success);
    QVERIFY(!source.hasTool(QStringLiteral("b")));
    QVERIFY(QFile::exists(pathB));
    // 删除不存在的工具
    r = invokeSync(source, QStringLiteral("delete_tool"), {
        {QStringLiteral("name"), QStringLiteral("nope")},
    }, ctx);
    QVERIFY(r.isError);
    // 保留名不可删
    r = invokeSync(source, QStringLiteral("delete_tool"), {
        {QStringLiteral("name"), QStringLiteral("create_tool")},
    }, ctx);
    QVERIFY(r.isError);
}

void ScriptToolSourceTests::syncInvoke_endToEnd()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过端到端同步调用测试");
    }
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    source.setRuntimeCommand(QStringLiteral("py"), python);
    source.setInvokeTimeoutMs(10000);
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");

    const QString code = QStringLiteral(
        "import sys, json\n"
        "for line in sys.stdin:\n"
        "    req = json.loads(line)\n"
        "    if req.get(\"type\") == \"invoke\":\n"
        "        out = {\"type\": \"result\", \"id\": req[\"id\"], \"ok\": True,\n"
        "               \"text\": \"echo:\" + str(req[\"args\"].get(\"who\", \"\"))}\n"
        "        sys.stdout.write(json.dumps(out) + \"\\n\")\n"
        "        sys.stdout.flush()\n");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("echo")},
        {QStringLiteral("description"), QStringLiteral("回显")},
        {QStringLiteral("code"), code},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);

    r = invokeSync(source, QStringLiteral("echo"), {
        {QStringLiteral("who"), QStringLiteral("world")},
    }, ctx);
    QVERIFY(r.success);
    QVERIFY(!r.isError);
    QCOMPARE(r.text, QStringLiteral("echo:world"));
    QCOMPARE(source.processCount(), 1);
}

void ScriptToolSourceTests::pushEvent_deliversToMailbox()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过异步推送测试");
    }
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    source.setRuntimeCommand(QStringLiteral("py"), python);
    source.setInvokeTimeoutMs(10000);

    SessionRuntime defaults;
    defaults.workingDirectory = tmp.path();
    AgentSessionConfig cfg;
    cfg.globalDefaults = &defaults;
    AgentSession session(cfg);
    Agent *agent = session.insertUnit(QStringLiteral("agent-0"), QStringLiteral("测试"));
    QVERIFY(agent);
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    ctx.session = &session;

    const QString code = QStringLiteral(
        "import sys, json, threading, time\n"
        "def send(obj):\n"
        "    sys.stdout.write(json.dumps(obj) + \"\\n\")\n"
        "    sys.stdout.flush()\n"
        "for line in sys.stdin:\n"
        "    req = json.loads(line)\n"
        "    if req.get(\"type\") == \"invoke\":\n"
        "        send({\"type\": \"result\", \"id\": req[\"id\"], \"ok\": True, \"text\": \"subscribed\"})\n"
        "        def emit():\n"
        "            time.sleep(0.3)\n"
        "            send({\"type\": \"event\", \"text\": \"tick\", \"payload\": {\"n\": 1}})\n"
        "        threading.Thread(target=emit, daemon=True).start()\n");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("watch")},
        {QStringLiteral("description"), QStringLiteral("监控")},
        {QStringLiteral("code"), code},
        {QStringLiteral("language"), QStringLiteral("py")},
        {QStringLiteral("mode"), QStringLiteral("push")},
    }, ctx);
    QVERIFY(r.success);

    r = invokeSync(source, QStringLiteral("watch"), QJsonObject{}, ctx);
    QVERIFY(r.success);
    QCOMPARE(r.text, QStringLiteral("subscribed"));

    // 脚本异步事件 → 单元邮箱
    QTRY_VERIFY_WITH_TIMEOUT(agent->hasPendingInboxMessages(), 10000);
    const QList<AgentInboxMessage> msgs = agent->takePendingInboxMessages();
    QCOMPARE(msgs.size(), 1);
    QCOMPARE(msgs.first().type, QStringLiteral("tool_event"));
    QCOMPARE(msgs.first().fromAgentId, QStringLiteral("watch"));
    QCOMPARE(msgs.first().payload.value(QStringLiteral("tool")).toString(),
             QStringLiteral("watch"));
    QCOMPARE(msgs.first().payload.value(QStringLiteral("n")).toInt(), 1);
}

void ScriptToolSourceTests::sessionClose_killsProcessesAndCleansEphemeral()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过会话关闭测试");
    }
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    source.setEphemeralDirectory(tmp.path() + QStringLiteral("/eph"));
    source.setRuntimeCommand(QStringLiteral("py"), python);
    source.setInvokeTimeoutMs(10000);

    const QString code = QStringLiteral(
        "import sys, json\n"
        "for line in sys.stdin:\n"
        "    req = json.loads(line)\n"
        "    if req.get(\"type\") == \"invoke\":\n"
        "        out = {\"type\": \"result\", \"id\": req[\"id\"], \"ok\": True, \"text\": \"ok\"}\n"
        "        sys.stdout.write(json.dumps(out) + \"\\n\")\n"
        "        sys.stdout.flush()\n");
    const QString ephFile = tmp.path() + QStringLiteral("/eph/agent-0/tmp.py");
    {
        SessionRuntime defaults;
        defaults.workingDirectory = tmp.path();
        AgentSessionConfig cfg;
        cfg.globalDefaults = &defaults;
        cfg.externalToolSource = &source;
        AgentSession session(cfg);
        ToolInvokeContext ctx;
        ctx.agentId = QStringLiteral("agent-0");
        ctx.session = &session;

        ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
            {QStringLiteral("name"), QStringLiteral("echo")},
            {QStringLiteral("description"), QStringLiteral("d")},
            {QStringLiteral("code"), code},
            {QStringLiteral("language"), QStringLiteral("py")},
        }, ctx);
        QVERIFY(r.success);
        r = invokeSync(source, QStringLiteral("create_tool"), {
            {QStringLiteral("name"), QStringLiteral("tmp")},
            {QStringLiteral("description"), QStringLiteral("d")},
            {QStringLiteral("code"), code},
            {QStringLiteral("language"), QStringLiteral("py")},
            {QStringLiteral("ephemeral"), true},
        }, ctx);
        QVERIFY(r.success);
        QVERIFY(QFile::exists(ephFile));

        // 调用一次 → 进程起来
        r = invokeSync(source, QStringLiteral("echo"), {
            {QStringLiteral("who"), QStringLiteral("x")},
        }, ctx);
        QVERIFY(r.success);
        QCOMPARE(source.processCount(), 1);
    }
    // 会话销毁 → 协调器析构 → sessionClosing：进程全关、临时文件删除、持久文件保留
    QCOMPARE(source.processCount(), 0);
    QVERIFY(!QFile::exists(ephFile));
    QVERIFY(QFile::exists(tmp.path() + QStringLiteral("/tools/agent-0/echo.py")));
}

void ScriptToolSourceTests::coordinator_removeSourceAndOwner()
{
    ToolCoordinator coordinator(nullptr);
    ScriptToolSource source;
    QSignalSpy spy(&coordinator, &ToolCoordinator::toolsUpdated);

    coordinator.addSource(&source, QStringLiteral("agent-0"));
    QCOMPARE(coordinator.sourceOwner(&source), QStringLiteral("agent-0"));
    QVERIFY(specCount(coordinator.allSpecs(), QStringLiteral("create_tool")) == 1);

    coordinator.removeSource(&source);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(coordinator.sourceOwner(&source), QString());
    QCOMPARE(specCount(coordinator.allSpecs(), QStringLiteral("create_tool")), 0);
}

QTEST_MAIN(ScriptToolSourceTests)
#include "ScriptToolSourceTests.moc"
