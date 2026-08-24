#pragma once

/**
 * @file ProviderEvent.h
 * @brief 出站流式事件：信封 + Delta + MessageEnd（TurnState 见 ProviderAdapterTypes.h）
 */

#include "ProviderCommon.h"
#include "ProviderItem.h"

#include <QJsonObject>
#include <QList>
#include <QString>

// ── 枚举 ──

/// 流式输出部件类别（标记 delta 属于哪类 part）
enum class ProviderStreamPartKind {
    Text,       ///< 正文文本
    Reasoning,  ///< 思考/推理
    ToolCall,   ///< 工具调用
    Image,      ///< 图片
    Audio,      ///< 音频
    Video,      ///< 视频
    Transcript, ///< 语音转写
    Unknown,    ///< 未知/未分类
};

/// 模型停止生成的原因
enum class StopReason {
    EndTurn,    ///< 正常结束本轮
    MaxTokens,  ///< 触达最大输出长度
    ToolUse,    ///< 因需要调用工具而停（含 Gemini Interactions requires_action）
    PauseTurn,  ///< 服务端 agentic 循环暂停（Anthropic pause_turn；须原样续跑）
    Incomplete, ///< 完成但结果不完整（Gemini interaction incomplete 等）
    Safety,     ///< 安全策略拦截
    Error,      ///< 错误终止
    Cancelled,  ///< 用户或上层取消
};

/// 流式事件种类（扁平信封的 kind）
///
/// Gemini Interactions 的 steps（user_input / thought / function_call / function_result /
/// model_output / google_search_call / google_search_result / code_execution 等）
/// 由 adapter 投影到下列事件 + 最终 outputItems，不引入 Opaque step 类型。
/// step.delta（thought_summary / text / arguments_delta）→ ReasoningDelta / TextDelta / ToolCall*。
enum class ProviderEventKind {
    MessageStarted,        ///< 一条助手消息开始
    MessageCompleted,      ///< 一条助手消息完成（含 outputItems）
    ContentPartStarted,    ///< 某个 content part 开始
    ContentPartCompleted,  ///< 某个 content part 结束
    TextDelta,             ///< 正文增量
    ReasoningDelta,        ///< 思考增量（含 Gemini thought summary 流）
    ToolCallStarted,       ///< 客户端或服务端工具开始（看 deltaPayload.toolName）
    ToolCallCompleted,     ///< 工具调用参数/结果完整（服务端结果亦可用 Completed+outputItems）
    ImageOutput,           ///< 图片输出
    AudioDelta,            ///< 流式音频块（TTS / 原生音频）
    TranscriptDelta,       ///< 流式转写文本
    UsageUpdated,          ///< 用量更新
    ResponseMetadata,      ///< 响应级元数据（response id / interaction id 等）
    Cancelled,             ///< 取消
    Error,                 ///< 错误
};

// ── 事件载荷 ──

/// 流式 delta 的公共定位字段（消息 id、part 下标、序号）
struct ProviderDeltaBase
{
    QString messageId;   ///< 所属消息 id
    int partIndex = -1;  ///< content part 下标；-1 表示未指定
    qint64 sequence = 0; ///< 事件序号（单调，便于排序/去重）
};

/// MessageStarted 载荷：消息开始时的 id 与可选初始用量
struct ProviderMessageStart
{
    QString messageId;            ///< 本条消息 id
    ProviderUsage initialUsage;   ///< 可选初始用量
    qint64 sequence = 0;          ///< 事件序号
};

/**
 * @brief MessageCompleted 载荷
 *
 * 包含停止原因、最终用量，以及与入站同型的 outputItems。
 * outputItems 是写账本的权威完成态（可含多个并行 FunctionCall）。
 */
struct ProviderMessageEnd
{
    QString messageId;                 ///< 本条消息 id
    StopReason stopReason = StopReason::EndTurn; ///< 停止原因
    bool wasTruncated = false;         ///< 输出是否被截断
    ProviderUsage finalUsage;          ///< 最终用量
    QList<ProviderItem> outputItems;   ///< 完成态条目（与 Request.items 元素同型）
    /**
     * 可选 logprobs 载荷（可移植 JSON，非厂商 raw 整包）。
     * 约定：{ "content": [ { "token", "logprob", "top":[{token,logprob}] } ] }
     * 顶层键须通过 isAllowedLogprobsKey / validateLogprobsObject。
     * 空对象表示未请求或未返回。
     */
    QJsonObject logprobs;
    qint64 sequence = 0;               ///< 事件序号

    /**
     * @brief 校验完成态载荷
     * @param error 失败原因
     * @param strictItems 是否对 outputItems 做 validate()
     * @param maxInlineAssetBytes 传给 outputItems.validate
     */
    [[nodiscard]] bool validate(QString *error = nullptr,
                                bool strictItems = true,
                                int maxInlineAssetBytes = kProviderMaxInlineAssetBytes) const;
};

/**
 * @brief 各类 delta / part / tool / 多模态输出共用的载荷体
 *
 * 按 ProviderEventKind 解读字段；tool 的 rawArguments 增量语义为 append。
 */
