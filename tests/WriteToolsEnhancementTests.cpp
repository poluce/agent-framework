#include "tools/BuiltinToolRuntime.h"
#include "tools/BuiltinTools/WriteFileTool.h"
#include "tools/BuiltinTools/EditTool.h"
#include "tools/BuiltinTools/ReadFileTool.h"
#include "tools/BuiltinTools/MultiEditTool.h"
#include "tools/BuiltinTools/helpers/WorkspaceHelper.h"
#include "tools/WriteCoordinator.h"

#include <QtTest/QtTest>

#include <QFile>
#include <QTemporaryDir>

/**
 * 写工具增强：append / 行号 / 匹配增强 / multi_edit / 防护 / dry-run+CRLF / 旧行为回归
 */
class WriteToolsEnhancementTests : public QObject
{
    Q_OBJECT

private slots:
    // B append
    void appendToExistingFile();
    void appendNoAutoNewline();
    void appendRequiresExistingFile();
    // C 行号
    void lineReplace();
    void lineInsert();
    void lineDelete();
    void lineOutOfRange();
    void lineModeOnEmptyFile();
    void oldStringAndLineModeMutuallyExclusive();
    // C2 匹配增强
    void regexReplace();
    void regexTooComplexRejected();
    void fuzzyWhitespaceTolerant();
    void fuzzyAndRegexMutuallyExclusive();
    // D multi_edit
    void multiEditAllSuccess();
    void multiEditPartialFailureNoRollback();
    void multiEditChainedSameFile();
    void multiEditSameFileFailureSkipsRest();
    // E 防护
    void directoryPathRejected();
    void oversizedContentRejected();
    // F dry-run + CRLF
    void dryRunDoesNotWrite();
    void dryRunStillValidatesRead();
    void crlfPreservedOnWrite();
    // 旧行为回归
    void oldBehaviorUnchanged();
    // G 校验状态机 5 根因回归（非 ASCII / 无尾换行 / multi_edit 缓存 / partialView）
    void editNonAsciiFileSucceeds();
    void restoreLineEndingsNoTrailingNewline();
    void multiEditThenWriteSameFileSucceeds();
    void partialViewFullReadToEndIsFalse();
    // H 校验三档区分（oldString 放行 / 行号拒 / write 拒 / append 放行 / mtime 防外部修改）
    void editOldStringWithPartialReadSucceeds();
    void editLineModeWithPartialReadRejected();
    void writeFileWithPartialReadRejected();
    void appendWithNoReadSucceeds();
    void externalModificationStillRejected();
};

