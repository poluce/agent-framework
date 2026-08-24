#include "logging/LogManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

/**
 * LogManager 目录由调用方注入：空 init / 未设请求体目录不得回落本产品 AppData。
 */
class LogManagerInjectionTests final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        LogManager::instance().setConsoleOutputEnabled(false);
    }

    void cleanup()
    {
        auto &logs = LogManager::instance();
        logs.shutdown();
        logs.setRequestBodiesDirectory(QString());
    }

    void initEmpty_doesNotFallbackToAppData()
    {
        auto &logs = LogManager::instance();
        logs.shutdown();

        const QString appDataLogs = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                    + QStringLiteral("/core/logs");
        const QString dateKey = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd"));
        const QString productFile = appDataLogs + QStringLiteral("/core_") + dateKey
                                    + QStringLiteral("_")
                                    + QString::number(QCoreApplication::applicationPid())
                                    + QStringLiteral(".log");
        const bool productFileExisted = QFile::exists(productFile);

        logs.init(QString());
        QVERIFY(logs.logFilePath().isEmpty());
        if (!productFileExisted)
            QVERIFY(!QFile::exists(productFile));
    }

    void initExplicitDir_writesThere()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        auto &logs = LogManager::instance();
        logs.shutdown();
        logs.init(tmp.path());

        const QString path = logs.logFilePath();
        QVERIFY(!path.isEmpty());
        QVERIFY(path.startsWith(tmp.path()));
        QVERIFY(QFile::exists(path));
    }

    void saveRequestBody_noopsUntilDirectoryInjected()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        auto &logs = LogManager::instance();
        logs.shutdown();
        logs.setRequestBodiesDirectory(QString());

        AgentLogContext ctx;
        ctx.sessionUuid = QStringLiteral("sess-log-inject");
        ctx.agentId = QStringLiteral("agent-0");
        ctx.agentType = QStringLiteral("main");
        ctx.sessionShortId = QStringLiteral("sesslog");

        const QByteArray body = QByteArrayLiteral("{\"messages\":[]}");
        logs.saveRequestBody(QStringLiteral("chat-completions"), body, ctx);
        QVERIFY(!QDir(tmp.path() + QStringLiteral("/sess-log-inject")).exists());

        logs.setRequestBodiesDirectory(tmp.path());
        QCOMPARE(logs.requestBodiesDirectory(), tmp.path());
        logs.saveRequestBody(QStringLiteral("chat-completions"), body, ctx);

        const QString written = tmp.path()
            + QStringLiteral("/sess-log-inject/main/request.json");
        QVERIFY(QFile::exists(written));
    }
};

QTEST_MAIN(LogManagerInjectionTests)
#include "LogManagerInjectionTests.moc"