struct ProviderDeltaPayload
{
    ProviderDeltaBase base;           ///< 公共定位
    QString text;                     ///< 文本/思考/转写增量
    QString toolCallId;               ///< 工具调用 id
    QString toolName;                 ///< 工具名
    /**
     * true=厂商服务端工具事件；false=客户端 FunctionCall。
     * ToolCallStarted/Completed 共用事件种类时用此区分（勿只靠 toolName 猜测）。
     */
    bool isServerTool = false;
    QJsonObject arguments;            ///< 已解析参数（完成时常见）
    bool parseFailed = false;         ///< 参数 JSON 解析是否失败
    QString rawArguments;             ///< 原始参数片段或全文；delta 时为 append
    ProviderImageAsset image;         ///< 图片输出
    ProviderAudioAsset audio;         ///< 音频输出块
    ProviderStreamPartKind partKind = ProviderStreamPartKind::Unknown; ///< 部件类别提示
};

using ProviderTextDelta = ProviderDeltaPayload;         ///< 正文增量
using ProviderReasoningDelta = ProviderDeltaPayload;    ///< 思考增量
using ProviderToolCallStart = ProviderDeltaPayload;     ///< 工具调用开始
using ProviderToolCallEnd = ProviderDeltaPayload;       ///< 工具调用完成
using ProviderImageOutput = ProviderDeltaPayload;       ///< 图片输出
using ProviderAudioDelta = ProviderDeltaPayload;        ///< 音频增量
using ProviderTranscriptDelta = ProviderDeltaPayload;   ///< 转写增量
using ProviderContentPartStart = ProviderDeltaPayload;  ///< part 开始
using ProviderContentPartEnd = ProviderDeltaPayload;    ///< part 结束

/// 响应级元数据（如 response/conversation id）
struct ProviderResponseMetadata
{
    QString providerResponseId;   ///< 厂商响应 id（continuation / previous_*_id）
    /**
     * 可选容器 id（Anthropic code_execution container；续跑时可能需要回传）。
     * 空=不适用。
     */
    QString containerId;
    /**
     * 可移植用量（优先写入此字段，而非厂商 raw）。
     * 与 ProviderEvent.usage / MessageEnd.finalUsage 对齐。
     */
    ProviderUsage portableUsage;
    /**
     * 厂商原始 usage 诊断字段（可选扩展位；**不得**作为账本权威用量）。
     * 权威用量用 portableUsage / MessageEnd.finalUsage。
     * 键须通过 isAllowedVendorUsageRawKey；账本/UI 路径应保持空
     * （validateVendorUsageRawObject(..., forbidNonEmpty=true)）。
     */
    QJsonObject vendorUsageRaw;
    qint64 sequence = 0;  ///< 事件序号

    /**
     * @brief 校验响应元数据扩展位
     * @param forbidVendorUsageRaw true=禁止非空 vendorUsageRaw（账本/UI）
     */
    [[nodiscard]] bool validate(QString *error = nullptr,
                                bool forbidVendorUsageRaw = true) const;
};

// ── ProviderEvent ──

/**
 * @brief 流式事件信封（出站过程的基本单位）
 *
 * 保持 struct + kind + 工厂，适合 Qt 信号与 token 级热路径。
 * 只通过工厂方法构造。
 *
 * ## 合法序列（契约）
 * ```
 * MessageStarted
 *   → (Text|Reasoning|ToolCall|Image|Audio|Transcript delta / part | Usage | ResponseMetadata)*
 *   → MessageCompleted
 *   ↘ Error | Cancelled   // 终态：之后不得再发 MessageCompleted 或 delta
 * ```
 * - 同一次 MessageCompleted.outputItems 可含多个 FunctionCall（并行工具）。
 * - **Error / Cancelled 为终端事件**：部分 delta 作废，不得再 Completed；
 *   若 adapter 有可恢复片段，应写入 ProviderTurnState.fallbackOutputItems（见 ProviderAdapterTypes.h，不进本信封）。
 *
 * ## 身份字段
 * - `messageId`（MessageStart/End / delta.base）：本条助手消息 UI/本地 id
 * - `providerResponseId`：厂商 previous_response_id / interaction id（续跑用）
 * - 禁止把 messageId 填进 providerResponseId
 *
 * ## sequence 权威源
 * - **以 `ProviderEvent.sequence` 为准**（单调、可排序/去重）
 * - payload 内 sequence 仅为镜像；消费者应读外层
 *
 * ## 值语义（immutability 在本模块的形态）
 * - 事件是 **值类型快照**，适合 Qt 信号按值传递。
 * - 跨线程 / 跨层 **只拷贝**；禁止多个消费者共享同一 ProviderEvent* 并就地改字段。
 * - 流式路径上 adapter 可改本地临时 Event 再 emit 拷贝；订阅方只读自己的拷贝。
 * - 字段 public 服务热路径与 MetaType，**不等于**允许跨模块共享可变状态。
 */
