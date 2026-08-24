#pragma once

#include <QFileInfo>
#include <QHash>
#include <QString>

/**
 * @brief 文件类型过滤（glob / grep 共用）。
 *
 * 从 GrepTool 旧 matchesTypeFilter 原样迁移（行为零变化）：
 * c/cpp/h/hh/hpp/hxx → cpp；ts/tsx → ts；js/jsx → js；py/qml/json/md/txt 各自；
 * 未知后缀 == 小写后缀本身。空 typeFilter 全部放行。
 */
namespace FileTypeFilter {

inline bool matches(const QString &absoluteFilePath, const QString &typeFilter)
{
    const QString normalized = typeFilter.trimmed().toLower();
    if (normalized.isEmpty())
        return true;

    const QString suffix = QFileInfo(absoluteFilePath).suffix().toLower();
    // 后缀别名表：同族后缀归一为同一 type；不在此表时按后缀原名匹配
    const QHash<QString, QString> aliasTable{
        {QStringLiteral("c"),    QStringLiteral("cpp")},
        {QStringLiteral("cc"),   QStringLiteral("cpp")},
        {QStringLiteral("cpp"),  QStringLiteral("cpp")},
        {QStringLiteral("cxx"),  QStringLiteral("cpp")},
        {QStringLiteral("h"),    QStringLiteral("cpp")},
        {QStringLiteral("hh"),   QStringLiteral("cpp")},
        {QStringLiteral("hpp"),  QStringLiteral("cpp")},
        {QStringLiteral("hxx"),  QStringLiteral("cpp")},
        {QStringLiteral("js"),   QStringLiteral("js")},
        {QStringLiteral("jsx"),  QStringLiteral("js")},
        {QStringLiteral("ts"),   QStringLiteral("ts")},
        {QStringLiteral("tsx"),  QStringLiteral("ts")},
    };
    return aliasTable.value(suffix, suffix) == normalized;
}

/** spec 的 type 参数共用描述（glob/grep 措辞一致）。 */
inline QString typeFilterDescription()
{
    return QStringLiteral("按文件类型过滤（cpp/ts/js/py/qml/json/md/txt 等后缀别名），可为空");
}

} // namespace FileTypeFilter
