#include "ExportService.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// 后台 Worker 线程：从队列取出完整帧，写入磁盘
// ---------------------------------------------------------------------------

/** 后台导出 Worker，循环消费队列中的完整帧并写出到磁盘。 */
struct FExportService::FImpl : public FRunnable
{
    explicit FImpl(int32 InCapacity)
        : Capacity(FMath::Max(1, InCapacity))
        , WorkEvent(FPlatformProcess::GetSynchEventFromPool(false))
    {
    }

    ~FImpl()
    {
        StopWorker();
        if (WorkEvent)
        {
            FPlatformProcess::ReturnSynchEventToPool(WorkEvent);
            WorkEvent = nullptr;
        }
    }

    // -- FRunnable 接口 --
    virtual bool Init() override { return true; }

    virtual uint32 Run() override
    {
        while (!bShouldExit.Load())
        {
            FFramePacket Packet;
            bool bConsumedAny = false;
            while (Pending.Dequeue(Packet))
            {
                ExportFrame(Packet);
                --PendingCount;
                bConsumedAny = true;
            }
            if (!bShouldExit.Load() && !bConsumedAny && WorkEvent)
            {
                // 事件唤醒替代 1ms Sleep 轮询，空队列不再持续占用 Worker 时间片。
                WorkEvent->Wait();
            }
        }
        FFramePacket Packet;
        while (Pending.Dequeue(Packet))
        {
            ExportFrame(Packet);
            --PendingCount;
        }
        return 0;
    }

    virtual void Stop() override
    {
        bShouldExit.Store(true);
    }

    // -- Worker 生命周期 --

    void StartWorker(const FString& InDatasetRoot)
    {
        DatasetRoot = InDatasetRoot;
        bShouldExit.Store(false);
        bRunning.Store(true);
        if (WorkEvent)
        {
            WorkEvent->Reset();
        }

        // 创建 Session 输出目录
        IFileManager::Get().MakeDirectory(*DatasetRoot, true);

        WorkerThread = FRunnableThread::Create(
            this,
            TEXT("ExportServiceWorker"),
            0,
            TPri_BelowNormal
        );
    }

    void StopWorker()
    {
        bRunning.Store(false);
        bShouldExit.Store(true);
        if (WorkEvent)
        {
            WorkEvent->Trigger();
        }

        if (WorkerThread)
        {
            WorkerThread->WaitForCompletion();
            delete WorkerThread;
            WorkerThread = nullptr;
        }
    }

    // -- 文件写入 --