struct ProviderEvent
{
    ProviderEventKind kind = ProviderEventKind::Error; ///< 事件种类
    ProviderMessageStart messageStart;                 ///< kind==MessageStarted
    ProviderMessageEnd messageEnd;                     ///< kind==MessageCompleted
    ProviderDeltaPayload deltaPayload;                 ///< 各类 delta / part / 多模态
    ProviderUsage usage;                               ///< kind==UsageUpdated
    ProviderResponseMetadata responseMetadata;         ///< kind==ResponseMetadata
    ProviderError error;                               ///< kind==Error
    /**
     * 厂商响应 id（previous_response_id / interaction id）。
     * 仅 ResponseMetadata 或 adapter 在确认厂商 id 后设置；
     * **不得**用本地 messageId 填充。
     */
    QString providerResponseId;
    qint64 sequence = 0;                               ///< 事件序号（权威）

    /**
     * @brief 构造 MessageStarted 事件
     * @param messageStart 消息开始载荷
     */
    [[nodiscard]] static ProviderEvent messageStarted(const ProviderMessageStart &messageStart);

    /**
     * @brief 构造 MessageCompleted 事件
     * @param messageEnd 消息完成载荷
     */
    [[nodiscard]] static ProviderEvent messageCompleted(const ProviderMessageEnd &messageEnd);

    /**
     * @brief 构造 ContentPartStarted 事件
     * @param part part 开始载荷
     */
    [[nodiscard]] static ProviderEvent contentPartStarted(const ProviderContentPartStart &part);

    /**
     * @brief 构造 ContentPartCompleted 事件
     * @param part part 结束载荷
     */
    [[nodiscard]] static ProviderEvent contentPartCompleted(const ProviderContentPartEnd &part);

    /**
     * @brief 构造 TextDelta 事件
     * @param textDelta 正文增量载荷
     */
    [[nodiscard]] static ProviderEvent fromTextDelta(const ProviderTextDelta &textDelta);

    /**
     * @brief 构造仅含文本的 TextDelta 事件
     * @param text 增量文本
     */
    [[nodiscard]] static ProviderEvent fromTextDelta(const QString &text);

    /**
     * @brief 构造 ReasoningDelta 事件
     * @param reasoningDelta 思考增量载荷
     */
    [[nodiscard]] static ProviderEvent fromReasoningDelta(const ProviderReasoningDelta &reasoningDelta);

    /**
     * @brief 构造 ToolCallStarted 事件
     * @param toolCallStart 工具调用开始载荷
     */
    [[nodiscard]] static ProviderEvent toolCallStarted(const ProviderToolCallStart &toolCallStart);

    /**
     * @brief 构造 ToolCallCompleted 事件
     * @param toolCallEnd 工具调用完成载荷
     */
    [[nodiscard]] static ProviderEvent toolCallCompleted(const ProviderToolCallEnd &toolCallEnd);

    /**
     * @brief 构造 ImageOutput 事件
     * @param imageOutput 图片输出载荷
     */
    [[nodiscard]] static ProviderEvent fromImageOutput(const ProviderImageOutput &imageOutput);

    /**
     * @brief 构造 AudioDelta 事件
     * @param audioDelta 音频增量载荷
     */
    [[nodiscard]] static ProviderEvent fromAudioDelta(const ProviderAudioDelta &audioDelta);

    /**
     * @brief 构造 TranscriptDelta 事件
     * @param transcriptDelta 转写增量载荷
     */
    [[nodiscard]] static ProviderEvent fromTranscriptDelta(const ProviderTranscriptDelta &transcriptDelta);

    /**
     * @brief 构造仅含文本的 TranscriptDelta 事件
     * @param text 转写增量文本
     */
    [[nodiscard]] static ProviderEvent fromTranscriptDelta(const QString &text);

    /**
     * @brief 构造 UsageUpdated 事件
     * @param usage 用量统计
     */
    [[nodiscard]] static ProviderEvent usageUpdated(const ProviderUsage &usage);

    /**
     * @brief 构造 ResponseMetadata 事件
     * @param metadata 响应级元数据
     */
    [[nodiscard]] static ProviderEvent responseMetadataUpdated(const ProviderResponseMetadata &metadata);

    /**
     * @brief 构造 Error 事件
     * @param error 错误描述
     */
    [[nodiscard]] static ProviderEvent fromError(const ProviderError &error);

    /**
     * @brief 构造 Error 事件（便捷重载）
     * @param code 错误码
     * @param message 错误说明
     */
    [[nodiscard]] static ProviderEvent fromError(const QString &code, const QString &message);

    /// 构造 Cancelled 事件
    [[nodiscard]] static ProviderEvent cancelled();
};

// 回合瞬态 ProviderTurnState 已迁至 ProviderAdapterTypes.h

Q_DECLARE_METATYPE(ProviderDeltaBase)
Q_DECLARE_METATYPE(ProviderMessageStart)
Q_DECLARE_METATYPE(ProviderMessageEnd)
Q_DECLARE_METATYPE(ProviderDeltaPayload)
Q_DECLARE_METATYPE(ProviderResponseMetadata)
Q_DECLARE_METATYPE(ProviderEvent)
