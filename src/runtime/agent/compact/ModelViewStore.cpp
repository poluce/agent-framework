#include "ModelViewStore.h"

#include "agent/ProviderRunLedger.h"

void ModelViewStore::clear()
{
    m_prefixTexts.clear();
}

void ModelViewStore::setPrefixTexts(QList<QString> texts)
{
    m_prefixTexts = std::move(texts);
}

void ModelViewStore::syncFromSummaryStore(const SummaryStore &store)
{
    QList<QString> texts;
    texts.reserve(store.records().size());
    for (const SummaryRecord &rec : store.records()) {
        if (!rec.text.trimmed().isEmpty()) {
            texts.append(rec.text);
        }
    }
    m_prefixTexts = std::move(texts);
}

qint64 ModelViewStore::totalTokenEstimate() const
{
    qint64 total = 0;
    for (const QString &text : m_prefixTexts) {
        total += estimateContextTokensForText(text);
    }
    return total;
}

QJsonObject ModelViewStore::toJson() const
{
    QJsonObject obj;
    QJsonArray arr;
    for (const QString &text : m_prefixTexts) {
        arr.append(text);
    }
    obj.insert(QStringLiteral("prefixTexts"), arr);
    return obj;
}

void ModelViewStore::fromJson(const QJsonObject &obj)
{
    m_prefixTexts.clear();
    const QJsonArray arr = obj.value(QStringLiteral("prefixTexts")).toArray();
    for (const QJsonValue &v : arr) {
        if (!v.isString()) {
            continue;
        }
        const QString text = v.toString();
        if (!text.trimmed().isEmpty()) {
            m_prefixTexts.append(text);
        }
    }
}
