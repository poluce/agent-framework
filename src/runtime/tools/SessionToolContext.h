#pragma once

#include "AbstractSession.h"
#include "AbstractUnit.h"
#include "config/SessionRuntime.h"

#include <QJsonArray>
#include <QString>
#include <QVariant>

/**
 * @brief 会话工具调用的上下文（session + caller 包装）
 *
 * 由 SessionToolRuntime 在每次调用时构造，**只在调用期间有效**（栈对象）。
 * 异步路径请捕获 ctx->session() / ctx->caller()（长生命），不要捕获 ctx 本身。
 */
class SessionToolContext
{
public:
    SessionToolContext(AbstractSession *session, AbstractUnit *caller)
        : m_session(session)
        , m_caller(caller)
    {
    }

    AbstractSession *session() const { return m_session; }
    AbstractUnit *caller() const { return m_caller; }

    // ── caller 侧 ──
    QString agentId() const { return m_caller ? m_caller->agentId() : QString(); }
    QJsonArray todos() const { return m_caller ? m_caller->todos() : QJsonArray(); }
    void setTodos(const QJsonArray &todos)
    {
        if (m_caller) {
            m_caller->setTodos(todos);
        }
    }
    void appendSessionEvent(const QString &text)
    {
        if (m_caller) {
            m_caller->appendSessionEvent(text);
        }
    }

    // ── session 侧 ──
    const SessionRuntime &runtime() const { return m_session->runtime(); }
    bool setRuntimeField(const QString &key, const QVariant &value)
    {
        return m_session && m_session->setRuntimeField(key, value);
    }
    void setSessionWorkingDirectory(const QString &workingDirectory)
    {
        if (m_session) {
            m_session->setSessionWorkingDirectory(workingDirectory);
        }
    }
    QString userCustomPrompt() const
    {
        return m_session ? m_session->userCustomPrompt() : QString();
    }
    void setUserCustomPrompt(const QString &text)
    {
        if (m_session) {
            m_session->setUserCustomPrompt(text);
        }
    }

    // ── 可见性 / 查单元 ──
    bool toolVisible(const QString &sourceId, const QString &toolName) const
    {
        return !m_session || m_session->toolVisible(m_caller, sourceId, toolName);
    }
    AbstractUnit *findUnit(const QString &agentId) const
    {
        return m_session ? m_session->findUnit(agentId) : nullptr;
    }

private:
    AbstractSession *m_session;
    AbstractUnit *m_caller;
};