    /** 将一帧数据写出到磁盘。 */
    void ExportFrame(const FFramePacket& Packet)
    {
        const FString FinalFrameDir = FString::Printf(
            TEXT("%s/frame_%06llu"), *DatasetRoot, Packet.Header.FrameId);
        const FString FrameDir = FinalFrameDir + TEXT(".tmp");

        IFileManager& FileManager = IFileManager::Get();
        FileManager.DeleteDirectory(*FrameDir, false, true);
        if (FileManager.DirectoryExists(*FinalFrameDir)
            || !FileManager.MakeDirectory(*FrameDir, true))
        {
            UE_LOG(LogTemp, Error,
                TEXT("Cannot start frame export transaction: frame=%llu final='%s' temp='%s'."),
                Packet.Header.FrameId, *FinalFrameDir, *FrameDir);
            FailedFrameCount.Increment();
            return;
        }

        bool bSuccess = true;

        // 写出 RGB 和 Semantic 图像
        for (const FImagePayload& Image : Packet.Images)
        {
            if (!Image.ChannelGuid.IsValid())
            {
                UE_LOG(LogTemp, Error,
                    TEXT("Cannot export image without ChannelGuid: frame=%llu sensor=%s type=%u."),
                    Packet.Header.FrameId, *Image.SensorName.ToString(), static_cast<uint8>(Image.PayloadType));
                bSuccess = false;
                continue;
            }
            // 文件身份只由完整 ChannelGuid 决定；同模态多配置及传感器改名都不会覆盖。
            const FString IdentitySuffix = TEXT("_") + Image.ChannelGuid.ToString(EGuidFormats::Digits);
            if (Image.PayloadType == EPayloadType::Rgb)
            {
                bSuccess &= WritePng(FrameDir / (TEXT("rgb") + IdentitySuffix + TEXT(".png")), Image);
            }
            else if (Image.PayloadType == EPayloadType::Semantic)
            {
                bSuccess &= WritePng(FrameDir / (TEXT("semantic") + IdentitySuffix + TEXT(".png")), Image);
            }
            else if (Image.PayloadType == EPayloadType::Depth)
            {
                bSuccess &= FFileHelper::SaveArrayToFile(Image.Bytes, *(FrameDir / (TEXT("depth_meters_f32") + IdentitySuffix + TEXT(".bin"))));
            }
            else if (Image.PayloadType == EPayloadType::Instance)
            {
                // 保留原始小端 uint32 标识符，避免 PNG/颜色编码截断或改写 InstanceId。
                bSuccess &= FFileHelper::SaveArrayToFile(Image.Bytes, *(FrameDir / (TEXT("instance_u32") + IdentitySuffix + TEXT(".bin"))));
            }
        }

        // 写出 LiDAR 点云
        for (const FLidarScanPayload& Scan : Packet.LidarScans)
        {
            const FString IdentitySuffix = Packet.LidarScans.Num() > 1
                ? TEXT("_") + Scan.SensorGuid.ToString(EGuidFormats::Digits).Left(8)
                : FString();
            bSuccess &= WriteLidarBin(FrameDir / (TEXT("lidar") + IdentitySuffix + TEXT(".bin")), Scan);
        }

        // 即使场景中没有语义对象也写出合法空数组，避免“零对象”和“漏写”不可区分。
        bSuccess &= WriteGroundTruthJson(FrameDir / TEXT("groundtruth.json"),
            Packet.Header, Packet.Objects);

        // frame_info 最后写入；全部文件成功后才把临时目录原子提交为最终帧目录。
        bSuccess &= WriteFrameMetadataJson(FrameDir / TEXT("frame_info.json"), Packet);
        if (bSuccess)
        {
            bSuccess = FileManager.Move(
                *FinalFrameDir, *FrameDir,
                false, true, false, true);
            if (!bSuccess)
            {
                UE_LOG(LogTemp, Error,
                    TEXT("Failed to commit frame export transaction: frame=%llu temp='%s' final='%s'."),
                    Packet.Header.FrameId, *FrameDir, *FinalFrameDir);
            }
        }

        if (bSuccess)
        {
            ExportedFrameCount.Increment();
        }
        else
        {
            FailedFrameCount.Increment();
        }
    }

    /** 将 RGBA8 图像编码为 PNG 并写入文件。 */
    static bool WritePng(const FString& FilePath, const FImagePayload& Image)
    {
        if (Image.Bytes.Num() == 0 || Image.ImageSize.X <= 0 || Image.ImageSize.Y <= 0)
        {
            return false;
        }

        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> ImageWrapper =
            ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

        if (!ImageWrapper.IsValid())
        {
            return false;
        }

        // UE 的 PNG Writer 期望 BGRA 输入
        TArray<uint8> BgraData;
        BgraData.SetNumUninitialized(Image.Bytes.Num());
        for (int32 i = 0; i < Image.Bytes.Num(); i += 4)
        {
            BgraData[i + 0] = Image.Bytes[i + 2]; // B
            BgraData[i + 1] = Image.Bytes[i + 1]; // G
            BgraData[i + 2] = Image.Bytes[i + 0]; // R
            BgraData[i + 3] = Image.Bytes[i + 3]; // A
        }

        ImageWrapper->SetRaw(
            BgraData.GetData(), BgraData.Num(),
            Image.ImageSize.X, Image.ImageSize.Y,
            ERGBFormat::BGRA, 8
        );

        const TArray64<uint8> PngData = ImageWrapper->GetCompressed();
        return FFileHelper::SaveArrayToFile(PngData, *FilePath);
    }

    /** 将 LiDAR 点云以 float32 x,y,z,intensity 格式写入 .bin 文件。 */
    static bool WriteLidarBin(const FString& FilePath, const FLidarScanPayload& Scan)
    {
        // 每个点 4 个 float32 = 16 字节
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(Scan.Points.Num() * 16);

        uint8* Dest = Buffer.GetData();
        for (const FLidarPoint& Point : Scan.Points)
        {
            FMemory::Memcpy(Dest, &Point.PositionMeters.X, 4); Dest += 4;
            FMemory::Memcpy(Dest, &Point.PositionMeters.Y, 4); Dest += 4;
            FMemory::Memcpy(Dest, &Point.PositionMeters.Z, 4); Dest += 4;
            FMemory::Memcpy(Dest, &Point.Intensity, 4);        Dest += 4;
        }

        return FFileHelper::SaveArrayToFile(Buffer, *FilePath);
    }

