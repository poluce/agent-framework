#include "SessionRuntime.h"
#include "AgentMode.h"

#include <QDir>
#include <QMetaObject>
#include <QMetaProperty>

namespace {

QJsonValue variantToJsonValue(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Bool:    return QJsonValue(v.toBool());
    case QMetaType::Int:     return QJsonValue(v.toInt());
    case QMetaType::LongLong: return QJsonValue(v.toLongLong());
    default:                 return QJsonValue(v.toString());
    }
}

QVariant jsonValueToVariant(const QJsonValue &v, int typeId)
{
    switch (typeId) {
    case QMetaType::Bool:    return QVariant(v.toBool(false));
    case QMetaType::Int:     return QVariant(v.toInt(0));
    case QMetaType::LongLong: return QVariant(static_cast<qlonglong>(v.toDouble(0.0)));
    default:                 return QVariant(v.toString());
    }
}

bool setPolicyEnumField(SessionRuntime *runtime, const QString &name, const QString &raw)
{
    if (name == QStringLiteral("agentMode")) {
        runtime->agentMode = parseAgentMode(raw);
        return true;
    }
    if (name == QStringLiteral("toolScope")) {
        runtime->toolScope = parseToolScope(raw);
        return true;
    }
    if (name == QStringLiteral("approvalMode")) {
        // 落盘/配置键只保留 ask|auto；bypass* 收成 ask（权限别名只在 parseApprovalMode）。
        runtime->approvalMode = parseApprovalMode(SessionRuntime::normalizeApprovalMode(raw));
        return true;
    }
    return false;
}

void writePolicyEnumFields(QJsonObject *obj, const SessionRuntime &runtime)
{
    obj->insert(QStringLiteral("agentMode"), agentModeToString(runtime.agentMode));
    obj->insert(QStringLiteral("toolScope"), toolScopeToString(runtime.toolScope));
    obj->insert(QStringLiteral("approvalMode"), approvalModeToString(runtime.approvalMode));
}

void readPolicyEnumFields(SessionRuntime *runtime, const QJsonObject &obj)
{
    if (obj.contains(QStringLiteral("agentMode")))
        runtime->agentMode = parseAgentMode(obj.value(QStringLiteral("agentMode")).toString());
    if (obj.contains(QStringLiteral("toolScope")))
        runtime->toolScope = parseToolScope(obj.value(QStringLiteral("toolScope")).toString());
    if (obj.contains(QStringLiteral("approvalMode"))) {
        runtime->approvalMode = parseApprovalMode(
            SessionRuntime::normalizeApprovalMode(obj.value(QStringLiteral("approvalMode")).toString()));
    }
}

} // namespace

QString SessionRuntime::normalizeWorkingDirectory(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return QDir::currentPath();
    return QDir::cleanPath(QDir::isAbsolutePath(trimmed)
                               ? trimmed
                               : QDir::current().absoluteFilePath(trimmed));
}

QString SessionRuntime::normalizeAgentMode(const QString &value)
{
    return agentModeToString(parseAgentMode(value));
}

QString SessionRuntime::normalizeToolScope(const QString &value)
{
    return toolScopeToString(parseToolScope(value));
}

QString SessionRuntime::normalizeApprovalMode(const QString &value)
{
    return value.trimmed().compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("auto")
        : QStringLiteral("ask");
}

void SessionRuntime::normalizeFieldInPlace(const QString &name)
{
    if (name == QStringLiteral("workingDirectory"))
        workingDirectory = normalizeWorkingDirectory(workingDirectory);
    else if (name == QStringLiteral("providerType"))
        providerType = providerType.trimmed().toLower();
    else if (name == QStringLiteral("credentialInstanceId"))
        credentialInstanceId = credentialInstanceId.trimmed();
    else if (name == QStringLiteral("modelName"))
        modelName = modelName.trimmed();
    else if (name == QStringLiteral("reasoningEffort"))
        reasoningEffort = reasoningEffort.trimmed();
}

void SessionRuntime::normalizeInPlace()
{
    normalizeFieldInPlace(QStringLiteral("workingDirectory"));
    normalizeFieldInPlace(QStringLiteral("providerType"));
    normalizeFieldInPlace(QStringLiteral("credentialInstanceId"));
    normalizeFieldInPlace(QStringLiteral("modelName"));
    normalizeFieldInPlace(QStringLiteral("reasoningEffort"));
}

QJsonObject SessionRuntime::toJson() const
{
    QJsonObject obj;
    const QMetaObject *meta = &SessionRuntime::staticMetaObject;
    for (int i = 0; i < meta->propertyCount(); ++i) {
        QMetaProperty prop = meta->property(i);
        QVariant v = prop.readOnGadget(this);
        obj.insert(QString::fromUtf8(prop.name()), variantToJsonValue(v));
    }
    writePolicyEnumFields(&obj, *this);
    return obj;
}

SessionRuntime SessionRuntime::fromJson(const QJsonObject &obj)
{
    SessionRuntime sr;
    const QMetaObject *meta = &SessionRuntime::staticMetaObject;
    for (int i = 0; i < meta->propertyCount(); ++i) {
        QMetaProperty prop = meta->property(i);
        const QString key = QString::fromUtf8(prop.name());
        if (obj.contains(key))
            prop.writeOnGadget(&sr, jsonValueToVariant(obj.value(key), prop.metaType().id()));
    }
    readPolicyEnumFields(&sr, obj);
    return sr;
}

bool SessionRuntime::setField(const QString &name, const QVariant &value)
{
    if (setPolicyEnumField(this, name, value.toString()))
        return true;
    const QMetaObject *meta = &SessionRuntime::staticMetaObject;
    int idx = meta->indexOfProperty(name.toUtf8().constData());
    if (idx < 0) return false;
    QMetaProperty prop = meta->property(idx);
    if (!prop.isWritable()) return false;
    return prop.writeOnGadget(this, value);
}

bool SessionRuntime::setFieldNormalized(const QString &name, const QVariant &value)
{
    if (!setField(name, value))
        return false;
    // 仅对刚写入的字段做语义规范化，避免无关字段被改写
    normalizeFieldInPlace(name);
    return true;
}

CompactConfig SessionRuntime::toCompactConfig() const
{
    // 只映射执行参数；触发阈值 / 开关由 AbstractLoop 读 SessionRuntime
    CompactConfig cc;
    cc.targetTokenCount = compactTargetTokens;
    cc.maxRetries = compactMaxRetries;
    cc.userMessageTokenBudget = compactUserMessageTokenBudget;
    cc.maxOutputTokens = compactMaxOutputTokens;
    return cc;
}
