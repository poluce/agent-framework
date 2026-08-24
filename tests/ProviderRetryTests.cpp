#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>

#include "agent/AbstractLoop.h"
#include "providers/service/ProviderCredential.h"
#include "providers/core/AbstractProvider.h"
#include "providers/core/ProviderRetryPolicy.h"
#include "providers/ProviderTypes/ProviderTypes.h"
#include "providers/ProviderTypes/ProviderAdapterTypes.h"
#include "providers/anthropic/AnthropicProvider.h"
#include "providers/chatcompletions/ChatCompletionsProvider.h"
#include "providers/gemini/GeminiProvider.h"
#include "providers/responses/ResponsesProvider.h"

class ProviderRetryTests final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // 纯分类 / 退避
    void classifyTransientStatusCodes();
    void classifyPermanentStatusCodes();
    void classifyConnectionLevel();
    void classifyApiErrorObjects();
    void retryAfterParsing();
    void backoffBounds();

    // Provider 引擎
    void retriesUntilBudgetExhausted();
    void respectsRetryAfter();
    void retryAfterOverCapFallsBackToBackoff();
    void retryStartFailureEmitsFinalError();
    void nonRetryableEmitsImmediately();
    void retryOffByDefault();
    void cancelStopsBackoff();
    void streamStartedFailsThrough();
    void unCachedRequestNeverSwallowed();

    // 厂商 SSE error → emitErrorOccurred + retryable
    void anthropicOverloadedIsRetryable();
    void anthropicOtherErrorNotRetryable();
    void geminiExhaustedIsRetryable();
    void geminiOtherStatusNotRetryable();
    void responsesErrorGoesThroughEmitter();
    void responsesServerErrorIsRetryable();
    void chatCompletionsHttp200ErrorEmits();
    void chatCompletionsServerErrorIsRetryable();
    void chatCompletionsErrorThenFinishedDoesNotComplete();
    void chatCompletionsEmptyFinishedFailsTurn();

    // Loop 回归
    void retryableBeforeFirstByteKeepsTurnAlive();
    void exhaustedBudgetFailsTurn();
    void retryLastFailedTurnReusesUserMessage();

private:
    struct VendorParseResult {
        QList<ProviderEvent> events;
        QList<ProviderError> errors;
    };

    // friend 限定具体类名，经基类引用访问 protected 不生效
    template <typename ProviderT>
    VendorParseResult parseVendorPayload(ProviderT &provider, const QJsonObject &doc);
};

void ProviderRetryTests::initTestCase()
{
}

// ── 组 1：纯分类 / 退避 ──────────────────────────────────────

void ProviderRetryTests::classifyTransientStatusCodes()
{
    using ProviderRetry::classifyHttpStatus;

    const auto rateLimited = classifyHttpStatus(ProviderRetry::kStatusTooManyRequests, "5",
                                                ProviderRetry::kMaxRetryAfterMs);
    QVERIFY(rateLimited.retryable);
    QCOMPARE(rateLimited.retryAfterMs, 5000);

    const auto rateLimitedNoHeader = classifyHttpStatus(ProviderRetry::kStatusTooManyRequests, "",
                                                        ProviderRetry::kMaxRetryAfterMs);
    QVERIFY(rateLimitedNoHeader.retryable);
    QCOMPARE(rateLimitedNoHeader.retryAfterMs, -1);

    for (const int status : {ProviderRetry::kStatusServerErrorMin, 502, 503, 504}) {
        const auto cls = classifyHttpStatus(status, "", ProviderRetry::kMaxRetryAfterMs);
        QVERIFY2(cls.retryable, qPrintable(QStringLiteral("status %1 应可重试").arg(status)));
    }

    QVERIFY(classifyHttpStatus(ProviderRetry::kStatusRequestTimeout, "", ProviderRetry::kMaxRetryAfterMs).retryable);
    QVERIFY(classifyHttpStatus(ProviderRetry::kStatusConflict, "", ProviderRetry::kMaxRetryAfterMs).retryable);
    QVERIFY(classifyHttpStatus(ProviderRetry::kStatusOverloaded, "", ProviderRetry::kMaxRetryAfterMs).retryable);
}

