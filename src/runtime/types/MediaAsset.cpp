#include "MediaAsset.h"

bool ProviderBlobRef::isEmpty() const
{
    return blobId.trimmed().isEmpty()
           && contentHash.trimmed().isEmpty()
           && byteSize <= 0
           && scheme == ProviderUriScheme::Unset;
}

bool ProviderBlobRef::hasBlobId() const
{
    return !blobId.trimmed().isEmpty();
}

ProviderImageAsset ProviderImageAsset::fromUrl(const QString &uri,
                                               const QString &mimeType,
                                               const QString &altText)
{
    ProviderImageAsset asset;
    asset.uri = uri;
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

ProviderImageAsset ProviderImageAsset::fromBytes(const QByteArray &data,
                                                 const QString &mimeType,
                                                 const QString &altText)
{
    ProviderImageAsset asset;
    asset.data = data;
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

ProviderImageAsset ProviderImageAsset::fromBlob(const ProviderBlobRef &blob,
                                                const QString &mimeType,
                                                const QString &altText)
{
    ProviderImageAsset asset;
    asset.blobRef = withDefaultBlobScheme(blob);
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

bool ProviderImageAsset::hasUri() const
{
    return !uri.trimmed().isEmpty();
}

bool ProviderImageAsset::hasInlineData() const
{
    return !data.isEmpty();
}

bool ProviderImageAsset::hasBlobRef() const
{
    return blobRef.hasBlobId();
}

bool ProviderImageAsset::isEmpty() const
{
    return !hasUri() && !hasInlineData() && !hasBlobRef();
}
