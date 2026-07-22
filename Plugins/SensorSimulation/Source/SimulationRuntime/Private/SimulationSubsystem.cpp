#include "SimulationSubsystem.h"
#include "SemanticObjectComponent.h"
#include "SimSensorComponentBase.h"
#include "SimulationSettings.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

/** 初始化世界级传感器仿真子系统。 */
void USimulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    const USimulationSettings* Settings = GetDefault<USimulationSettings>();

    // 确定性模式下设置固定随机种子，保证可复现性
    if (Settings->SimulationMode == ERuntimeMode::DeterministicDataset)
    {
        FMath::RandInit(Settings->RandomSeed);
        FMath::SRandInit(Settings->RandomSeed);
        UE_LOG(LogTemp, Log,
            TEXT("Deterministic dataset mode: seed=%d, step=%.4fs"),
            Settings->RandomSeed, Settings->FixedStepSeconds);
    }

    // 启动数据集会话
    FString Root = Settings->DatasetRoot.Path;
    if (Root.IsEmpty())
    {
        Root = FPaths::ProjectSavedDir() / TEXT("SensorSimulation");
    }

    DatasetSession = MakeUnique<FDatasetSession>();
    DatasetSession->Start(Root);

    // 根据配置创建并启动导出服务，输出到会话目录下
    ExportService = MakeUnique<FExportService>(Settings->MaxPendingFrames);
    ExportService->Start(DatasetSession->GetSessionDirectory());
}

/** 清空传感器和语义状态后反初始化子系统。 */
void USimulationSubsystem::Deinitialize()
{
    // 先停止导出服务，确保所有帧已写出
    if (ExportService)
    {
        ExportService->Stop();
    }

    // 写入会话元数据
    if (DatasetSession && DatasetSession->GetState() == ESessionState::Running)
    {
        const USimulationSettings* Settings = GetDefault<USimulationSettings>();
        const FFrameAssemblerStats& Stats = FrameAssembler.GetStats();
        const FString Mode = Settings->SimulationMode == ERuntimeMode::DeterministicDataset
            ? TEXT("DeterministicDataset") : TEXT("Realtime");

        DatasetSession->WriteMetadata(
            Stats.TotalFrames, Stats.CompletedFrames, Stats.FailedFrames + Stats.TimeoutFrames,
            Settings->RandomSeed, Mode);
        DatasetSession->Stop();
    }

    ExportService.Reset();
    DatasetSession.Reset();
    Sensors.Reset();
    SemanticRegistry.Reset();
    Super::Deinitialize();
}

/** 推进仿真时钟、按固定步长发起采集并消费完整帧。 */
void USimulationSubsystem::Tick(float DeltaTime)
{
    const USimulationSettings* Settings = GetDefault<USimulationSettings>();
    const double Step = FMath::Max(0.001, Settings->FixedStepSeconds);
    AccumulatedSeconds += DeltaTime;
    SimulationSeconds += DeltaTime;

    // 确定性模式下，等待所有待处理帧完成后再请求新帧，保证严格顺序
    const bool bCanRequestNewFrame =
        Settings->SimulationMode == ERuntimeMode::DeterministicDataset
            ? FrameAssembler.GetPendingFrameCount() == 0
            : true;

    // 累加器把不稳定的游戏帧 DeltaTime 转换为固定频率采样；Fmod 保留不足一步的余量。
    if (bCanRequestNewFrame && AccumulatedSeconds >= Step)
    {
        AccumulatedSeconds = FMath::Fmod(AccumulatedSeconds, Step);
        RequestFrame(SimulationSeconds);
    }

    // 清理超时帧，避免 Pending 队列无限增长
    FrameAssembler.PurgeTimedOutFrames(SimulationSeconds, Settings->FrameTimeoutSeconds);

    FFramePacket CompletePacket;
    while (FrameAssembler.PopCompleteFrame(CompletePacket))
    {
        if (ExportService)
        {
            EExportBackpressurePolicy Policy = EExportBackpressurePolicy::RejectNewest;
            if (Settings->SimulationMode == ERuntimeMode::DeterministicDataset)
            {
                Policy = EExportBackpressurePolicy::BlockDatasetClock;
            }
            ExportService->Enqueue(MoveTemp(CompletePacket), Policy);
        }
    }
}

/** 返回 Unreal Tick 性能统计标识。 */
TStatId USimulationSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(USimulationSubsystem, STATGROUP_Tickables);
}

/** 把传感器加入当前世界的弱引用集合并避免重复。 */
void USimulationSubsystem::RegisterSensor(USimSensorComponentBase& Sensor)
{
    Sensors.AddUnique(&Sensor);
}

/** 从当前世界移除指定传感器的弱引用。 */
void USimulationSubsystem::UnregisterSensor(const USimSensorComponentBase& Sensor)
{
    Sensors.RemoveAll([&Sensor](const TWeakObjectPtr<USimSensorComponentBase>& Item) { return Item.Get() == &Sensor; });
}

/** 将语义组件注册到实例编号注册表。 */
uint32 USimulationSubsystem::RegisterSemanticObject(USemanticObjectComponent& Component)
{
    return SemanticRegistry.Register(Component);
}

/** 从实例编号注册表注销语义组件。 */
void USimulationSubsystem::UnregisterSemanticObject(const USemanticObjectComponent& Component)
{
    SemanticRegistry.Unregister(Component);
}

