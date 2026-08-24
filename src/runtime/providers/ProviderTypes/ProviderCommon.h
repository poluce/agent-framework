#pragma once

/**
 * @file ProviderCommon.h
 * @brief 协议公共层：版本号、跨模块枚举、多模态资产、Usage/Error、能力位（传输见 AdapterTypes）
 */

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

/// 协议主版本（主形状：单一 items、入出同型、无 Opaque、扁平 Event）
constexpr int kProviderProtocolVersion = 2;
/// 协议修订号（一等字段/校验/能力加法；不破主形状）
constexpr int kProviderProtocolRevision = 0;

/**
 * 请求路径默认：单 part 内联字节上限（validate 默认使用）。
 * 超过且无 blobRef → 失败；应落入 blob store。
 * validate(..., maxInlineAssetBytes=0) 表示账本策略：禁止任何内联 data。
 * maxInlineAssetBytes<0 表示不检查体积。
 */
constexpr int kProviderMaxInlineAssetBytes = 256 * 1024;

// ── 跨模块枚举 ──

/// 消息部件种类（出现在 User/Assistant 消息的 content parts 中）
enum class ProviderPartKind {
    Text,     ///< 纯文本（可附 citations）
    Image,    ///< 图片
    Audio,    ///< 音频
    Document, ///< 文档（PDF / 文本文件等）
    Video,    ///< 视频（Gemini fileData/inlineData 等）
};

/**
 * @brief 结构化输出期望（response_format / responseMimeType 的可移植子集）
 *
 * None = 不指定；JsonObject = 任意 JSON 对象；JsonSchema = 按 schema 约束。
 */
enum class ProviderResponseFormatKind {
    None,        ///< 不显式指定
    JsonObject,  ///< 要求 JSON 对象（无严格 schema）
    JsonSchema,  ///< 按 jsonSchema 约束
};

/**
 * @brief 目标厂商协议族（提示 adapter 编码方言，不是传输实现细节）
 *
 * - OpenAiResponses：/responses，input[] / previous_response_id
 * - OpenAiChatCompletions：/chat/completions 及多数 OpenAI 兼容网关
 * - DeepSeekChatCompletions：Chat Completions 方言（thinking.type / reasoning_content / reasoning_effort high|max）
 * - AnthropicMessages：/v1/messages，content blocks / thinking / cache_control
 * - GeminiGenerateContent：generateContent contents/parts
 * - GeminiInteractions：Interactions API（previous_interaction_id / thinking_level / thought steps）
 * - Auto：由凭证/实例配置决定，Request 不强制
 */
enum class ProviderProtocolFamily {
    Auto,                    ///< 由凭证/实例配置决定
    OpenAiResponses,         ///< OpenAI /responses
    OpenAiChatCompletions,   ///< OpenAI /chat/completions 及兼容网关
    DeepSeekChatCompletions, ///< DeepSeek Chat Completions 方言
    AnthropicMessages,       ///< Anthropic /v1/messages
    GeminiGenerateContent,   ///< Gemini generateContent
    GeminiInteractions,      ///< Gemini Interactions API
};

/// 提示缓存策略（Anthropic cache_control、部分网关 prompt cache 的可移植子集）
enum class ProviderCachePolicy {
    None,      ///< 不指定缓存断点
    Ephemeral, ///< 短暂缓存（如 Anthropic ephemeral）
};

/// 三态开关（未指定 / 是 / 否），用于 store 等可选布尔请求字段
enum class ProviderTriState {
    Unset, ///< 不序列化，交给厂商默认
    Yes,   ///< 显式 true
    No,    ///< 显式 false
};

/// 线路角色（仅模型可理解的子集；system 不走 role，而走顶层 systemPrompt）
enum class ProviderMessageRole {
    User,       ///< 用户
    Assistant,  ///< 助手
    Tool,       ///< 工具（结果侧）
};

/**
 * @brief 模型能力键（替代无限 bool 沼泽的主存储）
 *
 * ModelCapabilities 以集合持有这些键；supportsXxx() 为便捷只读。
 * 细粒度服务端工具仍用 supportedServerTools（短名列表）。
 */
