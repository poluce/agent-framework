#pragma once

#include "models/ConversationMessage.h"
#include "tools/BuiltinToolRuntime.h"
#include "providers/ProviderTypes/ProviderTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>



struct ProviderRequestBuild
{
    ProviderRequest request;
    QList<QString> submittedEntryIds;
};

[[nodiscard]] QString effectiveToolCallArgumentsJson(const ToolCall &toolCall);

/**
 * 轻量上下文 token 估算（非厂商 tokenizer）。
 * 中文按约 1.5 字/token、其它按约 4 字符/token；用于压缩门控与 UI 量级显示。
 */
[[nodiscard]] qint64 estimateContextTokensForText(const QString &text);
[[nodiscard]] qint64 estimateContextTokensForItem(const ProviderItem &item);
[[nodiscard]] qint64 estimateContextTokensForToolSpecs(const QList<ProviderToolSpecification> &tools);

class ProviderRunLedger
{
public:
    void clear();

    [[nodiscard]] const QList<ConversationMessage> &entries() const;

    /// UI/工具域入站投影为 ProviderItem；厂商输出必须使用 appendProviderItem。
    QString appendUiIngress(ConversationMessage entry);
    QString appendProviderItem(ProviderItem item,
                               const QString &turnId = {},
                               const QString &continuationId = {});
    void setProviderItemForEntry(const QString &entryId,
                                 ProviderItem item,
                                 const QString &continuationId = {});
    /// 只清线路记录，保留 UI 投影（空推理不可回放时用）。
    bool clearProviderRecord(const QString &entryId);
    /// 回滚未提交收口的目标 Turn（取消或异常终止时物理抹除脏数据）
    void rollbackUncommittedTurn(const QString &turnId);
    bool removeEntry(const QString &entryId);
    ConversationMessage *findLatestReasoningForTurn(const QString &turnId);
    const ConversationMessage *findLatestReasoningForTurn(const QString &turnId) const;
    [[nodiscard]] const QList<ProviderItem> providerItems() const;
    ConversationMessage *findById(const QString &entryId);
    const ConversationMessage *findById(const QString &entryId) const;
    ConversationMessage *findToolCallByUseId(const QString &toolUseId);
    const ConversationMessage *findToolCallByUseId(const QString &toolUseId) const;

    void markSubmitted(const QList<QString> &entryIds);
    void markEntriesCompacted(const QList<QString> &entryIds);
    // 检查是否有 ToolCall 条目缺少对应的 ToolResult（会导致 API 400 错误）
    [[nodiscard]] bool hasUnresolvedToolCalls() const;
    [[nodiscard]] ProviderRequestBuild buildRequest(const QList<ProviderToolSpecification> &tools,
                                                    const ProviderOutputSpec &desiredOutput = ProviderOutputSpec::textOnly(),
                                                    const QString &conversationId = {}) const;
    /**
     * 当前可回放上下文占用估算（O(记录数)，无 hydrate / 无 buildRequest）。
     * 可选 overhead：系统提示 + 工具 schema 等请求级固定开销。
     * 若有厂商上一轮 input_tokens，取 max(本地估算, 厂商 input) 作下界，减轻系统性低估。
     */
    [[nodiscard]] qint64 estimatedContextTokens(qint64 requestOverheadTokens = 0) const;
    /// 记录厂商回传的本轮 prompt/input tokens，供后续估算作下界校正
    void noteProviderInputTokens(qint64 inputTokens);
    [[nodiscard]] qint64 lastProviderInputTokens() const { return m_lastProviderInputTokens; }
    [[nodiscard]] QList<ConversationMessage> projectMessages() const;

    [[nodiscard]] QJsonArray toJson() const;
    void fromJson(const QJsonArray &entries);

private:
    struct ProviderRecord {
        ProviderItem item;
        QString entryId;
        bool submitted = false;
        bool compacted = false;
        QString continuationId;
        /// 缓存的 item 估算；-1 表示脏，下次读取时重算。
        /// mutable：const 估算路径可填缓存，不改变可观察语义。
        mutable qint64 tokenEstimate = -1;
    };

    ProviderRecord *findProviderRecord(const QString &entryId);
    const ProviderRecord *findProviderRecord(const QString &entryId) const;
    static void refreshTokenEstimate(ProviderRecord &record);

    QList<ConversationMessage> m_entries;
    QList<ProviderRecord> m_providerRecords;
    qint64 m_lastProviderInputTokens = 0;
};
