#pragma once

#include "types/ConversationMessage.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

/// 摘要库单条（三层中的「摘要库」）
struct SummaryRecord {
    QString summaryId;
    QList<QString> spanEntryIds;
    QString text;
    qint64 tokenEstimate = 0;
    qint64 createdAtMs = 0;
    /// segment | bulk
    QString source;

    [[nodiscard]] QJsonObject toJson() const;
    static SummaryRecord fromJson(const QJsonObject &obj);
};

/**
 * 增量段摘要库：只追加；大压成功后可 clear 或替换。
 * 不参与 UI 过程卡；供边界组装模型短上下文。
 * 可序列化，与账本 toJson 并列落盘/恢复。
 */
class SummaryStore
{
public:
    void clear();
    void append(SummaryRecord record);
    /// 大压后作废全部旧段摘要（随后可再 append bulk）
    void replaceAll(SummaryRecord bulkRecord);

    [[nodiscard]] const QList<SummaryRecord> &records() const { return m_records; }
    [[nodiscard]] bool isEmpty() const { return m_records.isEmpty(); }
    [[nodiscard]] int recordCount() const { return m_records.size(); }
    [[nodiscard]] qint64 totalTokenEstimate() const;
    [[nodiscard]] QList<QString> allCoveredEntryIds() const;
    /// 库中最后一条覆盖的 entry id；空库返回空串
    [[nodiscard]] QString lastCoveredEntryId() const;

    [[nodiscard]] QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    QList<SummaryRecord> m_records;
};
