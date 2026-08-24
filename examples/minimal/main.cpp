#include "framework/AgentFramework.h"
#include "providers/core/AbstractProvider.h"
#include "providers/service/ProviderCredential.h"
#include "providers/service/ProviderService.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace {

constexpr auto kProviderType = "fake";
constexpr auto kReplyText = "framework-host-turn-ok";

class HostOrchestration final : public AbstractOrchestration
{
public:
    explicit HostOrchestration(QObject *parent = nullptr)
        : AbstractOrchestration(parent)
    {
    }

    AbstractToolSource *toolSource() override { return nullptr; }

    void attach(AgentSession *session) override { m_session = session; }
    void detach() override { m_session = nullptr; }

    void onSessionStarted() override
    {
        if (!m_session || m_session->count() > 0)
            return;
        Agent *unit = m_session->insertUnit(QStringLiteral("agent-0"),
                                            QStringLiteral("Main"));
        if (!unit)
            return;
        m_session->setSelectedAgentId(unit->agentId());
        m_session->applyRuntimeToPrimary();
    }

    void onUnitInserted(Agent *unit) override
    {
        if (unit && m_primaryAgentId.isEmpty())
            m_primaryAgentId = unit->agentId();
    }

    void onUnitsClearing() override { m_primaryAgentId.clear(); }

    Agent *primaryUnit() const override
    {
        return m_session ? m_session->findById(m_primaryAgentId) : nullptr;
    }

    bool isPrimary(const Agent *unit) const override
    {
        return unit && !m_primaryAgentId.isEmpty()
            && unit->agentId() == m_primaryAgentId;
    }

private:
    AgentSession *m_session = nullptr;
    QString m_primaryAgentId;
};

class FakeProvider final : public AbstractProvider
{
public:
    FakeProvider()
        : AbstractProvider(QString::fromLatin1(kProviderType))
    {
        seedLocalCatalog();
    }

    void setAuth(const ProviderAuth &auth) override
    {
        AbstractProvider::setAuth(auth);
        // 基类在 baseUrl/apiKey 变化时会 invalidate 目录；假通道没有 HTTP 刷新。
        seedLocalCatalog();
    }

protected:
    ProviderError validateProviderRequest(const ProviderRequest &) const override
    {
        return {};
    }

    ProviderTransportRequest buildProviderTransportRequest(
        const ProviderRequest &) const override
    {
        ProviderTransportRequest transport;
        transport.body = QByteArrayLiteral("{}");
        return transport;
    }

    QList<ProviderEvent> parseProviderTransportPayload(
        const ProviderTransportPayload &) override
    {
        return {};
    }

    void resetProviderTurnState() override {}

    bool startProviderTransportRequest(const ProviderTransportRequest &,
                                       ProviderError *) override
    {
        // 不得在 sendRequest 栈内同步收口，否则 Loop 会在 startProviderTurnImpl
        // 返回后误装看门狗。
        QTimer::singleShot(0, this, [this]() {
            emitProviderEvent(ProviderEvent::fromTextDelta(
                QString::fromLatin1(kReplyText)));
            ProviderMessageEnd end;
            end.messageId = QStringLiteral("fw-msg");
            end.stopReason = StopReason::EndTurn;
            emitProviderEvent(ProviderEvent::messageCompleted(end));
        });
        return true;
    }

    QUrl buildModelsUrl(const QString &) const override { return {}; }
    QList<ModelCapabilities> parseModelsPayload(const QByteArray &,
                                                QString *) const override
    {
        return {};
    }

private:
    void seedLocalCatalog()
    {
        ModelCapabilities model;
        model.modelId = auth().modelName.isEmpty()
            ? QStringLiteral("fake-model")
            : auth().modelName;
        seedAvailableModels({model});
    }
};

bool ledgerHasReply(const Agent *unit)
{
    if (!unit)
        return false;
    for (const ConversationMessage &message : unit->ledgerMessages()) {
        if (message.kind == ConversationMessage::Kind::AssistantText
            && message.text.contains(QString::fromLatin1(kReplyText))) {
            return true;
        }
    }
    return false;
}

