#include <QtTest>

#include <QTemporaryDir>

#include "agent/ProviderRunLedger.h"
#include "providers/ProviderTypes/ProviderTypes.h"

namespace {

bool itemTextContains(const ProviderItem &item, const QString &needle)
{
    for (const ProviderMessagePart &part : item.parts) {
        if (part.text.contains(needle))
            return true;
    }
    return false;
}

ConversationMessage makeLedgerMessage(const QString &id,
                                      ConversationMessage::Kind kind,
                                      const QString &text,
                                      bool submittedToModel = true)
{
    ConversationMessage message;
    message.id = id;
    message.kind = kind;
    message.text = text;
    message.submittedToModel = submittedToModel;
    return message;
}

ConversationMessage makeToolCallMessage(const QString &id, const ToolCall &call)
{
    ConversationMessage message = makeLedgerMessage(id, ConversationMessage::Kind::ToolCall, {});
    message.toolCall = call;
    message.toolUseId = call.id;
    message.toolName = call.toolName;
    return message;
}

ConversationMessage makeToolResultMessage(const QString &id,
                                          const QString &callId,
                                          const QString &toolName,
                                          const QString &text)
{
    ConversationMessage message =
        makeLedgerMessage(id, ConversationMessage::Kind::ToolResult, text);
    message.toolUseId = callId;
    message.toolName = toolName;
    return message;
}

QJsonObject makeTextProviderItem(const QString &itemId, const QString &text)
{
    return QJsonObject{
        {QStringLiteral("kind"), static_cast<int>(ProviderItemKind::UserMessage)},
        {QStringLiteral("itemId"), itemId},
        {QStringLiteral("parts"),
         QJsonArray{QJsonObject{
             {QStringLiteral("kind"), static_cast<int>(ProviderPartKind::Text)},
             {QStringLiteral("text"), text}}}}};
}

QJsonObject makeLedgerEntryJson(const QString &id,
                                const QString &kind,
                                const QString &text,
                                bool submittedToModel,
                                const QJsonObject &providerItem)
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("status"), QStringLiteral("completed")},
        {QStringLiteral("text"), text},
        {QStringLiteral("submittedToModel"), submittedToModel},
        {QStringLiteral("providerItem"), providerItem}};
}

} // namespace

class ProviderProtocolMigrationTests final : public QObject
{
    Q_OBJECT

private slots:
    void ledgerBuildsOneOrderedItemStream();
    void reasoningReplayMetadataSurvivesPersistence();
    void extendedProviderItemsSurvivePersistence();
    void inlineMediaIsExternalizedFromLedgerJson();
    void requestValidationEnforcesProtocolInvariants();
    void legacyUiEntryDoesNotBecomeProtocolHistory();
    void emptyReasoningItemIsDroppedFromRequest();
    void rollbackUncommittedTurnDropsUnresolvedToolCalls();
    void emptyFunctionCallArgsMustNotOverwriteCompletedToolCall();
    void sessionEventNeverEntersProviderRequest();
    void sessionEventProviderItemDroppedOnRestore();
    void fromJsonRejectsUnsupportedSchemaVersion();
    void fromJsonDowngradesInvalidProviderItemToUiOnly();
    void rollbackRemovesOrphanToolResultAndError();
    void indexesRemainCorrectAfterRemove();
    void buildRequestReportsHydrateError();
};

