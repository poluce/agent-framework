#include "tools/BuiltinTools/RunCommandTool.h"

#include <QtTest/QtTest>

/**
 * ANSI 转义剥除纯函数测试：
 * - CSI 颜色码（[32;1m 表头）
 * - OSC 超链接（]8;;url BEL）
 * - 私用序列（[?1049h 整屏切换）
 * - 无 ANSI 原样
 * - 中文/换行不受影响
 */
class AnsiStripTests : public QObject
{
    Q_OBJECT

private slots:
    void stripsCsiColorCodes();
    void stripsOscHyperlink();
    void stripsPrivateModeSequence();
    void leavesPlainTextUntouched();
    void preservesChineseAndNewlines();
};

void AnsiStripTests::stripsCsiColorCodes()
{
    // PowerShell 表头：绿色粗体 "Path" + 重置
    const QString input = QStringLiteral("\x1B[32;1mPath\x1B[0m");
    QCOMPARE(RunCommandTool::stripAnsi(input), QStringLiteral("Path"));
}

void AnsiStripTests::stripsOscHyperlink()
{
    const QString input = QStringLiteral("\x1B]8;;https://example.com\x07text\x1B]8;;\x07");
    QCOMPARE(RunCommandTool::stripAnsi(input), QStringLiteral("text"));
}

void AnsiStripTests::stripsPrivateModeSequence()
{
    const QString input = QStringLiteral("\x1B[?1049hhello\x1B[?1049l");
    QCOMPARE(RunCommandTool::stripAnsi(input), QStringLiteral("hello"));
}

void AnsiStripTests::leavesPlainTextUntouched()
{
    const QString input = QStringLiteral("D:\\Document");
    QCOMPARE(RunCommandTool::stripAnsi(input), input);
}

void AnsiStripTests::preservesChineseAndNewlines()
{
    const QString input = QStringLiteral("第一行\n\x1B[36m第二行\x1B[0m\n第三行");
    QCOMPARE(RunCommandTool::stripAnsi(input), QStringLiteral("第一行\n第二行\n第三行"));
}

QTEST_APPLESS_MAIN(AnsiStripTests)
#include "AnsiStripTests.moc"
