#include "tools/ScriptToolSource.h"
#include "tools/ToolCoordinator.h"
#include "tools/BuiltinToolRuntime.h"
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
                      const QJsonObject &input, const ToolInvokeContext &ctx,
                      const QString &callId = {})
{
    ToolResult result;
    bool done = false;
    ToolCall call;
    call.id = callId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : callId;
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
    void sessionClear_killsProcessesAndDropsEphemeral();
    void coordinator_removeSourceAndOwner();
    void metaTools_fillToolUseId();
    void createTool_codeDescriptionDocumentsEnvelope();
    void syncInvoke_bareJsonOneShot();
    void syncInvoke_bareJsonWithoutNewline();
    void syncInvoke_unknownTypeFailsImmediately();
    void pushInvoke_bareJsonRequiresEnvelope();
    void createTool_updateRestartsProcess();
    void syncInvoke_timeoutFailsThisCall();
    void syncInvoke_crashIncludesStderrTraceback();
    void syncInvoke_receivesWorkingDirectory();
    void inspectTool_listAndInspectSource();
    void scope_projectGlobalSession();
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
        QCOMPARE(specCount(specs, QStringLiteral("inspect_tool")), 1);
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
    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("inspect_tool")},
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
    r = invokeSync(source, QStringLiteral("delete_tool"), {
        {QStringLiteral("name"), QStringLiteral("inspect_tool")},
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

void ScriptToolSourceTests::sessionClear_killsProcessesAndDropsEphemeral()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过会话清空测试");
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
    r = invokeSync(source, QStringLiteral("echo"), {
        {QStringLiteral("who"), QStringLiteral("x")},
    }, ctx);
    QVERIFY(r.success);
    QCOMPARE(source.processCount(), 1);

    // 会话清空 → 进程全关、临时工具注销（文件删除）、持久工具保留
    session.clear();
    QCOMPARE(source.processCount(), 0);
    QVERIFY(!source.hasTool(QStringLiteral("tmp")));
    QVERIFY(!QFile::exists(ephFile));
    QVERIFY(source.hasTool(QStringLiteral("echo")));
    QVERIFY(QFile::exists(tmp.path() + QStringLiteral("/tools/agent-0/echo.py")));

    // 持久工具下次调用懒重启进程
    r = invokeSync(source, QStringLiteral("echo"), {
        {QStringLiteral("who"), QStringLiteral("y")},
    }, ctx);
    QVERIFY(r.success);
    QCOMPARE(source.processCount(), 1);
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

void ScriptToolSourceTests::metaTools_fillToolUseId()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");

    const QString createId = QStringLiteral("call-create-1");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("hello")},
        {QStringLiteral("description"), QStringLiteral("打招呼")},
        {QStringLiteral("code"), QStringLiteral("print('hi')")},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx, createId);
    QVERIFY(r.success);
    QCOMPARE(r.toolUseId, createId);

    const QString badId = QStringLiteral("call-create-bad");
    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("bad name")},
        {QStringLiteral("description"), QStringLiteral("d")},
        {QStringLiteral("code"), QStringLiteral("x")},
    }, ctx, badId);
    QVERIFY(r.isError);
    QCOMPARE(r.toolUseId, badId);

    const QString deleteId = QStringLiteral("call-delete-1");
    r = invokeSync(source, QStringLiteral("delete_tool"), {
        {QStringLiteral("name"), QStringLiteral("hello")},
    }, ctx, deleteId);
    QVERIFY(r.success);
    QCOMPARE(r.toolUseId, deleteId);

    BuiltinToolRuntime runtime;
    ToolCoordinator coordinator(nullptr);
    coordinator.addSource(&source, QStringLiteral("agent-0"));
    ToolCall call;
    call.id = QStringLiteral("call-dispatch-1");
    call.toolName = QStringLiteral("create_tool");
    call.input = {
        {QStringLiteral("name"), QStringLiteral("via_dispatch")},
        {QStringLiteral("description"), QStringLiteral("d")},
        {QStringLiteral("code"), QStringLiteral("x")},
        {QStringLiteral("language"), QStringLiteral("py")},
    };
    ToolResult dispatched;
    bool done = false;
    coordinator.dispatch(QStringLiteral("agent-0"), call, tmp.path(), runtime,
                         [&](ToolResult tr) {
                             dispatched = std::move(tr);
                             done = true;
                         });
    QVERIFY(done);
    QVERIFY(dispatched.success);
    QCOMPARE(dispatched.toolUseId, call.id);
}

