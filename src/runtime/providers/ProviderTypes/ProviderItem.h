#pragma once

/**
 * @file ProviderItem.h
 * @brief 终态条目：MessagePart + ProviderItem（入出同型）
 */

#include "ProviderCommon.h"
#include "tools/ToolTypes.h"

#include <QJsonObject>
#include <QList>
#include <QString>

// ── 枚举 ──

/**
 * @brief 统一终态条目种类 —— 入站 Request.items 与出站 MessageEnd.outputItems 共用
 *
 * @note 故意不设 Opaque（厂商原始 JSON 黑盒条目）。
 *       未知厂商块不得 makeOpaque 进账本；应在 adapter 内：映射为一等 Item、
 *       压成可读文本、仅 UI/日志提示、靠 continuationId 续状态，或将来正式加 kind。
 *       请求侧临时旋钮用 ProviderRequest::metadata，且不得承载核心语义。
 * @see ProviderTypes.h 设计原则第 4 条；docs/provider-protocol.md
 *
 * 分层（避免 IR 滑成某家 SDK 全集）：
 * - **Core**（所有 adapter 必须认识）：UserMessage / AssistantMessage /
 *   FunctionCall / FunctionCallOutput / Reasoning
 * - **Extended**（能力门控；未声明 supports* 时 validate 拒绝）：
 *   ServerTool* / Program* / Approval*（厂商 MCP 审批）/ Compaction
 *
 * 客户端工具 vs 服务端工具 vs 编排/审批：
 * - FunctionCall / FunctionCallOutput：由本机 ToolRuntime 执行（agent 工具）
 * - ServerToolCall / ServerToolResult：厂商侧执行；name 用稳定短名
 * - Program / ProgramOutput：OpenAI PTC；fingerprint 必回放
 * - ApprovalRequest / ApprovalResponse：**厂商 MCP 审批线路**（≠ 本机工具权限审批
 *   ToolPermissionApprovalRequest，见 tools/ToolTypes.h）
 * - Compaction：上下文压缩摘要
 */
enum class ProviderItemKind {
    // ── Core ──
    UserMessage,         ///< 用户消息（可含文本/图片/音频/文档 parts）
    AssistantMessage,    ///< 助手消息（可含文本/图片/音频/文档 parts）
    FunctionCall,        ///< 客户端工具调用（本机执行）
    FunctionCallOutput,  ///< 客户端工具结果（回灌给模型）
    Reasoning,           ///< 一等推理/思考块（账本终态优先；见 reasoning 单轨约定）
    // ── Extended（能力门控）──
    ServerToolCall,      ///< 厂商服务端工具调用（非本机 ToolRuntime）
    ServerToolResult,    ///< 厂商服务端工具结果（可携摘要 + 可移植 details）
    Program,             ///< 程序化工具编排块（OpenAI program；须回放 fingerprint）
    ProgramOutput,       ///< 程序化工具编排结果（OpenAI program_output）
    ApprovalRequest,     ///< 厂商 MCP 审批请求（非本机权限审批）
    ApprovalResponse,    ///< 厂商 MCP 审批答复
    Compaction,          ///< 上下文压缩条目（OpenAI compaction 等）
    // 无 Opaque：禁止 rawItem / 厂商私货整包作为线路终态
};

/// 是否为 Core kind（所有 adapter 基线）
[[nodiscard]] inline bool isCoreProviderItemKind(ProviderItemKind kind)
{
    switch (kind) {
    case ProviderItemKind::UserMessage:
    case ProviderItemKind::AssistantMessage:
    case ProviderItemKind::FunctionCall:
    case ProviderItemKind::FunctionCallOutput:
    case ProviderItemKind::Reasoning:
        return true;
    default:
        return false;
    }
}

/// 是否为 Extended kind（需能力位）
[[nodiscard]] inline bool isExtendedProviderItemKind(ProviderItemKind kind)
{
    return !isCoreProviderItemKind(kind);
}

