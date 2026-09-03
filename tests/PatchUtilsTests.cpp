#include "tools/builtin/helpers/PatchUtils.h"

#include <QtTest/QtTest>

#include <QElapsedTimer>

/**
 * 真 diff（LCS + hunk）纯函数测试：
 * - 增行/删行/改行块头断言
 * - 多 hunk 不合并 / 相邻 hunk 合并
 * - context 行前缀
 * - 空文件 / 完全相同 / 尾部换行变化
 * - CRLF 输入与 LF 同结果（内部归一验证）
 * - 大文件退化（10 万行 < 5s）
 * - 整文件重排退化
 */
class PatchUtilsTests : public QObject
{
    Q_OBJECT

private slots:
    void insertLine();
    void deleteLine();
    void replaceLine();
    void multipleHunksNotMerged();
    void adjacentHunksMerged();
    void contextLinesPresent();
    void emptyFile();
    void identicalContent();
    void trailingNewlineChange();
    void crlfEquivalentToLf();
    void hugeFileDegrades();
    void fullReorderDegrades();
};

namespace {

QJsonArray hunkLines(const QJsonArray &patch, int hunkIndex)
{
    return patch.at(hunkIndex).toObject().value(QStringLiteral("lines")).toArray();
}

QStringList hunkLineStrings(const QJsonArray &patch, int hunkIndex)
{
    QStringList out;
    const QJsonArray lines = hunkLines(patch, hunkIndex);
    for (const QJsonValue &v : lines)
        out.append(v.toString());
    return out;
}

// 生成 N 行连续内容（"line1\nline2\n…"），可替换某一行
QString numberedLines(int count, int changedIndex = -1, const QString &changedText = {})
{
    QString out;
    out.reserve(count * 8);
    for (int i = 1; i <= count; ++i) {
        const QString text = (i == changedIndex) ? changedText : QStringLiteral("line%1").arg(i);
        out += text + QLatin1Char('\n');
    }
    return out;
}

} // namespace

void PatchUtilsTests::insertLine()
{
    const QJsonArray patch = buildStructuredPatch(QStringLiteral("a\nb\n"), QStringLiteral("a\nb\nc\n"));
    QCOMPARE(patch.size(), 1);
    const QJsonObject hunk = patch.first().toObject();
    // 末尾追加：hunk 覆盖旧侧 2 行 + 新侧 3 行（含 context）；新内容行 3 处插入
    QVERIFY(hunk.value(QStringLiteral("oldStart")).toInt() >= 1);
    QVERIFY(hunk.value(QStringLiteral("newStart")).toInt() >= 1);
    QVERIFY(hunkLineStrings(patch, 0).contains(QStringLiteral("+c")));
}

void PatchUtilsTests::deleteLine()
{
    const QJsonArray patch = buildStructuredPatch(QStringLiteral("a\nb\nc\n"), QStringLiteral("a\nc\n"));
    QCOMPARE(patch.size(), 1);
    QVERIFY(hunkLineStrings(patch, 0).contains(QStringLiteral("-b")));
}

void PatchUtilsTests::replaceLine()
{
    const QJsonArray patch = buildStructuredPatch(QStringLiteral("a\nb\nc\n"), QStringLiteral("a\nB\nc\n"));
    QCOMPARE(patch.size(), 1);
    const QStringList lines = hunkLineStrings(patch, 0);
    QVERIFY(lines.contains(QStringLiteral("-b")));
    QVERIFY(lines.contains(QStringLiteral("+B")));
}

void PatchUtilsTests::multipleHunksNotMerged()
{
    // 两处改动相隔 > 6 行（context 3 内不合并）
    const QString oldContent = numberedLines(30, 2, QStringLiteral("line2-CHANGED"));
    const QString newContent = numberedLines(30, 25, QStringLiteral("line25-CHANGED"));
    const QJsonArray patch = buildStructuredPatch(oldContent, newContent);
    QVERIFY(patch.size() >= 2);
}

