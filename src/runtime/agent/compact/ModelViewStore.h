#pragma once

#include "SummaryStore.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

/**
 * 三层中的「模型短上下文（视图）」：摘要链前缀 SSOT。
 * 不写账本；由 Agent 从 SummaryStore 同步，Loop 请求时注入。
 */
class ModelViewStore
{
public:
    void clear();
    void setPrefixTexts(QList<QString> texts);
    /// 用摘要库正文链覆盖前缀（空库 → 清空）
    void syncFromSummaryStore(const SummaryStore &store);

    [[nodiscard]] const QList<QString> &prefixTexts() const { return m_prefixTexts; }
    [[nodiscard]] bool isEmpty() const { return m_prefixTexts.isEmpty(); }
    [[nodiscard]] int count() const { return m_prefixTexts.size(); }
    [[nodiscard]] qint64 totalTokenEstimate() const;

    [[nodiscard]] QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

private:
    QList<QString> m_prefixTexts;
};