/**
 * @brief 服务端工具稳定短名（adapter 从厂商 type/name 归一化到此）
 *
 * 来源对照（语义，非 wire）：
 * - web_search ← OpenAI web_search_call / Anthropic web_search / Gemini google_search
 * - file_search ← file_search_call / FILE_SEARCH
 * - code_interpreter ← code_interpreter_call / Anthropic code_execution
 * - computer ← computer_call（厂商托管时；Anthropic computer 若本机执行则 FunctionCall）
 * - web_fetch ← web_fetch
 * - image_generation ← image_generation_call
 * - mcp ← mcp_call
 * - mcp_list_tools ← mcp_list_tools
 * - tool_search ← tool_search_call / Anthropic tool_search_tool_*
 * - local_shell ← local_shell_call（若厂商托管）
 * - shell ← function_shell / shell
 * - apply_patch ← apply_patch_tool_call（若厂商托管；本机执行则 FunctionCall）
 * - advisor ← Anthropic advisor
 * - url_context ← Gemini url_context / urlContext
 * - google_maps ← Gemini google_maps / maps grounding
 */
namespace ProviderServerToolName {
inline const QString WebSearch = QStringLiteral("web_search"); ///< 含 Gemini google_search
inline const QString FileSearch = QStringLiteral("file_search");
inline const QString CodeInterpreter = QStringLiteral("code_interpreter"); ///< 含 Gemini/Anthropic code_execution
inline const QString Computer = QStringLiteral("computer");
inline const QString WebFetch = QStringLiteral("web_fetch");
inline const QString ImageGeneration = QStringLiteral("image_generation");
inline const QString Mcp = QStringLiteral("mcp");
inline const QString McpListTools = QStringLiteral("mcp_list_tools");
inline const QString ToolSearch = QStringLiteral("tool_search");
inline const QString LocalShell = QStringLiteral("local_shell");
inline const QString Shell = QStringLiteral("shell");
inline const QString ApplyPatch = QStringLiteral("apply_patch");
inline const QString Advisor = QStringLiteral("advisor");
inline const QString UrlContext = QStringLiteral("url_context"); ///< Gemini URL context 内置工具
inline const QString GoogleMaps = QStringLiteral("google_maps"); ///< Gemini Maps grounding
}

// ── 可移植引用 / 文档 / 视频 ──

/**
 * @brief 文本片段上的引用（citation）
 *
 * 对应 Anthropic text.citations、部分 Responses 注解的可移植子集。
 */
struct ProviderCitation
{
    QString url;       ///< 来源 URL（可空）
    QString title;     ///< 标题（可空）
    QString snippet;   ///< 摘录（可空）
    int startIndex = -1; ///< 在所属 text 中的起始下标；未知为 -1
    int endIndex = -1;   ///< 结束下标（半开或厂商约定）；未知为 -1
};

/**
 * @brief 文档资产（PDF / 纯文本文件等，非图片）
 *
 * 对应 Anthropic document block、部分 file 输入的可移植子集。
 */
struct ProviderDocumentAsset
{
    QString uri;        ///< 文档地址；与 data / blobRef 择一主承载
    QByteArray data;    ///< 内联字节（仅小载荷）
    QString mimeType;   ///< 如 application/pdf、text/plain
    QString title;      ///< 可选标题
    QString context;    ///< 可选上下文说明（给模型的额外提示）
    ProviderBlobRef blobRef; ///< 引用语义

