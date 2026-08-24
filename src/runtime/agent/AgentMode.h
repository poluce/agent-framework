#pragma once

#include <QString>

enum class AgentMode {
    Normal,
    Planning,
    AutoDebug
};

inline AgentMode parseAgentMode(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("planning")) {
        return AgentMode::Planning;
    }
    if (normalized == QStringLiteral("debug")) {
        return AgentMode::AutoDebug;
    }
    return AgentMode::Normal;
}

inline QString agentModeToString(const AgentMode mode)
{
    switch (mode) {
    case AgentMode::Planning:
        return QStringLiteral("planning");
    case AgentMode::AutoDebug:
        return QStringLiteral("debug");
    case AgentMode::Normal:
        return QStringLiteral("normal");
    }
    return QStringLiteral("normal");
}

enum class ToolScope {
    Full,
    ReadOnly
};

inline ToolScope parseToolScope(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("readonly")
        || normalized == QStringLiteral("read_only")) {
        return ToolScope::ReadOnly;
    }
    return ToolScope::Full;
}

inline QString toolScopeToString(const ToolScope scope)
{
    switch (scope) {
    case ToolScope::ReadOnly:
        return QStringLiteral("readOnly");
    case ToolScope::Full:
        break;
    }
    return QStringLiteral("full");
}

enum class ApprovalMode {
    Ask,
    Auto
};

/// 权限判定用：auto / bypass / bypassPermissions 都视为总开关。
/// 与 SessionRuntime::normalizeApprovalMode 不同：后者落盘只保留 ask|auto，
/// bypass* 会被收成 ask。
inline ApprovalMode parseApprovalMode(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("auto")
        || normalized == QStringLiteral("bypass")
        || normalized == QStringLiteral("bypasspermissions")) {
        return ApprovalMode::Auto;
    }
    return ApprovalMode::Ask;
}

inline QString approvalModeToString(const ApprovalMode mode)
{
    switch (mode) {
    case ApprovalMode::Auto:
        return QStringLiteral("auto");
    case ApprovalMode::Ask:
        break;
    }
    return QStringLiteral("ask");
}
