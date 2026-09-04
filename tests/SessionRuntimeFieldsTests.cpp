#include "config/AgentMode.h"
#include "config/SessionRuntime.h"

#include <QJsonObject>
#include <QMetaObject>
#include <QMetaProperty>
#include <QtTest>

/**
 * 0.4 执行配置表冻结：增删改字段须同步本名单与 SessionRuntime.fields.h。
 * 只链公开面，安装包路径也可跑。
 */
class SessionRuntimeFieldsTests final : public QObject
{
    Q_OBJECT

private slots:
    void gadgetProperties_matchFrozenFieldList();
    void jsonRoundTrip_preservesWritableValues();
    void fieldKeys_coverFrozenAndEnumFields();
};

namespace {

const QStringList kFrozenFieldNames = {
    QStringLiteral("providerType"),
    QStringLiteral("modelName"),
    QStringLiteral("contextWindow"),
    QStringLiteral("maxOutputTokens"),
    QStringLiteral("maxOutputTokensSource"),
    QStringLiteral("reasoningEnabled"),
    QStringLiteral("reasoningEffort"),
    QStringLiteral("systemPrompt"),
    QStringLiteral("maxInternalSteps"),
    QStringLiteral("modelResponseTimeoutSecs"),
    QStringLiteral("maxRetries"),
    QStringLiteral("defaultShell"),
    QStringLiteral("maxInboxMessages"),
    QStringLiteral("maxInboxMessageSize"),
    QStringLiteral("compactEnabled"),
    QStringLiteral("compactTriggerTokens"),
    QStringLiteral("compactReserveTokens"),
    QStringLiteral("compactTargetTokens"),
    QStringLiteral("compactMaxRetries"),
    QStringLiteral("compactUserMessageTokenBudget"),
    QStringLiteral("compactMaxOutputTokens"),
    QStringLiteral("summaryEnabled"),
    QStringLiteral("summarySegmentTokens"),
    QStringLiteral("summaryRecentTurns"),
    QStringLiteral("workingDirectory"),
    QStringLiteral("credentialInstanceId"),
};

QStringList gadgetPropertyNames()
{
    QStringList names;
    const QMetaObject *meta = &SessionRuntime::staticMetaObject;
    for (int i = 0; i < meta->propertyCount(); ++i) {
        const QMetaProperty prop = meta->property(i);
        if (prop.isValid())
            names.append(QString::fromUtf8(prop.name()));
    }
    names.sort();
    return names;
}

} // namespace

void SessionRuntimeFieldsTests::gadgetProperties_matchFrozenFieldList()
{
    QStringList frozen = kFrozenFieldNames;
    frozen.sort();
    QCOMPARE(gadgetPropertyNames(), frozen);
}

void SessionRuntimeFieldsTests::jsonRoundTrip_preservesWritableValues()
{
    SessionRuntime src;
    src.modelName = QStringLiteral("freeze-model");
    src.workingDirectory = QStringLiteral("D:/tmp/fw");
    src.maxRetries = 3;
    src.compactEnabled = false;
    src.systemPrompt = QStringLiteral("minimal");

    const SessionRuntime dst = SessionRuntime::fromJson(src.toJson());
    QCOMPARE(dst.modelName, src.modelName);
    QCOMPARE(dst.workingDirectory, src.workingDirectory);
    QCOMPARE(dst.maxRetries, src.maxRetries);
    QCOMPARE(dst.compactEnabled, src.compactEnabled);
    QCOMPARE(dst.systemPrompt, src.systemPrompt);
    // 枚举字段无 Q_PROPERTY，走成员 + toJson 字符串键
    src.agentMode = AgentMode::Planning;
    src.toolScope = ToolScope::ReadOnly;
    src.approvalMode = ApprovalMode::Auto;
    const SessionRuntime dst2 = SessionRuntime::fromJson(src.toJson());
    QCOMPARE(dst2.agentMode, AgentMode::Planning);
    QCOMPARE(dst2.toolScope, ToolScope::ReadOnly);
    QCOMPARE(dst2.approvalMode, ApprovalMode::Auto);
}

void SessionRuntimeFieldsTests::fieldKeys_coverFrozenAndEnumFields()
{
    const QStringList keys = SessionRuntime::fieldKeys();

    // 覆盖全部 Q_PROPERTY 字段（冻结名单）
    for (const QString &name : kFrozenFieldNames) {
        QVERIFY2(keys.contains(name),
                 qPrintable(QStringLiteral("fieldKeys 缺少字段: %1").arg(name)));
    }
    // 覆盖枚举字段（无 Q_PROPERTY，但 toJson 有键）
    QVERIFY(keys.contains(QStringLiteral("agentMode")));
    QVERIFY(keys.contains(QStringLiteral("toolScope")));
    QVERIFY(keys.contains(QStringLiteral("approvalMode")));
    // 与 toJson 键一致（字段集指纹用途：宿主白名单自动比对）
    const SessionRuntime rt;
    const QStringList jsonKeys = rt.toJson().keys();
    QCOMPARE(keys.size(), jsonKeys.size());
    for (const QString &key : jsonKeys) {
        QVERIFY2(keys.contains(key),
                 qPrintable(QStringLiteral("fieldKeys 缺少 toJson 键: %1").arg(key)));
    }
}

QTEST_MAIN(SessionRuntimeFieldsTests)
#include "SessionRuntimeFieldsTests.moc"
