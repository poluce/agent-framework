#pragma once

/**
 * @file CoreEventChannel.h
 * @brief Core 内环事件 fan-out 类型别名（非跨层契约）
 *
 * 客户端跨层入口只有 HostChannel。本文件提供 Agent/Session/Loop
 * 内部 Event 分发用的 handler 类型（Event + EventContext + SubmissionId）。
 */

#include "ir/CoreEvent.h"

#include <functional>

namespace core_ir {

using EventHandler = std::function<void(const Event &,
                                        const EventContext &,
                                        const SubmissionId &)>;
using HandlerId = const void *;

} // namespace core_ir