void ProviderProtocolMigrationTests::ledgerBuildsOneOrderedItemStream()
{
    ProviderRunLedger ledger;

    ConversationMessage user;
    user.id = QStringLiteral("user");
    user.kind = ConversationMessage::Kind::UserText;
    user.text = QStringLiteral("hello");
    user.submittedToModel = true;
    ledger.appendUiIngress(user);

    ConversationMessage reasoning;
    reasoning.id = QStringLiteral("reasoning");
    reasoning.kind = ConversationMessage::Kind::AssistantReasoning;
    reasoning.reasoningContent = QStringLiteral("think");
    reasoning.reasoningSignature = QStringLiteral("signed");
    reasoning.reasoningMustReplay = true;
    reasoning.submittedToModel = true;
    ledger.appendUiIngress(reasoning);

    ToolCall call;
    call.id = QStringLiteral("call-1");
    call.toolName = QStringLiteral("read_file");
    call.input = QJsonObject{{QStringLiteral("path"), QStringLiteral("a.txt")}};
    call.rawInputJson = QStringLiteral("{\"path\":\"a.txt\"}");
    ConversationMessage toolCall;
    toolCall.id = QStringLiteral("tool-call");
    toolCall.kind = ConversationMessage::Kind::ToolCall;
    toolCall.toolCall = call;
    toolCall.submittedToModel = true;
    ledger.appendUiIngress(toolCall);

    ConversationMessage result;
    result.id = QStringLiteral("result");
    result.kind = ConversationMessage::Kind::ToolResult;
    result.toolUseId = call.id;
    result.toolName = call.toolName;
    result.text = QStringLiteral("contents");
    ledger.appendUiIngress(result);

    const ProviderRequestBuild build = ledger.buildRequest({});
    QCOMPARE(build.request.items.size(), 4);
    QCOMPARE(build.request.items[0].kind, ProviderItemKind::UserMessage);
    QCOMPARE(build.request.items[1].kind, ProviderItemKind::Reasoning);
    QCOMPARE(build.request.items[2].kind, ProviderItemKind::FunctionCall);
    QCOMPARE(build.request.items[3].kind, ProviderItemKind::FunctionCallOutput);
    QCOMPARE(build.request.items[1].reasoningSignature, QStringLiteral("signed"));
    QVERIFY(build.request.items[1].reasoningMustReplay);
    QCOMPARE(build.submittedEntryIds, QList<QString>{QStringLiteral("result")});
}

void ProviderProtocolMigrationTests::reasoningReplayMetadataSurvivesPersistence()
{
    ProviderRunLedger source;
    ConversationMessage message;
    message.id = QStringLiteral("reasoning");
    message.kind = ConversationMessage::Kind::AssistantReasoning;
    message.reasoningContent = QStringLiteral("hidden");
    message.reasoningSignature = QStringLiteral("signature");
    message.reasoningRedacted = true;
    message.reasoningMustReplay = true;
    message.providerContinuationId = QStringLiteral("response-42");
    message.submittedToModel = true;
    source.appendUiIngress(message);

    ProviderRunLedger restored;
    restored.fromJson(source.toJson());
    const ProviderRequest request = restored.buildRequest({}).request;

    QCOMPARE(request.items.size(), 1);
    QCOMPARE(request.items.first().kind, ProviderItemKind::Reasoning);
    QCOMPARE(request.items.first().reasoningSignature, QStringLiteral("signature"));
    QVERIFY(request.items.first().reasoningRedacted);
    QVERIFY(request.items.first().reasoningMustReplay);
    // 账本仍保留厂商续接元数据，但默认请求不提升 continuationId（无状态全量回放）。
    QCOMPARE(restored.entries().first().providerContinuationId,
             QStringLiteral("response-42"));
    QVERIFY(request.continuationId.isEmpty());
}

void ProviderProtocolMigrationTests::extendedProviderItemsSurvivePersistence()
{
    ProviderRunLedger source;
    QList<ProviderItem> expected{
        ProviderItem::makeServerToolCall(
            QStringLiteral("srv-1"), ProviderServerToolName::WebSearch,
            QJsonObject{{QStringLiteral("query"), QStringLiteral("Qt 6")}}),
        ProviderItem::makeServerToolResult(
            QStringLiteral("srv-1"), ProviderServerToolName::WebSearch,
            QStringLiteral("result"),
            QJsonObject{{QStringLiteral("query"), QStringLiteral("Qt 6")},
                        {QStringLiteral("results"), QJsonArray{
                            QJsonObject{{QStringLiteral("title"), QStringLiteral("Qt")},
                                        {QStringLiteral("url"), QStringLiteral("https://qt.io")},
                                        {QStringLiteral("snippet"), QStringLiteral("Qt 6")}}}}}),
        ProviderItem::makeProgram(
            QStringLiteral("program-1"), QStringLiteral("return 42"),
            QStringLiteral("fingerprint")),
        ProviderItem::makeProgramOutput(
            QStringLiteral("program-1"), QStringLiteral("42")),
        ProviderItem::makeApprovalRequest(
            QStringLiteral("approval-1"), QStringLiteral("deploy"),
            QStringLiteral("{\"env\":\"test\"}"), QStringLiteral("server")),
        ProviderItem::makeApprovalResponse(
            QStringLiteral("approval-response-1"), QStringLiteral("approval-1"),
            true, QStringLiteral("approved")),
        ProviderItem::makeCompaction(QStringLiteral("summary"), QStringLiteral("compact-1"))
    };

    ProviderImageAsset image;
    image.uri = QStringLiteral("https://example.invalid/image.png");
    image.mimeType = QStringLiteral("image/png");
    ProviderItem assistant = ProviderItem::makeAssistantMessage(
        {ProviderMessagePart::makeText(QStringLiteral("caption")),
         ProviderMessagePart::makeImage(image)});
    expected.prepend(assistant);

    for (qsizetype i = 0; i < expected.size(); ++i) {
        if (expected[i].itemId.isEmpty())
            expected[i].itemId = QStringLiteral("item-%1").arg(i);
        source.appendProviderItem(expected[i], QStringLiteral("turn-1"),
                                  QStringLiteral("continuation-1"));
    }

    ProviderRunLedger restored;
    restored.fromJson(source.toJson());
    const QList<ProviderItem> actual = restored.providerItems();
    QCOMPARE(actual.size(), expected.size());
    for (qsizetype i = 0; i < expected.size(); ++i) {
        QCOMPARE(actual[i].kind, expected[i].kind);
        QCOMPARE(actual[i].itemId, expected[i].itemId);
        QCOMPARE(actual[i].callId, expected[i].callId);
        QCOMPARE(actual[i].name, expected[i].name);
        QCOMPARE(actual[i].output, expected[i].output);
        QCOMPARE(actual[i].programCode, expected[i].programCode);
        QCOMPARE(actual[i].programFingerprint, expected[i].programFingerprint);
        QCOMPARE(actual[i].approvalRequestId, expected[i].approvalRequestId);
        QCOMPARE(actual[i].compactionSummary, expected[i].compactionSummary);
    }
    QCOMPARE(actual.first().parts.size(), 2);
    QCOMPARE(actual.first().parts[1].image.uri, image.uri);
    QCOMPARE(restored.entries().first().imageOutput.uri, image.uri);
    QVERIFY(restored.entries()[3].isCompactExempt()); // Program fingerprint
    QVERIFY(restored.entries()[5].isCompactExempt()); // Approval request
}

