// X-macro 字段清单：一份定义同时驱动 struct 成员 + Q_PROPERTY + toJson/fromJson
// 以及 Host 白名单键名（host_api 只展开 #Name，不求值 Default）。
// 住在 shared：runtime 与 host_api 共用，契约库不依赖 runtime/ 路径。
// 格式：GD_FIELD(类型, 名称, 默认值)；GD_ENUM_FIELD(类型, 名称, 枚举默认, 字符串默认)
// 展开前必须同时定义两个宏。勿在注释里写 GD_FIELD(...) / GD_ENUM_FIELD(...) 形态。
// 求值 Default 的翻译单元须先可见 ModelTokenDefaults。

// ── 模型 ──
GD_FIELD(QString, providerType,        QString())
GD_FIELD(QString, modelName,           QString())
/// 当前模型上下文窗口（token）；由 Core 按 user>cache>default resolve 后写入，客户端只读
/// 默认引用 ModelTokenDefaults::kContextWindow（唯一字面量源）
GD_FIELD(qint64,  contextWindow,       ModelTokenDefaults::kContextWindow)
/// 当前模型最大输出（token）；由 Core 按 user>cache>default resolve 后写入，客户端只读
/// 默认引用 ModelTokenDefaults::kMaxOutputTokens（唯一字面量源）
GD_FIELD(qint64,  maxOutputTokens,     ModelTokenDefaults::kMaxOutputTokens)
/// 最大输出来源：user | cache | default（只读投影，写走 SetModelMeta）
GD_FIELD(QString, maxOutputTokensSource, QStringLiteral("default"))

// ── 策略 ──
GD_ENUM_FIELD(AgentMode,    agentMode,    AgentMode::Normal, QStringLiteral("normal"))
GD_ENUM_FIELD(ToolScope,    toolScope,    ToolScope::Full,   QStringLiteral("full"))
GD_ENUM_FIELD(ApprovalMode, approvalMode, ApprovalMode::Ask, QStringLiteral("ask"))

// ── 推理 ──
GD_FIELD(bool,    reasoningEnabled,    false)
GD_FIELD(QString, reasoningEffort,     QString())

// ── 身份 ──
GD_FIELD(QString, systemPrompt,        QString())

// ── 运行限制 ──
GD_FIELD(int,     maxInternalSteps,        200)
GD_FIELD(int,     modelResponseTimeoutSecs, 300)
// maxRetries：Provider 层瞬时错误（429/5xx/529）自动重试预算（不含首次）；看门狗只兜底无响应
GD_FIELD(int,     maxRetries,              5)
GD_FIELD(QString, defaultShell,            QStringLiteral("powershell"))

// ── 邮箱 ──
/// 收件箱最大未确认消息数（pending+in-flight）；0 = 不限
GD_FIELD(int,     maxInboxMessages,        0)
/// 单条收件箱消息 content 最大字符数；0 = 不限
GD_FIELD(int,     maxInboxMessageSize,     0)

// ── 压缩 ──
GD_FIELD(bool,    compactEnabled,              true)
/// 统一触发上限（token）；与 contextWindow 取 min 后再减预留。<=0 表示不设统一上限。
GD_FIELD(qint64,  compactTriggerTokens,        256000)
/// 触发预留（token）：threshold = min(contextWindow, compactTriggerTokens) - reserve
GD_FIELD(qint64,  compactReserveTokens,        16384)
GD_FIELD(qint64,  compactTargetTokens,         40000)
GD_FIELD(int,     compactMaxRetries,           2)
GD_FIELD(qint64,  compactUserMessageTokenBudget, 20000)
GD_FIELD(int,     compactMaxOutputTokens,      80000)
/// 轮后段摘要总开关（仅主代理生效；子代理忽略）
GD_FIELD(bool,    summaryEnabled,              true)
/// 段摘要入队：自上次成功摘要起累计新增估 token 阈值（默认 180000）
GD_FIELD(qint64,  summarySegmentTokens,        180000)
/// 组装模型视图时近尾完整轮数 K；并单独醒目注入最近 K 条用户原文
GD_FIELD(int,     summaryRecentTurns,          5)

// ── 会话原生 ──
GD_FIELD(QString, workingDirectory,     QString())
GD_FIELD(QString, credentialInstanceId, QString())
