#pragma once

#include "tools/AbstractBuiltinTool.h"
#include "tools/BuiltinToolRuntime.h"

#include <QJsonArray>
#include <QJsonValue>

#include <variant>

class AskQuestionTool : public AbstractBuiltinTool
{
public:
    /// 归一化后的提问请求（工具层契约结果，不含 Loop 状态）
    struct ParsedRequest {
        QString questionId;
        QString question;
        QStringList options;
        bool isMultiSelect = false;
    };

    [[nodiscard]] ToolSpec spec() const override
    {
        return ToolSpecBuilder("ask_question",
            QStringLiteral("向用户提问，获取反馈、确认或选择。在用户提交前执行流挂起。\n"
                           "重要规则：\n"
                           "- 每次调用只提一个问题。一个 question 字段 = 一个问题。\n"
                           "- 如需同时提多个独立问题，请在同一个响应中发起多次 ask_question 调用，每次一个。会以标签页形式展示。\n"
                           "- options 是必填字段，必须提供不少于 2 个选项。禁止把选项写在 question 文本里。\n"
                           "- options 必须是字符串数组，格式：[\"选项A\",\"选项B\"]。"))
            .requiredInput("question", "string", QStringLiteral("一个独立、明确的问题句子。只包含一个问题，不要合并多个问题"))
            .requiredInput("options", "stringArray", QStringLiteral("必填。选项列表，每个元素是字符串。格式：[\"A\",\"B\"]，至少 2 个"))
            .input("is_multi_select", "boolean", QStringLiteral("是否允许多选（默认 false）"))
            .output("answer", "string", QStringLiteral("用户提交的回答文本"))
            .build();
    }

    ToolResult execute(const ToolCall &call,
                       const QString &workspaceRoot,
                       const QString &workingDirectory,
                       const QVariantMap &threadSafeContext) override
    {
        Q_UNUSED(workspaceRoot);
        Q_UNUSED(workingDirectory);
        Q_UNUSED(threadSafeContext);
        return BuiltinToolRuntime::makeErrorResult(call, QStringLiteral("ask_question 应由 Loop 阻断处理。"));
    }

    /// 纯函数：按工具 schema 校验 ask_question 入参。成功返回 ParsedRequest，失败返回 ToolResult。
    /// 不触碰 Loop 状态；只接受规范字段 question + string[] options。
    [[nodiscard]] static std::variant<ParsedRequest, ToolResult> parseCall(const ToolCall &call)
    {
        const QString question =
            call.input.value(QStringLiteral("question")).toString();
        if (question.trimmed().isEmpty()) {
            return BuiltinToolRuntime::makeErrorResult(call,
                QStringLiteral("参数校验失败：question 是必填字段，请提供一个明确的问题内容后重试。"));
        }

        // 校验 options 存在且为数组
        const QJsonValue optsValue = call.input.value(QStringLiteral("options"));
        if (optsValue.isUndefined() || optsValue.isNull()) {
            return BuiltinToolRuntime::makeErrorResult(call,
                QStringLiteral("参数错误：options 是必填字段。必须提供选项列表，格式：[\"选项A\", \"选项B\"]。"));
        }
        if (!optsValue.isArray()) {
            return BuiltinToolRuntime::makeErrorResult(call,
                QStringLiteral("参数错误：options 必须是字符串数组，例如 [\"选项A\", \"选项B\"]。"));
        }

        QStringList options;
        const QJsonArray arr = optsValue.toArray();
        for (int i = 0; i < arr.size(); ++i) {
            const QJsonValue value = arr.at(i);
            if (!value.isString()) {
                return BuiltinToolRuntime::makeErrorResult(call,
                    QStringLiteral("参数错误：options[%1] 类型无效。每个元素必须是字符串，例如 [\"选项A\", \"选项B\"]。")
                        .arg(i));
            }
            const QString option = value.toString().trimmed();
            if (!option.isEmpty()) {
                options.append(option);
            }
        }
        if (options.size() < 2) {
            return BuiltinToolRuntime::makeErrorResult(call,
                QStringLiteral("参数错误：options 至少需要 2 个非空选项，格式：[\"选项A\", \"选项B\"]。"));
        }

        ParsedRequest parsed;
        parsed.questionId = call.id;
        parsed.question = question.trimmed();
        parsed.options = std::move(options);
        parsed.isMultiSelect = call.input.value(QStringLiteral("is_multi_select")).toBool();
        return parsed;
    }
};