void ScriptToolSourceTests::createTool_codeDescriptionDocumentsEnvelope()
{
    ScriptToolSource source;
    QString codeDesc;
    QString modeDesc;
    for (const ToolSpec &spec : source.specs()) {
        if (spec.name != QStringLiteral("create_tool")) {
            continue;
        }
        const QJsonObject props = spec.inputSchema.value(QStringLiteral("properties")).toObject();
        codeDesc = props.value(QStringLiteral("code")).toObject()
                       .value(QStringLiteral("description")).toString();
        modeDesc = props.value(QStringLiteral("mode")).toObject()
                       .value(QStringLiteral("description")).toString();
    }
    QVERIFY(codeDesc.contains(QStringLiteral("无 type")));
    QVERIFY(codeDesc.contains(QStringLiteral("type=result")));
    QVERIFY(codeDesc.contains(QStringLiteral("push")));
    QVERIFY(codeDesc.contains(QStringLiteral("workingDirectory")));
    QVERIFY(modeDesc.contains(QStringLiteral("信封")));
}

void ScriptToolSourceTests::syncInvoke_bareJsonOneShot()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过裸 JSON 同步调用测试");
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
        "req = json.loads(sys.stdin.readline())\n"
        "who = (req.get(\"args\") or {}).get(\"who\", \"\")\n"
        "print(json.dumps({\"ok\": True, \"text\": \"bare:\" + str(who)}), flush=True)\n");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("stats")},
        {QStringLiteral("description"), QStringLiteral("一次性")},
        {QStringLiteral("code"), code},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);

    r = invokeSync(source, QStringLiteral("stats"), {
        {QStringLiteral("who"), QStringLiteral("world")},
    }, ctx);
    QVERIFY(r.success);
    QVERIFY(!r.isError);
    QCOMPARE(r.text, QStringLiteral("bare:world"));
    QVERIFY(!r.text.contains(QStringLiteral("脚本进程退出")));
}

void ScriptToolSourceTests::syncInvoke_bareJsonWithoutNewline()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过无换行裸 JSON 测试");
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
        "req = json.loads(sys.stdin.readline())\n"
        "who = (req.get(\"args\") or {}).get(\"who\", \"\")\n"
        "sys.stdout.write(json.dumps({\"ok\": True, \"text\": \"nl:\" + str(who)}))\n"
        "sys.stdout.flush()\n");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("noline")},
        {QStringLiteral("description"), QStringLiteral("无换行")},
        {QStringLiteral("code"), code},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);

    r = invokeSync(source, QStringLiteral("noline"), {
        {QStringLiteral("who"), QStringLiteral("x")},
    }, ctx);
    QVERIFY(r.success);
    QCOMPARE(r.text, QStringLiteral("nl:x"));
}

void ScriptToolSourceTests::syncInvoke_unknownTypeFailsImmediately()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过协议错误测试");
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
        "req = json.loads(sys.stdin.readline())\n"
        "print(json.dumps({\"type\": \"nope\", \"id\": req.get(\"id\"), \"ok\": True, \"text\": \"x\"}), flush=True)\n");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("badtype")},
        {QStringLiteral("description"), QStringLiteral("错类型")},
        {QStringLiteral("code"), code},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);

    r = invokeSync(source, QStringLiteral("badtype"), QJsonObject{}, ctx);
    QVERIFY(r.isError);
    QVERIFY(r.text.contains(QStringLiteral("需要 type=result 且带回请求 id")));
    QVERIFY(!r.text.contains(QStringLiteral("脚本进程退出")));
}

void ScriptToolSourceTests::pushInvoke_bareJsonRequiresEnvelope()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过 push 信封测试");
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
        "req = json.loads(sys.stdin.readline())\n"
        "print(json.dumps({\"ok\": True, \"text\": \"bare\"}), flush=True)\n");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("pushbare")},
        {QStringLiteral("description"), QStringLiteral("push 裸 JSON")},
        {QStringLiteral("code"), code},
        {QStringLiteral("language"), QStringLiteral("py")},
        {QStringLiteral("mode"), QStringLiteral("push")},
    }, ctx);
    QVERIFY(r.success);

    r = invokeSync(source, QStringLiteral("pushbare"), QJsonObject{}, ctx);
    QVERIFY(r.isError);
    QVERIFY(r.text.contains(QStringLiteral("需要 type=result 且带回请求 id")));
}

