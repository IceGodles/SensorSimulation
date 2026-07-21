#include "FrameAssembler.h"

/** 创建或更新指定编号的待聚合帧并记录预期模态。 */
void FFrameAssembler::BeginFrame(const FFrameHeader& Header, EPayloadType ExpectedPayloads)
{
    FFramePacket& Packet = PendingFrames.FindOrAdd(Header.FrameId);
    Packet.Header = Header;
    Packet.ExpectedPayloads = ExpectedPayloads;
}

/** 把图像加入对应帧并更新模态完成状态。 */
bool FFrameAssembler::AddImage(FImagePayload&& Image)
{
    FFramePacket* Packet = PendingFrames.Find(Image.Header.FrameId);
    if (!Packet)
    {
        return false;
    }
    Packet->CompletedPayloads |= Image.PayloadType;
    Packet->Images.Add(MoveTemp(Image));
    // 仅当预期位掩码被已完成位掩码完全覆盖时，帧才进入可发布队列。
    if (Packet->IsComplete()) CompleteFrameIds.Enqueue(Packet->Header.FrameId);
    return true;
}

/** 验证完整扫描后把点云加入对应帧。 */
bool FFrameAssembler::AddLidar(FLidarScanPayload&& Scan)
{
    FFramePacket* Packet = PendingFrames.Find(Scan.Header.FrameId);
    if (!Packet || !Scan.bCompleteRevolution)
    {
        return false;
    }
    Packet->CompletedPayloads |= EPayloadType::Lidar;
    Packet->LidarScans.Add(MoveTemp(Scan));
    // 仅当预期位掩码被已完成位掩码完全覆盖时，帧才进入可发布队列。
    if (Packet->IsComplete()) CompleteFrameIds.Enqueue(Packet->Header.FrameId);
    return true;
}

/** 把对象真值移入对应帧并标记真值模态完成。 */
bool FFrameAssembler::AddGroundTruth(uint64 FrameId, TArray<FObjectGroundTruth>&& Objects)
{
    FFramePacket* Packet = PendingFrames.Find(FrameId);
    if (!Packet)
    {
        return false;
    }
    Packet->Objects = MoveTemp(Objects);
    Packet->CompletedPayloads |= EPayloadType::GroundTruth;
    if (Packet->IsComplete()) CompleteFrameIds.Enqueue(FrameId);
    return true;
}

/** 非阻塞取出并移除一个所有模态均到齐的帧。 */
bool FFrameAssembler::PopCompleteFrame(FFramePacket& OutPacket)
{
    uint64 FrameId = 0;
    if (!CompleteFrameIds.Dequeue(FrameId))
    {
        return false;
    }
    FFramePacket* Packet = PendingFrames.Find(FrameId);
    if (!Packet || !Packet->IsComplete())
    {
        return false;
    }
    // 使用移动语义转移可能很大的图像与点云缓冲区，避免发布阶段复制整帧数据。
    OutPacket = MoveTemp(*Packet);
    PendingFrames.Remove(FrameId);
    return true;
}

/** 返回当前等待所有模态到齐的帧数量。 */
int32 FFrameAssembler::GetPendingFrameCount() const
{
    return PendingFrames.Num();
}
