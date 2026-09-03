#include "tools/BuiltinToolRuntime.h"
#include "tools/builtin/GlobTool.h"
#include "tools/builtin/ReadFileTool.h"
#include "tools/builtin/GrepTool.h"
#include "tools/builtin/helpers/FileTypeFilter.h"
#include "tools/builtin/helpers/GlobBraceExpansion.h"
#include "tools/builtin/helpers/WriteGuardHelper.h"
#include "tools/builtin/helpers/TextEncoding.h"
#include "tools/builtin/helpers/WorkspaceHelper.h"

#include <QtTest/QtTest>

#include <QFile>
#include <QTemporaryDir>

/**
 * glob / read_file / grep 能力增强测试：
 * - 纯函数：GlobBraceExpansion / FileTypeFilter / WriteGuardHelper::isBinary / TextEncoding
 * - GlobTool：分页 / type / caseSensitive / absolute / brace / total
 * - ReadFileTool：encoding（GBK 原生）/ 二进制 / 1MB / stubKind
 * - GrepTool：patterns / excludeGlob / totalMatches / skippedFiles / spec 补声明
 */
class BuiltinToolsReadGlobGrepTests : public QObject
{
    Q_OBJECT

private slots:
    // 纯函数
    void braceExpansionBasic();
    void braceExpansionNoBrace();
    void braceExpansionNestedNotRecursive();
    void braceExpansionTooMany();
    void fileTypeFilterMappings();
    void isBinaryDetection();
    void textEncodingGbk();
    // GlobTool
    void globOldBehaviorRegression();
    void globPagination();
    void globTypeFilter();
    void globCaseSensitive();
    void globAbsolute();
    void globBracePattern();
    // ReadFileTool
    void readFileUtf8();
    void readFileGbk();
    void readFileBinaryRejected();
    void readFileOversizedRejected();
    void readFileUnchangedStub();
    void readFileEmptyStub();
    // GrepTool
    void grepTypeInSpec();
    void grepMultiPatterns();
    void grepPatternsMutuallyExclusive();
    void grepExcludeGlob();
    void grepCountTotalMatches();
    void grepSkippedFiles();
};

