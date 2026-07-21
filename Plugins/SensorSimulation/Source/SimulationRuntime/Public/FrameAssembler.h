#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"

/** 按帧编号聚合图像、点云与真值模态的同步器。 */
class SIMULATIONRUNTIME_API FFrameAssembler
{
public:
/** 创建或更新指定编号的待聚合帧并记录预期模态。 */
    void BeginFrame(const FFrameHeader& Header, EPayloadType ExpectedPayloads);
/** 把图像加入对应帧并更新模态完成状态。 */
    bool AddImage(FImagePayload&& Image);
/** 验证完整扫描后把点云加入对应帧。 */
    bool AddLidar(FLidarScanPayload&& Scan);
/** 把对象真值移入对应帧并标记真值模态完成。 */
    bool AddGroundTruth(uint64 FrameId, TArray<FObjectGroundTruth>&& Objects);
/** 非阻塞取出并移除一个所有模态均到齐的帧。 */
    bool PopCompleteFrame(FFramePacket& OutPacket);
/** 返回当前等待所有模态到齐的帧数量。 */
    int32 GetPendingFrameCount() const;

private:
    /** 以帧编号索引、仍在等待部分模态的帧数据包。 */
    TMap<uint64, FFramePacket> PendingFrames;
    /** 已经满足全部预期模态、等待消费者取出的帧编号队列。 */
    TQueue<uint64> CompleteFrameIds;
};
