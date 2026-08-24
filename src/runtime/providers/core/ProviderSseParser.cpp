#include "ProviderSseParser.h"


void ProviderSseParser::feed(const QByteArray &chunk)
{
    if (chunk.isEmpty()) {
        return;
    }
    m_buffer.append(chunk);
    consumeLines();
}

void ProviderSseParser::finish()
{
    // 如果 buffer 里还有残行（流正常结束时可能不带尾随换行），把它作为最后一行处理。
    if (!m_buffer.isEmpty()) {
        QByteArray line = m_buffer;
        m_buffer.clear();
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            dispatchPending();
        } else if (!line.startsWith(':')) {
            handleLine(line);
        }
    }
    // 还有累积中的事件就刷出来，模拟"协议末尾的空行"。
    dispatchPending();
}

void ProviderSseParser::reset()
{
    m_buffer.clear();
    m_pendingEvent.clear();
    m_pendingData.clear();
    m_hasPending = false;
    m_eventsQueue.clear();
}

void ProviderSseParser::consumeLines()
{
    int start = 0;
    while (true) {
        const int newlineIndex = m_buffer.indexOf('\n', start);
        if (newlineIndex < 0) {
            // 剩下的不是完整行，保留到下次。
            if (start > 0) {
                m_buffer.remove(0, start);
            }
            return;
        }

        QByteArray line = m_buffer.mid(start, newlineIndex - start);
        start = newlineIndex + 1;

        if (line.endsWith('\r')) {
            line.chop(1);
        }

        if (line.isEmpty()) {
            dispatchPending();
            continue;
        }
        if (line.startsWith(':')) {
            // 协议规定：冒号开头的行是注释，忽略。
            continue;
        }
        handleLine(line);
    }
}

void ProviderSseParser::handleLine(QByteArray line)
{
    const int separatorIndex = line.indexOf(':');
    QByteArray field;
    QByteArray value;
    if (separatorIndex < 0) {
        field = line;
    } else {
        field = line.left(separatorIndex);
        value = line.mid(separatorIndex + 1);
        if (value.startsWith(' ')) {
            value.remove(0, 1);
        }
    }

    if (field == "event") {
        m_pendingEvent = QString::fromUtf8(value);
        m_hasPending = true;
    } else if (field == "data") {
        m_pendingData.append(value);
        m_hasPending = true;
    } else if (field == "id" || field == "retry") {
        // 目前不消费 id/retry 语义，但仍视为"出现了字段"以便空 data 事件也能触发。
        m_hasPending = true;
    }
    // 其它未知字段按协议要求忽略。
}

void ProviderSseParser::dispatchPending()
{
    if (!m_hasPending) {
        return;
    }

    const QByteArray data = m_pendingData.join('\n');

    const QString eventName = m_pendingEvent;

    m_pendingEvent.clear();
    m_pendingData.clear();
    m_hasPending = false;

    m_eventsQueue.enqueue({eventName, data});
}