namespace {

QJsonObject inputWith(const QString &path, const QJsonObject &extra = {})
{
    QJsonObject input{{QStringLiteral("filePath"), path}};
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        input.insert(it.key(), it.value());
    return input;
}

ToolCall makeToolCall(const QString &toolName, const QString &path, const QJsonObject &extra = {})
{
    ToolCall call;
    call.id = QStringLiteral("call-%1").arg(QDateTime::currentMSecsSinceEpoch());
    call.toolName = toolName;
    call.input = inputWith(path, extra);
    return call;
}

ToolCall makeWriteCall(const QString &path, const QJsonObject &extra = {})
{
    return makeToolCall(QStringLiteral("write_file"), path, extra);
}

ToolCall makeEditCall(const QString &path, const QJsonObject &extra = {})
{
    return makeToolCall(QStringLiteral("edit"), path, extra);
}

ToolCall makeReadCall(const QString &path, const QJsonObject &extra = {})
{
    return makeToolCall(QStringLiteral("read_file"), path, extra);
}

ToolCall makeMultiEditCall(const QJsonArray &edits)
{
    ToolCall call;
    call.id = QStringLiteral("call-%1").arg(QDateTime::currentMSecsSinceEpoch());
    call.toolName = QStringLiteral("multi_edit");
    call.input = QJsonObject{{QStringLiteral("edits"), edits}};
    return call;
}

ToolResult runTool(BuiltinToolRuntime *runtime, const ToolCall &call,
                   const QString &workingDirectory, std::shared_ptr<AbstractBuiltinTool> tool)
{
    ToolResult out;
    QEventLoop loop;
    runtime->execute(QStringLiteral("agent-0"), call, workingDirectory, tool,
                     [&](const ToolResult &r) {
                         out = r;
                         loop.quit();
                     });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    return out;
}

ToolResult runWrite(BuiltinToolRuntime *runtime, const ToolCall &call, const QString &wd)
{
    return runTool(runtime, call, wd, std::make_shared<WriteFileTool>());
}

ToolResult runEdit(BuiltinToolRuntime *runtime, const ToolCall &call, const QString &wd)
{
    return runTool(runtime, call, wd, std::make_shared<EditTool>());
}

ToolResult runMultiEdit(BuiltinToolRuntime *runtime, const ToolCall &call, const QString &wd)
{
    return runTool(runtime, call, wd, std::make_shared<MultiEditTool>());
}

ToolResult runRead(BuiltinToolRuntime *runtime, const ToolCall &call, const QString &wd)
{
    return runTool(runtime, call, wd, std::make_shared<ReadFileTool>());
}

void seedReadCache(BuiltinToolRuntime *runtime, const QString &path, const QString &content)
{
    BuiltinToolRuntime::ReadFileState state;
    state.timestampMs = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    state.content = content;
    runtime->setReadFileState(WorkspaceHelper::normalizedPath(path), state);
}

void writeFileRaw(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(bytes);
}

QByteArray readFileRaw(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

} // namespace

// ── append ──

void WriteToolsEnhancementTests::appendToExistingFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("log.md"));
    writeFileRaw(path, "base\n");

    BuiltinToolRuntime runtime;
    // append 豁免「先读」：不需 seedReadCache
    const ToolResult r = runWrite(&runtime, makeWriteCall(path, QJsonObject{
        {QStringLiteral("content"), QStringLiteral("added\n")},
        {QStringLiteral("append"), true},
    }), dir.path());
    QVERIFY(r.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("base\nadded\n"));
}

void WriteToolsEnhancementTests::appendNoAutoNewline()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("no_nl.md"));
    writeFileRaw(path, "abc");

    BuiltinToolRuntime runtime;
    const ToolResult r = runWrite(&runtime, makeWriteCall(path, QJsonObject{
        {QStringLiteral("content"), QStringLiteral("def")},
        {QStringLiteral("append"), true},
    }), dir.path());
    QVERIFY(r.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("abcdef")); // 无自动换行
}

void WriteToolsEnhancementTests::appendRequiresExistingFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("new.md"));

    BuiltinToolRuntime runtime;
    const ToolResult r = runWrite(&runtime, makeWriteCall(path, QJsonObject{
        {QStringLiteral("content"), QStringLiteral("x")},
        {QStringLiteral("append"), true},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("append 模式要求文件已存在")));
}

// ── 行号 ──

void WriteToolsEnhancementTests::lineReplace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.md"));
    writeFileRaw(path, "a\nb\nc\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "a\nb\nc\n");
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("editMode"), QStringLiteral("replace")},
        {QStringLiteral("startLine"), 2},
        {QStringLiteral("endLine"), 2},
        {QStringLiteral("newString"), QStringLiteral("B")},
    }), dir.path());
    QVERIFY(r.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("a\nB\nc\n"));
}

void WriteToolsEnhancementTests::lineInsert()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("b.md"));
    writeFileRaw(path, "a\nb\nc\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "a\nb\nc\n");
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("editMode"), QStringLiteral("insert")},
        {QStringLiteral("startLine"), 2},
        {QStringLiteral("newString"), QStringLiteral("x\ny")},
    }), dir.path());
    QVERIFY(r.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("a\nb\nx\ny\nc\n"));
}

void WriteToolsEnhancementTests::lineDelete()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("c.md"));
    writeFileRaw(path, "a\nb\nc\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "a\nb\nc\n");
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("editMode"), QStringLiteral("delete")},
        {QStringLiteral("startLine"), 2},
        {QStringLiteral("endLine"), 3},
    }), dir.path());
    QVERIFY(r.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("a\n"));
}

void WriteToolsEnhancementTests::lineOutOfRange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("d.md"));
    writeFileRaw(path, "a\nb\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "a\nb\n");
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("startLine"), 99},
        {QStringLiteral("newString"), QStringLiteral("x")},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("超出文件行数范围")));
}