void ProviderRetryTests::classifyPermanentStatusCodes()
{
    using ProviderRetry::classifyHttpStatus;

    for (const int status : {ProviderRetry::kStatusBadRequest, ProviderRetry::kStatusUnauthorized,
                             ProviderRetry::kStatusForbidden, ProviderRetry::kStatusNotFound,
                             ProviderRetry::kStatusUnprocessable}) {
        const auto cls = classifyHttpStatus(status, "5", ProviderRetry::kMaxRetryAfterMs);
        QVERIFY2(!cls.retryable, qPrintable(QStringLiteral("status %1 不应可重试").arg(status)));
        QCOMPARE(cls.retryAfterMs, -1);
    }
}

void ProviderRetryTests::classifyConnectionLevel()
{
    const auto cls = ProviderRetry::classifyHttpStatus(0, "", ProviderRetry::kMaxRetryAfterMs);
    QVERIFY(cls.retryable);
    QCOMPARE(cls.retryAfterMs, -1);
}

void ProviderRetryTests::classifyApiErrorObjects()
{
    using ProviderRetry::classifyApiErrorObject;
    using ProviderRetry::classifyApiErrorValue;

    QVERIFY(classifyApiErrorObject(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("server_error")}}).retryable);
    QVERIFY(classifyApiErrorObject(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("overloaded_error")}}).retryable);
    QVERIFY(classifyApiErrorObject(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("rate_limit_error")}}).retryable);
    QVERIFY(classifyApiErrorObject(QJsonObject{
        {QStringLiteral("code"), QStringLiteral("rate_limit_exceeded")}}).retryable);
    QVERIFY(classifyApiErrorObject(QJsonObject{
        {QStringLiteral("status"), QStringLiteral("RESOURCE_EXHAUSTED")}}).retryable);
    QVERIFY(classifyApiErrorObject(QJsonObject{
        {QStringLiteral("status"), QStringLiteral("UNAVAILABLE")}}).retryable);
    QVERIFY(classifyApiErrorObject(QJsonObject{
        {QStringLiteral("code"), 503}}).retryable);

    QVERIFY(!classifyApiErrorObject(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("invalid_request_error")}}).retryable);
    QVERIFY(!classifyApiErrorObject(QJsonObject{
        {QStringLiteral("code"), 401}}).retryable);
    QVERIFY(!classifyApiErrorObject(QJsonObject{}).retryable);

    QVERIFY(classifyApiErrorValue(QJsonValue(QStringLiteral("Service Unavailable"))).retryable);
    QVERIFY(!classifyApiErrorValue(QJsonValue(QStringLiteral("invalid api key"))).retryable);
}

void ProviderRetryTests::retryAfterParsing()
{
    using ProviderRetry::parseRetryAfter;
    const int maxMs = ProviderRetry::kMaxRetryAfterMs;

    QCOMPARE(parseRetryAfter("5", maxMs), 5000);
    QCOMPARE(parseRetryAfter("0", maxMs), 0);
    QCOMPARE(parseRetryAfter(" 3.5 ", maxMs), 3500);

    QCOMPARE(parseRetryAfter("2500ms", maxMs), 2500);
    QCOMPARE(parseRetryAfter("1500 MS", maxMs), 1500);

    QCOMPARE(parseRetryAfter("120", maxMs), -1);
    QCOMPARE(parseRetryAfter("61000ms", maxMs), -1);

    QCOMPARE(parseRetryAfter("", maxMs), -1);
    QCOMPARE(parseRetryAfter("abc", maxMs), -1);
    QCOMPARE(parseRetryAfter("-5", maxMs), -1);
}

void ProviderRetryTests::backoffBounds()
{
    using ProviderRetry::backoffDelayMs;

    // jitter 0.75–1.0：delay ∈ [floor(0.75*base), base]
    for (int attempt = 0; attempt < 10; ++attempt) {
        const int base = qMin(ProviderRetry::kBackoffBaseMs * (1 << qMin(attempt, 20)),
                              ProviderRetry::kBackoffMaxMs);
        const int delay = backoffDelayMs(attempt);
        QVERIFY2(delay >= (base * 3) / 4 && delay <= base,
                 qPrintable(QStringLiteral("attempt %1 delay %2 应在 [%3, %4]")
                                .arg(attempt).arg(delay).arg(base * 3 / 4).arg(base)));
    }

    QVERIFY(backoffDelayMs(10) <= ProviderRetry::kBackoffMaxMs);
}

// ── 组 2：FakeProvider 引擎 ──────────────────────────────────