void ProviderProtocolMigrationTests::inlineMediaIsExternalizedFromLedgerJson()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ProviderRunLedger ledger;
    ledger.setBlobRoot(tempDir.path());
    ProviderImageAsset image =
        ProviderImageAsset::fromBytes(QByteArrayLiteral("png-bytes"),
                                      QStringLiteral("image/png"));
    ledger.appendProviderItem(
        ProviderItem::makeUserMessage({ProviderMessagePart::makeImage(image)}));

    const QList<ProviderItem> stored = ledger.providerItems();
    QCOMPARE(stored.size(), 1);
    // providerItems() 返回 hydrate 后的完整数据
    QVERIFY(!stored.first().parts.first().image.data.isEmpty());
    QVERIFY(stored.first().parts.first().image.blobRef.hasBlobId());

    const QJsonObject persistedItem =
        ledger.toJson().value(QStringLiteral("entries")).toArray().first().toObject()
            .value(QStringLiteral("providerItem")).toObject();
    const QJsonObject persistedImage =
        persistedItem.value(QStringLiteral("parts")).toArray().first().toObject()
            .value(QStringLiteral("image")).toObject();
    QVERIFY(persistedImage.value(QStringLiteral("data")).toString().isEmpty());
    QVERIFY(!persistedImage.value(QStringLiteral("blobRef")).toObject()
                 .value(QStringLiteral("blobId")).toString().isEmpty());

    const ProviderRequest request = ledger.buildRequest({}).request;
    QCOMPARE(request.items.size(), 1);
    QCOMPARE(request.items.first().parts.first().image.data,
             QByteArrayLiteral("png-bytes"));
}

void ProviderProtocolMigrationTests::requestValidationEnforcesProtocolInvariants()
{
    QCOMPARE(kProviderProtocolVersion, 2);
    QCOMPARE(kProviderProtocolRevision, 0);

    ProviderRequest request;
    request.items.append(ProviderItem::makeUserText(QStringLiteral("hello")));
    request.metadata.insert(QStringLiteral("conversation_history"), QStringLiteral("forbidden"));

    QString error;
    QVERIFY(!request.validate(&error));
    QVERIFY(!error.isEmpty());

    request.metadata.clear();
    QVERIFY(request.validate(&error));
}

void ProviderProtocolMigrationTests::legacyUiEntryDoesNotBecomeProtocolHistory()
{
    ProviderRunLedger ledger;
    ledger.fromJson(QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("legacy-user")},
        {QStringLiteral("kind"), QStringLiteral("user_text")},
        {QStringLiteral("status"), QStringLiteral("completed")},
        {QStringLiteral("text"), QStringLiteral("old display-only text")},
        {QStringLiteral("submittedToModel"), true}}});

    QCOMPARE(ledger.entries().size(), 1);
    QVERIFY(ledger.providerItems().isEmpty());
    QVERIFY(ledger.buildRequest({}).request.items.isEmpty());
}