void WriteToolsEnhancementTests::lineModeOnEmptyFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("empty.md"));
    writeFileRaw(path, "");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "");
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("startLine"), 1},
        {QStringLiteral("newString"), QStringLiteral("x")},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("文件为空")));
}

void WriteToolsEnhancementTests::oldStringAndLineModeMutuallyExclusive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("e.md"));
    writeFileRaw(path, "a\nb\n");

    BuiltinToolRuntime runtime;
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("a")},
        {QStringLiteral("newString"), QStringLiteral("X")},
        {QStringLiteral("startLine"), 1},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("互斥")));
}

// ── 匹配增强 ──

void WriteToolsEnhancementTests::regexReplace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("f.md"));
    writeFileRaw(path, "foo123\nbar456\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "foo123\nbar456\n");
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("\\d+")},
        {QStringLiteral("newString"), QStringLiteral("NUM")},
        {QStringLiteral("useRegex"), true},
    }), dir.path());
    QVERIFY(r.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("fooNUM\nbar456\n"));
}

void WriteToolsEnhancementTests::regexTooComplexRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("g.md"));
    writeFileRaw(path, "aaa\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "aaa\n");
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("(a+)+$")},
        {QStringLiteral("newString"), QStringLiteral("x")},
        {QStringLiteral("useRegex"), true},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("嵌套量词")));
}

void WriteToolsEnhancementTests::fuzzyWhitespaceTolerant()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("h.md"));
    writeFileRaw(path, "if (x) {\n    doSomething();\n}\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "if (x) {\n    doSomething();\n}\n");
    // oldString 缩进不同（2 空格 vs 实际 4 空格）→ fuzzy 仍命中
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("  doSomething();")},
        {QStringLiteral("newString"), QStringLiteral("  doSomethingElse();")},
        {QStringLiteral("fuzzy"), true},
    }), dir.path());
    QVERIFY(r.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("if (x) {\n    doSomethingElse();\n}\n"));
}

void WriteToolsEnhancementTests::fuzzyAndRegexMutuallyExclusive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("i.md"));
    writeFileRaw(path, "a\n");

    BuiltinToolRuntime runtime;
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("a")},
        {QStringLiteral("newString"), QStringLiteral("b")},
        {QStringLiteral("useRegex"), true},
        {QStringLiteral("fuzzy"), true},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("互斥")));
}

// ── multi_edit ──

void WriteToolsEnhancementTests::multiEditAllSuccess()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString p1 = dir.filePath(QStringLiteral("one.md"));
    const QString p2 = dir.filePath(QStringLiteral("two.md"));
    writeFileRaw(p1, "a\nb\n");
    writeFileRaw(p2, "x\ny\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, p1, "a\nb\n");
    seedReadCache(&runtime, p2, "x\ny\n");
    const QJsonArray edits{QJsonObject{{QStringLiteral("filePath"), QStringLiteral("one.md")},
                                       {QStringLiteral("oldString"), QStringLiteral("a")},
                                       {QStringLiteral("newString"), QStringLiteral("A")}},
                           QJsonObject{{QStringLiteral("filePath"), QStringLiteral("two.md")},
                                       {QStringLiteral("oldString"), QStringLiteral("x")},
                                       {QStringLiteral("newString"), QStringLiteral("X")}}};
    const ToolResult r = runMultiEdit(&runtime, makeMultiEditCall(edits), dir.path());
    QVERIFY(r.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(p1)), QStringLiteral("A\nb\n"));
    QCOMPARE(QString::fromUtf8(readFileRaw(p2)), QStringLiteral("X\ny\n"));
}

void WriteToolsEnhancementTests::multiEditPartialFailureNoRollback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString p1 = dir.filePath(QStringLiteral("one.md"));
    const QString p2 = dir.filePath(QStringLiteral("two.md"));
    writeFileRaw(p1, "a\n");
    writeFileRaw(p2, "x\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, p1, "a\n");
    seedReadCache(&runtime, p2, "x\n");
    const QJsonArray edits{QJsonObject{{QStringLiteral("filePath"), QStringLiteral("one.md")},
                                       {QStringLiteral("oldString"), QStringLiteral("a")},
                                       {QStringLiteral("newString"), QStringLiteral("A")}},
                           QJsonObject{{QStringLiteral("filePath"), QStringLiteral("two.md")},
                                       {QStringLiteral("oldString"), QStringLiteral("NOT_THERE")},
                                       {QStringLiteral("newString"), QStringLiteral("X")}}};
    const ToolResult r = runMultiEdit(&runtime, makeMultiEditCall(edits), dir.path());
    QVERIFY(!r.success); // 部分失败 → success=false
    QVERIFY(r.text.contains(QStringLiteral("1 个文件成功")));
    QCOMPARE(QString::fromUtf8(readFileRaw(p1)), QStringLiteral("A\n")); // 不回滚，第 1 项已生效
}

