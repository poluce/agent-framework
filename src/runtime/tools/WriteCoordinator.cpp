#include "tools/WriteCoordinator.h"

WriteCoordinator::WriteCoordinator(QObject *parent)
    : QObject(parent)
{
}

WriteCoordinator::~WriteCoordinator()
{
    qDeleteAll(m_slots);
}

bool WriteCoordinator::tryAcquire(const QString &fileKey)
{
    QSemaphore *slot = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        slot = m_slots.value(fileKey);
        if (!slot) {
            slot = new QSemaphore(1);
            m_slots.insert(fileKey, slot);
        }
    }
    // 计数=1：同路径第二个调用方立即失败（不阻塞）
    return slot->tryAcquire();
}

void WriteCoordinator::release(const QString &fileKey)
{
    QSemaphore *slot = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        slot = m_slots.value(fileKey);
    }
    if (slot)
        slot->release();
}

bool WriteCoordinator::tryAcquireAll(const QStringList &fileKeys)
{
    QStringList acquired;
    acquired.reserve(fileKeys.size());
    for (const QString &key : fileKeys) {
        if (!tryAcquire(key)) {
            // 任一失败：释放已获并回滚（保持原子性）
            for (const QString &a : acquired)
                release(a);
            return false;
        }
        acquired.append(key);
    }
    return true;
}

void WriteCoordinator::releaseAll(const QStringList &fileKeys)
{
    for (const QString &key : fileKeys)
        release(key);
}
