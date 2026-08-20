#include "FrameAssembler.h"

namespace { enum class EFrameTerminalState : uint8 { Completed, Failed, TimedOut }; }

FFrameAssembler::FFrameAssembler(
    const int32 InMaxPendingFrames,
    const int32 InTerminalFrameHistoryCapacity)
{
    ConfigureLimits(InMaxPendingFrames, InTerminalFrameHistoryCapacity);
}

bool FFrameAssembler::ConfigureLimits(
    const int32 InMaxPendingFrames,
    const int32 InTerminalFrameHistoryCapacity)
{
    if (!PendingFrames.IsEmpty())
    {
        return false;
    }
    MaxPendingFrames = FMath::Max(1, InMaxPendingFrames);
    TerminalFrameHistoryCapacity = FMath::Max(1, InTerminalFrameHistoryCapacity);
    return true;
}

/** 创建或更新指定编号的待聚合帧，并记录整帧预期模态。 */
bool FFrameAssembler::BeginFrame(const FFrameHeader& Header, const EPayloadType ExpectedPayloads,
    const TOptional<double> CreationTimeSeconds)
{
    if (PendingFrames.Contains(Header.FrameId))
    {
        UE_LOG(LogTemp, Error, TEXT("Cannot begin duplicate pending FrameId %llu."), Header.FrameId);
        return false;
    }
    ++Stats.TotalFrames;
    if (!HasCapacity())
    {
        ++Stats.FailedFrames;
        ++Stats.CapacityRejectedFrames;
        UE_LOG(LogTemp, Warning,
            TEXT("Frame %llu rejected because FrameAssembler is full (%d/%d)."),
            Header.FrameId, PendingFrames.Num(), MaxPendingFrames);
        return false;
    }
    FFramePacket& Packet = PendingFrames.FindOrAdd(Header.FrameId);
    Packet.Header = Header;
    Packet.ExpectedPayloads = ExpectedPayloads;
    FrameCreationTime.FindOrAdd(Header.FrameId) = CreationTimeSeconds.Get(Header.SimulationTimestampSeconds);
    Stats.PeakPendingFrames = FMath::Max(Stats.PeakPendingFrames, PendingFrames.Num());
    return true;
}

/** 注册一个传感器在本帧中的预期模态，用于多传感器精确计数。 */
void FFrameAssembler::RegisterSensor(
    const uint64 FrameId,
    const FGuid& SensorGuid,
    const FName SensorName,
    const EPayloadType ExpectedPayloads,
    const TArray<FExpectedImageChannel>& ExpectedImageChannels)
{
    if (!SensorGuid.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("Cannot register sensor '%s' with an invalid SensorGuid."), *SensorName.ToString());
        return;
    }
    TMap<FGuid, FSensorFrameStatus>& StatusMap = PerSensorStatus.FindOrAdd(FrameId);
    FSensorFrameStatus& Status = StatusMap.FindOrAdd(SensorGuid);
    Status.SensorName = SensorName;
    Status.ExpectedPayloads = ExpectedPayloads;
    Status.ExpectedImageChannels.Reset();
    Status.CompletedImageChannels.Reset();
    for (const FExpectedImageChannel& Channel : ExpectedImageChannels)
    {
        if (!Channel.ChannelGuid.IsValid() || Channel.PayloadType == EPayloadType::None ||
            Status.ExpectedImageChannels.Contains(Channel.ChannelGuid))
        {
            UE_LOG(LogTemp, Error,
                TEXT("Invalid or duplicate expected image channel for sensor '%s': %s."),
                *SensorName.ToString(),
                *Channel.ChannelGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
            continue;
        }
        Status.ExpectedImageChannels.Add(Channel.ChannelGuid, Channel.PayloadType);
    }
}

/** 把图像加入对应帧并更新该传感器的模态完成状态。 */
bool FFrameAssembler::AddImage(FImagePayload&& Image)
{
    const uint64 FrameId = Image.Header.FrameId;
    FFramePacket* Packet = PendingFrames.Find(FrameId);
    if (!Packet) { if (TerminalFrames.Contains(FrameId)) ++Stats.LatePayloads; return false; }
    TMap<FGuid, FSensorFrameStatus>* StatusMap = PerSensorStatus.Find(FrameId);
    FSensorFrameStatus* Status = StatusMap ? StatusMap->Find(Image.SensorGuid) : nullptr;
    if (!Status) return false;
    if (Status->ExpectedImageChannels.Num() > 0)
    {
        const EPayloadType* ExpectedType = Status->ExpectedImageChannels.Find(Image.ChannelGuid);
        if (!ExpectedType || *ExpectedType != Image.PayloadType)
        {
            return false;
        }
        if (Status->CompletedImageChannels.Contains(Image.ChannelGuid))
        {
            ++Stats.DuplicatePayloads;
            return false;
        }
        Status->CompletedImageChannels.Add(Image.ChannelGuid);
    }
    else if (EnumHasAnyFlags(Status->CompletedPayloads, Image.PayloadType))
    {
        // 兼容非 Camera/旧测试调用；正式 Camera 请求总是登记 ChannelGuid。
        ++Stats.DuplicatePayloads;
        return false;
    }
    Packet->CompletedPayloads |= Image.PayloadType;
    Status->CompletedPayloads |= Image.PayloadType;
    Packet->Images.Add(MoveTemp(Image));
    CheckAndEnqueueComplete(FrameId);
    return true;
}