namespace {

class FakeProvider final : public AbstractProvider
{
public:
    FakeProvider()
        : AbstractProvider(QStringLiteral("fake"), nullptr)
    {
    }

    int startCount = 0;
    bool failStart = false;

    void driveError(const ProviderError &error) { emitErrorOccurred(error); }
    void driveTextDelta(const QString &text)
    {
        ProviderTextDelta delta;
        delta.base.messageId = QStringLiteral("msg-1");
        delta.text = text;
        emitTextDelta(delta);
    }

protected:
    ProviderError validateProviderRequest(const ProviderRequest &) const override { return {}; }
    ProviderTransportRequest buildProviderTransportRequest(const ProviderRequest &) const override
    {
        ProviderTransportRequest t;
        t.body = "{}";
        return t;
    }
    QList<ProviderEvent> parseProviderTransportPayload(const ProviderTransportPayload &) override
    {
        return {};
    }
    void resetProviderTurnState() override {}
    bool startProviderTransportRequest(const ProviderTransportRequest &, ProviderError *error) override
    {
        ++startCount;
        if (failStart) {
            if (error) {
                error->code = QStringLiteral("transport_start_failed");
                error->message = QStringLiteral("启动失败");
            }
            return false;
        }
        return true;
    }
    QUrl buildModelsUrl(const QString &) const override { return {}; }
    QList<ModelCapabilities> parseModelsPayload(const QByteArray &, QString *) const override
    {
        return {};
    }
};

ProviderRequest makeRequest()
{
    ProviderRequest request;
    request.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.items = {ProviderItem::makeUserText(QStringLiteral("你好"))};
    return request;
}

ProviderError transientError(const QString &code = QStringLiteral("provider_network_error"),
                             const int retryAfterMs = -1)
{
    ProviderError error;
    error.code = code;
    error.message = QStringLiteral("Service is too busy（HTTP 503）");
    error.retryable = true;
    error.retryAfterMs = retryAfterMs;
    return error;
}

struct ErrorSpy {
    explicit ErrorSpy(AbstractProvider *provider)
    {
        QObject::connect(provider, &AbstractProvider::eventEmitted,
                         &captured, [this](const ProviderEvent &event) {
            if (event.kind == ProviderEventKind::Error)
                errors.append(event.error);
            else if (event.kind == ProviderEventKind::Cancelled)
                cancels.append(event);
        });
    }
    QObject captured;
    QList<ProviderError> errors;
    QList<ProviderEvent> cancels;
};

} // namespace

void ProviderRetryTests::retriesUntilBudgetExhausted()
{
    FakeProvider provider;
    ErrorSpy spy(&provider);
    provider.seedAvailableModels({});
    provider.setRetryPolicy(ProviderRetryPolicy{/*maxRetries=*/2});
    provider.sendRequestWithoutModelRefresh(makeRequest());
    QCOMPARE(provider.startCount, 1);

    provider.driveError(transientError());
    QTRY_VERIFY_WITH_TIMEOUT(provider.startCount >= 2, 5000);
    QCOMPARE(spy.errors.size(), 0);

    provider.driveError(transientError());
    QTRY_VERIFY_WITH_TIMEOUT(provider.startCount >= 3, 5000);
    QCOMPARE(spy.errors.size(), 0);

    // 预算耗尽：最终 Error，attempts==2
    provider.driveError(transientError());
    QTRY_COMPARE_WITH_TIMEOUT(spy.errors.size(), 1, 5000);
    QCOMPARE(spy.errors.first().attempts, 2);
    QCOMPARE(spy.cancels.size(), 0);
}

void ProviderRetryTests::respectsRetryAfter()
{
    FakeProvider provider;
    ErrorSpy spy(&provider);
    provider.seedAvailableModels({});
    provider.setRetryPolicy(ProviderRetryPolicy{/*maxRetries=*/1});
    provider.sendRequestWithoutModelRefresh(makeRequest());
    QCOMPARE(provider.startCount, 1);

    provider.driveError(transientError(QStringLiteral("responses_error"), 200));
    QTest::qWait(100);
    QCOMPARE(provider.startCount, 1); // 未到 200ms
    QTRY_VERIFY_WITH_TIMEOUT(provider.startCount >= 2, 5000);
    QCOMPARE(spy.errors.size(), 0);
}