void PatchUtilsTests::adjacentHunksMerged()
{
    // 两处改动相隔 2 行（≤ 6）→ 合并为 1 个 hunk
    const QJsonArray patch = buildStructuredPatch(
        QStringLiteral("a\nb\nc\nd\ne\nf\ng\nh\n"),
        QStringLiteral("a\nX\nc\nd\nY\nf\ng\nh\n"));
    QCOMPARE(patch.size(), 1);
}

void PatchUtilsTests::contextLinesPresent()
{
    // 改动在文件中部且前后有 >context 行 → 有 ' ' 前缀 context 行
    const QJsonArray patch = buildStructuredPatch(
        QStringLiteral("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\n"),
        QStringLiteral("a\nb\nc\nd\nE\nf\ng\nh\ni\nj\nk\nl\nm\nn\n"));
    QCOMPARE(patch.size(), 1);
    const QStringList lines = hunkLineStrings(patch, 0);
    const int contextCount = std::count_if(lines.cbegin(), lines.cend(),
                                           [](const QString &s) { return s.startsWith(QLatin1Char(' ')); });
    QVERIFY(contextCount >= 3);
    QVERIFY(lines.contains(QStringLiteral("-e")));
    QVERIFY(lines.contains(QStringLiteral("+E")));
}

void PatchUtilsTests::emptyFile()
{
    const QJsonArray patch = buildStructuredPatch(QString(), QStringLiteral("x\n"));
    QCOMPARE(patch.size(), 1);
    QCOMPARE(patch.first().toObject().value(QStringLiteral("oldStart")).toInt(), 1);
    QVERIFY(hunkLineStrings(patch, 0).contains(QStringLiteral("+x")));
}

void PatchUtilsTests::identicalContent()
{
    QCOMPARE(buildStructuredPatch(QStringLiteral("a\nb\n"), QStringLiteral("a\nb\n")), QJsonArray{});
}

void PatchUtilsTests::trailingNewlineChange()
{
    const QJsonArray patch = buildStructuredPatch(QStringLiteral("a"), QStringLiteral("a\n"));
    QCOMPARE(patch.size(), 1);
    QVERIFY(hunkLineStrings(patch, 0).contains(QStringLiteral("+")));
}

void PatchUtilsTests::crlfEquivalentToLf()
{
    // CRLF 输入（模拟 Text 读入后含 \r\n）与 LF 版同结果
    const QJsonArray crlfPatch = buildStructuredPatch(
        QStringLiteral("a\r\nb\r\nc\r\n"), QStringLiteral("a\r\nB\r\nc\r\n"));
    const QJsonArray lfPatch = buildStructuredPatch(
        QStringLiteral("a\nb\nc\n"), QStringLiteral("a\nB\nc\n"));
    QCOMPARE(crlfPatch.size(), lfPatch.size());
    QCOMPARE(crlfPatch, lfPatch);
}

void PatchUtilsTests::hugeFileDegrades()
{
    // 10 万行全不同 → 退化单块，< 5s
    QString oldContent, newContent;
    oldContent.reserve(900000);
    newContent.reserve(900000);
    for (int i = 0; i < 100000; ++i) {
        oldContent += QStringLiteral("old%1\n").arg(i);
        newContent += QStringLiteral("new%1\n").arg(i);
    }
    QElapsedTimer t;
    t.start();
    const QJsonArray patch = buildStructuredPatch(oldContent, newContent);
    QVERIFY(t.elapsed() < 5000);
    QVERIFY(patch.size() >= 1);
    QCOMPARE(patch.first().toObject().value(QStringLiteral("oldStart")).toInt(), 1);
}

void PatchUtilsTests::fullReorderDegrades()
{
    // 整文件顺序颠倒 → D 超预算退化单块（合法块）
    const QJsonArray patch = buildStructuredPatch(
        QStringLiteral("a\nb\nc\nd\ne\nf\ng\nh\n"),
        QStringLiteral("h\ng\nf\ne\nd\nc\nb\na\n"));
    QVERIFY(patch.size() >= 1);
}

QTEST_APPLESS_MAIN(PatchUtilsTests)
#include "PatchUtilsTests.moc"