enum class ProviderCapability {
    TextInput,                ///< 文本输入
    ImageInput,               ///< 图片输入
    AudioInput,               ///< 音频输入
    DocumentInput,            ///< 文档输入
    VideoInput,               ///< 视频输入
    TextOutput,               ///< 文本输出
    ImageOutput,              ///< 图片输出
    AudioOutput,              ///< 音频输出
    ToolCalling,              ///< 客户端 FunctionCall
    ServerTools,              ///< 任一服务端工具（细节看 supportedServerTools）
    Reasoning,                ///< 思考/推理
    ToolChoice,               ///< 工具选择策略
    MaxOutputTokens,          ///< 最大输出 token
    Citations,                ///< 文本引用
    ResponseFormat,           ///< 结构化输出格式
    SamplingTopP,             ///< top_p 采样
    SamplingSeed,             ///< 随机种子
    SamplingStop,             ///< 停止序列
    PresencePenalty,          ///< presence_penalty
    FrequencyPenalty,         ///< frequency_penalty
    PromptCache,              ///< 提示缓存
    Logprobs,                 ///< logprobs
    BackgroundExecution,      ///< 后台长任务
    ResponseInclude,          ///< 额外 include 片段
    ProgrammaticToolCalling,  ///< 程序化工具调用（PTC）
    ToolSearch,               ///< 工具搜索 / 延迟加载
    McpApproval,              ///< 厂商 MCP 审批
    Compaction,               ///< 上下文压缩
    TopK,                     ///< top_k 采样
    CachedContent,            ///< 厂商侧缓存资源
    MediaResolution,          ///< 媒体分辨率提示
    UrlContext,               ///< URL 上下文工具
    GoogleMapsGrounding,      ///< Google Maps grounding
    StatelessHistory,         ///< 无状态历史回放（默认通常为 true）
    Continuation,             ///< 有状态续跑（continuationId）
};

/// qHash 以供 QSet<ProviderCapability>
inline size_t qHash(ProviderCapability c, size_t seed = 0) noexcept
{
    return ::qHash(static_cast<int>(c), seed);
}

/**
 * @brief 工具调用者种类（替代自由字符串 callerType）
 *
 * Unset = 未指定/模型直接发起；与 wire 字符串映射见 toString/parse。
 */
enum class ProviderCallerKind {
    Unset,           ///< 空 / 未指定（通常等同 direct）
    Direct,          ///< 模型直接调用
    Program,         ///< OpenAI programmatic tool calling
    CodeExecution,   ///< Anthropic code_execution 内嵌调用
};

/**
 * @brief 思考强度档位（替代自由字符串 effort）
 *
 * Unset = 未指定；adapter 负责映射到各厂商 wire 值。
 */
enum class ProviderReasoningEffort {
    Unset,   ///< 未指定
    Minimal, ///< 最低强度
    Low,     ///< 低
    Medium,  ///< 中
    High,    ///< 高
    Max,     ///< 最大
    XHigh,   ///< 输入兼容档；DeepSeek 等可映射到 max
};

/**
 * @brief 条目/工具状态（ProgramOutput 等；替代自由字符串 status）
 */
enum class ProviderItemStatus {
    Unset,      ///< 未指定
    InProgress, ///< 进行中
    Completed,  ///< 已完成
    Incomplete, ///< 不完整
    Failed,     ///< 失败
    Calling,    ///< 调用中
};

/// 将调用者种类转为 wire 字符串（Unset 返回空串）。
[[nodiscard]] QString toString(ProviderCallerKind kind);
/**
 * @brief 解析调用者种类
 * @param text wire 字符串（大小写不敏感；空串→Unset）
 * @param ok 非空时写入是否识别成功
 */
[[nodiscard]] ProviderCallerKind parseCallerKind(const QString &text, bool *ok = nullptr);
/// 是否为已知调用者种类（Unset 视为合法）。
[[nodiscard]] bool isKnownCallerKind(ProviderCallerKind kind);

/// 将思考强度转为 wire 字符串（Unset 返回空串）。
[[nodiscard]] QString toString(ProviderReasoningEffort effort);
/**
 * @brief 解析思考强度
 * @param text wire 字符串（大小写不敏感；空串→Unset）
 * @param ok 非空时写入是否识别成功
 */
[[nodiscard]] ProviderReasoningEffort parseReasoningEffort(const QString &text, bool *ok = nullptr);
/// 是否为已知思考强度枚举值。
[[nodiscard]] bool isKnownReasoningEffort(ProviderReasoningEffort effort);