void ProviderRetryTests::retryAfterOverCapFallsBackToBackoff()
{
    FakeProvider provider;
    ErrorSpy spy(&provider);
    provider.seedAvailableModels({});
    provider.setRetryPolicy(ProviderRetryPolicy{/*maxRetries=*/1});
    provider.sendRequestWithoutModelRefresh(makeRequest());
    QCOMPARE(provider.startCount, 1);

    // 超上限 → 忽略，回退避（500ms 起步）
    provider.driveError(transientError(QStringLiteral("responses_error"), 120'000));
    QTest::qWait(100);
    QCOMPARE(provider.startCount, 1);
    QTRY_VERIFY_WITH_TIMEOUT(provider.startCount >= 2, 5000);
    QCOMPARE(spy.errors.size(), 0);
}

void ProviderRetryTests::retryStartFailureEmitsFinalError()
{
    FakeProvider provider;
    ErrorSpy spy(&provider);
    provider.seedAvailableModels({});
    provider.setRetryPolicy(ProviderRetryPolicy{/*maxRetries=*/2});
    provider.sendRequestWithoutModelRefresh(makeRequest());
    QCOMPARE(provider.startCount, 1);

    provider.failStart = true;
    provider.driveError(transientError());

    QTRY_VERIFY_WITH_TIMEOUT(provider.startCount >= 2, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(spy.errors.size(), 1, 5000);
    QCOMPARE(spy.errors.first().code, QStringLiteral("transport_start_failed"));
}

void ProviderRetryTests::nonRetryableEmitsImmediately()
{
    FakeProvider provider;
    ErrorSpy spy(&provider);
    provider.seedAvailableModels({});
    provider.setRetryPolicy(ProviderRetryPolicy{/*maxRetries=*/5});
    provider.sendRequestWithoutModelRefresh(makeRequest());

    ProviderError authError;
    authError.code = QStringLiteral("provider_network_error");
    authError.message = QStringLiteral("Unauthorized（HTTP 401）");
    authError.retryable = false;
    provider.driveError(authError);

    QCOMPARE(spy.errors.size(), 1);
    QCOMPARE(provider.startCount, 1);
}

void ProviderRetryTests::retryOffByDefault()
{
    FakeProvider provider;
    ErrorSpy spy(&provider);
    provider.seedAvailableModels({});
    provider.sendRequestWithoutModelRefresh(makeRequest());

    provider.driveError(transientError());
    QCOMPARE(spy.errors.size(), 1);
    QCOMPARE(provider.startCount, 1);
}

void ProviderRetryTests::cancelStopsBackoff()
{
    FakeProvider provider;
    ErrorSpy spy(&provider);
    provider.seedAvailableModels({});
    provider.setRetryPolicy(ProviderRetryPolicy{/*maxRetries=*/5});
    provider.sendRequestWithoutModelRefresh(makeRequest());

    provider.driveError(transientError());
    QTest::qWait(50);
    provider.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(spy.cancels.size(), 1, 5000);
    QCOMPARE(spy.errors.size(), 0);
    QTest::qWait(600);
    QCOMPARE(provider.startCount, 1);
}

void ProviderRetryTests::streamStartedFailsThrough()
{
    FakeProvider provider;
    ErrorSpy spy(&provider);
    provider.seedAvailableModels({});
    provider.setRetryPolicy(ProviderRetryPolicy{/*maxRetries=*/5});
    provider.sendRequestWithoutModelRefresh(makeRequest());

    // 首字节已出 → 流中失败不拦截
    provider.driveTextDelta(QStringLiteral("你好"));
    QCOMPARE(spy.errors.size(), 0);
    provider.driveError(transientError());
    QCOMPARE(spy.errors.size(), 1);
    QCOMPARE(provider.startCount, 1);
}

void ProviderRetryTests::unCachedRequestNeverSwallowed()
{
    FakeProvider provider;
    ErrorSpy spy(&provider);
    provider.setRetryPolicy(ProviderRetryPolicy{/*maxRetries=*/5});

    // 未 sendRequest → 禁止吞错
    provider.driveError(transientError());
    QCOMPARE(spy.errors.size(), 1);
    QTest::qWait(600);
    QCOMPARE(provider.startCount, 0);
}

// ── 组 2b：厂商 SSE 流内错误 ─────────────────────────────────

namespace {

QJsonObject anthropicErrorDoc(const QString &type, const QString &message)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("error")},
        {QStringLiteral("error"),
         QJsonObject{{QStringLiteral("type"), type},
                     {QStringLiteral("message"), message}}}};
}

QJsonObject geminiErrorDoc(const QString &status, const QString &message)
{
    return QJsonObject{
        {QStringLiteral("error"),
         QJsonObject{{QStringLiteral("code"), 8},
                     {QStringLiteral("status"), status},
                     {QStringLiteral("message"), message}}}};
}

} // namespace