void ProviderProtocolMigrationTests::emptyReasoningItemIsDroppedFromRequest()
{
    // 回归：正文与签名双空的推理条目不可回放，必须不进请求。
    // 曾导致**任何带工具调用的轮次在第 2 步整体失败** ——
    // 厂商回「reasoning requires content or signature」（真 Provider 手测复现）。
    ProviderRunLedger ledger;

    ConversationMessage user;
    user.id = QStringLiteral("user");
    user.kind = ConversationMessage::Kind::UserText;
    user.text = QStringLiteral("hello");
    ledger.appendUiIngress(user);

    // 空推理条目：ensureStreamingReasoningEntry 在首个 delta / outputItems 时就会建出这种形态
    ConversationMessage emptyReasoning;
    emptyReasoning.id = QStringLiteral("reasoning-empty");
    emptyReasoning.kind = ConversationMessage::Kind::AssistantReasoning;
    ledger.appendUiIngress(emptyReasoning);

    // UI 投影保留；线路记录不得存在。
    QCOMPARE(ledger.entries().size(), 2);
    QVERIFY(ledger.findById(QStringLiteral("reasoning-empty")));
    for (const ProviderItem &item : ledger.providerItems()) {
        QVERIFY2(item.kind != ProviderItemKind::Reasoning
                     || !item.reasoningText.trimmed().isEmpty()
                     || !item.reasoningSignature.trimmed().isEmpty()
                     || item.reasoningRedacted,
                 "空推理不得进入 providerItems");
    }

    ProviderRequestBuild build = ledger.buildRequest({});
    for (const ProviderItem &item : build.request.items) {
        QVERIFY2(item.kind != ProviderItemKind::Reasoning,
                 "空推理条目必须被丢弃，否则厂商拒整个请求");
    }
    QCOMPARE(build.request.items.size(), 1);

    // 整个请求必须通过 IR 校验（这才是厂商拒绝的真正判据）
    QString error;
    QVERIFY2(build.request.validate(&error), qPrintable(error));

    // 流式后补：setProviderItemForEntry 用双空 item 不得抹掉 UI 已有正文，
    // 且不得把空 item 写入线路。
    ConversationMessage streamed;
    streamed.id = QStringLiteral("reasoning-streamed");
    streamed.kind = ConversationMessage::Kind::AssistantReasoning;
    streamed.reasoningContent = QStringLiteral("think-stream");
    ledger.appendUiIngress(streamed);
    ledger.setProviderItemForEntry(
        streamed.id,
        ProviderItem::makeReasoning({}, {}, false, false));
    const ConversationMessage *streamedEntry = ledger.findById(streamed.id);
    QVERIFY(streamedEntry);
    QCOMPARE(streamedEntry->reasoningContent, QStringLiteral("think-stream"));
    build = ledger.buildRequest({});
    bool sawStreamed = false;
    for (const ProviderItem &item : build.request.items) {
        if (item.kind == ProviderItemKind::Reasoning
            && item.reasoningText == QStringLiteral("think-stream")) {
            sawStreamed = true;
        }
    }
    QVERIFY2(sawStreamed, "流式正文必须回填进请求，不得被空完成态覆盖后丢弃");
    QVERIFY2(build.request.validate(&error), qPrintable(error));

    // 反向：有签名（正文仍空）的推理条目是**可回放**的，不得被误丢
    ConversationMessage signedReasoning;
    signedReasoning.id = QStringLiteral("reasoning-signed");
    signedReasoning.kind = ConversationMessage::Kind::AssistantReasoning;
    signedReasoning.reasoningSignature = QStringLiteral("sig-1");
    signedReasoning.reasoningMustReplay = true;
    ledger.appendUiIngress(signedReasoning);

    build = ledger.buildRequest({});
    int reasoningCount = 0;
    for (const ProviderItem &item : build.request.items) {
        if (item.kind == ProviderItemKind::Reasoning) {
            ++reasoningCount;
            if (item.reasoningSignature == QStringLiteral("sig-1"))
                QCOMPARE(item.reasoningSignature, QStringLiteral("sig-1"));
        }
    }
    QVERIFY(reasoningCount >= 2);
    QVERIFY2(build.request.validate(&error), qPrintable(error));

    // redacted 推理正文可空也算可回放（IR 校验放行该形态）
    ConversationMessage redacted;
    redacted.id = QStringLiteral("reasoning-redacted");
    redacted.kind = ConversationMessage::Kind::AssistantReasoning;
    redacted.reasoningRedacted = true;
    ledger.appendUiIngress(redacted);

    build = ledger.buildRequest({});
    QVERIFY2(build.request.validate(&error), qPrintable(error));
    bool sawRedacted = false;
    for (const ProviderItem &item : build.request.items) {
        if (item.kind == ProviderItemKind::Reasoning && item.reasoningRedacted) {
            sawRedacted = true;
        }
    }
    QVERIFY2(sawRedacted, "redacted 推理条目可回放，不得被丢弃");

    // findLatestReasoningForTurn：复用本 turn 最近推理，避免 MessageCompleted 再建空条
    ConversationMessage *latest =
        ledger.findLatestReasoningForTurn({});
    QVERIFY(latest);
    QCOMPARE(latest->id, QStringLiteral("reasoning-redacted"));
}

