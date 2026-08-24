#include "SummaryStore.h"

#include <QSet>

QJsonObject SummaryRecord::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("summaryId"), summaryId);
    QJsonArray span;
    for (const QString &id : spanEntryIds) {
        span.append(id);
    }
    obj.insert(QStringLiteral("spanEntryIds"), span);
    obj.insert(QStringLiteral("text"), text);
    obj.insert(QStringLiteral("tokenEstimate"), tokenEstimate);
    obj.insert(QStringLiteral("createdAtMs"), createdAtMs);
    obj.insert(QStringLiteral("source"), source);
    return obj;
}

SummaryRecord SummaryRecord::fromJson(const QJsonObject &obj)
{
    SummaryRecord rec;
    rec.summaryId = obj.value(QStringLiteral("summaryId")).toString();
    const QJsonArray span = obj.value(QStringLiteral("spanEntryIds")).toArray();
    for (const QJsonValue &v : span) {
        if (v.isString()) {
            rec.spanEntryIds.append(v.toString());
        }
    }
    rec.text = obj.value(QStringLiteral("text")).toString();
    rec.tokenEstimate = static_cast<qint64>(obj.value(QStringLiteral("tokenEstimate")).toDouble(0));
    rec.createdAtMs = static_cast<qint64>(obj.value(QStringLiteral("createdAtMs")).toDouble(0));
    rec.source = obj.value(QStringLiteral("source")).toString();
    return rec;
}

void SummaryStore::clear()
{
    m_records.clear();
}

void SummaryStore::append(SummaryRecord record)
{
    if (record.summaryId.isEmpty() || record.text.trimmed().isEmpty()) {
        return;
    }
    m_records.append(std::move(record));
}

void SummaryStore::replaceAll(SummaryRecord bulkRecord)
{
    m_records.clear();
    append(std::move(bulkRecord));
}

qint64 SummaryStore::totalTokenEstimate() const
{
    qint64 total = 0;
    for (const SummaryRecord &r : m_records) {
        total += qMax<qint64>(0, r.tokenEstimate);
    }
    return total;
}

QList<QString> SummaryStore::allCoveredEntryIds() const
{
    QList<QString> ids;
    QSet<QString> seen;
    for (const SummaryRecord &r : m_records) {
        for (const QString &id : r.spanEntryIds) {
            if (seen.contains(id)) {
                continue;
            }
            seen.insert(id);
            ids.append(id);
        }
    }
    return ids;
}

QString SummaryStore::lastCoveredEntryId() const
{
    for (int i = m_records.size() - 1; i >= 0; --i) {
        const QList<QString> &span = m_records.at(i).spanEntryIds;
        if (!span.isEmpty()) {
            return span.last();
        }
    }
    return {};
}

QJsonObject SummaryStore::toJson() const
{
    QJsonObject obj;
    QJsonArray records;
    for (const SummaryRecord &r : m_records) {
        records.append(r.toJson());
    }
    obj.insert(QStringLiteral("records"), records);
    return obj;
}

void SummaryStore::fromJson(const QJsonObject &obj)
{
    m_records.clear();
    const QJsonArray records = obj.value(QStringLiteral("records")).toArray();
    for (const QJsonValue &v : records) {
        if (!v.isObject()) {
            continue;
        }
        append(SummaryRecord::fromJson(v.toObject()));
    }
}