/**
 * @brief 将内置 providerType id 映射为协议族
 * @param providerType 与 ProviderService / SessionRuntime.providerType 同一套 id
 * @return 已知类型返回对应族；未知返回 Auto
 */
[[nodiscard]] ProviderProtocolFamily protocolFamilyForProviderType(const QString &providerType);

/// 将条目状态转为 wire 字符串（Unset 返回空串）。
[[nodiscard]] QString toString(ProviderItemStatus status);
/**
 * @brief 解析条目状态
 * @param text wire 字符串（大小写不敏感；空串→Unset）
 * @param ok 非空时写入是否识别成功
 */
[[nodiscard]] ProviderItemStatus parseItemStatus(const QString &text, bool *ok = nullptr);

// ── 多模态资产 ──

/**
 * @brief URI / 引用方案（可移植；adapter 按此编码，禁止靠字符串猜）
 *
 * - Unset：未指定，可从 uri 前缀推断
 * - Https / Http：远程 URL
 * - File：本地文件路径（file:// 或裸路径由 adapter 约定）
 * - Data：data: URI
 * - ProviderFile：厂商文件 id（OpenAI file_id / Gemini Files API 名等）
 * - Blob：应用内 blob store 引用（见 blobId）
 */
enum class ProviderUriScheme {
    Unset,       ///< 未指定，可从 uri 前缀推断
    Https,       ///< 远程 HTTPS URL
    Http,        ///< 远程 HTTP URL
    File,        ///< 本地文件路径
    Data,        ///< data: URI
    ProviderFile,///< 厂商文件 id
    Blob,        ///< 应用内 blob store 引用
};

/**
 * @brief 多模态字节的引用语义（优先于内联 data）
 *
 * 账本/历史应尽量只持引用：blobId 或 provider file id；
 * data 仅适合小 demo / 单次请求，禁止把大块 QByteArray 当多轮真相。
 */
struct ProviderBlobRef
{
    QString blobId;       ///< 应用 blob store id；空=无
    QString contentHash;  ///< 可选内容哈希（如 sha256 hex）
    qint64 byteSize = 0;  ///< 字节数；0=未知
    qint64 expiresAtMs = 0; ///< 过期时间 epoch ms；0=未指定
    ProviderUriScheme scheme = ProviderUriScheme::Unset; ///< uri/引用方案

    /// 是否无有效引用信息。
    [[nodiscard]] bool isEmpty() const;
    /// 是否携带非空 blobId。
    [[nodiscard]] bool hasBlobId() const;
};

/**
 * @brief 图片资产
 *
 * 承载优先级：blobRef.blobId / provider file uri > uri > 内联 data。
 * 供 MessagePart / 流式 ImageOutput 使用。
 */
struct ProviderImageAsset
{
    QString uri;        ///< 远程/本地/厂商 file 地址；与 data / blobRef 择一主承载
    QByteArray data;    ///< 内联图片字节（仅小载荷；历史应转 blob）
    QString mimeType;   ///< 如 image/png、image/jpeg
    QString altText;    ///< 可选替代文本 / 文件名提示
    ProviderBlobRef blobRef; ///< 引用语义（blobId/hash/size/scheme）

    /**
     * @brief 从 URL 构造图片资产
     * @param uri 图片地址
     * @param mimeType MIME 类型，可选
     * @param altText 替代文本，可选
     */
    [[nodiscard]] static ProviderImageAsset fromUrl(const QString &uri,
                                                    const QString &mimeType = {},
                                                    const QString &altText = {});

    /**
     * @brief 从原始字节构造图片资产
     * @param data 内联图片字节
     * @param mimeType MIME 类型
     * @param altText 替代文本，可选
     */
    [[nodiscard]] static ProviderImageAsset fromBytes(const QByteArray &data,
                                                      const QString &mimeType,
                                                      const QString &altText = {});

    /// 从 blob 引用构造（data 留空）
    [[nodiscard]] static ProviderImageAsset fromBlob(const ProviderBlobRef &blob,
                                                      const QString &mimeType = {},
                                                      const QString &altText = {});