void ProviderProtocolMigrationTests::rollbackUncommittedTurnDropsUnresolvedToolCalls()
{
    // 回归：Esc 取消时 cancel() 必须走 discardIncompleteEntriesForTurn →
    // rollbackUncommittedTurn。若只 resetLoopState 而不 rollback，未闭合
    // FunctionCall 会进下一轮 Chat Completions 全量回放 → DeepSeek 400。
    ProviderRunLedger ledger;

    ConversationMessage user;
    user.id = QStringLiteral("user");
    user.kind = ConversationMessage::Kind::UserText;
    user.text = QStringLiteral("do stuff");
    user.turnId = QStringLiteral("turn-1");
    user.submittedToModel = true;
    ledger.appendUiIngress(user);

    // 已完成配对的工具：取消后应保留
    ToolCall doneCall;
    doneCall.id = QStringLiteral("call-done");
    doneCall.toolName = QStringLiteral("read_file");
    doneCall.rawInputJson = QStringLiteral("{\"path\":\"a.txt\"}");
    ConversationMessage doneToolCall;
    doneToolCall.id = QStringLiteral("tool-done");
    doneToolCall.kind = ConversationMessage::Kind::ToolCall;
    doneToolCall.toolCall = doneCall;
    doneToolCall.toolUseId = doneCall.id;
    doneToolCall.toolName = doneCall.toolName;
    doneToolCall.turnId = QStringLiteral("turn-1");
    doneToolCall.submittedToModel = true;
    ledger.appendUiIngress(doneToolCall);

    ConversationMessage doneResult;
    doneResult.id = QStringLiteral("result-done");
    doneResult.kind = ConversationMessage::Kind::ToolResult;
    doneResult.toolUseId = doneCall.id;
    doneResult.toolName = doneCall.toolName;
    doneResult.text = QStringLiteral("ok");
    doneResult.turnId = QStringLiteral("turn-1");
    ledger.appendUiIngress(doneResult);

    // 未闭合工具：取消后必须抹掉（含 ProviderRecord）
    ToolCall openCall;
    openCall.id = QStringLiteral("call-open");
    openCall.toolName = QStringLiteral("run_command");
    openCall.rawInputJson = QStringLiteral("{\"command\":\"sleep 99\"}");
    ConversationMessage openToolCall;
    openToolCall.id = QStringLiteral("tool-open");
    openToolCall.kind = ConversationMessage::Kind::ToolCall;
    openToolCall.toolCall = openCall;
    openToolCall.toolUseId = openCall.id;
    openToolCall.toolName = openCall.toolName;
    openToolCall.turnId = QStringLiteral("turn-1");
    openToolCall.submittedToModel = true;
    ledger.appendUiIngress(openToolCall);

    // 未提交的助手流式碎片：也应抹掉
    ConversationMessage streamText;
    streamText.id = QStringLiteral("asst-stream");
    streamText.kind = ConversationMessage::Kind::AssistantText;
    streamText.text = QStringLiteral("partial…");
    streamText.turnId = QStringLiteral("turn-1");
    streamText.submittedToModel = false;
    ledger.appendUiIngress(streamText);

    QVERIFY(ledger.hasUnresolvedToolCalls());
    QCOMPARE(ledger.providerItems().size(), 5); // user + 2 calls + result + asst

    ledger.rollbackUncommittedTurn(QStringLiteral("turn-1"));

    QVERIFY2(!ledger.hasUnresolvedToolCalls(),
             "取消后不得残留未闭合 tool_call");
    QVERIFY2(!ledger.findById(QStringLiteral("tool-open")),
             "未闭合 ToolCall 条目应被物理删除");
    QVERIFY2(!ledger.findById(QStringLiteral("asst-stream")),
             "未提交助手碎片应被物理删除");
    QVERIFY2(ledger.findById(QStringLiteral("tool-done")),
             "已有 ToolResult 的 ToolCall 应保留");
    QVERIFY2(ledger.findById(QStringLiteral("result-done")),
             "已完成 ToolResult 应保留");
    QVERIFY2(ledger.findById(QStringLiteral("user")),
             "用户消息应保留");

    const ProviderRequestBuild build = ledger.buildRequest({});
    int functionCalls = 0;
    int functionOutputs = 0;
    for (const ProviderItem &item : build.request.items) {
        if (item.kind == ProviderItemKind::FunctionCall) {
            ++functionCalls;
            QCOMPARE(item.callId, QStringLiteral("call-done"));
        } else if (item.kind == ProviderItemKind::FunctionCallOutput) {
            ++functionOutputs;
            QCOMPARE(item.callId, QStringLiteral("call-done"));
        }
    }
    QCOMPARE(functionCalls, 1);
    QCOMPARE(functionOutputs, 1);

    QString error;
    QVERIFY2(build.request.validate(&error), qPrintable(error));
}