template <typename ProviderT>
ProviderRetryTests::VendorParseResult
ProviderRetryTests::parseVendorPayload(ProviderT &provider, const QJsonObject &doc)
{
    VendorParseResult result;
    QObject::connect(&provider, &AbstractProvider::eventEmitted,
                     &provider, [&result](const ProviderEvent &event) {
        if (event.kind == ProviderEventKind::Error)
            result.errors.append(event.error);
    });
    result.events = provider.parseProviderTransportPayload(ProviderTransportPayload{doc});
    return result;
}

void ProviderRetryTests::anthropicOverloadedIsRetryable()
{
    AnthropicProvider provider;
    provider.setAuth(ProviderAuth{QStringLiteral("https://api.anthropic.com"),
                                  QStringLiteral("key"), QStringLiteral("claude-sonnet-4-5")});
    const auto result = parseVendorPayload(
        provider, anthropicErrorDoc(QStringLiteral("overloaded_error"),
                                    QStringLiteral("Overloaded")));
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(result.errors.first().retryable);
}

void ProviderRetryTests::anthropicOtherErrorNotRetryable()
{
    AnthropicProvider provider;
    provider.setAuth(ProviderAuth{QStringLiteral("https://api.anthropic.com"),
                                  QStringLiteral("key"), QStringLiteral("claude-sonnet-4-5")});
    const auto result = parseVendorPayload(
        provider, anthropicErrorDoc(QStringLiteral("invalid_request_error"),
                                    QStringLiteral("Bad request")));
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(!result.errors.first().retryable);
}

void ProviderRetryTests::geminiExhaustedIsRetryable()
{
    // 每例独立实例：terminal 门一次 turn 内吞后续 Error
    {
        GeminiProvider provider;
        provider.setAuth(ProviderAuth{QStringLiteral("https://generativelanguage.googleapis.com"),
                                      QStringLiteral("key"), QStringLiteral("gemini-2.5-pro")});
        const auto result = parseVendorPayload(
            provider, geminiErrorDoc(QStringLiteral("RESOURCE_EXHAUSTED"),
                                     QStringLiteral("Rate limit")));
        QCOMPARE(result.errors.size(), 1);
        QVERIFY(result.errors.first().retryable);
    }
    {
        GeminiProvider provider;
        provider.setAuth(ProviderAuth{QStringLiteral("https://generativelanguage.googleapis.com"),
                                      QStringLiteral("key"), QStringLiteral("gemini-2.5-pro")});
        const auto unavailable = parseVendorPayload(
            provider, geminiErrorDoc(QStringLiteral("UNAVAILABLE"),
                                     QStringLiteral("Service unavailable")));
        QCOMPARE(unavailable.errors.size(), 1);
        QVERIFY(unavailable.errors.first().retryable);
    }
}

void ProviderRetryTests::geminiOtherStatusNotRetryable()
{
    GeminiProvider provider;
    provider.setAuth(ProviderAuth{QStringLiteral("https://generativelanguage.googleapis.com"),
                                  QStringLiteral("key"), QStringLiteral("gemini-2.5-pro")});
    const auto result = parseVendorPayload(
        provider, geminiErrorDoc(QStringLiteral("INVALID_ARGUMENT"),
                                 QStringLiteral("Bad argument")));
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(!result.errors.first().retryable);
}

void ProviderRetryTests::responsesErrorGoesThroughEmitter()
{
    ResponsesProvider provider;
    provider.setAuth(ProviderAuth{QStringLiteral("https://api.openai.com"),
                                  QStringLiteral("key"), QStringLiteral("gpt-5")});
    const auto result = parseVendorPayload(
        provider, QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                              {QStringLiteral("error"),
                               QJsonObject{{QStringLiteral("message"),
                                            QStringLiteral("boom")}}}});
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.events.size(), 0); // 错误经 emit，parse 不再直接返回
    QVERIFY(!result.errors.first().retryable);
}