    /// 是否携带有效 uri
    [[nodiscard]] bool hasUri() const;
    /// 是否携带内联字节
    [[nodiscard]] bool hasInlineData() const;
    /// 是否携带 blobId
    [[nodiscard]] bool hasBlobRef() const;
    /// uri / data / blob 皆空
    [[nodiscard]] bool isEmpty() const;
};

/**
 * @brief 音频资产
 *
 * 可用于输入（麦克风/文件）或输出（TTS/原生音频）；可选附带转写文本。
 * 历史优先 blobRef，避免多轮内联大音频。
 */
struct ProviderAudioAsset
{
    QString uri;          ///< 音频地址；与 data / blobRef 择一主承载
    QByteArray data;      ///< 内联音频字节（仅小载荷）
    QString mimeType;     ///< 如 audio/wav、audio/mpeg、audio/webm、audio/ogg、audio/pcm
    QString transcript;   ///< 可选：已知转写（输入预识别或输出附带）
    int durationMs = 0;   ///< 可选时长（毫秒），未知为 0
    int sampleRate = 0;   ///< 可选采样率，未知为 0
    QString voice;        ///< 可选 TTS 音色标识
    ProviderBlobRef blobRef; ///< 引用语义

    /**
     * @brief 从 URL 构造音频资产
     * @param uri 音频地址
     * @param mimeType MIME 类型，可选
     * @param transcript 转写文本，可选
     */
    [[nodiscard]] static ProviderAudioAsset fromUrl(const QString &uri,
                                                    const QString &mimeType = {},
                                                    const QString &transcript = {});

    /**
     * @brief 从原始字节构造音频资产
     * @param data 内联音频字节
     * @param mimeType MIME 类型
     * @param transcript 转写文本，可选
     */
    [[nodiscard]] static ProviderAudioAsset fromBytes(const QByteArray &data,
                                                      const QString &mimeType,
                                                      const QString &transcript = {});

    /// 从 blob 引用构造（data 留空）
    [[nodiscard]] static ProviderAudioAsset fromBlob(const ProviderBlobRef &blob,
                                                      const QString &mimeType = {},
                                                      const QString &transcript = {});
    /// 是否携带有效 uri
    [[nodiscard]] bool hasUri() const;
    /// 是否携带内联字节
    [[nodiscard]] bool hasInlineData() const;
    /// 是否携带 blobId
    [[nodiscard]] bool hasBlobRef() const;
    /// uri / data / blob 皆空
    [[nodiscard]] bool isEmpty() const;
};

/**
 * @brief 视频资产（输入为主；uri / blob / 内联字节）
 *
 * 对应 Gemini inlineData/fileData（video MIME）等视频输入。
 */
struct ProviderVideoAsset
{
    QString uri;          ///< 视频地址；与 data / blobRef 择一主承载
    QByteArray data;      ///< 内联字节（仅小载荷）
    QString mimeType;     ///< 如 video/mp4、video/webm
    QString altText;      ///< 可选说明
    int startMs = 0;      ///< 可选截取起点（毫秒）
    int endMs = 0;        ///< 可选截取终点；0 表示未指定
    double fps = 0.0;     ///< 可选采样帧率；0 表示默认
    ProviderBlobRef blobRef; ///< 引用语义

    /**
     * @brief 从 URL 构造视频资产
     * @param uri 视频地址
     * @param mimeType MIME 类型，可选
     * @param altText 说明文本，可选
     */
    [[nodiscard]] static ProviderVideoAsset fromUrl(const QString &uri,
                                                    const QString &mimeType = {},
                                                    const QString &altText = {});
    /**
     * @brief 从原始字节构造视频资产
     * @param data 内联视频字节
     * @param mimeType MIME 类型
     * @param altText 说明文本，可选
     */
    [[nodiscard]] static ProviderVideoAsset fromBytes(const QByteArray &data,
                                                      const QString &mimeType,
                                                      const QString &altText = {});
    /// 从 blob 引用构造（data 留空）
    [[nodiscard]] static ProviderVideoAsset fromBlob(const ProviderBlobRef &blob,
                                                      const QString &mimeType = {},
                                                      const QString &altText = {});
    /// 是否携带有效 uri
    [[nodiscard]] bool hasUri() const;
    /// 是否携带内联字节
    [[nodiscard]] bool hasInlineData() const;
    /// 是否携带 blobId
    [[nodiscard]] bool hasBlobRef() const;
    /// uri / data / blob 皆空
    [[nodiscard]] bool isEmpty() const;
};