void WriteToolsEnhancementTests::multiEditChainedSameFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString p = dir.filePath(QStringLiteral("chain.md"));
    writeFileRaw(p, "a\nb\nc\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, p, "a\nb\nc\n");
    // 同文件链式：第 1 项把 a→A，第 2 项基于结果把 b→B
    const QJsonArray edits{QJsonObject{{QStringLiteral("filePath"), QStringLiteral("chain.md")},
                                       {QStringLiteral("oldString"), QStringLiteral("a")},
                                       {QStringLiteral("newString"), QStringLiteral("A")}},
                           QJsonObject{{QStringLiteral("filePath"), QStringLiteral("chain.md")},
                                       {QStringLiteral("oldString"), QStringLiteral("b")},
                                       {QStringLiteral("newString"), QStringLiteral("B")}}};
    const ToolResult r = runMultiEdit(&runtime, makeMultiEditCall(edits), dir.path());
    QVERIFY(r.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(p)), QStringLiteral("A\nB\nc\n"));
}

void WriteToolsEnhancementTests::multiEditSameFileFailureSkipsRest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString p = dir.filePath(QStringLiteral("skip.md"));
    writeFileRaw(p, "a\nb\nc\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, p, "a\nb\nc\n");
    // 第 1 项失败（oldString 不存在）→ 第 2 项同文件跳过
    const QJsonArray edits{QJsonObject{{QStringLiteral("filePath"), QStringLiteral("skip.md")},
                                       {QStringLiteral("oldString"), QStringLiteral("NOPE")},
                                       {QStringLiteral("newString"), QStringLiteral("X")}},
                           QJsonObject{{QStringLiteral("filePath"), QStringLiteral("skip.md")},
                                       {QStringLiteral("oldString"), QStringLiteral("b")},
                                       {QStringLiteral("newString"), QStringLiteral("B")}}};
    const ToolResult r = runMultiEdit(&runtime, makeMultiEditCall(edits), dir.path());
    QVERIFY(!r.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(p)), QStringLiteral("a\nb\nc\n")); // 文件未被改
}

// ── 防护 ──

void WriteToolsEnhancementTests::directoryPathRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    BuiltinToolRuntime runtime;
    const ToolResult r = runWrite(&runtime, makeWriteCall(dir.path(), QJsonObject{
        {QStringLiteral("content"), QStringLiteral("x")},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("目录")));
}

void WriteToolsEnhancementTests::oversizedContentRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("big.md"));

    BuiltinToolRuntime runtime;
    const QString huge(2 * 1024 * 1024, 'x'); // 2MB > 1MB 上限
    const ToolResult r = runWrite(&runtime, makeWriteCall(path, QJsonObject{
        {QStringLiteral("content"), huge},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("过大")));
}

// ── dry-run + CRLF ──

void WriteToolsEnhancementTests::dryRunDoesNotWrite()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("dry.md"));
    writeFileRaw(path, "a\nb\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "a\nb\n");
    const ToolResult r = runWrite(&runtime, makeWriteCall(path, QJsonObject{
        {QStringLiteral("content"), QStringLiteral("NEW")},
        {QStringLiteral("dryRun"), true},
    }), dir.path());
    QVERIFY(r.success);
    QVERIFY(r.payload.value(QStringLiteral("dryRun")).toBool());
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("a\nb\n")); // 未写入
}

void WriteToolsEnhancementTests::dryRunStillValidatesRead()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("dry2.md"));
    writeFileRaw(path, "a\n");

    BuiltinToolRuntime runtime; // 未 seedReadCache → 先读校验拦截
    const ToolResult r = runWrite(&runtime, makeWriteCall(path, QJsonObject{
        {QStringLiteral("content"), QStringLiteral("NEW")},
        {QStringLiteral("dryRun"), true},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("完整读取")));
}