void ProviderProtocolMigrationTests::emptyFunctionCallArgsMustNotOverwriteCompletedToolCall()
{
    // 回归：Chat Completions 流里 ToolCallStarted 时空参 → fallback FunctionCall 空；
    // ToolCallCompleted 才带完整 rawArguments 并写入 UI 条目。
    // MessageCompleted 若仍用空 fallback 调 setProviderItemForEntry，不得把账本
    // rawInputJson / 线路 rawArguments 抹空，否则下一轮 arguments=""。
    ProviderRunLedger ledger;

    ConversationMessage user;
    user.id = QStringLiteral("user");
    user.kind = ConversationMessage::Kind::UserText;
    user.text = QStringLiteral("run ls");
    user.submittedToModel = true;
    ledger.appendUiIngress(user);

    ToolCall call;
    call.id = QStringLiteral("call-1");
    call.toolName = QStringLiteral("run_command");
    call.input = QJsonObject{{QStringLiteral("command"), QStringLiteral("ls")}};
    call.rawInputJson = QStringLiteral("{\"command\":\"ls\"}");
    ConversationMessage toolCall;
    toolCall.id = QStringLiteral("tool-call");
    toolCall.kind = ConversationMessage::Kind::ToolCall;
    toolCall.toolCall = call;
    toolCall.toolUseId = call.id;
    toolCall.toolName = call.toolName;
    toolCall.toolInput = call.input;
    toolCall.submittedToModel = true;
    ledger.appendUiIngress(toolCall);

    // 模拟 MessageCompleted 的空参 fallback FunctionCall 覆盖
    ProviderItem emptyFallback = ProviderItem::makeFunctionCall(
        call.id, call.toolName, QJsonObject{}, QString{});
    emptyFallback.itemId = toolCall.id;
    ledger.setProviderItemForEntry(toolCall.id, emptyFallback);

    const ConversationMessage *entry = ledger.findById(toolCall.id);
    QVERIFY(entry);
    QCOMPARE(entry->toolCall.rawInputJson, QStringLiteral("{\"command\":\"ls\"}"));
    QVERIFY(!entry->toolCall.input.isEmpty());
    QCOMPARE(entry->toolCall.input.value(QStringLiteral("command")).toString(),
             QStringLiteral("ls"));

    const ProviderRequestBuild build = ledger.buildRequest({});
    bool sawCall = false;
    for (const ProviderItem &item : build.request.items) {
        if (item.kind != ProviderItemKind::FunctionCall)
            continue;
        sawCall = true;
        QCOMPARE(item.callId, call.id);
        QVERIFY2(!item.rawArguments.trimmed().isEmpty() || !item.arguments.isEmpty(),
                 "空 fallback 不得抹掉已完成工具参数");
        if (!item.rawArguments.trimmed().isEmpty()) {
            QVERIFY(item.rawArguments.contains(QStringLiteral("ls")));
        } else {
            QCOMPARE(item.arguments.value(QStringLiteral("command")).toString(),
                     QStringLiteral("ls"));
        }
    }
    QVERIFY(sawCall);

    QString error;
    QVERIFY2(build.request.validate(&error), qPrintable(error));
}