void ProviderRetryTests::responsesServerErrorIsRetryable()
{
    ResponsesProvider provider;
    provider.setAuth(ProviderAuth{QStringLiteral("https://api.openai.com"),
                                  QStringLiteral("key"), QStringLiteral("gpt-5")});
    const auto result = parseVendorPayload(
        provider, QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                              {QStringLiteral("error"),
                               QJsonObject{{QStringLiteral("type"),
                                            QStringLiteral("server_error")},
                                           {QStringLiteral("message"),
                                            QStringLiteral("The server had an error")}}}});
    QCOMPARE(result.errors.size(), 1);
    QVERIFY(result.errors.first().retryable);
}

void ProviderRetryTests::chatCompletionsHttp200ErrorEmits()
{
    ChatCompletionsProvider provider;
    provider.setAuth(ProviderAuth{QStringLiteral("https://api.openai.com"),
                                  QStringLiteral("key"), QStringLiteral("gpt-4o-mini")});
    const auto result = parseVendorPayload(
        provider, QJsonObject{{QStringLiteral("error"),
                               QJsonObject{{QStringLiteral("message"),
                                            QStringLiteral("That model is currently overloaded")},
                                           {QStringLiteral("type"),
                                            QStringLiteral("server_error")},
                                           {QStringLiteral("code"),
                                            QStringLiteral("overloaded")}}}});
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.events.size(), 0);
    QCOMPARE(result.errors.first().code, QStringLiteral("chat_completions_error"));
    QVERIFY(result.errors.first().message.contains(QStringLiteral("overloaded")));
    QVERIFY(result.errors.first().retryable);
}

void ProviderRetryTests::chatCompletionsServerErrorIsRetryable()
{
    ChatCompletionsProvider provider;
    provider.setAuth(ProviderAuth{QStringLiteral("https://api.openai.com"),
                                  QStringLiteral("key"), QStringLiteral("gpt-4o-mini")});
    {
        const auto retryable = parseVendorPayload(
            provider, QJsonObject{{QStringLiteral("error"),
                                   QJsonObject{{QStringLiteral("type"),
                                                QStringLiteral("server_error")},
                                               {QStringLiteral("message"),
                                                QStringLiteral("internal")}}}});
        QCOMPARE(retryable.errors.size(), 1);
        QVERIFY(retryable.errors.first().retryable);
    }
    {
        ChatCompletionsProvider authProvider;
        authProvider.setAuth(ProviderAuth{QStringLiteral("https://api.openai.com"),
                                          QStringLiteral("key"), QStringLiteral("gpt-4o-mini")});
        const auto permanent = parseVendorPayload(
            authProvider, QJsonObject{{QStringLiteral("error"),
                                       QJsonObject{{QStringLiteral("type"),
                                                    QStringLiteral("invalid_request_error")},
                                                   {QStringLiteral("message"),
                                                    QStringLiteral("missing model")}}}});
        QCOMPARE(permanent.errors.size(), 1);
        QVERIFY(!permanent.errors.first().retryable);
    }
}

void ProviderRetryTests::chatCompletionsErrorThenFinishedDoesNotComplete()
{
    ChatCompletionsProvider provider;
    provider.setAuth(ProviderAuth{QStringLiteral("https://api.openai.com"),
                                  QStringLiteral("key"), QStringLiteral("gpt-4o-mini")});
    QList<ProviderEventKind> kinds;
    QObject::connect(&provider, &AbstractProvider::eventEmitted,
                     &provider, [&kinds](const ProviderEvent &event) {
        kinds.append(event.kind);
    });

    // 先吃到正文，再收到 HTTP 200 error JSON；finished 不得再合成 Completed。
    ProviderTransportPayload textPayload;
    textPayload.document = QJsonObject{
        {QStringLiteral("id"), QStringLiteral("chatcmpl-1")},
        {QStringLiteral("choices"),
         QJsonArray{QJsonObject{
             {QStringLiteral("delta"),
              QJsonObject{{QStringLiteral("content"), QStringLiteral("半句")}}}}}}};
    provider.processProviderPayload(textPayload);
    QVERIFY(kinds.contains(ProviderEventKind::TextDelta));

    ProviderTransportPayload errorPayload;
    errorPayload.document = QJsonObject{
        {QStringLiteral("error"),
         QJsonObject{{QStringLiteral("message"), QStringLiteral("stream aborted")},
                     {QStringLiteral("type"), QStringLiteral("server_error")}}}};
    const auto parsed = provider.parseProviderTransportPayload(errorPayload);
    QCOMPARE(parsed.size(), 0);
    provider.handleTransportFinished();

    QVERIFY(kinds.contains(ProviderEventKind::Error));
    QVERIFY(!kinds.contains(ProviderEventKind::MessageCompleted));
}