namespace {

ToolCall makeGlobCall(const QString &pattern, const QJsonObject &extra = {})
{
    ToolCall call;
    call.id = QStringLiteral("call-%1").arg(QDateTime::currentMSecsSinceEpoch());
    call.toolName = QStringLiteral("glob");
    QJsonObject input{{QStringLiteral("pattern"), pattern}};
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        input.insert(it.key(), it.value());
    call.input = input;
    return call;
}

ToolCall makeReadCall(const QString &path, const QJsonObject &extra = {})
{
    ToolCall call;
    call.id = QStringLiteral("call-%1").arg(QDateTime::currentMSecsSinceEpoch());
    call.toolName = QStringLiteral("read_file");
    QJsonObject input{{QStringLiteral("filePath"), path}};
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        input.insert(it.key(), it.value());
    call.input = input;
    return call;
}

ToolCall makeGrepCall(const QJsonObject &input)
{
    ToolCall call;
    call.id = QStringLiteral("call-%1").arg(QDateTime::currentMSecsSinceEpoch());
    call.toolName = QStringLiteral("grep");
    call.input = input;
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

void writeFileRaw(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(bytes);
    f.close();
}

void seedReadCache(BuiltinToolRuntime *runtime, const QString &path, const QString &content)
{
    BuiltinToolRuntime::ReadFileState state;
    state.timestampMs = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    state.content = content;
    runtime->setReadFileState(WorkspaceHelper::normalizedPath(path), state);
}

} // namespace

// ── 纯函数 ──

void BuiltinToolsReadGlobGrepTests::braceExpansionBasic()
{
    const QStringList expanded = GlobBraceExpansion::expand(QStringLiteral("src/**/*.{cpp,h}"));
    QCOMPARE(expanded.size(), 2);
    QVERIFY(expanded.contains(QStringLiteral("src/**/*.cpp")));
    QVERIFY(expanded.contains(QStringLiteral("src/**/*.h")));
}

void BuiltinToolsReadGlobGrepTests::braceExpansionNoBrace()
{
    const QStringList expanded = GlobBraceExpansion::expand(QStringLiteral("src/**/*.cpp"));
    QCOMPARE(expanded.size(), 1);
    QCOMPARE(expanded.first(), QStringLiteral("src/**/*.cpp"));
}

void BuiltinToolsReadGlobGrepTests::braceExpansionNestedNotRecursive()
{
    // 嵌套 {a{b,c}}：只展开最外层 → 产物 {a{b,c}} 保持（不递归）
    const QStringList expanded = GlobBraceExpansion::expand(QStringLiteral("x{a{b,c}}"));
    QVERIFY(!expanded.isEmpty());
    // 行为确定：顶层展开 a{b,c} → 2 个
    QCOMPARE(expanded.size(), 2);
}

void BuiltinToolsReadGlobGrepTests::braceExpansionTooMany()
{
    // 16 个以上分支 → 返回空
    QString pattern = QStringLiteral("{a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t}");
    QVERIFY(GlobBraceExpansion::expand(pattern).isEmpty());
}

void BuiltinToolsReadGlobGrepTests::fileTypeFilterMappings()
{
    QVERIFY(FileTypeFilter::matches(QStringLiteral("x.cpp"), QStringLiteral("cpp")));
    QVERIFY(FileTypeFilter::matches(QStringLiteral("x.hpp"), QStringLiteral("cpp")));
    QVERIFY(FileTypeFilter::matches(QStringLiteral("x.h"), QStringLiteral("cpp")));
    QVERIFY(FileTypeFilter::matches(QStringLiteral("x.tsx"), QStringLiteral("ts")));
    QVERIFY(FileTypeFilter::matches(QStringLiteral("x.unknown"), QStringLiteral("unknown")));
    QVERIFY(!FileTypeFilter::matches(QStringLiteral("x.cpp"), QStringLiteral("py")));
    QVERIFY(FileTypeFilter::matches(QStringLiteral("x.py"), QString())); // 空 type 全过
}

void BuiltinToolsReadGlobGrepTests::isBinaryDetection()
{
    QVERIFY(!WriteGuardHelper::isBinary(QByteArray("plain text\n")));
    QVERIFY(WriteGuardHelper::isBinary(QByteArray("a\x00b", 3)));
    // 控制字节 ≥10%（4/20 = 20%）
    QByteArray control;
    control.append('\x01');
    control.append('\x02');
    control.append('\x03');
    control.append('\x04');
    control.append(QByteArray(16, 'x'));
    QVERIFY(WriteGuardHelper::isBinary(control));
    // 长文本不误伤
    QByteArray longText(9000, 'a');
    QVERIFY(!WriteGuardHelper::isBinary(longText));
}

void BuiltinToolsReadGlobGrepTests::textEncodingGbk()
{
    // "中文" 的 GBK 字节
    const QByteArray gbkBytes("\xD6\xD0\xCE\xC4", 4);
    QString error;
    const QString decoded = TextEncoding::decodeBytes(gbkBytes, QStringLiteral("gbk"), &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(decoded, QStringLiteral("中文"));
}

// ── GlobTool ──

void BuiltinToolsReadGlobGrepTests::globOldBehaviorRegression()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFileRaw(dir.filePath(QStringLiteral("a.cpp")), "x");
    writeFileRaw(dir.filePath(QStringLiteral("b.h")), "x");

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeGlobCall(QStringLiteral("**/*")), dir.path(),
                                 std::make_shared<GlobTool>());
    qDebug() << "GLOB RESULT:" << r.text;
    QVERIFY(r.success);
    // 默认相对路径 + 含 a.cpp/b.h
    QVERIFY(r.text.contains(QStringLiteral("a.cpp")));
    QVERIFY(r.text.contains(QStringLiteral("b.h")));
    QVERIFY(!r.text.contains(dir.path())); // 相对路径
}

void BuiltinToolsReadGlobGrepTests::globPagination()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (int i = 0; i < 5; ++i)
        writeFileRaw(dir.filePath(QStringLiteral("f%1.txt").arg(i)), "x");

    BuiltinToolRuntime runtime;
    const ToolResult page1 = runTool(&runtime, makeGlobCall(QStringLiteral("*.txt"), QJsonObject{
        {QStringLiteral("limit"), 2},
        {QStringLiteral("offset"), 0},
    }), dir.path(), std::make_shared<GlobTool>());
    const ToolResult page2 = runTool(&runtime, makeGlobCall(QStringLiteral("*.txt"), QJsonObject{
        {QStringLiteral("limit"), 2},
        {QStringLiteral("offset"), 2},
    }), dir.path(), std::make_shared<GlobTool>());
    QVERIFY(page1.success);
    QVERIFY(page2.success);
    QCOMPARE(page1.payload.value(QStringLiteral("files")).toArray().size(), 2);
    QCOMPARE(page2.payload.value(QStringLiteral("files")).toArray().size(), 2);
    QCOMPARE(page1.payload.value(QStringLiteral("total")).toInt(), 5);
}