// ── 工具规格（线路侧） ──

/**
 * @brief 暴露给模型的工具定义（线路侧，无 permissionKind）
 *
 * 由 ToolSpec 经 toProviderToolSpecification() 投影而来，进入 ProviderRequest.tools。
 * 与 tools/ToolTypes.h 的 ToolSpec 平行：权限审批留在工具域，协议侧只带模型可见字段。
 */
struct ProviderToolSpecification
{
    QString name;              ///< 工具名（模型调用时使用）
    QString description;       ///< 给模型看的功能说明
    QJsonObject inputSchema;   ///< JSON Schema：参数结构
    /// 严格符合 schema（Anthropic strict:true 等）；false=默认
    bool strictSchema = false;
    /**
     * 延迟加载（Anthropic defer_loading / OpenAI tool search defer_loading）。
     * true：初始 system 不展开完整定义，待 tool_search 后再注入。
     */
    bool deferLoading = false;
    /**
     * 允许的调用者。
     * 空=默认（通常仅 direct）；wire 字符串只在 adapter 边界转换。
     * OpenAI allowed_callers；Anthropic allowed_callers。
     */
    QList<ProviderCallerKind> allowedCallers;
    /**
     * 可选输出 JSON Schema（OpenAI function.output_schema，供 programmatic 调用可靠解析）。
     * 空对象=未指定。
     */
    QJsonObject outputSchema;
};

// ── Token / 错误 ──

/// 一次请求/响应的 token 用量统计
struct ProviderUsage
{
    int inputTokens = 0;       ///< 输入 token
    int outputTokens = 0;      ///< 输出 token
    int cacheReadTokens = 0;   ///< 缓存命中读取 token（若厂商支持）
    int cacheWriteTokens = 0;  ///< 写入缓存 token（若厂商支持）
    /**
     * 思考/推理 token（Gemini total_thought_tokens / thinking tokens 等）。
     * 0=未报告或未使用思考。
     */
    int thoughtTokens = 0;
    qint64 sequence = 0;       ///< 事件序号（流式对齐用）
};

/**
 * @brief 协议层统一错误描述
 *
 * 覆盖校验失败、传输失败、厂商 API 错误等场景。
 */
struct ProviderError
{
    QString code;              ///< 稳定错误码（如 empty_input、transport_failed）
    QString message;           ///< 人类可读说明
    bool retryable = false;    ///< 是否建议重试（瞬时过载类）
    int retryAfterMs = -1;     ///< 服务端建议等待时长（-1=未提供）
    int attempts = 0;          ///< 本 turn 已发生的重试次数（首次失败为 0）
    QJsonObject providerRaw;   ///< 厂商原始错误体（可选，便于诊断）
    qint64 sequence = 0;       ///< 事件序号

    /// code 或 message 任一非空则视为有效错误
    [[nodiscard]] bool isValid() const;
};

// 传输层类型已迁至 ProviderAdapterTypes.h（非账本 IR）。

// ── 模型能力 ──

/**
 * @brief 某模型/端点的能力声明
 *
 * **主存储**为 flags（ProviderCapability 集合），避免无限 bool 字段。
 * supportsXxx() 为便捷只读，与 flags 同步。
 * 服务端工具细粒度用 supportedServerTools 短名列表。
 *
 * ## 值语义
 * 能力表按值配置、按值传入 validate；运行中不要共享可变 ModelCapabilities 引用给多线程改 flags。
 */
struct ModelCapabilities
{
    QString modelId; ///< 模型标识
    /**
     * 能力键集合（主存储）。
     * 默认含 StatelessHistory（与历史 bool 默认 true 一致）。
     */
    QSet<ProviderCapability> flags{ProviderCapability::StatelessHistory};
    /**
     * 细粒度服务端工具短名白名单（web_search / code_interpreter / …）。
     * 空 + has(ServerTools)：不限制具体短名；
     * 非空：Request 中 ServerTool* name 必须落在此集合。
     */
    QStringList supportedServerTools;

