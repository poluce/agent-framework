#pragma once

/**
 * @file ProviderTypes.h
 * @brief agent-qt 多模型中间协议（LLM-agnostic）聚合头
 *
 * 设计原则：
 * 1. 入出共用终态模型 ProviderItem —— 不拆 ContentBlock / HistoryItem 双轨
 * 2. ProviderRequest 只持有有序 items[]；是否已提交由账本状态管理
 * 3. Reasoning 为一等 Item
 * 4. 不设 Opaque 逃逸舱（无 ProviderItemKind::Opaque / rawItem）。
 *    厂商私货不得进入 items/账本；仅允许 adapter 内部瞬态、continuationId、
 *    或投影为一等 Item/可读文本；请求扩展用 metadata（非历史条目）
 * 5. ProviderEvent 保持扁平信封 + 工厂方法（不做全量 variant）
 * 6. systemPrompt 保留顶层字段
 * 7. 多模态：Text / Image / Audio / Document / Video 均为一等 MessagePart（Text 可附 citations）
 * 8. 厂商服务端工具用 ServerToolCall/Result 一等 kind（ProviderServerToolName 短名），禁止 Opaque
 * 9. 结构化输出用 ProviderResponseFormat；工具结果可用 outputParts 承载多模态
 * 10. 多协议族：protocolFamily + sampling/cache 提示；DeepSeek/Anthropic/Gemini/Responses 语义可映射
 * 11. Program/Approval/Compaction 一等 kind；caller*、deferLoading、allowedCallers 覆盖 PTC 与 tool search
 * 12. Gemini：Interactions steps 与 generateContent parts 均可映射；url_context/google_maps；
 *     thoughtSignature 回放；topK / cachedContent / mediaResolution / thoughtTokens
 * 13. 硬化：validate()；未指定哨兵（temperature/maxTokens）；itemId；BlobRef；
 *     providerResponseId；Core/Extended kind；metadata/details 白名单；Tool 投影
 * 14. 值类型快照：跨层只拷贝、不共享可变引用。
 *     本协议 IR 对全局 immutability 规范的落地形态是「按值传递 + 入账新快照」，
 *     而非 private 字段 / withXxx 链式 API（以兼容 Qt MetaType 与信号热路径）。
 * 15. Transport/TurnState 仅在 ProviderAdapterTypes.h，不进本聚合头
 *
 * 数据流：
 *   账本 → ProviderRequest{items, tools, systemPrompt, ...}
 *        → adapter.buildTransport() → 厂商 JSON
 *        → SSE/JSON → ProviderEvent 流
 *        → MessageCompleted.outputItems: QList<ProviderItem>
 *        → 写回账本
 *
 * 文件拆分（依赖方向）：
 *   ProviderCommon.h         → 枚举 / 资产 / Usage / Error / 能力（账本 IR）
 *   ProviderItem.h           → MessagePart + ProviderItem
 *   ProviderRequest.h        → Request + 选项
 *   ProviderEvent.h          → 流式事件信封（账本 IR）
 *   ProviderAdapterTypes.h   → Transport / TurnState（**仅 adapter**；不进本聚合头）
 *   ProviderTypes.h          → 本文件（账本/UI 统一 include）
 *
 * 对外（账本/UI/Agent）：
 *   #include "providers/ProviderTypes/ProviderTypes.h"
 * Adapter/通道额外：
 *   #include "providers/ProviderTypes/ProviderAdapterTypes.h"
 *
 * 扩展本协议时：遵循 .agents/skills/update-provider-protocol/SKILL.md
 * （无 Opaque；查厂商文档后再加一等 kind；同步 docs/协议/provider-protocol.md）
 */

#include "ProviderCommon.h"
#include "ProviderEvent.h"
#include "ProviderItem.h"
#include "ProviderRequest.h"