void BuiltinToolsReadGlobGrepTests::globTypeFilter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFileRaw(dir.filePath(QStringLiteral("a.cpp")), "x");
    writeFileRaw(dir.filePath(QStringLiteral("b.md")), "x");

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeGlobCall(QStringLiteral("**/*"), QJsonObject{
        {QStringLiteral("type"), QStringLiteral("cpp")},
    }), dir.path(), std::make_shared<GlobTool>());
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("a.cpp")));
    QVERIFY(!r.text.contains(QStringLiteral("b.md")));
}

void BuiltinToolsReadGlobGrepTests::globCaseSensitive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFileRaw(dir.filePath(QStringLiteral("README.md")), "x");

    BuiltinToolRuntime runtime;
    const ToolResult insensitive = runTool(&runtime, makeGlobCall(QStringLiteral("readme.md")),
                                           dir.path(), std::make_shared<GlobTool>());
    QVERIFY(insensitive.success);
    QVERIFY(insensitive.text.contains(QStringLiteral("README.md"))); // 默认不区分大小写

    const ToolResult sensitive = runTool(&runtime, makeGlobCall(QStringLiteral("readme.md"), QJsonObject{
        {QStringLiteral("caseSensitive"), true},
    }), dir.path(), std::make_shared<GlobTool>());
    QVERIFY(sensitive.success);
    QVERIFY(!sensitive.text.contains(QStringLiteral("README.md"))); // 区分大小写不命中
}

void BuiltinToolsReadGlobGrepTests::globAbsolute()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFileRaw(dir.filePath(QStringLiteral("a.cpp")), "x");

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeGlobCall(QStringLiteral("*.cpp"), QJsonObject{
        {QStringLiteral("absolute"), true},
    }), dir.path(), std::make_shared<GlobTool>());
    QVERIFY(r.success);
    // absPath 用正斜杠 + Windows 路径大小写不敏感，断言用归一化 + 小写
    QVERIFY(r.text.toLower().contains(QDir::fromNativeSeparators(dir.path()).toLower()));
}

void BuiltinToolsReadGlobGrepTests::globBracePattern()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFileRaw(dir.filePath(QStringLiteral("a.cpp")), "x");
    writeFileRaw(dir.filePath(QStringLiteral("b.h")), "x");

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeGlobCall(QStringLiteral("**/*.{cpp,h}")),
                                 dir.path(), std::make_shared<GlobTool>());
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("a.cpp")));
    QVERIFY(r.text.contains(QStringLiteral("b.h")));
}

// ── ReadFileTool ──

void BuiltinToolsReadGlobGrepTests::readFileUtf8()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("u.txt"));
    writeFileRaw(path, QStringLiteral("hello\n世界\n").toUtf8());

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeReadCall(path), dir.path(),
                                 std::make_shared<ReadFileTool>());
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("世界")));
}

void BuiltinToolsReadGlobGrepTests::readFileGbk()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("gbk.txt"));
    writeFileRaw(path, QByteArray("\xD6\xD0\xCE\xC4\x0D\x0A", 6)); // "中文\r\n" GBK

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeReadCall(path, QJsonObject{
        {QStringLiteral("encoding"), QStringLiteral("gbk")},
    }), dir.path(), std::make_shared<ReadFileTool>());
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("中文")));
}

void BuiltinToolsReadGlobGrepTests::readFileBinaryRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bin.dat"));
    writeFileRaw(path, QByteArray("\x00\x01\x02\x03", 4));

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeReadCall(path), dir.path(),
                                 std::make_shared<ReadFileTool>());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("二进制")));
}

void BuiltinToolsReadGlobGrepTests::readFileOversizedRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("big.txt"));
    writeFileRaw(path, QByteArray(2 * 1024 * 1024, 'x'));

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeReadCall(path), dir.path(),
                                 std::make_shared<ReadFileTool>());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("过大")));
}

void BuiltinToolsReadGlobGrepTests::readFileUnchangedStub()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("u.md"));
    writeFileRaw(path, "a\nb\n");

    BuiltinToolRuntime runtime;
    seedReadCache(&runtime, path, "a\nb\n");
    const ToolResult r = runTool(&runtime, makeReadCall(path), dir.path(),
                                 std::make_shared<ReadFileTool>());
    QVERIFY(r.success);
    QCOMPARE(r.payload.value(QStringLiteral("kind")).toString(), QStringLiteral("fileUnchanged"));
    QCOMPARE(r.payload.value(QStringLiteral("stubKind")).toString(), QStringLiteral("fileUnchanged"));
    QVERIFY(r.text.contains(QStringLiteral("未变更")));
}

