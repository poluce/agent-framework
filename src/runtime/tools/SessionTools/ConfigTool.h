#pragma once

#include "agent/Agent.h"
#include "agent/AgentSession.h"
#include "config/SystemPromptBuilder.h"
#include <QJsonDocument>

#include "tools/AbstractSessionTool.h"

class ConfigTool : public AbstractSessionTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    ToolResult execute(Agent *caller,
                       const ToolCall &call,
                       const QString &workingDirectory) override;
};

inline ToolSpec ConfigTool::spec() const
{
    return ToolSpecBuilder("config", QStringLiteral("读取或更新当前会话配置。"))
        .requiredInput("setting", "string", QStringLiteral("配置键"))
        .input("value", "string", QStringLiteral("配置值，可为空；为空表示读取"))
        .output("operation", "string", "get/set")
        .output("setting", "string", QStringLiteral("配置项名称"))
        .output("value", "string", QStringLiteral("当前值"))
        .output("previousValue", "string", QStringLiteral("旧值"))
        .build();
}

inline ToolResult ConfigTool::execute(Agent *caller, const ToolCall &call, const QString &workingDirectory)
{
    Q_UNUSED(workingDirectory);

    const QString setting = call.input.value(QStringLiteral("setting")).toString().trimmed();
    const QJsonValue value = call.input.value(QStringLiteral("value"));
    if (setting.isEmpty()) {
        return makeError(call, QStringLiteral("Config 缺少 setting。"));
    }

    AgentSession *session = qobject_cast<AgentSession*>(caller->parent());
    if (!session) {
        return makeError(call, QStringLiteral("会话状态不存在"));
    }

    auto resolveKey = [](const QString &key) -> QString {
        return (key == QStringLiteral("model")) ? QStringLiteral("modelName") : key;
    };

    auto getValue = [session, &resolveKey](const QString &key) -> QJsonValue {
        if (key == QStringLiteral("systemPrompt")) {
            return session->promptBuilder()
                ? session->promptBuilder()->userCustomPrompt()
                : QString();
        }
        const QString propertyName = resolveKey(key);
        const QJsonObject json = session->runtime().toJson();
        if (!json.contains(propertyName)) {
            return QJsonValue(QJsonValue::Undefined);
        }
        return json.value(propertyName);
    };

    const QJsonValue previousValue = getValue(setting);
    if (previousValue.isUndefined()) {
        return makeError(call, QStringLiteral("不支持的配置项：%1").arg(setting));
    }

    ToolResult result;
    if (!call.input.contains(QStringLiteral("value"))) {
        result.success = true;
        result.text = QStringLiteral("%1=%2").arg(setting, previousValue.toVariant().toString());
        result.payloadType = QStringLiteral("configResult");
        result.payload = QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("get")},
            {QStringLiteral("setting"), setting},
            {QStringLiteral("value"), previousValue},
            {QStringLiteral("previousValue"), previousValue},
        };
        return result;
    }

    const QString valueText = value.isString()
                                  ? value.toString()
                                  : QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);

    if (setting == QStringLiteral("systemPrompt")) {
        if (session->promptBuilder()) {
            session->promptBuilder()->setUserCustomPrompt(valueText);
        }
    } else if (setting == QStringLiteral("workingDirectory")) {
        session->setSessionWorkingDirectory(valueText);
    } else {
        // getValue 已确认键存在；setRuntimeField 失败仅表示规范化后无变化（幂等）。
        session->setRuntimeField(resolveKey(setting), value.toVariant());
    }

    result.success = true;
    result.text = QStringLiteral("已更新配置 %1").arg(setting);
    result.payloadType = QStringLiteral("configResult");
    result.payload = QJsonObject{
        {QStringLiteral("operation"), QStringLiteral("set")},
        {QStringLiteral("setting"), setting},
        {QStringLiteral("value"), getValue(setting)},
        {QStringLiteral("previousValue"), previousValue},
    };
    return result;
}