/** 校验完成图像，并仅把满足协议约束的载荷移交给帧聚合器。 */
bool USimulationSubsystem::SubmitImage(FImagePayload&& Image)
{
    if (!ValidateImagePayload(Image))
    {
        UE_LOG(LogTemp, Error,
            TEXT("Rejected image payload: frame=%llu sensor='%s' type=%u size=%dx%d bytes=%d."),
            Image.Header.FrameId, *Image.SensorName.ToString(), static_cast<uint8>(Image.PayloadType),
            Image.ImageSize.X, Image.ImageSize.Y, Image.Bytes.Num());
        return false;
    }
    return FrameAssembler.AddImage(MoveTemp(Image));
}

/** 把完成的点云移交给帧聚合器。 */
void USimulationSubsystem::SubmitLidar(FLidarScanPayload&& Scan)
{
    FrameAssembler.AddLidar(MoveTemp(Scan));
}

/** 注册相机标定参数，会话结束时写入 calibration.json。 */
void USimulationSubsystem::RegisterCalibration(const FCalibration& Calibration)
{
    if (DatasetSession)
    {
        DatasetSession->RegisterCalibration(Calibration);
    }
}

/** 创建同步帧、采集真值并向所有启用的传感器下发请求。 */
void USimulationSubsystem::RequestFrame(double TimestampSeconds)
{
    FFrameHeader Header;
    Header.SequenceId = SequenceId;
    Header.FrameId = NextFrameId++;
    Header.SimulationTimestampSeconds = TimestampSeconds;

    // 先声明整帧预期模态，聚合器据此判断异步返回的数据何时全部到齐。
    EPayloadType Expected = EPayloadType::GroundTruth;
    for (const TWeakObjectPtr<USimSensorComponentBase>& Sensor : Sensors)
    {
        if (Sensor.IsValid() && Sensor->bSensorEnabled)
        {
            Expected |= Sensor->GetPayloadTypes();
        }
    }

    FrameAssembler.BeginFrame(Header, Expected);
    FrameAssembler.AddGroundTruth(Header.FrameId, CaptureGroundTruth());

    for (const TWeakObjectPtr<USimSensorComponentBase>& Sensor : Sensors)
    {
        if (!Sensor.IsValid() || !Sensor->bSensorEnabled)
        {
            continue;
        }

        const EPayloadType SensorPayloads = Sensor->GetPayloadTypes();

        // 注册传感器的预期模态，用于多传感器精确完成计数
        FrameAssembler.RegisterSensor(Header.FrameId, Sensor->SensorName, SensorPayloads);

        FCaptureRequest Request;
        Request.Header = Header;
        Request.SensorName = Sensor->SensorName;
        Request.ExpectedPayloads = SensorPayloads;
        Sensor->RequestCapture(Request);
    }
}

/** 验证紧密 RGBA8 图像布局，以及 Semantic 的通道值和注册标签集合。 */
bool USimulationSubsystem::ValidateImagePayload(const FImagePayload& Image) const
{
    if ((Image.PayloadType != EPayloadType::Rgb && Image.PayloadType != EPayloadType::Semantic)
        || Image.ImageSize.X <= 0 || Image.ImageSize.Y <= 0 || Image.BytesPerPixel != 4)
    {
        return false;
    }
    const int64 RequiredBytes = static_cast<int64>(Image.ImageSize.X)
        * static_cast<int64>(Image.ImageSize.Y) * Image.BytesPerPixel;
    if (RequiredBytes != Image.Bytes.Num())
    {
        return false;
    }
    if (Image.PayloadType == EPayloadType::Semantic)
    {
        TSet<uint8> ValidIds;
        SemanticRegistry.GetImageSemanticIds(ValidIds);
        for (int64 PixelOffset = 0; PixelOffset < RequiredBytes; PixelOffset += 4)
        {
            const uint8 SemanticId = Image.Bytes[PixelOffset + 0];
            if (!ValidIds.Contains(SemanticId)
                || Image.Bytes[PixelOffset + 1] != 0
                || Image.Bytes[PixelOffset + 2] != 0
                || Image.Bytes[PixelOffset + 3] != 255)
            {
                return false;
            }
        }
    }
    return true;
}

/** 遍历带语义组件的 Actor 并采集位姿、包围盒和速度真值。 */
TArray<FObjectGroundTruth> USimulationSubsystem::CaptureGroundTruth() const
{
    TArray<FObjectGroundTruth> Output;
    if (!GetWorld())
    {
        return Output;
    }

    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        const AActor* Actor = *It;
        const USemanticObjectComponent* Semantic = Actor->FindComponentByClass<USemanticObjectComponent>();
        if (!Semantic)
        {
            continue;
        }

        FObjectGroundTruth& Item = Output.AddDefaulted_GetRef();
        Item.InstanceId = static_cast<uint32>(FMath::Max(0, Semantic->InstanceId));
        Item.SemanticId = static_cast<uint16>(FMath::Clamp(Semantic->SemanticId, 0, 65535));
        Item.WorldTransform = Actor->GetActorTransform();
        Item.WorldBounds = FBox3d(Actor->GetComponentsBoundingBox(true));
        Item.LinearVelocity = FVector3d(Actor->GetVelocity());
    }
    return Output;
}

