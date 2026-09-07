#pragma once

/**
 * @file CoreEventChannel.h
 * @brief 执行单元内环事件 fan-out
 *
 * Agent / Session / Loop / CompactEngine 共用同一套 handler 增删。
 */

#include "CoreEvent.h"

#include <cstdint>
#include <functional>
#include <map>

namespace core_ir {

using EventHandler = std::function<void(const Event &,
                                        const EventContext &,
                                        const SubmissionId &)>;
using HandlerId = const void *;

class EventHandlerRegistry
{
public:
    HandlerId add(EventHandler handler)
    {
        const HandlerId id = reinterpret_cast<HandlerId>(m_next++);
        m_handlers.emplace(id, std::move(handler));
        return id;
    }

    void remove(HandlerId id)
    {
        m_handlers.erase(id);
    }

    void dispatch(const Event &event,
                  const EventContext &context = {},
                  const SubmissionId &submissionId = {}) const
    {
        for (const auto &pair : m_handlers) {
            pair.second(event, context, submissionId);
        }
    }

    [[nodiscard]] bool empty() const { return m_handlers.empty(); }

private:
    std::uintptr_t m_next = 1;
    std::map<HandlerId, EventHandler> m_handlers;
};

} // namespace core_ir
