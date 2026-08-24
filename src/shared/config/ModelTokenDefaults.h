#pragma once

#include <QtGlobal>

/**
 * 未知模型回落窗口 / 最大输出（token）的唯一字面量源。
 * SessionRuntime 默认、Host 白名单 defaultFor、ModelContextMetaStore 均引用此常量。
 */
namespace ModelTokenDefaults {

constexpr qint64 kContextWindow = 500000;
constexpr qint64 kMaxOutputTokens = 65536;

} // namespace ModelTokenDefaults
