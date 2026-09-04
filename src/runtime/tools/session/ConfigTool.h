#pragma once

#include <QJsonDocument>
#include <QSet>

#include "tools/AbstractSessionTool.h"
#include "tools/SessionToolContext.h"

namespace {

/// agent 可自调的配置键白名单。
/// 危险键（approvalMode/toolScope/providerType/workingDirectory）与只读投影
/// （contextWindow/maxOutputTokens/maxOutputTokensSource）不可由 agent 修改，
/// 归宿主/配方管理。systemPrompt/systemPromptAppend 走用户提示词特殊路径。
const QSet<QString> kAgentSettableKeys = {
    QStringLiteral("modelName"),
    QStringLiteral("agentMode"),
    QStringLiteral("reasoningEnabled"),
    QStringLiteral("reasoningEffort"),
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
};

QString jsonValueToText(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString();
    }
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

} // namespace

class ConfigTool : public AbstractSessionTool
{
public:
    [[nodiscard]] ToolSpec spec() const override;
    ToolResult execute(SessionToolContext *ctx,
                       const ToolCall &call,
                       const QString &workingDirectory) override;

private:
    ToolResult handleUserPrompt(SessionToolContext *ctx, const ToolCall &call,
                                const QString &setting, const QJsonValue &value) const;
};

inline ToolSpec ConfigTool::spec() const
{
    return ToolSpecBuilder(
        "config",
        QStringLiteral(
            "读取或更新当前会话配置。可写键：modelName（模型）、agentMode（normal/planning/debug）、"
            "maxInternalSteps、modelResponseTimeoutSecs、maxRetries、defaultShell、reasoningEnabled、"
            "reasoningEffort、maxInboxMessages、maxInboxMessageSize、compactEnabled、compactTriggerTokens、"
            "compactReserveTokens、compactTargetTokens、compactMaxRetries、compactUserMessageTokenBudget、"
            "compactMaxOutputTokens、summaryEnabled、summarySegmentTokens、summaryRecentTurns、"
            "systemPrompt（用户提示词，全量替换）、systemPromptAppend（追加到用户提示词）。"
            "示例：config(\"modelName\", \"deepseek-chat\")；config(\"modelName\") 读取当前值。"))
        .requiredInput("setting", "string", QStringLiteral("配置键（见描述）"))
        .input("value", "string", QStringLiteral("配置值，可为空；为空表示读取"))
        .output("operation", "string", "get/set")
        .output("setting", "string", QStringLiteral("配置项名称"))
        .output("value", "string", QStringLiteral("当前值"))
        .output("previousValue", "string", QStringLiteral("旧值"))
        .build();
}

inline ToolResult ConfigTool::execute(SessionToolContext *ctx, const ToolCall &call,
                                      const QString &workingDirectory)
{
    Q_UNUSED(workingDirectory);

    const QString setting = call.input.value(QStringLiteral("setting")).toString().trimmed();
    const QJsonValue value = call.input.value(QStringLiteral("value"));
    if (setting.isEmpty()) {
        return makeError(call, QStringLiteral("Config 缺少 setting。"));
    }
    if (!ctx || !ctx->session()) {
        return makeError(call, QStringLiteral("会话状态不存在"));
    }

    // 用户提示词特殊键（非 SessionRuntime 字段）
    if (setting == QStringLiteral("systemPrompt")
        || setting == QStringLiteral("systemPromptAppend")) {
        return handleUserPrompt(ctx, call, setting, value);
    }

    auto resolveKey = [](const QString &key) -> QString {
        return (key == QStringLiteral("model")) ? QStringLiteral("modelName") : key;
    };
    const QString propertyName = resolveKey(setting);
    const QJsonObject json = ctx->runtime().toJson();
    if (!json.contains(propertyName)) {
        return makeError(call, QStringLiteral("不支持的配置项：%1").arg(setting));
    }
    const QJsonValue previousValue = json.value(propertyName);

    // 写白名单：危险键/只读投影不可由 agent 修改（读不受限）
    if (call.input.contains(QStringLiteral("value"))
        && !kAgentSettableKeys.contains(propertyName)) {
        return makeError(call, QStringLiteral("该配置项不可由 agent 修改：%1").arg(setting));
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

    // getValue 已确认键存在；setRuntimeField 失败仅表示规范化后无变化（幂等）。
    ctx->setRuntimeField(propertyName, value.toVariant());

    result.success = true;
    result.text = QStringLiteral("已更新配置 %1").arg(setting);
    result.payloadType = QStringLiteral("configResult");
    result.payload = QJsonObject{
        {QStringLiteral("operation"), QStringLiteral("set")},
        {QStringLiteral("setting"), setting},
        {QStringLiteral("value"), ctx->runtime().toJson().value(propertyName)},
        {QStringLiteral("previousValue"), previousValue},
    };
    return result;
}

inline ToolResult ConfigTool::handleUserPrompt(SessionToolContext *ctx, const ToolCall &call,
                                               const QString &setting,
                                               const QJsonValue &value) const
{
    const QString current = ctx->userCustomPrompt();
    if (!call.input.contains(QStringLiteral("value"))) {
        ToolResult result;
        result.success = true;
        result.text = QStringLiteral("%1=%2").arg(setting, current);
        result.payloadType = QStringLiteral("configResult");
        result.payload = QJsonObject{
            {QStringLiteral("operation"), QStringLiteral("get")},
            {QStringLiteral("setting"), setting},
            {QStringLiteral("value"), current},
        };
        return result;
    }

    const QString valueText = jsonValueToText(value);
    const QString previous = current;
    if (setting == QStringLiteral("systemPromptAppend")) {
        const QString updated = current.isEmpty() ? valueText : current + QStringLiteral("\n") + valueText;
        ctx->setUserCustomPrompt(updated);
    } else {
        ctx->setUserCustomPrompt(valueText);
    }

    ToolResult result;
    result.success = true;
    result.text = QStringLiteral("已更新配置 %1").arg(setting);
    result.payloadType = QStringLiteral("configResult");
    result.payload = QJsonObject{
        {QStringLiteral("operation"), QStringLiteral("set")},
        {QStringLiteral("setting"), setting},
        {QStringLiteral("value"), ctx->userCustomPrompt()},
        {QStringLiteral("previousValue"), previous},
    };
    return result;
}