void ProviderProtocolMigrationTests::sessionEventNeverEntersProviderRequest()
{
    // 回归：后台任务结束等 SessionEvent 若映射成 UserMessage 并落在
    // ToolCall 与 ToolResult 之间，Chat Completions / DeepSeek 会 400
    // 「tool_calls must be followed by tool messages」。
    ProviderRunLedger ledger;

    ledger.appendUiIngress(makeLedgerMessage(QStringLiteral("user"),
                                             ConversationMessage::Kind::UserText,
                                             QStringLiteral("kill bg")));

    ToolCall call;
    call.id = QStringLiteral("call-bg");
    call.toolName = QStringLiteral("run_command");
    call.rawInputJson = QStringLiteral("{\"command\":\"kill\"}");
    ledger.appendUiIngress(makeToolCallMessage(QStringLiteral("tool-call"), call));

    // 后台结束通知插在 Call 与 Result 之间（真实时序）
    ledger.appendUiIngress(makeLedgerMessage(
        QStringLiteral("bg-done"),
        ConversationMessage::Kind::SessionEvent,
        QStringLiteral("后台任务 bg-1 已结束，可用 agent_status 查看状态。"),
        /*submittedToModel=*/false));

    ledger.appendUiIngress(makeToolResultMessage(QStringLiteral("tool-result"),
                                                 call.id,
                                                 call.toolName,
                                                 QStringLiteral("killed")));

    // UI 仍可见；线路不得有 SessionEvent / 对应 UserMessage
    QCOMPARE(ledger.entries().size(), 4);
    QVERIFY(ledger.findById(QStringLiteral("bg-done")));
    for (const ProviderItem &item : ledger.providerItems()) {
        QVERIFY2(item.itemId != QStringLiteral("bg-done"),
                 "SessionEvent 不得进入 providerItems");
        if (item.kind == ProviderItemKind::UserMessage) {
            QVERIFY2(!itemTextContains(item, QStringLiteral("后台任务")),
                     "SessionEvent 正文不得作为 UserMessage 进线路");
        }
    }

    const ProviderRequestBuild build = ledger.buildRequest({});
    QCOMPARE(build.request.items.size(), 3);
    QCOMPARE(build.request.items[0].kind, ProviderItemKind::UserMessage);
    QCOMPARE(build.request.items[1].kind, ProviderItemKind::FunctionCall);
    QCOMPARE(build.request.items[2].kind, ProviderItemKind::FunctionCallOutput);
    // Call 与 Result 之间不得夹任何其它 item
    QCOMPARE(build.request.items[1].callId, call.id);
    QCOMPARE(build.request.items[2].callId, call.id);

    QString error;
    QVERIFY2(build.request.validate(&error), qPrintable(error));
}

void ProviderProtocolMigrationTests::sessionEventProviderItemDroppedOnRestore()
{
    // 旧盘若已把 SessionEvent 写成 providerItem(UserMessage)，恢复时必须丢掉线路。
    const QString bgText =
        QStringLiteral("后台任务 bg-1 已结束，可用 agent_status 查看状态。");
    ProviderRunLedger ledger;
    ledger.fromJson(QJsonArray{
        makeLedgerEntryJson(QStringLiteral("user"),
                            QStringLiteral("user_text"),
                            QStringLiteral("hi"),
                            true,
                            makeTextProviderItem(QStringLiteral("user"), QStringLiteral("hi"))),
        makeLedgerEntryJson(QStringLiteral("bg-done"),
                            QStringLiteral("session_event"),
                            bgText,
                            false,
                            makeTextProviderItem(QStringLiteral("bg-done"), bgText)),
    });

    QCOMPARE(ledger.entries().size(), 2);
    QVERIFY(ledger.findById(QStringLiteral("bg-done")));
    const ProviderRequestBuild build = ledger.buildRequest({});
    QCOMPARE(build.request.items.size(), 1);
    QCOMPARE(build.request.items.first().kind, ProviderItemKind::UserMessage);
    QCOMPARE(build.request.items.first().itemId, QStringLiteral("user"));
}

void ProviderProtocolMigrationTests::fromJsonRejectsUnsupportedSchemaVersion()
{
    ProviderRunLedger ledger;
    ConversationMessage user;
    user.id = QStringLiteral("user");
    user.kind = ConversationMessage::Kind::UserText;
    user.text = QStringLiteral("hi");
    ledger.appendUiIngress(user);

    QJsonObject envelope;
    envelope.insert(QStringLiteral("schemaVersion"), 2);
    envelope.insert(QStringLiteral("entries"), QJsonArray{});
    QVERIFY(!ledger.fromJson(envelope));
    QCOMPARE(ledger.entries().size(), 1);
    QVERIFY(ledger.findById(QStringLiteral("user")));
}