    /**
     * @brief 从 URL 构造文档资产
     * @param uri 文档地址
     * @param mimeType MIME 类型，可选
     * @param title 标题，可选
     */
    [[nodiscard]] static ProviderDocumentAsset fromUrl(const QString &uri,
                                                       const QString &mimeType = {},
                                                       const QString &title = {});
    /**
     * @brief 从原始字节构造文档资产
     * @param data 内联文档字节
     * @param mimeType MIME 类型
     * @param title 标题，可选
     */
    [[nodiscard]] static ProviderDocumentAsset fromBytes(const QByteArray &data,
                                                         const QString &mimeType,
                                                         const QString &title = {});
    /// 从 blob 引用构造（data 留空）
    [[nodiscard]] static ProviderDocumentAsset fromBlob(const ProviderBlobRef &blob,
                                                         const QString &mimeType = {},
                                                         const QString &title = {});
    /// 是否携带有效 uri
    [[nodiscard]] bool hasUri() const;
    /// 是否携带内联字节
    [[nodiscard]] bool hasInlineData() const;
    /// 是否携带 blobId
    [[nodiscard]] bool hasBlobRef() const;
    /// uri / data / blob 皆空
    [[nodiscard]] bool isEmpty() const;
};

// ── 消息部件 ──

/**
 * @brief 用户/助手消息内部的一个 content part
 *
 * Text / Image / Audio / Document / Video 五选一有效。
 */
struct ProviderMessagePart
{
    ProviderPartKind kind = ProviderPartKind::Text; ///< 部件种类
    QString text;                                   ///< kind==Text 时有效
    ProviderImageAsset image;                       ///< kind==Image 时有效
    ProviderAudioAsset audio;                       ///< kind==Audio 时有效
    ProviderDocumentAsset document;                 ///< kind==Document 时有效
    ProviderVideoAsset video;                       ///< kind==Video 时有效
    QList<ProviderCitation> citations;              ///< kind==Text 时可选引用列表
    /**
     * 可选提示缓存策略（Anthropic cache_control 等）。
     * 仅请求编码提示，不改变 part 语义；None 表示不指定。
     */
    ProviderCachePolicy cachePolicy = ProviderCachePolicy::None;
    /**
     * 媒体分辨率提示（Gemini mediaResolution / videoMetadata 相关可移植子集）。
     * 空=未指定；常见值 low / medium / high（由 adapter 映射枚举）。
     * 主要用于 Image / Video / Document 输入 part。
     */
    QString mediaResolution;

    /**
     * @brief 构造文本部件
     * @param text 文本内容
     * @param citations 可选引用
     */
    [[nodiscard]] static ProviderMessagePart makeText(const QString &text,
                                                       const QList<ProviderCitation> &citations = {});

    /**
     * @brief 构造图片部件
     * @param image 图片资产
     */
    [[nodiscard]] static ProviderMessagePart makeImage(const ProviderImageAsset &image);

    /**
     * @brief 构造音频部件
     * @param audio 音频资产
     */
    [[nodiscard]] static ProviderMessagePart makeAudio(const ProviderAudioAsset &audio);

    /**
     * @brief 构造文档部件
     * @param document 文档资产
     */
    [[nodiscard]] static ProviderMessagePart makeDocument(const ProviderDocumentAsset &document);

    /**
     * @brief 构造视频部件
     * @param video 视频资产
     */
    [[nodiscard]] static ProviderMessagePart makeVideo(const ProviderVideoAsset &video);

    /// 调试用简短描述
    [[nodiscard]] QString toDebugString() const;
};

/// 拼接 parts 中全部 Text 部件正文（非 Text 部件忽略）。
[[nodiscard]] QString joinedText(const QList<ProviderMessagePart> &parts);

// ── ProviderItem ──

/**
 * @brief 线路上的一条终态消息项（对话真相的基本单位）
 *
 * - 入：装入 ProviderRequest.items（历史 + 本轮）
 * - 出：来自 ProviderMessageEnd.outputItems（写账本权威源）
 * - 推荐通过工厂方法构造，再以 validate() 入账
 *
 * ## 值语义（immutability 在本模块的形态）
 * - 本类型是 **值类型快照**（可拷贝、可赋值），不是共享可变实体。
 * - 跨线程 / 跨层（账本 ↔ UI ↔ adapter）**只传拷贝或按值**，禁止长期持有可写引用
 *   并就地改同一实例（否则会产生隐蔽副作用）。
 * - 「不可变」在此体现为：入账前 validate，写入新快照；需要变更时构造/拷贝后改副本，
 *   而不是多模块共享 ProviderItem* 原地 mutate。
 * - 字段 public 是为 Qt MetaType / 信号 / 调试友好，**不是**鼓励跨层共享可变状态。
 *
 * 字段按 kind 选用：
 * - 消息类：parts
 * - 客户端/服务端工具：callId / name / arguments / rawArguments / output / callerKind
 * - Reasoning：reasoningText（+ 可选 signature）；仅 kind==Reasoning
 * - ServerToolResult：output + outputParts + details（可移植摘要，不是厂商 raw 整包）
 * - Program：callId + programCode + programFingerprint
 * - ProgramOutput：callId + output + status
 * - Approval*：callId / name / rawArguments / serverLabel / approved / approval*
 * - Compaction：compactionSummary（+ itemId；禁止占用 callId）
 */