void ScriptToolSourceTests::createTool_updateRestartsProcess()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过更新重启进程测试");
    }
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    source.setRuntimeCommand(QStringLiteral("py"), python);
    source.setInvokeTimeoutMs(10000);
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");

    const QString loopCode = QStringLiteral(
        "import sys, json\n"
        "for line in sys.stdin:\n"
        "    req = json.loads(line)\n"
        "    if req.get(\"type\") == \"invoke\":\n"
        "        out = {\"type\": \"result\", \"id\": req[\"id\"], \"ok\": True, \"text\": \"%1\"}\n"
        "        sys.stdout.write(json.dumps(out) + \"\\n\")\n"
        "        sys.stdout.flush()\n");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("echo")},
        {QStringLiteral("description"), QStringLiteral("v1")},
        {QStringLiteral("code"), loopCode.arg(QStringLiteral("v1"))},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);
    r = invokeSync(source, QStringLiteral("echo"), QJsonObject{}, ctx);
    QVERIFY(r.success);
    QCOMPARE(r.text, QStringLiteral("v1"));
    QCOMPARE(source.processCount(), 1);

    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("echo")},
        {QStringLiteral("description"), QStringLiteral("v2")},
        {QStringLiteral("code"), loopCode.arg(QStringLiteral("v2"))},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);
    QCOMPARE(source.processCount(), 0);

    r = invokeSync(source, QStringLiteral("echo"), QJsonObject{}, ctx);
    QVERIFY(r.success);
    QCOMPARE(r.text, QStringLiteral("v2"));
}

void ScriptToolSourceTests::syncInvoke_timeoutFailsThisCall()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过超时回调测试");
    }
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    source.setRuntimeCommand(QStringLiteral("py"), python);
    source.setInvokeTimeoutMs(400);
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");

    const QString code = QStringLiteral(
        "import sys, time\n"
        "sys.stdin.readline()\n"
        "time.sleep(30)\n");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("hang")},
        {QStringLiteral("description"), QStringLiteral("挂起")},
        {QStringLiteral("code"), code},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);

    r = invokeSync(source, QStringLiteral("hang"), QJsonObject{}, ctx);
    QVERIFY(r.isError);
    QVERIFY(r.text.contains(QStringLiteral("脚本工具调用超时")));
}

void ScriptToolSourceTests::syncInvoke_crashIncludesStderrTraceback()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过崩溃 Traceback 捕获测试");
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
        "import sys\n"
        "sys.stdin.readline()\n"
        "raise RuntimeError(\"custom_crash_traceback_marker\")\n");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("crash_tool")},
        {QStringLiteral("description"), QStringLiteral("故意抛异常")},
        {QStringLiteral("code"), code},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);

    r = invokeSync(source, QStringLiteral("crash_tool"), QJsonObject{}, ctx);
    QVERIFY(r.isError);
    QVERIFY(r.text.contains(QStringLiteral("脚本进程退出")));
    QVERIFY(r.text.contains(QStringLiteral("RuntimeError")));
    QVERIFY(r.text.contains(QStringLiteral("custom_crash_traceback_marker")));
}

void ScriptToolSourceTests::syncInvoke_receivesWorkingDirectory()
{
    const QString python = findPython();
    if (python.isEmpty()) {
        QSKIP("未找到 python 运行时，跳过 workingDirectory 传递测试");
    }
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    source.setRuntimeCommand(QStringLiteral("py"), python);
    source.setInvokeTimeoutMs(10000);
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    ctx.workingDirectory = tmp.path() + QStringLiteral("/my_workspace");

    const QString code = QStringLiteral(
        "import sys, json\n"
        "req = json.loads(sys.stdin.readline())\n"
        "cwd = req.get(\"workingDirectory\", \"\")\n"
        "print(json.dumps({\"ok\": True, \"text\": \"cwd:\" + cwd}), flush=True)\n");
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("cwd_tool")},
        {QStringLiteral("description"), QStringLiteral("回显工作区")},
        {QStringLiteral("code"), code},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);

    r = invokeSync(source, QStringLiteral("cwd_tool"), QJsonObject{}, ctx);
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("cwd:")));
    QVERIFY(r.text.contains(QStringLiteral("my_workspace")));
}