void ProviderProtocolMigrationTests::fromJsonDowngradesInvalidProviderItemToUiOnly()
{
    ProviderRunLedger ledger;
    QJsonObject badItem;
    badItem.insert(QStringLiteral("kind"), 999);
    badItem.insert(QStringLiteral("itemId"), QStringLiteral("bad"));
    const QJsonObject entryJson =
        makeLedgerEntryJson(QStringLiteral("bad"), QStringLiteral("user_text"),
                            QStringLiteral("hello"), true, badItem);
    QVERIFY(ledger.fromJson(QJsonArray{entryJson}));
    QCOMPARE(ledger.entries().size(), 1);
    QVERIFY(ledger.providerItems().isEmpty());
    QVERIFY(ledger.buildRequest({}).request.items.isEmpty());
}

void ProviderProtocolMigrationTests::rollbackRemovesOrphanToolResultAndError()
{
    ProviderRunLedger ledger;
    ConversationMessage user;
    user.id = QStringLiteral("user");
    user.kind = ConversationMessage::Kind::UserText;
    user.text = QStringLiteral("go");
    user.turnId = QStringLiteral("t1");
    user.submittedToModel = true;
    ledger.appendUiIngress(user);

    ConversationMessage orphanResult;
    orphanResult.id = QStringLiteral("orphan-result");
    orphanResult.kind = ConversationMessage::Kind::ToolResult;
    orphanResult.toolUseId = QStringLiteral("no-call");
    orphanResult.turnId = QStringLiteral("t1");
    ledger.appendUiIngress(orphanResult);

    ConversationMessage error;
    error.id = QStringLiteral("error");
    error.kind = ConversationMessage::Kind::Error;
    error.text = QStringLiteral("boom");
    error.turnId = QStringLiteral("t1");
    ledger.appendUiIngress(error);

    QVERIFY(ledger.rollbackUncommittedTurn(QStringLiteral("t1")));
    QVERIFY(!ledger.findById(QStringLiteral("orphan-result")));
    QVERIFY(!ledger.findById(QStringLiteral("error")));
    QVERIFY(ledger.findById(QStringLiteral("user")));
}

void ProviderProtocolMigrationTests::indexesRemainCorrectAfterRemove()
{
    ProviderRunLedger ledger;
    ConversationMessage user;
    user.id = QStringLiteral("u");
    user.kind = ConversationMessage::Kind::UserText;
    user.text = QStringLiteral("hi");
    ledger.appendUiIngress(user);

    ToolCall call;
    call.id = QStringLiteral("c1");
    call.toolName = QStringLiteral("read_file");
    call.rawInputJson = QStringLiteral("{}");
    ConversationMessage tool;
    tool.id = QStringLiteral("t");
    tool.kind = ConversationMessage::Kind::ToolCall;
    tool.toolCall = call;
    tool.toolUseId = call.id;
    tool.toolName = call.toolName;
    ledger.appendUiIngress(tool);

    QVERIFY(ledger.findById(QStringLiteral("u")));
    QVERIFY(ledger.findToolCallByUseId(QStringLiteral("c1")));
    QVERIFY(ledger.removeEntry(QStringLiteral("u")));
    QVERIFY(!ledger.findById(QStringLiteral("u")));
    QVERIFY(ledger.findById(QStringLiteral("t")));
    QVERIFY(ledger.findToolCallByUseId(QStringLiteral("c1")));
}

void ProviderProtocolMigrationTests::buildRequestReportsHydrateError()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ProviderRunLedger ledger;
    ledger.setBlobRoot(tempDir.path());
    ProviderImageAsset image;
    image.blobRef.blobId = QStringLiteral("missing-blob");
    image.blobRef.contentHash = QStringLiteral("sha256:0000");
    image.blobRef.byteSize = 4;
    image.blobRef.scheme = ProviderUriScheme::Blob;
    ProviderItem item = ProviderItem::makeUserMessage(
        {ProviderMessagePart::makeImage(image)});
    ledger.appendProviderItem(item);

    const ProviderRequestBuild build = ledger.buildRequest({});
    QVERIFY(!build.hydrateError.isEmpty());
}

QTEST_MAIN(ProviderProtocolMigrationTests)
#include "ProviderProtocolMigrationTests.moc"
