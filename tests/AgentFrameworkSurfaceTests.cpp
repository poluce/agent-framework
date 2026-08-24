#include <QtTest>

#include "framework/AgentFramework.h"

#include <memory>

namespace {

class DummyOrchestration final : public AbstractOrchestration
{
public:
    explicit DummyOrchestration(QObject *parent = nullptr)
        : AbstractOrchestration(parent)
    {
    }

    AbstractToolSource *toolSource() override { return nullptr; }
    void attach(AgentSession *session) override { m_session = session; }
    void detach() override { m_session = nullptr; }
    AgentSession *attached() const { return m_session; }

private:
    AgentSession *m_session = nullptr;
};

} // namespace

class AgentFrameworkSurfaceTests final : public QObject
{
    Q_OBJECT

private slots:
    void umbrellaCompilesAndRegistryAcceptsCustomRecipe();
};

void AgentFrameworkSurfaceTests::umbrellaCompilesAndRegistryAcceptsCustomRecipe()
{
    OrchestrationRegistry registry;
    QVERIFY(!registry.contains(QStringLiteral("dummy")));
    registry.add(QStringLiteral("dummy"), [](QObject *parent) {
        return std::make_unique<DummyOrchestration>(parent);
    });
    QVERIFY(registry.contains(QStringLiteral("dummy")));
    auto orch = registry.create(QStringLiteral("dummy"));
    QVERIFY(orch);
    QVERIFY(orch->toolSource() == nullptr);
    QVERIFY(orch->createUnit(UnitCreateRequest{}) == nullptr);
    QVERIFY(orch->closeUnit(QStringLiteral("x")) == nullptr);
}

QTEST_MAIN(AgentFrameworkSurfaceTests)
#include "AgentFrameworkSurfaceTests.moc"
