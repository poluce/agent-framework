#pragma once

#include "../chatcompletions/ChatCompletionsProvider.h"

#include <optional>

class DeepSeekProvider final : public ChatCompletionsProvider
{
    Q_OBJECT
    friend class ProviderAdapterFixtureTests;

public:
    explicit DeepSeekProvider(QObject *parent = nullptr);
    ~DeepSeekProvider() override;

    void setAuth(const ProviderAuth &auth) override;

protected:
    // 重写通用聊天接口的行为，加入 DeepSeek 特有参数与特有字段
    [[nodiscard]] ProviderError validateProviderRequest(const ProviderRequest &request) const override;
    [[nodiscard]] QJsonObject buildRequestBody(const ProviderRequest &request) const override;
    [[nodiscard]] QJsonObject buildAssistantMessageForToolCall(const ProviderItem &item, const ProviderRequest &request) const override;
    [[nodiscard]] QList<ProviderEvent> handleDeltaContent(const QJsonObject &delta) override;
};
