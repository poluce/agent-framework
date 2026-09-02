# Changelog

本仓库所有值得使用者关注的变更都记录在此。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [0.2.0] - 2026-09-02

### 🚨 Breaking Changes（升级前必看）

- 邮箱投递语义变更：`takePendingInboxMessages()` 不再自动 ack
  - 调用方必须改为：投递成功 → `ackInboxMessages()`；失败 → `requeueInboxMessages()`
- `enqueueInboxMessage()` 返回类型从 `void` 改为 `bool`
- `AbstractLoop::enqueueMessage()` / `enqueueAgentTask()` / `enqueueUserMessage*()` 返回类型从 `void` 改为 `bool`
- `SessionRuntime` 新增字段 `maxInboxMessages` / `maxInboxMessageSize`（默认 0 = 不限，不改变旧行为）

### ✨ 新增功能

- 邮箱消息支持优先级（Low / Normal / High / Urgent），`takePendingInboxMessages()` 按优先级排序
- 邮箱消息模型版本化：`schemaVersion` / `type` / `payload`
- 邮箱容量/大小限制：`maxInboxMessages` / `maxInboxMessageSize`
- 新增 IR 事件：`EventInboxMessageEnqueued` / `EventInboxMessageDelivered` / `EventInboxMessageDropped`

### 🔧 功能修改

- 邮箱投递改为两阶段确认：take 只标记 in-flight，投递成功才 ack
- 会话清理时未读邮箱消息不再静默丢失，会发出 Dropped 事件
- `enqueueAgentTask()` 现在能感知投递结果（空消息返回 false）

### 🐛 修复

- #1 邮箱消息可能假送达：投递成功后才 ack
- #5 会话清理时未读邮箱消息直接丢失
- #8 enqueueAgentTask 无返回值，投递结果不可知