void ProviderRetryTests::chatCompletionsEmptyFinishedFailsTurn()
{
    ChatCompletionsProvider provider;
    provider.setAuth(ProviderAuth{QStringLiteral("https://api.openai.com"),
                                  QStringLiteral("key"), QStringLiteral("gpt-4o-mini")});
    ErrorSpy spy(&provider);
    provider.handleTransportFinished();
    QCOMPARE(spy.errors.size(), 1);
    QCOMPARE(spy.errors.first().code, QStringLiteral("chat_completions_empty_response"));
}

// ── 组 3：Loop 回归 ──────────────────────────────────────────

namespace {

/// start 只计数；经 QTimer 异步 drive 瞬时失败，模拟真实 failed 信号
class LoopFakeProvider final : public AbstractProvider
{
public:
    LoopFakeProvider()
        : AbstractProvider(QStringLiteral("anthropic"), nullptr)
    {
    }

    int startCount = 0;
    bool failWithRetryable = false;
    int failTimes = 0;
    bool failOnceNonRetryable = false;

    void driveSuccess()
    {
        ProviderMessageEnd end;
        end.messageId = QStringLiteral("msg-loop-1");
        emitProviderEvent(ProviderEvent::messageCompleted(end));
    }

protected:
    ProviderError validateProviderRequest(const ProviderRequest &) const override { return {}; }
    ProviderTransportRequest buildProviderTransportRequest(const ProviderRequest &) const override
    {
        ProviderTransportRequest t;
        t.body = "{}";
        return t;
    }
    QList<ProviderEvent> parseProviderTransportPayload(const ProviderTransportPayload &) override
    {
        return {};
    }
    void resetProviderTurnState() override {}
    bool startProviderTransportRequest(const ProviderTransportRequest &, ProviderError *) override
    {
        ++startCount;
        if (failOnceNonRetryable) {
            failOnceNonRetryable = false;
            ProviderError error;
            error.code = QStringLiteral("anthropic_error");
            error.message = QStringLiteral("stream aborted");
            error.retryable = false;
            QTimer::singleShot(0, this, [this, error]() { emitErrorOccurred(error); });
            return true;
        }
        if (failWithRetryable && failTimes > 0) {
            --failTimes;
            ProviderError error;
            error.code = QStringLiteral("anthropic_network_error");
            error.message = QStringLiteral("Service is too busy（HTTP 503）");
            error.retryable = true;
            QTimer::singleShot(0, this, [this, error]() { emitErrorOccurred(error); });
        }
        return true;
    }
    QUrl buildModelsUrl(const QString &) const override { return {}; }
    QList<ModelCapabilities> parseModelsPayload(const QByteArray &, QString *) const override
    {
        return {};
    }
};

AbstractLoop::ProviderFactory factoryReturning(LoopFakeProvider *provider)
{
    return [provider](const QString &) -> std::unique_ptr<AbstractProvider> {
        return std::unique_ptr<AbstractProvider>(provider);
    };
}

/// 堆分配：所有权归 Loop；预置 auth 与凭据一致 → setAuth 幂等，不触发模型刷新
LoopFakeProvider *makeLoopProvider(AbstractLoop &loop)
{
    auto *provider = new LoopFakeProvider;
    provider->setAuth(ProviderAuth{QStringLiteral("https://api.anthropic.com"),
                                   QStringLiteral("key"), {}});
    provider->seedAvailableModels({});
    loop.setProviderFactory(factoryReturning(provider));
    return provider;
}

QString makeCredential(ProviderCredential &credential)
{
    return credential.createInstance(QStringLiteral("anthropic"), QStringLiteral("测试"),
                                     QStringLiteral("https://api.anthropic.com"),
                                     QStringLiteral("key"));
}

bool ledgerHasError(const AbstractLoop &loop)
{
    for (const ConversationMessage &msg : loop.messages()) {
        if (msg.kind == ConversationMessage::Kind::Error)
            return true;
    }
    return false;
}

} // namespace

