#pragma once

/**
 * @file ProviderRequest.h
 * @brief 入站请求：ProviderRequest + 采样/工具/推理/音频选项
 */

#include "ProviderItem.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// ── 选项枚举与结构 ──

/// 工具选择策略（映射各家 tool_choice / tool_config）
enum class ProviderToolChoiceMode {
    ProviderDefault, ///< 不显式指定，交给厂商默认
    None,            ///< 禁止调用工具
    Auto,            ///< 模型自行决定
    Required,        ///< 必须调用至少一个工具
    Named,           ///< 必须调用 toolName 指定工具
};

/**
 * @brief 期望的输出模态开关
 *
 * 告诉 adapter 需要哪些输出通道。
 */
struct ProviderOutputSpec
{
    bool textEnabled = true;   ///< 需要文本输出
    bool imageEnabled = false; ///< 需要图片输出
    bool audioEnabled = false; ///< 需要音频输出

    /// 仅文本
    [[nodiscard]] static ProviderOutputSpec textOnly();
    /// 文本 + 图片
    [[nodiscard]] static ProviderOutputSpec textAndImages();
    /// 文本 + 音频
    [[nodiscard]] static ProviderOutputSpec textAndAudio();
    /// 文本 + 图片 + 音频
    [[nodiscard]] static ProviderOutputSpec multimodal();
};

/// 本次请求的工具选择策略
struct ProviderToolChoice
{
    ProviderToolChoiceMode mode = ProviderToolChoiceMode::ProviderDefault; ///< 策略模式
    QString toolName; ///< mode==Named 时的目标工具名
    /**
     * 是否允许并行工具调用（可移植）。
     * - Unset：不传（OpenAI 默认常允许；Anthropic 默认可并行）
     * - Yes：允许并行
     * - No：禁用并行 ← Anthropic tool_choice.disable_parallel_tool_use=true；
     *   OpenAI parallel_tool_calls=false
     */
    ProviderTriState allowParallel = ProviderTriState::Unset;

    /// 是否相对 ProviderDefault 做了显式指定（含 allowParallel）
    [[nodiscard]] bool isExplicit() const;
    /// 禁止调用工具
    [[nodiscard]] static ProviderToolChoice none();
    /// 模型自行决定
    [[nodiscard]] static ProviderToolChoice autoChoice();
    /// 必须调用至少一个工具（Anthropic any / OpenAI required）
    [[nodiscard]] static ProviderToolChoice required();

    /**
     * @brief 必须调用指定工具
     * @param toolName 目标工具名
     */
    [[nodiscard]] static ProviderToolChoice named(const QString &toolName);
};

/**
 * @brief 思考/推理选项
 *
 * 开闭与强度；adapter 映射为 thinking / reasoning_effort 等。
 */
struct ProviderReasoningOptions
{
    bool requested = false; ///< 本次是否显式指定（false 表示用模型/会话默认）
    bool enabled = false;   ///< 是否启用思考
    /**
     * 强度档位（枚举，禁止自由字符串）。
     * Unset=未指定；adapter 映射到各厂商 wire
     * （DeepSeek：Minimal/Low/Medium/High→high、XHigh/Max→max；
     *  Responses/ChatCompletions：toString 直通）。
     */
    ProviderReasoningEffort effort = ProviderReasoningEffort::Unset;
    /**
     * 是否返回思考摘要（非完整加密链）。
     * Gemini Interactions：thinking_summaries=auto；
     * Gemini generateContent：thinkingConfig.includeThoughts；
     * OpenAI reasoning.summary；其他厂商有则映射，无则忽略。
     */
    bool includeSummary = false;
    /**
     * 思考 token 预算（Anthropic thinking.budget_tokens；
     * Gemini generateContent thinkingConfig.thinkingBudget 等）。
     * 0=未指定；与 effort 可同时存在，adapter 按厂商择一或组合。
     */
    int budgetTokens = 0;

    /// 是否做了显式指定（看 requested）
    [[nodiscard]] bool isExplicit() const;
    /// 显式开启思考（effort 默认）
    [[nodiscard]] static ProviderReasoningOptions enabledOption();
    /// 显式关闭思考
    [[nodiscard]] static ProviderReasoningOptions disabledOption();
};

/// 语音相关请求选项（TTS 音色、编解码提示、是否要转写）
struct ProviderAudioOptions
{
    QString voice;                  ///< TTS 音色，空=默认
    QString inputFormat;            ///< 输入编码提示，如 wav / webm；空=由 mime 推断
    QString outputFormat;           ///< 期望输出编码；空=provider 默认
    bool requestTranscript = false; ///< 是否同时请求转写文本

    /// 任一字段非默认则视为显式配置
    [[nodiscard]] bool isExplicit() const;
};

/**
 * @brief 可移植采样参数（Chat Completions / Responses / 兼容网关交集）
 *
 * 未显式使用的字段保持「哨兵默认」：
 * - topP < 0、seed < 0、penalty == 0 且列表空 → adapter 可不序列化该字段
 * DeepSeek/OpenAI 兼容网关常见：temperature、top_p、stop；seed/penalty 按能力位裁剪。
 */
