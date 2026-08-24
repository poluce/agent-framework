#pragma once

#include "providers/ProviderTypes/ProviderCommon.h"
#include "tools/ToolTypes.h"

#include <QString>
#include <QJsonObject>
#include <QtGlobal>

struct ConversationMessage
{
    enum class ResultCategory {
        None,
        Success,
        Error,
        Rejected,
        Canceled
    };

    enum class Role {
        System,
        User,
        Assistant,
        Tool,
        Error,
        Approval
    };

    enum class Kind {
        SystemPrompt,
        Summary,
        SessionEvent,
        UserText,
        AgentTask,
        AssistantText,
        AssistantReasoning,
        ToolCall,
        ToolResult,
        SkillInvoke,
        Error,
        ApprovalRequest
    };

    enum class Status {
        Pending,
        Queued,
        ClassifierChecking,
        Streaming,
        WaitingApproval,
        WaitingAnswer,
        Running,
        Approved,
        Rejected,
        Completed,
        Failed,
        Canceled
    };

    QString id;
    Kind kind = Kind::AssistantText;
    Status status = Status::Completed;
    QString text;
    QString reasoningContent;
    QString reasoningSignature;
    bool reasoningRedacted = false;
    bool reasoningMustReplay = false;
    bool providerMustReplay = false;  // Provider 协议签名/指纹/审批等要求逐字回放
    QString toolName;
    QString toolUseId;
    QJsonObject toolInput;
    QString toolPayloadType;
    QJsonObject toolPayload;
    QString summaryText;
    QString progressText;
    QString previewText;
    QString persistedPath;
    QString groupKey;
    QString turnId;
    QString responseId;
    QString providerContinuationId;
    QJsonObject providerLogprobs;
    qint64 estimatedTokenCount = 0;
    int inputTokens = 0;
    int outputTokens = 0;
    int cacheReadTokens = 0;
    int cacheCreationTokens = 0;
    int thoughtTokens = 0;
    bool isError = false;
    bool wasPersisted = false;
    bool wasTruncated = false;
    ResultCategory resultCategory = ResultCategory::None;
    qint64 createdAtMs = 0;
    bool submittedToModel = false;
    bool wasCompacted = false;
    ToolCall toolCall;
    QStringList attachedFilePaths;
    ProviderImageAsset imageOutput;

    // 消息身份的唯一来源是 kind，role 是派生只读视图（不可存储、不可独立赋值），
    // 仅供 UI 分组与展示使用。发送给 Provider 的线路角色由 ProviderRunLedger
    // 按 kind 统一投影为 ProviderItem，与本访问器无关。
    [[nodiscard]] static constexpr Role roleFromKind(const Kind kind)
    {
        switch (kind) {
        case Kind::SystemPrompt:
        case Kind::Summary:
        case Kind::SessionEvent:
        case Kind::AgentTask:
        case Kind::SkillInvoke:
            return Role::System;
        case Kind::UserText:
            return Role::User;
        case Kind::AssistantText:
        case Kind::AssistantReasoning:
            return Role::Assistant;
        case Kind::ToolCall:
        case Kind::ToolResult:
            return Role::Tool;
        case Kind::ApprovalRequest:
            return Role::Approval;
        case Kind::Error:
            return Role::Error;
        }
        return Role::Assistant;
    }

    [[nodiscard]] constexpr Role role() const { return roleFromKind(kind); }

    // 持久指令：上下文压缩时逐字保留、不参与摘要（CompactEngine 的选择与降级截断均豁免）
    [[nodiscard]] constexpr bool isCompactExempt() const
    {
        return kind == Kind::SkillInvoke || reasoningMustReplay || providerMustReplay;
    }
};