void WriteToolsEnhancementTests::crlfPreservedOnWrite()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("crlf.md"));
    writeFileRaw(path, "a\r\nb\r\n"); // CRLF 文件

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "a\nb\n");
    const ToolResult r = runWrite(&runtime, makeWriteCall(path, QJsonObject{
        {QStringLiteral("content"), QStringLiteral("a\nb\nc\n")},
    }), dir.path());
    QVERIFY(r.success);
    QCOMPARE(readFileRaw(path), QByteArray("a\r\nb\r\nc\r\n")); // 行尾保持 CRLF
}

// ── 旧行为回归 ──

void WriteToolsEnhancementTests::oldBehaviorUnchanged()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("regress.md"));
    writeFileRaw(path, "a\nb\na\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "a\nb\na\n");
    // 无新参数：oldString 出现多次 → 报错（旧行为）
    const ToolResult r1 = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("a")},
        {QStringLiteral("newString"), QStringLiteral("A")},
    }), dir.path());
    QVERIFY(!r1.success);
    QVERIFY(r1.text.contains(QStringLiteral("multiple times")));

    // replaceAll 正常（旧行为）
    const ToolResult r2 = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("a")},
        {QStringLiteral("newString"), QStringLiteral("A")},
        {QStringLiteral("replaceAll"), true},
    }), dir.path());
    QVERIFY(r2.success);
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("A\nb\nA\n"));
}

// ── G 校验状态机 5 根因回归 ──

// 根因 1：含中文（非 ASCII）的 .cpp edit 不应报「被修改」（normalizeToLf 改 UTF-8 解码）
void WriteToolsEnhancementTests::editNonAsciiFileSucceeds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cn.cpp"));
    writeFileRaw(path, QStringLiteral("// 中文注释\nint x = 1;\n").toUtf8());

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, QStringLiteral("// 中文注释\nint x = 1;\n"));
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("int x = 1;")},
        {QStringLiteral("newString"), QStringLiteral("int x = 2;")},
    }), dir.path());
    QVERIFY2(r.success, qPrintable(r.text));
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("// 中文注释\nint x = 2;\n"));
}

// 根因 2：无尾换行文件编辑新增行 → 不吞换行
void WriteToolsEnhancementTests::restoreLineEndingsNoTrailingNewline()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nonl.cpp"));
    writeFileRaw(path, QStringLiteral("a\nb").toUtf8()); // 无尾换行

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, QStringLiteral("a\nb"));
    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("b")},
        {QStringLiteral("newString"), QStringLiteral("b\nc")},
    }), dir.path());
    QVERIFY2(r.success, qPrintable(r.text));
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("a\nb\nc"));
}

// 根因 3：multi_edit 成功后缓存同步 → 再次写同文件不报「被修改」
void WriteToolsEnhancementTests::multiEditThenWriteSameFileSucceeds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("m.md"));
    writeFileRaw(path, QStringLiteral("a\nb\n").toUtf8());

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, QStringLiteral("a\nb\n"));
    const QJsonArray edits{QJsonObject{{QStringLiteral("filePath"), QStringLiteral("m.md")},
                                       {QStringLiteral("oldString"), QStringLiteral("a")},
                                       {QStringLiteral("newString"), QStringLiteral("A")}}};
    const ToolResult r1 = runMultiEdit(&runtime, makeMultiEditCall(edits), dir.path());
    QVERIFY2(r1.success, qPrintable(r1.text));

    // 紧接着再 edit 同文件：缓存已被 multi_edit 同步 → 应通过
    const ToolResult r2 = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("b")},
        {QStringLiteral("newString"), QStringLiteral("B")},
    }), dir.path());
    QVERIFY2(r2.success, qPrintable(r2.text));
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("A\nB\n"));
}

