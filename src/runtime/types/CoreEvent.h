#pragma once

#include "ConversationMessage.h"
#include "tools/ToolTypes.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <optional>
#include <variant>

/**
 * @file CoreEvent.h
 * @brief 执行单元内环事件 IR
 *
 * Loop / Agent / Session 发出这些事件。宿主自行投影到自己的协议。
 * 本文件不承载 MCP、团队成员、会话列表、技能目录、凭据目录等产品壳载荷。
 */

namespace core_ir {

// ── 类型别名 ──

using SubmissionId = QString;
using ItemId = QString;
using TurnId = QString;
using AgentId = QString;
using CallId = QString;
using SessionId = QString;

/// 每条内环事件带显式归属。空字段表示无会话/单元归属。
struct EventContext {
    SessionId sessionId;
    AgentId agentId;

    [[nodiscard]] bool isGlobal() const
    {
        return sessionId.isEmpty() && agentId.isEmpty();
    }
};

/// Loop 阶段身份（非展示文案）。
enum class AgentPhase {
    Idle,
    Preparing,
    CallingProvider,
    Streaming,
    WaitingApproval,
    WaitingAnswer,
    ExecutingTool,
    WaitingTool,
    AppendingToolResults,
    Completed,
    Failed,
    Canceled,
    Compacting
};

[[nodiscard]] inline QString agentPhaseKey(const AgentPhase phase)
{
    switch (phase) {
    case AgentPhase::Idle: return QStringLiteral("idle");
    case AgentPhase::Preparing: return QStringLiteral("preparing");
    case AgentPhase::CallingProvider: return QStringLiteral("calling_provider");
    case AgentPhase::Streaming: return QStringLiteral("streaming");
    case AgentPhase::WaitingApproval: return QStringLiteral("waiting_approval");
    case AgentPhase::WaitingAnswer: return QStringLiteral("waiting_answer");
    case AgentPhase::ExecutingTool: return QStringLiteral("executing_tool");
    case AgentPhase::WaitingTool: return QStringLiteral("waiting_tool");
    case AgentPhase::AppendingToolResults: return QStringLiteral("appending_tool_results");
    case AgentPhase::Completed: return QStringLiteral("completed");
    case AgentPhase::Failed: return QStringLiteral("failed");
    case AgentPhase::Canceled: return QStringLiteral("canceled");
    case AgentPhase::Compacting: return QStringLiteral("compacting");
    }
    return QStringLiteral("idle");
}

/// Agent 管理/派生运行态（排队、终态），与 Loop 的 AgentPhase 不同。
enum class AgentStatus {
    Idle,
    Running,
    Queued,
    Completed,
    Failed,
    Canceled
};

[[nodiscard]] inline QString agentStatusKey(const AgentStatus status)
{
    switch (status) {
    case AgentStatus::Idle: return QStringLiteral("idle");
    case AgentStatus::Running: return QStringLiteral("running");
    case AgentStatus::Queued: return QStringLiteral("queued");
    case AgentStatus::Completed: return QStringLiteral("completed");
    case AgentStatus::Failed: return QStringLiteral("failed");
    case AgentStatus::Canceled: return QStringLiteral("canceled");
    }
    return QStringLiteral("idle");
}

[[nodiscard]] inline AgentStatus agentStatusFromKey(const QString &key)
{
    const QString k = key.trimmed().toLower();
    if (k == QLatin1String("running")) return AgentStatus::Running;
    if (k == QLatin1String("queued")) return AgentStatus::Queued;
    if (k == QLatin1String("completed")) return AgentStatus::Completed;
    if (k == QLatin1String("failed")) return AgentStatus::Failed;
    if (k == QLatin1String("canceled")) return AgentStatus::Canceled;
    return AgentStatus::Idle;
}

/// 邮箱消息优先级（内核只负责排序/携带，调度策略由编排决定）。
enum class InboxPriority {
    Low,
    Normal,
    High,
    Urgent
};

[[nodiscard]] inline QString inboxPriorityKey(const InboxPriority priority)
{
    switch (priority) {
    case InboxPriority::Low: return QStringLiteral("low");
    case InboxPriority::Normal: return QStringLiteral("normal");
    case InboxPriority::High: return QStringLiteral("high");
    case InboxPriority::Urgent: return QStringLiteral("urgent");
    }
    return QStringLiteral("normal");
}

struct PendingQuestion {
    QString questionId;
    QString question;
    QStringList options;
    bool isMultiSelect = false;
    bool answered = false;
};

// ── Item 生命周期 ──

enum class ItemKind {
    Message,
    Reasoning,
    FunctionCall,
};

struct EventItemStarted {
    ItemId itemId;
    TurnId turnId;
    ItemKind itemKind;
    qint64 startedAtMs = 0;
};

struct EventItemCompleted {
    ItemId itemId;
    TurnId turnId;
    ItemKind itemKind;
    qint64 completedAtMs = 0;
    std::optional<ToolCall> toolCall;
    std::optional<ToolResult> toolResult;
    QString summaryText;
};

// ── 消息 ──

struct EventMessageAppended {
    ConversationMessage message;
};

struct EventMessageStatusChanged {
    QString messageId;
    ConversationMessage::Status status;
};

struct EventAgentMessageContentDelta {
    QString messageId;
    QString delta;
};

struct EventReasoningContentDelta {
    QString messageId;
    QString delta;
    int summaryIndex = 0;
};

// ── 工具 ──

struct EventToolCallBegin {
    CallId callId;
    QString toolName;
    QJsonObject input;
    QString summaryText;
    // 账本条目身份：增量构建的消息必须与全量同步投影同构，否则 UI 差分合并退化为整表重置
    QString messageId;
    QString turnId;
};

struct EventToolProgress {
    CallId callId;
    QString progressKind;
    QString message;
};

struct EventCommandOutputDelta {
    CallId callId;
    QString delta;
    bool isStderr = false;
};

struct EventToolCallEnd {
    CallId callId;
    ToolResult result;
    // 账本条目身份（同 EventToolCallBegin）
    QString messageId;
    QString turnId;
};

/// Agent 成功写入/编辑的工作区文件。
struct EventFileTouched {
    QString path;
    QString toolName;
    qint64 atMs = 0;
};

// ── Agent 状态 ──

struct EventAgentStateChanged {
    AgentId agentId;
    bool busy = false;
    AgentPhase phase = AgentPhase::Idle;
    bool canSubmit = true;
    bool hasPendingApproval = false;
    QString pendingApprovalSummary;
    QString lastError;
    qint64 currentContextTokenEstimate = 0;
    AgentStatus status = AgentStatus::Idle;
    bool hasPendingQuestion = false;
    int pendingQuestionCount = 0;
    QList<PendingQuestion> pendingQuestionList;
    /// next_turn 待发送条数（预览截断归宿主投影）
    int pendingNextTurnCount = 0;
    QStringList pendingNextTurnPreviews;
    /// 自上次段摘要/入队末尾起累计的可摘要 token（未开段摘要恒 0）
    qint64 segmentSummaryAddedTokens = 0;
};

struct EventApprovalRequested {
    CallId toolUseId;
    QString toolName;
    QString summary;
    QString rawInputJson;
};

// ── 回合 ──

struct EventTurnStarted {
    TurnId turnId;
    qint64 startedAtMs = 0;
};

struct EventTurnComplete {
    TurnId turnId;
    qint64 durationMs = 0;
    qint64 timeToFirstTokenMs = 0;
};

// ── 邮箱 ──

struct EventInboxMessageEnqueued {
    QString messageId;
    AgentId fromAgentId;
    AgentId targetAgentId;
    InboxPriority priority = InboxPriority::Normal;
};

struct EventInboxMessageDelivered {
    QString messageId;
    AgentId fromAgentId;
    AgentId targetAgentId;
};

struct EventInboxMessageDropped {
    QString messageId;
    AgentId fromAgentId;
    AgentId targetAgentId;
    QString reason;
};

// ── 会话 ──

struct EventSessionEvent {
    AgentId agentId;
    QString text;
};

struct EventConfigChanged {
    QString key;
    QVariant value;
};

// ── 统计 ──

struct EventTokenCount {
    int inputTokens = 0;
    int outputTokens = 0;
    int cacheReadTokens = 0;
    int cacheWriteTokens = 0;
};

// ── 系统 ──

struct EventError {
    AgentId agentId;
    QString message;
};

struct EventWarning {
    AgentId agentId;
    QString message;
};

/// 压缩/组装写库原因（非展示文案）。
enum class CompactReason {
    Assemble,
    Bulk,
    Segment,
    Truncate
};

[[nodiscard]] inline QString compactReasonKey(const CompactReason reason)
{
    switch (reason) {
    case CompactReason::Assemble: return QStringLiteral("assemble");
    case CompactReason::Bulk: return QStringLiteral("bulk");
    case CompactReason::Segment: return QStringLiteral("segment");
    case CompactReason::Truncate: return QStringLiteral("truncate");
    }
    return QStringLiteral("assemble");
}

/// 压缩/组装写库后通知（可观测摘要库规模；不回传正文）
struct EventContextCompacted {
    CompactReason reason = CompactReason::Assemble;
    int summaryRecordCount = 0;
    int modelViewPrefixCount = 0;
    qint64 summaryTokenEstimate = 0;
};

struct EventImageOutput {
    QString messageId;
    ProviderImageAsset image;
};

struct EventShutdownComplete {};

using Event = std::variant<
    EventItemStarted,
    EventItemCompleted,
    EventMessageAppended,
    EventMessageStatusChanged,
    EventAgentMessageContentDelta,
    EventReasoningContentDelta,
    EventToolCallBegin,
    EventToolProgress,
    EventCommandOutputDelta,
    EventToolCallEnd,
    EventFileTouched,
    EventAgentStateChanged,
    EventApprovalRequested,
    EventTurnStarted,
    EventTurnComplete,
    EventInboxMessageEnqueued,
    EventInboxMessageDelivered,
    EventInboxMessageDropped,
    EventSessionEvent,
    EventConfigChanged,
    EventTokenCount,
    EventError,
    EventWarning,
    EventContextCompacted,
    EventImageOutput,
    EventShutdownComplete
>;

/// 测试 fixture 信封；生产 fan-out 不再构造（直接 Event+Context+SubmissionId）。
struct EventEnvelope {
    SubmissionId submissionId;
    Event msg;
    EventContext context;
};

} // namespace core_ir
