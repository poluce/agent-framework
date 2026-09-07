#pragma once

/**
 * @file CoreEventChannel.h
 * @brief 执行单元内环事件 fan-out 类型别名
 *
 * Agent / Session / Loop 内部 Event 分发用的 handler 类型
 * （Event + EventContext + SubmissionId）。
 */

#include "CoreEvent.h"

#include <functional>

namespace core_ir {

using EventHandler = std::function<void(const Event &,
                                        const EventContext &,
                                        const SubmissionId &)>;
using HandlerId = const void *;

} // namespace core_ir
