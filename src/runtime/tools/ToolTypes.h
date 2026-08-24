#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

enum class ToolResultCategory {
    Success,
    Error,
    Rejected,
    Canceled
};

enum class ToolPermissionKind {
    ReadOnly,
    Write,
    Command
};

enum class ToolPermissionDecision {
    Allow,
    NeedsApproval,
    Deny
};

struct ToolSpec
{
    QString name;
    QString description;
    QJsonObject inputSchema;
    QJsonObject outputSchema;
    ToolPermissionKind permissionKind = ToolPermissionKind::ReadOnly;
    bool strictSchema = false;
    bool deferLoading = false;
    QStringList allowedCallers;
};

struct ToolCall
{
    QString id;
    QString toolName;
    QJsonObject input;
    QString rawInputJson;
    QString callerType;
    QString callerId;
};

struct ToolResult
{
    QString toolName;
    QString toolUseId;
    bool success = false;
    bool isError = false;
    ToolResultCategory category = ToolResultCategory::Success;
    QString payloadType;
    QJsonObject payload;
    QString text;
    QString summaryText;
    QString progressText;
    QString previewText;
    QString persistedPath;
    bool wasPersisted = false;
    bool wasTruncated = false;
};

struct PendingApprovalRequest
{
    QString toolUseId;
    QString toolName;
    QString summary;
    QString rawInputJson;
    ToolPermissionKind permissionKind = ToolPermissionKind::ReadOnly;

    [[nodiscard]] bool isValid() const
    {
        return !toolUseId.isEmpty() && !toolName.isEmpty();
    }
};

// ═══════════════════════════════════════════
//  ToolSpec 构建器（链式 API）
// ═══════════════════════════════════════════

#include <QSet>
#include <QJsonArray>

class ToolSpecBuilder
{
public:
    ToolSpecBuilder(const QString &name,
                    const QString &description,
                    ToolPermissionKind permission = ToolPermissionKind::ReadOnly)
        : m_name(name), m_description(description), m_permission(permission) {}

    ToolSpecBuilder &input(const QString &name, const QString &type, const QString &desc)
    {
        m_inputProperties.insert(name, propertyForType(type, desc));
        return *this;
    }

    ToolSpecBuilder &requiredInput(const QString &name, const QString &type, const QString &desc)
    {
        m_inputProperties.insert(name, propertyForType(type, desc));
        m_requiredInputs.append(name);
        return *this;
    }

    ToolSpecBuilder &output(const QString &name, const QString &type, const QString &desc)
    {
        m_outputProperties.insert(name, propertyForType(type, desc));
        return *this;
    }

    ToolSpecBuilder &input(const QString &name, const QJsonObject &schema, const QString &desc)
    {
        m_inputProperties.insert(name, propertyFromSchema(schema, desc));
        return *this;
    }

    ToolSpecBuilder &requiredInput(const QString &name, const QJsonObject &schema, const QString &desc)
    {
        m_inputProperties.insert(name, propertyFromSchema(schema, desc));
        m_requiredInputs.append(name);
        return *this;
    }

    ToolSpecBuilder &output(const QString &name, const QJsonObject &schema, const QString &desc)
    {
        m_outputProperties.insert(name, propertyFromSchema(schema, desc));
        return *this;
    }

    ToolSpec build() const
    {
        QJsonArray requiredArray;
        for (const QString &r : m_requiredInputs)
            requiredArray.append(r);

        ToolSpec spec;
        spec.name = m_name;
        spec.description = m_description;
        spec.permissionKind = m_permission;
        spec.inputSchema = {
            {QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("properties"), m_inputProperties},
            {QStringLiteral("required"), requiredArray},
        };

        if (!m_outputProperties.isEmpty()) {
            spec.outputSchema = {
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("properties"), m_outputProperties},
                {QStringLiteral("required"), QJsonArray{}},
            };
        }

        return spec;
    }

private:
    static QJsonObject propertyForType(const QString &type, const QString &desc)
    {
        static const QSet<QString> validTypes{
            QStringLiteral("string"),
            QStringLiteral("integer"),
            QStringLiteral("boolean"),
            QStringLiteral("array"),
            QStringLiteral("object"),
            QStringLiteral("stringArray"),
            QStringLiteral("jsonArray"),
        };
        Q_ASSERT_X(validTypes.contains(type),
                   "ToolSpecBuilder",
                   qPrintable(QStringLiteral("unknown property type: %1").arg(type)));

        if (type == QStringLiteral("stringArray")) {
            return {{QStringLiteral("type"), QStringLiteral("array")},
                    {QStringLiteral("description"), desc},
                    {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}};
        }
        if (type == QStringLiteral("object")) {
            return {{QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("description"), desc}};
        }
        if (type == QStringLiteral("array")) {
            return {{QStringLiteral("type"), QStringLiteral("array")},
                    {QStringLiteral("description"), desc}};
        }
        if (type == QStringLiteral("jsonArray")) {
            return {{QStringLiteral("type"), QStringLiteral("array")},
                    {QStringLiteral("description"), desc},
                    {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}};
        }
        return {{QStringLiteral("type"), type},
                {QStringLiteral("description"), desc}};
    }

    static QJsonObject propertyFromSchema(QJsonObject schema, const QString &desc)
    {
        if (!schema.contains(QStringLiteral("description")))
            schema.insert(QStringLiteral("description"), desc);
        return schema;
    }

    QString m_name;
    QString m_description;
    ToolPermissionKind m_permission;
    QJsonObject m_inputProperties;
    QStringList m_requiredInputs;
    QJsonObject m_outputProperties;
};
