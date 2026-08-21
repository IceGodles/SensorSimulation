#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"

/** 导出队列满载时采用的背压策略。 */
enum class EExportBackpressurePolicy : uint8
{
    /** 队列满时拒绝新帧入队。 */
    RejectNewest,
    /** 队列满时丢弃最旧的帧，为新帧腾出空间。 */
    DropOldest,
    /** 确定性模式：队列满时立即返回 false，由调度器显式暂停时间轴。 */
    PauseDatasetClock,
    /** 旧名称兼容别名；行为已改为非阻塞显式暂停。 */
    BlockDatasetClock = PauseDatasetClock
};

/** Export Queue 到最终磁盘提交的分层统计；Committed 才等于可消费的 frame 目录。 */
struct SIMULATIONRUNTIME_API FExportServiceStats
{
    int64 EnqueuedFrames = 0;
    int64 RejectedFrames = 0;
    int64 DroppedFrames = 0;
    int64 CommittedFrames = 0;
    int64 FailedFrames = 0;
    int32 PeakPendingFrames = 0;
};

/** 接收完整帧并异步导出数据集的有界队列服务。 */
class SIMULATIONRUNTIME_API FExportService
{
public:
/** 构造并初始化 FExportService 的默认状态。 */
    explicit FExportService(int32 InCapacity = 8);
/** 销毁对象并停止后台 Worker。 */
    ~FExportService();

/** 创建输出目录并启动后台 Worker 线程。 */
    bool Start(const FString& InDatasetRoot);
/** 通知 Worker 退出并等待线程结束。 */
    void Stop();
/** 根据背压策略将完整帧提交到导出队列。 */
    bool Enqueue(FFramePacket&& Packet, EExportBackpressurePolicy Policy);
/** 返回当前仍在队列中等待处理的帧数量。 */
    int32 GetPendingCount() const;
    /** 返回 Session 内观测到的最高队列深度。 */
    int32 GetPeakPendingCount() const;
/** 返回队列是否还能立即接收一个完整帧，不进行等待。 */
    bool HasCapacity() const;
/** 返回 Worker 已成功写出的帧数量。 */
    int64 GetExportedFrameCount() const;
/** 返回 Worker 写入失败的帧数量。 */
    int64 GetFailedFrameCount() const;
    /** 返回 Queue、Writer 和最终提交的完整统计快照。 */
    FExportServiceStats GetStats() const;

private:
    struct FImpl;
/** 隐藏队列、Worker 线程及文件写入实现的私有对象。 */
    TUniquePtr<FImpl> Impl;
};
