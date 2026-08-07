#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"

/** 单个传感器在本帧中的预期与已完成模态。 */
struct FSensorFrameStatus
{
    /** 仅用于超时日志的人类可读名称；身份键由外层 SensorGuid 提供。 */
    FName SensorName = NAME_None;
/** 该传感器预期产出的模态位集合。 */
    EPayloadType ExpectedPayloads = EPayloadType::None;
/** 该传感器已经完成的模态位集合。 */
    EPayloadType CompletedPayloads = EPayloadType::None;
    /** 以 ChannelGuid 为键的独立图像预期；值用于校验 PayloadType。 */
    TMap<FGuid, EPayloadType> ExpectedImageChannels;
    /** 已经返回且通过类型校验的图像 ChannelGuid。 */
    TSet<FGuid> CompletedImageChannels;

/** 判断模态位与每条独立图像通道是否都已完成。 */
    bool IsComplete() const
    {
        return EnumHasAllFlags(CompletedPayloads, ExpectedPayloads) &&
            CompletedImageChannels.Num() == ExpectedImageChannels.Num();
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
    /** 因传感器报告 Busy 而立即失败的帧数。 */
    int64 BusyFrames = 0;
    /** 因传感器报告 Rejected 而立即失败的帧数。 */
    int64 RejectedFrames = 0;
    /** 帧仍 Pending 时收到的重复 SensorGuid + ChannelGuid 数量。 */
    int64 DuplicatePayloads = 0;
    /** 帧完成、失败或超时后才到达并被丢弃的 Payload 数量。 */
    int64 LatePayloads = 0;
};

/** 按帧编号、按传感器名称聚合图像、点云与真值模态的同步器。 */
class SIMULATIONRUNTIME_API FFrameAssembler
{
public:
/** 创建或更新指定编号的待聚合帧，并记录整帧预期模态。 */
    void BeginFrame(const FFrameHeader& Header, EPayloadType ExpectedPayloads);
/** 注册传感器的预期模态及逐 ChannelGuid 图像项；旧调用可省略图像通道。 */
    void RegisterSensor(
        uint64 FrameId,
        const FGuid& SensorGuid,
        FName SensorName,
        EPayloadType ExpectedPayloads,
        const TArray<FExpectedImageChannel>& ExpectedImageChannels = {});
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
    /** 传感器拒绝请求时立即终止帧，后续异步结果将按迟到 Payload 丢弃。 */
    bool FailFrame(uint64 FrameId, const FGuid& SensorGuid, FName SensorName, ECaptureRequestResult Result);
/** 返回当前等待所有模态到齐的帧数量。 */
    int32 GetPendingFrameCount() const;
/** 返回帧聚合器的统计信息。 */
    const FFrameAssemblerStats& GetStats() const { return Stats; }

private:
    /** 以帧编号索引、仍在等待部分模态的帧数据包。 */
    TMap<uint64, FFramePacket> PendingFrames;
    /** 以帧编号索引、每个传感器的完成状态。 */
    TMap<uint64, TMap<FGuid, FSensorFrameStatus>> PerSensorStatus;
    /** 以帧编号索引、帧创建时间（用于超时检测）。 */
    TMap<uint64, double> FrameCreationTime;
    /** 已经满足全部预期模态、等待消费者取出的帧编号队列。 */
    TQueue<uint64> CompleteFrameIds;
    /** 防止满足条件后因重复回调把同一 FrameId 多次压入完成队列。 */
    TSet<uint64> EnqueuedCompleteFrames;
    /** 已结束帧的有限历史；用于把“未知帧”区分为可统计的迟到结果。 */
    TMap<uint64, uint8> TerminalFrames;
    TQueue<uint64> TerminalFrameOrder;
    static constexpr int32 MaxTerminalFrameHistory = 1024;
    /** 帧聚合统计。 */
    FFrameAssemblerStats Stats;

/** 检查指定帧的所有传感器是否全部完成，若完成则加入完成队列。 */
    void CheckAndEnqueueComplete(uint64 FrameId);
/** 清理指定帧的所有关联数据。 */
    void CleanupFrame(uint64 FrameId);
    /** 记录有限终态历史，超过上限时按最旧 FrameId 淘汰。 */
    void RememberTerminalFrame(uint64 FrameId, uint8 TerminalState);
};