void BuiltinToolsReadGlobGrepTests::readFileEmptyStub()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("empty.txt"));
    writeFileRaw(path, "");

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeReadCall(path), dir.path(),
                                 std::make_shared<ReadFileTool>());
    QVERIFY(r.success);
    QCOMPARE(r.payload.value(QStringLiteral("stubKind")).toString(), QStringLiteral("empty"));
}

// ── GrepTool ──

void BuiltinToolsReadGlobGrepTests::grepTypeInSpec()
{
    // spec 补声明验证：type/patterns/excludeGlob 必须在 inputSchema 里
    GrepTool tool;
    const ToolSpec spec = tool.spec();
    const QJsonObject schema = spec.inputSchema;
    const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
    QVERIFY(props.contains(QStringLiteral("type")));
    QVERIFY(props.contains(QStringLiteral("patterns")));
    QVERIFY(props.contains(QStringLiteral("excludeGlob")));
}

void BuiltinToolsReadGlobGrepTests::grepMultiPatterns()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFileRaw(dir.filePath(QStringLiteral("a.txt")), "foo\n");
    writeFileRaw(dir.filePath(QStringLiteral("b.txt")), "bar\n");

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeGrepCall(QJsonObject{
        {QStringLiteral("patterns"), QJsonArray{QStringLiteral("foo"), QStringLiteral("bar")}},
    }), dir.path(), std::make_shared<GrepTool>());
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("a.txt")));
    QVERIFY(r.text.contains(QStringLiteral("b.txt")));
}

void BuiltinToolsReadGlobGrepTests::grepPatternsMutuallyExclusive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFileRaw(dir.filePath(QStringLiteral("a.txt")), "foo\n");

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeGrepCall(QJsonObject{
        {QStringLiteral("pattern"), QStringLiteral("foo")},
        {QStringLiteral("patterns"), QJsonArray{QStringLiteral("bar")}},
    }), dir.path(), std::make_shared<GrepTool>());
    QVERIFY(!r.success);
    QVERIFY(r.text.contains(QStringLiteral("互斥")));
}

void BuiltinToolsReadGlobGrepTests::grepExcludeGlob()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFileRaw(dir.filePath(QStringLiteral("a.txt")), "foo\n");
    writeFileRaw(dir.filePath(QStringLiteral("b.md")), "foo\n");

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeGrepCall(QJsonObject{
        {QStringLiteral("pattern"), QStringLiteral("foo")},
        {QStringLiteral("excludeGlob"), QStringLiteral("**/*.md")},
    }), dir.path(), std::make_shared<GrepTool>());
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("a.txt")));
    QVERIFY(!r.text.contains(QStringLiteral("b.md")));
}

void BuiltinToolsReadGlobGrepTests::grepCountTotalMatches()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFileRaw(dir.filePath(QStringLiteral("a.txt")), "foo\nfoo\n");
    writeFileRaw(dir.filePath(QStringLiteral("b.txt")), "foo\n");

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeGrepCall(QJsonObject{
        {QStringLiteral("pattern"), QStringLiteral("foo")},
        {QStringLiteral("mode"), QStringLiteral("count")},
    }), dir.path(), std::make_shared<GrepTool>());
    QVERIFY(r.success);
    QCOMPARE(r.payload.value(QStringLiteral("totalMatches")).toInt(), 3);
}

void BuiltinToolsReadGlobGrepTests::grepSkippedFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    writeFileRaw(dir.filePath(QStringLiteral("small.txt")), "foo\n");
    writeFileRaw(dir.filePath(QStringLiteral("big.txt")), QByteArray(2 * 1024 * 1024, 'x'));

    BuiltinToolRuntime runtime;
    const ToolResult r = runTool(&runtime, makeGrepCall(QJsonObject{
        {QStringLiteral("pattern"), QStringLiteral("foo")},
    }), dir.path(), std::make_shared<GrepTool>());
    QVERIFY(r.success);
    QVERIFY(r.text.contains(QStringLiteral("small.txt")));
    const QJsonArray skipped = r.payload.value(QStringLiteral("skippedFiles")).toArray();
    QCOMPARE(skipped.size(), 1);
    QVERIFY(skipped.first().toString().contains(QStringLiteral("big.txt")));
}

QTEST_MAIN(BuiltinToolsReadGlobGrepTests)
#include "BuiltinToolsReadGlobGrepTests.moc"