struct ProviderItem
{
    ProviderItemKind kind = ProviderItemKind::UserMessage; ///< 条目种类

    /**
     * 稳定条目身份（账本去重 / UI key / 压缩锚点 / 调试关联）。
     * **工厂默认生成 UUID**；手工构造时应赋值。与 callId 分离，禁止混用。
     */
    QString itemId;

    /// UserMessage / AssistantMessage：有序 content parts
    QList<ProviderMessagePart> parts;

    /**
     * 工具/编排调用关联 id（**不是**通用 itemId）：
     * - FunctionCall / FunctionCallOutput / ServerTool* / Program*
     * - Approval*：审批请求/答复 id（MCP 线路）
     * Compaction **不得**占用 callId（用 itemId）。
     */
    QString callId;
    /// 工具名：客户端为工具注册名；服务端为稳定短名（web_search、file_search、
    /// code_interpreter、computer、web_fetch 等）
    QString name;
    /// FunctionCall / ServerToolCall：已解析的参数对象
    QJsonObject arguments;
    /// FunctionCall / ServerToolCall：原始参数 JSON 字符串
    QString rawArguments;
    /// FunctionCallOutput / ServerToolResult：回给模型的结果正文或摘要
    QString output;
    /**
     * FunctionCallOutput / ServerToolResult：多模态结果部件（图/文等）。
     * 对应 Gemini functionResponse.parts、Anthropic tool_result content 数组；
     * 仅文本时用 output 即可，parts 可空。
     */
    QList<ProviderMessagePart> outputParts;
    /// FunctionCallOutput / ServerToolResult：是否表示执行失败
    bool isError = false;
    /// 结果是否被截断
    bool wasTruncated = false;

    /**
     * ServerToolResult 的可移植结构化摘要（非厂商 raw 黑盒）。
     * 约定示例键（可选）：
     * - query: string
     * - results: [ { title, url, snippet } ]
     * adapter 只写入跨厂商可理解的字段；看不懂的长尾放不进此处，应压进 output 文本
     * 或 continuationId，禁止整包 raw JSON 冒充 details。
     */
    QJsonObject details;

