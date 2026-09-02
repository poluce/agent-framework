# 代码风格：注释规范

## 基本原则

1. 公共类和公共函数使用 Doxygen 文档注释。
2. 私有函数只在逻辑不明显时添加说明。
3. 函数内部重点解释设计原因、边界条件和特殊处理。
4. 避免逐行翻译代码。
5. 注释发生变化时，要与代码同步修改。
6. 已废弃代码交给 Git 保存，不长期注释在源码中。

## 注释风格

### 类和公共函数

```cpp
/**
 * @brief 管理会话内的执行单元表
 *
 * 负责单元的登记、查找、生命周期和主单元回落。
 */
class AgentSession
{
public:
    /**
     * @brief 按 agentId 查找执行单元
     * @param agentId 会话内唯一标识
     * @return 找到时返回单元指针，否则返回 nullptr
     */
    Agent *findById(const QString &agentId);
};
```

### 枚举和成员变量

```cpp
enum class AgentStatus
{
    Idle,      ///< 空闲，可接收新任务
    Running,   ///< 正在执行一轮
    Completed, ///< 本轮成功收口
    Failed     ///< 本轮失败
};

QString m_selectedAgentId;  ///< 会话内当前选中单元的 id
```

### 简短接口

```cpp
/// 返回当前主单元；无主单元时返回 nullptr。
Agent *primaryUnit() const;
```

### 完整示例

```cpp
/**
 * @brief 向收件箱投递一条消息
 *
 * @param msg 待投递的邮箱消息
 * @return 入队成功返回 true；容量/大小超限时返回 false 并发出 Dropped 事件
 *
 * @pre msg.id 非空
 * @note 投递后由编排负责 take → ack/requeue
 * @warning 超限消息不会进入队列，调用方需处理拒绝结果
 */
bool enqueueInboxMessage(const AgentInboxMessage &msg);
```

## 常用 Doxygen 标签

| 标签 | 用途 |
|------|------|
| `@file` | 文件说明 |
| `@brief` | 一句话摘要 |
| `@param` | 参数说明 |
| `@return` | 返回值说明 |
| `@retval` | 特定返回值说明 |
| `@note` | 补充说明 |
| `@warning` | 警告信息 |
| `@see` | 交叉引用 |
| `@throws` | 可能抛出的异常 |
| `@pre` | 前置条件 |
| `@post` | 后置条件 |
| `@deprecated` | 已废弃标记 |