void ScriptToolSourceTests::inspectTool_listAndInspectSource()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    ScriptToolSource source;
    source.setToolDirectory(tmp.path() + QStringLiteral("/tools"));
    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    ctx.workingDirectory = tmp.path();

    // 1. 空状态：无工具
    ToolResult r = invokeSync(source, QStringLiteral("inspect_tool"), QJsonObject{}, ctx);
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("没有任何自建工具")));

    // 2. 创建一个工具
    const QString sampleCode = QStringLiteral("import sys\nprint('custom code 123')\n");
    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("calc")},
        {QStringLiteral("description"), QStringLiteral("计算器工具")},
        {QStringLiteral("code"), sampleCode},
        {QStringLiteral("language"), QStringLiteral("py")},
    }, ctx);
    QVERIFY(r.success);

    // 3. 查清单（空 name）
    r = invokeSync(source, QStringLiteral("inspect_tool"), QJsonObject{}, ctx);
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("calc")));
    QVERIFY(r.text.contains(QStringLiteral("计算器工具")));
    QVERIFY(r.payload.value(QStringLiteral("tools")).toArray().size() == 1);

    // 4. 查具体工具源码（带 name="calc"）
    r = invokeSync(source, QStringLiteral("inspect_tool"), {
        {QStringLiteral("name"), QStringLiteral("calc")},
    }, ctx);
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("print('custom code 123')")));
    QVERIFY(r.text.contains(QStringLiteral("create_tool"))); // 包含修改指引
    QCOMPARE(r.payload.value(QStringLiteral("name")).toString(), QStringLiteral("calc"));
    QCOMPARE(r.payload.value(QStringLiteral("scope")).toString(), QStringLiteral("project"));
    QCOMPARE(r.payload.value(QStringLiteral("language")).toString(), QStringLiteral("py"));
    QCOMPARE(r.payload.value(QStringLiteral("code")).toString().trimmed(), sampleCode.trimmed());

    // 5. 查不存在的工具
    r = invokeSync(source, QStringLiteral("inspect_tool"), {
        {QStringLiteral("name"), QStringLiteral("ghost")},
    }, ctx);
    QVERIFY(r.isError);
    QVERIFY(r.text.contains(QStringLiteral("不存在")));

    // 6. 查元工具（非自建工具）
    r = invokeSync(source, QStringLiteral("inspect_tool"), {
        {QStringLiteral("name"), QStringLiteral("create_tool")},
    }, ctx);
    QVERIFY(r.isError);
    QVERIFY(r.text.contains(QStringLiteral("系统元工具")));

    // 7. 验证 specs 中自建工具的 description 自动注入标识
    const QList<ToolSpec> specs = source.specs();
    bool foundCalc = false;
    for (const ToolSpec &spec : specs) {
        if (spec.name == QStringLiteral("calc")) {
            foundCalc = true;
            QVERIFY(spec.description.contains(QStringLiteral("[自建工具 | 作用域: project | py/sync]")));
            QVERIFY(spec.description.contains(QStringLiteral("inspect_tool")));
        }
    }
    QVERIFY(foundCalc);
}

