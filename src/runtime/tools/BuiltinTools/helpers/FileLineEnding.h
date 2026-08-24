#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

/**
 * @brief 行尾（CRLF/LF）处理：写工具与 diff 共用。
 *
 * 读原样字节 → normalizeToLf 归一；写时按原文件逐行行尾还原。
 * 避免 QIODevice::Text 在 Windows 上 \n↔\r\n 双重转换（模型手写 \r\n 会变成 \r\r\n）。
 */
namespace FileLineEnding {

/** 把字节流按 \r\n→\n 归一为 QString（UTF-8 解码，真字符而非 Latin-1 码点）。 */
inline QString normalizeToLf(const QByteArray &bytes)
{
    // 先字节级把 \r\n → \n（不碰 ≥0x80 字节，保留原 UTF-8 序列）
    QByteArray lf = bytes;
    lf.replace("\r\n", "\n");
    // 再 UTF-8 解码（真字符；此前逐字节 QLatin1Char 会把中文变成 U+00xx 乱码，
    // 导致 fileContentMatches 对含非 ASCII 的文件乐观校验必然失败）
    return QString::fromUtf8(lf);
}

/**
 * @brief 逐行记录行尾：返回每行末尾的换行串（"\r\n" / "\n" / ""）。
 * 第 i 个元素 = 第 i 行（0-based）之后的换行；末行无换行 → ""。
 * 混合行尾文件保持逐行原样（不做占比判定，避免整文件误转）。
 *
 * 例：`"a\nb\nc\n"` → ["\n","\n","\n"]；`"a\r\nb\nc"` → ["\r\n","\n",""]；`""` → []
 */
inline QList<QString> lineEndingsOf(const QByteArray &bytes)
{
    QList<QString> endings;
    for (int i = 0; i < bytes.size(); ++i) {
        if (bytes.at(i) == '\n') {
            const bool crlf = i > 0 && bytes.at(i - 1) == '\r';
            endings.append(crlf ? QStringLiteral("\r\n") : QStringLiteral("\n"));
        }
    }
    if (!bytes.isEmpty() && !bytes.endsWith('\n'))
        endings.append(QString()); // 末行无换行
    return endings;
}

/**
 * @brief 按行尾数组还原：把 LF 内容逐行接上原行尾。
 * split('\n') 段间有 N-1 个换行；endings 提供 N-1 个原行尾（第 i 段后）。
 * 把段间换行替换为原行尾（CRLF 则 \n 前补 \r）。
 * 例：content="a\nb\nc\n" + ["\r\n","\r\n","\r\n"] → "a\r\nb\r\nc\r\n"
 */
inline QByteArray restoreLineEndings(const QString &lfContent, const QList<QString> &endings)
{
    // 最后一个非空行尾：content 新增行/补尾换行时兜底用（空串是「末行无换行」标记，
    // 不能当真实行尾，否则会把 content 的换行吞掉 → 乐观校验失败）
    QString lastRealEnding = QStringLiteral("\n");
    for (const QString &e : endings) {
        if (!e.isEmpty())
            lastRealEnding = e;
    }

    const QByteArray utf8 = lfContent.toUtf8();
    QByteArray out;
    out.reserve(utf8.size() + endings.size());
    int segment = 0;
    for (int i = 0; i < utf8.size(); ++i) {
        const char c = utf8.at(i);
        if (c == '\n') {
            // 该行尾取原文件对应行尾；空串标记或超出 → 用最后真实行尾
            const QString ending = (segment < endings.size() && !endings.at(segment).isEmpty())
                ? endings.at(segment)
                : lastRealEnding;
            out.append(ending.toUtf8());
            ++segment;
        } else {
            out.append(c);
        }
    }
    return out;
}

} // namespace FileLineEnding
