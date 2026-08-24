#pragma once

#include <QByteArray>
#include <QString>
#include <QStringConverter>

#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

/**
 * @brief 文本编码解码（read_file 的 encoding 参数）。
 *
 * 分层策略（Qt 6 无 ICU 时 QStringDecoder 只支持内置 8 编码，GBK 系不可用）：
 * - UTF-8 / Latin-1：Qt 内置直接转
 * - UTF-16/32 系列：QStringDecoder（内置）
 * - **GBK/GB2312：Windows 原生 MultiByteToWideChar(936)**（代码页 936，任何 Windows 都有）
 * - **GB18030：Windows 原生 MultiByteToWideChar(54936)**（Win10 1803+）
 * - 其他：QStringDecoder 按名尝试（ICU 版 Qt 自动支持）→ isValid 失败报「未知编码」
 * - 非 Windows：GBK 系回落 QStringDecoder 尽力支持
 */
namespace TextEncoding {

/** 解码字节为 QString；失败时 *error 非空并返回空串。 */
inline QString decodeBytes(const QByteArray &bytes, const QString &encodingName, QString *error)
{
    if (error)
        error->clear();
    const QString enc = encodingName.trimmed().toLower();

    QString decoded;
    if (enc.isEmpty() || enc == QStringLiteral("utf-8") || enc == QStringLiteral("utf8")) {
        decoded = QString::fromUtf8(bytes);
    } else if (enc == QStringLiteral("latin-1") || enc == QStringLiteral("latin1")
               || enc == QStringLiteral("iso-8859-1")) {
        decoded = QString::fromLatin1(bytes);
    } else {
#ifdef Q_OS_WIN
        // Windows 原生代码页：GBK/GB2312 → 936；GB18030 → 54936（Win10 1803+）
        UINT codepage = 0;
        if (enc == QStringLiteral("gbk") || enc == QStringLiteral("gb2312") || enc == QStringLiteral("cp936")) {
            codepage = 936;
        } else if (enc == QStringLiteral("gb18030")) {
            codepage = 54936;
        }
        if (codepage != 0) {
            const int len = MultiByteToWideChar(codepage, 0, bytes.constData(), bytes.size(),
                                                nullptr, 0);
            if (len <= 0) {
                if (error)
                    *error = QStringLiteral("编码 %1 解码失败（代码页 %2）").arg(encodingName).arg(codepage);
                return {};
            }
            std::vector<wchar_t> buf(static_cast<std::size_t>(len));
            MultiByteToWideChar(codepage, 0, bytes.constData(), bytes.size(), buf.data(), len);
            decoded = QString::fromWCharArray(buf.data(), len);
        } else
#endif
        {
            // 其他：QStringDecoder 按名尝试（内置 8 种 + ICU 版 Qt 的扩展）
            QStringDecoder decoder(encodingName.toUtf8());
            if (!decoder.isValid()) {
                if (error)
                    *error = QStringLiteral("未知编码: %1").arg(encodingName);
                return {};
            }
            decoded = decoder.decode(bytes);
        }
    }
    return decoded;
}

} // namespace TextEncoding
