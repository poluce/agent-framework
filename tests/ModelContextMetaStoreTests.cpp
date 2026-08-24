#include "providers/service/ModelContextMetaStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class ModelContextMetaStoreTests final : public QObject
{
    Q_OBJECT

private slots:
    void resolvePriority_userOverCacheOverDefault()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString cachePath = QDir(dir.path()).filePath(QStringLiteral("model_context_cache.json"));
        const QString metaPath = QDir(dir.path()).filePath(QStringLiteral("model_meta.json"));

        ModelContextMetaStore store(cachePath, metaPath);
        store.load();

        QCOMPARE(store.resolveWindow(QStringLiteral("anthropic"),
                                     QStringLiteral("inst-1"),
                                     QStringLiteral("claude-x")),
                 ModelContextMetaStore::kDefaultContextWindow);

        QHash<QString, qint64> windows;
        windows.insert(QStringLiteral("claude-x"), 200000);
        QCOMPARE(store.mergeProviderCache(QStringLiteral("anthropic"), windows), 1);
        QCOMPARE(store.resolve(QStringLiteral("anthropic"),
                               QStringLiteral("inst-1"),
                               QStringLiteral("claude-x")).contextWindowSource,
                 ModelContextMetaStore::Source::Cache);
        QCOMPARE(store.resolveWindow(QStringLiteral("anthropic"),
                                     QStringLiteral("inst-1"),
                                     QStringLiteral("claude-x")),
                 200000);
        QVERIFY(QFile::exists(cachePath));

        QVERIFY(store.setUserOverride(QStringLiteral("inst-1"),
                                      QStringLiteral("claude-x"),
                                      50000));
        const auto user = store.resolve(QStringLiteral("anthropic"),
                                        QStringLiteral("inst-1"),
                                        QStringLiteral("claude-x"));
        QCOMPARE(user.contextWindowSource, ModelContextMetaStore::Source::User);
        QCOMPARE(user.contextWindow, 50000);
        QVERIFY(QFile::exists(metaPath));

        // 其他 instance 仍看 cache
        QCOMPARE(store.resolveWindow(QStringLiteral("anthropic"),
                                     QStringLiteral("inst-2"),
                                     QStringLiteral("claude-x")),
                 200000);

        // 清除用户覆盖回落 cache
        QVERIFY(store.setUserOverride(QStringLiteral("inst-1"),
                                      QStringLiteral("claude-x"),
                                      0));
        QCOMPARE(store.resolve(QStringLiteral("anthropic"),
                               QStringLiteral("inst-1"),
                               QStringLiteral("claude-x")).contextWindowSource,
                 ModelContextMetaStore::Source::Cache);
    }

    void mergeDoesNotTouchUserMeta()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ModelContextMetaStore store(
            QDir(dir.path()).filePath(QStringLiteral("cache.json")),
            QDir(dir.path()).filePath(QStringLiteral("meta.json")));
        store.load();
        QVERIFY(store.setUserOverride(QStringLiteral("inst-merge"),
                                      QStringLiteral("m1"),
                                      11111));

        QHash<QString, qint64> windows;
        windows.insert(QStringLiteral("m1"), 999999);
        store.mergeProviderCache(QStringLiteral("responses"), windows);

        QCOMPARE(store.resolveWindow(QStringLiteral("responses"),
                                     QStringLiteral("inst-merge"),
                                     QStringLiteral("m1")),
                 11111);
    }

    void removeInstanceClearsUserOnly()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ModelContextMetaStore store(
            QDir(dir.path()).filePath(QStringLiteral("cache.json")),
            QDir(dir.path()).filePath(QStringLiteral("meta.json")));
        store.load();

        QHash<QString, qint64> windows;
        windows.insert(QStringLiteral("m-rm"), 180000);
        store.mergeProviderCache(QStringLiteral("deepseek"), windows);
        QVERIFY(store.setUserOverride(QStringLiteral("inst-rm"),
                                      QStringLiteral("m-rm"),
                                      9000));
        store.removeInstance(QStringLiteral("inst-rm"));
        QCOMPARE(store.resolve(QStringLiteral("deepseek"),
                               QStringLiteral("inst-rm"),
                               QStringLiteral("m-rm")).contextWindowSource,
                 ModelContextMetaStore::Source::Cache);
        QCOMPARE(store.resolveWindow(QStringLiteral("deepseek"),
                                     QStringLiteral("inst-rm"),
                                     QStringLiteral("m-rm")),
                 180000);
    }

    void sourceToStringStable()
    {
        QCOMPARE(ModelContextMetaStore::sourceToString(ModelContextMetaStore::Source::User),
                 QStringLiteral("user"));
        QCOMPARE(ModelContextMetaStore::sourceToString(ModelContextMetaStore::Source::Cache),
                 QStringLiteral("cache"));
        QCOMPARE(ModelContextMetaStore::sourceToString(ModelContextMetaStore::Source::Default),
                 QStringLiteral("default"));
    }

    void reloadPersistsAcrossInstances()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString cachePath = QDir(dir.path()).filePath(QStringLiteral("cache.json"));
        const QString metaPath = QDir(dir.path()).filePath(QStringLiteral("meta.json"));

        {
            ModelContextMetaStore store(cachePath, metaPath);
            store.load();
            QHash<QString, qint64> windows;
            windows.insert(QStringLiteral("gpt-x"), 128000);
            store.mergeProviderCache(QStringLiteral("responses"), windows);
            store.setUserOverride(QStringLiteral("i1"), QStringLiteral("gpt-x"), 64000);
        }

        ModelContextMetaStore reloaded(cachePath, metaPath);
        reloaded.load();
        QCOMPARE(reloaded.resolveWindow(QStringLiteral("responses"),
                                        QStringLiteral("i1"),
                                        QStringLiteral("gpt-x")),
                 64000);
        QCOMPARE(reloaded.resolve(QStringLiteral("responses"),
                                  QStringLiteral("i2"),
                                  QStringLiteral("gpt-x")).contextWindowSource,
                 ModelContextMetaStore::Source::Cache);
    }

    void legacyJsonRoundTrip()
    {
        // 老格式 {"contextWindow": N}（无 maxOutputTokens）→ 读入回落默认，写回自动补新键
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString metaPath = QDir(dir.path()).filePath(QStringLiteral("meta.json"));
        {
            QFile file(metaPath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(R"({"version":1,"instances":{"inst-1":{"claude-x":{"contextWindow":128000}}}})");
        }

        ModelContextMetaStore store(
            QDir(dir.path()).filePath(QStringLiteral("cache.json")), metaPath);
        store.load();
        const auto resolved = store.resolve(QStringLiteral("anthropic"),
                                            QStringLiteral("inst-1"),
                                            QStringLiteral("claude-x"));
        QCOMPARE(resolved.contextWindow, qint64(128000));
        QCOMPARE(resolved.contextWindowSource, ModelContextMetaStore::Source::User);
        // 未提供 maxOutputTokens → 回落 provider 默认
        QCOMPARE(resolved.maxOutputTokens, qint64(32768));
        QCOMPARE(resolved.maxOutputSource, ModelContextMetaStore::Source::Default);

        // 写回自动补 maxOutputTokens 键
        store.setUserOverride(QStringLiteral("inst-1"), QStringLiteral("claude-x"), 128000, 32000);
        QFile file(metaPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        QVERIFY(content.contains("maxOutputTokens"));
        QVERIFY(content.contains("contextWindow"));
    }

    void maxOutputResolvePriority()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ModelContextMetaStore store(
            QDir(dir.path()).filePath(QStringLiteral("cache.json")),
            QDir(dir.path()).filePath(QStringLiteral("meta.json")));
        store.load();

        // 未设置：回落 provider 查表（deepseek → 65536）
        QCOMPARE(store.resolveMaxOutputTokens(QStringLiteral("deepseek"),
                                              QStringLiteral("inst-1"),
                                              QStringLiteral("model-x")),
                 qint64(65536));

        // cache 命中 → cache
        QHash<QString, qint64> outputs;
        outputs.insert(QStringLiteral("model-x"), 128000);
        store.mergeProviderCache(QStringLiteral("deepseek"), {}, outputs);
        const auto cached = store.resolve(QStringLiteral("deepseek"),
                                          QStringLiteral("inst-1"),
                                          QStringLiteral("model-x"));
        QCOMPARE(cached.maxOutputTokens, qint64(128000));
        QCOMPARE(cached.maxOutputSource, ModelContextMetaStore::Source::Cache);

        // user 覆盖 > cache
        QVERIFY(store.setUserOverride(QStringLiteral("inst-1"), QStringLiteral("model-x"),
                                      /*contextWindow=*/-1, /*maxOutputTokens=*/32000));
        const auto user = store.resolve(QStringLiteral("deepseek"),
                                        QStringLiteral("inst-1"),
                                        QStringLiteral("model-x"));
        QCOMPARE(user.maxOutputTokens, qint64(32000));
        QCOMPARE(user.maxOutputSource, ModelContextMetaStore::Source::User);

        // 清除 user 覆盖回落 cache
        QVERIFY(store.setUserOverride(QStringLiteral("inst-1"), QStringLiteral("model-x"),
                                      /*contextWindow=*/-1, /*maxOutputTokens=*/0));
        const auto cleared = store.resolve(QStringLiteral("deepseek"),
                                           QStringLiteral("inst-1"),
                                           QStringLiteral("model-x"));
        QCOMPARE(cleared.maxOutputTokens, qint64(128000));
        QCOMPARE(cleared.maxOutputSource, ModelContextMetaStore::Source::Cache);
    }

    void setUserOverrideIndependentKeys()
    {
        // -1=不改该键；两键独立设置/清除
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ModelContextMetaStore store(
            QDir(dir.path()).filePath(QStringLiteral("cache.json")),
            QDir(dir.path()).filePath(QStringLiteral("meta.json")));
        store.load();

        // 只设窗口
        QVERIFY(store.setUserOverride(QStringLiteral("inst-1"), QStringLiteral("m1"),
                                      50000, -1));
        auto r1 = store.resolve(QStringLiteral("anthropic"),
                                QStringLiteral("inst-1"), QStringLiteral("m1"));
        QCOMPARE(r1.contextWindow, qint64(50000));
        QCOMPARE(r1.contextWindowSource, ModelContextMetaStore::Source::User);
        // 窗口 user 时输出仍回落 provider 默认
        QCOMPARE(r1.maxOutputTokens, qint64(32768));
        QCOMPARE(r1.maxOutputSource, ModelContextMetaStore::Source::Default);

        // 只设输出
        QVERIFY(store.setUserOverride(QStringLiteral("inst-1"), QStringLiteral("m1"),
                                      -1, 64000));
        auto r2 = store.resolve(QStringLiteral("anthropic"),
                                QStringLiteral("inst-1"), QStringLiteral("m1"));
        QCOMPARE(r2.contextWindow, qint64(50000)); // 窗口保留
        QCOMPARE(r2.maxOutputTokens, qint64(64000));
        QCOMPARE(r2.maxOutputSource, ModelContextMetaStore::Source::User);

        // 只清输出，窗口不动
        QVERIFY(store.setUserOverride(QStringLiteral("inst-1"), QStringLiteral("m1"),
                                      -1, 0));
        auto r3 = store.resolve(QStringLiteral("anthropic"),
                                QStringLiteral("inst-1"), QStringLiteral("m1"));
        QCOMPARE(r3.contextWindow, qint64(50000));
        QCOMPARE(r3.maxOutputTokens, qint64(32768)); // 回落默认
        QCOMPARE(r3.maxOutputSource, ModelContextMetaStore::Source::Default);
    }

    void mergeProviderCacheSecondArg()
    {
        // 只传 windows 时 maxOutput 表不动；只传 outputs 时 windows 表不动
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ModelContextMetaStore store(
            QDir(dir.path()).filePath(QStringLiteral("cache.json")),
            QDir(dir.path()).filePath(QStringLiteral("meta.json")));
        store.load();

        QHash<QString, qint64> windows;
        windows.insert(QStringLiteral("m1"), 200000);
        store.mergeProviderCache(QStringLiteral("responses"), windows);

        const auto after = store.resolve(QStringLiteral("responses"),
                                         QStringLiteral("inst-1"), QStringLiteral("m1"));
        QCOMPARE(after.contextWindow, qint64(200000));
        // windows-only merge 未动输出 → 回落 provider 默认 65536
        QCOMPARE(after.maxOutputTokens, qint64(65536));
        QCOMPARE(after.maxOutputSource, ModelContextMetaStore::Source::Default);
    }

    void defaultMaxOutputByProvider()
    {
        // resolve default 层按 provider 查表
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ModelContextMetaStore store(
            QDir(dir.path()).filePath(QStringLiteral("cache.json")),
            QDir(dir.path()).filePath(QStringLiteral("meta.json")));
        store.load();

        QCOMPARE(store.resolveMaxOutputTokens(QStringLiteral("anthropic"),
                                              QStringLiteral("i"), QStringLiteral("m")),
                 qint64(32768));
        QCOMPARE(store.resolveMaxOutputTokens(QStringLiteral("chat-completions"),
                                              QStringLiteral("i"), QStringLiteral("m")),
                 qint64(32768));
        QCOMPARE(store.resolveMaxOutputTokens(QStringLiteral("deepseek"),
                                              QStringLiteral("i"), QStringLiteral("m")),
                 qint64(65536));
        QCOMPARE(store.resolveMaxOutputTokens(QStringLiteral("google"),
                                              QStringLiteral("i"), QStringLiteral("m")),
                 qint64(65536));
        QCOMPARE(store.resolveMaxOutputTokens(QStringLiteral("responses"),
                                              QStringLiteral("i"), QStringLiteral("m")),
                 qint64(65536));
        // 未知 provider → kDefaultMaxOutputTokens
        QCOMPARE(store.resolveMaxOutputTokens(QStringLiteral("unknown-provider"),
                                              QStringLiteral("i"), QStringLiteral("m")),
                 ModelContextMetaStore::kDefaultMaxOutputTokens);
    }
};

QTEST_MAIN(ModelContextMetaStoreTests)
#include "ModelContextMetaStoreTests.moc"
