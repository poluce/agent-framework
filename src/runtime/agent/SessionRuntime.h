#pragma once

#include "AgentMode.h"
#include "compact/CompactConfig.h"
#include "config/ModelTokenDefaults.h"
#include <QJsonObject>
#include <QVariant>

/// 统一的 Agent 运行时配置：Q_GADGET + X-macro 驱动 toJson/fromJson/setField。
/// 客户端不直绑本结构；经 Host RuntimeConfigSnapshot / SetSessionConfig 投影。
/// 会话活副本由 AgentSession 持有；Agent 持执行快照（经 applySessionSettings 同步）。
/// 策略字段（agentMode/toolScope/approvalMode）内核是枚举；JSON/Host 边界转键。
struct SessionRuntime {
    Q_GADGET

#define GD_FIELD(Type, Name, Default) Q_PROPERTY(Type Name MEMBER Name)
#define GD_ENUM_FIELD(Type, Name, EnumDefault, StringDefault)
#include "config/SessionRuntime.fields.h"
#undef GD_FIELD
#undef GD_ENUM_FIELD

public:
#define GD_FIELD(Type, Name, Default) Type Name = Default;
#define GD_ENUM_FIELD(Type, Name, EnumDefault, StringDefault) Type Name = EnumDefault;
#include "config/SessionRuntime.fields.h"
#undef GD_FIELD
#undef GD_ENUM_FIELD

    QJsonObject toJson() const;
    static SessionRuntime fromJson(const QJsonObject &obj);

    /// 按字段名设值（Host SetSessionConfig / 内部工具）；不做语义规范化。
    bool setField(const QString &name, const QVariant &value);

    /// 设值后对已知枚举/路径字段做规范化（会话活副本入口）。
    bool setFieldNormalized(const QString &name, const QVariant &value);

    /// 规范化全部已知语义字段。
    void normalizeInPlace();

    /// 提取压缩配置给 CompactEngine
    CompactConfig toCompactConfig() const;

    static QString normalizeWorkingDirectory(const QString &value);
    static QString normalizeAgentMode(const QString &value);
    static QString normalizeToolScope(const QString &value);
    static QString normalizeApprovalMode(const QString &value);

private:
    /// 仅规范化 name 对应字段；未知名无副作用。
    void normalizeFieldInPlace(const QString &name);
};
