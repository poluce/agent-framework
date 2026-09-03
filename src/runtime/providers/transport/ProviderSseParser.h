#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QQueue>

struct ProviderSseEvent {
    QString eventName;
    QByteArray data;
};

// 增量式 SSE 解析器。
//
// 设计目标：
// - 不依赖 QNetworkReply / QObject / 回调机制，从架构上彻底避免解析栈深处的生命周期问题。
// - 状态机按行驱动：只要遇到空行就视为一个完整事件边界，提取事件到队列。
// - data 字段以 QByteArray 暴露，不做 UTF-8 解码，避免多字节字符跨分片时错位。
// - [DONE] 哨兵不在此层过滤，原样透出由业务层决定语义。
//
// 使用方式：
//   ProviderSseParser parser;
//   parser.feed(chunk);
//   while (parser.hasNext()) {
//       auto ev = parser.takeNext();
//       // handle ev
//   }
class ProviderSseParser
{
public:
    ProviderSseParser() = default;

    // 喂入任意大小的字节分片。可以包含多个事件，也可以只是半行字节。
    void feed(const QByteArray &chunk);

    // 声明流结束：如果当前还有未派发的事件（没有尾随空行），在此刷出。
    void finish();

    // 重置所有状态（缓冲、累积中的事件），用于复用实例。
    void reset();

    // 提取解析出的事件
    bool hasNext() const { return !m_eventsQueue.isEmpty(); }
    ProviderSseEvent takeNext() { return m_eventsQueue.dequeue(); }

private:
    void consumeLines();
    void handleLine(QByteArray line);
    void dispatchPending();

    QByteArray m_buffer;              // 未处理的尾部字节
    QString m_pendingEvent;           // 累积中的 event 字段
    QList<QByteArray> m_pendingData;  // 累积中的 data 多行
    bool m_hasPending = false;        // 是否存在待派发事件（至少出现过一行非注释字段）

    QQueue<ProviderSseEvent> m_eventsQueue;
};