namespace ConversationMessageText {

inline QString storageRole(const ConversationMessage::Role role)
{
    switch (role) {
    case ConversationMessage::Role::System:
        return QStringLiteral("system");
    case ConversationMessage::Role::User:
        return QStringLiteral("user");
    case ConversationMessage::Role::Assistant:
        return QStringLiteral("assistant");
    case ConversationMessage::Role::Tool:
        return QStringLiteral("tool");
    case ConversationMessage::Role::Error:
        return QStringLiteral("error");
    case ConversationMessage::Role::Approval:
        return QStringLiteral("approval");
    }

    return QStringLiteral("assistant");
}

inline QString storageKind(const ConversationMessage::Kind kind)
{
    switch (kind) {
    case ConversationMessage::Kind::SystemPrompt:
        return QStringLiteral("system_prompt");
    case ConversationMessage::Kind::Summary:
        return QStringLiteral("summary");
    case ConversationMessage::Kind::SessionEvent:
        return QStringLiteral("session_event");
    case ConversationMessage::Kind::UserText:
        return QStringLiteral("user_text");
    case ConversationMessage::Kind::AgentTask:
        return QStringLiteral("agent_task");
    case ConversationMessage::Kind::AssistantText:
        return QStringLiteral("assistant_text");
    case ConversationMessage::Kind::AssistantReasoning:
        return QStringLiteral("assistant_reasoning");
    case ConversationMessage::Kind::ToolCall:
        return QStringLiteral("tool_use");
    case ConversationMessage::Kind::ToolResult:
        return QStringLiteral("tool_result");
    case ConversationMessage::Kind::SkillInvoke:
        return QStringLiteral("skill_invoke");
    case ConversationMessage::Kind::Error:
        return QStringLiteral("error");
    case ConversationMessage::Kind::ApprovalRequest:
        return QStringLiteral("approval_request");
    }

    return QStringLiteral("assistant_text");
}

inline ConversationMessage::Kind kindFromStorage(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("system_prompt")) return ConversationMessage::Kind::SystemPrompt;
    if (normalized == QStringLiteral("summary")) return ConversationMessage::Kind::Summary;
    if (normalized == QStringLiteral("session_event")) return ConversationMessage::Kind::SessionEvent;
    if (normalized == QStringLiteral("user_text")) return ConversationMessage::Kind::UserText;
    if (normalized == QStringLiteral("agent_task")) return ConversationMessage::Kind::AgentTask;
    if (normalized == QStringLiteral("assistant_reasoning")) return ConversationMessage::Kind::AssistantReasoning;
    if (normalized == QStringLiteral("tool_use") || normalized == QStringLiteral("tool_call")) return ConversationMessage::Kind::ToolCall;
    if (normalized == QStringLiteral("tool_result")) return ConversationMessage::Kind::ToolResult;
    if (normalized == QStringLiteral("skill_invoke")) return ConversationMessage::Kind::SkillInvoke;
    if (normalized == QStringLiteral("error")) return ConversationMessage::Kind::Error;
    if (normalized == QStringLiteral("approval_request")) return ConversationMessage::Kind::ApprovalRequest;
    return ConversationMessage::Kind::AssistantText;
}

inline QString storageStatus(const ConversationMessage::Status status)
{
    switch (status) {
    case ConversationMessage::Status::Pending:
        return QStringLiteral("pending");
    case ConversationMessage::Status::Queued:
        return QStringLiteral("queued");
    case ConversationMessage::Status::ClassifierChecking:
        return QStringLiteral("classifier_checking");
    case ConversationMessage::Status::Streaming:
        return QStringLiteral("streaming");
    case ConversationMessage::Status::WaitingApproval:
        return QStringLiteral("waiting_approval");
    case ConversationMessage::Status::WaitingAnswer:
        return QStringLiteral("waiting_answer");
    case ConversationMessage::Status::Running:
        return QStringLiteral("running");
    case ConversationMessage::Status::Approved:
        return QStringLiteral("approved");
    case ConversationMessage::Status::Rejected:
        return QStringLiteral("rejected");
    case ConversationMessage::Status::Completed:
        return QStringLiteral("completed");
    case ConversationMessage::Status::Failed:
        return QStringLiteral("failed");
    case ConversationMessage::Status::Canceled:
        return QStringLiteral("canceled");
    }

    return QStringLiteral("completed");
}

inline ConversationMessage::Status statusFromStorage(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("pending")) return ConversationMessage::Status::Pending;
    if (normalized == QStringLiteral("queued")) return ConversationMessage::Status::Queued;
    if (normalized == QStringLiteral("classifier_checking")) return ConversationMessage::Status::ClassifierChecking;
    if (normalized == QStringLiteral("streaming")) return ConversationMessage::Status::Streaming;
    if (normalized == QStringLiteral("waiting_approval")) return ConversationMessage::Status::WaitingApproval;
    if (normalized == QStringLiteral("waiting_answer")) return ConversationMessage::Status::WaitingAnswer;
    if (normalized == QStringLiteral("running")) return ConversationMessage::Status::Running;
    if (normalized == QStringLiteral("approved")) return ConversationMessage::Status::Approved;
    if (normalized == QStringLiteral("rejected")) return ConversationMessage::Status::Rejected;
    if (normalized == QStringLiteral("failed")) return ConversationMessage::Status::Failed;
    if (normalized == QStringLiteral("canceled")) return ConversationMessage::Status::Canceled;
    return ConversationMessage::Status::Completed;
}