void ProviderRetryTests::retryableBeforeFirstByteKeepsTurnAlive()
{
    AbstractLoop loop;
    ProviderCredential credential;
    const QString instanceId = makeCredential(credential);
    loop.setCredentialStore(&credential);
    loop.setAgentInfo(QStringLiteral("agent-1"));

    LoopFakeProvider *provider = makeLoopProvider(loop);

    SessionRuntime config;
    config.providerType = QStringLiteral("anthropic");
    config.credentialInstanceId = instanceId;
    config.maxRetries = 2;

    // fail 标志必须在 start 前设好（ensureProvider 在 start 内首次 factory）
    provider->failWithRetryable = true;
    provider->failTimes = 1;

    loop.enqueueUserMessage(QStringLiteral("你好"));
    loop.start(config);

    QTRY_VERIFY_WITH_TIMEOUT(provider->startCount >= 2, 5000);

    provider->driveSuccess();
    QTRY_VERIFY_WITH_TIMEOUT(!loop.isBusy(), 5000);
    QVERIFY(loop.lastError().isEmpty());
    QVERIFY(!ledgerHasError(loop));
}

void ProviderRetryTests::exhaustedBudgetFailsTurn()
{
    AbstractLoop loop;
    ProviderCredential credential;
    const QString instanceId = makeCredential(credential);
    loop.setCredentialStore(&credential);
    loop.setAgentInfo(QStringLiteral("agent-1"));

    LoopFakeProvider *provider = makeLoopProvider(loop);

    SessionRuntime config;
    config.providerType = QStringLiteral("anthropic");
    config.credentialInstanceId = instanceId;
    config.maxRetries = 2;

    provider->failWithRetryable = true;
    provider->failTimes = 5;

    loop.enqueueUserMessage(QStringLiteral("你好"));
    loop.start(config);

    QTRY_VERIFY_WITH_TIMEOUT(provider->startCount >= 3, 8000);

    QVERIFY(ledgerHasError(loop));
    QVERIFY(!loop.lastError().isEmpty());
    QVERIFY2(loop.lastError().contains(QStringLiteral("已自动重试")),
             qPrintable(loop.lastError()));
}

void ProviderRetryTests::retryLastFailedTurnReusesUserMessage()
{
    AbstractLoop loop;
    ProviderCredential credential;
    const QString instanceId = makeCredential(credential);
    loop.setCredentialStore(&credential);
    loop.setAgentInfo(QStringLiteral("agent-1"));

    LoopFakeProvider *provider = makeLoopProvider(loop);

    SessionRuntime config;
    config.providerType = QStringLiteral("anthropic");
    config.credentialInstanceId = instanceId;
    config.maxRetries = 0;

    provider->failOnceNonRetryable = true;
    loop.enqueueUserMessage(QStringLiteral("你好"));
    loop.start(config);

    QTRY_VERIFY_WITH_TIMEOUT(!loop.isBusy(), 5000);
    QVERIFY(loop.canRetryLastFailedTurn());
    QVERIFY(ledgerHasError(loop));

    int userCount = 0;
    for (const ConversationMessage &msg : loop.messages()) {
        if (msg.kind == ConversationMessage::Kind::UserText)
            ++userCount;
    }
    QCOMPARE(userCount, 1);

    QVERIFY(loop.retryLastFailedTurn(config));
    QTRY_VERIFY_WITH_TIMEOUT(provider->startCount >= 2, 5000);
    provider->driveSuccess();
    QTRY_VERIFY_WITH_TIMEOUT(!loop.isBusy(), 5000);

    userCount = 0;
    int errorCount = 0;
    for (const ConversationMessage &msg : loop.messages()) {
        if (msg.kind == ConversationMessage::Kind::UserText)
            ++userCount;
        else if (msg.kind == ConversationMessage::Kind::Error)
            ++errorCount;
    }
    QCOMPARE(userCount, 1);
    QCOMPARE(errorCount, 0);
    QVERIFY(loop.lastError().isEmpty());
    QVERIFY(!loop.canRetryLastFailedTurn());
}

QTEST_MAIN(ProviderRetryTests)

#include "ProviderRetryTests.moc"