struct ProviderSamplingOptions
{
    double topP = -1.0;              ///< nucleus sampling；<0 表示未指定
    int topK = -1;                   ///< top_k（Gemini generationConfig.topK）；<0 表示未指定
    qint64 seed = -1;                ///< 随机种子；<0 表示未指定
    QStringList stop;                ///< 停止序列；空表示未指定
    double presencePenalty = 0.0;    ///< presence_penalty；0 且未 request 时可不发
    double frequencyPenalty = 0.0;   ///< frequency_penalty
    bool penaltiesRequested = false; ///< true 时即使 penalty 为 0 也显式发送

    /// 是否显式指定 topP（>=0）。
    [[nodiscard]] bool hasTopP() const;
    /// 是否显式指定 topK（>=0）。
    [[nodiscard]] bool hasTopK() const;
    /// 是否显式指定 seed（>=0）。
    [[nodiscard]] bool hasSeed() const;
    /// 是否显式指定停止序列。
    [[nodiscard]] bool hasStop() const;
    /// 是否相对默认哨兵做了任一显式采样配置。
    [[nodiscard]] bool isExplicit() const;
};

/**
 * @brief 结构化输出期望（OpenAI response_format / Gemini responseMimeType+schema）
 *
 * kind=None 表示不指定；JsonSchema 时 jsonSchema 为 JSON Schema 对象。
 */
struct ProviderResponseFormat
{
    ProviderResponseFormatKind kind = ProviderResponseFormatKind::None; ///< 格式种类
    QJsonObject jsonSchema; ///< kind==JsonSchema 时的 schema（可空则降级为 JsonObject）
    QString schemaName;     ///< 可选 schema 名（部分厂商需要）

    /// 是否相对 None 做了显式格式指定。
    [[nodiscard]] bool isExplicit() const;
    /// 不指定结构化输出格式。
    [[nodiscard]] static ProviderResponseFormat none();
    /// 要求任意 JSON 对象（无严格 schema）。
    [[nodiscard]] static ProviderResponseFormat jsonObject();
    /**
     * @brief 构造 JsonSchema 格式
     * @param schema JSON Schema 对象
     * @param name 可选 schema 名
     * @note 字段 jsonSchema 已占用同名，故工厂用 fromJsonSchema
     */
    [[nodiscard]] static ProviderResponseFormat fromJsonSchema(const QJsonObject &schema,
                                                               const QString &name = {});
};

// ── ProviderRequest ──

/**
 * @brief 发给任意 Provider 的统一入站请求（文档式完整快照）
 *
 * items 按时间顺序包含历史 + 本轮；systemPrompt 为顶层字段不进 items。
 * Adapter 负责映射为各厂商原生 JSON，上层不直接拼厂商字段。
 *
 * ## 值语义
 * - 值类型快照：一次请求构造完整 Request，按值交给 adapter；不要跨任务共享可变 Request&。
 * - 发送前 validate()；写账本用 validateForLedger()（禁内联多模态）。
 * - 需要改参数时改本地副本或重建，避免账本/运行环持有同一可写引用。
 */
struct ProviderRequest
{
    QString requestId;       ///< 本次请求 id（日志/去重/关联事件）
    QString conversationId;  ///< 会话 id（可选，便于厂商侧会话关联）

    /// 有序线路条目（历史 + 本轮）：消息/客户端工具/服务端工具/Reasoning
    QList<ProviderItem> items;

    /// 本轮可用工具列表
    QList<ProviderToolSpecification> tools;
    /// 期望输出模态
    ProviderOutputSpec desiredOutput;
    /// 工具选择策略
    ProviderToolChoice toolChoice;
    /**
     * 最大输出 token。
     * <0 = 未指定（adapter **不得**序列化该字段，走厂商/模型默认）；
     * >=0 = 显式上限。
     */
    int maxOutputTokens = -1;
    /**
     * 采样温度。
     * <0 = 未指定（adapter **不得**序列化，走厂商默认）；
     * >=0 = 显式温度（含 0.0 表示用户明确要确定性采样）。
     */
    double temperature = -1.0;
    bool stream = true;         ///< 是否流式
    /// 思考开闭与 effort（low/medium/high）
    ProviderReasoningOptions reasoning;
    /// 语音相关选项
    ProviderAudioOptions audio;
    /// 结构化输出期望（可选）
    ProviderResponseFormat responseFormat;
    /// 采样扩展（top_p / seed / stop / penalties）
    ProviderSamplingOptions sampling;
    /**
     * 目标协议族（可选）。
     * Auto：由 Provider 实例/凭证决定；显式设置时 adapter 按该方言编码
     * （如 DeepSeekChatCompletions 写入 thinking + reasoning_content 历史字段）。
     */
    ProviderProtocolFamily protocolFamily = ProviderProtocolFamily::Auto;
    /// 是否请求 logprobs（Chat Completions 等；能力位 supportsLogprobs）
    bool requestLogprobs = false;
    /// logprobs 返回的 top 数量；0 表示仅用厂商默认（当 requestLogprobs 为真）
    int topLogprobs = 0;

