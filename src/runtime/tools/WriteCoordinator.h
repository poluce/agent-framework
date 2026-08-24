#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSemaphore>
#include <QStringList>

/**
 * @brief 会话级 per-file 写互斥（同进程内信号量串行）。
 *
 * 多子代理共享同一工作区，写工具（write_file/edit/notebook_edit/multi_edit）在
 * 共享线程池并行执行；若无协调，并发写同一文件会「最后完成者胜、
 * 中间修改静默丢失」（读-改-写非原子）。本类按规范化绝对路径提供
 * 计数=1 的信号量槽：同路径并发写时，后到者 tryAcquire 失败，由调用方
 * 返回「正被其他代理写入」错误让模型重试——不等待、不阻塞主线程。
 *
 * 槽随会话存活（不回收）：信号量计数=1 本身即锁状态，回收会引入
 * 「持有期间槽被删、新槽重复可获」的竞态；析构时统一清理。
 *
 * 线程归属：tryAcquire/release 在池线程成对调用；槽生命周期由 m_mutex
 * 保护。随 AgentSession 构造/析构。
 */
class WriteCoordinator : public QObject
{
    Q_OBJECT

public:
    explicit WriteCoordinator(QObject *parent = nullptr);
    ~WriteCoordinator() override;

    /** 尝试获取写权（非阻塞）。fileKey 为规范化绝对路径。失败=被他人持有。 */
    [[nodiscard]] bool tryAcquire(const QString &fileKey);

    /** 释放写权。必须与 tryAcquire 成对。 */
    void release(const QString &fileKey);

    /**
     * 多键原子获取：全部成功才返回 true；任一失败则释放已获并返回 false。
     * 调用方应先对 keys 去重（同路径重复 tryAcquire 会自锁失败）。
     */
    [[nodiscard]] bool tryAcquireAll(const QStringList &fileKeys);

    /** 释放 tryAcquireAll 成功拿到的全部键。 */
    void releaseAll(const QStringList &fileKeys);

private:
    QMutex m_mutex;
    /** fileKey -> 每路径互斥槽（计数=1；随会话存活）。 */
    QHash<QString, QSemaphore *> m_slots;
};
