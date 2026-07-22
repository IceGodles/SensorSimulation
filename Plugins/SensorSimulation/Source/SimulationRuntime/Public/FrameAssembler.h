#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"

/** 单个传感器在本帧中的预期与已完成模态。 */
struct FSensorFrameStatus
{
/** 该传感器预期产出的模态位集合。 */
    EPayloadType ExpectedPayloads = EPayloadType::None;
/** 该传感器已经完成的模态位集合。 */
    EPayloadType CompletedPayloads = EPayloadType::None;

/** 判断该传感器的所有预期模态是否已全部完成。 */
    bool IsComplete() const
    {
        return EnumHasAllFlags(CompletedPayloads, ExpectedPayloads);
    }
};

/** 帧聚合器的统计信息。 */
struct FFrameAssemblerStats
{
/** 本次 Session 中已创建的总帧数。 */
    int64 TotalFrames = 0;
/** 已成功聚合完成的帧数。 */
    int64 CompletedFrames = 0;
/** 因超时而丢弃的帧数。 */
    int64 TimeoutFrames = 0;
/** 因其他原因失败的帧数。 */
    int64 FailedFrames = 0;
};

/** 按帧编号、按传感器名称聚合图像、点云与真值模态的同步器。 */
class SIMULATIONRUNTIME_API FFrameAssembler
{
public:
/** 创建或更新指定编号的待聚合帧，并记录整帧预期模态。 */
    void BeginFrame(const FFrameHeader& Header, EPayloadType ExpectedPayloads);
/** 注册一个传感器在本帧中的预期模态，用于多传感器精确计数。 */
    void RegisterSensor(uint64 FrameId, FName SensorName, EPayloadType ExpectedPayloads);
/** 把图像加入对应帧并更新该传感器的模态完成状态。 */
    bool AddImage(FImagePayload&& Image);
/** 验证完整扫描后把点云加入对应帧。 */
    bool AddLidar(FLidarScanPayload&& Scan);
/** 把对象真值移入对应帧并标记真值模态完成。 */
    bool AddGroundTruth(uint64 FrameId, TArray<FObjectGroundTruth>&& Objects);
/** 非阻塞取出并移除一个所有模态均到齐的帧。 */
    bool PopCompleteFrame(FFramePacket& OutPacket);
/** 检查并清理超时帧，返回被清理的帧数。 */
    int32 PurgeTimedOutFrames(double CurrentTimeSeconds, double TimeoutSeconds);
/** 返回当前等待所有模态到齐的帧数量。 */
    int32 GetPendingFrameCount() const;
/** 返回帧聚合器的统计信息。 */
    const FFrameAssemblerStats& GetStats() const { return Stats; }

private:
    /** 以帧编号索引、仍在等待部分模态的帧数据包。 */
    TMap<uint64, FFramePacket> PendingFrames;
    /** 以帧编号索引、每个传感器的完成状态。 */
    TMap<uint64, TMap<FName, FSensorFrameStatus>> PerSensorStatus;
    /** 以帧编号索引、帧创建时间（用于超时检测）。 */
    TMap<uint64, double> FrameCreationTime;
    /** 已经满足全部预期模态、等待消费者取出的帧编号队列。 */
    TQueue<uint64> CompleteFrameIds;
    /** 帧聚合统计。 */
    FFrameAssemblerStats Stats;

/** 检查指定帧的所有传感器是否全部完成，若完成则加入完成队列。 */
    void CheckAndEnqueueComplete(uint64 FrameId);
/** 清理指定帧的所有关联数据。 */
    void CleanupFrame(uint64 FrameId);
};