    /**
     * 请求额外输出片段（可移植 include 列表，非 Opaque）。
     * OpenAI Responses 示例值（adapter 原样或映射）：
     * - web_search_call.action.sources
     * - code_interpreter_call.outputs
     * - computer_call_output.output.image_url
     * - file_search_call.results
     * - reasoning.encrypted_content（无状态续聊加密 reasoning）
     * - message.output_text.logprobs
     * 空=不传 include。
     */
    QStringList responseInclude;

    /**
     * 是否后台执行长任务（OpenAI Responses background / Gemini Interactions background）。
     * Unset=不传；Yes/No=显式。
     */
    ProviderTriState backgroundExecution = ProviderTriState::Unset;

    /// 系统提示：顶层字段，由 adapter 映射为 system / instructions / system_instruction
    QString systemPrompt;

    /**
     * 有状态续跑 id（opt-in，默认空）：
     * - OpenAI Responses：previous_response_id
     * - Gemini Interactions：previous_interaction_id
     * **默认策略是无状态全量 items 回放**：ledger 可存厂商 id，但 buildRequest
     * 不会自动提升本字段；仅调用方显式写入时 adapter 才发 previous_* 并裁增量。
     */
    QString continuationId;

    /**
     * 是否让厂商服务端持久化本轮状态（OpenAI Responses store / Gemini Interactions store）。
     * Unset=不传；Yes/No=显式 true/false。
     * 与 continuationId 配合：有状态续跑通常需 store=Yes；默认无状态时保持 Unset/No。
     */
    ProviderTriState storeServerState = ProviderTriState::Unset;

    /**
     * OpenAI Conversations API 等长寿命会话对象 id（可选）。
     * 与 conversationId（应用内会话）区分：本字段专指厂商侧 conversation 资源。
     * 空=不使用 Conversations API 对象。
     */
    QString providerConversationId;

    /**
     * 厂商侧上下文缓存资源名（Gemini cachedContent / cachedContents/{id}）。
     * 空=不使用显式缓存；与 part.cachePolicy（断点提示）不同：本字段是已创建的缓存对象。
     */
    QString providerCachedContentId;

    /**
     * 请求级默认媒体分辨率（Gemini generationConfig.mediaResolution）。
     * 空=未指定；part.mediaResolution 非空时优先于本字段。
     */
    QString mediaResolution;

    /**
     * 未一等化的扩展参数暂存（**不得**承载核心语义 / 对话记忆）。
     * 键须通过 isAllowedRequestMetadataKey()；validate() 会检查。
     */
    QVariantMap metadata;

    /// temperature 是否显式指定（>=0）
    [[nodiscard]] bool hasTemperature() const;
    /// maxOutputTokens 是否显式指定（>=0）
    [[nodiscard]] bool hasMaxOutputTokens() const;

    /**
     * @brief 校验请求不变量 + 各 item（发送前）
     * @param error 失败时写入人类可读原因
     * @param capabilities 可选；非空时做能力门控（图/工具/扩展 kind 等）
     * @param maxInlineAssetBytes 传给 item.validate；默认允许有限内联
     * @return true 可发送
     */
    [[nodiscard]] bool validate(QString *error = nullptr,
                                const ModelCapabilities *capabilities = nullptr,
                                int maxInlineAssetBytes = kProviderMaxInlineAssetBytes) const;

    /**
     * @brief 账本写入策略校验：reasoning 单轨 + 禁止内联 data（仅 uri/blob）
     *
     * 等价于 validate(error, caps, maxInlineAssetBytes=0)。
     */
    [[nodiscard]] bool validateForLedger(QString *error = nullptr,
                                         const ModelCapabilities *capabilities = nullptr) const;

    /// 是否包含客户端工具结果（FunctionCallOutput）
    [[nodiscard]] bool hasFunctionCallOutput() const;
    /// 是否包含服务端工具结果（ServerToolResult）
    [[nodiscard]] bool hasServerToolResult() const;
    /// items 中是否含图片 part
    [[nodiscard]] bool hasImageInput() const;
    /// items 中是否含音频 part
    [[nodiscard]] bool hasAudioInput() const;
    /// items 中是否含视频 part
    [[nodiscard]] bool hasVideoInput() const;
    /// items 中是否含文档 part
    [[nodiscard]] bool hasDocumentInput() const;
    /// 拼接用户文本（及音频转写）供调试/简单 provider
    [[nodiscard]] QString joinedUserText() const;
};

Q_DECLARE_METATYPE(ProviderOutputSpec)
Q_DECLARE_METATYPE(ProviderToolChoice)
Q_DECLARE_METATYPE(ProviderReasoningOptions)
Q_DECLARE_METATYPE(ProviderAudioOptions)
Q_DECLARE_METATYPE(ProviderResponseFormat)
Q_DECLARE_METATYPE(ProviderSamplingOptions)
Q_DECLARE_METATYPE(ProviderRequest)