// 根因 4：offset>0 但读到结尾 → partialView=false（覆盖全文件）
void WriteToolsEnhancementTests::partialViewFullReadToEndIsFalse()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("pv.txt"));
    writeFileRaw(path, QStringLiteral("a\nb\nc\n").toUtf8());

    BuiltinToolRuntime runtime;
    const ToolResult r = runRead(&runtime, makeReadCall(path, QJsonObject{
        {QStringLiteral("offset"), 2},
    }), dir.path());
    QVERIFY(r.success);
    QVERIFY(!r.payload.value(QStringLiteral("partial")).toBool()); // 读到结尾 → false
}

// ── H 校验三档区分 ──

// H1：edit oldString 替换 + 分块读（partialView）→ 放行（oldString 匹配自带证明）
void WriteToolsEnhancementTests::editOldStringWithPartialReadSucceeds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("h1.cpp"));
    writeFileRaw(path, QStringLiteral("line1\nint target = 1;\nline3\nline4\nline5\n").toUtf8());

    BuiltinToolRuntime runtime;
    // 分块读：只读中间一段（partialView=true）
    runRead(&runtime, makeReadCall(path, QJsonObject{
        {QStringLiteral("offset"), 1},
        {QStringLiteral("limit"), 2},
    }), dir.path());

    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("int target = 1;")},
        {QStringLiteral("newString"), QStringLiteral("int target = 2;")},
    }), dir.path());
    QVERIFY2(r.success, qPrintable(r.text)); // 分块读后 oldString edit 放行
}

// H2：edit 行号模式 + 分块读 → 拒「需先完整读取」
void WriteToolsEnhancementTests::editLineModeWithPartialReadRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("h2.cpp"));
    writeFileRaw(path, QStringLiteral("a\nb\nc\n").toUtf8());

    BuiltinToolRuntime runtime;
    runRead(&runtime, makeReadCall(path, QJsonObject{
        {QStringLiteral("offset"), 1},
        {QStringLiteral("limit"), 1},
    }), dir.path());

    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("startLine"), 2},
        {QStringLiteral("newString"), QStringLiteral("B")},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("完整读取")));
}

// H3：write_file 全量覆盖 + 分块读 → 拒「需先完整读取」
void WriteToolsEnhancementTests::writeFileWithPartialReadRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("h3.txt"));
    writeFileRaw(path, QStringLiteral("a\nb\nc\n").toUtf8());

    BuiltinToolRuntime runtime;
    runRead(&runtime, makeReadCall(path, QJsonObject{
        {QStringLiteral("offset"), 1},
        {QStringLiteral("limit"), 1},
    }), dir.path());

    const ToolResult r = runWrite(&runtime, makeWriteCall(path, QJsonObject{
        {QStringLiteral("content"), QStringLiteral("new content")},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("完整读取")));
}

// H4：write_file append + 从未读过 → 放行（append 豁免）
void WriteToolsEnhancementTests::appendWithNoReadSucceeds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("h4.txt"));
    writeFileRaw(path, QStringLiteral("base\n").toUtf8());

    BuiltinToolRuntime runtime; // 未 seedReadCache、未 read → 从未读过
    const ToolResult r = runWrite(&runtime, makeWriteCall(path, QJsonObject{
        {QStringLiteral("content"), QStringLiteral("added\n")},
        {QStringLiteral("append"), true},
    }), dir.path());
    QVERIFY2(r.success, qPrintable(r.text));
    QCOMPARE(QString::fromUtf8(readFileRaw(path)), QStringLiteral("base\nadded\n"));
}

// H5：外部修改（mtime 变 + 内容变）→ 仍拒「被修改」（oldString 模式也拒）
void WriteToolsEnhancementTests::externalModificationStillRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("h5.txt"));
    writeFileRaw(path, QStringLiteral("aaa\nbbb\n").toUtf8());

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, QStringLiteral("aaa\nbbb\n"));
    // 外部修改：改内容 + 改 mtime（sleep 确保 mtime 变化）
    QTest::qWait(1100);
    writeFileRaw(path, QStringLiteral("aaa\nCHANGED\n").toUtf8());

    const ToolResult r = runEdit(&runtime, makeEditCall(path, QJsonObject{
        {QStringLiteral("oldString"), QStringLiteral("bbb")},
        {QStringLiteral("newString"), QStringLiteral("ccc")},
    }), dir.path());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("modified since read")));
}

QTEST_MAIN(WriteToolsEnhancementTests)
#include "WriteToolsEnhancementTests.moc"
