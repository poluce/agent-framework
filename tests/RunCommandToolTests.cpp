#include "tools/builtin/RunCommandTool.h"

#include <QJsonObject>
#include <QtTest/QtTest>

/**
 * run_command：PowerShell 二次嵌套解包与 spec 防误用提示。
 */
class RunCommandToolTests : public QObject
{
    Q_OBJECT

private slots:
    void unwrap_doubleQuotedCommand();
    void unwrap_singleQuotedCommand();
    void unwrap_leadingFlags();
    void unwrap_shortDashC();
    void unwrap_colonAttachedCommand();
    void unwrap_caseInsensitive();
    void unwrap_recursive();
    void unwrap_plainCommandUnchanged();
    void unwrap_fileInvocationUnchanged();
    void unwrap_encodedCommandUnchanged();
    void unwrap_nonPowerShellUnchanged();
    void spec_commandWarnsAgainstNestedWrapper();
    void resolveShellCommand_stripsNestedWrapper();
    void resolveShellCommand_bashKeepsNestedPwsh();
};

void RunCommandToolTests::unwrap_doubleQuotedCommand()
{
    const QString input =
        QStringLiteral("pwsh -Command \"foreach ($d in $dirs) { $d }\"");
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(input),
             QStringLiteral("foreach ($d in $dirs) { $d }"));
}

void RunCommandToolTests::unwrap_singleQuotedCommand()
{
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(
                 QStringLiteral("powershell -Command 'Get-Date'")),
             QStringLiteral("Get-Date"));
}

void RunCommandToolTests::unwrap_leadingFlags()
{
    const QString input =
        QStringLiteral("pwsh.exe -NoProfile -ExecutionPolicy Bypass -Command \"Get-ChildItem\"");
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(input),
             QStringLiteral("Get-ChildItem"));
}

void RunCommandToolTests::unwrap_shortDashC()
{
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(
                 QStringLiteral("pwsh -c Get-Date")),
             QStringLiteral("Get-Date"));
}

void RunCommandToolTests::unwrap_colonAttachedCommand()
{
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(
                 QStringLiteral("pwsh -Command:\"Get-Date\"")),
             QStringLiteral("Get-Date"));
}

void RunCommandToolTests::unwrap_caseInsensitive()
{
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(
                 QStringLiteral("PWSH -command \"Get-Date\"")),
             QStringLiteral("Get-Date"));
}

void RunCommandToolTests::unwrap_recursive()
{
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(
                 QStringLiteral("pwsh -Command \"pwsh -Command Get-Date\"")),
             QStringLiteral("Get-Date"));
}

void RunCommandToolTests::unwrap_plainCommandUnchanged()
{
    const QString input = QStringLiteral("Get-ChildItem -Recurse");
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(input), input);
}

void RunCommandToolTests::unwrap_fileInvocationUnchanged()
{
    const QString input = QStringLiteral("pwsh -File script.ps1");
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(input), input);
}

void RunCommandToolTests::unwrap_encodedCommandUnchanged()
{
    const QString input = QStringLiteral("pwsh -EncodedCommand ABC");
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(input), input);
}

void RunCommandToolTests::unwrap_nonPowerShellUnchanged()
{
    const QString input = QStringLiteral("bash -lc echo hi");
    QCOMPARE(RunCommandTool::unwrapNestedPowerShellCommand(input), input);
}

void RunCommandToolTests::spec_commandWarnsAgainstNestedWrapper()
{
    RunCommandTool tool;
    const QJsonObject command = tool.spec()
                                    .inputSchema.value(QStringLiteral("properties")).toObject()
                                    .value(QStringLiteral("command")).toObject();
    const QString desc = command.value(QStringLiteral("description")).toString();
    QVERIFY(desc.contains(QStringLiteral("pwsh -Command")));
    QVERIFY(desc.contains(QStringLiteral("powershell -Command")));
}

void RunCommandToolTests::resolveShellCommand_stripsNestedWrapper()
{
    auto cfg = RunCommandTool::resolveShellCommand(
        QStringLiteral("pwsh"), QStringLiteral("pwsh"),
        QStringLiteral("pwsh -Command \"Get-Date\""));
    if (!cfg.error.isEmpty()) {
        cfg = RunCommandTool::resolveShellCommand(
            QStringLiteral("powershell"), QStringLiteral("powershell"),
            QStringLiteral("powershell -Command \"Get-Date\""));
        if (!cfg.error.isEmpty()) {
            QSKIP("pwsh/powershell 未安装");
        }
    }
    QVERIFY(!cfg.arguments.isEmpty());
    const QString body = cfg.arguments.last();
    QVERIFY(body.contains(QStringLiteral("Get-Date")));
    QVERIFY(!body.contains(QStringLiteral("pwsh -Command"), Qt::CaseInsensitive));
    QVERIFY(!body.contains(QStringLiteral("powershell -Command"), Qt::CaseInsensitive));
}

void RunCommandToolTests::resolveShellCommand_bashKeepsNestedPwsh()
{
    const auto cfg = RunCommandTool::resolveShellCommand(
        QStringLiteral("bash"), QStringLiteral("bash"),
        QStringLiteral("pwsh -Command Get-Date"));
    if (!cfg.error.isEmpty()) {
        QSKIP("bash 未安装");
    }
    QVERIFY(!cfg.arguments.isEmpty());
    QCOMPARE(cfg.arguments.last(), QStringLiteral("pwsh -Command Get-Date"));
}

QTEST_APPLESS_MAIN(RunCommandToolTests)
#include "RunCommandToolTests.moc"
