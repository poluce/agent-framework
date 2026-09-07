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

ProviderAudioAsset ProviderAudioAsset::fromUrl(const QString &uri,
                                               const QString &mimeType,
                                               const QString &transcript)
{
    ProviderAudioAsset asset;
    asset.uri = uri;
    asset.mimeType = mimeType;
    asset.transcript = transcript;
    return asset;
}

ProviderAudioAsset ProviderAudioAsset::fromBytes(const QByteArray &data,
                                                 const QString &mimeType,
                                                 const QString &transcript)
{
    ProviderAudioAsset asset;
    asset.data = data;
    asset.mimeType = mimeType;
    asset.transcript = transcript;
    return asset;
}

ProviderAudioAsset ProviderAudioAsset::fromBlob(const ProviderBlobRef &blob,
                                                const QString &mimeType,
                                                const QString &transcript)
{
    ProviderAudioAsset asset;
    asset.blobRef = withDefaultBlobScheme(blob);
    asset.mimeType = mimeType;
    asset.transcript = transcript;
    return asset;
}

bool ProviderAudioAsset::hasUri() const
{
    return !uri.trimmed().isEmpty();
}

bool ProviderAudioAsset::hasInlineData() const
{
    return !data.isEmpty();
}

bool ProviderAudioAsset::hasBlobRef() const
{
    return blobRef.hasBlobId();
}

bool ProviderAudioAsset::isEmpty() const
{
    return !hasUri() && !hasInlineData() && !hasBlobRef();
}

ProviderVideoAsset ProviderVideoAsset::fromUrl(const QString &uri,
                                               const QString &mimeType,
                                               const QString &altText)
{
    ProviderVideoAsset asset;
    asset.uri = uri;
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

ProviderVideoAsset ProviderVideoAsset::fromBytes(const QByteArray &data,
                                                 const QString &mimeType,
                                                 const QString &altText)
{
    ProviderVideoAsset asset;
    asset.data = data;
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

ProviderVideoAsset ProviderVideoAsset::fromBlob(const ProviderBlobRef &blob,
                                                const QString &mimeType,
                                                const QString &altText)
{
    ProviderVideoAsset asset;
    asset.blobRef = withDefaultBlobScheme(blob);
    asset.mimeType = mimeType;
    asset.altText = altText;
    return asset;
}

bool ProviderVideoAsset::hasUri() const
{
    return !uri.trimmed().isEmpty();
}

bool ProviderVideoAsset::hasInlineData() const
{
    return !data.isEmpty();
}

bool ProviderVideoAsset::hasBlobRef() const
{
    return blobRef.hasBlobId();
}

bool ProviderVideoAsset::isEmpty() const
{
    return !hasUri() && !hasInlineData() && !hasBlobRef();
}