    /// 是否启用指定能力键。
    [[nodiscard]] bool has(ProviderCapability c) const;
    /// 启用指定能力键并返回自身引用。
    ModelCapabilities &enable(ProviderCapability c);
    /// 禁用指定能力键并返回自身引用。
    ModelCapabilities &disable(ProviderCapability c);
    /// 按开关设置能力键并返回自身引用。
    ModelCapabilities &set(ProviderCapability c, bool on);

    // ── 便捷只读（读 flags，不另存 bool）──
    /// 是否支持文本输入。
    [[nodiscard]] bool supportsTextInput() const;
    /// 是否支持图片输入。
    [[nodiscard]] bool supportsImageInput() const;
    /// 是否支持音频输入。
    [[nodiscard]] bool supportsAudioInput() const;
    /// 是否支持文档输入。
    [[nodiscard]] bool supportsDocumentInput() const;
    /// 是否支持视频输入。
    [[nodiscard]] bool supportsVideoInput() const;
    /// 是否支持文本输出。
    [[nodiscard]] bool supportsTextOutput() const;
    /// 是否支持图片输出。
    [[nodiscard]] bool supportsImageOutput() const;
    /// 是否支持音频输出。
    [[nodiscard]] bool supportsAudioOutput() const;
    /// 是否支持客户端工具调用。
    [[nodiscard]] bool supportsToolCalling() const;
    /// 是否支持服务端工具。
    [[nodiscard]] bool supportsServerTools() const;
    /// 是否支持思考/推理。
    [[nodiscard]] bool supportsReasoning() const;
    /// 是否支持工具选择策略。
    [[nodiscard]] bool supportsToolChoice() const;
    /// 是否支持最大输出 token。
    [[nodiscard]] bool supportsMaxOutputTokens() const;
    /// 是否支持文本引用。
    [[nodiscard]] bool supportsCitations() const;
    /// 是否支持结构化输出格式。
    [[nodiscard]] bool supportsResponseFormat() const;
    /// 是否支持 top_p 采样。
    [[nodiscard]] bool supportsSamplingTopP() const;
    /// 是否支持随机种子。
    [[nodiscard]] bool supportsSamplingSeed() const;
    /// 是否支持停止序列。
    [[nodiscard]] bool supportsSamplingStop() const;
    /// 是否支持 presence_penalty。
    [[nodiscard]] bool supportsPresencePenalty() const;
    /// 是否支持 frequency_penalty。
    [[nodiscard]] bool supportsFrequencyPenalty() const;
    /// 是否支持提示缓存。
    [[nodiscard]] bool supportsPromptCache() const;
    /// 是否支持 logprobs。
    [[nodiscard]] bool supportsLogprobs() const;
    /// 是否支持后台长任务。
    [[nodiscard]] bool supportsBackgroundExecution() const;
    /// 是否支持 response include。
    [[nodiscard]] bool supportsResponseInclude() const;
    /// 是否支持程序化工具调用。
    [[nodiscard]] bool supportsProgrammaticToolCalling() const;
    /// 是否支持工具搜索。
    [[nodiscard]] bool supportsToolSearch() const;
    /// 是否支持厂商 MCP 审批。
    [[nodiscard]] bool supportsMcpApproval() const;
    /// 是否支持上下文压缩。
    [[nodiscard]] bool supportsCompaction() const;
    /// 是否支持 top_k 采样。
    [[nodiscard]] bool supportsTopK() const;
    /// 是否支持厂商侧缓存资源。
    [[nodiscard]] bool supportsCachedContent() const;
    /// 是否支持媒体分辨率提示。
    [[nodiscard]] bool supportsMediaResolution() const;
    /// 是否支持 URL 上下文工具。
    [[nodiscard]] bool supportsUrlContext() const;
    /// 是否支持 Google Maps grounding。
    [[nodiscard]] bool supportsGoogleMapsGrounding() const;
    /// 是否支持无状态历史回放。
    [[nodiscard]] bool supportsStatelessHistory() const;
    /// 是否支持有状态续跑。
    [[nodiscard]] bool supportsContinuation() const;

    /// 是否声明支持某服务端工具短名
    [[nodiscard]] bool supportsServerToolName(const QString &name) const;

