#include "ProviderService.h"

#include "providers/core/AbstractProvider.h"
#include "providers/chatcompletions/ChatCompletionsProvider.h"
#include "providers/deepseek/DeepSeekProvider.h"
#include "providers/responses/ResponsesProvider.h"
#include "providers/anthropic/AnthropicProvider.h"
#include "providers/gemini/GeminiProvider.h"
#include "logging/LogManager.h"

#include <utility>

QString ProviderService::normalizeProviderType(const QString &type)
{
    return type.trimmed().toLower();
}

QString ProviderService::providerDisplayName(const QString &type)
{
    const QString normalized = normalizeProviderType(type);
    if (normalized == QStringLiteral("anthropic")) return QStringLiteral("Anthropic");
    if (normalized == QStringLiteral("chat-completions")) return QStringLiteral("ChatCompletions");
    if (normalized == QStringLiteral("deepseek")) return QStringLiteral("DeepSeek");
    if (normalized == QStringLiteral("google")) return QStringLiteral("Gemini");
    if (normalized == QStringLiteral("google-interactions"))
        return QStringLiteral("Gemini Interactions");
    if (normalized == QStringLiteral("responses")) return QStringLiteral("Responses");
    return type.trimmed().isEmpty() ? normalized : type.trimmed();
}

void ProviderService::registerProvider(const QString &type, FactoryFunc factory)
{
    const QString normalized = normalizeProviderType(type);
    if (normalized.isEmpty())
        return;
    m_factories.insert(normalized, std::move(factory));
}

void ProviderService::registerBuiltins()
{
    registerProvider(QStringLiteral("responses"), []() {
        return std::make_unique<ResponsesProvider>();
    });
    registerProvider(QStringLiteral("chat-completions"), []() {
        return std::make_unique<ChatCompletionsProvider>();
    });
    registerProvider(QStringLiteral("deepseek"), []() {
        return std::make_unique<DeepSeekProvider>();
    });
    registerProvider(QStringLiteral("anthropic"), []() {
        return std::make_unique<AnthropicProvider>();
    });
    registerProvider(QStringLiteral("google"), []() {
        return std::make_unique<GeminiProvider>();
    });
    registerProvider(QStringLiteral("google-interactions"), []() {
        return std::make_unique<GeminiProvider>(
            ProviderProtocolFamily::GeminiInteractions);
    });
    // 启动固定清单：降噪为一次 DEBUG 汇总（勿回退成逐种 INFO）
    LOGD(LogCat::System) << "内置 Provider 种类已注册"
                         << logf("types", availableProviderTypes().join(QLatin1Char(',')));
}

std::unique_ptr<AbstractProvider> ProviderService::create(const QString &type, const char *callSite) const
{
    const QString normalized = normalizeProviderType(type);
    const auto it = m_factories.constFind(normalized);
    if (it != m_factories.constEnd() && it.value()) {
        LOGD(LogCat::Provider) << "创建 Provider"
            << logf("type", normalized)
            << logf("callSite", callSite ? QString::fromLatin1(callSite) : QStringLiteral("unknown"));
        return it.value()();
    }
    LOGW(LogCat::Provider) << "未注册的 Provider"
        << logf("type", normalized);
    return {};
}

bool ProviderService::supportsProvider(const QString &type) const
{
    return m_factories.contains(normalizeProviderType(type));
}

QStringList ProviderService::availableProviderTypes() const
{
    // 固定展示顺序，与历史凭据 UI 一致
    static const QStringList kPreferred{
        QStringLiteral("anthropic"),
        QStringLiteral("chat-completions"),
        QStringLiteral("deepseek"),
        QStringLiteral("google"),
        QStringLiteral("google-interactions"),
        QStringLiteral("responses"),
    };
    QStringList ordered;
    ordered.reserve(m_factories.size());
    for (const QString &type : kPreferred) {
        if (m_factories.contains(type))
            ordered.append(type);
    }
    QStringList extras = m_factories.keys();
    extras.sort(Qt::CaseInsensitive);
    for (const QString &type : extras) {
        if (!ordered.contains(type))
            ordered.append(type);
    }
    return ordered;
}