    /**
     * Reasoning 条目的正文（**仅** kind==Reasoning 时有效）。
     *
     * 命名刻意不用「可往任意消息上贴的 content」语义：
     * 本字段就是独立 Reasoning Item 的 text 载荷，不是 Assistant/FunctionCall 附件。
     *
     * **账本终态单轨（强制）：**
     * - 只允许出现在 ProviderItemKind::Reasoning；
     * - 工厂只通过 makeReasoning() 写入；
     * - validate() 默认 strictReasoningSingleTrack=true，其它 kind 非空则失败；
     * - Adapter 编码 DeepSeek 等 wire 时：从相邻 Reasoning Item 投影到 JSON，
     *   不得把本字段写回非 Reasoning 的账本 items。
     */
    QString reasoningText;
    /**
     * 思考块签名 / 加密态句柄（可移植回放字段，不是任意 Opaque）。
     * - Anthropic：thinking.signature
     * - Gemini Interactions：thought.signature（独立 thought step）
     * - Gemini generateContent：part.thoughtSignature（可挂在 functionCall / 末 text part；
     *   有 tool call 时须原样回放在对应 FunctionCall 上，否则 4xx）
     * - OpenAI Responses：加密 reasoning item 的回放材料（stateless 时须保留）
     * 亦可挂在 FunctionCall / ServerToolCall 上（Gemini 3 多步 tool 时常见）。
     */
    QString reasoningSignature;
    /// 是否为已脱敏/加密的思考块（仅占位续跑，正文可能不可读）
    bool reasoningRedacted = false;
    /**
     * 该思考内容是否必须进入后续请求的 items（厂商硬性回放要求）。
     * DeepSeek：本轮若发生 tool call，则 assistant.reasoning_content 后续各轮必须回传；
     * 无 tool call 时可为 false（API 会忽略）。
     * OpenAI 加密 reasoning / Gemini thoughtSignature（尤其 functionCall 附带）回放时亦应 true。
     * Gemini Interactions store=false：须回放全部 thought + tool steps 的 signature。
     * 账本/adapter 据此决定压缩时是否可丢弃。
     */
    bool reasoningMustReplay = false;
    /**
     * OpenAI 等助手消息 phase（推理模型多段输出时须原样回放）。
     * 仅 AssistantMessage 使用；空=不适用。
     */
    QString assistantPhase;

    /**
     * 调用者种类（枚举）。
     * FunctionCall / FunctionCallOutput / ServerTool* 使用；Unset=模型直接发起。
     * @see ProviderCallerKind
     */
    ProviderCallerKind callerKind = ProviderCallerKind::Unset;
    /**
     * 调用者 id（如 program 的 call_id）。
     * 与 callerKind 成对；回灌 FunctionCallOutput 时须原样带回以恢复 program。
     */
    QString callerId;
    /**
     * MCP 等：服务端标签（server_label）。
     * ApprovalRequest / Mcp ServerTool / FunctionCall 可选。
     */
    QString serverLabel;
    /**
     * Program：生成的代码（OpenAI program.code）。
     * 须原样回放，禁止当 Opaque 丢弃。
     */
    QString programCode;
    /**
     * Program：回放指纹（OpenAI program.fingerprint）。
     * 无状态续聊时必须带回。
     */
    QString programFingerprint;
    /**
     * ProgramOutput / 部分工具：状态枚举。
     * Unset=未指定。
     */
    ProviderItemStatus status = ProviderItemStatus::Unset;
    /**
     * ApprovalResponse：是否批准。
     * 仅 ApprovalResponse 使用。
     */
    bool approved = false;
    /**
     * ApprovalResponse：可选原因；或 ApprovalRequest 的说明。
     */
    QString approvalReason;
    /**
     * ApprovalResponse 指向的 ApprovalRequest id（approval_request_id）。
     */
    QString approvalRequestId;
    /**
     * Compaction：压缩后的摘要文本（可移植；非厂商 raw 整包）。
     */
    QString compactionSummary;

    // ── 工厂：唯一推荐的构造路径 ──

    /** @brief 用户多部件消息 */
    [[nodiscard]] static ProviderItem makeUserMessage(const QList<ProviderMessagePart> &parts);
    /** @brief 纯文本用户消息 */
    [[nodiscard]] static ProviderItem makeUserText(const QString &text);
    /** @brief 用户图片消息（caption 非空时前置 Text part） */
    [[nodiscard]] static ProviderItem makeUserImage(const ProviderImageAsset &image,
                                                     const QString &caption = {});
    /** @brief 用户音频消息 */
    [[nodiscard]] static ProviderItem makeUserAudio(const ProviderAudioAsset &audio,
                                                     const QString &caption = {});
    /** @brief 用户文档消息 */
    [[nodiscard]] static ProviderItem makeUserDocument(const ProviderDocumentAsset &document,
                                                        const QString &caption = {});
    /** @brief 用户视频消息 */
    [[nodiscard]] static ProviderItem makeUserVideo(const ProviderVideoAsset &video,
                                                     const QString &caption = {});