inline QString storageResultCategory(const ConversationMessage::ResultCategory category)
{
    switch (category) {
    case ConversationMessage::ResultCategory::None:
        return QStringLiteral("none");
    case ConversationMessage::ResultCategory::Success:
        return QStringLiteral("success");
    case ConversationMessage::ResultCategory::Error:
        return QStringLiteral("error");
    case ConversationMessage::ResultCategory::Rejected:
        return QStringLiteral("rejected");
    case ConversationMessage::ResultCategory::Canceled:
        return QStringLiteral("canceled");
    }

    return QStringLiteral("none");
}

inline ConversationMessage::ResultCategory resultCategoryFromStorage(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("success")) return ConversationMessage::ResultCategory::Success;
    if (normalized == QStringLiteral("error")) return ConversationMessage::ResultCategory::Error;
    if (normalized == QStringLiteral("rejected")) return ConversationMessage::ResultCategory::Rejected;
    if (normalized == QStringLiteral("canceled")) return ConversationMessage::ResultCategory::Canceled;
    return ConversationMessage::ResultCategory::None;
}

inline QString uiRole(const ConversationMessage::Role role)
{
    return storageRole(role);
}

inline QString uiKind(const ConversationMessage::Kind kind)
{
    return storageKind(kind);
}

inline QString uiStatus(const ConversationMessage::Status status)
{
    switch (status) {
    case ConversationMessage::Status::Pending:
    case ConversationMessage::Status::Queued:
        return QStringLiteral("queued");
    case ConversationMessage::Status::ClassifierChecking:
        return QStringLiteral("classifier_checking");
    case ConversationMessage::Status::Streaming:
        return QStringLiteral("streaming");
    case ConversationMessage::Status::WaitingApproval:
        return QStringLiteral("waiting_permission");
    case ConversationMessage::Status::WaitingAnswer:
        return QStringLiteral("waiting_answer");
    case ConversationMessage::Status::Running:
        return QStringLiteral("running");
    case ConversationMessage::Status::Approved:
        return QStringLiteral("approved");
    case ConversationMessage::Status::Rejected:
        return QStringLiteral("rejected");
    case ConversationMessage::Status::Completed:
        return QStringLiteral("completed");
    case ConversationMessage::Status::Failed:
        return QStringLiteral("failed");
    case ConversationMessage::Status::Canceled:
        return QStringLiteral("canceled");
    }

    return QStringLiteral("completed");
}

inline QString uiResultCategory(const ConversationMessage::ResultCategory category)
{
    switch (category) {
    case ConversationMessage::ResultCategory::None:
        return QStringLiteral("pending");
    case ConversationMessage::ResultCategory::Success:
        return QStringLiteral("success");
    case ConversationMessage::ResultCategory::Error:
        return QStringLiteral("error");
    case ConversationMessage::ResultCategory::Rejected:
        return QStringLiteral("rejected");
    case ConversationMessage::ResultCategory::Canceled:
        return QStringLiteral("canceled");
    }

    return QStringLiteral("pending");
}

inline QString transcriptLabel(const ConversationMessage &message)
{
    switch (message.kind) {
    case ConversationMessage::Kind::SystemPrompt:
        return QStringLiteral("[system]");
    case ConversationMessage::Kind::Summary:
        return QStringLiteral("[summary]");
    case ConversationMessage::Kind::SessionEvent:
        return QStringLiteral("[session]");
    case ConversationMessage::Kind::UserText:
        return QStringLiteral("[user]");
    case ConversationMessage::Kind::AgentTask:
        return QStringLiteral("[agent_task]");
    case ConversationMessage::Kind::AssistantText:
        return QStringLiteral("[assistant]");
    case ConversationMessage::Kind::AssistantReasoning:
        return QStringLiteral("[reasoning]");
    case ConversationMessage::Kind::ToolCall:
        return QStringLiteral("[tool_call:%1]").arg(message.toolName);
    case ConversationMessage::Kind::ToolResult:
        return QStringLiteral("[tool_result:%1]").arg(message.toolName);
    case ConversationMessage::Kind::SkillInvoke:
        return QStringLiteral("[skill:%1]").arg(message.toolName);
    case ConversationMessage::Kind::ApprovalRequest:
        return QStringLiteral("[approval:%1]").arg(message.toolName);
    case ConversationMessage::Kind::Error:
        return QStringLiteral("[error]");
    }
    return QStringLiteral("[message]");
}

} // namespace ConversationMessageText
