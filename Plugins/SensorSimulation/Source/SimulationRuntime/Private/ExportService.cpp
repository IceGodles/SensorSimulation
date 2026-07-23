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
    {
    }

    ~FImpl()
    {
        StopWorker();
    }

    // -- FRunnable 接口 --
    virtual bool Init() override { return true; }

    virtual uint32 Run() override
    {
        while (!bShouldExit.Load())
        {
            FFramePacket Packet;
            if (Pending.Dequeue(Packet))
            {
                ExportFrame(Packet);
                --PendingCount;
            }
            else
            {
                // 队列为空，短暂休眠避免忙等
                FPlatformProcess::SleepNoStats(0.001f);
            }
        }

        // 退出前排空剩余帧
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
        const FString FrameDir = FString::Printf(
            TEXT("%s/frame_%06llu"), *DatasetRoot, Packet.Header.FrameId);

        IFileManager::Get().MakeDirectory(*FrameDir, true);

        bool bSuccess = true;

        // 写出 RGB 和 Semantic 图像
        for (const FImagePayload& Image : Packet.Images)
        {
            if (Image.PayloadType == EPayloadType::Rgb)
            {
                bSuccess &= WritePng(FrameDir / TEXT("rgb.png"), Image);
            }
            else if (Image.PayloadType == EPayloadType::Semantic)
            {
                bSuccess &= WritePng(FrameDir / TEXT("semantic.png"), Image);
            }
        }

        // 写出 LiDAR 点云
        for (const FLidarScanPayload& Scan : Packet.LidarScans)
        {
            bSuccess &= WriteLidarBin(FrameDir / TEXT("lidar.bin"), Scan);
        }

        // 写出 Ground Truth
        if (Packet.Objects.Num() > 0)
        {
            bSuccess &= WriteGroundTruthJson(FrameDir / TEXT("groundtruth.json"),
                Packet.Header, Packet.Objects);
        }

        // 写出帧元数据
        WriteFrameMetadataJson(FrameDir / TEXT("frame_info.json"), Packet);

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
        if (Scan.Points.Num() == 0)
        {
            return true; // 空点云不算失败
        }

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
    static void WriteFrameMetadataJson(const FString& FilePath, const FFramePacket& Packet)
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
        Writer->WriteObjectEnd();
        Writer->Close();

        FFileHelper::SaveStringToFile(JsonStr, *FilePath);
    }

    // -- 状态 --
    int32 Capacity;
    FString DatasetRoot;
    TQueue<FFramePacket, EQueueMode::Mpsc> Pending;
    TAtomic<int32> PendingCount { 0 };
    TAtomic<bool> bRunning { false };
    TAtomic<bool> bShouldExit { false };
    FRunnableThread* WorkerThread = nullptr;
    FThreadSafeCounter ExportedFrameCount { 0 };
    FThreadSafeCounter FailedFrameCount { 0 };
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
            return false;

        case EExportBackpressurePolicy::DropOldest:
            {
                FFramePacket Dropped;
                if (Impl->Pending.Dequeue(Dropped))
                {
                    --Impl->PendingCount;
                    UE_LOG(LogTemp, Warning,
                        TEXT("Export queue full, dropped frame %llu to make room for %llu"),
                        Dropped.Header.FrameId, Packet.Header.FrameId);
                }
            }
            break;

        case EExportBackpressurePolicy::BlockDatasetClock:
            // 等待队列有空位（用于确定性模式）
            while (Impl->PendingCount.Load() >= Impl->Capacity && Impl->bRunning.Load())
            {
                FPlatformProcess::SleepNoStats(0.001f);
            }
            if (!Impl->bRunning.Load())
            {
                return false;
            }
            break;
        }
    }

    Impl->Pending.Enqueue(MoveTemp(Packet));
    ++Impl->PendingCount;
    return true;
}

int32 FExportService::GetPendingCount() const
{
    return Impl->PendingCount.Load();
}

int64 FExportService::GetExportedFrameCount() const
{
    return Impl->ExportedFrameCount.GetValue();
}

int64 FExportService::GetFailedFrameCount() const
{
    return Impl->FailedFrameCount.GetValue();
}