void fail(const char *why, const QString &detail = {})
{
    if (detail.isEmpty())
        std::fprintf(stderr, "agent-framework-minimal: %s\n", why);
    else
        std::fprintf(stderr, "agent-framework-minimal: %s: %s\n",
                     why, qPrintable(detail));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 配方登记：仓外宿主自己 add，不碰产品内置 id。
    OrchestrationRegistry registry;
    registry.add(QStringLiteral("host-single"), [](QObject *parent) {
        return std::make_unique<HostOrchestration>(parent);
    });
    if (!registry.contains(QStringLiteral("host-single"))) {
        fail("registry rejected host-single");
        return 1;
    }

    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        fail("temp workspace");
        return 1;
    }

    ProviderService providers;
    providers.registerProvider(QString::fromLatin1(kProviderType), []() {
        return std::make_unique<FakeProvider>();
    });

    ProviderCredential credentials;
    const QString instanceId = credentials.createInstance(
        QString::fromLatin1(kProviderType),
        QStringLiteral("fake"),
        QStringLiteral("https://example.test"),
        QStringLiteral("key"));
    if (instanceId.isEmpty()) {
        fail("create credential");
        return 1;
    }

    SessionRuntime defaults;
    defaults.systemPrompt = QStringLiteral("minimal host");
    defaults.workingDirectory = workspace.path();
    defaults.providerType = QString::fromLatin1(kProviderType);
    defaults.modelName = QStringLiteral("fake-model");
    defaults.credentialInstanceId = instanceId;
    defaults.compactEnabled = false;

    // 编排声明在会话之前，析构先拆会话。
    auto orchestration = registry.create(QStringLiteral("host-single"));
    if (!orchestration) {
        fail("create orchestration");
        return 1;
    }

    AgentSessionConfig config;
    config.globalDefaults = &defaults;
    config.credentialStore = &credentials;
    config.orchestration = orchestration.get();
    config.providerFactory = [&providers](const QString &type) {
        return providers.create(type, "agent-framework-minimal");
    };

    AgentSession session(config);
    session.setRuntime(defaults);
    session.start();

    Agent *unit = session.primaryUnit();
    if (!unit) {
        fail("start() did not insert a unit");
        return 1;
    }

    QEventLoop wait;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool finished = false;
    bool sawBusy = false;
    const auto maybeQuit = [&]() {
        if (unit->busy()) {
            sawBusy = true;
            return;
        }
        if (sawBusy || ledgerHasReply(unit) || !unit->lastError().isEmpty()) {
            finished = true;
            wait.quit();
        }
    };
    // 轮次事件在 Agent 上，不在 Session 的 addEventHandler。
    unit->addEventHandler(
        [&](const core_ir::Event &,
            const core_ir::EventContext &,
            const core_ir::SubmissionId &) { maybeQuit(); });
    QObject::connect(unit, &Agent::stateChanged, &wait, maybeQuit);
    QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);

    unit->submitUserDelivery(QStringLiteral("ping"),
                             {},
                             AbstractLoop::UserDelivery::NextTurn);
    maybeQuit();

    if (!finished && !ledgerHasReply(unit)) {
        if (!unit->busy() && !unit->lastError().isEmpty()) {
            fail("turn failed to start", unit->lastError());
            return 1;
        }
        QTimer poll;
        poll.setInterval(20);
        QObject::connect(&poll, &QTimer::timeout, &wait, [&]() {
            if (ledgerHasReply(unit) || !unit->lastError().isEmpty()) {
                finished = true;
                wait.quit();
            }
        });
        poll.start();
        timeout.start(5000);
        wait.exec();
        poll.stop();
    }

    if (!ledgerHasReply(unit)) {
        fail("no assistant reply", unit->lastError());
        return 1;
    }
    return 0;
}