    /**
     * @brief 助手多部件消息（不含 reasoning；思考用 makeReasoning 独立条目）
     */
    [[nodiscard]] static ProviderItem makeAssistantMessage(const QList<ProviderMessagePart> &parts);
    /**
     * @brief 纯文本助手消息（不含 reasoning；思考用 makeReasoning 独立条目）
     */
    [[nodiscard]] static ProviderItem makeAssistantText(const QString &text);
    /** @brief 助手图片输出 */
    [[nodiscard]] static ProviderItem makeAssistantImage(const ProviderImageAsset &image,
                                                          const QString &caption = {});
    /** @brief 助手音频输出 */
    [[nodiscard]] static ProviderItem makeAssistantAudio(const ProviderAudioAsset &audio,
                                                          const QString &caption = {});

    /**
     * @brief 客户端工具调用（原始字段；不含 reasoning 侧轨）
     */
    [[nodiscard]] static ProviderItem makeFunctionCall(const QString &callId,
                                                        const QString &name,
                                                        const QJsonObject &arguments,
                                                        const QString &rawArguments);
    /** @brief 客户端工具调用（从 ToolCall 投影） */
    [[nodiscard]] static ProviderItem makeFunctionCall(const ToolCall &toolCall);
    /** @brief 客户端工具结果（原始字段） */
    [[nodiscard]] static ProviderItem makeFunctionCallOutput(const QString &callId,
                                                              const QString &name,
                                                              const QString &output,
                                                              bool isError = false,
                                                              bool wasTruncated = false,
                                                              const QList<ProviderMessagePart> &outputParts = {});
    /** @brief 客户端工具结果（从 ToolResult 投影） */
    [[nodiscard]] static ProviderItem makeFunctionCallOutput(const ToolResult &result);

    /**
     * @brief 一等推理/思考块
     * @param content 推理正文（redacted 时可为占位或空）
     * @param signature 厂商回放签名（可空）
     * @param redacted 是否脱敏块
     * @param mustReplay 后续请求是否必须带回（DeepSeek tool 轮 / 加密 reasoning）
     */
    [[nodiscard]] static ProviderItem makeReasoning(const QString &content,
                                                     const QString &signature = {},
                                                     bool redacted = false,
                                                     bool mustReplay = false);

    /**
     * @brief 厂商服务端工具调用
     * @param callId 调用 id
     * @param name 稳定短名（web_search / file_search / code_interpreter / computer / web_fetch …）
     * @param arguments 已解析参数
     * @param rawArguments 原始参数 JSON
     */
    [[nodiscard]] static ProviderItem makeServerToolCall(const QString &callId,
                                                          const QString &name,
                                                          const QJsonObject &arguments = {},
                                                          const QString &rawArguments = {});

    /**
     * @brief 厂商服务端工具结果
     * @param callId 对应 ServerToolCall id
     * @param name 工具短名
     * @param output 可读摘要（给模型/UI）
     * @param details 可移植结构化摘要（禁止塞厂商 raw 整包）
     * @param isError 是否失败
     */
    [[nodiscard]] static ProviderItem makeServerToolResult(const QString &callId,
                                                            const QString &name,
                                                            const QString &output,
                                                            const QJsonObject &details = {},
                                                            bool isError = false,
                                                            const QList<ProviderMessagePart> &outputParts = {});

    /**
     * @brief 程序化工具编排块（OpenAI program）
     * @param callId 稳定 call_id
     * @param code 生成的 JavaScript 源码
     * @param fingerprint 必须回放的指纹
     */
    [[nodiscard]] static ProviderItem makeProgram(const QString &callId,
                                                   const QString &code,
                                                   const QString &fingerprint);

    /**
     * @brief 程序化工具编排结果（OpenAI program_output）
     * @param callId 对应 Program.callId
     * @param result 程序最终结果字符串
     * @param status completed / incomplete 等
     */
    [[nodiscard]] static ProviderItem makeProgramOutput(const QString &callId,
                                                         const QString &result,
                                                         const QString &status = QStringLiteral("completed"));
    /** @brief 程序编排结果（状态用枚举） */
    [[nodiscard]] static ProviderItem makeProgramOutput(const QString &callId,
                                                         const QString &result,
                                                         ProviderItemStatus status);

