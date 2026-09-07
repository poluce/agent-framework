#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

/**
 * @file MediaAsset.h
 * @brief 账本与协议共用的多模态引用 / 图片资产（类型层，不依赖 providers/）
 */

enum class ProviderUriScheme {
    Unset,
    Https,
    Http,
    File,
    Data,
    ProviderFile,
    Blob,
};

struct ProviderBlobRef
{
    QString blobId;
    QString contentHash;
    qint64 byteSize = 0;
    qint64 expiresAtMs = 0;
    ProviderUriScheme scheme = ProviderUriScheme::Unset;

    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] bool hasBlobId() const;
};

struct ProviderImageAsset
{
    QString uri;
    QByteArray data;
    QString mimeType;
    QString altText;
    ProviderBlobRef blobRef;

    [[nodiscard]] static ProviderImageAsset fromUrl(const QString &uri,
                                                    const QString &mimeType = {},
                                                    const QString &altText = {});
    [[nodiscard]] static ProviderImageAsset fromBytes(const QByteArray &data,
                                                      const QString &mimeType,
                                                      const QString &altText = {});
    [[nodiscard]] static ProviderImageAsset fromBlob(const ProviderBlobRef &blob,
                                                      const QString &mimeType = {},
                                                      const QString &altText = {});

    [[nodiscard]] bool hasUri() const;
    [[nodiscard]] bool hasInlineData() const;
    [[nodiscard]] bool hasBlobRef() const;
    [[nodiscard]] bool isEmpty() const;
};

[[nodiscard]] inline ProviderBlobRef withDefaultBlobScheme(ProviderBlobRef blob)
{
    if (blob.scheme == ProviderUriScheme::Unset) {
        blob.scheme = ProviderUriScheme::Blob;
    }
    return blob;
}

Q_DECLARE_METATYPE(ProviderBlobRef)
Q_DECLARE_METATYPE(ProviderImageAsset)