    /** 将 Ground Truth 对象列表写入 JSON 文件。 */
    static bool WriteGroundTruthJson(
        const FString& FilePath,
        const FFrameHeader& Header,
        const TArray<FObjectGroundTruth>& Objects)
    {
        FString JsonStr;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);

        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("frame_id"), FString::Printf(TEXT("%llu"), Header.FrameId));
        Writer->WriteValue(TEXT("timestamp"), Header.SimulationTimestampSeconds);

        Writer->WriteArrayStart(TEXT("objects"));
        for (const FObjectGroundTruth& Obj : Objects)
        {
            Writer->WriteObjectStart();
            Writer->WriteValue(TEXT("instance_id"), static_cast<int64>(Obj.InstanceId));
            Writer->WriteValue(TEXT("semantic_id"), static_cast<int64>(Obj.SemanticId));

            const FVector Pos = Obj.WorldTransform.GetLocation();
            Writer->WriteValue(TEXT("position_x"), Pos.X);
            Writer->WriteValue(TEXT("position_y"), Pos.Y);
            Writer->WriteValue(TEXT("position_z"), Pos.Z);

            const FVector Vel = Obj.LinearVelocity;
            Writer->WriteValue(TEXT("velocity_x"), Vel.X);
            Writer->WriteValue(TEXT("velocity_y"), Vel.Y);
            Writer->WriteValue(TEXT("velocity_z"), Vel.Z);

            const FBox Bounds = FBox(Obj.WorldBounds);
            Writer->WriteValue(TEXT("bounds_min_x"), Bounds.Min.X);
            Writer->WriteValue(TEXT("bounds_min_y"), Bounds.Min.Y);
            Writer->WriteValue(TEXT("bounds_min_z"), Bounds.Min.Z);
            Writer->WriteValue(TEXT("bounds_max_x"), Bounds.Max.X);
            Writer->WriteValue(TEXT("bounds_max_y"), Bounds.Max.Y);
            Writer->WriteValue(TEXT("bounds_max_z"), Bounds.Max.Z);

            Writer->WriteObjectEnd();
        }
        Writer->WriteArrayEnd();
        Writer->WriteObjectEnd();
        Writer->Close();

        return FFileHelper::SaveStringToFile(JsonStr, *FilePath);
    }

    /** 写出单帧的采集元数据。 */
    static bool WriteFrameMetadataJson(const FString& FilePath, const FFramePacket& Packet)
    {
        FString JsonStr;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);

        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("frame_id"), FString::Printf(TEXT("%llu"), Packet.Header.FrameId));
        Writer->WriteValue(TEXT("sequence_id"), FString::Printf(TEXT("%llu"), Packet.Header.SequenceId));
        Writer->WriteValue(TEXT("timestamp"), Packet.Header.SimulationTimestampSeconds);
        Writer->WriteValue(TEXT("image_count"), Packet.Images.Num());
        Writer->WriteValue(TEXT("lidar_scan_count"), Packet.LidarScans.Num());
        Writer->WriteValue(TEXT("object_count"), Packet.Objects.Num());
        Writer->WriteArrayStart(TEXT("images"));
        for (const FImagePayload& Image : Packet.Images)
        {
            Writer->WriteObjectStart();
            Writer->WriteValue(TEXT("sensor_guid"), Image.SensorGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
            Writer->WriteValue(TEXT("sensor_name"), Image.SensorName.ToString());
            Writer->WriteValue(TEXT("channel_guid"), Image.ChannelGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
            Writer->WriteValue(TEXT("payload_type"), static_cast<int32>(Image.PayloadType));
            Writer->WriteObjectEnd();
        }
        Writer->WriteArrayEnd();
        Writer->WriteArrayStart(TEXT("lidar_scans"));
        for (const FLidarScanPayload& Scan : Packet.LidarScans)
        {
            Writer->WriteObjectStart();
            Writer->WriteValue(TEXT("sensor_guid"), Scan.SensorGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
            Writer->WriteValue(TEXT("sensor_name"), Scan.SensorName.ToString());
            Writer->WriteValue(TEXT("expected_ray_count"), static_cast<int64>(Scan.ExpectedRayCount));
            Writer->WriteValue(TEXT("completed_ray_count"), static_cast<int64>(Scan.CompletedRayCount));
            Writer->WriteValue(TEXT("hit_count"), Scan.Points.Num());
            Writer->WriteValue(TEXT("complete_revolution"), Scan.bCompleteRevolution);
            Writer->WriteObjectEnd();
        }
        Writer->WriteArrayEnd();
        Writer->WriteObjectEnd();
        Writer->Close();

        return FFileHelper::SaveStringToFile(JsonStr, *FilePath);
    }

    // -- 状态 --
    int32 Capacity;
    FString DatasetRoot;
    TQueue<FFramePacket, EQueueMode::Mpsc> Pending;
    TAtomic<int32> PendingCount { 0 };
    TAtomic<int32> PeakPendingCount { 0 };
    TAtomic<bool> bRunning { false };
    TAtomic<bool> bShouldExit { false };
    /** Worker 在无任务时等待的事件，由 Enqueue 和 Stop 触发。 */
    FEvent* WorkEvent = nullptr;
    FRunnableThread* WorkerThread = nullptr;
    FThreadSafeCounter ExportedFrameCount { 0 };
    FThreadSafeCounter FailedFrameCount { 0 };
    FThreadSafeCounter EnqueuedFrameCount { 0 };
    FThreadSafeCounter RejectedFrameCount { 0 };
    FThreadSafeCounter DroppedFrameCount { 0 };
};

// ---------------------------------------------------------------------------
// FExportService 公共接口
// ---------------------------------------------------------------------------

FExportService::FExportService(int32 InCapacity)
    : Impl(MakeUnique<FImpl>(InCapacity))
{
}

FExportService::~FExportService()
{
    Stop();
}

bool FExportService::Start(const FString& InDatasetRoot)
{
    Impl->StartWorker(InDatasetRoot);
    return true;
}

void FExportService::Stop()
{
    Impl->StopWorker();
}

bool FExportService::Enqueue(FFramePacket&& Packet, EExportBackpressurePolicy Policy)
{
    if (!Impl->bRunning.Load() || !Packet.IsComplete())
    {
        return false;
    }

    const int32 CurrentCount = Impl->PendingCount.Load();

    // 队列满时根据策略处理
    if (CurrentCount >= Impl->Capacity)
    {
        switch (Policy)
        {
        case EExportBackpressurePolicy::RejectNewest:
            UE_LOG(LogTemp, Warning,
                TEXT("Export queue full (%d/%d), rejecting frame %llu"),
                CurrentCount, Impl->Capacity, Packet.Header.FrameId);
            Impl->RejectedFrameCount.Increment();
            return false;

        case EExportBackpressurePolicy::DropOldest:
            {
                FFramePacket Dropped;
                if (Impl->Pending.Dequeue(Dropped))
                {
                    --Impl->PendingCount;
                    Impl->DroppedFrameCount.Increment();
                    UE_LOG(LogTemp, Warning,
                        TEXT("Export queue full, dropped frame %llu to make room for %llu"),
                        Dropped.Header.FrameId, Packet.Header.FrameId);
                }
            }
            break;

        case EExportBackpressurePolicy::PauseDatasetClock:
            // 非阻塞返回；Subsystem 保留完整帧并显式暂停确定性调度器。
            Impl->RejectedFrameCount.Increment();
            return false;
        }
    }

    Impl->Pending.Enqueue(MoveTemp(Packet));
    Impl->EnqueuedFrameCount.Increment();
    const int32 NewPendingCount = ++Impl->PendingCount;
    int32 ExpectedPeak = Impl->PeakPendingCount.Load();
    while (NewPendingCount > ExpectedPeak &&
        !Impl->PeakPendingCount.CompareExchange(ExpectedPeak, NewPendingCount))
    {
    }
    if (Impl->WorkEvent)
    {
        Impl->WorkEvent->Trigger();
    }
    return true;
}

int32 FExportService::GetPendingCount() const
{
    return Impl->PendingCount.Load();
}

int32 FExportService::GetPeakPendingCount() const
{
    return Impl->PeakPendingCount.Load();
}

bool FExportService::HasCapacity() const
{
    return Impl->bRunning.Load() && Impl->PendingCount.Load() < Impl->Capacity;
}

int64 FExportService::GetExportedFrameCount() const
{
    return Impl->ExportedFrameCount.GetValue();
}

int64 FExportService::GetFailedFrameCount() const
{
    return Impl->FailedFrameCount.GetValue();
}

FExportServiceStats FExportService::GetStats() const
{
    FExportServiceStats Stats;
    Stats.EnqueuedFrames = Impl->EnqueuedFrameCount.GetValue();
    Stats.RejectedFrames = Impl->RejectedFrameCount.GetValue();
    Stats.DroppedFrames = Impl->DroppedFrameCount.GetValue();
    Stats.CommittedFrames = Impl->ExportedFrameCount.GetValue();
    Stats.FailedFrames = Impl->FailedFrameCount.GetValue();
    Stats.PeakPendingFrames = Impl->PeakPendingCount.Load();
    return Stats;
}
