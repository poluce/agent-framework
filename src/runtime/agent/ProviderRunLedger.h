#pragma once

#include "types/ConversationMessage.h"
#include "tools/BuiltinToolRuntime.h"
#include "providers/ProviderTypes/ProviderTypes.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>



struct ProviderRequestBuild
{
    ProviderRequest request;
    QList<QString> submittedEntryIds;
    /// hydrate 失败时非空；调用方应中止本轮并提示用户。
    QString hydrateError;
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
    /// 设置本账本 blob 存储根目录（建议按会话隔离）；空 = 不读写磁盘。
    void setBlobRoot(const QString &path);
    /// 删除 blobRoot 目录中不在 keep 集合内的文件（宿主兜底用）。
    static void gcProviderBlobs(const QString &blobRoot, const QSet<QString> &keep);

    [[nodiscard]] const QList<ConversationMessage> &entries() const;

    /// UI/工具域入站投影为 ProviderItem；厂商输出必须使用 appendProviderItem。
    QString appendUiIngress(ConversationMessage entry);
    QString appendProviderItem(ProviderItem item,
                               const QString &turnId = {},
                               const QString &continuationId = {});
    /// 更新/补挂 ProviderItem；找不到 entry 返回 false。externalize=false 用于加载恢复（不写盘）。
    bool setProviderItemForEntry(const QString &entryId,
                                 ProviderItem item,
                                 const QString &continuationId = {},
                                 bool externalize = true);
    /// 只清线路记录，保留 UI 投影（空推理不可回放时用）。
    bool clearProviderRecord(const QString &entryId);
    /// 回滚未提交收口的目标 Turn（取消或异常终止时物理抹除脏数据）；有删除返回 true。
    bool rollbackUncommittedTurn(const QString &turnId);
    bool removeEntry(const QString &entryId);
    ConversationMessage *findLatestReasoningForTurn(const QString &turnId);
    const ConversationMessage *findLatestReasoningForTurn(const QString &turnId) const;
    /// 返回 hydrate 后的完整线路项（安全默认）。
    [[nodiscard]] QList<ProviderItem> providerItems() const;
    /// 返回未 hydrate 的线路项（仅内部/序列化使用；data 可能为空，只剩 blobRef）。
    [[nodiscard]] QList<ProviderItem> providerItemsUnhydrated() const;
    ConversationMessage *findById(const QString &entryId);
    const ConversationMessage *findById(const QString &entryId) const;
    ConversationMessage *findToolCallByUseId(const QString &toolUseId);
    const ConversationMessage *findToolCallByUseId(const QString &toolUseId) const;

    /// 标记已提交；有标记返回 true。
    bool markSubmitted(const QList<QString> &entryIds);
    /// 标记已压缩；有标记返回 true。
    bool markEntriesCompacted(const QList<QString> &entryIds);
    // 检查是否有 ToolCall 条目缺少对应的 ToolResult（会导致 API 400 错误）
    [[nodiscard]] bool hasUnresolvedToolCalls() const;
    [[nodiscard]] ProviderRequestBuild buildRequest(const QList<ProviderToolSpecification> &tools,
                                                    const ProviderOutputSpec &desiredOutput = ProviderOutputSpec::textOnly(),
                                                    const QString &conversationId = {}) const;
    /**
     * 当前可回放上下文占用估算（O(记录数)，无 hydrate / 无 buildRequest）。
     * 可选 overhead：系统提示 + 工具 schema 等请求级固定开销。
     * 若有厂商上一轮 input_tokens，取 max(本地估算, 厂商 input) 作下界，减轻系统性低估。
     * 注意：本方法会更新内部 token 估算缓存，因此不是 const。
     */
    [[nodiscard]] qint64 estimatedContextTokens(qint64 requestOverheadTokens = 0);
    /// 记录厂商回传的本轮 prompt/input tokens，供后续估算作下界校正
    void noteProviderInputTokens(qint64 inputTokens);
    [[nodiscard]] qint64 lastProviderInputTokens() const { return m_lastProviderInputTokens; }
    [[nodiscard]] QList<ConversationMessage> projectMessages() const;

    /// 账本当前引用的全部 blobId（供宿主在适当时机做全局 GC）。
    [[nodiscard]] QSet<QString> referencedBlobIds() const;

    /// 版本化信封：{schemaVersion, providerProtocolVersion, providerProtocolRevision, entries}
    [[nodiscard]] QJsonObject toJson() const;
    /// 接受新信封对象或旧裸数组；失败返回 false 且账本保持原状。
    bool fromJson(const QJsonValue &data);

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

    // ── 索引维护 ──
    void rebuildIndexes();
    void indexEntry(const QString &id, qsizetype pos);
    void indexProviderRecord(const QString &entryId, qsizetype pos);
    void indexToolUse(const QString &toolUseId, qsizetype pos);

    // ── 回滚策略 ──
    [[nodiscard]] bool isTurnEntryCommitted(const ConversationMessage &entry,
                                            const QSet<QString> &resolvedToolUseIds) const;

    // ── UI 投影合并 ──
    void mergeProviderProjection(ConversationMessage &entry,
                                 const ProviderItem &item,
                                 const QString &continuationId) const;

    // ── 线路项收集 ──
    [[nodiscard]] QList<ProviderItem> collectProviderItems(bool hydrate) const;
    [[nodiscard]] QList<ProviderItem> hydrateItemsForRequest(QList<ProviderItem> items,
                                                             QString *error) const;

    QList<ConversationMessage> m_entries;
    QList<ProviderRecord> m_providerRecords;
    QHash<QString, qsizetype> m_entryIndex;
    QHash<QString, qsizetype> m_providerIndex;
    QHash<QString, qsizetype> m_toolUseIndex;
    QString m_blobRoot;
    qint64 m_lastProviderInputTokens = 0;
};
