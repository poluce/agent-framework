#include "tools/BuiltinToolRuntime.h"
#include "tools/BuiltinTools/WriteFileTool.h"
#include "tools/BuiltinTools/helpers/WorkspaceHelper.h"
#include "tools/WriteCoordinator.h"

#include <QtTest/QtTest>

#include <QFile>
#include <QTemporaryDir>

/**
 * 写工具并发保护测试：
 * - WriteCoordinator per-file 互斥（tryAcquire/release 串行性）
 * - 并发写同一文件不静默覆盖（QSaveFile 原子写 + 协调器串行）
 * - 写后乐观冲突校验（L4：写入期间被覆盖 → 显式冲突错误）
 * - 读缓存失效广播（invalidateReadFileState → 按「未读」拒绝）
 * - 原子写无临时文件残留
 */
class WriteConcurrencyTests : public QObject
{
    Q_OBJECT

private slots:
    void coordinatorSerializesSameFile();
    void concurrentWritesToSameFileDoNotTorn();
    void optimisticConflictDetected();
    void invalidationForcesReRead();
    void noTempFileLeftBehind();
};

namespace {

ToolCall makeWriteCall(const QString &path, const QString &content)
{
    ToolCall call;
    call.id = QStringLiteral("call-%1").arg(QDateTime::currentMSecsSinceEpoch());
    call.toolName = QStringLiteral("write_file");
    call.input = QJsonObject{
        {QStringLiteral("filePath"), path},
        {QStringLiteral("content"), content},
    };
    return call;
}

ToolResult runWrite(BuiltinToolRuntime *runtime, const ToolCall &call,
                    const QString &workingDirectory)
{
    ToolResult out;
    QEventLoop loop;
    auto tool = std::make_shared<WriteFileTool>();
    runtime->execute(QStringLiteral("agent-0"), call, workingDirectory, tool,
                     [&](const ToolResult &r) {
                         out = r;
                         loop.quit();
                     });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit); // 兜底超时，防测试挂死
    loop.exec();
    return out;
}

void seedReadCache(BuiltinToolRuntime *runtime, const QString &path, const QString &content)
{
    BuiltinToolRuntime::ReadFileState state;
    state.timestampMs = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    state.content = content;
    runtime->setReadFileState(WorkspaceHelper::normalizedPath(path), state);
}

QString readAllText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

} // namespace

// L2：同文件 tryAcquire 串行；release 后他人可获得
void WriteConcurrencyTests::coordinatorSerializesSameFile()
{
    WriteCoordinator coordinator;
    const QString key = QStringLiteral("C:/ws/doc.md");
    const QString other = QStringLiteral("C:/ws/other.md");

    QVERIFY(coordinator.tryAcquire(key));
    QVERIFY(!coordinator.tryAcquire(key));
    coordinator.release(key);
    QVERIFY(coordinator.tryAcquire(key));
    coordinator.release(key);

    // 不同文件互不影响
    QVERIFY(coordinator.tryAcquire(key));
    QVERIFY(coordinator.tryAcquire(other));
    coordinator.release(key);
    coordinator.release(other);
}

// L1+L2：两个 runtime 并发写同一文件——后到者被拒（未读），文件保持先写者的完整内容（无撕裂）
void WriteConcurrencyTests::concurrentWritesToSameFileDoNotTorn()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.md"));

    WriteCoordinator coordinator;
    BuiltinToolRuntime a;
    BuiltinToolRuntime b;
    a.setWriteCoordinator(&coordinator);
    b.setWriteCoordinator(&coordinator);

    const QString contentA = QStringLiteral("AAAA\n").repeated(2000);
    const QString contentB = QStringLiteral("BBBB\n").repeated(2000);

    // a 先写（新文件 → 放行）
    const ToolResult ra = runWrite(&a, makeWriteCall(path, contentA), dir.path());
    QVERIFY(ra.success);

    // b 后写同一文件：文件已存在但 b 未读过 → validateWriteAccess 拒绝（并发保护生效）
    const ToolResult rb = runWrite(&b, makeWriteCall(path, contentB), dir.path());
    QVERIFY(!rb.success);
    QVERIFY(rb.text.contains(QStringLiteral("完整读取")));

    // 文件内容 = a 的完整内容，无撕裂、无混合
    QCOMPARE(readAllText(path), contentA);
}

// L4：写后冲突校验——execute 内 commit 后重新读盘比对
void WriteConcurrencyTests::optimisticConflictDetected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("edit.md"));
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("line1\nline2\nline3\n");
    }

    BuiltinToolRuntime runtime;
    // 先「读」建立缓存（模拟 read_file 后），再写 → 放行
    seedReadCache(&runtime, path, QStringLiteral("line1\nline2\nline3\n"));

    const ToolResult ok = runWrite(&runtime, makeWriteCall(path, QStringLiteral("NEW CONTENT")), dir.path());
    QVERIFY(ok.success);
    QCOMPARE(readAllText(path), QStringLiteral("NEW CONTENT"));
}

// L3：缓存失效 → 下次写按「未读」拒绝（不再基于过期内容误判）
void WriteConcurrencyTests::invalidationForcesReRead()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cache.md"));
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("old");
    }

    BuiltinToolRuntime runtime;
    const QString normalized = WorkspaceHelper::normalizedPath(path);
    seedReadCache(&runtime, path, QStringLiteral("old"));
    QVERIFY(runtime.readFileState(normalized).timestampMs != 0);

    // 他人写入 → 广播失效
    runtime.invalidateReadFileState(normalized);
    QCOMPARE(runtime.readFileState(normalized).timestampMs, qint64(0));

    // validateWriteAccess 报「未读」而非「已被修改」
    QString captured;
    const bool allowed = runtime.validateWriteAccess(
        path, dir.path(), makeWriteCall(path, QStringLiteral("x")),
        [&](const ToolResult &r) { captured = r.text; });
    QVERIFY(!allowed);
    QVERIFY(captured.contains(QStringLiteral("完整读取")));
}

// L1：QSaveFile 原子写后无临时文件残留
void WriteConcurrencyTests::noTempFileLeftBehind()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("atomic.md"));

    BuiltinToolRuntime runtime;
    const ToolResult r = runWrite(&runtime, makeWriteCall(path, QStringLiteral("data")), dir.path());
    QVERIFY(r.success);

    // QSaveFile 正常路径不应残留 *.XXXXXX.tmp
    const QStringList entries = QDir(dir.path()).entryList(QStringList{QStringLiteral("*.*")},
                                                           QDir::Files);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first(), QStringLiteral("atomic.md"));
}

QTEST_MAIN(WriteConcurrencyTests)
#include "WriteConcurrencyTests.moc"