void ScriptToolSourceTests::scope_projectGlobalSession()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString globalDir = tmp.path() + QStringLiteral("/global_tools");
    const QString projectDir = tmp.path() + QStringLiteral("/my_project/.agent/tools");
    const QString ephDir = tmp.path() + QStringLiteral("/ephemeral_tools");

    ScriptToolSource source;
    source.setGlobalDirectory(globalDir);
    source.setProjectDirectory(projectDir);
    source.setEphemeralDirectory(ephDir);

    ToolInvokeContext ctx;
    ctx.agentId = QStringLiteral("agent-0");
    ctx.workingDirectory = tmp.path() + QStringLiteral("/my_project");

    // 1. 创建全局工具
    ToolResult r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("tool_g")},
        {QStringLiteral("description"), QStringLiteral("全局工具")},
        {QStringLiteral("code"), QStringLiteral("print('g')")},
        {QStringLiteral("language"), QStringLiteral("py")},
        {QStringLiteral("scope"), QStringLiteral("global")},
    }, ctx);
    QVERIFY(r.success);
    QVERIFY(QFile::exists(globalDir + QStringLiteral("/agent-0/tool_g.py")));

    // 2. 创建项目工具
    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("tool_p")},
        {QStringLiteral("description"), QStringLiteral("项目工具")},
        {QStringLiteral("code"), QStringLiteral("print('p')")},
        {QStringLiteral("language"), QStringLiteral("py")},
        {QStringLiteral("scope"), QStringLiteral("project")},
    }, ctx);
    QVERIFY(r.success);
    QVERIFY(QFile::exists(projectDir + QStringLiteral("/agent-0/tool_p.py")));

    // 3. 创建临时/会话工具
    r = invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("tool_s")},
        {QStringLiteral("description"), QStringLiteral("会话工具")},
        {QStringLiteral("code"), QStringLiteral("print('s')")},
        {QStringLiteral("language"), QStringLiteral("py")},
        {QStringLiteral("scope"), QStringLiteral("session")},
    }, ctx);
    QVERIFY(r.success);
    QVERIFY(QFile::exists(ephDir + QStringLiteral("/agent-0/tool_s.py")));

    // 4. inspect_tool 验证作用域信息透传
    r = invokeSync(source, QStringLiteral("inspect_tool"), {
        {QStringLiteral("name"), QStringLiteral("tool_g")},
    }, ctx);
    QVERIFY(r.success);
    QCOMPARE(r.payload.value(QStringLiteral("scope")).toString(), QStringLiteral("global"));
    QVERIFY(r.text.contains(QStringLiteral("全局级")));

    r = invokeSync(source, QStringLiteral("inspect_tool"), {
        {QStringLiteral("name"), QStringLiteral("tool_p")},
    }, ctx);
    QVERIFY(r.success);
    QCOMPARE(r.payload.value(QStringLiteral("scope")).toString(), QStringLiteral("project"));
    QVERIFY(r.text.contains(QStringLiteral("项目级")));

    r = invokeSync(source, QStringLiteral("inspect_tool"), {
        {QStringLiteral("name"), QStringLiteral("tool_s")},
    }, ctx);
    QVERIFY(r.success);
    QCOMPARE(r.payload.value(QStringLiteral("scope")).toString(), QStringLiteral("session"));
    QVERIFY(r.text.contains(QStringLiteral("会话级")));

    // 5. 优先级遮蔽：创建同名全局工具与项目工具
    invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("override_tool")},
        {QStringLiteral("description"), QStringLiteral("全局版本")},
        {QStringLiteral("code"), QStringLiteral("print('global_ver')")},
        {QStringLiteral("language"), QStringLiteral("py")},
        {QStringLiteral("scope"), QStringLiteral("global")},
    }, ctx);

    // 再建项目同名工具，应当遮蔽全局版本
    invokeSync(source, QStringLiteral("create_tool"), {
        {QStringLiteral("name"), QStringLiteral("override_tool")},
        {QStringLiteral("description"), QStringLiteral("项目版本")},
        {QStringLiteral("code"), QStringLiteral("print('project_ver')")},
        {QStringLiteral("language"), QStringLiteral("py")},
        {QStringLiteral("scope"), QStringLiteral("project")},
    }, ctx);

    r = invokeSync(source, QStringLiteral("inspect_tool"), {
        {QStringLiteral("name"), QStringLiteral("override_tool")},
    }, ctx);
    QVERIFY(r.success);
    QCOMPARE(r.payload.value(QStringLiteral("scope")).toString(), QStringLiteral("project"));
    QVERIFY(r.text.contains(QStringLiteral("project_ver")));

    // 6. 会话清空：临时工具被删除，全局和项目工具保留
    source.sessionCleared();
    QVERIFY(!source.hasTool(QStringLiteral("tool_s")));
    QVERIFY(!QFile::exists(ephDir + QStringLiteral("/agent-0/tool_s.py")));
    QVERIFY(source.hasTool(QStringLiteral("tool_g")));
    QVERIFY(source.hasTool(QStringLiteral("tool_p")));
    QVERIFY(QFile::exists(globalDir + QStringLiteral("/agent-0/tool_g.py")));
    QVERIFY(QFile::exists(projectDir + QStringLiteral("/agent-0/tool_p.py")));
}

QTEST_MAIN(ScriptToolSourceTests)
#include "ScriptToolSourceTests.moc"