bool FFrameAssembler::AddLidar(FLidarScanPayload&& Scan)
{
    const uint64 FrameId = Scan.Header.FrameId;
    FFramePacket* Packet = PendingFrames.Find(FrameId);
    if (!Packet) { if (TerminalFrames.Contains(FrameId)) ++Stats.LatePayloads; return false; }
    if (!Scan.bCompleteRevolution) return false;
    TMap<FGuid, FSensorFrameStatus>* StatusMap = PerSensorStatus.Find(FrameId);
    FSensorFrameStatus* Status = StatusMap ? StatusMap->Find(Scan.SensorGuid) : nullptr;
    if (!Status) return false;
    if (EnumHasAnyFlags(Status->CompletedPayloads, EPayloadType::Lidar)) { ++Stats.DuplicatePayloads; return false; }
    Packet->CompletedPayloads |= EPayloadType::Lidar;
    Status->CompletedPayloads |= EPayloadType::Lidar;
    Packet->LidarScans.Add(MoveTemp(Scan));
    CheckAndEnqueueComplete(FrameId);
    return true;
}

bool FFrameAssembler::AddGroundTruth(uint64 FrameId, TArray<FObjectGroundTruth>&& Objects)
{
    FFramePacket* Packet = PendingFrames.Find(FrameId);
    if (!Packet) { if (TerminalFrames.Contains(FrameId)) ++Stats.LatePayloads; return false; }
    if (EnumHasAnyFlags(Packet->CompletedPayloads, EPayloadType::GroundTruth)) { ++Stats.DuplicatePayloads; return false; }
    Packet->Objects = MoveTemp(Objects);
    Packet->CompletedPayloads |= EPayloadType::GroundTruth;
    CheckAndEnqueueComplete(FrameId);
    return true;
}

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
    RememberTerminalFrame(FrameId, static_cast<uint8>(EFrameTerminalState::Completed));
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
    TMap<FGuid, FSensorFrameStatus>* StatusMap = PerSensorStatus.Find(FrameId);
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

    if (!EnqueuedCompleteFrames.Contains(FrameId))
    {
        EnqueuedCompleteFrames.Add(FrameId);
        CompleteFrameIds.Enqueue(FrameId);
    }
}

/** 检查并清理超时帧，返回被清理的帧数。 */
int32 FFrameAssembler::PurgeTimedOutFrames(double CurrentTimeSeconds, double TimeoutSeconds)
{
    TArray<uint64> TimedOutFrames;

    for (const auto& Pair : PendingFrames)
    {
        const uint64 FrameId = Pair.Key;
        // 已完成帧可能因 Export 背压暂留；它不再等待传感器，不能按采集超时删除。
        if (EnqueuedCompleteFrames.Contains(FrameId))
        {
            continue;
        }
        const double* CreationTime = FrameCreationTime.Find(FrameId);
        if (CreationTime && (CurrentTimeSeconds - *CreationTime) > TimeoutSeconds)
        {
            TimedOutFrames.Add(FrameId);
        }
    }

    for (const uint64 FrameId : TimedOutFrames)
    {
        const double* CreationTime = FrameCreationTime.Find(FrameId);
        // 记录超时帧的详细信息
        const FFramePacket* Packet = PendingFrames.Find(FrameId);
        if (Packet && CreationTime)
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
            const TMap<FGuid, FSensorFrameStatus>* StatusMap = PerSensorStatus.Find(FrameId);
            if (StatusMap)
            {
                for (const auto& SensorPair : *StatusMap)
                {
                    SensorStatus += FString::Printf(TEXT("  %s: expected=%u completed=%u channels=%d/%d "),
                        *SensorPair.Value.SensorName.ToString(),
                        static_cast<uint8>(SensorPair.Value.ExpectedPayloads),
                        static_cast<uint8>(SensorPair.Value.CompletedPayloads),
                        SensorPair.Value.CompletedImageChannels.Num(),
                        SensorPair.Value.ExpectedImageChannels.Num());
                }
            }

            UE_LOG(LogTemp, Warning,
                TEXT("Frame %llu timed out after %.2fs. Missing: [%s] Sensors:{%s}"),
                FrameId, CurrentTimeSeconds - *CreationTime,
                *MissingModalities, *SensorStatus);
        }

        RememberTerminalFrame(FrameId, static_cast<uint8>(EFrameTerminalState::TimedOut));
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
    EnqueuedCompleteFrames.Remove(FrameId);
}

bool FFrameAssembler::FailFrame(const uint64 FrameId, const FGuid& SensorGuid, const FName SensorName, const ECaptureRequestResult Result)
{
    if (Result == ECaptureRequestResult::Accepted || !PendingFrames.Contains(FrameId)) return false;
    UE_LOG(LogTemp, Warning, TEXT("Frame %llu failed immediately: sensor='%s' guid=%s result=%s."),
        FrameId, *SensorName.ToString(), *SensorGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
        Result == ECaptureRequestResult::Busy ? TEXT("Busy") : TEXT("Rejected"));
    ++Stats.FailedFrames;
    if (Result == ECaptureRequestResult::Busy) ++Stats.BusyFrames;
    else ++Stats.RejectedFrames;
    RememberTerminalFrame(FrameId, static_cast<uint8>(EFrameTerminalState::Failed));
    CleanupFrame(FrameId);
    return true;
}

void FFrameAssembler::RememberTerminalFrame(const uint64 FrameId, const uint8 TerminalState)
{
    if (TerminalFrames.Contains(FrameId)) return;
    TerminalFrames.Add(FrameId, TerminalState);
    TerminalFrameOrder.Enqueue(FrameId);
    while (TerminalFrames.Num() > TerminalFrameHistoryCapacity)
    {
        uint64 OldestFrameId = 0;
        if (!TerminalFrameOrder.Dequeue(OldestFrameId)) break;
        TerminalFrames.Remove(OldestFrameId);
    }
}
