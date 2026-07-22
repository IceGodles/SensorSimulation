#include "FrameAssembler.h"

/** 创建或更新指定编号的待聚合帧，并记录整帧预期模态。 */
void FFrameAssembler::BeginFrame(const FFrameHeader& Header, EPayloadType ExpectedPayloads)
{
    FFramePacket& Packet = PendingFrames.FindOrAdd(Header.FrameId);
    Packet.Header = Header;
    Packet.ExpectedPayloads = ExpectedPayloads;
    FrameCreationTime.FindOrAdd(Header.FrameId) = Header.SimulationTimestampSeconds;
    ++Stats.TotalFrames;
}

/** 注册一个传感器在本帧中的预期模态，用于多传感器精确计数。 */
void FFrameAssembler::RegisterSensor(uint64 FrameId, FName SensorName, EPayloadType ExpectedPayloads)
{
    TMap<FName, FSensorFrameStatus>& StatusMap = PerSensorStatus.FindOrAdd(FrameId);
    FSensorFrameStatus& Status = StatusMap.FindOrAdd(SensorName);
    Status.ExpectedPayloads = ExpectedPayloads;
}

/** 把图像加入对应帧并更新该传感器的模态完成状态。 */
bool FFrameAssembler::AddImage(FImagePayload&& Image)
{
    FFramePacket* Packet = PendingFrames.Find(Image.Header.FrameId);
    if (!Packet)
    {
        return false;
    }

    // 更新整帧位掩码（兼容未注册传感器的情况）
    Packet->CompletedPayloads |= Image.PayloadType;

    // 更新按传感器的完成状态
    TMap<FName, FSensorFrameStatus>* StatusMap = PerSensorStatus.Find(Image.Header.FrameId);
    if (StatusMap)
    {
        FSensorFrameStatus* Status = StatusMap->Find(Image.SensorName);
        if (Status)
        {
            Status->CompletedPayloads |= Image.PayloadType;
        }
    }

    Packet->Images.Add(MoveTemp(Image));
    CheckAndEnqueueComplete(Image.Header.FrameId);
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

    // 更新整帧位掩码
    Packet->CompletedPayloads |= EPayloadType::Lidar;

    // 更新按传感器的完成状态
    TMap<FName, FSensorFrameStatus>* StatusMap = PerSensorStatus.Find(Scan.Header.FrameId);
    if (StatusMap)
    {
        FSensorFrameStatus* Status = StatusMap->Find(Scan.SensorName);
        if (Status)
        {
            Status->CompletedPayloads |= EPayloadType::Lidar;
        }
    }

    Packet->LidarScans.Add(MoveTemp(Scan));
    CheckAndEnqueueComplete(Packet->Header.FrameId);
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
    CheckAndEnqueueComplete(FrameId);
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
    CleanupFrame(FrameId);
    ++Stats.CompletedFrames;
    return true;
}

/** 返回当前等待所有模态到齐的帧数量。 */
int32 FFrameAssembler::GetPendingFrameCount() const
{
    return PendingFrames.Num();
}

/** 检查指定帧的所有传感器是否全部完成，若完成则加入完成队列。 */
void FFrameAssembler::CheckAndEnqueueComplete(uint64 FrameId)
{
    // 先检查整帧位掩码是否满足
    FFramePacket* Packet = PendingFrames.Find(FrameId);
    if (!Packet || !Packet->IsComplete())
    {
        return;
    }

    // 再检查所有注册的传感器是否都已完成
    TMap<FName, FSensorFrameStatus>* StatusMap = PerSensorStatus.Find(FrameId);
    if (StatusMap)
    {
        for (const auto& Pair : *StatusMap)
        {
            if (!Pair.Value.IsComplete())
            {
                return; // 还有传感器未完成
            }
        }
    }

    CompleteFrameIds.Enqueue(FrameId);
}

/** 检查并清理超时帧，返回被清理的帧数。 */
int32 FFrameAssembler::PurgeTimedOutFrames(double CurrentTimeSeconds, double TimeoutSeconds)
{
    TArray<uint64> TimedOutFrames;

    for (const auto& Pair : PendingFrames)
    {
        const uint64 FrameId = Pair.Key;
        const double* CreationTime = FrameCreationTime.Find(FrameId);
        if (CreationTime && (CurrentTimeSeconds - *CreationTime) > TimeoutSeconds)
        {
            TimedOutFrames.Add(FrameId);
        }
    }

    for (const uint64 FrameId : TimedOutFrames)
    {
        // 记录超时帧的详细信息
        const FFramePacket* Packet = PendingFrames.Find(FrameId);
        if (Packet)
        {
            // 收集缺失模态信息
            FString MissingModalities;
            const EPayloadType Missing = Packet->ExpectedPayloads & ~Packet->CompletedPayloads;
            if (EnumHasAnyFlags(Missing, EPayloadType::Rgb)) MissingModalities += TEXT("RGB ");
            if (EnumHasAnyFlags(Missing, EPayloadType::Semantic)) MissingModalities += TEXT("Semantic ");
            if (EnumHasAnyFlags(Missing, EPayloadType::Lidar)) MissingModalities += TEXT("LiDAR ");
            if (EnumHasAnyFlags(Missing, EPayloadType::GroundTruth)) MissingModalities += TEXT("GroundTruth ");

            // 收集各传感器完成状态
            FString SensorStatus;
            const TMap<FName, FSensorFrameStatus>* StatusMap = PerSensorStatus.Find(FrameId);
            if (StatusMap)
            {
                for (const auto& SensorPair : *StatusMap)
                {
                    SensorStatus += FString::Printf(TEXT("  %s: expected=%u completed=%u "),
                        *SensorPair.Key.ToString(),
                        static_cast<uint8>(SensorPair.Value.ExpectedPayloads),
                        static_cast<uint8>(SensorPair.Value.CompletedPayloads));
                }
            }

            UE_LOG(LogTemp, Warning,
                TEXT("Frame %llu timed out after %.2fs. Missing: [%s] Sensors:{%s}"),
                FrameId, CurrentTimeSeconds - *CreationTime,
                *MissingModalities, *SensorStatus);
        }

        CleanupFrame(FrameId);
        ++Stats.TimeoutFrames;
    }

    return TimedOutFrames.Num();
}

/** 清理指定帧的所有关联数据。 */
void FFrameAssembler::CleanupFrame(uint64 FrameId)
{
    PendingFrames.Remove(FrameId);
    PerSensorStatus.Remove(FrameId);
    FrameCreationTime.Remove(FrameId);
}