    /**
     * @brief 文本 + 客户端工具基线能力
     * @param modelId 可选模型标识
     */
    [[nodiscard]] static ModelCapabilities textToolsBaseline(const QString &modelId = {});
    /**
     * @brief Agent 多模态基线（在 textTools 上叠加图/音/视频/推理/服务端工具等）
     * @param modelId 可选模型标识
     */
    [[nodiscard]] static ModelCapabilities agentMultimodalBaseline(const QString &modelId = {});
};

/**
 * @brief 从 API 错误响应 JSON 体中提取错误消息
 * @param body 原始响应体
 * @return 尽力解析得到的消息；失败返回空字符串
 * @note 兼容 `error.message` 对象字段、`error` 字符串字段与顶层 `message`
 */
[[nodiscard]] QString extractApiErrorMessage(const QByteArray &body);

/**
 * @brief 将 JSON 值压成紧凑字符串（字符串原样；对象/数组 Compact JSON；其余 toString）
 * @param value 任意 JSON 值
 */
[[nodiscard]] QString compactJson(const QJsonValue &value);

/**
 * @brief 从 uri 字符串推断 ProviderUriScheme
 * @param uri 原始 uri
 */
[[nodiscard]] ProviderUriScheme inferUriScheme(const QString &uri);

/// 已知服务端工具短名？（ProviderServerToolName 登记表）
[[nodiscard]] bool isKnownServerToolName(const QString &name);
/**
 * @brief Request.metadata 键是否允许进入请求（白名单前缀护栏）
 *
 * 允许：空键拒绝；键以 "x-" / "ext." / "adapter." 开头；或落在内置白名单。
 * 禁止用 metadata 存对话正文 / 历史 items。
 */
[[nodiscard]] bool isAllowedRequestMetadataKey(const QString &key);

/**
 * @brief ServerToolResult.details 顶层键是否为可移植摘要键
 *
 * 允许：query / results / url / title / snippet / code / output / status / error / sources
 * 及 "x-" 前缀扩展。禁止把厂商整包 raw 当 details。
 * 工具结果摘要扩展键见 isAllowedToolDetailsKey。
 */
[[nodiscard]] bool isAllowedServerToolDetailKey(const QString &key);

/**
 * @brief MessageEnd.logprobs 顶层键是否允许
 *
 * 允许：content / token / logprob / top / bytes 及 "x-" 前缀。
 */
[[nodiscard]] bool isAllowedLogprobsKey(const QString &key);

/**
 * @brief 浅校验 logprobs 对象（顶层键白名单）
 *
 * 非空时：所有顶层键须在白名单；且必须含 content 或至少一个 x- 扩展键。
 */
[[nodiscard]] bool validateLogprobsObject(const QJsonObject &logprobs, QString *error = nullptr);

/**
 * @brief ResponseMetadata.vendorUsageRaw 顶层键是否允许（诊断扩展位）
 *
 * 允许常见 token 计数字段及 "x-" 前缀。
 * 权威用量仍是 ProviderUsage / portableUsage，不得用 raw 替代。
 */
[[nodiscard]] bool isAllowedVendorUsageRawKey(const QString &key);

/**
 * @brief 浅校验 vendorUsageRaw
 * @param forbidNonEmpty true 时禁止任何非空 raw（账本/UI 路径推荐）
 */
[[nodiscard]] bool validateVendorUsageRawObject(const QJsonObject &raw,
                                                QString *error = nullptr,
                                                bool forbidNonEmpty = false);

/**
 * @brief details 可移植键（server tool + 工具结果摘要扩展）
 *
 * 在 isAllowedServerToolDetailKey 上额外允许 summary / payloadType / hasPayload。
 */
[[nodiscard]] bool isAllowedToolDetailsKey(const QString &key);

/// 浅校验 details 对象顶层键
[[nodiscard]] bool validateToolDetailsObject(const QJsonObject &details, QString *error = nullptr);

Q_DECLARE_METATYPE(ProviderBlobRef)
Q_DECLARE_METATYPE(ProviderImageAsset)
Q_DECLARE_METATYPE(ProviderAudioAsset)
Q_DECLARE_METATYPE(ProviderVideoAsset)
Q_DECLARE_METATYPE(ProviderToolSpecification)
Q_DECLARE_METATYPE(ProviderUsage)
Q_DECLARE_METATYPE(ProviderError)
Q_DECLARE_METATYPE(ModelCapabilities)