    /**
     * @brief 工具审批请求（OpenAI mcp_approval_request 等）
     * @param requestId 审批请求 id
     * @param name 工具名
     * @param argumentsJson 参数 JSON 字符串
     * @param serverLabel MCP server_label
     */
    [[nodiscard]] static ProviderItem makeApprovalRequest(const QString &requestId,
                                                           const QString &name,
                                                           const QString &argumentsJson = {},
                                                           const QString &serverLabel = {});

    /**
     * @brief 工具审批答复
     * @param responseId 答复 id
     * @param approvalRequestId 对应请求 id
     * @param approved 是否批准
     * @param reason 可选原因
     */
    [[nodiscard]] static ProviderItem makeApprovalResponse(const QString &responseId,
                                                            const QString &approvalRequestId,
                                                            bool approved,
                                                            const QString &reason = {});

    /**
     * @brief 上下文压缩条目
     * @param summary 可移植摘要文本
     * @param itemId 可选稳定条目 id（写入 itemId，**不**占用 callId）
     */
    [[nodiscard]] static ProviderItem makeCompaction(const QString &summary,
                                                      const QString &itemId = {});

    /**
     * @brief 按 kind 校验字段不变量
     * @param error 失败原因
     * @param strictReasoningSingleTrack 默认 true：非 Reasoning kind 不得带 reasoningText
     * @param maxInlineAssetBytes 内联 data 上限；默认 kProviderMaxInlineAssetBytes；
     *        0 = 禁止任何内联 data（账本推荐）；<0 = 不检查体积
     *
     * 写账本前 **必须** 调用；工厂是推荐构造路径，public 字段仍可被误改，靠本函数拦截。
     */
    [[nodiscard]] bool validate(QString *error = nullptr,
                                bool strictReasoningSingleTrack = true,
                                int maxInlineAssetBytes = kProviderMaxInlineAssetBytes) const;

    /// 调试用简短描述
    [[nodiscard]] QString toDebugString() const;
    /// 是否为 User/Assistant 消息
    [[nodiscard]] bool isConversational() const;
    /// 是否为客户端/服务端工具或编排相关条目
    [[nodiscard]] bool isToolRelated() const;
    /// 是否为服务端工具相关
    [[nodiscard]] bool isServerToolRelated() const;
    /// 是否为 Program / ProgramOutput
    [[nodiscard]] bool isProgramRelated() const;
    /// 是否为 ApprovalRequest / ApprovalResponse
    [[nodiscard]] bool isApprovalRelated() const;
    /// parts 中是否含图片
    [[nodiscard]] bool hasImageParts() const;
    /// parts 中是否含音频
    [[nodiscard]] bool hasAudioParts() const;
    /// parts 中是否含文档
    [[nodiscard]] bool hasDocumentParts() const;
    /// parts 中是否含视频
    [[nodiscard]] bool hasVideoParts() const;
};

/**
 * @brief ToolSpec → 线路侧工具规格（去掉 permissionKind）
 */
[[nodiscard]] ProviderToolSpecification toProviderToolSpecification(const ToolSpec &spec);

/**
 * @brief ToolResult → FunctionCallOutput（保留 text / error / truncated；
 *        若 payload 含可识别多模态，调用方可再填 outputParts）
 * @note payload 非空时写入 details 的可移植子集键 summary/payloadType（非 raw 整包）
 */
[[nodiscard]] ProviderItem functionCallOutputFromToolResult(const ToolResult &result);

Q_DECLARE_METATYPE(ProviderCitation)
Q_DECLARE_METATYPE(ProviderDocumentAsset)
Q_DECLARE_METATYPE(ProviderMessagePart)
Q_DECLARE_METATYPE(ProviderItem)
